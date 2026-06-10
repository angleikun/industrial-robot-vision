#include "CalibrationManager.h"
#include <QDebug>
#include <QtMath>
#include <QImage>
#include <QFileInfo>
#include <vector>
#include <cstring>

CalibrationManager::CalibrationManager(QObject *parent)
    : QObject(parent)
{
}

CalibrationManager::~CalibrationManager()
{
    // 析构函数边界：必须吞异常（C++ 析构函数抛异常会触发 std::terminate）。
    // 仅记录日志，不重抛。
    try {
        if (m_calibDataID.Length() > 0) {
            HalconCpp::ClearCalibData(m_calibDataID);
        }
        if (m_distMapComputed) {
            m_distortionMap.Clear();
        }
    } catch (const HalconCpp::HException &e) {
        qWarning() << "~CalibrationManager HALCON exception (swallowed):"
                   << e.ErrorMessage().Text();
    } catch (const std::exception &e) {
        qWarning() << "~CalibrationManager std exception (swallowed):" << e.what();
    } catch (...) {
        qWarning() << "~CalibrationManager unknown exception (swallowed)";
    }
}

// ── Helpers ──────────────────────────────────────────────────────

HalconCpp::HImage CalibrationManager::qImageToHImage(const QImage &img)
{
    QImage gray = img;
    if (gray.format() != QImage::Format_Grayscale8) {
        gray = gray.convertToFormat(QImage::Format_Grayscale8);
    }

    const int w = gray.width();
    const int h = gray.height();

    // QImage scanlines are 32-bit aligned; copy row-by-row into a contiguous
    // buffer so GenImage1 (stride == width) gets a correct image even when the
    // width is not a multiple of 4. GenImage1 copies the data.
    std::vector<uchar> buffer(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        std::memcpy(buffer.data() + static_cast<size_t>(y) * w,
                    gray.constScanLine(y),
                    static_cast<size_t>(w));
    }

    HalconCpp::HImage hImage;
    hImage.GenImage1("byte", w, h, buffer.data());
    return hImage;
}

void CalibrationManager::setPatternParams(int rows, int cols, double squareSizeMm)
{
    m_patternRows  = rows;
    m_patternCols  = cols;
    m_squareSizeMm = squareSizeMm;
}

void CalibrationManager::setMinPoses(int count)
{
    m_minPoses = count;
}

void CalibrationManager::setCaltabFile(const QString &descrFile)
{
    m_caltabFile = descrFile;
}

// ── Intrinsic calibration ────────────────────────────────────────

bool CalibrationManager::addCalibrationImage(const QImage &image)
{
    try {
        HalconCpp::HImage hImg = qImageToHImage(image);

        // Try to find calibration plate
        HalconCpp::HObject caltabRegion;
        HalconCpp::FindCaltab(hImg, &caltabRegion,
                               m_caltabFile.toStdWString().c_str(),
                               3, 112, 5);

        // Check if caltab was found (non-empty region)
        HalconCpp::HTuple hv_Area, hv_Row, hv_Col;
        HalconCpp::AreaCenter(caltabRegion, &hv_Area, &hv_Row, &hv_Col);
        if (hv_Area[0].D() < 100) {
            emit calibError("未检测到标定板，请确保标定板完整可见");
            return false;
        }

        m_calibImages.append(hImg);
        emit calibImageAdded(m_calibImages.size(), m_minPoses);
        return true;

    } catch (HalconCpp::HException &e) {
        QString msg = "FindCaltab 失败: " + QString(e.ErrorMessage().Text());
        emit calibError(msg);
        return false;
    }
}

