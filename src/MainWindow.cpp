#include "MainWindow.h"
#include "ImageView.h"
#include "Logger.h"
#include "CalibrationWizard.h"
#include "CameraManager.h"
#include "VisionProcessor.h"
#include "CalibrationManager.h"
#include "MeasurementEngine.h"
#include "CoordTransform.h"
#include "RobotClient.h"
#include "DatabaseManager.h"
#include "ReportManager.h"

#include <QDockWidget>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QTextEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QDateTime>
#include <QDir>

#include <cmath>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("工业机器人视觉引导系统 v1.2");
    resize(1400, 900);
    setMinimumSize(1024, 768);

    m_settings = new JsonSettings(
        QApplication::applicationDirPath() + "/config/settings.json", this);

    // Create modules
    m_cameraMgr   = new CameraManager(this);
    m_calibMgr    = new CalibrationManager(this);
    m_visionProc  = new VisionProcessor;  // no parent: will be moved to worker thread
    m_visionThread = new QThread(this);
    m_visionProc->moveToThread(m_visionThread);
    connect(m_visionThread, &QThread::finished, m_visionProc, &QObject::deleteLater);
    m_visionThread->start();
    m_measureEng  = new MeasurementEngine(this);
    m_coordTrans  = new CoordTransform(this);
    m_robotClient = new RobotClient(this);
    m_dbMgr       = new DatabaseManager(this);
    m_reportMgr   = new ReportManager(this);

    setupUi();
    setupDockWidgets();
    setupMenuBar();
    setupStatusBar();
    setupConnections();
    loadSettings();
    applyMatchConfig();   // push loaded score/max-targets into the matcher
    checkHalconLicense();

    appendLog("系统启动完成。");
}

MainWindow::~MainWindow()
{
    saveSettings();

    // Tear-down order matters: stop producers BEFORE shutting down sinks.
    // 1. Stop the camera worker — no more frames produced.
    if (m_cameraMgr && m_cameraMgr->isAcquiring()) {
        m_cameraMgr->stopAcquisition();
    }

    // 2. Stop the vision thread — drains any in-flight match, then exits.
    //    Until this returns, m_visionProc may still emit matchingComplete
    //    which routes to onMatchResult → m_dbMgr->saveDetection. So the DB
    //    must still be alive at this point.
    if (m_visionThread) {
        m_visionThread->quit();
        m_visionThread->wait(3000);
    }

    // 3. Disconnect the robot — no more outbound frames.
    if (m_robotClient) {
        m_robotClient->disconnect();
    }

    // 4. Finally close the database. The ~DatabaseManager will also flush
    //    the write queue before closing — so any records produced during the
    //    shutdown drain above are not lost.
    if (m_dbMgr) {
        m_dbMgr->close();
    }
}

// ── UI Setup ─────────────────────────────────────────────────────

void MainWindow::setupUi()
{
    m_imageView = new ImageView(this);
    setCentralWidget(m_imageView);
}

void MainWindow::setupDockWidgets()
{
    setupControlDock();
    setupParamDock();
    setupBottomDocks();
}

void MainWindow::setupControlDock()
{
    m_controlDock = new QDockWidget("控制面板", this);
    auto *ctrlWidget = new QWidget;
    auto *ctrlLayout = new QVBoxLayout(ctrlWidget);

    // Camera group
    auto *camGroup = new QGroupBox("相机");
    auto *camLayout = new QVBoxLayout(camGroup);
    m_cameraCombo = new QComboBox;
    m_btnStartAcq = new QPushButton("开始采集");
    m_btnStopAcq  = new QPushButton("停止采集");
    m_btnStopAcq->setEnabled(false);
    camLayout->addWidget(new QLabel("选择相机:"));
    camLayout->addWidget(m_cameraCombo);
    camLayout->addWidget(m_btnStartAcq);
    camLayout->addWidget(m_btnStopAcq);
    ctrlLayout->addWidget(camGroup);

    // Template group
    auto *tplGroup = new QGroupBox("模板操作");
    auto *tplLayout = new QVBoxLayout(tplGroup);
    m_btnCreateTpl = new QPushButton("创建模板");
    m_btnLoadTpl   = new QPushButton("加载模板");
    m_btnCreateTpl->setEnabled(false);
    m_btnLoadTpl->setEnabled(true);
    tplLayout->addWidget(m_btnCreateTpl);
    tplLayout->addWidget(m_btnLoadTpl);
    ctrlLayout->addWidget(tplGroup);

    // Measure group
    auto *measGroup = new QGroupBox("测量");
    auto *measLayout = new QVBoxLayout(measGroup);
    m_measureType = new QComboBox;
    m_measureType->addItems({"长度", "圆直径", "距离", "角度", "面积"});
    m_btnMeasure = new QPushButton("执行测量");
    m_btnMeasure->setCheckable(true);
    measLayout->addWidget(new QLabel("测量类型:"));
    measLayout->addWidget(m_measureType);
    measLayout->addWidget(m_btnMeasure);
    ctrlLayout->addWidget(measGroup);

    // Robot group
    auto *robotGroup = new QGroupBox("机器人通信");
    auto *robotLayout = new QVBoxLayout(robotGroup);
    m_btnConnect  = new QPushButton("连接机器人");
    m_btnSendPose = new QPushButton("发送抓取位姿");
    m_btnSendPose->setEnabled(false);
    robotLayout->addWidget(m_btnConnect);
    robotLayout->addWidget(m_btnSendPose);
    ctrlLayout->addWidget(robotGroup);

    ctrlLayout->addStretch();

    connect(m_btnStartAcq, &QPushButton::clicked, this, &MainWindow::onStartAcquisition);
    connect(m_btnStopAcq,  &QPushButton::clicked, this, &MainWindow::onStopAcquisition);
    connect(m_btnCreateTpl, &QPushButton::clicked, this, &MainWindow::onCreateTemplate);
    connect(m_btnLoadTpl,   &QPushButton::clicked, this, &MainWindow::onLoadTemplate);
    connect(m_btnConnect,   &QPushButton::clicked, this, &MainWindow::onConnectRobot);
    connect(m_btnSendPose,  &QPushButton::clicked, this, &MainWindow::onSendGrabPose);
    connect(m_btnMeasure, &QPushButton::clicked, this, [this](bool checked) {
        if (checked) {
            int idx = m_measureType->currentIndex();
            onMeasure(static_cast<MeasureType>(idx));
        } else {
            cancelMeasurement();
        }
    });

    m_controlDock->setWidget(ctrlWidget);
    addDockWidget(Qt::LeftDockWidgetArea, m_controlDock);
}

