#include "RobotClient.h"
#include "CoordTransform.h"
#include <QTcpSocket>
#include <QHostAddress>
#include <QDebug>

// ── CRC-16/MODBUS lookup table ──────────────────────────────────

static const quint16 crc16Table[256] = {
    0x0000,0xC0C1,0xC181,0x0140,0xC301,0x03C0,0x0280,0xC241,
    0xC601,0x06C0,0x0780,0xC741,0x0500,0xC5C1,0xC481,0x0440,
    0xCC01,0x0CC0,0x0D80,0xCD41,0x0F00,0xCFC1,0xCE81,0x0E40,
    0x0A00,0xCAC1,0xCB81,0x0B40,0xC901,0x09C0,0x0880,0xC841,
    0xD801,0x18C0,0x1980,0xD941,0x1B00,0xDBC1,0xDA81,0x1A40,
    0x1E00,0xDEC1,0xDF81,0x1F40,0xDD01,0x1DC0,0x1C80,0xDC41,
    0x1400,0xD4C1,0xD581,0x1540,0xD701,0x17C0,0x1680,0xD641,
    0xD201,0x12C0,0x1380,0xD341,0x1100,0xD1C1,0xD081,0x1040,
    0xF001,0x30C0,0x3180,0xF141,0x3300,0xF3C1,0xF281,0x3240,
    0x3600,0xF6C1,0xF781,0x3740,0xF501,0x35C0,0x3480,0xF441,
    0x3C00,0xFCC1,0xFD81,0x3D40,0xFF01,0x3FC0,0x3E80,0xFE41,
    0xFA01,0x3AC0,0x3B80,0xFB41,0x3900,0xF9C1,0xF881,0x3840,
    0x2800,0xE8C1,0xE981,0x2940,0xEB01,0x2BC0,0x2A80,0xEA41,
    0xEE01,0x2EC0,0x2F80,0xEF41,0x2D00,0xEDC1,0xEC81,0x2C40,
    0xE401,0x24C0,0x2580,0xE541,0x2700,0xE7C1,0xE681,0x2640,
    0x2200,0xE2C1,0xE381,0x2340,0xE101,0x21C0,0x2080,0xE041,
    0xA001,0x60C0,0x6180,0xA141,0x6300,0xA3C1,0xA281,0x6240,
    0x6600,0xA6C1,0xA781,0x6740,0xA501,0x65C0,0x6480,0xA441,
    0x6C00,0xACC1,0xAD81,0x6D40,0xAF01,0x6FC0,0x6E80,0xAE41,
    0xAA01,0x6AC0,0x6B80,0xAB41,0x6900,0xA9C1,0xA881,0x6840,
    0x7800,0xB8C1,0xB981,0x7940,0xBB01,0x7BC0,0x7A80,0xBA41,
    0xBE01,0x7EC0,0x7F80,0xBF41,0x7D00,0xBDC1,0xBC81,0x7C40,
    0xB401,0x74C0,0x7580,0xB541,0x7700,0xB7C1,0xB681,0x7640,
    0x7200,0xB2C1,0xB381,0x7340,0xB101,0x71C0,0x7080,0xB041,
    0x5000,0x90C1,0x9181,0x5140,0x9301,0x53C0,0x5280,0x9241,
    0x9601,0x56C0,0x5780,0x9741,0x5500,0x95C1,0x9481,0x5440,
    0x9C01,0x5CC0,0x5D80,0x9D41,0x5F00,0x9FC1,0x9E81,0x5E40,
    0x5A00,0x9AC1,0x9B81,0x5B40,0x9901,0x59C0,0x5880,0x9841,
    0x8801,0x48C0,0x4980,0x8941,0x4B00,0x8BC1,0x8A81,0x4A40,
    0x4E00,0x8EC1,0x8F81,0x4F40,0x8D01,0x4DC0,0x4C80,0x8C41,
    0x4400,0x84C1,0x8581,0x4540,0x8701,0x47C0,0x4680,0x8641,
    0x8201,0x42C0,0x4380,0x8341,0x4100,0x81C1,0x8081,0x4040
};

