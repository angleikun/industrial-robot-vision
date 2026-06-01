#ifndef COORDTRANSFORM_H
#define COORDTRANSFORM_H

#include <QObject>
#include <QPointF>

#include "HalconCpp.h"

struct CameraIntrinsic {
    double fx = 0.0, fy = 0.0;
    double cx = 0.0, cy = 0.0;
    double k1 = 0.0, k2 = 0.0, k3 = 0.0;
    double p1 = 0.0, p2 = 0.0;
    bool   valid = false;
};

struct WorldCoordinate {
    double x_mm    = 0.0;   // X in robot base frame
    double y_mm    = 0.0;   // Y in robot base frame
    double z_mm    = 0.0;   // Z (work plane height)
    double angle   = 0.0;   // rotation angle (degrees)
    bool   valid   = false;
};

struct RobotToolPose {
    double rx = 0.0, ry = 0.0, rz = 0.0;  // degrees
    double tx = 0.0, ty = 0.0, tz = 0.0;  // mm
};

class CoordTransform : public QObject {
    Q_OBJECT

public:
    explicit CoordTransform(QObject *parent = nullptr);

    void setIntrinsic(const CameraIntrinsic &K);
    CameraIntrinsic intrinsic() const;

    // Set hand-eye from 6-DOF pose (rx, ry, rz in degrees; tx, ty, tz in mm)
    void setHandEyePose(double rx, double ry, double rz,
                        double tx, double ty, double tz);

    // Set HALCON camera param and calibration pose directly
    void setCameraParam(const HalconCpp::HTuple &camParam);
    void setCalibPose(const HalconCpp::HTuple &calibPose);

    void setWorkPlaneHeight(double z_mm);
    void setWorkspaceBounds(double xMin, double xMax, double yMin, double yMax);

    // 测试标定模式：无真实相机标定/机器人时，用 像素×比例 的线性映射代替
    // HALCON 世界平面投影，让 isFullyCalibrated() 通过、pixelToWorld 返回有效
    // 坐标，仅用于打通 检测→变换→发送→机器人模拟器 全链路（如 T3 测试）。
    void setTestMode(bool on, double mmPerPx = 0.05);
    bool testMode() const { return m_testMode; }

    // Full transform chain: pixel → camera → tool → robot base
    WorldCoordinate pixelToWorld(double px, double py, double angle,
                                 const RobotToolPose &base_H_tool);
    WorldCoordinate pixelToWorld(double px, double py, double angle);

    // Undistort pixel → normalized camera coordinates
    QPointF undistortPixel(double px, double py) const;

    // Validate if world coordinate is within workspace
    bool isInWorkspace(double x, double y) const;

    // Set current robot pose for active transforms
    void setCurrentToolPose(const RobotToolPose &pose);
    RobotToolPose currentToolPose() const;

    bool isReady() const;
    bool isFullyCalibrated() const;

signals:
    void transformError(const QString &error);
    void outOfWorkspace(double x, double y);

private:
    CameraIntrinsic m_intrinsic;
    HalconCpp::HHomMat3D m_camHomTool;   // cam_H_tool: 4x4 homogeneous matrix
    HalconCpp::HTuple    m_camParam;     // camera internal parameters (from calibration)
    HalconCpp::HTuple    m_calibPose;    // calibration plate pose (for world plane projection)

    double m_workPlaneZ = 0.0;
    double m_wsXMin = -500.0, m_wsXMax = 500.0;
    double m_wsYMin = -500.0, m_wsYMax = 500.0;
    RobotToolPose m_currentToolPose;

    bool m_hasCamParam        = false;
    bool m_handEyeConfigured  = false;
    bool   m_testMode  = false;
    double m_testScale = 0.05;
};

#endif // COORDTRANSFORM_H