void MainWindow::setupParamDock()
{
    m_paramDock = new QDockWidget("参数配置", this);
    auto *paramWidget = new QWidget;
    auto *paramLayout = new QFormLayout(paramWidget);

    m_scoreThresh = new QDoubleSpinBox;
    m_scoreThresh->setRange(0.5, 1.0);
    m_scoreThresh->setSingleStep(0.05);
    m_scoreThresh->setValue(0.7);

    m_maxTargets = new QSpinBox;
    m_maxTargets->setRange(1, 16);
    m_maxTargets->setValue(4);

    paramLayout->addRow("匹配得分阈值:", m_scoreThresh);
    paramLayout->addRow("最大目标数:", m_maxTargets);

    auto *calibBtn = new QPushButton("相机标定");
    auto *handEyeBtn = new QPushButton("手眼标定");
    paramLayout->addRow(calibBtn);
    paramLayout->addRow(handEyeBtn);

    connect(calibBtn,   &QPushButton::clicked, this, &MainWindow::onCalibrateCamera);
    connect(handEyeBtn, &QPushButton::clicked, this, &MainWindow::onCalibrateHandEye);

    m_paramDock->setWidget(paramWidget);
    addDockWidget(Qt::RightDockWidgetArea, m_paramDock);
}

void MainWindow::setupBottomDocks()
{
    m_logDock = new QDockWidget("日志 / 结果", this);
    m_logView = new QTextEdit;
    m_logView->setReadOnly(true);
    m_logView->document()->setMaximumBlockCount(5000);
    m_logDock->setWidget(m_logView);
    addDockWidget(Qt::BottomDockWidgetArea, m_logDock);

    m_historyDock = new QDockWidget("历史记录", this);
    auto *histWidget = new QWidget;
    auto *histLayout = new QVBoxLayout(histWidget);
    auto *btnQuery  = new QPushButton("查询最近100条");
    auto *btnExport = new QPushButton("导出CSV");
    histLayout->addWidget(btnQuery);
    histLayout->addWidget(btnExport);
    histLayout->addStretch();
    m_historyDock->setWidget(histWidget);
    tabifyDockWidget(m_logDock, m_historyDock);
    m_logDock->raise();

    connect(btnQuery,  &QPushButton::clicked, this, &MainWindow::onQueryHistory);
    connect(btnExport, &QPushButton::clicked, this, &MainWindow::onExportCSV);
}

void MainWindow::setupMenuBar()
{
    auto *fileMenu = menuBar()->addMenu("文件(&F)");

    auto *actLoadTpl = fileMenu->addAction("加载模板...");
    connect(actLoadTpl, &QAction::triggered, this, &MainWindow::onLoadTemplate);

    auto *actExportCSV = fileMenu->addAction("导出CSV...");
    connect(actExportCSV, &QAction::triggered, this, &MainWindow::onExportCSV);

    auto *actExportReport = fileMenu->addAction("导出报表...");
    connect(actExportReport, &QAction::triggered, this, &MainWindow::onExportReport);

    fileMenu->addSeparator();
    auto *actExit = fileMenu->addAction("退出(&X)");
    connect(actExit, &QAction::triggered, this, &QMainWindow::close);

    auto *calibMenu = menuBar()->addMenu("标定(&C)");
    auto *actIntrinsic = calibMenu->addAction("相机内参标定...");
    auto *actHandEye   = calibMenu->addAction("手眼标定...");
    connect(actIntrinsic, &QAction::triggered, this, &MainWindow::onCalibrateCamera);
    connect(actHandEye,   &QAction::triggered, this, &MainWindow::onCalibrateHandEye);

    calibMenu->addSeparator();
    auto *actTestCalib = calibMenu->addAction("测试标定模式 (无机器人时联调 T3)");
    actTestCalib->setCheckable(true);
    connect(actTestCalib, &QAction::toggled, this, [this](bool on) {
        m_coordTrans->setTestMode(on, 0.05);
        appendLog(on ? "已启用【测试标定模式】：用像素×0.05 的线性映射代替真实标定，"
                       "仅供模拟器联调，坐标非真实物理量。"
                     : "已关闭测试标定模式。");
    });

    auto *helpMenu = menuBar()->addMenu("帮助(&H)");

    // ── T4 稳定性测试（自动触发）────────────────────────────────
    auto *actT4 = helpMenu->addAction("T4 稳定性测试模式 (每30秒自动发送抓取)");
    actT4->setCheckable(true);
    m_t4Timer = new QTimer(this);
    m_t4Timer->setInterval(30000);  // 30 s
    connect(m_t4Timer, &QTimer::timeout, this, &MainWindow::onSendGrabPose);
    connect(actT4, &QAction::toggled, this, [this](bool on) {
        if (on) {
            m_t4Timer->start();
            appendLog("【T4 稳定性测试】已启动：每 30 秒自动触发一次抓取，请保持"
                      "相机采集+模板加载+机器人已连接。");
        } else {
            m_t4Timer->stop();
            appendLog("【T4 稳定性测试】已停止。");
        }
    });
    auto *actAbout = helpMenu->addAction("关于...");
    connect(actAbout, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, "关于",
            "工业机器人视觉引导系统 v1.2\n\n"
            "技术栈: Qt 6.11 + HALCON 24.11 + OpenCV 4.8\n"
            "编译: MSVC 2022 x64");
    });
}

