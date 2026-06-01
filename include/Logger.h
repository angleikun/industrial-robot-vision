#ifndef LOGGER_H
#define LOGGER_H

#include <QObject>
#include <QString>
#include <QFile>
#include <QTextStream>
#include <QMutex>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>

class Logger : public QObject {
    Q_OBJECT

public:
    static Logger *instance();

    void init(const QString &logDir);
    void log(const QString &msg, const QString &level = "INFO");
    void warn(const QString &msg);
    void error(const QString &msg);

    static void info(const QString &msg);
    static void warning(const QString &msg);
    static void err(const QString &msg);

private:
    explicit Logger(QObject *parent = nullptr);
    void writeLog(const QString &level, const QString &msg);
    void rotateLog();
    void cleanupOldLogs(const QString &logDir);

    QFile m_file;
    QString m_logDir;
    QTextStream m_stream;
    QMutex m_mutex;
    bool m_initialized = false;

    static Logger *s_instance;
};

#endif // LOGGER_H