CalibResult CalibrationManager::calibrateCamera()
{
    CalibResult result;

    if (m_calibImages.size() < m_minPoses) {
        result.errorMessage = QString("标定图像不足：需要至少 %1 张，当前 %2 张")
            .arg(m_minPoses).arg(m_calibImages.size());
        emit calibError(result.errorMessage);
        return result;
    }

    try {
        // Create calibration data model
        HalconCpp::CreateCalibData("calibration_object", 1, 1, &m_calibDataID);

        // area_scan_division model: 8 parameters
        // [0] Focus (m), [1] Kappa, [2] Sx (m), [3] Sy (m),
        // [4] Cx (px), [5] Cy (px), [6] ImageWidth, [7] ImageHeight
        double pixelSize = 5.5e-6;    // 5.5 μm typical 1/2.3" CMOS
        double focalLen  = 0.012;     // 12mm lens estimate

        HalconCpp::HTuple startCamParam;
        startCamParam.Append(focalLen);     // [0] Focus (m)
        startCamParam.Append(0.0);           // [1] Kappa
        startCamParam.Append(pixelSize);     // [2] Sx (m)
        startCamParam.Append(pixelSize);     // [3] Sy (m)
        startCamParam.Append(960.0);         // [4] Cx (px)
        startCamParam.Append(540.0);         // [5] Cy (px)
        startCamParam.Append(1920);          // [6] ImageWidth
        startCamParam.Append(1080);          // [7] ImageHeight

        HalconCpp::SetCalibDataCamParam(m_calibDataID, 0, "area_scan_division",
                                         startCamParam);

        HalconCpp::SetCalibDataCalibObject(m_calibDataID, 0,
                                            m_caltabFile.toStdWString().c_str());

        // Feed each image into calibration
        for (int i = 0; i < m_calibImages.size(); i++) {
            try {
                HalconCpp::HTuple empty;
                HalconCpp::FindCalibObject(m_calibImages[i], m_calibDataID, 0, 0,
                                           i, empty, empty);
            } catch (HalconCpp::HException &) {
                // Some images may fail to find marks — skip them
                continue;
            }
        }

        // Run calibration
        HalconCpp::HTuple hv_Error;
        HalconCpp::CalibrateCameras(m_calibDataID, &hv_Error);

        // Retrieve results
        HalconCpp::GetCalibData(m_calibDataID, "camera", 0, "params", &m_camParam);

        result.success          = true;
        result.reprojectionError = hv_Error[0].D();
        // area_scan_division: [0]Focus(m) [1]Kappa [2]Sx(m) [3]Sy(m) [4]Cx(px) [5]Cy(px) [6]ImgW [7]ImgH
        result.focalLength       = m_camParam[0].D();
        result.distortionK1      = m_camParam[1].D();
        result.distortionK2      = 0.0;
        result.distortionK3      = 0.0;
        result.distortionP1      = 0.0;
        result.distortionP2      = 0.0;
        result.cx                = m_camParam[4].D();
        result.cy                = m_camParam[5].D();

        // Save last calibration object pose for CoordTransform
        HalconCpp::GetCalibDataObservPose(m_calibDataID, 0, 0,
                                           m_calibImages.size() - 1,
                                           &m_lastCalibPose);

        m_intrinsicDone = true;
        emit intrinsicCompleted(result);

    } catch (HalconCpp::HException &e) {
        result.errorMessage = "标定失败: " + QString(e.ErrorMessage().Text());
        emit calibError(result.errorMessage);
    }

    return result;
}

QImage CalibrationManager::undistortImage(const QImage &image)
{
    if (!m_intrinsicDone) return image;

    try {
        if (!m_distMapComputed) {
            HalconCpp::HTuple empty;
            HalconCpp::GenRadialDistortionMap(&m_distortionMap, m_camParam,
                                               empty, "bilinear");
            m_distMapComputed = true;
        }

        HalconCpp::HImage hImg = qImageToHImage(image);
        HalconCpp::HObject undistorted;
        HalconCpp::MapImage(hImg, m_distortionMap, &undistorted);

        // Convert HImage → QImage
        HalconCpp::HTuple hv_Pointer, hv_Type, hv_W, hv_H;
        HalconCpp::GetImagePointer1(undistorted, &hv_Pointer, &hv_Type, &hv_W, &hv_H);

        int w = hv_W[0].I();
        int h = hv_H[0].I();
        QImage result(reinterpret_cast<const uchar *>(hv_Pointer[0].L()),
                      w, h, QImage::Format_Grayscale8);
        return result.copy();

    } catch (HalconCpp::HException &e) {
        qWarning() << "undistort failed:" << e.ErrorMessage().Text();
        return image;
    }
}

