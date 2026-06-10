#ifndef MEASUREMENTENGINE_H
#define MEASUREMENTENGINE_H

#include <QObject>
#include <QString>
#include <QPointF>
#include <QRectF>

enum class MeasureType {
    LENGTH,
    CIRCLE,
    DISTANCE,
    ANGLE,
    AREA
};

struct MeasureResult {
    bool    valid = false;
    MeasureType type = MeasureType::LENGTH;
    double  value    = 0.0;
    QString unit;
    QPointF pt1, pt2;       // for distance / length
    QPointF center;         // for circle
    double  radius  = 0.0;   // for circle
    double  area    = 0.0;   // for area
    QString label;
};

struct MeasurementConfig {
    int minAreaPx = 100;   // measureArea 最小连通域面积（像素）
};

class MeasurementEngine : public QObject {
    Q_OBJECT

public:
    explicit MeasurementEngine(QObject *parent = nullptr);

    void setPixelEquivalent(double mmPerPixel);
    double pixelEquivalent() const;

    void setMeasurementConfig(const MeasurementConfig &cfg);
    MeasurementConfig measurementConfig() const;

    MeasureResult measure(const QImage &image, MeasureType type,
                          const QList<QPointF> &points = {},
                          const QRectF &roi = QRectF());

    // Individual measurement helpers
    MeasureResult measureLength(const QImage &image, const QRectF &roi);
    MeasureResult measureCircle(const QImage &image, const QRectF &roi);
    MeasureResult measureDistance(const QImage &image, const QPointF &p1, const QPointF &p2);
    MeasureResult measureAngle(const QImage &image, const QList<QPointF> &line1,
                              const QList<QPointF> &line2);
    MeasureResult measureArea(const QImage &image, const QRectF &roi);

signals:
    void measurementComplete(const MeasureResult &result);

private:
    double m_pixelEquivalentMm = 0.05;
    MeasurementConfig m_config;
};

#endif // MEASUREMENTENGINE_H
