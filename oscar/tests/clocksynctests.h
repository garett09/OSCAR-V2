/* Clock Sync Analyzer Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "tests/AutoTest.h"

class ClockSyncTests : public QObject
{
    Q_OBJECT
private slots:
    void testCorrelateRecoversKnownOffset();
    void testCorrelateLargeOffsetUnbiased();
    void testCorrelateSubBinPrecision();
    void testCorrelateRejectsNoise();
    void testCorrelateEmptyInputs();
    void testCorrelateLongNight();
    void testDetectDropsBasic();
    void testDetectDropsIgnoresArtifacts();
    void testDetectDropsSlowSampleRate();
    void testDetectSurgesBasic();
    void testCombinePhysioDelay();
    void testCombineDisagreement();
    void testFitDriftRecoversSlope();
    void testFitDriftFlagsOutliers();
    void testFitConstantOffsetClean();
    void testFitConstantOffsetRejectsArtifacts();
    void testFitConstantOffsetTooFew();
    void testFitConstantOffsetStartEndSpread();
    void testFitConstantOffsetStableNoJump();
    void testFitConstantOffsetDetectsMidRangeStep();
    void testDriftToModelRoundTrip();
    void testFitDriftLineRecoversSlope();
    void testFitDriftLineFlatIsNotSignificant();
};
DECLARE_TEST(ClockSyncTests)
