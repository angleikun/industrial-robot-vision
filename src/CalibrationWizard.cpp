#include "CalibrationWizard.h"
#include "CameraManager.h"
#include "CalibrationManager.h"
#include "RobotClient.h"
#include "CoordTransform.h"

#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QThread>

CalibrationWizard::CalibrationWizard(Mode mode,
                                       CameraManager *cam,
                                       CalibrationManager *calib,
                                       RobotClient *robot,
                                       QWidget *parent)
    : QDialog(parent), m_mode(mode), m_cam(cam), m_calib(calib), m_robot(robot)
{
    setupUi();
    m_required = 15;

    if (m_cam) {
        connect(m_cam, &CameraManager::frameReady,
                this, &CalibrationWizard::onFrameReady);
    }
}

void CalibrationWizard::setupUi()
{
    QString modeName = (m_mode == INTRINSIC) ? "相机内参标定" : "手眼标定";
    setWindowTitle(modeName + "向导");
    resize(700, 500);
    setMinimumSize(500, 400);

    auto *mainLayout = new QHBoxLayout(this);

    // ── Left: preview ──────────────
    m_lblPreview = new QLabel;
    m_lblPreview->setMinimumSize(320, 240);
    m_lblPreview->setAlignment(Qt::AlignCenter);
    m_lblPreview->setStyleSheet("background-color: #1a1a1a; border: 1px solid #444;");
    m_lblPreview->setText("等待图像...");
    mainLayout->addWidget(m_lblPreview, 1);

    // ── Right: controls ────────────
    auto *rightLayout = new QVBoxLayout;

    m_lblInstruction = new QLabel;
    if (m_mode == INTRINSIC) {
        m_lblInstruction->setText(
            "1. 将标定板放在相机视野中\n"
            "2. 调整标定板到不同位置和角度\n"
            "3. 每次点击「采集当前帧」\n"
            "4. 采集 ≥ 15 组后点击「开始标定」");
    } else {
        m_lblInstruction->setText(
            "1. 将标定板固定在相机视野中\n"
            "2. 移动机器人到不同位姿\n"
            "3. 每次点击「采集当前帧」\n"
            "4. 采集 ≥ 15 组后点击「开始标定」");
    }
    m_lblInstruction->setWordWrap(true);
    rightLayout->addWidget(m_lblInstruction);

    rightLayout->addSpacing(10);

    m_btnCapture = new QPushButton("采集当前帧");
    m_btnCapture->setMinimumHeight(40);
    rightLayout->addWidget(m_btnCapture);

    m_progress = new QProgressBar;
    m_progress->setRange(0, m_required);
    m_progress->setValue(0);
    m_progress->setFormat("已采集 %v / %m");
    rightLayout->addWidget(m_progress);

    m_btnCalibrate = new QPushButton("开始标定");
    m_btnCalibrate->setMinimumHeight(50);
    m_btnCalibrate->setEnabled(false);
    m_btnCalibrate->setStyleSheet("QPushButton:enabled { background-color: #4CAF50; color: white; }");
    rightLayout->addWidget(m_btnCalibrate);

    rightLayout->addStretch();
    mainLayout->addLayout(rightLayout);

    // ── Connections ─────────────────
    connect(m_btnCapture,   &QPushButton::clicked, this, &CalibrationWizard::onCaptureClicked);
    connect(m_btnCalibrate, &QPushButton::clicked, this, &CalibrationWizard::onCalibrateClicked);
}

void CalibrationWizard::onFrameReady(const QImage &img)
{
    m_currentFrame = img;
    QPixmap pix = QPixmap::fromImage(img).scaled(
        m_lblPreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_lblPreview->setPixmap(pix);
}

void CalibrationWizard::onCaptureClicked()
{
    if (m_currentFrame.isNull()) {
        QMessageBox::warning(this, "无图像", "请等待相机画面出现后再采集。");
        return;
    }

    bool ok = false;
    if (m_mode == INTRINSIC) {
        ok = m_calib->addCalibrationImage(m_currentFrame);
    } else {
        // HAND_EYE: query current robot pose
        RobotPose rp;
        if (m_robot && m_robot->isConnected()) {
            RobotToolPose toolPose = m_robot->queryToolPose();
            rp.tx = toolPose.tx; rp.ty = toolPose.ty; rp.tz = toolPose.tz;
            rp.rx = toolPose.rx; rp.ry = toolPose.ry; rp.rz = toolPose.rz;
        }
        ok = m_calib->addCalibPose(rp, m_currentFrame);
    }

    if (ok) {
        m_capturedCount++;
        updateProgress();
    }
}

void CalibrationWizard::onCalibrateClicked()
{
    m_btnCalibrate->setEnabled(false);
    m_btnCalibrate->setText("标定中...");

    bool ok = false;
    QString resultText;

    if (m_mode == INTRINSIC) {
        CalibResult r = m_calib->calibrateCamera();
        ok = r.success;
        if (ok) {
            resultText = QString("内参标定完成！\n\n"
                               "重投影误差: %1 px\n"
                               "焦距: %2 mm\n"
                               "主点: (%3, %4) px")
                .arg(r.reprojectionError, 0, 'f', 4)
                .arg(r.focalLength * 1000.0, 0, 'f', 2)
                .arg(r.cx, 0, 'f', 1).arg(r.cy, 0, 'f', 1);
        } else {
            resultText = "标定失败: " + r.errorMessage;
        }
    } else {
        HandEyeResult r = m_calib->calibrateHandEye();
        ok = r.success;
        if (ok) {
            resultText = QString("手眼标定完成！\n\n"
                               "平移误差: %1 mm\n"
                               "旋转误差: %2°\n"
                               "cam_H_tool: T(%3, %4, %5) R(%6, %7, %8)")
                .arg(r.translationErrorMm, 0, 'f', 4)
                .arg(r.rotationErrorDeg, 0, 'f', 4)
                .arg(r.camInToolTX, 0, 'f', 3).arg(r.camInToolTY, 0, 'f', 3).arg(r.camInToolTZ, 0, 'f', 3)
                .arg(r.camInToolRX, 0, 'f', 3).arg(r.camInToolRY, 0, 'f', 3).arg(r.camInToolRZ, 0, 'f', 3);
        } else {
            resultText = "标定失败: " + r.errorMessage;
        }
    }

    m_btnCalibrate->setText("开始标定");
    m_btnCalibrate->setEnabled(true);

    if (ok) {
        QMessageBox::information(this, "标定完成", resultText);
        emit calibrationCompleted(true);
        accept();
    } else {
        QMessageBox::warning(this, "标定失败", resultText);
    }
}

void CalibrationWizard::updateProgress()
{
    m_progress->setValue(m_capturedCount);
    m_btnCalibrate->setEnabled(m_capturedCount >= m_required);
}
