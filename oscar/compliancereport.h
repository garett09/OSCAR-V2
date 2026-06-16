/* Compliance Report Exporter Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef COMPLIANCEREPORT_H
#define COMPLIANCEREPORT_H

#include <QDialog>
#include <QDate>
#include <QComboBox>
#include <QDateEdit>
#include <QLabel>
#include <QPushButton>

#include <QCoreApplication>
#include <QListWidget>
#include <QList>
#include <QVector>
#include <QByteArray>
#include <QTextFormat>
#include <QAbstractTextDocumentLayout>

class QPainter;
class QTextDocument;

struct SelectedChannel
{
    unsigned int id;
    QString label;
    int type; // MachineType
};

// Renders the report's charts as TRUE PDF vector graphics.
//
// Every chart (the four fixed charts plus each channel chart) is produced as an
// SVG. The old path rasterised each SVG to a PNG and embedded it as an <img>,
// which baked the chart into pixels at screen resolution — it blurred on zoom and
// never matched the crisp clinical reports it mimics (e.g. ResMed AirView).
//
// Instead, each SVG is registered here by index and a placeholder <img
// src="oscarchart:N"> is emitted into the report HTML. After QTextDocument::setHtml
// those placeholders are swapped for a custom text object of ChartObjectType.
// During QTextDocument::print() the layout calls drawObject() with the PRINTER's
// QPainter, and we replay the SVG straight onto it with QSvgRenderer — so the PDF
// stores vector paths and text, sharp at any zoom.
class ChartObjectHandler : public QObject, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    // Custom text-object type, plus the format properties carrying each chart's
    // registry index and on-page display size.
    enum {
        ChartObjectType    = QTextFormat::UserObject + 1,
        ChartIndexProperty = QTextFormat::UserProperty + 1,
        ChartSizeProperty  = QTextFormat::UserProperty + 2,
    };

    explicit ChartObjectHandler(const QVector<QByteArray>& svgList, QObject* parent = nullptr)
        : QObject(parent), m_svg(svgList) {}

    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument,
                         const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc,
                    int posInDocument, const QTextFormat& format) override;

private:
    QVector<QByteArray> m_svg;  // SVG bytes per chart, indexed by ChartIndexProperty
};

class ComplianceReportDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ComplianceReportDialog(QWidget *parent = nullptr);
    QDate startDate() const;
    QDate endDate() const;
    QDate lastSettingsChangeStartDate() const;
    QList<SelectedChannel> selectedChannels() const;

private slots:
    void onRangeChanged(int index);

private:
    QComboBox *m_rangeCombo;
    QDateEdit *m_startDateEdit;
    QDateEdit *m_endDateEdit;
    QLabel *m_startLabel;
    QLabel *m_endLabel;
    QListWidget *m_channelsListWidget;
};

class ComplianceReport
{
    Q_DECLARE_TR_FUNCTIONS(ComplianceReport)
public:
    static void exportReport(QWidget *parent);
};

#endif // COMPLIANCEREPORT_H