void MainWindow::setupStatusBar()
{
    m_statusCamera    = new QLabel("相机: 未连接");
    m_statusRobot     = new QLabel("机器人: 未连接");
    m_statusLicense   = new QLabel("License: 检查中...");
    m_statusFps       = new QLabel("FPS: --");
    m_statusFps->setMinimumWidth(80);
    m_statusDetection = new QLabel("检测: --");
    m_statusDetection->setMinimumWidth(300);
    m_statusMeasure   = new QLabel("测量: 空闲");
    m_statusMeasure->setMinimumWidth(220);

    statusBar()->addPermanentWidget(m_statusDetection);
    statusBar()->addPermanentWidget(m_statusMeasure);
    statusBar()->addPermanentWidget(m_statusCamera);
    statusBar()->addPermanentWidget(m_statusRobot);
    statusBar()->addPermanentWidget(m_statusLicense);
    statusBar()->addPermanentWidget(m_statusFps);
}

void MainWindow::setupConnections()
{
    // Camera → ImageView
    connect(m_cameraMgr, &CameraManager::frameReady, this, &MainWindow::onFrameReady);
    connect(m_cameraMgr, &CameraManager::fpsUpdated, this, &MainWindow::onFpsUpdated);
    connect(m_cameraMgr, &CameraManager::acquisitionError, this, [this](const QString &e) {
        appendLog("相机错误: " + e);
        m_statusCamera->setText("相机: 错误");
    });

    // Vision processor
    connect(m_visionProc, &VisionProcessor::matchingComplete, this, &MainWindow::onMatchResult);

    // Push match-parameter changes (score threshold, max targets) into the matcher.
    connect(m_scoreThresh, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double){ applyMatchConfig(); });
    connect(m_maxTargets, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int){ applyMatchConfig(); });

    // Combo change → update status bar "已选 <类型>" hint. Suppressed while a
    // measurement is active (button checked) so the user can't silently switch
    // type mid-flow without first cancelling.
    connect(m_measureType, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (m_btnMeasure && m_btnMeasure->isChecked()) return;
        const QString name = m_measureType->itemText(idx);
        m_statusMeasure->setText(QString("测量: 已选 %1（按\"执行测量\"开始）").arg(name));
    });

    // ImageView interactions for measurement
    connect(m_imageView, &ImageView::pointPicked, this, &MainWindow::onPointPicked);
    connect(m_imageView, &ImageView::roiSelected, this, [this](const QRectF &roi) {
        if (m_awaitingTemplateRoi) {
            m_awaitingTemplateRoi = false;
            createTemplateWithRoi(roi);
        } else if (m_pointsNeeded == 0) {
            onRoiSelectedForMeasure(roi);
        }
    });
    connect(m_visionProc, &VisionProcessor::templateCreated, this, [this](bool ok) {
        appendLog(ok ? "模板创建成功。" : "模板创建失败。");
    });

    // Robot client
    connect(m_robotClient, &RobotClient::stateChanged, this, [this](RobotState s) {
        m_statusRobot->setText("机器人: " + robotStateToString(s));
        if (s != m_lastRobotState) {
            // Log state transitions explicitly (otherwise the user can't tell
            // from the log when the TCP link is up — status frames only arrive
            // after the first heartbeat ACK and may be absent in some setups).
            appendLog("机器人连接: " + robotStateToString(s));
            m_lastRobotState = s;
        }
        if (s == RobotState::FAULT) onRobotFault();
    });
    connect(m_robotClient, &RobotClient::statusChanged, this, &MainWindow::onRobotStatusChanged);
    connect(m_robotClient, &RobotClient::connectionLost, this, [this]() {
        // Qt may fire QTcpSocket::disconnected and errorOccurred back-to-back,
        // both routed into connectionLost — suppress duplicates within 500 ms.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastConnLostMs < 500) return;
        m_lastConnLostMs = now;
        appendLog("机器人连接丢失，开始自动重连...");
    });

    // Calibration → CoordTransform auto-config
    connect(m_calibMgr, &CalibrationManager::intrinsicCompleted,
            this, [this](const CalibResult &r) {
        if (!r.success) return;
        m_coordTrans->setCameraParam(m_calibMgr->camParam());
        m_coordTrans->setCalibPose(m_calibMgr->lastCalibPose());

        CameraIntrinsic intrinsic;
        intrinsic.fx = r.focalLength / 5.5e-6;   // convert m → px
        intrinsic.fy = intrinsic.fx;
        intrinsic.cx = r.cx;
        intrinsic.cy = r.cy;
        intrinsic.valid = true;
        m_coordTrans->setIntrinsic(intrinsic);

        appendLog(QString("内参已应用 → CoordTransform (重投影误差 %1 px)")
                  .arg(r.reprojectionError, 0, 'f', 3));
    });

    connect(m_calibMgr, &CalibrationManager::handEyeCompleted,
            this, [this](const HandEyeResult &r) {
        if (!r.success) return;
        m_coordTrans->setHandEyePose(r.camInToolRX, r.camInToolRY, r.camInToolRZ,
                                      r.camInToolTX, r.camInToolTY, r.camInToolTZ);
        appendLog(QString("手眼矩阵已应用 → CoordTransform "
                           "(平移误差 %1 mm, 旋转误差 %2°)")
                  .arg(r.translationErrorMm, 0, 'f', 3)
                  .arg(r.rotationErrorDeg,   0, 'f', 3));
    });

    // Database
    connect(m_dbMgr, &DatabaseManager::databaseError, this, [this](const QString &e) {
        appendLog("数据库错误: " + e);
    });
}

