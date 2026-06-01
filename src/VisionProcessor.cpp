#include "VisionProcessor.h"
#include <QDebug>
#include <QtMath>
#include <QImage>
#include <vector>
#include <cstring>

static VisionError mapHalconError(int hCode)
{
    // HALCON error codes reference.
    // License-related errors live in the 5200–5399 range.
    // NOTE: #1301 is "wrong value of control parameter" — a programming/binding
    // error, NOT a license problem, so it must not be mapped to NO_LICENSE.
    if (hCode >= 5200 && hCode <= 5399) return VisionError::NO_LICENSE;
    if (hCode == 1201 || hCode == 1202) return VisionError::INVALID_IMAGE;
    if (hCode == 6001 || hCode == 6002) return VisionError::OUT_OF_MEMORY;
    if (hCode == 3510 || hCode == 3511) return VisionError::LOW_CONTRAST;
    return VisionError::UNKNOWN;
}

VisionProcessor::VisionProcessor(QObject *parent)
    : QObject(parent)
{
}

VisionProcessor::~VisionProcessor()
{
    clearTemplate();
}

// ── QImage → HImage conversion ───────────────────────────────────

HalconCpp::HImage VisionProcessor::qImageToHImage(const QImage &img)
{
    QImage gray = img;
    if (gray.format() != QImage::Format_Grayscale8) {
        gray = gray.convertToFormat(QImage::Format_Grayscale8);
    }

    const int w = gray.width();
    const int h = gray.height();

    // QImage scanlines are 32-bit aligned, so bytesPerLine() can be larger
    // than the width (row padding). GenImage1 expects tightly-packed rows
    // (stride == width); copy row-by-row into a contiguous buffer to avoid a
    // skewed image when the width is not a multiple of 4. GenImage1 copies the
    // pixel data, so the temporary buffer is safe to release on return.
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

// ── Template management ──────────────────────────────────────────

bool VisionProcessor::createTemplate(const QImage &image, const QRectF &roi)
{
    QMutexLocker lock(&m_modelMutex);

    try {
        HalconCpp::HImage hImg = qImageToHImage(image);

        if (!roi.isNull() && roi.width() > 10 && roi.height() > 10) {
            // HRegion rectangle constructor is (Row1, Column1, Row2, Column2),
            // i.e. (top, left, bottom, right) = (y, x, y+h, x+w).
            HalconCpp::HRegion roiRegion(roi.y(), roi.x(),
                                          roi.y() + roi.height(),
                                          roi.x() + roi.width());
            hImg = hImg.ReduceDomain(roiRegion);
        }

        double angleStartRad  = m_config.angleStart * M_PI / 180.0;
        double angleExtentRad = m_config.angleExtent * M_PI / 180.0;

        // CreateScaledShapeModel(Template, NumLevels, AngleStart, AngleExtent,
        //   AngleStep, ScaleMin, ScaleMax, ScaleStep, Optimization, Metric,
        //   Contrast, MinContrast)
        // Scaled model tolerates size changes (e.g. the part moving closer/
        // farther from the camera), which plain create_shape_model cannot —
        // that was why a small move of the bottle lost the match.
        // Param 9 = Optimization ('auto'), Param 10 = Metric ('use_polarity').
        m_shapeModel.CreateScaledShapeModel(hImg, 0,
                                            angleStartRad, angleExtentRad, 0,
                                            m_config.minScale, m_config.maxScale, 0,
                                            L"auto", L"use_polarity",
                                            L"auto", L"auto");

        m_modelValid = true;
        emit templateCreated(true);
        return true;

    } catch (HalconCpp::HException &e) {
        QString msg = QString(e.ErrorMessage().Text());
        qWarning() << "CreateScaledShapeModel failed:" << msg;
        emit processingError(mapHalconError(e.ErrorCode()), msg);
        m_modelValid = false;
        return false;
    }
}

bool VisionProcessor::loadTemplate(const QString &filePath)
{
    QMutexLocker lock(&m_modelMutex);
    clearTemplateUnlocked();   // mutex already held — must NOT re-lock

    try {
        m_shapeModel.ReadShapeModel(filePath.toStdWString().c_str());
        m_modelValid = true;
        return true;
    } catch (HalconCpp::HException &e) {
        QString msg = QString(e.ErrorMessage().Text());
        qWarning() << "ReadShapeModel failed:" << msg;
        emit processingError(mapHalconError(e.ErrorCode()), msg);
        return false;
    }
}

bool VisionProcessor::saveTemplate(const QString &filePath)
{
    QMutexLocker lock(&m_modelMutex);

    if (!m_modelValid) return false;

    try {
        m_shapeModel.WriteShapeModel(filePath.toStdWString().c_str());
        return true;
    } catch (HalconCpp::HException &e) {
        QString msg = QString(e.ErrorMessage().Text());
        qWarning() << "WriteShapeModel failed:" << msg;
        emit processingError(mapHalconError(e.ErrorCode()), msg);
        return false;
    }
}

bool VisionProcessor::isTemplateLoaded() const
{
    return m_modelValid;
}

void VisionProcessor::clearTemplate()
{
    QMutexLocker lock(&m_modelMutex);
    clearTemplateUnlocked();
}

void VisionProcessor::clearTemplateUnlocked()
{
    if (m_modelValid) {
        try {
            m_shapeModel.ClearShapeModel();
        } catch (...) {
            // ignore destructor exceptions
        }
        m_modelValid = false;
    }
}

// ── Matching ─────────────────────────────────────────────────────

QList<MatchResult> VisionProcessor::findObject(const QImage &image)
{
    QList<MatchResult> results;
    QMutexLocker lock(&m_modelMutex);

    if (!m_modelValid) return results;

    try {
        HalconCpp::HImage hImg = qImageToHImage(image);

        double angleStartRad  = m_config.angleStart * M_PI / 180.0;
        double angleExtentRad = m_config.angleExtent * M_PI / 180.0;

        HalconCpp::HTuple hv_Row, hv_Column, hv_Angle, hv_Scale, hv_Score;

        // FindScaledShapeModel(Image, AngleStart, AngleExtent,
        //   ScaleMin, ScaleMax, MinScore, NumMatches, MaxOverlap, SubPixel,
        //   NumLevels, Greediness, &Row, &Column, &Angle, &Scale, &Score)
        m_shapeModel.FindScaledShapeModel(hImg,
                                          angleStartRad, angleExtentRad,
                                          m_config.minScale, m_config.maxScale,
                                          m_config.minScore,
                                          m_config.maxTargets,
                                          0.5,
                                          L"least_squares",
                                          0,
                                          0.9,
                                          &hv_Row, &hv_Column, &hv_Angle,
                                          &hv_Scale, &hv_Score);

        int numFound = hv_Score.Length();
        for (int i = 0; i < numFound; i++) {
            MatchResult r;
            r.x        = hv_Column[i].D();
            r.y        = hv_Row[i].D();
            r.angle    = hv_Angle[i].D() * 180.0 / M_PI;
            r.score    = hv_Score[i].D();
            r.valid    = r.score >= m_config.minScore;
            r.targetId = i;
            results.append(r);
        }

    } catch (HalconCpp::HException &e) {
        QString msg = QString(e.ErrorMessage().Text());
        qWarning() << "FindScaledShapeModel failed:" << msg;
        emit processingError(mapHalconError(e.ErrorCode()), msg);
    }

    return results;
}

void VisionProcessor::processImage(const QImage &image)
{
    // Drop this frame if the previous one is still being processed. This keeps
    // the vision thread's event queue from accumulating an unbounded backlog
    // of matches, which is what made createTemplate() (called from the GUI
    // thread) block on m_modelMutex and appear to "do nothing" after the user
    // pressed Stop.
    bool expected = false;
    if (!m_busy.compare_exchange_strong(expected, true)) {
        return;
    }

    // Always clear the busy flag, even if matching throws — otherwise every
    // subsequent frame would be permanently dropped.
    struct BusyGuard {
        std::atomic<bool> &flag;
        ~BusyGuard() { flag.store(false); }
    } guard{m_busy};

    auto results = findObject(image);
    emit matchingComplete(results);
}

void VisionProcessor::setMatchConfig(const MatchConfig &cfg)
{
    QMutexLocker lock(&m_modelMutex);
    m_config = cfg;
}

MatchConfig VisionProcessor::matchConfig() const
{
    QMutexLocker lock(&m_modelMutex);
    return m_config;
}
