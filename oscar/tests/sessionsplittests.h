/* Session Split Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "tests/AutoTest.h"

//! \brief Tests for SessionSplit: cutting non-CPAP sessions at the day split time (GitLab #216).
class SessionSplitTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void testBoundariesWithin();
    void testWaveformCut();
    void testValueEventsCarryAcrossCut();
    void testFlagsNotCarried();
    void testNoSamplesAfterCut();
    void testSlicesAndSettings();
};
DECLARE_TEST(SessionSplitTests)