// ── CRC helper ───────────────────────────────────────────────────

quint16 computeCrc16Modbus(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (int i = 0; i < data.size(); i++) {
        quint8 idx = quint8(crc ^ quint8(data[i]));
        crc = (crc >> 8) ^ crc16Table[idx];
    }
    return crc;
}

// ── RobotFrame ───────────────────────────────────────────────────

QByteArray RobotFrame::serialize() const
{
    QByteArray frame;
    frame.append(char(headerH));
    frame.append(char(headerL));
    frame.append(char(cmd));

    // Data length (big-endian)
    quint16 len = quint16(data.size());
    frame.append(char((len >> 8) & 0xFF));
    frame.append(char(len & 0xFF));

    // Data payload
    frame.append(data);

    // CRC-16/MODBUS over CMD + LEN + DATA
    QByteArray crcInput;
    crcInput.append(char(cmd));
    crcInput.append(char((len >> 8) & 0xFF));
    crcInput.append(char(len & 0xFF));
    crcInput.append(data);

    quint16 crcVal = computeCrc16Modbus(crcInput);
    frame.append(char(crcVal & 0xFF));        // CRC low byte
    frame.append(char((crcVal >> 8) & 0xFF)); // CRC high byte

    // Tail
    frame.append(char(tail));
    return frame;
}

bool RobotFrame::deserialize(const QByteArray &raw, RobotFrame &frame)
{
    // Minimum size: 2H + 1CMD + 2LEN + 2CRC + 1Tail = 8 bytes
    if (raw.size() < 8) return false;

    // Validate header
    if (quint8(raw[0]) != 0xAA || quint8(raw[1]) != 0xFF) return false;

    frame.cmd     = quint8(raw[2]);
    frame.dataLen = quint16((quint8(raw[3]) << 8) | quint8(raw[4]));

    // DoS protection: cap maximum payload size
    constexpr int MAX_PAYLOAD = 1024;
    if (frame.dataLen > MAX_PAYLOAD) return false;

    // Check full frame size
    if (raw.size() < 8 + frame.dataLen) return false;

    frame.data = raw.mid(5, frame.dataLen);

    int crcPos = 5 + frame.dataLen;
    frame.crc  = quint16((quint8(raw[crcPos + 1]) << 8) |
                          quint8(raw[crcPos]));

    // Validate CRC
    QByteArray crcInput;
    crcInput.append(char(frame.cmd));
    crcInput.append(char((frame.dataLen >> 8) & 0xFF));
    crcInput.append(char(frame.dataLen & 0xFF));
    crcInput.append(frame.data);
    if (computeCrc16Modbus(crcInput) != frame.crc) return false;

    // Validate tail
    frame.tail = quint8(raw[crcPos + 2]);
    if (frame.tail != 0x0D) return false;

    return true;
}

// ── RobotCommunicator (worker) ───────────────────────────────────

RobotCommunicator::RobotCommunicator(QObject *parent)
    : QObject(parent)
{
    QTcpSocket *sock = new QTcpSocket(this);
    m_socket = sock;

    // Forward socket signals
    connect(sock, &QTcpSocket::connected,    this, &RobotCommunicator::connected);
    connect(sock, &QTcpSocket::disconnected, this, &RobotCommunicator::disconnected);
    connect(sock, &QTcpSocket::errorOccurred, this, [this, sock]() {
        emit errorOccurred(sock->errorString());
    });
    connect(sock, &QTcpSocket::readyRead,    this, &RobotCommunicator::onReadyRead);
}

bool RobotCommunicator::connectToHost(const QString &ip, quint16 port, int timeoutMs)
{
    QTcpSocket *sock = static_cast<QTcpSocket*>(m_socket);
    sock->connectToHost(QHostAddress(ip), port);
    return sock->waitForConnected(timeoutMs);
}

void RobotCommunicator::disconnect()
{
    QTcpSocket *sock = static_cast<QTcpSocket*>(m_socket);
    if (sock->state() == QAbstractSocket::ConnectedState)
        sock->disconnectFromHost();
}

