/* Machine Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "tests/AutoTest.h"

//! \brief Tests for Machine's derived per-channel capability set
//! (noteReportedChannels / hasReportedEvents / reportsHypopneaMechanism).
class MachineTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void testReportedChannelsFromEventLists();
    void testReportedChannelsFromCounts();
    void testEmptyListDoesNotReport();
};
DECLARE_TEST(MachineTests)
