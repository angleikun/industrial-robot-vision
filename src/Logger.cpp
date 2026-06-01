#include "Logger.h"
#include <QDebug>

Logger *Logger::s_instance = nullptr;

Logger::Logger(QObject *parent)
    : QObject(parent)
{
}

Logger *Logger::instance()
{
    // C++11 magic statics: thread-safe initialization, no double-checked
    // locking needed. The previous version had a classic race where two
    // threads calling instance() concurrently could create two Loggers.
    static Logger inst;
    s_instance = &inst;
    return &inst;
}

void Logger::init(const QString &logDir)
{
    QMutexLocker lock(&m_mutex);

    m_logDir = logDir;
    QDir().mkpath(logDir);

    QString fileName = logDir + "/RobotVision_"
                       + QDateTime::currentDateTime().toString("yyyyMMdd")
                       + ".log";

    if (m_file.isOpen()) {
        m_stream.flush();
        m_file.close();
    }

    m_file.setFileName(fileName);
    (void)m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    m_stream.setDevice(&m_file);
    m_initialized = true;

    cleanupOldLogs(logDir);
}

void Logger::cleanupOldLogs(const QString &logDir)
{
    QDir dir(logDir);
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-30);
    const auto entries = dir.entryInfoList(
        QStringList() << "RobotVision_*.log*", QDir::Files);
    for (const QFileInfo &fi : entries) {
        if (fi.lastModified() < cutoff) {
            QFile::remove(fi.absoluteFilePath());
        }
    }
}

void Logger::writeLog(const QString &level, const QString &msg)
{
    QMutexLocker lock(&m_mutex);
    if (!m_initialized) return;

    // Disk space check: < 100MB available → silent degrade
    QStorageInfo storage(QFileInfo(m_file.fileName()).absolutePath());
    if (storage.bytesAvailable() < 100LL * 1024 * 1024) {
        return;
    }

    // Rotate if > 50MB
    if (m_file.size() > 50 * 1024 * 1024) {
        rotateLog();
    }

    QString line = QString("[%1] [%2] %3\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz"))
        .arg(level, -5)
        .arg(msg);

    m_stream << line;
    m_stream.flush();
}

void Logger::rotateLog()
{
    m_stream.flush();
    m_file.close();

    QString oldPath = m_file.fileName();
    QString rotated = oldPath + "."
        + QDateTime::currentDateTime().toString("hhmmss");
    QFile::rename(oldPath, rotated);

    m_file.setFileName(oldPath);
    (void)m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    m_stream.setDevice(&m_file);
}

void Logger::log(const QString &msg, const QString &level)
{
    writeLog(level, msg);
}

void Logger::warn(const QString &msg)
{
    writeLog("WARN", msg);
}

void Logger::error(const QString &msg)
{
    writeLog("ERROR", msg);
}

void Logger::info(const QString &msg)
{
    instance()->log(msg, "INFO");
}

void Logger::warning(const QString &msg)
{
    instance()->warn(msg);
}

void Logger::err(const QString &msg)
{
    instance()->error(msg);
}