// ── Settings ─────────────────────────────────────────────────────

void MainWindow::loadSettings()
{
    JsonSettings *s = m_settings;

    // Camera
    auto camIds = m_cameraMgr->enumerateCameras();
    m_cameraCombo->addItems(camIds);
    int lastCam = s->value("camera.last_index", 0).toInt();
    if (lastCam < m_cameraCombo->count())
        m_cameraCombo->setCurrentIndex(lastCam);

    // Match
    m_scoreThresh->setValue(s->value("halcon.match.min_score", 0.7).toDouble());
    m_maxTargets->setValue(s->value("halcon.match.max_targets", 16).toInt());

    // Robot
    QString ip   = s->value("robot.ip", "192.168.1.100").toString();
    quint16 port = s->value("robot.port", 5000).toUInt();
    m_robotClient->setConnectionParams(ip, port,
        s->value("robot.connection_timeout_ms", 3000).toInt());

    // Whitelist
    QStringList whitelist;
    for (const auto &v : s->value("robot.allowed_ip_whitelist").toList())
        whitelist << v.toString();
    if (!whitelist.isEmpty())
        m_robotClient->setWhitelist(whitelist);

    // Database
    QString dbPath = QApplication::applicationDirPath() + "/" +
                     s->value("database.path", "data/robot_vision.db").toString();
    QDir().mkpath(QFileInfo(dbPath).absolutePath());
    m_dbMgr->open(dbPath);

    // Pixel equivalent
    m_measureEng->setPixelEquivalent(
        s->value("calibration.pixel_equivalent_mm", 0.05).toDouble());

    appendLog(QString("已加载配置: %1 (%2 项)")
        .arg(s->filePath()).arg(s->allKeys().size()));
    appendLog(QString("匹配得分阈值: %1").arg(m_scoreThresh->value(), 0, 'f', 2));
}

void MainWindow::saveSettings()
{
    JsonSettings *s = m_settings;
    s->setValue("camera.last_index", m_cameraCombo->currentIndex());
    s->setValue("halcon.match.min_score", m_scoreThresh->value());
    s->setValue("halcon.match.max_targets", m_maxTargets->value());
    s->sync();
}

// ── License ──────────────────────────────────────────────────────

void MainWindow::checkHalconLicense()
{
    try {
        HalconCpp::HImage test;
        test.GenImageConst("byte", 16, 16);
        onLicenseStatusChanged(true);
    } catch (HalconCpp::HException &e) {
        const int     code = e.ErrorCode();
        const QString msg  = QString(e.ErrorMessage().Text());

        // Only a *genuine* license error should put the app into degraded mode
        // and disable the vision features. HALCON license errors live in the
        // 5200–5399 range (or carry "License" in the message). Anything else —
        // e.g. #1301 "wrong value of control parameter" from a binding/DLL
        // version quirk — must NOT disable the buttons, otherwise a self-test
        // hiccup permanently bricks the "创建模板" feature.
        const bool licenseProblem =
            (code >= 5200 && code <= 5399) ||
            msg.contains("License", Qt::CaseInsensitive);

        if (licenseProblem) {
            appendLog(QString("HALCON License 失败: ") + msg);
            onLicenseStatusChanged(false);
        } else {
            appendLog(QString("HALCON 自检异常（非 License 问题，功能保持可用）: ") + msg);
            onLicenseStatusChanged(true);
        }
    } catch (...) {
        appendLog("HALCON 自检发生未知异常（功能保持可用）。");
        onLicenseStatusChanged(true);
    }
}

void MainWindow::applyMatchConfig()
{
    // Push the UI match parameters into the matcher. Without this, the score
    // threshold / max-targets spin boxes had no effect (setMatchConfig was
    // never called), so the matcher always used the struct defaults.
    // setMatchConfig only touches m_config under its mutex (no HALCON handle),
    // so calling it directly from the GUI thread is safe.
    MatchConfig cfg;   // struct defaults keep full 360° rotation & scale range
    cfg.minScore   = m_scoreThresh->value();
    cfg.maxTargets = m_maxTargets->value();
    m_visionProc->setMatchConfig(cfg);
}

void MainWindow::onLicenseStatusChanged(bool available)
{
    m_licenseAvailable = available;
    m_degradedMode     = !available;

    if (m_degradedMode) {
        m_statusLicense->setText("License: 不可用 (降级模式)");
        m_statusLicense->setStyleSheet("color: red; font-weight: bold;");

        // Disable HALCON-dependent features
        m_btnCreateTpl->setEnabled(false);
        QMetaObject::invokeMethod(m_visionProc, "clearTemplate",
                                  Qt::BlockingQueuedConnection);

        appendLog("警告: HALCON License 不可用，视觉算法功能已禁用。");
        appendLog("  相机采集、通信、数据管理仍可正常使用。");

        QMessageBox::warning(this, "License 不可用",
            "HALCON License 验证失败。\n\n"
            "系统将以降级模式运行：\n"
            "- 相机采集、图像显示、TCP通信、数据管理 正常可用\n"
            "- 模板匹配、标定、视觉测量功能 已禁用\n\n"
            "请检查 License 文件或 License Server 连接后重启程序。");
    } else {
        m_statusLicense->setText("License: OK");
        m_statusLicense->setStyleSheet("color: green;");
        m_btnCreateTpl->setEnabled(true);
        appendLog("HALCON License 验证成功，全部功能可用。");
    }
}