bool RobotCommunicator::isConnected() const
{
    return static_cast<QTcpSocket*>(m_socket)->state() == QAbstractSocket::ConnectedState;
}

bool RobotCommunicator::sendFrame(const RobotFrame &frame)
{
    QTcpSocket *sock = static_cast<QTcpSocket*>(m_socket);
    if (!isConnected()) return false;
    qint64 written = sock->write(frame.serialize());
    return written > 0;
}

void RobotCommunicator::onReadyRead()
{
    QTcpSocket *sock = static_cast<QTcpSocket*>(m_socket);
    m_rxBuffer.append(sock->readAll());
    tryParseFrame();
}

void RobotCommunicator::tryParseFrame()
{
    // Search for frame header 0xAA 0xFF
    while (m_rxBuffer.size() >= 8) {
        int headerPos = m_rxBuffer.indexOf(QByteArray::fromRawData("\xAA\xFF", 2));
        if (headerPos < 0) {
            m_rxBuffer.clear(); // no valid header found, discard
            return;
        }

        // Discard data before header
        if (headerPos > 0) {
            m_rxBuffer.remove(0, headerPos);
        }

        if (m_rxBuffer.size() < 8) return; // need minimum frame size

        // Extract data length
        quint16 dataLen = quint16((quint8(m_rxBuffer[3]) << 8) | quint8(m_rxBuffer[4]));
        constexpr int MAX_PAYLOAD = 1024;
        if (dataLen > MAX_PAYLOAD) {
            m_rxBuffer.remove(0, 1); // skip one byte and retry
            continue;
        }

        int frameSize = 8 + dataLen;
        if (m_rxBuffer.size() < frameSize) return; // incomplete frame, wait for more data

        // Extract complete frame
        QByteArray frameData = m_rxBuffer.left(frameSize);
        m_rxBuffer.remove(0, frameSize);

        RobotFrame frame;
        if (RobotFrame::deserialize(frameData, frame)) {
            emit frameReceived(frameData);
        }
        // Invalid frames are silently dropped; tryParseFrame continues
    }
}

// ── RobotClient ──────────────────────────────────────────────────

RobotClient::RobotClient(QObject *parent)
    : QObject(parent)
{
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &RobotClient::onReconnectTimer);

    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &RobotClient::onHeartbeatTimer);
}

RobotClient::~RobotClient()
{
    disconnect();
    stopWorker();
}

void RobotClient::setConnectionParams(const QString &ip, quint16 port, int timeoutMs)
{
    m_robotIp   = ip;
    m_robotPort = port;
    m_timeoutMs = timeoutMs;
}

void RobotClient::setWhitelist(const QStringList &allowedIps)
{
    m_ipWhitelist = allowedIps;
}

// ── Connection ───────────────────────────────────────────────────

bool RobotClient::connectToRobot()
{
    if (!validatePeerAddress()) {
        emit fault(QString("IP %1 不在白名单中，连接被拒绝").arg(m_robotIp));
        setState(RobotState::FAULT);
        return false;
    }

    if (m_state == RobotState::CONNECTED) return true;
    if (m_state == RobotState::FAULT) {
        resetReconnectCounter();
    }

    setState(RobotState::CONNECTING);
    startWorker();

    bool ok = false;
    QMetaObject::invokeMethod(m_worker, [this, &ok]() {
        ok = m_worker->connectToHost(m_robotIp, m_robotPort, m_timeoutMs);
    }, Qt::BlockingQueuedConnection);

    if (ok) {
        onSocketConnected();
    } else {
        emit connectionLost();
    }
    return ok;
}

void RobotClient::disconnect()
{
    m_heartbeatTimer->stop();
    m_reconnectTimer->stop();
    resetReconnectCounter();

    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, &RobotCommunicator::disconnect,
                                  Qt::BlockingQueuedConnection);
    }
    setState(RobotState::DISCONNECTED);
}

bool RobotClient::isConnected() const
{
    return m_state == RobotState::CONNECTED;
}

// ── Send commands ────────────────────────────────────────────────

