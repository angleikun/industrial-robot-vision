#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QVariant>
#include <QTimer>
#include <QMutex>
#include <QQueue>

struct DetectionRecord {
    int     id        = -1;
    QDateTime timestamp;
    QString imagePath;
    double  x         = 0.0;
    double  y         = 0.0;
    double  angle     = 0.0;
    double  score     = 0.0;
    QString result;   // "PASS" / "FAIL"
};

struct MeasurementRecord {
    int     id      = -1;
    int     detId   = -1;
    QString type;
    double  value   = 0.0;
    QString unit;
};

struct GrabRecord {
    int     id        = -1;
    int     detId     = -1;
    QDateTime timestamp;
    double  robotX    = 0.0;
    double  robotY    = 0.0;
    double  robotAngle = 0.0;
    QString status;    // "SENT" / "ACK" / "FAIL"
};

struct CalibSessionRecord {
    int     id       = -1;
    QDateTime timestamp;
    QString type;     // "intrinsic" / "handeye"
    double  reprojectionErr = 0.0;
    QString filePath;
};

class DatabaseManager : public QObject {
    Q_OBJECT

public:
    explicit DatabaseManager(QObject *parent = nullptr);
    ~DatabaseManager() override;

    bool open(const QString &dbPath);
    void close();
    bool isOpen() const;

    // Detections
    int  saveDetection(const DetectionRecord &rec);       // async: queues write, returns -1
    int  saveDetectionSync(const DetectionRecord &rec);   // sync: immediate write, returns ID
    bool updateDetectionResult(int id, const QString &result);
    QList<DetectionRecord> queryDetections(const QDateTime &from, const QDateTime &to,
                                           int limit = 1000);
    DetectionRecord getDetection(int id);

    // Measurements
    void saveMeasurement(int detId, const MeasurementRecord &rec);
    QList<MeasurementRecord> queryMeasurements(int detId);

    // Grab records
    int  saveGrabRecord(const GrabRecord &rec);
    bool updateGrabStatus(int id, const QString &status);
    QList<GrabRecord> queryGrabRecords(const QDateTime &from, const QDateTime &to);

    // Calibration sessions
    int  saveCalibSession(const CalibSessionRecord &rec);
    QList<CalibSessionRecord> queryCalibSessions(const QString &type = QString());

    // Maintenance
    bool pruneOldRecords(int retentionDays);
    bool backupDatabase(const QString &backupPath);
    qint64 databaseSizeBytes() const;

signals:
    void databaseError(const QString &error);
    void recordInserted(const QString &table, int id);
    void pruneCompleted(int removedCount);
    void flushCompleted(int count);

private slots:
    void flushQueue();

private:
    bool createTables();
    bool migrateSchema(int fromVersion);
    int  saveDetectionInternal(const DetectionRecord &rec);

    QString m_dbPath;
    void   *m_db = nullptr;
    bool    m_open = false;

    // Async write queue
    QQueue<DetectionRecord> m_writeQueue;
    QMutex m_queueMutex;
    QTimer *m_flushTimer = nullptr;
    static constexpr int FLUSH_INTERVAL_MS = 200;
};

#endif // DATABASEMANAGER_H