void MainWindow::updateLicenseIndicator(bool available)
{
    onLicenseStatusChanged(available);
}

// ── Camera slots ─────────────────────────────────────────────────

void MainWindow::onCameraSelected(int index)
{
    Q_UNUSED(index)
}

void MainWindow::onStartAcquisition()
{
    QString id = m_cameraCombo->currentText();
    if (id.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先选择相机。");
        return;
    }
    if (!m_cameraMgr->open(id)) {
        QMessageBox::critical(this, "错误", "无法打开相机: " + id);
        return;
    }
    m_cameraMgr->startAcquisition();
    m_btnStartAcq->setEnabled(false);
    m_btnStopAcq->setEnabled(true);
    m_statusCamera->setText("相机: 采集中");
    appendLog("相机已启动: " + id);
}

void MainWindow::onStopAcquisition()
{
    m_cameraMgr->stopAcquisition();
    m_btnStartAcq->setEnabled(true);
    m_btnStopAcq->setEnabled(false);
    m_statusCamera->setText("相机: 已停止");
    appendLog("相机已停止。");
}

void MainWindow::onFrameReady(const QImage &image)
{
    m_imageView->setImage(image);

    // Run template matching if template loaded (async via vision thread)
    if (m_visionProc->isTemplateLoaded() && !m_degradedMode) {
        QMetaObject::invokeMethod(m_visionProc, "processImage",
                                  Qt::QueuedConnection,
                                  Q_ARG(QImage, image));
    }
}

void MainWindow::onFpsUpdated(double fps)
{
    m_statusFps->setText(QString("FPS: %1").arg(fps, 0, 'f', 1));
}

// ── Template matching ────────────────────────────────────────────

void MainWindow::onCreateTemplate()
{
    if (m_degradedMode) {
        QMessageBox::warning(this, "功能不可用", "License 不可用，模板创建已禁用。");
        return;
    }
    if (m_imageView->currentImage().isNull()) {
        QMessageBox::warning(this, "提示",
            "请先采集图像（建议采集后停止，停在清晰的一帧再建模）。");
        return;
    }

    // Second click cancels a pending selection.
    if (m_awaitingTemplateRoi) {
        m_awaitingTemplateRoi = false;
        m_btnCreateTpl->setText("创建模板");
        appendLog("已取消框选建模。");
        return;
    }

    // Enter ROI-selection mode: the next rubber-band drag becomes the template.
    m_awaitingTemplateRoi = true;
    m_btnCreateTpl->setText("框选中…(再次点击取消)");
    appendLog("请在图像上按住左键拖动，框选要建模的目标区域（如瓶身），松开即建模。");
}

void MainWindow::createTemplateWithRoi(const QRectF &roi)
{
    m_btnCreateTpl->setText("创建模板");

    QImage img = m_imageView->currentImage();
    if (img.isNull()) {
        QMessageBox::warning(this, "提示", "当前没有图像。");
        return;
    }

    // Run on the vision thread (where m_visionProc lives). BlockingQueuedConnection
    // serialises this behind any in-flight match and avoids touching the HALCON
    // model from the GUI thread, while still returning the result synchronously.
    bool ok = false;
    QMetaObject::invokeMethod(m_visionProc, "createTemplate",
                              Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(bool, ok),
                              Q_ARG(QImage, img),
                              Q_ARG(QRectF, roi));
    if (!ok) {
        appendLog("模板创建失败：该区域边缘可能太少，换一个清晰、对比强的区域再试。");
        return;
    }

    // Save template
    QString path = QFileDialog::getSaveFileName(this, "保存模板",
        QApplication::applicationDirPath() + "/resources/templates/",
        "HALCON Shape Model (*.shm)");
    if (!path.isEmpty()) {
        bool saved = false;
        QMetaObject::invokeMethod(m_visionProc, "saveTemplate",
                                  Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(bool, saved),
                                  Q_ARG(QString, path));
    }
}

void MainWindow::onLoadTemplate()
{
    QString path = QFileDialog::getOpenFileName(this, "加载模板",
        QApplication::applicationDirPath() + "/resources/templates/",
        "HALCON Shape Model (*.shm)");
    if (!path.isEmpty()) {
        bool loaded = false;
        QMetaObject::invokeMethod(m_visionProc, "loadTemplate",
                                  Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(bool, loaded),
                                  Q_ARG(QString, path));
        if (loaded) {
            appendLog("模板已加载: " + path);
        }
    }
}

