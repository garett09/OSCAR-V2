/* Clock Drift History Dialog Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef CLOCKDRIFTHISTORYDIALOG_H
#define CLOCKDRIFTHISTORYDIALOG_H

#include <QDialog>
#include <QDate>
#include "SleepLib/machine.h"
#include "database/night_clock_offsets_repository.h"
#include "database/device_time_correction_repository.h"

namespace Ui { class ClockDriftHistoryDialog; }

class ClockDriftHistoryDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ClockDriftHistoryDialog(Machine* cpap, Machine* oxi, QWidget* parent = nullptr);
    ~ClockDriftHistoryDialog();

private slots:
    void onReset();
    void onExport();
    void onShowUndoneToggled(bool checked);
    void refresh();

private:
    Ui::ClockDriftHistoryDialog *ui;
    Machine* m_cpap;
    Machine* m_oxi;

    void populateNightlyTable(const QList<NightClockOffsetData>& rows);
    void populateCorrectionsTable(const QList<DeviceTimeCorrectionData>& rows);
    void populateChart(const QList<NightClockOffsetData>& nights,
                       const QList<DeviceTimeCorrectionData>& corrections);
    QString formatCorrectionValue(const DeviceTimeCorrectionData& r) const;
};

#endif // CLOCKDRIFTHISTORYDIALOG_H
