#include "DatabaseManager.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QDebug>
#include <QApplication>

// ── Schema version ───────────────────────────────────────────────
static const int SCHEMA_VERSION = 1;

DatabaseManager::DatabaseManager(QObject *parent)
    : QObject(parent)
{
    m_flushTimer = new QTimer(this);
    connect(m_flushTimer, &QTimer::timeout, this, &DatabaseManager::flushQueue);
}

DatabaseManager::~DatabaseManager()
{
    if (m_flushTimer) m_flushTimer->stop();
    // Flush any records still in the write queue before tearing down the
    // connection — otherwise records added in the last <FLUSH_INTERVAL_MS>
    // before exit are silently lost (T2 data sampling near the end of a run
    // would drop several rows). Safe to call even with empty queue.
    flushQueue();
    close();
}

// ── Open / Close ─────────────────────────────────────────────────

bool DatabaseManager::open(const QString &dbPath)
{
    if (m_open) close();

    m_dbPath = dbPath;
    QDir().mkpath(QFileInfo(dbPath).absolutePath());

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "main_connection");
    db.setDatabaseName(dbPath);

    if (!db.open()) {
        emit databaseError("无法打开数据库: " + db.lastError().text());
        return false;
    }

    // Enable WAL mode for concurrent reads
    QSqlQuery q(db);
    q.exec("PRAGMA journal_mode=WAL");
    q.exec("PRAGMA foreign_keys=ON");
    q.exec("PRAGMA synchronous=NORMAL");

    m_open = true;

    if (!createTables()) {
        emit databaseError("无法创建数据库表");
        return false;
    }

    return true;
}

void DatabaseManager::close()
{
    if (!m_open) return;

    QString connName;
    {
        QSqlDatabase db = QSqlDatabase::database("main_connection");
        if (db.isOpen()) {
            connName = db.connectionName();
            db.close();
        }
    }
    if (!connName.isEmpty()) {
        QSqlDatabase::removeDatabase(connName);
    }
    m_open = false;
}

bool DatabaseManager::isOpen() const
{
    return m_open;
}

// ── Schema ───────────────────────────────────────────────────────

bool DatabaseManager::createTables()
{
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);

    // ── detections ──────────────────────────────────────────────
    bool ok = q.exec(
        "CREATE TABLE IF NOT EXISTS detections ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp  TEXT NOT NULL,"
        "  image_path TEXT,"
        "  x          REAL,"
        "  y          REAL,"
        "  angle      REAL,"
        "  score      REAL,"
        "  result     TEXT DEFAULT 'PASS'"
        ")"
    );
    if (!ok) {
        qWarning() << "create detections table failed:" << q.lastError().text();
        return false;
    }

    // Index for time-range queries
    q.exec("CREATE INDEX IF NOT EXISTS idx_detections_ts ON detections(timestamp)");

    // ── measurements ────────────────────────────────────────────
    q.exec(
        "CREATE TABLE IF NOT EXISTS measurements ("
        "  id      INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  det_id  INTEGER NOT NULL REFERENCES detections(id) ON DELETE CASCADE,"
        "  type    TEXT,"
        "  value   REAL,"
        "  unit    TEXT"
        ")"
    );
    q.exec("CREATE INDEX IF NOT EXISTS idx_measurements_det ON measurements(det_id)");

    // ── grab_records ────────────────────────────────────────────
    q.exec(
        "CREATE TABLE IF NOT EXISTS grab_records ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  det_id      INTEGER REFERENCES detections(id) ON DELETE SET NULL,"
        "  timestamp   TEXT NOT NULL,"
        "  robot_x     REAL,"
        "  robot_y     REAL,"
        "  robot_angle REAL,"
        "  status      TEXT DEFAULT 'SENT'"
        ")"
    );
    q.exec("CREATE INDEX IF NOT EXISTS idx_grab_ts ON grab_records(timestamp)");

    // ── calib_sessions ──────────────────────────────────────────
    q.exec(
        "CREATE TABLE IF NOT EXISTS calib_sessions ("
        "  id               INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp        TEXT NOT NULL,"
        "  type             TEXT,"
        "  reprojection_err REAL,"
        "  file_path        TEXT"
        ")"
    );

    return true;
}

bool DatabaseManager::migrateSchema(int fromVersion)
{
    Q_UNUSED(fromVersion)
    // Future migration logic goes here
    return true;
}

// ── Detections ───────────────────────────────────────────────────

int DatabaseManager::saveDetection(const DetectionRecord &rec)
{
    QMutexLocker lock(&m_queueMutex);
    m_writeQueue.enqueue(rec);
    if (!m_flushTimer->isActive()) {
        m_flushTimer->start(FLUSH_INTERVAL_MS);
    }
    return -1; // async: ID not yet available
}

int DatabaseManager::saveDetectionSync(const DetectionRecord &rec)
{
    return saveDetectionInternal(rec);
}