bool RobotClient::sendGrabPose(double x, double y, double angle)
{
    if (m_state != RobotState::CONNECTED) return false;

    RobotFrame frame;
    frame.cmd = RobotCmd::SEND_POSE;

    // Pack: X(4B float) Y(4B float) Angle(4B float) in IEEE 754 big-endian
    QByteArray payload;
    auto packFloat = [&](float v) {
        quint32 raw;
        memcpy(&raw, &v, 4);
        payload.append(char((raw >> 24) & 0xFF));
        payload.append(char((raw >> 16) & 0xFF));
        payload.append(char((raw >> 8) & 0xFF));
        payload.append(char(raw & 0xFF));
    };
    packFloat(float(x));
    packFloat(float(y));
    packFloat(float(angle));

    frame.data = payload;

    QMutexLocker lock(&m_sendMutex);
    bool ok = false;
    QMetaObject::invokeMethod(m_worker, [this, frame, &ok]() {
        ok = m_worker->sendFrame(frame);
    }, Qt::BlockingQueuedConnection);

    emit poseSent(ok);
    return ok;
}

bool RobotClient::sendEstop()
{
    RobotFrame frame;
    frame.cmd  = RobotCmd::ESTOP;
    frame.data = QByteArray();
    QMutexLocker lock(&m_sendMutex);
    bool ok = false;
    QMetaObject::invokeMethod(m_worker, [this, frame, &ok]() {
        ok = m_worker->sendFrame(frame);
    }, Qt::BlockingQueuedConnection);
    return ok;
}

RobotStatus RobotClient::queryStatus()
{
    RobotFrame frame;
    frame.cmd = RobotCmd::QUERY;
    // ... send and wait for response ...
    return RobotStatus::STATUS_UNKNOWN;
}

RobotToolPose RobotClient::queryToolPose(int timeoutMs)
{
    if (m_state != RobotState::CONNECTED) return RobotToolPose{};

    RobotFrame frame;
    frame.cmd  = RobotCmd::QUERY_POSE;
    frame.data = QByteArray();

    {
        QMutexLocker lock(&m_poseRespMutex);
        m_poseRespPending = true;
        m_lastPoseResp = RobotToolPose{};
    }

    QMetaObject::invokeMethod(m_worker, [this, frame]() {
        m_worker->sendFrame(frame);
    }, Qt::QueuedConnection);

    {
        QMutexLocker lock(&m_poseRespMutex);
        if (m_poseRespPending) {
            m_poseRespCondition.wait(&m_poseRespMutex, timeoutMs);
        }
    }
    return m_lastPoseResp;
}

RobotState RobotClient::state() const
{
    QReadLocker lock(&m_stateLock);
    return m_state;
}

// ── Private slots ────────────────────────────────────────────────

void RobotClient::onSocketConnected()
{
    setState(RobotState::CONNECTED);
    resetReconnectCounter();
    m_heartbeatTimer->start(HEARTBEAT_INTERVAL_MS);
    emit connected();
}

void RobotClient::onSocketDisconnected()
{
    m_heartbeatTimer->stop();
    if (m_state == RobotState::CONNECTED || m_state == RobotState::CONNECTING) {
        setState(RobotState::RECONNECT_WAITING);
        emit connectionLost();
        scheduleReconnect();
    }
}

void RobotClient::onSocketError(const QString &error)
{
    Q_UNUSED(error)
    onSocketDisconnected();
}

