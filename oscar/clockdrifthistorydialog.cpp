/* Clock Drift History Dialog Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "clockdrifthistorydialog.h"
#include "ui_clockdrifthistorydialog.h"
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QFileDialog>
#include <QTextStream>
#include <QFile>
#include <QDateTime>

ClockDriftHistoryDialog::ClockDriftHistoryDialog(Machine* cpap, Machine* oxi, QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::ClockDriftHistoryDialog)
    , m_cpap(cpap)
    , m_oxi(oxi)
{
    ui->setupUi(this);

    // Device pair label
    auto label = [this](Machine* m) -> QString {
        if (!m) return tr("none");
        QString s = m->brand() + " " + m->model();
        if (s.trimmed().isEmpty()) s = m->loaderName();
        return s + " (" + m->serial() + ")";
    };
    ui->deviceLabel->setText(tr("CPAP: %1  |  Ref: %2").arg(label(cpap)).arg(label(oxi)));
    setWindowTitle(tr("Clock Drift History -- %1 vs %2").arg(label(cpap)).arg(label(oxi)));

    // Table setup
    ui->nightlyTable->horizontalHeader()->setStretchLastSection(true);
    ui->nightlyTable->verticalHeader()->setVisible(false);
    ui->nightlyTable->setColumnWidth(0, 90);
    ui->nightlyTable->setColumnWidth(1, 80);
    ui->nightlyTable->setColumnWidth(2, 80);
    ui->nightlyTable->setColumnWidth(3, 80);
    ui->nightlyTable->setColumnWidth(4, 60);

    ui->correctionsTable->horizontalHeader()->setStretchLastSection(true);
    ui->correctionsTable->verticalHeader()->setVisible(false);
    ui->correctionsTable->setColumnWidth(0, 90);
    ui->correctionsTable->setColumnWidth(1, 90);
    ui->correctionsTable->setColumnWidth(2, 70);
    ui->correctionsTable->setColumnWidth(3, 130);
    ui->correctionsTable->setColumnWidth(4, 140);

    // Splitter initial proportions (60/40)
    ui->splitter->setSizes({ 370, 250 });

    // Determine default date range from stored nights
    QDate fullFrom, fullTo = QDate::currentDate();
    if (cpap && oxi) {
        NightClockOffsetsRepository repo;
        auto all = repo.findByMachines(cpap->getDatabaseId(), oxi->getDatabaseId());
        for (const auto& n : all) {
            QDate d = QDate::fromString(n.date, Qt::ISODate);
            if (d.isValid()) {
                if (!fullFrom.isValid() || d < fullFrom) fullFrom = d;
            }
        }
    }
    if (!fullFrom.isValid()) fullFrom = QDate::currentDate().addDays(-30);

    // Block signals while setting initial values to avoid double refresh
    ui->fromDate->blockSignals(true);
    ui->toDate->blockSignals(true);
    ui->fromDate->setDate(fullFrom);
    ui->toDate->setDate(fullTo);
    ui->fromDate->blockSignals(false);
    ui->toDate->blockSignals(false);

    connect(ui->fromDate,      &QDateEdit::dateChanged,    this, &ClockDriftHistoryDialog::refresh);
    connect(ui->toDate,        &QDateEdit::dateChanged,    this, &ClockDriftHistoryDialog::refresh);
    connect(ui->btnReset,      &QPushButton::clicked,      this, &ClockDriftHistoryDialog::onReset);
    connect(ui->btnExport,     &QPushButton::clicked,      this, &ClockDriftHistoryDialog::onExport);
    connect(ui->chkShowUndone, &QCheckBox::toggled,        this, &ClockDriftHistoryDialog::onShowUndoneToggled);

    refresh();
}

ClockDriftHistoryDialog::~ClockDriftHistoryDialog()
{
    delete ui;
}

void ClockDriftHistoryDialog::onReset()
{
    if (!m_cpap || !m_oxi) return;
    NightClockOffsetsRepository repo;
    auto all = repo.findByMachines(m_cpap->getDatabaseId(), m_oxi->getDatabaseId());
    QDate fullFrom;
    for (const auto& n : all) {
        QDate d = QDate::fromString(n.date, Qt::ISODate);
        if (d.isValid() && (!fullFrom.isValid() || d < fullFrom)) fullFrom = d;
    }
    if (!fullFrom.isValid()) fullFrom = QDate::currentDate().addDays(-30);

    ui->fromDate->blockSignals(true);
    ui->toDate->blockSignals(true);
    ui->fromDate->setDate(fullFrom);
    ui->toDate->setDate(QDate::currentDate());
    ui->fromDate->blockSignals(false);
    ui->toDate->blockSignals(false);
    refresh();
}

void ClockDriftHistoryDialog::onShowUndoneToggled(bool /*checked*/)
{
    refresh();
}

