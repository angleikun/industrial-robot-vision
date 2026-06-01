#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include "MainWindow.h"
#include "Logger.h"

// Route Qt diagnostic messages (qDebug/qWarning/qCritical) into the Logger so
// HALCON / network errors emitted via qWarning() are persisted to the log file.
// Without this, those messages went to stderr (closed in a GUI app) and were
// effectively lost.
static void qtMessageToLogger(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    Q_UNUSED(ctx);
    switch (type) {
    case QtDebugMsg:    Logger::info(msg);    break;
    case QtInfoMsg:     Logger::info(msg);    break;
    case QtWarningMsg:  Logger::warning(msg); break;
    case QtCriticalMsg: Logger::err(msg);     break;
    case QtFatalMsg:    Logger::err(msg);     break;
    }
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("RobotVisionSystem");
    app.setApplicationVersion("1.2");
    app.setOrganizationName("IndustrialVision");

    // Ensure config/data directories exist
    QString appDir = QApplication::applicationDirPath();
    QDir().mkpath(appDir + "/config");
    QDir().mkpath(appDir + "/data");
    QDir().mkpath(appDir + "/exports");
    QDir().mkpath(appDir + "/resources/templates");
    QDir().mkpath(appDir + "/logs");
    Logger::instance()->init(appDir + "/logs");
    qInstallMessageHandler(qtMessageToLogger);

    // Copy default settings.json if not present
    QString configPath = appDir + "/config/settings.json";
    if (!QFile::exists(configPath)) {
        QString bundledConfig = appDir + "/../config/settings.json";
        if (QFile::exists(bundledConfig)) {
            QFile::copy(bundledConfig, configPath);
        }
    }

    MainWindow mainWindow;
    mainWindow.show();

    return app.exec();
}
