/* Machine Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "machinetests.h"
#include "SleepLib/machine.h"
#include "SleepLib/session.h"
#include "SleepLib/schema.h"

// AddEventList() looks the channel up in schema::channel, so the channel table must
// exist. Guarded as in apextests.cpp: another test class may have initialised it.
void MachineTests::initTestCase()
{
    if (CPAP_Obstructive == 0) { schema::init(); }
}

// An import-time session: events live in EventLists, m_cnt is still empty.
void MachineTests::testReportedChannelsFromEventLists()
{
    Machine mach(nullptr, 1);
    Session sess(&mach, 1);
    EventList *ch = sess.AddEventList(CPAP_CentralHypopnea, EVL_Event);
    ch->AddEvent(1000, 0);

    QVERIFY(!mach.reportsHypopneaMechanism());
    mach.noteReportedChannels(&sess);
    QVERIFY(mach.hasReportedEvents(CPAP_CentralHypopnea));
    QVERIFY(!mach.hasReportedEvents(CPAP_ObstructiveHypopnea));
    QVERIFY(mach.reportsHypopneaMechanism());
}

// A database-loaded session: EventLists are empty, m_cnt came from session_channels.
void MachineTests::testReportedChannelsFromCounts()
{
    Machine mach(nullptr, 2);
    Session sess(&mach, 1);
    sess.m_cnt[CPAP_ObstructiveHypopnea] = 3;
    sess.m_cnt[CPAP_RERA] = 0;

    mach.noteReportedChannels(&sess);
    QVERIFY(mach.hasReportedEvents(CPAP_ObstructiveHypopnea));
    QVERIFY(!mach.hasReportedEvents(CPAP_RERA));      // count 0 is not evidence
    QVERIFY(mach.reportsHypopneaMechanism());
}

// The empty lists the OH/CH loaders used to create must not count as evidence.
void MachineTests::testEmptyListDoesNotReport()
{
    Machine mach(nullptr, 3);
    Session sess(&mach, 1);
    sess.AddEventList(CPAP_ObstructiveHypopnea, EVL_Event);
    sess.AddEventList(CPAP_CentralHypopnea, EVL_Event);

    mach.noteReportedChannels(&sess);
    QVERIFY(!mach.reportsHypopneaMechanism());
}
