#include "ImageView.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QtMath>

ImageView::ImageView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
}

void ImageView::setImage(const QImage &image)
{
    m_image = image;
    m_dirtyCache = true;
    update();
}

void ImageView::clearImage()
{
    m_image = QImage();
    m_cachedPixmap = QPixmap();
    clearAllOverlays();
    update();
}

// ── Overlays ─────────────────────────────────────────────────────

void ImageView::addDetectionOverlay(double x, double y, double angle, double score, bool valid)
{
    DetectionItem item;
    item.center = QPointF(x, y);
    item.angle  = angle;
    item.score  = score;
    item.valid  = valid;
    m_detections.append(item);
    update();
}

void ImageView::addMeasurementOverlay(double x, double y, const QString &label)
{
    MeasurementItem item;
    item.pos   = QPointF(x, y);
    item.label = label;
    m_measurements.append(item);
    update();
}

void ImageView::addLineMeasurementOverlay(const QPointF &p1, const QPointF &p2, const QString &label)
{
    LineMeasurementItem item;
    item.p1    = p1;
    item.p2    = p2;
    item.label = label;
    m_lineMeasurements.append(item);
    update();
}

void ImageView::addCircleMeasurementOverlay(const QPointF &center, double radius, const QString &label)
{
    CircleMeasurementItem item;
    item.center = center;
    item.radius = radius;
    item.label  = label;
    m_circleMeasurements.append(item);
    update();
}

void ImageView::addAngleOverlay(const QPointF &line1Start, const QPointF &line1End,
                                const QPointF &line2Start, const QPointF &line2End,
                                const QString &label)
{
    AngleMeasurementItem item;
    item.l1Start = line1Start;
    item.l1End   = line1End;
    item.l2Start = line2Start;
    item.l2End   = line2End;
    item.label   = label;
    m_angleMeasurements.append(item);
    update();
}

void ImageView::addCalibOverlay(const QList<QPointF> &corners)
{
    m_calibCorners = corners;
    update();
}

void ImageView::addClickedPointMarker(const QPointF &pos, const QColor &color, const QString &label)
{
    ClickedPointMarker item;
    item.pos   = pos;
    item.color = color;
    item.label = label;
    m_clickedPoints.append(item);
    update();
}

void ImageView::clearClickedPointMarkers()
{
    m_clickedPoints.clear();
    update();
}

void ImageView::clearDetectionOverlays()
{
    m_detections.clear();
    update();
}

void ImageView::clearMeasurementOverlays()
{
    m_measurements.clear();
    m_lineMeasurements.clear();
    m_circleMeasurements.clear();
    m_angleMeasurements.clear();
    update();
}

void ImageView::clearCalibrationOverlays()
{
    m_calibCorners.clear();
    update();
}

void ImageView::clearAllOverlays()
{
    m_detections.clear();
    m_measurements.clear();
    m_lineMeasurements.clear();
    m_circleMeasurements.clear();
    m_angleMeasurements.clear();
    m_calibCorners.clear();
    m_clickedPoints.clear();
    update();
}

void ImageView::clearOverlays()
{
    clearAllOverlays();
}

// ── View controls ────────────────────────────────────────────────

void ImageView::setInteractionMode(InteractionMode mode)
{
    m_interactionMode = mode;
    setCursor(mode == InteractionMode::POINT_PICK ? Qt::CrossCursor : Qt::ArrowCursor);
}

void ImageView::fitToWindow()
{
    if (m_image.isNull()) return;
    double sx = double(width())  / m_image.width();
    double sy = double(height()) / m_image.height();
    m_zoom = qMin(sx, sy) * 0.9;
    m_panX = 0;
    m_panY = 0;
    m_dirtyCache = true;
    update();
}

void ImageView::setZoom(double factor)
{
    m_zoom = qBound(0.05, factor, 20.0);
    m_dirtyCache = true;
    emit zoomChanged(m_zoom);
    update();
}

// ── Coordinate transforms ────────────────────────────────────────

QPointF ImageView::viewToImage(const QPointF &viewPos) const
{
    QRectF vp = getImageViewport();
    double ix = (viewPos.x() - vp.center().x()) / m_zoom + m_image.width()  / 2.0 + m_panX;
    double iy = (viewPos.y() - vp.center().y()) / m_zoom + m_image.height() / 2.0 + m_panY;
    return QPointF(ix, iy);
}

