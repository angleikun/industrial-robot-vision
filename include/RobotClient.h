#ifndef ROBOTCLIENT_H
#define ROBOTCLIENT_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QReadWriteLock>
#include <QTimer>
#include <QString>
#include <atomic>
#include <QWaitCondition>
#include "CoordTransform.h"

// ── Robot communication state machine ────────────────────────────
enum class RobotState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    RECONNECT_WAITING,
    FAULT
};

enum class RobotStatus {
    READY          = 0x81,
    BUSY           = 0x82,
    ERR            = 0x83,
    STATUS_UNKNOWN = 0x00
};

// Command codes
namespace RobotCmd {
    constexpr quint8 HEARTBEAT   = 0x00;
    constexpr quint8 SEND_POSE   = 0x01;
    constexpr quint8 QUERY       = 0x02;
    constexpr quint8 ESTOP       = 0x03;
    constexpr quint8 QUERY_POSE  = 0x04;
}

// Binary frame (see spec function 6)
struct RobotFrame {
    quint8  headerH  = 0xAA;
    quint8  headerL  = 0xFF;
    quint8  cmd      = 0x00;
    quint16 dataLen  = 0;
    QByteArray data;
    quint16 crc      = 0;
    quint8  tail     = 0x0D;

    QByteArray serialize() const;
    static bool deserialize(const QByteArray &raw, RobotFrame &frame);
};

quint16 computeCrc16Modbus(const QByteArray &data);

class RobotCommunicator;

class RobotClient : public QObject {
    Q_OBJECT

public:
    explicit RobotClient(QObject *parent = nullptr);
    ~RobotClient() override;

    void setConnectionParams(const QString &ip, quint16 port, int timeoutMs = 3000);
    void setWhitelist(const QStringList &allowedIps);

    bool connectToRobot();
    void disconnect();
    bool isConnected() const;

    bool sendGrabPose(double x, double y, double angle);
    bool sendEstop();
    RobotStatus queryStatus();
    RobotToolPose queryToolPose(int timeoutMs = 1000);

    RobotState state() const;

signals:
    void connected();
    void disconnected();
    void stateChanged(RobotState state);
    void statusChanged(RobotStatus status);
    void poseSent(bool success);
    void connectionLost();
    void fault(const QString &reason);
    void heartbeatTimeout();

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketError(const QString &error);
    void onDataReceived(const QByteArray &data);
    void onReconnectTimer();
    void onHeartbeatTimer();
    void onWorkerFrameReceived(const QByteArray &data);

private:
    void startWorker();
    void stopWorker();
    void setState(RobotState newState);
    void sendHeartbeat();
    void scheduleReconnect();
    void resetReconnectCounter();
    bool validatePeerAddress() const;

    // ── State ──
    RobotState m_state = RobotState::DISCONNECTED;
    mutable QReadWriteLock m_stateLock;

    // ── Connection params ──
    QString  m_robotIp;
    quint16  m_robotPort  = 5000;
    int      m_timeoutMs  = 3000;
    QStringList m_ipWhitelist;

    // ── Reconnect ──
    QTimer  *m_reconnectTimer = nullptr;
    int      m_reconnectCount = 0;
    static constexpr int MAX_RECONNECT = 5;
    static constexpr int RECONNECT_INTERVAL_MS = 5000;

    // ── Heartbeat ──
    QTimer  *m_heartbeatTimer = nullptr;
    static constexpr int HEARTBEAT_INTERVAL_MS = 1000;

    // ── Worker thread ──
    QThread           *m_workerThread = nullptr;
    RobotCommunicator *m_worker       = nullptr;

    QMutex m_sendMutex;

    // ── Pose query sync ──
    QMutex       m_poseRespMutex;
    QWaitCondition m_poseRespCondition;
    RobotToolPose  m_lastPoseResp;
    bool           m_poseRespPending = false;
};

// ── Internal worker (runs in worker thread) ──────────────────────
class RobotCommunicator : public QObject {
    Q_OBJECT

public:
    explicit RobotCommunicator(QObject *parent = nullptr);
    bool connectToHost(const QString &ip, quint16 port, int timeoutMs);
    void disconnect();
    bool isConnected() const;
    bool sendFrame(const RobotFrame &frame);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &error);
    void frameReceived(const QByteArray &data);

private slots:
    void onReadyRead();

private:
    void tryParseFrame();
    void *m_socket    = nullptr;  // QTcpSocket* opaque pointer
    QByteArray m_rxBuffer;
};

inline QString robotStateToString(RobotState s) {
    switch (s) {
        case RobotState::DISCONNECTED:      return "DISCONNECTED";
        case RobotState::CONNECTING:        return "CONNECTING";
        case RobotState::CONNECTED:         return "CONNECTED";
        case RobotState::RECONNECT_WAITING:  return "RECONNECT_WAITING";
        case RobotState::FAULT:             return "FAULT";
    }
    return "UNKNOWN";
}

inline QString robotStatusToString(RobotStatus s) {
    switch (s) {
        case RobotStatus::READY: return "就绪";
        case RobotStatus::BUSY:  return "执行中";
        case RobotStatus::ERR:            return "错误";
        case RobotStatus::STATUS_UNKNOWN: return "未知";
    }
    return "未知";
}

#endif // ROBOTCLIENT_H
