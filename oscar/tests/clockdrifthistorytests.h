/* Clock Drift History Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef CLOCKDRIFTHISTORYTESTS_H
#define CLOCKDRIFTHISTORYTESTS_H

#include <QApplication>
#include "tests/AutoTest.h"

class ClockDriftHistoryTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void testSetCorrectionDatesStoresDates();
    void testClearCorrectionDatesEmptiesList();
    void testCorrectionDateCountMatchesInput();
    void testFindAllIncludingUndoneReturnsUndoneRows();
    void testSchemaVersion18HasNightClockOffsetsTable();
    void testUpsertNightsInsertsRows();
    void testUpsertNightsUpdatesOnRerun();
    void testFindByMachinesFiltersDateRange();
private:
    QApplication* m_app = nullptr;
};
DECLARE_TEST(ClockDriftHistoryTests)

#endif // CLOCKDRIFTHISTORYTESTS_H