bool CalibrationManager::saveIntrinsic(const QString &filePath)
{
    if (!m_intrinsicDone) return false;
    try {
        HalconCpp::WriteCamPar(m_camParam, filePath.toStdWString().c_str());
        return true;
    } catch (HalconCpp::HException &e) {
        emit calibError("WriteCamPar 失败: " + QString(e.ErrorMessage().Text()));
        return false;
    }
}

bool CalibrationManager::loadIntrinsic(const QString &filePath)
{
    try {
        HalconCpp::ReadCamPar(filePath.toStdWString().c_str(), &m_camParam);
        m_intrinsicDone = true;
        return true;
    } catch (HalconCpp::HException &e) {
        emit calibError("ReadCamPar 失败: " + QString(e.ErrorMessage().Text()));
        return false;
    }
}

bool CalibrationManager::isIntrinsicCalibrated() const
{
    return m_intrinsicDone;
}

void CalibrationManager::clearCalibImages()
{
    m_calibImages.clear();
    try {
        if (m_calibDataID.Length() > 0) {
            HalconCpp::ClearCalibData(m_calibDataID);
        }
    } catch (const HalconCpp::HException &e) {
        qWarning() << "clearCalibImages HALCON exception:"
                   << e.ErrorMessage().Text();
    } catch (const std::exception &e) {
        qWarning() << "clearCalibImages std exception:" << e.what();
    } catch (...) {
        qWarning() << "clearCalibImages unknown exception";
    }
}

// ── Eye-in-hand calibration ──────────────────────────────────────

bool CalibrationManager::addCalibPose(const RobotPose &toolInBase,
                                       const QImage &calibImage)
{
    if (!m_intrinsicDone) {
        emit calibError("请先完成相机内参标定");
        return false;
    }

    try {
        HalconCpp::HImage hImg = qImageToHImage(calibImage);

        // Find calibration plate and extract marks
        HalconCpp::HObject caltabRegion;
        HalconCpp::FindCaltab(hImg, &caltabRegion,
                               m_caltabFile.toStdWString().c_str(),
                               3, 112, 5);

        HalconCpp::HTuple hv_RCoord, hv_CCoord, hv_StartPose;
        HalconCpp::FindMarksAndPose(hImg, caltabRegion,
                                     m_caltabFile.toStdWString().c_str(),
                                     m_camParam,
                                     128, 10, 18, 0.9, 15, 100,
                                     &hv_RCoord, &hv_CCoord, &hv_StartPose);

        if (hv_RCoord.Length() < 10) {
            emit calibError("标定板标记点不足，请调整图像质量或光照");
            return false;
        }

        // Store robot pose and image (CalibData API will find marks later)
        m_robotPoses.append(toolInBase);
        m_handEyeImages.append(hImg);
        m_handEyePoseCount = m_robotPoses.size();

        emit calibImageAdded(m_handEyePoseCount, m_minPoses);
        return true;

    } catch (HalconCpp::HException &e) {
        QString msg = "FindMarksAndPose 失败: " + QString(e.ErrorMessage().Text());
        emit calibError(msg);
        return false;
    }
}

