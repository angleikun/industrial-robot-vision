#ifndef CALIBRATIONWIZARD_H
#define CALIBRATIONWIZARD_H

#include <QDialog>
#include <QImage>

class QLabel;
class QPushButton;
class QProgressBar;
class CameraManager;
class CalibrationManager;
class RobotClient;

class CalibrationWizard : public QDialog {
    Q_OBJECT

public:
    enum Mode { INTRINSIC, HAND_EYE };

    CalibrationWizard(Mode mode,
                      CameraManager *cam,
                      CalibrationManager *calib,
                      RobotClient *robot,   // HAND_EYE 模式需要
                      QWidget *parent = nullptr);

signals:
    void calibrationCompleted(bool success);

private slots:
    void onCaptureClicked();
    void onCalibrateClicked();
    void onFrameReady(const QImage &img);   //实时图像回调

private:
    void setupUi();
    void updateProgress();

    Mode m_mode;
    CameraManager      *m_cam;
    CalibrationManager *m_calib;
    RobotClient        *m_robot;

    QLabel       *m_lblPreview  = nullptr;
    QLabel       *m_lblInstruction = nullptr;
    QPushButton  *m_btnCapture  = nullptr;
    QPushButton  *m_btnCalibrate = nullptr;
    QProgressBar *m_progress    = nullptr;
    QImage        m_currentFrame;
    int           m_capturedCount = 0;
    int           m_required      = 15;
};

#endif // CALIBRATIONWIZARD_H
