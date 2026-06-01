#ifndef CAMERAMANAGER_H
#define CAMERAMANAGER_H

#include <QObject>
#include <QStringList>
#include <QThread>
#include <QMutex>
#include <atomic>

class CameraWorker;

enum class CameraBackend { USB, Basler, Hikvision };

struct CameraInfo {
    QString id;
    QString name;
    CameraBackend backend;
};

class CameraManager : public QObject {
    Q_OBJECT

public:
    explicit CameraManager(QObject *parent = nullptr);
    ~CameraManager() override;

    QStringList enumerateCameras();
    bool open(const QString &id);
    void close();
    bool isOpen() const;

    void startAcquisition();
    void stopAcquisition();
    bool isAcquiring() const;

    void setResolution(int width, int height);
    void setFps(int fps);
    CameraBackend detectBackend(const QString &id) const;

signals:
    void frameReady(const QImage &image);
    void acquisitionError(const QString &error);
    void cameraDisconnected();
    void fpsUpdated(double fps);

private slots:
    void onWorkerFrameReady(const QImage &image);
    void onWorkerError(const QString &error);

private:
    void startWorker();
    void stopWorker();

    QString          m_currentId;
    CameraBackend    m_backend = CameraBackend::USB;
    QThread         *m_workerThread = nullptr;
    CameraWorker    *m_worker       = nullptr;
    QMutex           m_mutex;
    std::atomic<bool> m_acquiring{false};
    int m_width  = 1920;
    int m_height = 1080;
    int m_fps    = 30;
    int m_acquisitionTimeoutMs = 2000;
};

#endif // CAMERAMANAGER_H