QPointF ImageView::imageToView(const QPointF &imagePos) const
{
    QRectF vp = getImageViewport();
    double vx = (imagePos.x() - m_image.width()  / 2.0 - m_panX) * m_zoom + vp.center().x();
    double vy = (imagePos.y() - m_image.height() / 2.0 - m_panY) * m_zoom + vp.center().y();
    return QPointF(vx, vy);
}

QRectF ImageView::getImageViewport() const
{
    return QRectF(0, 0, width(), height());
}

// ── Paint ────────────────────────────────────────────────────────

void ImageView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // Background
    painter.fillRect(rect(), QColor(40, 40, 40));

    if (m_image.isNull()) {
        painter.setPen(QColor(128, 128, 128));
        painter.drawText(rect(), Qt::AlignCenter, "无图像信号");
        return;
    }

    // Draw image centered with zoom & pan
    QRectF vp = getImageViewport();
    QPointF imgCenter(m_image.width() / 2.0 + m_panX, m_image.height() / 2.0 + m_panY);
    QPointF viewCenter = vp.center();
    QPointF topLeft = viewCenter - QPointF(imgCenter.x() * m_zoom, imgCenter.y() * m_zoom);
    QRectF targetRect(topLeft, QSizeF(m_image.width() * m_zoom, m_image.height() * m_zoom));

    if (m_dirtyCache) {
        m_cachedPixmap = QPixmap::fromImage(m_image);
        m_dirtyCache = false;
    }
    painter.drawPixmap(targetRect, m_cachedPixmap, QRectF(m_image.rect()));

    // Draw overlays
    drawOverlays(painter);
}

// ── Overlay drawing ──────────────────────────────────────────────

void ImageView::drawOverlays(QPainter &painter)
{
    // Rendering order (bottom → top):
    //   1. detection (template match results)
    //   2. calibration (calib board corners)
    //   3. measurement (user's current focus — must stay on top of detection/calib)
    //   4. interactive ROI selection rectangle (active drag — above everything)

    // ── 1. Detection ──
    for (const auto &d : m_detections) {
        QColor color = d.valid ? QColor(0, 255, 0) : QColor(255, 0, 0);
        drawOrientedBox(painter, d.center, d.angle, 80, 60, color);

        QPointF labelPos = imageToView(d.center + QPointF(0, -45));
        painter.setPen(color);
        painter.drawText(labelPos, QString::number(d.score, 'f', 3));
    }

    // ── 2. Calibration corners ──
    painter.setPen(QPen(Qt::yellow, 2));
    for (const auto &pt : m_calibCorners) {
        QPointF vp = imageToView(pt);
        painter.drawEllipse(vp, 2, 2);
    }

    // ── 3. Measurement overlays (cyan) ──
    const QColor measureColor(0, 200, 255);
    QFont labelFont = painter.font();
    labelFont.setBold(true);
    painter.setFont(labelFont);

    auto markCross = [&](const QPointF &p, double r) {
        painter.drawLine(p + QPointF(-r, 0), p + QPointF(r, 0));
        painter.drawLine(p + QPointF(0, -r), p + QPointF(0, r));
    };

    // 3a. Cross markers (AREA & legacy callers)
    painter.setPen(QPen(measureColor, 2));
    for (const auto &m : m_measurements) {
        QPointF vp = imageToView(m.pos);
        markCross(vp, 6);
        if (!m.label.isEmpty()) {
            painter.drawText(vp + QPointF(8, -8), m.label);
        }
    }

    // 3b. Line segments (LENGTH / DISTANCE)
    painter.setPen(QPen(measureColor, 2));
    for (const auto &lm : m_lineMeasurements) {
        QPointF v1 = imageToView(lm.p1);
        QPointF v2 = imageToView(lm.p2);
        painter.drawLine(v1, v2);
        markCross(v1, 4);
        markCross(v2, 4);
        if (!lm.label.isEmpty()) {
            QPointF mid = (v1 + v2) / 2.0;
            painter.drawText(mid + QPointF(8, -8), lm.label);
        }
    }

    // 3c. Circles (CIRCLE)
    painter.setPen(QPen(measureColor, 2));
    painter.setBrush(Qt::NoBrush);
    for (const auto &cm : m_circleMeasurements) {
        QPointF vc = imageToView(cm.center);
        double  vr = cm.radius * m_zoom;
        painter.drawEllipse(vc, vr, vr);
        markCross(vc, 4);
        if (!cm.label.isEmpty()) {
            painter.drawText(vc + QPointF(vr + 4, -8), cm.label);
        }
    }

    // 3d. Angle (two line segments)
    painter.setPen(QPen(measureColor, 2));
    for (const auto &am : m_angleMeasurements) {
        QPointF v1a = imageToView(am.l1Start);
        QPointF v1b = imageToView(am.l1End);
        QPointF v2a = imageToView(am.l2Start);
        QPointF v2b = imageToView(am.l2End);
        painter.drawLine(v1a, v1b);
        painter.drawLine(v2a, v2b);
        markCross(v1a, 4); markCross(v1b, 4);
        markCross(v2a, 4); markCross(v2b, 4);
        if (!am.label.isEmpty()) {
            painter.drawText(v1a + QPointF(8, -8), am.label);
        }
    }

    // 3e. Clicked-point markers (transient — visible only during input collection;
    //     MainWindow clears them before the final measurement overlay is drawn).
    for (const auto &cp : m_clickedPoints) {
        QPointF vp = imageToView(cp.pos);
        painter.setPen(QPen(cp.color, 2));
        markCross(vp, 6);
        if (!cp.label.isEmpty()) {
            painter.drawText(vp + QPointF(8, -8), cp.label);
        }
    }

    // ── 4. Interactive ROI selection (topmost) ──
    if (m_selecting) {
        painter.setPen(QPen(QColor(0, 160, 255), 1.5, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(m_roiStart, m_roiEnd));
    }
}

void ImageView::drawOrientedBox(QPainter &painter, const QPointF &center,
                                 double angle, double w, double h, const QColor &color)
{
    painter.save();
    QPointF vc = imageToView(center);
    painter.translate(vc);
    painter.rotate(angle);

    double sw = w * m_zoom;
    double sh = h * m_zoom;
    QPen pen(color, 2);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(-sw / 2, -sh / 2, sw, sh));

    // Direction indicator
    painter.drawLine(QPointF(0, 0), QPointF(sw / 2 + 10, 0));
    painter.restore();
}

