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
#include "clockdrifthistorydialog.h"
#include "SleepLib/profiles.h"
#include "SleepLib/machine_common.h"
#include "database/device_time_correction_repository.h"
#include <QCheckBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QStringList>
#include <QTableWidgetItem>
#include <QEventLoop>

ClockSyncDialog::ClockSyncDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ClockSyncDialog)
{
    ui->setupUi(this);
    connect(ui->btnAnalyze,   &QPushButton::clicked, this, &ClockSyncDialog::onAnalyze);
    connect(ui->btnApply,     &QPushButton::clicked, this, &ClockSyncDialog::onApply);
    connect(ui->btnFirstData, &QPushButton::clicked, this, &ClockSyncDialog::onFirstData);
    connect(ui->btnLastData,  &QPushButton::clicked, this, &ClockSyncDialog::onLastData);
    connect(ui->btnHistory,   &QPushButton::clicked, this, &ClockSyncDialog::onHistory);

    connect(ui->startDate, &QDateEdit::dateChanged, this, &ClockSyncDialog::invalidateAnalysis);
    connect(ui->endDate, &QDateEdit::dateChanged, this, &ClockSyncDialog::invalidateAnalysis);
    connect(ui->cpapCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ClockSyncDialog::invalidateAnalysis);
    connect(ui->oxiCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ClockSyncDialog::invalidateAnalysis);
    connect(ui->chkValidate, &QCheckBox::toggled, this, &ClockSyncDialog::invalidateAnalysis);
    ui->btnApply->setEnabled(false);

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
    invalidateAnalysis();
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
    invalidateAnalysis();
    Machine* cpap = currentCpap();
    Machine* oxi  = currentOxi();
    if (!cpap) { ui->statusLabel->setText(tr("No CPAP device selected.")); return; }
    if (!oxi)  { ui->statusLabel->setText(tr("No reference device selected.")); return; }

    QDate start = ui->startDate->date();
    QDate end   = ui->endDate->date();
    if (!start.isValid() || !end.isValid() || start > end) {
        ui->statusLabel->setText(tr("Invalid date range."));
        return;
    }

    ui->statusLabel->setText(tr("Analyzing…"));
    // Keep painting during expensive event loading without allowing user input
    // to change the devices/range or re-enter this dialog during analysis.
    setEnabled(false);
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);

    // PRIMARY: constant offset from recording-window differences (all paired nights).
    m_nights = ClockSyncAnalyzer::collectStartDiffs(cpap, oxi, start, end);
    m_fit    = ClockSyncAnalyzer::fitConstantOffset(m_nights);

    // OPTIONAL: event cross-correlation sanity check on the nights kept by the fit.
    if (ui->chkValidate->isChecked()) {
        for (NightStartDiff& n : m_nights) {
            if (!n.used) continue;
            ClockSyncAnalyzer::validateNight(cpap, oxi, n);
            qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    }

    setEnabled(true);
    populateTable();
    m_hasAnalysis = true;

    // Persist nightly measurements for history view
    if (cpap && oxi)
        m_nightRepo.upsertNights(cpap->getDatabaseId(), oxi->getDatabaseId(), m_nights);

    refreshSelection();
}

void ClockSyncDialog::invalidateAnalysis()
{
    m_hasAnalysis = false;
    m_nights.clear();
    m_fit = ConstantOffsetFit();
    ui->resultsTable->setRowCount(0);
    ui->btnApply->setEnabled(false);
    ui->statusLabel->setText(tr("Inputs changed. Click Analyze Nights to calculate a new estimate."));
}

QVector<NightStartDiff> ClockSyncDialog::selectedNights() const
{
    QVector<NightStartDiff> selected;
    for (int row = 0; row < m_nights.size(); ++row) {
        QWidget* w = ui->resultsTable->cellWidget(row, 0);
        QCheckBox* cb = w ? w->findChild<QCheckBox*>() : nullptr;
        if (cb && cb->isChecked()) selected.append(m_nights[row]);
    }
    return selected;
}

