#include "CameraManager.h"
#include <QMetaObject>
#include <QDebug>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>

// ── Worker class (lives in worker thread) ────────────────────────
class CameraWorker : public QObject {
    Q_OBJECT
public:
    explicit CameraWorker(QObject *parent = nullptr);
    ~CameraWorker() override;

    bool open(const QString &id, CameraBackend backend, int w, int h, int fps, int timeoutMs);
    void close();
    void startGrab();
    void stopGrab();

signals:
    void frameReady(const QImage &image);
    void error(const QString &msg);
    void fpsUpdated(double fps);

private slots:
    void grabFrame();

private:
    QImage grabUSB();

    CameraBackend m_backend = CameraBackend::USB;
    cv::VideoCapture *m_capture = nullptr;
    QTimer *m_grabTimer = nullptr;

    QElapsedTimer m_fpsTimer;
    int m_frameCount = 0;

    bool  m_running = false;
    int   m_width  = 1920;
    int   m_height = 1080;
    int   m_fps    = 30;
    int   m_timeoutMs = 2000;
    int   m_failCount  = 0;
    static constexpr int MAX_FAIL_COUNT = 3;
};

CameraWorker::CameraWorker(QObject *parent) : QObject(parent)
{
    m_grabTimer = new QTimer(this);
    connect(m_grabTimer, &QTimer::timeout, this, &CameraWorker::grabFrame);
}

CameraWorker::~CameraWorker()
{
    close();
}

bool CameraWorker::open(const QString &id, CameraBackend backend,
                         int w, int h, int fps, int timeoutMs)
{
    // Defensive: if called twice without close() in between, the previous
    // VideoCapture would leak. This shouldn't happen in normal use but we
    // guard against it explicitly (cheap, no behaviour change otherwise).
    if (m_capture) close();

    m_backend   = backend;
    m_width     = w;
    m_height    = h;
    m_fps       = fps;
    m_timeoutMs = timeoutMs;

    if (backend == CameraBackend::USB) {
        m_capture = new cv::VideoCapture(id.toInt(), cv::CAP_DSHOW);
        if (!m_capture->isOpened()) {
            delete m_capture;
            m_capture = nullptr;
            emit error("无法打开 USB 相机: " + id);
            return false;
        }
        m_capture->set(cv::CAP_PROP_FRAME_WIDTH, m_width);
        m_capture->set(cv::CAP_PROP_FRAME_HEIGHT, m_height);
        m_capture->set(cv::CAP_PROP_FPS, m_fps);
        return true;
    }

    // Basler: TODO: 需要 Pylon SDK 链接后实现
    if (backend == CameraBackend::Basler) {
        emit error("Basler 相机后端暂未实现（需要 Pylon SDK）");
        return false;
    }

    // Hikvision: TODO: 需要 MVS SDK 链接后实现
    if (backend == CameraBackend::Hikvision) {
        emit error("海康相机后端暂未实现（需要 MVS SDK）");
        return false;
    }

    return false;
}

void CameraWorker::close()
{
    stopGrab();
    if (m_capture) {
        m_capture->release();
        delete m_capture;
        m_capture = nullptr;
    }
}

void CameraWorker::startGrab()
{
    if (!m_capture) {
        emit error("相机未打开，无法开始采集");
        return;
    }
    m_running = true;
    m_failCount = 0;
    m_fpsTimer.start();
    m_frameCount = 0;
    int intervalMs = qMax(1, 1000 / m_fps);
    m_grabTimer->start(intervalMs);
}

void CameraWorker::stopGrab()
{
    m_running = false;
    m_grabTimer->stop();
}

void CameraWorker::grabFrame()
{
    if (!m_running || !m_capture) return;

    QImage qimg = grabUSB();
    if (!qimg.isNull()) {
        m_failCount = 0;
        m_frameCount++;

        if (m_fpsTimer.elapsed() >= 1000) {
            double fps = m_frameCount * 1000.0 / m_fpsTimer.elapsed();
            emit fpsUpdated(fps);
            m_frameCount = 0;
            m_fpsTimer.restart();
        }

        emit frameReady(qimg);
    } else {
        m_failCount++;
        if (m_failCount >= MAX_FAIL_COUNT) {
            emit error("采集超时，连续 " + QString::number(m_failCount) + " 次失败");
            m_failCount = 0;
        }
    }
}