void MainWindow::onMatchResult(const QList<MatchResult> &results)
{
    m_lastDetections = results;
    m_imageView->clearDetectionOverlays();

    int validCount = 0;
    const MatchResult *best = nullptr;

    for (const auto &r : results) {
        m_imageView->addDetectionOverlay(r.x, r.y, r.angle, r.score, r.valid);

        if (r.valid) {
            if (!best || r.score > best->score) best = &r;
            ++validCount;

            // Always save to DB so T2 data collection is unaffected.
            DetectionRecord rec;
            rec.timestamp = QDateTime::currentDateTime();
            rec.x = r.x;  rec.y = r.y;
            rec.angle = r.angle;  rec.score = r.score;
            rec.result = "PASS";
            Q_UNUSED(m_dbMgr->saveDetection(rec));
        }
    }

    // Cache the best valid detection so onSendGrabPose can use it even if the
    // click lands on a frame where matching transiently missed (matching is
    // often intermittent across frames; this prevents the "无目标" dialog when
    // a valid match happened only milliseconds earlier).
    if (best) {
        m_lastValidMatch    = *best;
        m_lastValidMatchMs  = QDateTime::currentMSecsSinceEpoch();
    }

    // ── Status bar: update every frame (real-time, no log spam) ──
    if (best) {
        m_statusDetection->setText(
            QString("检测: x=%1 y=%2 θ=%3° s=%4")
                .arg(best->x,     0, 'f', 1)
                .arg(best->y,     0, 'f', 1)
                .arg(best->angle, 0, 'f', 1)
                .arg(best->score, 0, 'f', 3));
        m_statusDetection->setStyleSheet("color: green;");
    } else {
        m_statusDetection->setText("检测: 无目标");
        m_statusDetection->setStyleSheet("color: gray;");
    }

    // ── Log: only on state transition, never per-frame ──────────
    if (validCount > 0 && m_lastValidCount <= 0) {
        // Transition: no match → match found. Log the first hit.
        appendLog(QString("检测到目标 ×%1  x=%2 y=%3 θ=%4° score=%5")
            .arg(validCount)
            .arg(best->x,     0, 'f', 2)
            .arg(best->y,     0, 'f', 2)
            .arg(best->angle, 0, 'f', 2)
            .arg(best->score, 0, 'f', 4));
    } else if (validCount == 0 && m_lastValidCount > 0) {
        // Transition: match → no match. Log once.
        appendLog(QString("目标丢失（阈值 %1）。")
            .arg(m_scoreThresh->value(), 0, 'f', 2));
    }
    m_lastValidCount = validCount;
}

// ── Calibration ──────────────────────────────────────────────────

void MainWindow::onCalibrateCamera()
{
    if (m_degradedMode) {
        QMessageBox::warning(this, "功能不可用", "License 不可用，标定功能已禁用。");
        return;
    }
    if (!m_cameraMgr->isAcquiring()) {
        QMessageBox::warning(this, "提示", "请先开启相机采集。");
        return;
    }

    auto *wiz = new CalibrationWizard(CalibrationWizard::INTRINSIC,
                                       m_cameraMgr, m_calibMgr, nullptr, this);
    connect(wiz, &CalibrationWizard::calibrationCompleted, this, [this](bool ok) {
        if (ok) {
            appendLog("相机内参标定完成。");
            m_coordTrans->setCameraParam(m_calibMgr->camParam());
        }
    });
    wiz->exec();
    wiz->deleteLater();
}

void MainWindow::onCalibrateHandEye()
{
    if (m_degradedMode) {
        QMessageBox::warning(this, "功能不可用", "License 不可用，标定功能已禁用。");
        return;
    }
    if (!m_calibMgr->isIntrinsicCalibrated()) {
        QMessageBox::warning(this, "提示", "请先完成相机内参标定。");
        return;
    }

    auto *wiz = new CalibrationWizard(CalibrationWizard::HAND_EYE,
                                       m_cameraMgr, m_calibMgr, m_robotClient, this);
    connect(wiz, &CalibrationWizard::calibrationCompleted, this, [this](bool ok) {
        if (ok) {
            appendLog("手眼标定完成。");
        }
    });
    wiz->exec();
    wiz->deleteLater();
}

// ── Measurement ──────────────────────────────────────────────────

void MainWindow::applyMeasurementOverlay(const MeasureResult &r)
{
    // Q-A policy: only the most recent measurement is shown.
    m_imageView->clearMeasurementOverlays();
    switch (r.type) {
        case MeasureType::LENGTH:
        case MeasureType::DISTANCE:
            m_imageView->addLineMeasurementOverlay(r.pt1, r.pt2, r.label);
            break;
        case MeasureType::CIRCLE:
            m_imageView->addCircleMeasurementOverlay(r.center, r.radius, r.label);
            break;
        case MeasureType::AREA:
            m_imageView->addMeasurementOverlay(r.center.x(), r.center.y(), r.label);
            break;
        case MeasureType::ANGLE:
            // Scheme B: render the user's 4 raw clicked points as two line segments.
            if (m_collectedPoints.size() >= 4) {
                m_imageView->addAngleOverlay(
                    m_collectedPoints[0], m_collectedPoints[1],
                    m_collectedPoints[2], m_collectedPoints[3],
                    r.label);
            }
            break;
    }
}

void MainWindow::onMeasure(MeasureType type)
{
    if (m_degradedMode) {
        QMessageBox::warning(this, "功能不可用", "License 不可用，测量功能已禁用。");
        resetMeasurementState();
        return;
    }
    QImage img = m_imageView->currentImage();
    if (img.isNull()) {
        resetMeasurementState();
        return;
    }

    m_collectedPoints.clear();
    m_pendingMeasureType = type;

    const QString typeName = m_measureType->currentText();
    switch (type) {
        case MeasureType::LENGTH:
        case MeasureType::CIRCLE:
        case MeasureType::AREA:
            m_imageView->setInteractionMode(InteractionMode::ROI);
            m_pointsNeeded = 0;
            appendLog("请在图像上拖拽选择测量区域...");
            m_statusMeasure->setText(QString("测量中: %1（请在图像上拖拽 ROI）").arg(typeName));
            break;
        case MeasureType::DISTANCE:
            m_imageView->setInteractionMode(InteractionMode::POINT_PICK);
            m_pointsNeeded = 2;
            appendLog("请在图像上依次点击两个点...");
            m_statusMeasure->setText(QString("测量中: %1（请点击 2 个点）").arg(typeName));
            break;
        case MeasureType::ANGLE:
            m_imageView->setInteractionMode(InteractionMode::POINT_PICK);
            m_pointsNeeded = 4;
            appendLog("请在图像上依次点击四个点（两条直线）...");
            m_statusMeasure->setText(QString("测量中: %1（请点击 4 个点）").arg(typeName));
            break;
    }
}

