#include "MeasurementEngine.h"
#include <QtMath>
#include <QDebug>
#include <QImage>
#include <QRectF>
#include <QPointF>
#include <vector>
#include <cstring>
#include <algorithm>

#include "HalconCpp.h"

static HalconCpp::HImage qImageToHImage(const QImage &img)
{
    QImage gray = img;
    if (gray.format() != QImage::Format_Grayscale8) {
        gray = gray.convertToFormat(QImage::Format_Grayscale8);
    }

    const int w = gray.width();
    const int h = gray.height();

    // QImage scanlines are 32-bit aligned; copy row-by-row into a contiguous
    // buffer so GenImage1 (which expects stride == width) gets a correct image
    // even when width is not a multiple of 4. GenImage1 copies the data.
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

MeasurementEngine::MeasurementEngine(QObject *parent)
    : QObject(parent)
{
}

void MeasurementEngine::setPixelEquivalent(double mmPerPixel)
{
    m_pixelEquivalentMm = mmPerPixel;
}

double MeasurementEngine::pixelEquivalent() const
{
    return m_pixelEquivalentMm;
}

void MeasurementEngine::setMeasurementConfig(const MeasurementConfig &cfg)
{
    m_config = cfg;
}

MeasurementConfig MeasurementEngine::measurementConfig() const
{
    return m_config;
}

// ── Dispatcher ───────────────────────────────────────────────────

MeasureResult MeasurementEngine::measure(const QImage &image, MeasureType type,
                                          const QList<QPointF> &points,
                                          const QRectF &roi)
{
    switch (type) {
        case MeasureType::LENGTH: return measureLength(image, roi);
        case MeasureType::CIRCLE: return measureCircle(image, roi);
        case MeasureType::DISTANCE:
            if (points.size() < 2) {
                MeasureResult r; r.valid = false;
                r.label = "需要选择两个点"; return r;
            }
            return measureDistance(image, points[0], points[1]);
        case MeasureType::ANGLE: {
            const QList<QPointF> line1 = points.mid(0, 2);
            const QList<QPointF> line2 = points.mid(2, 2);
            if (line1.size() < 2 || line2.size() < 2) {
                MeasureResult r;
                r.type = MeasureType::ANGLE;
                r.valid = false;
                r.label = "需要先在图像上点选两条线（每条线 2 个端点）";
                return r;
            }
            return measureAngle(image, line1, line2);
        }
        case MeasureType::AREA:
            if (roi.isEmpty() || !roi.isValid()) {
                MeasureResult r;
                r.type = MeasureType::AREA;
                r.valid = false;
                r.label = "需要先选择 ROI 区域";
                return r;
            }
            return measureArea(image, roi);
    }
    return MeasureResult{};
}

// ── Length measurement ───────────────────────────────────────────

MeasureResult MeasurementEngine::measureLength(const QImage &image, const QRectF &roi)
{
    MeasureResult r;
    r.type = MeasureType::LENGTH;

    try {
        HalconCpp::HImage hImg = qImageToHImage(image);

        // Reduce domain to ROI
        // HRegion rectangle constructor is (Row1, Column1, Row2, Column2).
        HalconCpp::HRegion roiRegion(roi.y(), roi.x(),
                                      roi.y() + roi.height(),
                                      roi.x() + roi.width());
        HalconCpp::HImage reduced = hImg.ReduceDomain(roiRegion);

        // Extract sub-pixel edges
        HalconCpp::HObject edges;
        HalconCpp::EdgesSubPix(reduced, &edges,
            HalconCpp::HTuple("canny"), HalconCpp::HTuple(0.5),
            HalconCpp::HTuple(10), HalconCpp::HTuple(30));

        HalconCpp::HObject selected;
        HalconCpp::SelectShapeXld(edges, &selected,
            HalconCpp::HTuple("contlength"), HalconCpp::HTuple("and"),
            HalconCpp::HTuple(50), HalconCpp::HTuple(99999));

        HalconCpp::HTuple hv_RowBegin, hv_ColBegin, hv_RowEnd, hv_ColEnd, hv_Nr, hv_Nc, hv_Dist;
        HalconCpp::HTuple e_len;
        HalconCpp::FitLineContourXld(selected,
            HalconCpp::HTuple("tukey"), HalconCpp::HTuple(-1),
            HalconCpp::HTuple(0), HalconCpp::HTuple(5), HalconCpp::HTuple(2),
            &hv_RowBegin, &hv_ColBegin, &hv_RowEnd, &hv_ColEnd,
            &hv_Nr, &hv_Nc, &hv_Dist);

        // Compute Euclidean distance × pixel equivalent
        double dx = hv_ColEnd[0].D() - hv_ColBegin[0].D();
        double dy = hv_RowEnd[0].D() - hv_RowBegin[0].D();
        double lengthPx = std::sqrt(dx * dx + dy * dy);
        r.value = lengthPx * m_pixelEquivalentMm;
        r.unit  = "mm";
        r.valid = true;
        r.label = "长度";
        r.pt1   = QPointF(hv_ColBegin[0].D(), hv_RowBegin[0].D());
        r.pt2   = QPointF(hv_ColEnd[0].D(), hv_RowEnd[0].D());

    } catch (HalconCpp::HException &e) {
        qWarning() << "measureLength failed:" << e.ErrorMessage().Text();
        r.valid = false;
        r.label = QString("长度测量失败: ") + e.ErrorMessage().Text();
    }

    emit measurementComplete(r);
    return r;
}

// ── Circle diameter measurement ──────────────────────────────────

MeasureResult MeasurementEngine::measureCircle(const QImage &image, const QRectF &roi)
{
    MeasureResult r;
    r.type = MeasureType::CIRCLE;

    try {
        HalconCpp::HImage hImg = qImageToHImage(image);

        // HRegion rectangle constructor is (Row1, Column1, Row2, Column2).
        HalconCpp::HRegion roiRegion(roi.y(), roi.x(),
                                      roi.y() + roi.height(),
                                      roi.x() + roi.width());
        HalconCpp::HImage reduced = hImg.ReduceDomain(roiRegion);

        HalconCpp::HObject edges;
        HalconCpp::EdgesSubPix(reduced, &edges,
            HalconCpp::HTuple("canny"), HalconCpp::HTuple(0.5),
            HalconCpp::HTuple(10), HalconCpp::HTuple(30));

        HalconCpp::HObject selected;
        HalconCpp::SelectShapeXld(edges, &selected,
            HalconCpp::HTuple("contlength"), HalconCpp::HTuple("and"),
            HalconCpp::HTuple(30), HalconCpp::HTuple(99999));

        HalconCpp::HTuple hv_Row, hv_Col, hv_Radius;
        HalconCpp::HTuple emptyOut;
        HalconCpp::FitCircleContourXld(selected,
            HalconCpp::HTuple("algebraic"), HalconCpp::HTuple(-1),
            HalconCpp::HTuple(0), HalconCpp::HTuple(0),
            HalconCpp::HTuple(3), HalconCpp::HTuple(2),
            &hv_Row, &hv_Col, &hv_Radius,
            &emptyOut, &emptyOut, &emptyOut);

        double radiusMm = hv_Radius[0].D() * m_pixelEquivalentMm;
        r.radius = radiusMm;
        r.value  = 2.0 * radiusMm;   // diameter
        r.center = QPointF(hv_Col[0].D(), hv_Row[0].D());
        r.unit   = "mm";
        r.valid  = true;
        r.label  = "圆直径";

    } catch (HalconCpp::HException &e) {
        qWarning() << "measureCircle failed:" << e.ErrorMessage().Text();
        r.valid = false;
        r.label = QString("圆直径测量失败: ") + e.ErrorMessage().Text();
    }

    emit measurementComplete(r);
    return r;
}

// ── Point-to-point distance ──────────────────────────────────────

MeasureResult MeasurementEngine::measureDistance(const QImage &image,
                                                  const QPointF &p1, const QPointF &p2)
{
    MeasureResult r;
    r.type = MeasureType::DISTANCE;

    try {
        HalconCpp::HImage hImg = qImageToHImage(image);

        HalconCpp::HTuple hv_Dist;
        HalconCpp::DistancePp(p1.y(), p1.x(), p2.y(), p2.x(), &hv_Dist);

        r.value = hv_Dist[0].D() * m_pixelEquivalentMm;
        r.unit  = "mm";
        r.pt1   = p1;
        r.pt2   = p2;
        r.valid = true;
        r.label = "距离";

    } catch (HalconCpp::HException &e) {
        qWarning() << "measureDistance failed:" << e.ErrorMessage().Text();
        r.valid = false;
        r.label = QString("距离测量失败: ") + e.ErrorMessage().Text();
    }

    emit measurementComplete(r);
    return r;
}

// ── Angle between two lines ──────────────────────────────────────
// Each line is defined by 2 user-clicked endpoints. We build a small ROI
// window around each pair (bbox + padding), run EdgesSubPix inside that
// window, fit a line to the surviving contours, then take AngleLl between
// the two fitted lines. If either ROI yields no usable contour we throw
// so the dispatcher returns r.valid = false (instead of a phantom number).
MeasureResult MeasurementEngine::measureAngle(const QImage &image,
                                               const QList<QPointF> &line1,
                                               const QList<QPointF> &line2)
{
    MeasureResult r;
    r.type = MeasureType::ANGLE;

    try {
        if (line1.size() < 2 || line2.size() < 2) {
            throw HalconCpp::HException("measureAngle: 每条线至少需要 2 个端点");
        }

        HalconCpp::HImage hImg = qImageToHImage(image);
        const int imgW = image.width();
        const int imgH = image.height();

        auto fitLineInRoi = [&](const QPointF &a, const QPointF &b,
                                 HalconCpp::HTuple &Rs, HalconCpp::HTuple &Cs,
                                 HalconCpp::HTuple &Re, HalconCpp::HTuple &Ce) {
            constexpr int pad = 20;
            // Parenthesize std::min/std::max calls to defeat the Windows.h
            // min/max macros that HalconCpp transitively drags in.
            double x0 = (std::max)(0.0,  (std::min)(a.x(), b.x()) - pad);
            double y0 = (std::max)(0.0,  (std::min)(a.y(), b.y()) - pad);
            double x1 = (std::min)(double(imgW - 1), (std::max)(a.x(), b.x()) + pad);
            double y1 = (std::min)(double(imgH - 1), (std::max)(a.y(), b.y()) + pad);
            if (x1 - x0 < 5 || y1 - y0 < 5) {
                throw HalconCpp::HException("线 ROI 过小（端点太近或越界）");
            }

            HalconCpp::HRegion roi(y0, x0, y1, x1);
            HalconCpp::HImage  reduced = hImg.ReduceDomain(roi);

            HalconCpp::HObject edges, selected;
            HalconCpp::EdgesSubPix(reduced, &edges,
                HalconCpp::HTuple("canny"), HalconCpp::HTuple(0.5),
                HalconCpp::HTuple(10), HalconCpp::HTuple(30));
            HalconCpp::SelectShapeXld(edges, &selected,
                HalconCpp::HTuple("contlength"), HalconCpp::HTuple("and"),
                HalconCpp::HTuple(10), HalconCpp::HTuple(99999));

            HalconCpp::HTuple cnt;
            HalconCpp::CountObj(selected, &cnt);
            if (cnt[0].I() == 0) {
                throw HalconCpp::HException("线 ROI 内边缘点过少（contlength<10）");
            }

            // Collapse to the single longest contour. EdgesSubPix in a noisy ROI
            // typically returns multiple long edges; feeding all of them into
            // FitLineContourXld would produce N-element output tuples, and the
            // downstream AngleLl would then throw HALCON #1405 (control param 5
            // count mismatch) when line1 and line2 ROIs yielded different N.
            HalconCpp::HObject longestContour;
            if (cnt[0].I() == 1) {
                longestContour = selected;
            } else {
                HalconCpp::HTuple lengths;
                HalconCpp::LengthXld(selected, &lengths);
                HalconCpp::HTuple sortedIndices;
                HalconCpp::TupleSortIndex(lengths, &sortedIndices);
                // SelectObj index is 1-based; TupleSortIndex returns 0-based ascending.
                HalconCpp::SelectObj(selected, &longestContour,
                                      sortedIndices[cnt[0].I() - 1] + 1);
            }

            HalconCpp::HTuple Nr, Nc, Dist;
            HalconCpp::FitLineContourXld(longestContour,
                HalconCpp::HTuple("tukey"), HalconCpp::HTuple(-1),
                HalconCpp::HTuple(0), HalconCpp::HTuple(5), HalconCpp::HTuple(2),
                &Rs, &Cs, &Re, &Ce, &Nr, &Nc, &Dist);
        };

        HalconCpp::HTuple R1s, C1s, R1e, C1e;
        HalconCpp::HTuple R2s, C2s, R2e, C2e;
        fitLineInRoi(line1[0], line1[1], R1s, C1s, R1e, C1e);
        fitLineInRoi(line2[0], line2[1], R2s, C2s, R2e, C2e);

        HalconCpp::HTuple hv_Angle;
        HalconCpp::AngleLl(R1s, C1s, R1e, C1e,
                            R2s, C2s, R2e, C2e,
                            &hv_Angle);

        r.value = std::fabs(hv_Angle[0].D() * 180.0 / M_PI);
        r.unit  = "°";
        r.valid = true;
        r.label = "角度";
        r.pt1   = QPointF(C1s[0].D(), R1s[0].D());
        r.pt2   = QPointF(C2s[0].D(), R2s[0].D());

    } catch (HalconCpp::HException &e) {
        qWarning() << "measureAngle failed:" << e.ErrorMessage().Text();
        r.valid = false;
        r.label = QString("角度测量失败: ") + e.ErrorMessage().Text();
    }

    emit measurementComplete(r);
    return r;
}

// ── Area measurement ─────────────────────────────────────────────
// Reduce the image domain to the user-selected ROI, run BinaryThreshold in
// "max_separability" (Otsu) "dark" mode to find dark objects, filter by
// minimum connected-component area (configurable), pick the largest, and
// compute its area in mm² via the pixel equivalent. Throws on invalid ROI
// or empty result so the dispatcher returns r.valid = false (instead of a
// phantom number computed over the whole image with a hardcoded threshold).
MeasureResult MeasurementEngine::measureArea(const QImage &image, const QRectF &roi)
{
    MeasureResult r;
    r.type = MeasureType::AREA;

    try {
        if (roi.isEmpty() || !roi.isValid() ||
            roi.width() < 5 || roi.height() < 5) {
            throw HalconCpp::HException("ROI 未指定或过小（< 5×5 px）");
        }

        HalconCpp::HImage hImg = qImageToHImage(image);

        // HRegion rectangle constructor is (Row1, Column1, Row2, Column2).
        HalconCpp::HRegion roiRegion(roi.y(), roi.x(),
                                      roi.y() + roi.height(),
                                      roi.x() + roi.width());
        HalconCpp::HImage reduced = hImg.ReduceDomain(roiRegion);

        // Auto Otsu threshold for dark objects (replaces hardcoded [0,128]).
        HalconCpp::HObject thresholdRegion;
        HalconCpp::HTuple usedThresh;
        HalconCpp::BinaryThreshold(reduced, &thresholdRegion,
            HalconCpp::HTuple("max_separability"),
            HalconCpp::HTuple("dark"),
            &usedThresh);

        HalconCpp::HObject connected;
        HalconCpp::Connection(thresholdRegion, &connected);

        HalconCpp::HObject selected;
        HalconCpp::SelectShape(connected, &selected,
            HalconCpp::HTuple("area"), HalconCpp::HTuple("and"),
            HalconCpp::HTuple(m_config.minAreaPx),
            HalconCpp::HTuple(1e9));

        HalconCpp::HTuple cnt;
        HalconCpp::CountObj(selected, &cnt);
        if (cnt[0].I() == 0) {
            const QByteArray emsg =
                QString("ROI 内未检测到目标（按 Otsu 阈值 + 最小面积 %1 px）")
                    .arg(m_config.minAreaPx).toLocal8Bit();
            throw HalconCpp::HException(emsg.constData());
        }

        // Pick the single largest connected component (Percent unused for max_area).
        HalconCpp::HObject biggest;
        HalconCpp::SelectShapeStd(selected, &biggest,
            HalconCpp::HTuple("max_area"), HalconCpp::HTuple(0));

        HalconCpp::HTuple hv_Area, hv_Row, hv_Col;
        HalconCpp::AreaCenter(biggest, &hv_Area, &hv_Row, &hv_Col);

        double areaMm2 = hv_Area[0].D() * m_pixelEquivalentMm * m_pixelEquivalentMm;
        r.value  = areaMm2;
        r.unit   = "mm²";
        r.valid  = true;
        r.label  = QString("面积 (阈值=%1)").arg(usedThresh[0].I());
        r.area   = areaMm2;
        r.center = QPointF(hv_Col[0].D(), hv_Row[0].D());

    } catch (HalconCpp::HException &e) {
        qWarning() << "measureArea failed:" << e.ErrorMessage().Text();
        r.valid = false;
        r.label = QString("面积测量失败: ") + e.ErrorMessage().Text();
    }

    emit measurementComplete(r);
    return r;
}