QImage CameraWorker::grabUSB()
{
    if (!m_capture || !m_capture->isOpened()) return QImage();

    cv::Mat frame;
    if (!m_capture->read(frame)) {
        return QImage();
    }

    if (frame.empty()) return QImage();

    // BGR → RGB
    if (frame.channels() == 3) {
        cv::cvtColor(frame, frame, cv::COLOR_BGR2RGB);
    }

    // cv::Mat → QImage (deep copy)
    QImage img(frame.data, frame.cols, frame.rows, static_cast<int>(frame.step),
               QImage::Format_RGB888);
    return img.copy();
}

// ── CameraManager ────────────────────────────────────────────────

CameraManager::CameraManager(QObject *parent)
    : QObject(parent)
{
}

CameraManager::~CameraManager()
{
    stopWorker();
}

QStringList CameraManager::enumerateCameras()
{
    QStringList cameras;

    // USB cameras via OpenCV — use CAP_DSHOW on Windows for stability
    for (int i = 0; i < 4; i++) {
        cv::VideoCapture cap(i, cv::CAP_DSHOW);
        if (cap.isOpened()) {
            cameras << QString("USB Camera %1").arg(i);
            cap.release();
        }
    }

    // Basler via Pylon (TODO)
    // Hikvision via MVS (TODO)

    if (cameras.isEmpty()) {
        // Always provide at least one entry so UI is functional
        cameras << "USB Camera 0";
    }
    return cameras;
}

bool CameraManager::open(const QString &id)
{
    QMutexLocker lock(&m_mutex);
    m_currentId = id;
    m_backend   = detectBackend(id);

    startWorker();

    bool ok = false;
    QMetaObject::invokeMethod(m_worker, [this, &ok]() {
        ok = m_worker->open(m_currentId, m_backend, m_width, m_height, m_fps, m_acquisitionTimeoutMs);
    }, Qt::BlockingQueuedConnection);

    return ok;
}

void CameraManager::close()
{
    QMutexLocker lock(&m_mutex);
    stopAcquisition();
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, &CameraWorker::close, Qt::BlockingQueuedConnection);
    }
    stopWorker();
}

bool CameraManager::isOpen() const
{
    return m_worker != nullptr;
}

void CameraManager::startAcquisition()
{
    m_acquiring = true;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, &CameraWorker::startGrab, Qt::QueuedConnection);
    }
}

void CameraManager::stopAcquisition()
{
    m_acquiring = false;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, &CameraWorker::stopGrab, Qt::QueuedConnection);
    }
}

bool CameraManager::isAcquiring() const
{
    return m_acquiring;
}

void CameraManager::setResolution(int width, int height)
{
    m_width  = width;
    m_height = height;
}

void CameraManager::setFps(int fps)
{
    m_fps = fps;
}

CameraBackend CameraManager::detectBackend(const QString &id) const
{
    if (id.contains("USB", Qt::CaseInsensitive))    return CameraBackend::USB;
    if (id.contains("Basler", Qt::CaseInsensitive))  return CameraBackend::Basler;
    if (id.contains("Hik", Qt::CaseInsensitive) || id.contains("MV-", Qt::CaseInsensitive))
        return CameraBackend::Hikvision;
    return CameraBackend::USB;
}

void CameraManager::startWorker()
{
    if (m_workerThread) return;
    m_workerThread = new QThread(this);
    m_worker       = new CameraWorker;
    m_worker->moveToThread(m_workerThread);

    connect(m_worker, &CameraWorker::frameReady, this, &CameraManager::onWorkerFrameReady);
    connect(m_worker, &CameraWorker::error,       this, &CameraManager::onWorkerError);
    connect(m_worker, &CameraWorker::fpsUpdated, this, &CameraManager::fpsUpdated);
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_workerThread->start();
}

void CameraManager::stopWorker()
{
    if (!m_workerThread) return;
    m_workerThread->quit();
    m_workerThread->wait(3000);
    m_workerThread = nullptr;
    m_worker       = nullptr;
}

void CameraManager::onWorkerFrameReady(const QImage &image)
{
    emit frameReady(image);
}

void CameraManager::onWorkerError(const QString &error)
{
    emit acquisitionError(error);
}

#include "CameraManager.moc"