void MainWindow::onPointPicked(const QPointF &p)
{
    m_collectedPoints.append(p);

    // Visual marker + log: classify by measurement type and index so the user
    // can see which line / which endpoint each click belongs to (ANGLE uses
    // color to distinguish line 1 vs line 2).
    const int idx = m_collectedPoints.size();   // 1-based for the just-appended point
    QColor   markerColor(0, 200, 255);          // default cyan
    QString  markerLabel;
    QString  logLabel;
    switch (m_pendingMeasureType) {
        case MeasureType::DISTANCE:
            markerLabel = QString("点%1").arg(idx);
            logLabel    = QString("点 %1").arg(idx);
            break;
        case MeasureType::ANGLE: {
            const int lineNo = (idx <= 2) ? 1 : 2;
            const int ptNo   = ((idx - 1) % 2) + 1;
            markerColor      = (lineNo == 1) ? QColor(0, 200, 255)
                                              : QColor(255, 100, 200);
            markerLabel      = QString("线%1点%2").arg(lineNo).arg(ptNo);
            logLabel         = QString("线 %1 端点 %2").arg(lineNo).arg(ptNo);
            break;
        }
        default:
            // LENGTH / CIRCLE / AREA don't enter point-pick mode; defensive fallthrough.
            markerLabel = QString::number(idx);
            logLabel    = QString("点 %1").arg(idx);
            break;
    }
    m_imageView->addClickedPointMarker(p, markerColor, markerLabel);
    appendLog(QString("%1: (%2, %3)")
        .arg(logLabel)
        .arg(p.x(), 0, 'f', 1).arg(p.y(), 0, 'f', 1));

    if (m_collectedPoints.size() >= m_pointsNeeded) {
        // For ANGLE: reject lines whose two endpoints are within 10 px of each
        // other. Such a degenerate "line" cannot define a fitting direction —
        // measureAngle would either throw inside its ROI window or produce a
        // meaningless angle.
        if (m_pendingMeasureType == MeasureType::ANGLE) {
            const QPointF &a = m_collectedPoints[0];
            const QPointF &b = m_collectedPoints[1];
            const QPointF &c = m_collectedPoints[2];
            const QPointF &d = m_collectedPoints[3];
            const double d1 = std::hypot(b.x() - a.x(), b.y() - a.y());
            const double d2 = std::hypot(d.x() - c.x(), d.y() - c.y());
            if (d1 < 10.0 || d2 < 10.0) {
                QMessageBox::warning(this, "提示",
                    "每条线的两个端点距离过近（< 10 像素），无法定义有效线段。"
                    "请重新点选。");
                m_imageView->clearClickedPointMarkers();
                m_collectedPoints.clear();
                m_imageView->setInteractionMode(InteractionMode::ROI);
                resetMeasurementState();
                return;
            }
        }

        // Clear transient input markers before drawing the final measurement
        // overlay so the user sees only the result (line segments / cross).
        m_imageView->clearClickedPointMarkers();

        QImage img = m_imageView->currentImage();
        MeasureResult r = m_measureEng->measure(img, m_pendingMeasureType,
                                                 m_collectedPoints, QRectF());
        if (r.valid) {
            appendLog(QString("测量结果: %1 = %2 %3")
                .arg(r.label).arg(r.value, 0, 'f', 4).arg(r.unit));
            applyMeasurementOverlay(r);
        } else {
            appendLog("测量失败: " + r.label);
        }
        m_imageView->setInteractionMode(InteractionMode::ROI);
        m_collectedPoints.clear();
        resetMeasurementState();
    }
}

void MainWindow::onRoiSelectedForMeasure(const QRectF &roi)
{
    if (roi.isNull() || !roi.isValid() ||
        roi.width() < 5 || roi.height() < 5) {
        QMessageBox::warning(this, "提示",
            "请先在图像上拖拽选择测量区域（最小 5×5 像素）。");
        resetMeasurementState();
        return;
    }

    QImage img = m_imageView->currentImage();
    if (img.isNull()) {
        resetMeasurementState();
        return;
    }

    MeasureResult r = m_measureEng->measure(img, m_pendingMeasureType, {}, roi);
    if (r.valid) {
        appendLog(QString("测量结果: %1 = %2 %3")
            .arg(r.label).arg(r.value, 0, 'f', 4).arg(r.unit));
        applyMeasurementOverlay(r);
    } else {
        appendLog("测量失败: " + r.label);
        QMessageBox::warning(this, "测量失败", r.label);
    }
    m_imageView->setInteractionMode(InteractionMode::ROI);
    resetMeasurementState();
}

void MainWindow::cancelMeasurement()
{
    m_imageView->clearClickedPointMarkers();
    m_collectedPoints.clear();
    m_imageView->setInteractionMode(InteractionMode::ROI);
    appendLog("测量已取消。");
    resetMeasurementState();
}

void MainWindow::resetMeasurementState()
{
    if (m_btnMeasure) m_btnMeasure->setChecked(false);
    if (m_statusMeasure) m_statusMeasure->setText("测量: 空闲");
}

// ── Robot communication ──────────────────────────────────────────