int DatabaseManager::saveDetectionInternal(const DetectionRecord &rec)
{
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO detections (timestamp, image_path, x, y, angle, score, result) "
        "VALUES (:ts, :ip, :x, :y, :angle, :score, :result)"
    );
    q.bindValue(":ts",     rec.timestamp.toString(Qt::ISODate));
    q.bindValue(":ip",     rec.imagePath);
    q.bindValue(":x",      rec.x);
    q.bindValue(":y",      rec.y);
    q.bindValue(":angle",  rec.angle);
    q.bindValue(":score",  rec.score);
    q.bindValue(":result", rec.result);

    if (!q.exec()) {
        emit databaseError("保存检测记录失败: " + q.lastError().text());
        return -1;
    }

    int id = q.lastInsertId().toInt();
    emit recordInserted("detections", id);
    return id;
}

void DatabaseManager::flushQueue()
{
    QList<DetectionRecord> batch;
    {
        QMutexLocker lock(&m_queueMutex);
        while (!m_writeQueue.isEmpty()) batch.append(m_writeQueue.dequeue());
    }

    if (batch.isEmpty()) {
        m_flushTimer->stop();
        return;
    }

    QSqlDatabase db = QSqlDatabase::database("main_connection");
    db.transaction();

    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO detections (timestamp, image_path, x, y, angle, score, result) "
        "VALUES (:ts, :ip, :x, :y, :angle, :score, :result)"
    );

    int count = 0;
    for (const auto &rec : batch) {
        q.bindValue(":ts",     rec.timestamp.toString(Qt::ISODate));
        q.bindValue(":ip",     rec.imagePath);
        q.bindValue(":x",      rec.x);
        q.bindValue(":y",      rec.y);
        q.bindValue(":angle",  rec.angle);
        q.bindValue(":score",  rec.score);
        q.bindValue(":result", rec.result);

        if (q.exec()) {
            count++;
        }
    }

    db.commit();

    if (count > 0) {
        emit flushCompleted(count);
    }
}

bool DatabaseManager::updateDetectionResult(int id, const QString &result)
{
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);
    q.prepare("UPDATE detections SET result = :result WHERE id = :id");
    q.bindValue(":result", result);
    q.bindValue(":id", id);
    return q.exec();
}

QList<DetectionRecord> DatabaseManager::queryDetections(const QDateTime &from,
                                                         const QDateTime &to, int limit)
{
    QList<DetectionRecord> results;
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);
    q.prepare(
        "SELECT id, timestamp, image_path, x, y, angle, score, result "
        "FROM detections WHERE timestamp BETWEEN :from AND :to "
        "ORDER BY timestamp DESC LIMIT :limit"
    );
    q.bindValue(":from",  from.toString(Qt::ISODate));
    q.bindValue(":to",    to.toString(Qt::ISODate));
    q.bindValue(":limit", limit);

    if (!q.exec()) {
        emit databaseError("查询检测记录失败: " + q.lastError().text());
        return results;
    }

    while (q.next()) {
        DetectionRecord r;
        r.id        = q.value(0).toInt();
        r.timestamp = QDateTime::fromString(q.value(1).toString(), Qt::ISODate);
        r.imagePath = q.value(2).toString();
        r.x         = q.value(3).toDouble();
        r.y         = q.value(4).toDouble();
        r.angle     = q.value(5).toDouble();
        r.score     = q.value(6).toDouble();
        r.result    = q.value(7).toString();
        results.append(r);
    }
    return results;
}

DetectionRecord DatabaseManager::getDetection(int id)
{
    DetectionRecord r;
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);
    q.prepare(
        "SELECT id, timestamp, image_path, x, y, angle, score, result "
        "FROM detections WHERE id = :id"
    );
    q.bindValue(":id", id);
    if (q.exec() && q.next()) {
        r.id        = q.value(0).toInt();
        r.timestamp = QDateTime::fromString(q.value(1).toString(), Qt::ISODate);
        r.imagePath = q.value(2).toString();
        r.x         = q.value(3).toDouble();
        r.y         = q.value(4).toDouble();
        r.angle     = q.value(5).toDouble();
        r.score     = q.value(6).toDouble();
        r.result    = q.value(7).toString();
    }
    return r;
}

// ── Measurements ─────────────────────────────────────────────────

void DatabaseManager::saveMeasurement(int detId, const MeasurementRecord &rec)
{
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO measurements (det_id, type, value, unit) "
        "VALUES (:detId, :type, :val, :unit)"
    );
    q.bindValue(":detId", detId);
    q.bindValue(":type",  rec.type);
    q.bindValue(":val",   rec.value);
    q.bindValue(":unit",  rec.unit);

    if (!q.exec()) {
        emit databaseError("保存测量数据失败: " + q.lastError().text());
    }
}

QList<MeasurementRecord> DatabaseManager::queryMeasurements(int detId)
{
    QList<MeasurementRecord> results;
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);
    q.prepare("SELECT id, det_id, type, value, unit FROM measurements WHERE det_id = :detId");
    q.bindValue(":detId", detId);

    if (q.exec()) {
        while (q.next()) {
            MeasurementRecord r;
            r.id    = q.value(0).toInt();
            r.detId = q.value(1).toInt();
            r.type  = q.value(2).toString();
            r.value = q.value(3).toDouble();
            r.unit  = q.value(4).toString();
            results.append(r);
        }
    }
    return results;
}

