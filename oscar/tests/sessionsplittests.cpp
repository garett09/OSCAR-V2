/* Session Split Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include <QTimeZone>

#include "sessionsplittests.h"
#include "SleepLib/machine.h"
#include "SleepLib/session.h"
#include "SleepLib/session_split.h"
#include "SleepLib/schema.h"

namespace {

//! Local wall-clock time on a fixed test date, ms since epoch.
qint64 at(int hour, int minute, int second = 0, int dayOffset = 0)
{
    return QDateTime(QDate(2026, 3, 10).addDays(dayOffset), QTime(hour, minute, second),
                     QTimeZone(QTimeZone::LocalTime)).toMSecsSinceEpoch();
}

const QTime kNoon(12, 0, 0);

} // namespace

// AddEventList() and holdsValue() look channels up in schema::channel. Guarded as in
// machinetests.cpp: another test class may have initialised it.
void SessionSplitTests::initTestCase()
{
    if (CPAP_Obstructive == 0) { schema::init(); }
}

void SessionSplitTests::testBoundariesWithin()
{
    // 05:40 to 13:30 crosses noon once.
    QList<qint64> b = SessionSplit::boundariesWithin(at(5, 40), at(13, 30), kNoon);
    QCOMPARE(b.size(), 1);
    QCOMPARE(b[0], at(12, 0));

    // A normal night, 22:00 to 06:30, crosses none.
    QVERIFY(SessionSplit::boundariesWithin(at(22, 0), at(6, 30, 0, 1), kNoon).isEmpty());

    // Ending exactly at the split time is not a crossing.
    QVERIFY(SessionSplit::boundariesWithin(at(5, 0), at(12, 0), kNoon).isEmpty());

    // 13:00 to 14:00 two days later crosses the next two noons.
    b = SessionSplit::boundariesWithin(at(13, 0), at(14, 0, 0, 2), kNoon);
    QCOMPARE(b.size(), 2);
    QCOMPARE(b[0], at(12, 0, 0, 1));
    QCOMPARE(b[1], at(12, 0, 0, 2));
}

// A Viatom-style waveform: one sample a second, filled with AddEvent().
void SessionSplitTests::testWaveformCut()
{
    Machine mach(nullptr, 1);
    Session sess(&mach, 1);
    const qint64 start = at(11, 58);
    EventList *el = sess.AddEventList(OXI_SPO2, EVL_Waveform, 1.0, 0.0, 0.0, 0.0, 1000);
    for (int i = 0; i < 240; ++i) {
        el->AddEvent(start + i * 1000L, 90 + (i % 7));
    }
    sess.really_set_first(start);
    sess.really_set_last(start + 240 * 1000L);

    const qint64 noon = at(12, 0);
    Session *tail = SessionSplit::splitAt(&sess, noon, 99);
    QVERIFY(tail != nullptr);

    QCOMPARE(sess.realLast(), noon);
    QCOMPARE(tail->realFirst(), noon);
    QCOMPARE(tail->realLast(), start + 240 * 1000L);
    QCOMPARE(tail->session(), SessionID(99));

    EventList *h = sess.eventlist[OXI_SPO2].at(0);
    EventList *t = tail->eventlist[OXI_SPO2].at(0);
    QCOMPARE(h->count(), quint32(120));
    QCOMPARE(t->count(), quint32(120));
    QCOMPARE(h->first(), start);
    QCOMPARE(t->first(), noon);
    QCOMPARE(t->time(0), noon);
    QCOMPARE(h->raw(119), EventStoreType(90 + (119 % 7)));
    QCOMPARE(t->raw(0), EventStoreType(90 + (120 % 7)));
    QCOMPARE(t->getTime().size(), 120);
    QCOMPARE(t->getTime()[0], quint32(0));       // offsets rebased to the new start
    QCOMPARE(t->getTime()[1], quint32(1000));

    delete tail;
}

// Change-only samples (the oximeter import wizard): the value before the cut is held to it.
void SessionSplitTests::testValueEventsCarryAcrossCut()
{
    Machine mach(nullptr, 2);
    Session sess(&mach, 1);
    EventList *el = sess.AddEventList(OXI_Pulse, EVL_Event);
    el->AddEvent(at(11, 50), 60);
    el->AddEvent(at(12, 10), 70);
    el->AddEvent(at(12, 20), 70);
    sess.really_set_first(at(11, 50));
    sess.really_set_last(at(12, 20));
    sess.m_cnt[OXI_Pulse] = 3;   // a stale cached summary

    Session *tail = SessionSplit::splitAt(&sess, at(12, 0), 99);
    QVERIFY(tail != nullptr);

    EventList *h = sess.eventlist[OXI_Pulse].at(0);
    QCOMPARE(h->count(), quint32(2));
    QCOMPARE(h->time(1), at(12, 0));
    QCOMPARE(h->raw(1), EventStoreType(60));

    EventList *t = tail->eventlist[OXI_Pulse].at(0);
    QCOMPARE(t->count(), quint32(3));
    QCOMPARE(t->time(0), at(12, 0));
    QCOMPARE(t->raw(0), EventStoreType(60));
    QCOMPARE(t->time(1), at(12, 10));
    QCOMPARE(t->raw(1), EventStoreType(70));

    QVERIFY(sess.m_cnt.isEmpty());     // caches discarded
    QVERIFY(sess.IsChanged());
    QVERIFY(tail->IsChanged());

    delete tail;
}

// Flags mark instants; a cut must not invent one.
void SessionSplitTests::testFlagsNotCarried()
{
    Machine mach(nullptr, 3);
    Session sess(&mach, 1);
    EventList *drops = sess.AddEventList(OXI_SPO2Drop, EVL_Event, 1, 0, 0, 0, 0, true);
    drops->AddEvent(at(11, 55), 0, 20);
    drops->AddEvent(at(12, 5), 0, 15);
    sess.really_set_first(at(11, 50));
    sess.really_set_last(at(12, 20));

    Session *tail = SessionSplit::splitAt(&sess, at(12, 0), 99);
    QVERIFY(tail != nullptr);

    EventList *h = sess.eventlist[OXI_SPO2Drop].at(0);
    EventList *t = tail->eventlist[OXI_SPO2Drop].at(0);
    QCOMPARE(h->count(), quint32(1));
    QCOMPARE(t->count(), quint32(1));
    QCOMPARE(t->time(0), at(12, 5));
    QCOMPARE(t->raw2(0), EventStoreType(15));   // the duration travels with the flag

    delete tail;
}

// A session whose end time passes the cut but whose samples do not is left whole.
void SessionSplitTests::testNoSamplesAfterCut()
{
    Machine mach(nullptr, 4);
    Session sess(&mach, 1);
    EventList *el = sess.AddEventList(OXI_Pulse, EVL_Event);
    el->AddEvent(at(11, 0), 60);
    el->AddEvent(at(11, 50), 62);
    sess.really_set_first(at(11, 0));
    sess.really_set_last(at(12, 30));

    QVERIFY(SessionSplit::splitAt(&sess, at(12, 0), 99) == nullptr);
    QCOMPARE(sess.realLast(), at(12, 30));
    QCOMPARE(sess.eventlist[OXI_Pulse].at(0)->count(), quint32(2));
}

// Mask-on slices are clipped; only the SpO2 baseline is copied to the new piece.
void SessionSplitTests::testSlicesAndSettings()
{
    Machine mach(nullptr, 5);
    Session sess(&mach, 1);
    EventList *el = sess.AddEventList(OXI_SPO2, EVL_Event);
    el->AddEvent(at(11, 0), 95);
    el->AddEvent(at(12, 30), 94);
    sess.really_set_first(at(11, 0));
    sess.really_set_last(at(12, 30));
    sess.m_slices.append(SessionSlice(at(11, 0), at(12, 30), MaskOn));
    sess.settings[OXI_SPO2Drop] = 96;
    sess.settings[SLEEP_TimeInREM] = 50;

    Session *tail = SessionSplit::splitAt(&sess, at(12, 0), 99);
    QVERIFY(tail != nullptr);

    QCOMPARE(sess.m_slices.size(), 1);
    QCOMPARE(sess.m_slices[0].end, at(12, 0));
    QCOMPARE(tail->m_slices.size(), 1);
    QCOMPARE(tail->m_slices[0].start, at(12, 0));
    QCOMPARE(tail->m_slices[0].end, at(12, 30));

    QCOMPARE(tail->settings.value(OXI_SPO2Drop).toInt(), 96);
    QVERIFY(!tail->settings.contains(SLEEP_TimeInREM));   // per-recording totals stay put
    QCOMPARE(sess.settings.value(SLEEP_TimeInREM).toInt(), 50);

    delete tail;
}
