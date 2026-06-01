#ifndef CALIBRATIONMANAGER_H
#define CALIBRATIONMANAGER_H

#include <QObject>
#include <QList>
#include <QString>
#include <QImage>
#include <QVector>

#include "HalconCpp.h"

struct RobotPose {
    double rx = 0.0, ry = 0.0, rz = 0.0;
    double tx = 0.0, ty = 0.0, tz = 0.0;
};

struct CalibResult {
    bool success = false;
    double reprojectionError = 0.0;
    double focalLength       = 0.0;
    double cx = 0.0, cy = 0.0;
    double distortionK1 = 0.0, distortionK2 = 0.0, distortionK3 = 0.0;
    double distortionP1 = 0.0, distortionP2 = 0.0;
    QString errorMessage;
};

struct HandEyeResult {
    bool success = false;
    double rotationErrorDeg   = 0.0;
    double translationErrorMm = 0.0;
    // cam_H_tool pose: rx, ry, rz (deg), tx, ty, tz (mm)
    double camInToolRX = 0.0, camInToolRY = 0.0, camInToolRZ = 0.0;
    double camInToolTX = 0.0, camInToolTY = 0.0, camInToolTZ = 0.0;
    QString errorMessage;
};

class CalibrationManager : public QObject {
    Q_OBJECT

public:
    explicit CalibrationManager(QObject *parent = nullptr);
    ~CalibrationManager() override;

    void setPatternParams(int rows, int cols, double squareSizeMm);
    void setMinPoses(int count);
    void setCaltabFile(const QString &descrFile);

    // Intrinsic calibration
    bool addCalibrationImage(const QImage &image);
    CalibResult calibrateCamera();
    QImage undistortImage(const QImage &image);
    bool saveIntrinsic(const QString &filePath);
    bool loadIntrinsic(const QString &filePath);
    bool isIntrinsicCalibrated() const;
    void clearCalibImages();

    // Eye-in-hand calibration (3D pose mode)
    bool addCalibPose(const RobotPose &toolInBase, const QImage &calibImage);
    HandEyeResult calibrateHandEye();
    bool saveHandEyeResult(const QString &filePath);
    bool loadHandEyeResult(const QString &filePath);
    bool isHandEyeCalibrated() const;
    int  poseCount() const;

    // Accessors
    HalconCpp::HTuple camParam() const;
    HalconCpp::HTuple handEyePose() const;

signals:
    void calibImageAdded(int count, int required);
    void intrinsicCompleted(const CalibResult &result);
    void handEyeCompleted(const HandEyeResult &result);
    void calibError(const QString &error);

private:
    HalconCpp::HImage qImageToHImage(const QImage &img);

    int    m_patternRows   = 7;
    int    m_patternCols   = 7;
    double m_squareSizeMm  = 6.0;
    int    m_minPoses      = 15;
    QString m_caltabFile   = "caltab_30mm.descr";

    // Intrinsic calibration state
    HalconCpp::HTuple m_calibDataID;
    HalconCpp::HTuple m_camParam;
    HalconCpp::HObject m_distortionMap;
    QList<HalconCpp::HImage> m_calibImages;
    bool m_intrinsicDone   = false;
    bool m_distMapComputed = false;

    // Hand-eye state
    HalconCpp::HTuple m_handEyeCamInToolPose;
    bool m_handEyeDone = false;
    // Hand-eye: robot poses + images + last calib pose
    QVector<RobotPose> m_robotPoses;
    QVector<HalconCpp::HImage> m_handEyeImages;
    HalconCpp::HTuple m_lastCalibPose;
    int m_handEyePoseCount = 0;

public:
    HalconCpp::HTuple lastCalibPose() const { return m_lastCalibPose; }
};

#endif // CALIBRATIONMANAGER_H