void ClockDriftHistoryDialog::refresh()
{
    if (!m_cpap || !m_oxi) return;

    QDate from = ui->fromDate->date();
    QDate to   = ui->toDate->date();
    if (!from.isValid() || !to.isValid() || from > to) return;

    NightClockOffsetsRepository nightRepo;
    DeviceTimeCorrectionRepository corrRepo;

    auto nights = nightRepo.findByMachines(m_cpap->getDatabaseId(),
                                           m_oxi->getDatabaseId(), from, to);
    QList<DeviceTimeCorrectionData> corrections;
    if (ui->chkShowUndone->isChecked())
        corrections = corrRepo.findAllIncludingUndone(m_cpap->getDatabaseId());
    else
        corrections = corrRepo.findActive(m_cpap->getDatabaseId());

    populateNightlyTable(nights);
    populateCorrectionsTable(corrections);
    populateChart(nights, corrections);
}

void ClockDriftHistoryDialog::populateNightlyTable(const QList<NightClockOffsetData>& rows)
{
    auto item = [](const QString& s) {
        auto* i = new QTableWidgetItem(s);
        i->setFlags(Qt::ItemIsEnabled);
        return i;
    };
    auto secs = [](qint64 ms) { return QString::number(ms / 1000.0, 'f', 1); };

    QTableWidget* t = ui->nightlyTable;
    t->setRowCount(0);
    t->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const auto& n = rows[row];
        t->setItem(row, 0, item(n.date));
        t->setItem(row, 1, item(secs(n.startDiffMs)));
        t->setItem(row, 2, item(secs(n.endDiffMs)));
        t->setItem(row, 3, item(secs(n.midMs)));
        t->setItem(row, 4, item(n.used ? tr("yes") : tr("no (outlier)")));
        QString xcheck = tr("--");
        if (n.validated)
            xcheck = n.validatorAgrees == 1 ? tr("agrees") : tr("disagrees");
        t->setItem(row, 5, item(xcheck));
        if (!n.used)
            for (int col = 0; col < 6; ++col)
                if (t->item(row, col)) t->item(row, col)->setForeground(Qt::gray);
    }
    t->resizeRowsToContents();
}

void ClockDriftHistoryDialog::populateCorrectionsTable(const QList<DeviceTimeCorrectionData>& rows)
{
    auto item = [](const QString& s) {
        auto* i = new QTableWidgetItem(s);
        i->setFlags(Qt::ItemIsEnabled);
        return i;
    };

    QTableWidget* t = ui->correctionsTable;
    t->setRowCount(0);
    t->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const auto& r = rows[row];
        t->setItem(row, 0, item(r.dateFrom));
        t->setItem(row, 1, item(r.dateTo.isEmpty() ? tr("open") : r.dateTo));
        t->setItem(row, 2, item(r.type));
        t->setItem(row, 3, item(formatCorrectionValue(r)));
        // appliedAt stored as UTC datetime('now'); convert to local time for display
        QDateTime dt = QDateTime::fromString(r.appliedAt.left(19), "yyyy-MM-dd HH:mm:ss");
        dt.setTimeSpec(Qt::UTC);
        QString localTime = dt.isValid() ? dt.toLocalTime().toString("yyyy-MM-dd HH:mm") : r.appliedAt.left(16);
        t->setItem(row, 4, item(localTime));
        QTableWidgetItem* status;
        if (r.undoneAt.isEmpty()) {
            status = item(tr("active"));
            status->setForeground(QColor(0, 130, 0));
        } else {
            status = item(tr("superseded %1").arg(r.undoneAt.left(10)));
            status->setForeground(Qt::gray);
        }
        t->setItem(row, 5, status);
        if (!r.undoneAt.isEmpty())
            for (int col = 0; col < 5; ++col)
                if (t->item(row, col)) t->item(row, col)->setForeground(Qt::gray);
    }
    t->resizeRowsToContents();
}