HandEyeResult CalibrationManager::calibrateHandEye()
{
    HandEyeResult result;

    if (m_handEyePoseCount < m_minPoses) {
        result.errorMessage = QString("位姿组数不足：需要至少 %1 组，当前 %2 组")
            .arg(m_minPoses).arg(m_handEyePoseCount);
        emit calibError(result.errorMessage);
        return result;
    }

    try {
        // Use calib_data API for hand-eye calibration
        HalconCpp::HTuple hv_HE_CalibDataID;
        HalconCpp::CreateCalibData("hand_eye_moving_cam",
                                    1, m_handEyePoseCount,
                                    &hv_HE_CalibDataID);

        HalconCpp::SetCalibDataCamParam(hv_HE_CalibDataID, 0,
                                         "area_scan_division", m_camParam);
        HalconCpp::SetCalibDataCalibObject(hv_HE_CalibDataID, 0,
                                            m_caltabFile.toStdWString().c_str());

        // Feed each image + robot pose
        for (int i = 0; i < m_handEyePoseCount; i++) {
            HalconCpp::HTuple empty;
            HalconCpp::FindCalibObject(m_handEyeImages[i],
                                        hv_HE_CalibDataID, 0, i,
                                        0, empty, empty);

            const RobotPose &rp = m_robotPoses[i];
            HalconCpp::HTuple hv_Pose;
            HalconCpp::CreatePose(rp.tx, rp.ty, rp.tz, rp.rx, rp.ry, rp.rz,
                                  "Rp+T", "gba", "point", &hv_Pose);

            HalconCpp::SetCalibData(hv_HE_CalibDataID,
                                     "tool", i, "tool_in_base_pose",
                                     hv_Pose);
        }

        // Run hand-eye calibration
        HalconCpp::HTuple hv_Errors;
        HalconCpp::CalibrateHandEye(hv_HE_CalibDataID, &hv_Errors);

        // Read result
        HalconCpp::GetCalibData(hv_HE_CalibDataID,
                                 "camera", 0, "tool_in_cam_pose",
                                 &m_handEyeCamInToolPose);

        result.success = true;
        result.translationErrorMm = hv_Errors[0].D();
        result.rotationErrorDeg   = hv_Errors[1].D() * 180.0 / M_PI;

        // Extract cam_H_tool from result pose
        result.camInToolTX = m_handEyeCamInToolPose[0].D();
        result.camInToolTY = m_handEyeCamInToolPose[1].D();
        result.camInToolTZ = m_handEyeCamInToolPose[2].D();
        result.camInToolRX = m_handEyeCamInToolPose[3].D();
        result.camInToolRY = m_handEyeCamInToolPose[4].D();
        result.camInToolRZ = m_handEyeCamInToolPose[5].D();

        // Cleanup
        HalconCpp::ClearCalibData(hv_HE_CalibDataID);

        m_handEyeDone = true;
        emit handEyeCompleted(result);

    } catch (HalconCpp::HException &e) {
        result.errorMessage = "手眼标定失败: " + QString(e.ErrorMessage().Text());
        emit calibError(result.errorMessage);
    }

    return result;
}

bool CalibrationManager::saveHandEyeResult(const QString &filePath)
{
    if (!m_handEyeDone) return false;
    try {
        HalconCpp::WritePose(m_handEyeCamInToolPose, filePath.toStdWString().c_str());
        return true;
    } catch (HalconCpp::HException &e) {
        emit calibError("WritePose 失败: " + QString(e.ErrorMessage().Text()));
        return false;
    }
}

bool CalibrationManager::loadHandEyeResult(const QString &filePath)
{
    try {
        HalconCpp::ReadPose(filePath.toStdWString().c_str(), &m_handEyeCamInToolPose);
        m_handEyeDone = true;
        return true;
    } catch (HalconCpp::HException &e) {
        emit calibError("ReadPose 失败: " + QString(e.ErrorMessage().Text()));
        return false;
    }
}

bool CalibrationManager::isHandEyeCalibrated() const
{
    return m_handEyeDone;
}

int CalibrationManager::poseCount() const
{
    return m_handEyePoseCount;
}

HalconCpp::HTuple CalibrationManager::camParam() const
{
    return m_camParam;
}

HalconCpp::HTuple CalibrationManager::handEyePose() const
{
    return m_handEyeCamInToolPose;
}
