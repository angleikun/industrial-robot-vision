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
    clearOverlays();
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

void ImageView::addCalibOverlay(const QList<QPointF> &corners)
{
    m_calibCorners = corners;
    update();
}

void ImageView::clearOverlays()
{
    m_detections.clear();
    m_measurements.clear();
    m_calibCorners.clear();
    update();
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
    for (const auto &d : m_detections) {
        QColor color = d.valid ? QColor(0, 255, 0) : QColor(255, 0, 0);
        drawOrientedBox(painter, d.center, d.angle, 80, 60, color);

        // Score label
        QPointF labelPos = imageToView(d.center + QPointF(0, -45));
        painter.setPen(color);
        painter.drawText(labelPos, QString::number(d.score, 'f', 3));
    }

    // Draw ROI selection rectangle
    if (m_selecting) {
        painter.setPen(QPen(QColor(0, 160, 255), 1.5, Qt::DashLine));
        painter.drawRect(QRectF(m_roiStart, m_roiEnd));
    }

    // Draw calib corners
    painter.setPen(QPen(Qt::yellow, 2));
    for (const auto &pt : m_calibCorners) {
        QPointF vp = imageToView(pt);
        painter.drawEllipse(vp, 2, 2);
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