void ClockDriftHistoryDialog::populateChart(const QList<NightClockOffsetData>& nights,
                                            const QList<DeviceTimeCorrectionData>& corrections)
{
    ui->plotWidget->clear();

    // Per-night dots: used=filled, outlier=hollow ref points
    QList<DriftPlotWidget::Point> pts;
    QList<DriftPlotWidget::Point> outlierPts;
    for (const auto& n : nights) {
        QDate d = QDate::fromString(n.date, Qt::ISODate);
        if (!d.isValid()) continue;
        if (n.used)
            pts.append({ d, (double)n.midMs });
        else
            outlierPts.append({ d, (double)n.midMs });
    }
    // setData/setRefPoints must be called before setCorrectionDates (early-return guard in DriftPlotWidget)
    ui->plotWidget->setData(pts);
    ui->plotWidget->setRefPoints(outlierPts);

    // Active drift/offset model -> red line
    // Most recent undone drift -> gray dashed
    const DeviceTimeCorrectionData* activeModel   = nullptr;
    const DeviceTimeCorrectionData* previousModel = nullptr;
    for (const auto& r : corrections) {
        if (r.type != "drift" && r.type != "offset") continue;
        if (r.undoneAt.isEmpty()) {
            if (!activeModel || r.dateFrom > activeModel->dateFrom)
                activeModel = &r;
        } else {
            if (!previousModel || r.dateFrom > previousModel->dateFrom)
                previousModel = &r;
        }
    }
    if (activeModel) {
        // slope stored as c1-1.0 in DriftPlotWidget
        double slope = (activeModel->type == "drift") ? (activeModel->c1 - 1.0) : 0.0;
        double c0    = (activeModel->type == "drift") ? (double)activeModel->c0Ms
                                                      : (double)activeModel->offsetMs;
        ui->plotWidget->setModel(c0, slope);
    }
    if (previousModel) {
        double slope = (previousModel->type == "drift") ? (previousModel->c1 - 1.0) : 0.0;
        double c0    = (previousModel->type == "drift") ? (double)previousModel->c0Ms
                                                       : (double)previousModel->offsetMs;
        ui->plotWidget->setReferenceModel(c0, slope);
    }

    // Correction markers
    QList<QDate> markerDates;
    for (const auto& r : corrections) {
        QDate d = QDate::fromString(r.dateFrom, Qt::ISODate);
        if (d.isValid()) markerDates.append(d);
    }
    ui->plotWidget->setCorrectionDates(markerDates);
}

QString ClockDriftHistoryDialog::formatCorrectionValue(const DeviceTimeCorrectionData& r) const
{
    if (r.type == "drift") {
        // m_slope = c1 - 1.0; s/day = (c1-1.0) * 86400
        double sPerDay = (r.c1 - 1.0) * 86400.0;
        return tr("%1 s/day from %2").arg(sPerDay, 0, 'f', 2).arg(r.dateFrom);
    }
    return tr("%1 s").arg(r.offsetMs / 1000.0, 0, 'f', 1);
}

void ClockDriftHistoryDialog::onExport()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Export CSV"),
                       QString(), tr("CSV files (*.csv)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream out(&f);

    int tabIdx = ui->tabWidget->currentIndex();
    QTableWidget* t = (tabIdx == 0) ? ui->nightlyTable : ui->correctionsTable;

    // Header row
    QStringList headers;
    for (int col = 0; col < t->columnCount(); ++col)
        headers << t->horizontalHeaderItem(col)->text();
    out << headers.join(",") << "\n";

    // Data rows
    for (int row = 0; row < t->rowCount(); ++row) {
        QStringList cells;
        for (int col = 0; col < t->columnCount(); ++col) {
            QTableWidgetItem* it = t->item(row, col);
            QString val = it ? it->text() : QString();
            // Quote cells that contain commas
            if (val.contains(',') || val.contains('"') || val.contains('\n')) {
                val.replace("\"", "\"\"");
                val = "\"" + val + "\"";
            }
            cells << val;
        }
        out << cells.join(",") << "\n";
    }
}