// ── Mouse events ─────────────────────────────────────────────────

void ImageView::mousePressEvent(QMouseEvent *event)
{
    if (m_image.isNull()) return;

    if (event->button() == Qt::RightButton) {
        // Pan mode
        m_panning = true;
        m_lastMousePos = event->position();
        setCursor(Qt::ClosedHandCursor);
    } else if (event->button() == Qt::LeftButton) {
        // ROI selection
        m_selecting = true;
        m_roiStart = event->position();
        m_roiEnd   = event->position();
    }
}

void ImageView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        QPointF delta = event->position() - m_lastMousePos;
        m_panX += delta.x() / m_zoom;
        m_panY += delta.y() / m_zoom;
        m_lastMousePos = event->position();
        m_dirtyCache = true;
        update();
    }

    if (m_selecting) {
        m_roiEnd = event->position();
        update();
    }

    QPointF imgPos = viewToImage(event->position());
    emit mousePositionChanged(imgPos);
}

void ImageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton && m_panning) {
        m_panning = false;
        setCursor(Qt::ArrowCursor);
    }

    if (event->button() == Qt::LeftButton) {
        if (m_selecting) {
            m_selecting = false;
            setCursor(Qt::ArrowCursor);

            QRectF roi = QRectF(m_roiStart, m_roiEnd).normalized();
            if (roi.width() > 10 && roi.height() > 10) {
                QPointF p1 = viewToImage(roi.topLeft());
                QPointF p2 = viewToImage(roi.bottomRight());
                emit roiSelected(QRectF(p1, p2));
                update();
                return;   // Don't also emit pointPicked — the user clearly dragged a ROI.
            }
            update();
        }

        if (m_interactionMode == InteractionMode::POINT_PICK) {
            QPointF imgPos = viewToImage(event->position());
            emit pointPicked(imgPos);
        }
    }
}

void ImageView::wheelEvent(QWheelEvent *event)
{
    if (m_image.isNull()) return;

    double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    setZoom(m_zoom * factor);
}

void ImageView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_dirtyCache = true;
}
