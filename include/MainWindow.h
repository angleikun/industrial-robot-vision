#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QImage>
#include <QThread>

#include "JsonSettings.h"

#include "VisionProcessor.h"     // for MatchResult
#include "MeasurementEngine.h"   // for MeasureType
#include "RobotClient.h"         // for RobotStatus, RobotState

class ImageView;
class CameraManager;
class CalibrationManager;
class CoordTransform;
class DatabaseManager;
class ReportManager;

class QDockWidget;
class QTextEdit;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    // Camera
    void onCameraSelected(int index);
    void onStartAcquisition();
    void onStopAcquisition();
    void onFrameReady(const QImage &image);
    void onFpsUpdated(double fps);

    // Template matching
    void onCreateTemplate();
    void onLoadTemplate();
    void onMatchResult(const QList<MatchResult> &results);

    // Calibration
    void onCalibrateCamera();
    void onCalibrateHandEye();

    // Measurement
    void onMeasure(MeasureType type);
    void onPointPicked(const QPointF &p);
    void onRoiSelectedForMeasure(const QRectF &roi);

    // Robot
    void onConnectRobot();
    void onSendGrabPose();
    void onRobotStatusChanged(RobotStatus status);
    void onRobotFault();

    // Data
    void onQueryHistory();
    void onExportReport();
    void onExportCSV();

    // License
    void onLicenseStatusChanged(bool available);

private:
    void setupUi();
    void setupDockWidgets();
    void setupControlDock();
    void setupParamDock();
    void setupBottomDocks();
    void setupMenuBar();
    void setupStatusBar();
    void setupConnections();
    void loadSettings();
    void saveSettings();
    void updateLicenseIndicator(bool available);
    void checkHalconLicense();
    void applyMatchConfig();   // push UI match params (score, max targets) into VisionProcessor
    void createTemplateWithRoi(const QRectF &roi);   // create+save template from a selected ROI
    void appendLog(const QString &msg);
    void doExport(const QString &format);

    // Dock widgets
    QDockWidget *m_controlDock  = nullptr;
    QDockWidget *m_paramDock    = nullptr;
    QDockWidget *m_logDock      = nullptr;
    QDockWidget *m_historyDock  = nullptr;

    // Central widget
    ImageView *m_imageView = nullptr;

    // Control widgets
    QComboBox     *m_cameraCombo   = nullptr;
    QPushButton   *m_btnStartAcq   = nullptr;
    QPushButton   *m_btnStopAcq    = nullptr;
    QPushButton   *m_btnCreateTpl  = nullptr;
    QPushButton   *m_btnLoadTpl    = nullptr;
    QPushButton   *m_btnConnect    = nullptr;
    QPushButton   *m_btnSendPose   = nullptr;

    // Param widgets
    QDoubleSpinBox *m_scoreThresh  = nullptr;
    QSpinBox      *m_maxTargets    = nullptr;
    QComboBox     *m_measureType   = nullptr;

    // Log
    QTextEdit *m_logView = nullptr;

    // Status bar
    QLabel *m_statusCamera     = nullptr;
    QLabel *m_statusRobot      = nullptr;
    QLabel *m_statusLicense    = nullptr;
    QLabel *m_statusFps        = nullptr;
    QLabel *m_statusDetection  = nullptr;   // live detection result — updates per frame without logging
    int     m_lastValidCount   = -1;        // for match-state transition logging

    // Core modules (owned)
    CameraManager     *m_cameraMgr      = nullptr;
    VisionProcessor   *m_visionProc     = nullptr;
    CalibrationManager *m_calibMgr      = nullptr;
    MeasurementEngine *m_measureEng     = nullptr;
    CoordTransform    *m_coordTrans     = nullptr;
    RobotClient       *m_robotClient    = nullptr;
    DatabaseManager   *m_dbMgr          = nullptr;
    ReportManager     *m_reportMgr      = nullptr;

    QThread        *m_visionThread  = nullptr;

    // Point-picking state
    MeasureType m_pendingMeasureType = MeasureType::LENGTH;
    QList<QPointF> m_collectedPoints;
    int m_pointsNeeded = 0;
    bool m_awaitingTemplateRoi = false;   // true while waiting for the user to drag a template ROI
    RobotStatus m_lastRobotStatus = RobotStatus::STATUS_UNKNOWN;  // for de-duping status logs
    RobotState  m_lastRobotState  = RobotState::DISCONNECTED;     // for de-duping state logs
    qint64      m_lastConnLostMs  = 0;     // for de-duping repeated "连接丢失" logs
    MatchResult m_lastValidMatch;          // cached most-recent valid detection
    qint64      m_lastValidMatchMs = 0;    // when m_lastValidMatch was captured
    QTimer     *m_t4Timer = nullptr;       // T4 stability test: fires onSendGrabPose every 30 s

    JsonSettings *m_settings = nullptr;
    QList<MatchResult> m_lastDetections;
    bool m_licenseAvailable = false;
    bool m_degradedMode     = false;
};

#endif // MAINWINDOW_H
