#ifndef VISIONPROCESSOR_H
#define VISIONPROCESSOR_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QQueue>
#include <QString>
#include <QRectF>
#include <atomic>

#include "HalconCpp.h"

enum class VisionError {
    OK,
    NO_LICENSE,
    INVALID_IMAGE,
    NO_TEMPLATE,
    LOW_CONTRAST,
    OUT_OF_MEMORY,
    UNKNOWN
};

struct MatchResult {
    double x       = 0.0;
    double y       = 0.0;
    double angle   = 0.0;    // degrees
    double score   = 0.0;
    bool   valid   = false;
    int    targetId = -1;
};

struct MatchConfig {
    double minScore    = 0.7;
    int    maxTargets  = 16;
    double angleStart  = -180.0;
    double angleExtent = 360.0;
    double minScale    = 0.7;
    double maxScale    = 1.5;
};

class VisionProcessor : public QObject {
    Q_OBJECT

public:
    explicit VisionProcessor(QObject *parent = nullptr);
    ~VisionProcessor() override;

    // Template management.
    // These touch the HALCON model and must execute in the thread that owns
    // this object (the vision thread). They are Q_INVOKABLE so the GUI can
    // call them via QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)
    // instead of calling them directly across threads.
    Q_INVOKABLE bool createTemplate(const QImage &image, const QRectF &roi = QRectF());
    Q_INVOKABLE bool loadTemplate(const QString &filePath);
    Q_INVOKABLE bool saveTemplate(const QString &filePath);
    bool isTemplateLoaded() const;
    Q_INVOKABLE void clearTemplate();

    // Matching
    QList<MatchResult> findObject(const QImage &image);

public slots:
    void processImage(const QImage &image);

    // Configuration
    void setMatchConfig(const MatchConfig &cfg);
    MatchConfig matchConfig() const;

signals:
    void matchingComplete(const QList<MatchResult> &results);
    void processingError(VisionError code, const QString &detail);
    void templateCreated(bool success);

private:
    HalconCpp::HImage qImageToHImage(const QImage &img);

    // Clears the model WITHOUT taking m_modelMutex. Call only while the mutex
    // is already held (e.g. from loadTemplate). The public clearTemplate()
    // locks and then delegates here. This avoids re-locking the non-recursive
    // mutex from the same thread, which previously deadlocked loadTemplate().
    void clearTemplateUnlocked();

    HalconCpp::HShapeModel m_shapeModel;
    std::atomic<bool> m_modelValid{false};

    // Guards against an unbounded backlog of queued processImage() calls:
    // if a frame is still being matched, newer frames are dropped instead of
    // piling up in the vision thread's event queue (which would otherwise
    // block createTemplate() on m_modelMutex and freeze the UI).
    std::atomic<bool> m_busy{false};

    MatchConfig m_config;
    mutable QMutex m_modelMutex;
};

#endif // VISIONPROCESSOR_H
