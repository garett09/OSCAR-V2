/* Clock Sync Dialog Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef CLOCKSYNCDIALOG_H
#define CLOCKSYNCDIALOG_H

#include <QDialog>
#include <QDate>
#include <QVector>
#include "clocksyncanalyzer.h"
#include "database/night_clock_offsets_repository.h"

namespace Ui {
class ClockSyncDialog;
}

class ClockSyncDialog : public QDialog
{
    Q_OBJECT
#ifdef UNITTEST_MODE
    friend class ClockDriftHistoryTests;
#endif
public:
    explicit ClockSyncDialog(QWidget *parent = nullptr);
    ~ClockSyncDialog();

    void setDate(const QDate& date);

signals:
    void correctionsChanged();

private slots:
    void onAnalyze();
    void onApply();
    void onHistory();
    void onFirstData();
    void onLastData();
    void invalidateAnalysis();
    void refreshSelection();

private:
    Ui::ClockSyncDialog *ui;
    QVector<NightStartDiff> m_nights;   // primary: per-night window offsets
    ConstantOffsetFit       m_fit;      // robust constant fitted across nights
    bool m_hasAnalysis = false;
    NightClockOffsetsRepository m_nightRepo;

    void populateCpapCombo();
    void populateOxiCombo();
    Machine* currentCpap() const;
    Machine* currentOxi() const;
    void populateTable();
    QVector<NightStartDiff> selectedNights() const;
};

#endif // CLOCKSYNCDIALOG_H
