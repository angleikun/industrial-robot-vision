#include "ReportManager.h"
#include "DatabaseManager.h"
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QDir>
#include <QApplication>
#include <QDebug>
#include <QPrinter>
#include <QPainter>
#include <QPageSize>
#include <QMarginsF>

ReportManager::ReportManager(QObject *parent)
    : QObject(parent)
{
    m_config.outputDir = QApplication::applicationDirPath() + "/exports";
    m_config.prefix    = "report";
}

void ReportManager::setExportConfig(const ExportConfig &cfg)
{
    m_config = cfg;
}

ExportConfig ReportManager::exportConfig() const
{
    return m_config;
}

// ── CSV Export (UTF-8 BOM) ───────────────────────────────────────

bool ReportManager::exportCSV(const QList<DetectionRecord> &detections,
                               const QString &filePath)
{
    return exportCSV(detections, QList<MeasurementRecord>(), filePath);
}

bool ReportManager::exportCSV(const QList<DetectionRecord> &detections,
                               const QList<MeasurementRecord> &measurements,
                               const QString &filePath)
{
    QString path = filePath.isEmpty() ? buildDefaultPath(".csv") : filePath;
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit exportError(path, "无法创建文件");
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "\xEF\xBB\xBF"; // UTF-8 BOM

    out << "ID,时间,X,Y,角度(°),得分,结果\n";

    for (int i = 0; i < detections.size(); i++) {
        const auto &d = detections[i];
        out << d.id << ","
            << d.timestamp.toString("yyyy-MM-dd hh:mm:ss") << ","
            << d.x << ","
            << d.y << ","
            << d.angle << ","
            << d.score << ","
            << (d.result.isEmpty() ? "PASS" : d.result) << "\n";

        if (i % 100 == 0) {
            emit exportProgress(i * 100 / detections.size(), "导出CSV中...");
        }
    }

    file.close();
    emit exportCompleted(path, detections.size());
    return true;
}

// ── Excel Export (placeholder) ────────────────────────────────────

bool ReportManager::exportExcel(const QList<DetectionRecord> &detections,
                                 const QString &filePath)
{
    QString path = filePath.isEmpty() ? buildDefaultPath(".xlsx") : filePath;
    Q_UNUSED(detections)
    emit exportError(path, "Excel 导出需 libxlsxwriter 或 QAxObject");
    return false;
}

// ── PDF Export (via QPrinter + QPainter) ──────────────────────────

bool ReportManager::exportPDF(const QList<DetectionRecord> &detections,
                               const QString &title,
                               const QString &filePath)
{
    QString path = filePath.isEmpty() ? buildDefaultPath(".pdf") : filePath;

    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(path);
    printer.setPageSize(QPageSize::A4);
    printer.setPageMargins(QMarginsF(20, 20, 20, 20), QPageLayout::Millimeter);

    QPainter painter;
    if (!painter.begin(&printer)) {
        emit exportError(path, "无法创建 PDF 文件");
        return false;
    }

    const int pageW = painter.viewport().width();

    // ── Title ──
    QFont titleFont("Microsoft YaHei", 18, QFont::Bold);
    painter.setFont(titleFont);
    painter.drawText(QRect(0, 0, pageW, 80), Qt::AlignCenter, title);

    // ── Table header ──
    QFont headerFont("Microsoft YaHei", 10, QFont::Bold);
    painter.setFont(headerFont);
    int y = 120;
    QStringList headers = {"ID", "时间", "X(mm)", "Y(mm)", "角度(°)", "得分", "结果"};
    int colWidths[] = {60, 180, 90, 90, 80, 80, 60};
    int x = 0;
    for (int i = 0; i < headers.size(); i++) {
        painter.drawText(QRect(x, y, colWidths[i], 30), Qt::AlignCenter, headers[i]);
        x += colWidths[i];
    }

    // ── Data rows ──
    QFont dataFont("Microsoft YaHei", 9);
    painter.setFont(dataFont);
    y += 40;
    int passCount = 0, failCount = 0;
    double scoreSum = 0;

    for (const auto &d : detections) {
        int pageH = painter.viewport().height();
        if (y > pageH - 80) {
            printer.newPage();
            y = 60;
        }
        x = 0;
        QStringList cells = {
            QString::number(d.id),
            d.timestamp.toString("yyyy-MM-dd hh:mm:ss"),
            QString::number(d.x, 'f', 2),
            QString::number(d.y, 'f', 2),
            QString::number(d.angle, 'f', 2),
            QString::number(d.score, 'f', 4),
            d.result
        };
        for (int i = 0; i < cells.size(); i++) {
            painter.drawText(QRect(x, y, colWidths[i], 25), Qt::AlignCenter, cells[i]);
            x += colWidths[i];
        }
        y += 25;
        if (d.result == "PASS") passCount++; else failCount++;
        scoreSum += d.score;
    }

    // ── Summary ──
    y += 40;
    QFont sumFont("Microsoft YaHei", 11, QFont::Bold);
    painter.setFont(sumFont);
    int total = detections.size();
    double avgScore = total ? scoreSum / total : 0;
    double yield = total ? passCount * 100.0 / total : 0;
    painter.drawText(QRect(0, y, pageW, 30),
        QString("统计: 总数 %1,  PASS %2,  FAIL %3,  平均得分 %4,  良率 %5%")
            .arg(total).arg(passCount).arg(failCount)
            .arg(avgScore, 0, 'f', 4)
            .arg(yield, 0, 'f', 1));

    painter.end();
    emit exportCompleted(path, detections.size());
    return true;
}

// ── Utility ──────────────────────────────────────────────────────

QString ReportManager::generateTimestampedFilename(const QString &extension) const
{
    return m_config.outputDir + "/" + m_config.prefix + "_"
           + currentTimestamp() + extension;
}

QString ReportManager::currentTimestamp()
{
    return QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
}

QString ReportManager::buildDefaultPath(const QString &ext) const
{
    return generateTimestampedFilename(ext);
}

QString ReportManager::escapeCsvField(const QString &field) const
{
    if (field.contains(',') || field.contains('"') || field.contains('\n')) {
        QString escaped = field;
        escaped.replace('"', "\"\"");
        return "\"" + escaped + "\"";
    }
    return field;
}
