/* Clock Sync Dialog Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "clocksyncdialog.h"
#include "ui_clocksyncdialog.h"
#include "clocksyncanalyzer.h"
#include "SleepLib/profiles.h"
#include "SleepLib/machine_common.h"
#include "database/device_time_correction_repository.h"
#include <QCheckBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QTableWidgetItem>

ClockSyncDialog::ClockSyncDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ClockSyncDialog)
{
    ui->setupUi(this);
    connect(ui->btnAnalyze, &QPushButton::clicked, this, &ClockSyncDialog::onAnalyze);
    connect(ui->btnApply,   &QPushButton::clicked, this, &ClockSyncDialog::onApply);

    ui->resultsTable->horizontalHeader()->setStretchLastSection(true);
    ui->resultsTable->verticalHeader()->setVisible(false);
    ui->resultsTable->setColumnWidth(0, 40);
    ui->resultsTable->setColumnWidth(1, 90);
    ui->resultsTable->setColumnWidth(2, 100);
    ui->resultsTable->setColumnWidth(3, 90);
    ui->resultsTable->setColumnWidth(4, 90);
    ui->resultsTable->setColumnWidth(5, 80);
}

ClockSyncDialog::~ClockSyncDialog()
{
    delete ui;
}

void ClockSyncDialog::setDate(const QDate& date)
{
    if (date.isValid()) {
        ui->endDate->setDate(date);
        ui->startDate->setDate(date.addDays(-30));
    }
    populateCpapCombo();
    populateOxiCombo();
}

void ClockSyncDialog::populateCpapCombo()
{
    if (!p_profile) return;
    ui->cpapCombo->blockSignals(true);
    ui->cpapCombo->clear();
    for (Machine* m : p_profile->GetMachines()) {
        if (m->type() != MT_CPAP) continue;
        QString label = m->brand() + " " + m->model();
        if (label.trimmed().isEmpty()) label = m->loaderName();
        label += " (" + m->serial() + ")";
        ui->cpapCombo->addItem(label, QVariant::fromValue(reinterpret_cast<quintptr>(m)));
    }
    ui->cpapCombo->blockSignals(false);
}

void ClockSyncDialog::populateOxiCombo()
{
    if (!p_profile) return;
    ui->oxiCombo->blockSignals(true);
    ui->oxiCombo->clear();
    for (Machine* m : p_profile->GetMachines()) {
        if (m->type() == MT_CPAP) continue;
        if (!Machine::isCorrectableType(m->type())) continue;
        QString label = m->brand() + " " + m->model();
        if (label.trimmed().isEmpty()) label = m->loaderName();
        label += " (" + m->serial() + ")";
        ui->oxiCombo->addItem(label, QVariant::fromValue(reinterpret_cast<quintptr>(m)));
    }
    ui->oxiCombo->blockSignals(false);
}

Machine* ClockSyncDialog::currentCpap() const
{
    int idx = ui->cpapCombo->currentIndex();
    if (idx < 0) return nullptr;
    quintptr ptr = ui->cpapCombo->itemData(idx).value<quintptr>();
    return ptr ? reinterpret_cast<Machine*>(ptr) : nullptr;
}

Machine* ClockSyncDialog::currentOxi() const
{
    int idx = ui->oxiCombo->currentIndex();
    if (idx < 0) return nullptr;
    quintptr ptr = ui->oxiCombo->itemData(idx).value<quintptr>();
    return ptr ? reinterpret_cast<Machine*>(ptr) : nullptr;
}

void ClockSyncDialog::onAnalyze()
{
    Machine* cpap = currentCpap();
    Machine* oxi  = currentOxi();
    if (!cpap) { ui->statusLabel->setText(tr("No CPAP device selected.")); return; }
    if (!oxi)  { ui->statusLabel->setText(tr("No reference device selected.")); return; }

    QDate start = ui->startDate->date();
    QDate end   = ui->endDate->date();
    if (!start.isValid() || !end.isValid() || start >= end) {
        ui->statusLabel->setText(tr("Invalid date range."));
        return;
    }

    ui->statusLabel->setText(tr("Analyzing…"));
    ui->btnAnalyze->setEnabled(false);
    qApp->processEvents();

    m_results = ClockSyncAnalyzer::analyze(cpap, oxi, start, end, &m_fit);

    ui->btnAnalyze->setEnabled(true);
    populateTable();

    int accepted = 0, outliers = 0;
    for (const auto& r : m_results) {
        if (r.accepted) accepted++;
        if (r.driftOutlier) outliers++;
    }
    QString msg = tr("%1 nights analyzed, %2 accepted (z ≥ %3).")
                      .arg(m_results.size())
                      .arg(accepted)
                      .arg(ClockSyncAnalyzer::MIN_ZSCORE, 0, 'f', 1);
    if (m_fit.valid) {
        msg += " " + tr("Drift fit: %1 s/day over %2 nights.")
                         .arg(m_fit.slopeMsPerDay / 1000.0, 0, 'f', 2)
                         .arg(m_fit.nUsed);
        if (outliers > 0)
            msg += " " + tr("%1 outlier night(s) auto-unchecked.").arg(outliers);
    }
    msg += " " + tr("Uncheck any you want to skip, then click Save.");
    ui->statusLabel->setText(msg);
    ui->btnApply->setEnabled(accepted > 0);
}

void ClockSyncDialog::populateTable()
{
    QTableWidget* t = ui->resultsTable;
    t->setRowCount(0);
    t->setRowCount(m_results.size());

    for (int row = 0; row < m_results.size(); ++row) {
        const NightSyncResult& r = m_results[row];

        // Checkbox column
        QWidget* checkWidget = new QWidget();
        QCheckBox* cb = new QCheckBox();
        cb->setChecked(r.accepted && !r.driftOutlier);
        cb->setEnabled(r.accepted);
        QHBoxLayout* lay = new QHBoxLayout(checkWidget);
        lay->addWidget(cb);
        lay->setAlignment(Qt::AlignCenter);
        lay->setContentsMargins(0,0,0,0);
        checkWidget->setLayout(lay);
        t->setCellWidget(row, 0, checkWidget);

        auto item = [](const QString& s) {
            QTableWidgetItem* i = new QTableWidgetItem(s);
            i->setFlags(Qt::ItemIsEnabled);
            return i;
        };

        t->setItem(row, 1, item(r.date.toString("yyyy-MM-dd")));
        t->setItem(row, 2, item(r.accepted ? QString::number(r.offsetMs) : QString("-")));
        t->setItem(row, 3, item(r.accepted || r.zscore > 0
                                ? tr("z=%1").arg(r.zscore, 0, 'f', 1) : QString("-")));
        t->setItem(row, 4, item(QString::number(r.cpapEvents)));
        t->setItem(row, 5, item(QString::number(r.oxiDrops)));
        t->setItem(row, 6, item(r.note));

        if (!r.accepted) {
            for (int col = 1; col <= 6; ++col) {
                if (t->item(row, col))
                    t->item(row, col)->setForeground(Qt::gray);
            }
        }
    }
    t->resizeRowsToContents();
}

void ClockSyncDialog::onApply()
{
    Machine* oxi = currentOxi();
    if (!oxi || oxi->getDatabaseId() <= 0) return;

    QTableWidget* t = ui->resultsTable;
    DeviceTimeCorrectionRepository repo;
    int count = 0;

    for (int row = 0; row < m_results.size(); ++row) {
        const NightSyncResult& r = m_results[row];
        if (!r.accepted) continue;
        // Check if user left the checkbox ticked
        QWidget* w = t->cellWidget(row, 0);
        if (!w) continue;
        QCheckBox* cb = w->findChild<QCheckBox*>();
        if (!cb || !cb->isChecked()) continue;

        QString dateStr = r.date.toString(Qt::ISODate);
        repo.upsertTyped(oxi->getDatabaseId(), dateStr, "offset", r.offsetMs);
        ++count;
    }

    if (count == 0) {
        QMessageBox::information(this, tr("Auto-Detect CPAP Clock Offset"),
                                 tr("No nights were checked."));
        return;
    }

    Machine::reloadCorrectionsFromDb(oxi);
    ui->statusLabel->setText(tr("%1 offset entr%2 saved to %3. "
                                "Open Drift Analysis to fit a model.")
                                 .arg(count)
                                 .arg(count == 1 ? "y" : "ies")
                                 .arg(ui->oxiCombo->currentText()));
    ui->btnApply->setEnabled(false);
    emit correctionsChanged();
}