// ── Grab records ─────────────────────────────────────────────────

int DatabaseManager::saveGrabRecord(const GrabRecord &rec)
{
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO grab_records (det_id, timestamp, robot_x, robot_y, robot_angle, status) "
        "VALUES (:detId, :ts, :rx, :ry, :ra, :st)"
    );
    // det_id is nullable: bind NULL when no detection is linked (detId < 0)
    // so that the FOREIGN KEY constraint on detections(id) is satisfied.
    if (rec.detId >= 0)
        q.bindValue(":detId", rec.detId);
    else
        q.bindValue(":detId", QVariant());   // SQL NULL — allowed by the schema
    q.bindValue(":ts",    QDateTime::currentDateTime().toString(Qt::ISODate));
    q.bindValue(":rx",    rec.robotX);
    q.bindValue(":ry",    rec.robotY);
    q.bindValue(":ra",    rec.robotAngle);
    q.bindValue(":st",    rec.status);

    if (!q.exec()) {
        emit databaseError("保存抓取记录失败: " + q.lastError().text());
        return -1;
    }
    return q.lastInsertId().toInt();
}

bool DatabaseManager::updateGrabStatus(int id, const QString &status)
{
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);
    q.prepare("UPDATE grab_records SET status = :st WHERE id = :id");
    q.bindValue(":st", status);
    q.bindValue(":id", id);
    return q.exec();
}

QList<GrabRecord> DatabaseManager::queryGrabRecords(const QDateTime &from, const QDateTime &to)
{
    QList<GrabRecord> results;
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);
    q.prepare(
        "SELECT id, det_id, timestamp, robot_x, robot_y, robot_angle, status "
        "FROM grab_records WHERE timestamp BETWEEN :from AND :to ORDER BY timestamp DESC"
    );
    q.bindValue(":from", from.toString(Qt::ISODate));
    q.bindValue(":to",   to.toString(Qt::ISODate));

    if (q.exec()) {
        while (q.next()) {
            GrabRecord r;
            r.id         = q.value(0).toInt();
            r.detId      = q.value(1).toInt();
            r.timestamp  = QDateTime::fromString(q.value(2).toString(), Qt::ISODate);
            r.robotX     = q.value(3).toDouble();
            r.robotY     = q.value(4).toDouble();
            r.robotAngle = q.value(5).toDouble();
            r.status     = q.value(6).toString();
            results.append(r);
        }
    }
    return results;
}

// ── Calibration sessions ─────────────────────────────────────────

int DatabaseManager::saveCalibSession(const CalibSessionRecord &rec)
{
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO calib_sessions (timestamp, type, reprojection_err, file_path) "
        "VALUES (:ts, :type, :err, :fp)"
    );
    q.bindValue(":ts",   rec.timestamp.toString(Qt::ISODate));
    q.bindValue(":type", rec.type);
    q.bindValue(":err",  rec.reprojectionErr);
    q.bindValue(":fp",   rec.filePath);

    if (!q.exec()) {
        emit databaseError("保存标定记录失败: " + q.lastError().text());
        return -1;
    }
    return q.lastInsertId().toInt();
}

QList<CalibSessionRecord> DatabaseManager::queryCalibSessions(const QString &type)
{
    QList<CalibSessionRecord> results;
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);
    QString sql = "SELECT id, timestamp, type, reprojection_err, file_path FROM calib_sessions";
    if (!type.isEmpty()) {
        sql += " WHERE type = :type";
    }
    sql += " ORDER BY timestamp DESC";
    q.prepare(sql);
    if (!type.isEmpty()) q.bindValue(":type", type);

    if (q.exec()) {
        while (q.next()) {
            CalibSessionRecord r;
            r.id              = q.value(0).toInt();
            r.timestamp       = QDateTime::fromString(q.value(1).toString(), Qt::ISODate);
            r.type            = q.value(2).toString();
            r.reprojectionErr = q.value(3).toDouble();
            r.filePath        = q.value(4).toString();
            results.append(r);
        }
    }
    return results;
}

// ── Maintenance ──────────────────────────────────────────────────

bool DatabaseManager::pruneOldRecords(int retentionDays)
{
    QSqlDatabase db = QSqlDatabase::database("main_connection");
    QSqlQuery q(db);
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-retentionDays);

    q.prepare("DELETE FROM detections WHERE timestamp < :cutoff");
    q.bindValue(":cutoff", cutoff.toString(Qt::ISODate));
    if (!q.exec()) return false;

    int removed = q.numRowsAffected();
    q.exec("PRAGMA optimize");  // compact after deletion
    emit pruneCompleted(removed);
    return true;
}

bool DatabaseManager::backupDatabase(const QString &backupPath)
{
    if (!m_open) return false;
    return QFile::copy(m_dbPath, backupPath);
}

qint64 DatabaseManager::databaseSizeBytes() const
{
    return QFileInfo(m_dbPath).size();
}
