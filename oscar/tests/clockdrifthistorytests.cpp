/* Clock Drift History Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "clockdrifthistorytests.h"
#include "driftplotwidget.h"
#include <QDate>

void ClockDriftHistoryTests::initTestCase()
{
    int argc = 0;
    char** argv = nullptr;
    m_app = new QApplication(argc, argv);
}

void ClockDriftHistoryTests::cleanupTestCase()
{
    delete m_app;
    m_app = nullptr;
}

void ClockDriftHistoryTests::testSetCorrectionDatesStoresDates()
{
    DriftPlotWidget w;
    QList<QDate> dates = { QDate(2026, 1, 15), QDate(2026, 3, 20) };
    w.setCorrectionDates(dates);
    QCOMPARE(w.correctionDateCount(), 2);
}

void ClockDriftHistoryTests::testClearCorrectionDatesEmptiesList()
{
    DriftPlotWidget w;
    w.setCorrectionDates({ QDate(2026, 1, 15) });
    w.clearCorrectionDates();
    QCOMPARE(w.correctionDateCount(), 0);
}

void ClockDriftHistoryTests::testCorrectionDateCountMatchesInput()
{
    DriftPlotWidget w;
    QCOMPARE(w.correctionDateCount(), 0);
    w.setCorrectionDates({ QDate(2026, 2, 1), QDate(2026, 4, 1), QDate(2026, 6, 1) });
    QCOMPARE(w.correctionDateCount(), 3);
}