void RobotClient::onDataReceived(const QByteArray &rawData)
{
    RobotFrame frame;
    if (!RobotFrame::deserialize(rawData, frame)) return;

    if (frame.cmd == 0x84 && frame.data.size() >= 24) {
        // Pose response: 6 float32 BE: tx, ty, tz, rx, ry, rz
        auto unpackFloat = [&](int offset) {
            quint32 raw = (quint8(frame.data[offset])   << 24) |
                          (quint8(frame.data[offset+1]) << 16) |
                          (quint8(frame.data[offset+2]) << 8)  |
                          (quint8(frame.data[offset+3]));
            float f;
            memcpy(&f, &raw, 4);
            return double(f);
        };
        QMutexLocker lock(&m_poseRespMutex);
        m_lastPoseResp.tx = unpackFloat(0);
        m_lastPoseResp.ty = unpackFloat(4);
        m_lastPoseResp.tz = unpackFloat(8);
        m_lastPoseResp.rx = unpackFloat(12);
        m_lastPoseResp.ry = unpackFloat(16);
        m_lastPoseResp.rz = unpackFloat(20);
        m_poseRespPending = false;
        m_poseRespCondition.wakeAll();

    } else if (frame.cmd == 0x81 || frame.cmd == 0x82 || frame.cmd == 0x83) {
        RobotStatus st = static_cast<RobotStatus>(frame.cmd);
        emit statusChanged(st);
    }
}

void RobotClient::onReconnectTimer()
{
    if (m_state == RobotState::FAULT) return;

    m_reconnectCount++;
    if (m_reconnectCount > MAX_RECONNECT) {
        setState(RobotState::FAULT);
        emit fault("重连次数超过最大限制 (" + QString::number(MAX_RECONNECT) + ")");
        return;
    }

    setState(RobotState::CONNECTING);
    if (!m_worker) return;

    // Fire-and-forget: previously this used BlockingQueuedConnection +
    // waitForConnected(1000ms), which froze the GUI for up to 1 s per retry.
    // Now we kick off the connect on the worker thread and let the worker's
    // `connected` / `disconnected` signals drive the next state transition —
    // success calls onSocketConnected(), failure calls onSocketDisconnected()
    // which (via this same timer chain) schedules the next retry.
    QMetaObject::invokeMethod(m_worker, [this]() {
        m_worker->connectToHost(m_robotIp, m_robotPort, m_timeoutMs);
    }, Qt::QueuedConnection);
}

void RobotClient::onHeartbeatTimer()
{
    sendHeartbeat();
}

void RobotClient::onWorkerFrameReceived(const QByteArray &data)
{
    onDataReceived(data);
}

// ── Internal helpers ─────────────────────────────────────────────

void RobotClient::startWorker()
{
    if (m_workerThread) return;
    m_workerThread = new QThread(this);
    m_worker       = new RobotCommunicator;
    m_worker->moveToThread(m_workerThread);

    connect(m_worker, &RobotCommunicator::connected,       this, &RobotClient::onSocketConnected);
    connect(m_worker, &RobotCommunicator::disconnected,    this, &RobotClient::onSocketDisconnected);
    connect(m_worker, &RobotCommunicator::errorOccurred,   this, &RobotClient::onSocketError);
    connect(m_worker, &RobotCommunicator::frameReceived,   this, &RobotClient::onWorkerFrameReceived);
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_workerThread->start();
}

void RobotClient::stopWorker()
{
    if (!m_workerThread) return;
    m_workerThread->quit();
    m_workerThread->wait(3000);
    m_workerThread = nullptr;
    m_worker       = nullptr;
}

void RobotClient::setState(RobotState newState)
{
    QWriteLocker lock(&m_stateLock);
    if (m_state != newState) {
        m_state = newState;
        emit stateChanged(m_state);
    }
}

void RobotClient::sendHeartbeat()
{
    RobotFrame frame;
    frame.cmd  = RobotCmd::HEARTBEAT;   // 0x00
    frame.data = QByteArray();

    QMutexLocker lock(&m_sendMutex);
    if (m_worker && m_state == RobotState::CONNECTED) {
        QMetaObject::invokeMethod(m_worker, [this, frame]() {
            m_worker->sendFrame(frame);
        }, Qt::QueuedConnection);
    }
}

void RobotClient::scheduleReconnect()
{
    setState(RobotState::RECONNECT_WAITING);
    m_reconnectTimer->start(RECONNECT_INTERVAL_MS);
}

void RobotClient::resetReconnectCounter()
{
    m_reconnectCount = 0;
}

bool RobotClient::validatePeerAddress() const
{
    if (m_ipWhitelist.isEmpty()) return true;
    return m_ipWhitelist.contains(m_robotIp);
}
