/* Apex Medical XT Auto Decode-Layer Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef APEXTESTS_H
#define APEXTESTS_H

#include "tests/AutoTest.h"

class ApexTests : public QObject
{
    Q_OBJECT
private slots:
    // .APF decode
    void testDecodeApfRecord();
    void testApfEmptySlotStopsTable();
    void testApfTableCapacity();

    // .APE ring arithmetic
    void testRingAdvanceWraps();

    // .APE session-run decode
    void testDecodeApeSessionRun_basic();
    void testDecodeApeSessionRun_wrapsAcrossRingEnd();
    void testDecodeApeSessionRun_cursorOffsetWraps();
    void testDecodeApeSessionRun_optionalZeroPrefix();
    void testDecodeApeSessionRun_staleCursorAfterWrap();
    void testDecodeApeSessionRun_missingMarker();
    void testDecodeApeSessionRun_unterminatedExceedsMaxMinutes();

    // .APE session-table parsing
    void testParseApe_matchesByExactTimestamp();
    void testParseApe_staleEntrySkippedSilently();
    void testParseApe_unusedTableEntriesSkipped();

    // OSCAR channel mapping
    void testFixedPressureSettingMapping();
    void testLeakChannelMapping();
    void testMinuteDetailExtendsSession();
};
DECLARE_TEST(ApexTests)

#endif // APEXTESTS_H
