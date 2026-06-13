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
    ui->resultsTable->setColumnWidth(2, 80);
    ui->resultsTable->setColumnWidth(3, 80);
    ui->resultsTable->setColumnWidth(4, 80);
    ui->resultsTable->setColumnWidth(5, 110);
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

    // PRIMARY: constant offset from recording-window differences (all paired nights).
    m_nights = ClockSyncAnalyzer::collectStartDiffs(cpap, oxi, start, end);
    m_fit    = ClockSyncAnalyzer::fitConstantOffset(m_nights);

    // OPTIONAL: event cross-correlation sanity check on the nights kept by the fit.
    if (ui->chkValidate->isChecked()) {
        for (NightStartDiff& n : m_nights) {
            if (!n.used) continue;
            ClockSyncAnalyzer::validateNight(cpap, oxi, n);
            qApp->processEvents();
        }
    }

    ui->btnAnalyze->setEnabled(true);
    populateTable();

    if (!m_fit.valid) {
        ui->statusLabel->setText(tr("Not enough paired nights (%1) — need at least %2 nights "
                                    "where both devices recorded.")
                                     .arg(m_nights.size())
                                     .arg(ClockSyncAnalyzer::MIN_OFFSET_NIGHTS));
        ui->btnApply->setEnabled(false);
        return;
    }

    QString msg = tr("Constant offset: %1 s (median of %2 nights, ± %3 s).")
                      .arg(m_fit.offsetMs / 1000.0, 0, 'f', 1)
                      .arg(m_fit.nUsed)
                      .arg(m_fit.madMs / 1000.0, 0, 'f', 1);
    if (m_fit.nOutliers > 0)
        msg += " " + tr("%1 outlier night(s) excluded.").arg(m_fit.nOutliers);
    if (qAbs(m_fit.startEndSpreadMs) > 90000)
        msg += " " + tr("Note: start/end differ by %1 s — some of this gap may be when you "
                        "start/stop each device, not clock offset.")
                         .arg(m_fit.startEndSpreadMs / 1000.0, 0, 'f', 0);
    msg += " " + tr("Saves one correction to the CPAP. Uncheck nights to exclude, then Save.");
    ui->statusLabel->setText(msg);
    ui->btnApply->setEnabled(true);
}

void ClockSyncDialog::populateTable()
{
    QTableWidget* t = ui->resultsTable;
    t->setRowCount(0);
    t->setRowCount(m_nights.size());

    auto item = [](const QString& s) {
        QTableWidgetItem* i = new QTableWidgetItem(s);
        i->setFlags(Qt::ItemIsEnabled);
        return i;
    };
    auto secs = [](qint64 ms) { return QString::number(ms / 1000.0, 'f', 1); };

    for (int row = 0; row < m_nights.size(); ++row) {
        const NightStartDiff& n = m_nights[row];

        QWidget* checkWidget = new QWidget();
        QCheckBox* cb = new QCheckBox();
        cb->setChecked(n.used);
        QHBoxLayout* lay = new QHBoxLayout(checkWidget);
        lay->addWidget(cb);
        lay->setAlignment(Qt::AlignCenter);
        lay->setContentsMargins(0,0,0,0);
        checkWidget->setLayout(lay);
        t->setCellWidget(row, 0, checkWidget);

        t->setItem(row, 1, item(n.date.toString("yyyy-MM-dd")));
        t->setItem(row, 2, item(secs(n.startDiffMs)));
        t->setItem(row, 3, item(secs(n.endDiffMs)));
        t->setItem(row, 4, item(secs(n.midMs)));

        QString xcheck = "—";
        if (n.validated) {
            xcheck = n.validatorAgrees
                ? tr("✓ z=%1").arg(n.validatorZ, 0, 'f', 1)
                : tr("✗ %1 s").arg(n.validatorOffsetMs / 1000.0, 0, 'f', 0);
        } else if (n.cpapEvents > 0 && n.cpapEvents < ClockSyncAnalyzer::MIN_EVENTS) {
            xcheck = tr("low AHI");
        }
        t->setItem(row, 5, item(xcheck));

        QString note;
        if (!n.used) note = tr("excluded (pairing outlier)");
        else if (n.validated && !n.validatorAgrees)
            note = tr("event cross-check disagrees — possible session mismatch");
        t->setItem(row, 6, item(note));

        if (!n.used) {
            for (int col = 1; col <= 6; ++col)
                if (t->item(row, col)) t->item(row, col)->setForeground(Qt::gray);
        }
    }
    t->resizeRowsToContents();
}

void ClockSyncDialog::onApply()
{
    Machine* cpap = currentCpap();
    if (!cpap || cpap->getDatabaseId() <= 0) return;

    // Recompute the constant over the nights the user left checked.
    QTableWidget* t = ui->resultsTable;
    QVector<NightStartDiff> checked;
    for (int row = 0; row < m_nights.size(); ++row) {
        QWidget* w = t->cellWidget(row, 0);
        if (!w) continue;
        QCheckBox* cb = w->findChild<QCheckBox*>();
        if (cb && cb->isChecked()) checked.append(m_nights[row]);
    }
    ConstantOffsetFit fit = ClockSyncAnalyzer::fitConstantOffset(checked);
    if (!fit.valid) {
        QMessageBox::information(this, tr("Auto-Detect CPAP Clock Offset"),
            tr("Need at least %1 checked nights to fit a constant offset.")
                .arg(ClockSyncAnalyzer::MIN_OFFSET_NIGHTS));
        return;
    }

    // The offset is applied to the CPAP (the drifting device) to move it onto the
    // reference clock — NOT to the reference, which the user trusts as correct.
    DeviceTimeCorrectionRepository repo;

    // Supersede ALL existing active corrections on the CPAP (every type feeds
    // Machine::correctionMs — non-drift rows add offset_ms, drift rows add a model),
    // including the legacy migrated clockDrift row. The measured constant is the full
    // offset, so any surviving row would double-correct.
    int superseded = 0;
    for (const auto& r : repo.findActive(cpap->getDatabaseId())) {
        if (repo.markUndone(r.id)) ++superseded;
    }

    QDate fromDate;
    for (const NightStartDiff& n : checked)
        if (n.used && (!fromDate.isValid() || n.date < fromDate)) fromDate = n.date;
    if (!fromDate.isValid()) fromDate = ui->startDate->date();

    repo.upsertTyped(cpap->getDatabaseId(), fromDate.toString(Qt::ISODate),
                     QString(), "offset", fit.offsetMs);

    Machine::reloadCorrectionsFromDb(cpap);
    ui->statusLabel->setText(tr("Saved constant offset %1 s to %2 (from %3 onward). "
                                "%4 prior correction(s) superseded.")
                                 .arg(fit.offsetMs / 1000.0, 0, 'f', 1)
                                 .arg(ui->cpapCombo->currentText())
                                 .arg(fromDate.toString(Qt::ISODate))
                                 .arg(superseded));
    ui->btnApply->setEnabled(false);
    emit correctionsChanged();
}
