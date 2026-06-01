#include "MeasurementEngine.h"
#include <QtMath>
#include <QDebug>
#include <QImage>
#include <QRectF>
#include <QPointF>
#include <vector>
#include <cstring>

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
        case MeasureType::ANGLE:
            if (points.size() < 4) {
                MeasureResult r; r.valid = false;
                r.label = "需要选择两条直线（4 个点）"; return r;
            }
            return measureAngle(image, {points[0], points[1]},
                                       {points[2], points[3]});
        case MeasureType::AREA: return measureArea(image, roi);
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
    }

    emit measurementComplete(r);
    return r;
}

// ── Angle between two lines ──────────────────────────────────────

MeasureResult MeasurementEngine::measureAngle(const QImage &image,
                                               const QList<QPointF> & /*line1*/,
                                               const QList<QPointF> & /*line2*/)
{
    MeasureResult r;
    r.type = MeasureType::ANGLE;

    try {
        HalconCpp::HImage hImg = qImageToHImage(image);

        // Extract edges in ROI (approximate from points)
        HalconCpp::HObject edges;
        HalconCpp::EdgesSubPix(hImg, &edges,
            HalconCpp::HTuple("canny"), HalconCpp::HTuple(0.5),
            HalconCpp::HTuple(10), HalconCpp::HTuple(30));
        HalconCpp::HObject longEdges;
        HalconCpp::SelectShapeXld(edges, &longEdges,
            HalconCpp::HTuple("contlength"), HalconCpp::HTuple("and"),
            HalconCpp::HTuple(30), HalconCpp::HTuple(99999));

        // Split contours into separate objects
        HalconCpp::HObject connected;
        HalconCpp::Connection(longEdges, &connected);

        // Select 2 longest contours → fit 2 lines → compute angle
        HalconCpp::HObject sorted;
        HalconCpp::SortContoursXld(connected, &sorted,
            HalconCpp::HTuple("upper_left"), HalconCpp::HTuple("true"),
            HalconCpp::HTuple("column"));

        HalconCpp::HTuple hv_Row1, hv_Col1, hv_Row2, hv_Col2;
        HalconCpp::HTuple hv_Row3, hv_Col3, hv_Row4, hv_Col4;

        // Fit line 1 to first contour
        HalconCpp::HObject contour1;
        HalconCpp::SelectObj(sorted, &contour1, 1);
        HalconCpp::HTuple e1;
        HalconCpp::FitLineContourXld(contour1,
            HalconCpp::HTuple("tukey"), HalconCpp::HTuple(-1),
            HalconCpp::HTuple(0), HalconCpp::HTuple(5), HalconCpp::HTuple(2),
            &hv_Row1, &hv_Col1, &hv_Row2, &hv_Col2,
            &e1, &e1, &e1);

        HalconCpp::HObject contour2;
        HalconCpp::SelectObj(sorted, &contour2, 2);
        HalconCpp::HTuple e2;
        HalconCpp::FitLineContourXld(contour2,
            HalconCpp::HTuple("tukey"), HalconCpp::HTuple(-1),
            HalconCpp::HTuple(0), HalconCpp::HTuple(5), HalconCpp::HTuple(2),
            &hv_Row3, &hv_Col3, &hv_Row4, &hv_Col4,
            &e2, &e2, &e2);

        // Compute angle between lines
        HalconCpp::HTuple hv_Angle;
        HalconCpp::AngleLl(hv_Row1, hv_Col1, hv_Row2, hv_Col2,
                            hv_Row3, hv_Col3, hv_Row4, hv_Col4,
                            &hv_Angle);

        r.value = fabs(hv_Angle[0].D() * 180.0 / M_PI);
        r.unit  = "°";
        r.valid = true;
        r.label = "角度";

    } catch (HalconCpp::HException &e) {
        qWarning() << "measureAngle failed:" << e.ErrorMessage().Text();
    }

    emit measurementComplete(r);
    return r;
}

// ── Area measurement ─────────────────────────────────────────────

MeasureResult MeasurementEngine::measureArea(const QImage &image, const QRectF & /*roi*/)
{
    MeasureResult r;
    r.type = MeasureType::AREA;

    try {
        HalconCpp::HImage hImg = qImageToHImage(image);

        // Threshold at mid-gray to get binary region
        HalconCpp::HObject thresholdRegion;
        HalconCpp::Threshold(hImg, &thresholdRegion,
            HalconCpp::HTuple(0), HalconCpp::HTuple(128));

        HalconCpp::HObject connected;
        HalconCpp::Connection(thresholdRegion, &connected);
        HalconCpp::HObject selected;
        HalconCpp::SelectShapeStd(connected, &selected,
            HalconCpp::HTuple("max_area"), HalconCpp::HTuple(70));

        HalconCpp::HTuple hv_Area, hv_Row, hv_Col;
        HalconCpp::AreaCenter(selected, &hv_Area, &hv_Row, &hv_Col);

        double areaMm2 = hv_Area[0].D() * m_pixelEquivalentMm * m_pixelEquivalentMm;
        r.value  = areaMm2;
        r.unit   = "mm²";
        r.valid  = true;
        r.label  = "面积";
        r.center = QPointF(hv_Col[0].D(), hv_Row[0].D());

    } catch (HalconCpp::HException &e) {
        qWarning() << "measureArea failed:" << e.ErrorMessage().Text();
    }

    emit measurementComplete(r);
    return r;
}
