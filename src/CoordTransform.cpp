#include "CoordTransform.h"
#include <QtMath>
#include <QDebug>

CoordTransform::CoordTransform(QObject *parent)
    : QObject(parent)
{
    m_camHomTool.HomMat3dIdentity();
}

// ── Setters ──────────────────────────────────────────────────────

void CoordTransform::setIntrinsic(const CameraIntrinsic &K)
{
    m_intrinsic = K;
}

CameraIntrinsic CoordTransform::intrinsic() const
{
    return m_intrinsic;
}

void CoordTransform::setHandEyePose(double rx, double ry, double rz,
                                     double tx, double ty, double tz)
{
    HalconCpp::HPose pose;
    pose.CreatePose(tx, ty, tz, rx, ry, rz, "Rp+T", "gba", "point");
    m_camHomTool = pose.PoseToHomMat3d();
    m_handEyeConfigured = true;
}

bool CoordTransform::isFullyCalibrated() const
{
    if (m_testMode) return true;
    return m_hasCamParam && m_calibPose.Length() > 0 && m_handEyeConfigured;
}

void CoordTransform::setTestMode(bool on, double mmPerPx)
{
    m_testMode  = on;
    m_testScale = mmPerPx;
}

void CoordTransform::setCameraParam(const HalconCpp::HTuple &camParam)
{
    m_camParam    = camParam;
    m_hasCamParam = true;
}

void CoordTransform::setCalibPose(const HalconCpp::HTuple &calibPose)
{
    m_calibPose = calibPose;
}

void CoordTransform::setWorkPlaneHeight(double z_mm)
{
    m_workPlaneZ = z_mm;
}

void CoordTransform::setWorkspaceBounds(double xMin, double xMax,
                                         double yMin, double yMax)
{
    m_wsXMin = xMin;
    m_wsXMax = xMax;
    m_wsYMin = yMin;
    m_wsYMax = yMax;
}

// ── Full transform chain ─────────────────────────────────────────

WorldCoordinate CoordTransform::pixelToWorld(double px, double py, double angle,
                                               const RobotToolPose &base_H_tool)
{
    WorldCoordinate wc;

    if (m_testMode) {
        // 测试标定模式：线性映射代替 HALCON 世界平面投影（仅联调用）。
        wc.x_mm  = px * m_testScale;
        wc.y_mm  = py * m_testScale;
        wc.z_mm  = m_workPlaneZ;
        wc.angle = angle;
        wc.valid = true;
        return wc;
    }

    if (!m_hasCamParam) {
        wc.valid = false;
        emit transformError("相机内参未标定，请先完成相机标定");
        return wc;
    }
    if (m_calibPose.Length() == 0) {
        wc.valid = false;
        emit transformError("标定 pose 缺失，请重新执行相机标定");
        return wc;
    }
    if (!m_handEyeConfigured) {
        wc.valid = false;
        emit transformError("手眼矩阵未配置，请先完成手眼标定");
        return wc;
    }

    try {
        // Step 1: Pixel → Camera world plane coordinates
        // ImagePointsToWorldPlane uses (Row=y, Col=x) — note swapped order
        HalconCpp::HTuple hv_Rows(py);
        HalconCpp::HTuple hv_Cols(px);
        HalconCpp::HTuple hv_X_cam, hv_Y_cam;

        HalconCpp::ImagePointsToWorldPlane(m_camParam, m_calibPose,
                                            hv_Rows, hv_Cols, "mm",
                                            &hv_X_cam, &hv_Y_cam);
        double X_cam = hv_X_cam[0].D();
        double Y_cam = hv_Y_cam[0].D();

        // Step 2: Camera → Tool via cam_H_tool
        HalconCpp::HTuple hv_X_cam_tup(X_cam);
        HalconCpp::HTuple hv_Y_cam_tup(Y_cam);
        HalconCpp::HTuple hv_Z_zero(0.0);   // 像素已投影到 Z=0 平面
        HalconCpp::HTuple hv_X_tool, hv_Y_tool, hv_Z_tool;

        hv_X_tool = m_camHomTool.AffineTransPoint3d(
                        hv_X_cam_tup, hv_Y_cam_tup, hv_Z_zero,
                        &hv_Y_tool, &hv_Z_tool);
        double X_tool = hv_X_tool[0].D();
        double Y_tool = hv_Y_tool[0].D();
        double Z_tool = hv_Z_tool[0].D();

        // Step 3: Tool → Robot Base via base_H_tool
        HalconCpp::HPose basePose;
        basePose.CreatePose(base_H_tool.tx, base_H_tool.ty, base_H_tool.tz,
                            base_H_tool.rx, base_H_tool.ry, base_H_tool.rz,
                            "Rp+T", "gba", "point");
        HalconCpp::HHomMat3D baseHomTool = basePose.PoseToHomMat3d();

        HalconCpp::HTuple hv_X_tool_tup(X_tool);
        HalconCpp::HTuple hv_Y_tool_tup(Y_tool);
        HalconCpp::HTuple hv_Z_tool_tup(Z_tool);
        HalconCpp::HTuple hv_X_base, hv_Y_base, hv_Z_base;

        hv_X_base = baseHomTool.AffineTransPoint3d(
                        hv_X_tool_tup, hv_Y_tool_tup, hv_Z_tool_tup,
                        &hv_Y_base, &hv_Z_base);

        double X_base = hv_X_base[0].D();
        double Y_base = hv_Y_base[0].D();
        double Z_base = hv_Z_base[0].D();

        // Angle transform (Z rotation only, simplified)
        double worldAngle = angle + base_H_tool.rz;
        while (worldAngle > 180.0)  worldAngle -= 360.0;
        while (worldAngle < -180.0) worldAngle += 360.0;

        wc.x_mm  = X_base;
        wc.y_mm  = Y_base;
        wc.z_mm  = Z_base;
        wc.angle = worldAngle;
        wc.valid = true;

        if (!isInWorkspace(wc.x_mm, wc.y_mm)) {
            emit outOfWorkspace(wc.x_mm, wc.y_mm);
        }

    } catch (HalconCpp::HException &e) {
        wc.valid = false;
        QString msg = "坐标变换失败: " + QString(e.ErrorMessage().Text());
        qWarning() << msg;
        emit transformError(msg);
    }

    return wc;
}

WorldCoordinate CoordTransform::pixelToWorld(double px, double py, double angle)
{
    return pixelToWorld(px, py, angle, m_currentToolPose);
}

// ── Undistort ────────────────────────────────────────────────────

QPointF CoordTransform::undistortPixel(double px, double py) const
{
    if (!m_intrinsic.valid) return QPointF(px, py);

    double xn = (px - m_intrinsic.cx) / m_intrinsic.fx;
    double yn = (py - m_intrinsic.cy) / m_intrinsic.fy;
    return QPointF(xn, yn);
}

// ── Workspace check ──────────────────────────────────────────────

bool CoordTransform::isInWorkspace(double x, double y) const
{
    return x >= m_wsXMin && x <= m_wsXMax && y >= m_wsYMin && y <= m_wsYMax;
}

void CoordTransform::setCurrentToolPose(const RobotToolPose &pose)
{
    m_currentToolPose = pose;
}

RobotToolPose CoordTransform::currentToolPose() const
{
    return m_currentToolPose;
}

bool CoordTransform::isReady() const
{
    return m_intrinsic.valid && m_hasCamParam;
}