void MainWindow::onConnectRobot()
{
    if (m_robotClient->state() == RobotState::CONNECTED) {
        m_robotClient->disconnect();
        m_btnConnect->setText("连接机器人");
        m_lastRobotStatus = RobotStatus::STATUS_UNKNOWN;
        m_lastRobotState  = RobotState::DISCONNECTED;
        return;
    }
    if (m_robotClient->connectToRobot()) {
        m_btnConnect->setText("断开连接");
        m_btnSendPose->setEnabled(true);
        appendLog("正在连接机器人...");
    } else {
        QMessageBox::warning(this, "连接失败", "无法连接机器人，请检查网络和配置。");
    }
}

void MainWindow::onSendGrabPose()
{
    if (m_degradedMode) return;

    if (!m_coordTrans->isFullyCalibrated()) {
        QMessageBox::warning(this, "标定未完成",
            "请先完成：\n  1. 相机内参标定\n  2. 手眼标定");
        return;
    }

    // Select best valid detection result
    const MatchResult *best = nullptr;
    for (const auto &m : m_lastDetections) {
        if (m.valid && (!best || m.score > best->score)) best = &m;
    }

    // Fall back to the cached last-valid detection if the current frame has
    // none. Matching is often intermittent (esp. with scaled-shape-model on a
    // moving handheld part), so without this the click had to land exactly on
    // a matching frame — almost impossible in practice.
    MatchResult fallback;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!best && m_lastValidMatchMs > 0 && nowMs - m_lastValidMatchMs <= 1500) {
        fallback = m_lastValidMatch;
        best = &fallback;
        appendLog(QString("当前帧无匹配，使用 %1 ms 前缓存的检测结果。")
                      .arg(nowMs - m_lastValidMatchMs));
    }

    if (!best) {
        QMessageBox::warning(this, "无目标", "当前无有效检测结果，请先采集并匹配。");
        return;
    }

    RobotToolPose currentPose = m_robotClient->queryToolPose();
    m_coordTrans->setCurrentToolPose(currentPose);

    WorldCoordinate wc = m_coordTrans->pixelToWorld(best->x, best->y, best->angle);
    if (!wc.valid) {
        appendLog("错误: 坐标变换失败，请检查标定状态。");
        return;
    }
    if (!m_coordTrans->isInWorkspace(wc.x_mm, wc.y_mm)) {
        appendLog("警告: 目标坐标超出机器人工作空间，发送已取消。");
        return;
    }

    bool ok = m_robotClient->sendGrabPose(wc.x_mm, wc.y_mm, wc.angle);
    if (ok) {
        appendLog(QString("已发送抓取位姿: X=%1 Y=%2 θ=%3°")
            .arg(wc.x_mm, 0, 'f', 2)
            .arg(wc.y_mm, 0, 'f', 2)
            .arg(wc.angle, 0, 'f', 2));

        // Log grab record
        GrabRecord gr;
        gr.robotX = wc.x_mm;
        gr.robotY = wc.y_mm;
        gr.robotAngle = wc.angle;
        gr.status = "SENT";
        m_dbMgr->saveGrabRecord(gr);
    }
}

void MainWindow::onRobotStatusChanged(RobotStatus status)
{
    // Only log when the status actually changes — the robot ACKs every heartbeat
    // (~1/s) with READY, which would otherwise flood the log with "机器人状态: 就绪".
    if (status != m_lastRobotStatus) {
        appendLog("机器人状态: " + robotStatusToString(status));
        m_lastRobotStatus = status;
    }
    m_btnSendPose->setEnabled(status == RobotStatus::READY);
}

void MainWindow::onRobotFault()
{
    appendLog("错误: 机器人通信故障（FAULT），请检查网络连接后手动重连。");
    QMessageBox::critical(this, "机器人通信故障",
        "重连 5 次均失败，系统已进入 FAULT 状态。\n\n"
        "请检查：\n"
        "1. 机器人控制器是否正常运行\n"
        "2. 网络连接是否正常\n"
        "3. IP 地址和端口配置是否正确\n\n"
        "检查完毕后请点击「连接机器人」重新连接。");
}

// ── Data / History ───────────────────────────────────────────────

void MainWindow::onQueryHistory()
{
    auto records = m_dbMgr->queryDetections(
        QDateTime::currentDateTime().addDays(-7),
        QDateTime::currentDateTime(), 100);

    appendLog(QString("查询到 %1 条历史记录。").arg(records.size()));
    for (const auto &r : records) {
        appendLog(QString("  [%1] x=%2 y=%3 score=%4 %5")
            .arg(r.timestamp.toString("yyyy-MM-dd hh:mm:ss"))
            .arg(r.x, 0, 'f', 2)
            .arg(r.y, 0, 'f', 2)
            .arg(r.score, 0, 'f', 4)
            .arg(r.result));
    }
}

void MainWindow::doExport(const QString &format)
{
    auto records = m_dbMgr->queryDetections(
        QDateTime::currentDateTime().addDays(-30),
        QDateTime::currentDateTime(), 10000);

    QString ext = "." + format;
    QString path = m_reportMgr->generateTimestampedFilename(ext);

    bool ok = false;
    if (format == "csv")  ok = m_reportMgr->exportCSV(records, path);
    if (format == "pdf")  ok = m_reportMgr->exportPDF(records, "检测汇总报表", path);
    if (format == "xlsx") ok = m_reportMgr->exportExcel(records, path);

    appendLog(ok ? (format.toUpper() + " 已导出: " + path)
                 : (format.toUpper() + " 导出失败"));
}

void MainWindow::onExportReport()
{
    doExport("pdf");
}

void MainWindow::onExportCSV()
{
    doExport("csv");
}

// ── Log helper ───────────────────────────────────────────────────

void MainWindow::appendLog(const QString &msg)
{
    QString ts = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    m_logView->append("[" + ts + "] " + msg);
    Logger::info(msg);
}