void ClockSyncDialog::refreshSelection()
{
    if (!m_hasAnalysis) return;
    // Checkbox state is the explicit selection; preserve the initial outlier
    // classification only as context, not as a stale claim of exclusion.
    for (int row = 0; row < m_nights.size(); ++row) {
        auto* w = ui->resultsTable->cellWidget(row, 0);
        auto* cb = w ? w->findChild<QCheckBox*>() : nullptr;
        const bool included = cb && cb->isChecked();
        const auto& n = m_nights[row];
        QString note;
        if (!included) note = tr("excluded (not selected)");
        else if (!n.used) note = tr("included (initial pairing outlier)");
        if (included && n.validated && !n.validatorAgrees) {
            if (!note.isEmpty()) note += " — ";
            note += tr("event cross-check disagrees — possible session mismatch");
        }
        ui->resultsTable->item(row, 6)->setText(note);
        for (int col = 1; col <= 6; ++col)
            ui->resultsTable->item(row, col)->setForeground(included ? QBrush() : QBrush(Qt::gray));
    }
    auto selected = selectedNights();
    m_fit = ClockSyncAnalyzer::fitConstantOffset(selected, false);
    ui->btnApply->setEnabled(m_fit.valid);
    if (!m_fit.valid) {
        ui->statusLabel->setText(tr("Need at least %1 checked paired nights; currently %2.")
            .arg(ClockSyncAnalyzer::MIN_OFFSET_NIGHTS).arg(selected.size()));
        return;
    }
    QString msg = tr("Estimated constant offset: %1 s (median of %2 checked nights). "
                     "Night-to-night spread (MAD): %3 s; this is not an accuracy bound. "
                     "Recording start/stop habits can bias the estimate. "
                     "Save adds this amount to CPAP timestamps from the first checked night onward.")
                     .arg(m_fit.offsetMs / 1000.0, 0, 'f', 1).arg(m_fit.nUsed)
                     .arg(m_fit.madMs / 1000.0, 0, 'f', 1);
    if (m_fit.unstable)
        msg += " " + tr("The range is unstable; verify alignment before saving.");
    if (qAbs(m_fit.startEndSpreadMs) > 90000)
        msg += " " + tr("Start/end differences disagree by %1 s.")
            .arg(m_fit.startEndSpreadMs / 1000.0, 0, 'f', 1);
    const auto trend = ClockSyncAnalyzer::fitDriftLine(selected);
    if (trend.valid && trend.significant)
        msg += " " + tr("Recording windows show a trend; this does not establish clock drift. "
                         "No drift model or extrapolation will be saved.");
    int validated = 0, disagreed = 0;
    for (const auto& n : selected) {
        if (n.validated) { ++validated; if (!n.validatorAgrees) ++disagreed; }
    }
    msg += " " + tr("Event cross-check: %1 validated, %2 disagree. "
                     "Check alignment independently before using this estimate.")
                     .arg(validated).arg(disagreed);
    ui->statusLabel->setText(msg);
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
        connect(cb, &QCheckBox::toggled, this, &ClockSyncDialog::refreshSelection);

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

void ClockSyncDialog::onFirstData()
{
    Machine* cpap = currentCpap();
    Machine* oxi  = currentOxi();
    QDate first;
    if (cpap && cpap->FirstDay().isValid())
        first = cpap->FirstDay();
    if (oxi && oxi->FirstDay().isValid())
        first = first.isValid() ? std::min(first, oxi->FirstDay()) : oxi->FirstDay();
    if (first.isValid())
        ui->startDate->setDate(first);
}

void ClockSyncDialog::onLastData()
{
    Machine* cpap = currentCpap();
    Machine* oxi  = currentOxi();
    QDate last;
    if (cpap && cpap->LastDay().isValid())
        last = cpap->LastDay();
    if (oxi && oxi->LastDay().isValid())
        last = last.isValid() ? std::max(last, oxi->LastDay()) : oxi->LastDay();
    if (last.isValid())
        ui->endDate->setDate(last);
}

void ClockSyncDialog::onApply()
{
    if (!m_hasAnalysis) return;
    Machine* cpap = currentCpap();
    if (!cpap || cpap->getDatabaseId() <= 0) return;
    refreshSelection();
    if (!m_fit.valid) return;
    const auto checked = selectedNights();
    const QDate from = checked.first().date;

    QString message = tr("Save a constant correction of %1 seconds, added to CPAP timestamps "
                         "from %2 onward? Earlier corrections will be preserved.\n\n"
                         "This estimate uses recording windows, which can include differences "
                         "in when you start and stop each device. The reported spread is not "
                         "proof of clock accuracy. Verify alignment independently.")
                         .arg(m_fit.offsetMs / 1000.0, 0, 'f', 1)
                         .arg(from.toString(Qt::ISODate));
    if (m_fit.unstable)
        message += "\n\n" + tr("Warning: the selected range is unstable.");
    for (const auto& n : checked) {
        if (n.validated && !n.validatorAgrees) {
            message += "\n\n" + tr("Warning: event cross-check disagrees on selected nights.");
            break;
        }
    }
    if (QMessageBox::question(this, tr("Save constant clock correction"), message,
                             QMessageBox::Save | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Save) return;

    DeviceTimeCorrectionRepository repo;
    if (!repo.replaceWithConstant(cpap->getDatabaseId(), from.toString(Qt::ISODate), m_fit.offsetMs)) {
        ui->statusLabel->setText(tr("Save failed. Previous corrections were preserved."));
        return;
    }
    Machine::reloadCorrectionsFromDb(cpap);
    ui->statusLabel->setText(tr("Saved constant offset %1 s to %2, from %3 onward.")
        .arg(m_fit.offsetMs / 1000.0, 0, 'f', 1)
        .arg(ui->cpapCombo->currentText()).arg(from.toString(Qt::ISODate)));
    ui->btnApply->setEnabled(false);
    m_hasAnalysis = false;
    emit correctionsChanged();
}

void ClockSyncDialog::onHistory()
{
    Machine* cpap = currentCpap();
    Machine* oxi  = currentOxi();
    if (!cpap || !oxi) {
        ui->statusLabel->setText(tr("Select both devices before opening history."));
        return;
    }
    ClockDriftHistoryDialog dlg(cpap, oxi, this);
    dlg.exec();
}
