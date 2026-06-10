#ifndef IMAGEVIEW_H
#define IMAGEVIEW_H

#include <QWidget>
#include <QPixmap>
#include <QPointF>
#include <QList>
#include <QCursor>

enum class InteractionMode { ROI, POINT_PICK };

class ImageView : public QWidget {
    Q_OBJECT

public:
    explicit ImageView(QWidget *parent = nullptr);

    void setImage(const QImage &image);
    void clearImage();
    QImage currentImage() const { return m_image; }
    void setInteractionMode(InteractionMode mode);
    InteractionMode interactionMode() const { return m_interactionMode; }

    // Overlay drawing
    void addDetectionOverlay(double x, double y, double angle, double score, bool valid);
    void addMeasurementOverlay(double x, double y, const QString &label);
    void addLineMeasurementOverlay(const QPointF &p1, const QPointF &p2, const QString &label);
    void addCircleMeasurementOverlay(const QPointF &center, double radius, const QString &label);
    void addAngleOverlay(const QPointF &line1Start, const QPointF &line1End,
                         const QPointF &line2Start, const QPointF &line2End,
                         const QString &label);
    void addCalibOverlay(const QList<QPointF> &corners);

    // Transient markers for the user's in-progress click input (DISTANCE / ANGLE).
    // Cleared by MainWindow once the full point set is collected.
    void addClickedPointMarker(const QPointF &pos, const QColor &color, const QString &label);
    void clearClickedPointMarkers();

    void clearDetectionOverlays();
    void clearMeasurementOverlays();
    void clearCalibrationOverlays();
    void clearAllOverlays();
    void clearOverlays();   // compat alias → clearAllOverlays()

    // View controls
    void fitToWindow();
    void setZoom(double factor);
    double zoom() const { return m_zoom; }

signals:
    void mousePositionChanged(const QPointF &imagePos);
    void roiSelected(const QRectF &roi);
    void pointPicked(const QPointF &imagePos);
    void zoomChanged(double factor);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QPointF  viewToImage(const QPointF &viewPos) const;
    QPointF  imageToView(const QPointF &imagePos) const;
    QRectF   getImageViewport() const;

    void drawOverlays(QPainter &painter);
    void drawRoi(QPainter &painter);
    void drawCrosshair(QPainter &painter, const QPointF &pos, const QColor &color);
    void drawOrientedBox(QPainter &painter, const QPointF &center, double angle,
                         double w, double h, const QColor &color);

    QImage  m_image;
    QPixmap m_cachedPixmap;
    double  m_zoom       = 1.0;
    double  m_panX       = 0.0;
    double  m_panY       = 0.0;

    InteractionMode m_interactionMode = InteractionMode::ROI;

    QPointF m_lastMousePos;
    bool    m_panning    = false;
    bool    m_selecting  = false;
    QPointF m_roiStart;
    QPointF m_roiEnd;

    // Overlay data
    struct DetectionItem {
        QPointF center;
        double  angle = 0.0;
        double  score = 0.0;
        bool    valid = false;
    };
    QList<DetectionItem> m_detections;

    struct MeasurementItem {
        QPointF pos;
        QString label;
    };
    QList<MeasurementItem> m_measurements;

    struct LineMeasurementItem {
        QPointF p1;
        QPointF p2;
        QString label;
    };
    QList<LineMeasurementItem> m_lineMeasurements;

    struct CircleMeasurementItem {
        QPointF center;
        double  radius = 0.0;
        QString label;
    };
    QList<CircleMeasurementItem> m_circleMeasurements;

    struct AngleMeasurementItem {
        QPointF l1Start, l1End;
        QPointF l2Start, l2End;
        QString label;
    };
    QList<AngleMeasurementItem> m_angleMeasurements;

    QList<QPointF> m_calibCorners;

    struct ClickedPointMarker {
        QPointF pos;
        QColor  color;
        QString label;
    };
    QList<ClickedPointMarker> m_clickedPoints;

    bool m_dirtyCache = true;
};

#endif // IMAGEVIEW_H
