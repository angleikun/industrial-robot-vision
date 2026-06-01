#ifndef REPORTMANAGER_H
#define REPORTMANAGER_H

#include <QObject>
#include <QString>
#include <QList>
#include <QDateTime>

struct DetectionRecord;
struct MeasurementRecord;
struct GrabRecord;

struct ExportConfig {
    QString outputDir;
    QString prefix;
    bool    includeImages  = false;
    bool    includeStats   = true;
};

class ReportManager : public QObject {
    Q_OBJECT

public:
    explicit ReportManager(QObject *parent = nullptr);

    void setExportConfig(const ExportConfig &cfg);
    ExportConfig exportConfig() const;

    // CSV export (UTF-8 BOM)
    bool exportCSV(const QList<DetectionRecord> &detections,
                   const QString &filePath = QString());

    bool exportCSV(const QList<DetectionRecord> &detections,
                   const QList<MeasurementRecord> &measurements,
                   const QString &filePath = QString());

    // Excel export (via libxlsxwriter or QAxObject)
    bool exportExcel(const QList<DetectionRecord> &detections,
                     const QString &filePath = QString());

    // PDF report with statistics and charts (via QPrinter / QPdfWriter)
    bool exportPDF(const QList<DetectionRecord> &detections,
                   const QString &title,
                   const QString &filePath = QString());

    // Utility
    QString generateTimestampedFilename(const QString &extension) const;
    static QString currentTimestamp();

signals:
    void exportProgress(int percent, const QString &status);
    void exportCompleted(const QString &filePath, int recordCount);
    void exportError(const QString &filePath, const QString &error);

private:
    QString buildDefaultPath(const QString &ext) const;
    bool writeCsvRow(void *file, const QStringList &cells);
    QString escapeCsvField(const QString &field) const;

    ExportConfig m_config;
};

#endif // REPORTMANAGER_H
