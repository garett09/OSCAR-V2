/* Apex Medical XT Auto Decode-Layer Unit Tests
 *
 * Synthetic byte-level fixtures covering the .APF/.APE decode layer described
 * in Notes/loaders/Apex/APEX_XT_AUTO_CARD_ANALYSIS.md. No OSCAR Machine/Session
 * objects are involved - everything here exercises ApexParsing directly.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include <QTemporaryDir>
#include <QTimeZone>

#include "apextests.h"
#include "SleepLib/loader_plugins/apexDataParsing.h"
#include "SleepLib/loader_plugins/apex_loader.h"
#include "SleepLib/calcs.h"
#include "SleepLib/profiles.h"
#include "SleepLib/schema.h"
#include "SleepLib/session.h"

using namespace ApexParsing;

namespace {

QByteArray makeApfRecordBytes(int startY, int startM, int startD, int startH, int startMin,
                               int endY, int endM, int endD, int endH, int endMin,
                               quint8 rawInitial, quint8 rawMax, quint8 rawMin,
                               quint8 rawAvg, quint8 rawLeak)
{
    QByteArray r(kApfRecordSize, '\0');
    r[0x00] = static_cast<char>(startY - 2000);
    r[0x01] = static_cast<char>(startM);
    r[0x02] = static_cast<char>(startD);
    r[0x03] = static_cast<char>(startH);
    r[0x04] = static_cast<char>(startMin);
    r[0x05] = static_cast<char>(endY - 2000);
    r[0x06] = static_cast<char>(endM);
    r[0x07] = static_cast<char>(endD);
    r[0x08] = static_cast<char>(endH);
    r[0x09] = static_cast<char>(endMin);
    r[0x0C] = static_cast<char>(0x0B);
    r[0x0D] = static_cast<char>(0x08);
    r[0x0E] = static_cast<char>(0x08);
    r[0x0F] = static_cast<char>(rawInitial);
    r[0x10] = static_cast<char>(rawMax);
    r[0x11] = static_cast<char>(rawMin);
    r[0x13] = static_cast<char>(rawAvg);
    r[0x14] = static_cast<char>(rawLeak);
    return r;
}

QByteArray makeEmptyApe()
{
    // 0xAA never collides with the 0xFE session marker or the 0xFF end
    // marker, so background fill never produces an accidental false match.
    return QByteArray(kApeSize, static_cast<char>(0xAA));
}

void writeRingBytes(QByteArray &ape, int offset, const QByteArray &data)
{
    int pos = ringAdvance(offset, 0);
    for (int i = 0; i < data.size(); ++i) {
        ape[pos] = data.at(i);
        pos = ringAdvance(pos, 1);
    }
}

void writeApeTableEntry(QByteArray &ape, int entryIndex, int year, int month, int day,
                         int hour, int minute, quint16 cursor)
{
    const int off = kApeSessionTableOffset + entryIndex * kApeSessionEntrySize;
    ape[off + 0] = static_cast<char>(kApeSessionMarker);
    ape[off + 1] = static_cast<char>(year - 2000);
    ape[off + 2] = static_cast<char>(month);
    ape[off + 3] = static_cast<char>(day);
    ape[off + 4] = static_cast<char>(hour);
    ape[off + 5] = static_cast<char>(minute);
    ape[off + 6] = static_cast<char>(cursor & 0xFF);
    ape[off + 7] = static_cast<char>((cursor >> 8) & 0xFF);
}

QByteArray makeMinuteBytes(quint8 pressureRaw, quint8 apnea, quint8 hypopnea,
                           quint8 snoring, quint8 eventSecond = 0)
{
    QByteArray b(4, '\0');
    b[0] = static_cast<char>(pressureRaw);
    b[1] = static_cast<char>(eventSecond);
    const quint8 nibbles = static_cast<quint8>(((apnea & 0x0F) << 4) | (hypopnea & 0x0F));
    b[2] = static_cast<char>(nibbles);
    b[3] = static_cast<char>(snoring & 0x0F);
    return b;
}

//! Write a complete session run (marker, optional zero prefix, minute
//! records, end marker) at the ring position derived from cursorRaw, using
//! the same ringAdvance() the production decoder uses - so this helper and
//! decodeApeSessionRun() agree on ring geometry by construction.
void writeSessionRun(QByteArray &ape, quint16 cursorRaw, const QVector<QByteArray> &minutes,
                      bool includeZeroPrefix)
{
    int pos = ringAdvance(static_cast<int>(cursorRaw) + 2, 0);

    QByteArray marker;
    marker.append(char(0xFE)).append(char(0xFE)).append(char(0xFE));
    writeRingBytes(ape, pos, marker);
    pos = ringAdvance(pos, 3);

    if (includeZeroPrefix) {
        writeRingBytes(ape, pos, QByteArray(4, '\0'));
        pos = ringAdvance(pos, 4);
    }

    for (const QByteArray &m : minutes) {
        writeRingBytes(ape, pos, m);
        pos = ringAdvance(pos, 4);
    }

    QByteArray end;
    end.append(char(0xFF)).append(char(0xFF)).append(char(0xFF));
    writeRingBytes(ape, pos, end);
}

}  // namespace

void ApexTests::testDecodeApfRecord()
{
    QByteArray bytes = makeApfRecordBytes(2024, 1, 15, 22, 3,
                                          2024, 1, 16, 6, 15,
                                          16, 20, 8, 90, 24);
    ApfRecord rec;
    QVERIFY(decodeApfRecord(reinterpret_cast<const quint8 *>(bytes.constData()), rec));

    QCOMPARE(rec.start, QDateTime(QDate(2024, 1, 15), QTime(22, 3, 0), QTimeZone::LocalTime));
    QCOMPARE(rec.end, QDateTime(QDate(2024, 1, 16), QTime(6, 15, 0), QTimeZone::LocalTime));
    QCOMPARE(rec.initialPressure, 8.0f);
    QCOMPARE(rec.maxPressure, 10.0f);
    QCOMPARE(rec.minPressure, 4.0f);
    QCOMPARE(rec.averagePressure, 9.0f);
    QCOMPARE(rec.averageLeak, 24.0f);
    QCOMPARE(rec.rawMinPressure, quint8(8));
    QCOMPARE(rec.rawMaxPressure, quint8(20));
    QVERIFY(rec.isApap());

    // CPAP (fixed pressure): min == max.
    QByteArray cpapBytes = makeApfRecordBytes(2024, 1, 15, 22, 3,
                                              2024, 1, 16, 6, 15,
                                              20, 20, 20, 20, 5);
    ApfRecord cpapRec;
    QVERIFY(decodeApfRecord(reinterpret_cast<const quint8 *>(cpapBytes.constData()), cpapRec));
    QVERIFY(!cpapRec.isApap());
}

void ApexTests::testApfEmptySlotStopsTable()
{
    QByteArray data;
    data.append(QByteArray(kApfTableStartOffset, '\0'));   // 2-byte unknown header
    data.append(makeApfRecordBytes(2024, 1, 1, 10, 0, 2024, 1, 1, 12, 0, 20, 20, 10, 15, 5));
    data.append(makeApfRecordBytes(2024, 1, 2, 10, 0, 2024, 1, 2, 12, 0, 20, 20, 10, 15, 5));
    data.append(QByteArray(kApfRecordSize, static_cast<char>(kApfEmptySlotByte)));   // sentinel
    data.append(makeApfRecordBytes(2024, 1, 3, 10, 0, 2024, 1, 3, 12, 0, 20, 20, 10, 15, 5));

    QVector<ApfRecord> out;
    QString error;
    QVERIFY(parseApf(data, out, error));
    QCOMPARE(out.size(), 2);
    QCOMPARE(out.at(0).start.date(), QDate(2024, 1, 1));
    QCOMPARE(out.at(1).start.date(), QDate(2024, 1, 2));
}

void ApexTests::testApfTableCapacity()
{
    const int recordCount = 732;   // real .APF tables hold roughly this many
    QByteArray data(kApfTableStartOffset, '\0');
    for (int i = 0; i < recordCount; ++i) {
        const int minute = i % 60;
        const int hour   = (i / 60) % 24;
        const int day    = 1 + (i / 1440) % 28;      // stays valid for every month
        const int month  = 1 + (i / (1440 * 28)) % 12;
        data.append(makeApfRecordBytes(2024, month, day, hour, minute,
                                       2024, month, day, hour, minute,
                                       20, 20, 10, 15, 5));
    }

    QVector<ApfRecord> out;
    QString error;
    QVERIFY(parseApf(data, out, error));
    QCOMPARE(out.size(), recordCount);
    QCOMPARE(out.first().start, QDateTime(QDate(2024, 1, 1), QTime(0, 0, 0), QTimeZone::LocalTime));
}

void ApexTests::testRingAdvanceWraps()
{
    const int span = kApeRingEnd - kApeRingStart;

    QCOMPARE(ringAdvance(kApeRingEnd - 2, 5), kApeRingStart + 3);
    QCOMPARE(ringAdvance(kApeRingStart, span * 2 + 7), kApeRingStart + 7);
    QCOMPARE(ringAdvance(kApeRingStart + 100, span * 3 + 50), kApeRingStart + 150);

    // Every result must land inside ring bounds regardless of how far out of
    // range the inputs start.
    const int result = ringAdvance(kApeRingEnd + 1000, -2500);
    QVERIFY(result >= kApeRingStart);
    QVERIFY(result < kApeRingEnd);
}

void ApexTests::testDecodeApeSessionRun_basic()
{
    QByteArray ape = makeEmptyApe();
    const quint16 cursorRaw = 0x0100;   // cursor+2 == kApeRingStart exactly

    QVector<QByteArray> minutes;
    minutes.append(makeMinuteBytes(100, /*apnea*/2, /*hypopnea*/0, /*snoring*/0));
    minutes.append(makeMinuteBytes(110, 0, /*hypopnea*/3, /*snoring*/4));
    minutes.append(makeMinuteBytes(95, false, false, false));
    // Event nibbles paired with an out-of-range seconds byte are stale and
    // must not become OSCAR events; the pressure sample remains valid.
    minutes.append(makeMinuteBytes(90, 5, 6, 7, 60));
    writeSessionRun(ape, cursorRaw, minutes, /*includeZeroPrefix*/false);

    QVector<ApeMinuteRecord> out;
    QString error;
    QVERIFY(decodeApeSessionRun(ape, cursorRaw, out, error));
    QCOMPARE(out.size(), 4);

    QCOMPARE(out.at(0).pressure, 10.0f);
    QCOMPARE(out.at(0).apnea, quint8(2));
    QCOMPARE(out.at(0).hypopnea, quint8(0));
    QCOMPARE(out.at(0).snoring, quint8(0));

    QCOMPARE(out.at(1).pressure, 11.0f);
    QCOMPARE(out.at(1).apnea, quint8(0));
    QCOMPARE(out.at(1).hypopnea, quint8(3));
    QCOMPARE(out.at(1).snoring, quint8(4));

    QCOMPARE(out.at(2).pressure, 9.5f);
    QVERIFY(!out.at(2).apnea);
    QVERIFY(!out.at(2).hypopnea);
    QVERIFY(!out.at(2).snoring);

    QCOMPARE(out.at(3).pressure, 9.0f);
    QCOMPARE(out.at(3).apnea, quint8(0));
    QCOMPARE(out.at(3).hypopnea, quint8(0));
    QCOMPARE(out.at(3).snoring, quint8(0));
}

void ApexTests::testDecodeApeSessionRun_wrapsAcrossRingEnd()
{
    QByteArray ape = makeEmptyApe();
    // cursor+2 lands on the very last valid ring byte, so the FE FE FE start
    // marker itself straddles the wrap back to kApeRingStart.
    const quint16 cursorRaw = static_cast<quint16>(kApeRingEnd - 1 - 2);

    QVector<QByteArray> minutes;
    minutes.append(makeMinuteBytes(50, false, true, false));
    minutes.append(makeMinuteBytes(200, false, false, true));
    writeSessionRun(ape, cursorRaw, minutes, /*includeZeroPrefix*/false);

    QVector<ApeMinuteRecord> out;
    QString error;
    QVERIFY2(decodeApeSessionRun(ape, cursorRaw, out, error), qPrintable(error));
    QCOMPARE(out.size(), 2);
    QCOMPARE(out.at(0).pressure, 5.0f);
    QVERIFY(out.at(0).hypopnea);
    QCOMPARE(out.at(1).pressure, 20.0f);
    QVERIFY(out.at(1).snoring);
}

void ApexTests::testDecodeApeSessionRun_cursorOffsetWraps()
{
    QByteArray ape = makeEmptyApe();
    const quint16 cursorRaw = static_cast<quint16>(kApeRingEnd - 1);
    QVector<QByteArray> minutes;
    minutes.append(makeMinuteBytes(80, false, false, false));
    writeSessionRun(ape, cursorRaw, minutes, false);

    QVector<ApeMinuteRecord> out;
    QString error;
    QVERIFY2(decodeApeSessionRun(ape, cursorRaw, out, error), qPrintable(error));
    QCOMPARE(out.size(), 1);
    QCOMPARE(out.first().pressure, 8.0f);
}

void ApexTests::testDecodeApeSessionRun_optionalZeroPrefix()
{
    QByteArray ape = makeEmptyApe();
    const quint16 cursorRaw = 0x0100;

    QVector<QByteArray> minutes;
    minutes.append(makeMinuteBytes(80, true, true, false));
    minutes.append(makeMinuteBytes(85, false, false, true));
    writeSessionRun(ape, cursorRaw, minutes, /*includeZeroPrefix*/true);

    QVector<ApeMinuteRecord> out;
    QString error;
    QVERIFY(decodeApeSessionRun(ape, cursorRaw, out, error));
    QCOMPARE(out.size(), 2);
    QCOMPARE(out.at(0).pressure, 8.0f);
    QVERIFY(out.at(0).apnea);
    QVERIFY(out.at(0).hypopnea);
    QCOMPARE(out.at(1).pressure, 8.5f);
    QVERIFY(out.at(1).snoring);
}

void ApexTests::testDecodeApeSessionRun_staleCursorAfterWrap()
{
    QByteArray ape = makeEmptyApe();
    QVector<ApeMinuteRecord> out;
    QString error;

    // Arbitrary cursor values are wrapped into the ring, then rejected when
    // the wrapped location does not contain a session marker.
    QVERIFY(!decodeApeSessionRun(ape, quint16(0xFFFF), out, error));
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(!decodeApeSessionRun(ape, quint16(0), out, error));
    QVERIFY(!error.isEmpty());
}

void ApexTests::testDecodeApeSessionRun_missingMarker()
{
    QByteArray ape = makeEmptyApe();   // no FE FE FE written anywhere
    QVector<ApeMinuteRecord> out;
    QString error;
    QVERIFY(!decodeApeSessionRun(ape, quint16(0x0100), out, error));
    QVERIFY(!error.isEmpty());
}

void ApexTests::testDecodeApeSessionRun_unterminatedExceedsMaxMinutes()
{
    QByteArray ape = makeEmptyApe();
    const quint16 cursorRaw = 0x0100;
    int pos = ringAdvance(static_cast<int>(cursorRaw) + 2, 0);

    QByteArray marker;
    marker.append(char(0xFE)).append(char(0xFE)).append(char(0xFE));
    writeRingBytes(ape, pos, marker);
    pos = ringAdvance(pos, 3);

    // kApeMaxMinutes records, none of which contain an FF FF FF triplet, and
    // deliberately no end marker anywhere.
    for (int i = 0; i < kApeMaxMinutes; ++i) {
        writeRingBytes(ape, pos, makeMinuteBytes(10, false, false, false));
        pos = ringAdvance(pos, 4);
    }

    QVector<ApeMinuteRecord> out;
    QString error;
    QVERIFY(!decodeApeSessionRun(ape, cursorRaw, out, error));
    QVERIFY(!error.isEmpty());
}

void ApexTests::testParseApe_matchesByExactTimestamp()
{
    QByteArray ape = makeEmptyApe();

    writeApeTableEntry(ape, 0, 2024, 3, 10, 7, 30, 0x0100);
    QVector<QByteArray> minutes0;
    minutes0.append(makeMinuteBytes(60, false, false, false));
    minutes0.append(makeMinuteBytes(65, true, false, false));
    writeSessionRun(ape, 0x0100, minutes0, false);

    writeApeTableEntry(ape, 1, 2024, 3, 11, 8, 0, 1000);
    QVector<QByteArray> minutes1;
    minutes1.append(makeMinuteBytes(70, false, true, false));
    writeSessionRun(ape, 1000, minutes1, false);

    QHash<QDateTime, QVector<ApeMinuteRecord>> out;
    QVERIFY(parseApe(ape, out));
    QCOMPARE(out.size(), 2);

    const QDateTime ts0(QDate(2024, 3, 10), QTime(7, 30, 0), QTimeZone::LocalTime);
    const QDateTime ts1(QDate(2024, 3, 11), QTime(8, 0, 0), QTimeZone::LocalTime);
    QVERIFY(out.contains(ts0));
    QVERIFY(out.contains(ts1));
    QCOMPARE(out.value(ts0).size(), 2);
    QCOMPARE(out.value(ts1).size(), 1);
    QCOMPARE(out.value(ts0).at(1).pressure, 6.5f);
    QVERIFY(out.value(ts0).at(1).apnea);
}

void ApexTests::testParseApe_staleEntrySkippedSilently()
{
    QByteArray ape = makeEmptyApe();

    // Entry 0: valid marker/timestamp, but a cursor pointing off the ring -
    // simulates a table entry whose payload has since been overwritten.
    writeApeTableEntry(ape, 0, 2024, 4, 1, 9, 0, 0xFFFF);

    // Entry 1: fully valid and decodable.
    writeApeTableEntry(ape, 1, 2024, 4, 2, 9, 0, 0x0100);
    QVector<QByteArray> minutes1;
    minutes1.append(makeMinuteBytes(77, false, false, true));
    writeSessionRun(ape, 0x0100, minutes1, false);

    QHash<QDateTime, QVector<ApeMinuteRecord>> out;
    QVERIFY(parseApe(ape, out));
    QCOMPARE(out.size(), 1);

    const QDateTime staleTs(QDate(2024, 4, 1), QTime(9, 0, 0), QTimeZone::LocalTime);
    const QDateTime liveTs(QDate(2024, 4, 2), QTime(9, 0, 0), QTimeZone::LocalTime);
    QVERIFY(!out.contains(staleTs));
    QVERIFY(out.contains(liveTs));
}

void ApexTests::testParseApe_unusedTableEntriesSkipped()
{
    // A card that has never recorded a session in this ring: every table
    // entry is still background fill, so none has the 0xFE marker.
    QByteArray ape = makeEmptyApe();

    QHash<QDateTime, QVector<ApeMinuteRecord>> out;
    QVERIFY(parseApe(ape, out));
    QVERIFY(out.isEmpty());
}

void ApexTests::testLeakChannelMapping()
{
    if (CPAP_LeakTotal == 0) {
        schema::init();
    }

    QTemporaryDir profileDir;
    QVERIFY(profileDir.isValid());
    Profile profile(profileDir.path(), false);
    CPAP machine(&profile, 1);
    MachineInfo machineInfo;
    machineInfo.type = MT_CPAP;
    machine.setInfo(machineInfo);

    ApfRecord rec;
    rec.averagePressure = 6.4f;
    rec.averageLeak = 17.0f;

    QVector<ApeMinuteRecord> minutes(2);
    minutes[0].pressure = 6.0f;
    minutes[1].pressure = 7.0f;

    constexpr qint64 startMs = 1000000;
    constexpr qint64 endMs = startMs + 180000;
    rec.start = QDateTime::fromMSecsSinceEpoch(startMs);
    rec.end = QDateTime::fromMSecsSinceEpoch(endMs);

    Session detailed(&machine, 1);
    ApexLoader::importMinuteDetail(&detailed, rec, minutes, startMs);

    QVERIFY(detailed.eventlist.contains(CPAP_LeakTotal));
    QCOMPARE(detailed.eventlist[CPAP_LeakTotal].size(), 1);
    EventList *totalLeak = detailed.eventlist[CPAP_LeakTotal].first();
    QCOMPARE(totalLeak->count(), 2U);
    QCOMPARE(totalLeak->time(0), startMs);
    // The leak trace is clamped to the pressure waveform's extent, not rec.end,
    // so that calcLeaks() can resolve a pressure for both samples. Here the two
    // minute records cover less than the session's three-minute span.
    QCOMPARE(totalLeak->time(1), startMs + qint64(minutes.size()) * 60000);
    QCOMPARE(totalLeak->data(0), 17.0f);
    QCOMPARE(totalLeak->data(1), 17.0f);

    QVERIFY(detailed.eventlist.contains(CPAP_Pressure));
    QVERIFY(!detailed.eventlist.contains(CPAP_PressureSet));
    EventList *pressure = detailed.eventlist[CPAP_Pressure].first();
    QCOMPARE(pressure->count(), 2U);
    QCOMPARE(pressure->data(0), 6.0f);
    QCOMPARE(pressure->data(1), 7.0f);
    QCOMPARE(pressure->last(), startMs + qint64(minutes.size()) * 60000);
    QCOMPARE(detailed.last(), endMs);

    Session summary(&machine, 2);
    ApexLoader::importSessionAverages(&summary, rec);
    QVERIFY(!summary.summaryOnly());
    QVERIFY(summary.eventlist.contains(CPAP_Pressure));
    QVERIFY(!summary.eventlist.contains(CPAP_PressureSet));
    EventList *summaryPressure = summary.eventlist[CPAP_Pressure].first();
    QCOMPARE(summaryPressure->count(), 2U);
    QCOMPARE(summaryPressure->data(0), 6.4f);
    QCOMPARE(summaryPressure->data(1), 6.4f);

    Profile *previousProfile = p_profile;
    p_profile = &profile;
    profile.cpap->setCalculateUnintentionalLeaks(true);
    QCOMPARE(summary.type(), MT_CPAP);
    QVERIFY(profile.cpap->calculateUnintentionalLeaks());
    const int detailedDerivedCount = calcLeaks(&detailed);
    const int summaryDerivedCount = calcLeaks(&summary);
    const bool hasDerivedLeak = summary.eventlist.contains(CPAP_Leak);
    const int derivedLeakLists = hasDerivedLeak
        ? summary.eventlist.value(CPAP_Leak).size() : 0;
    const quint32 derivedLeakCount = derivedLeakLists == 1
        ? summary.eventlist.value(CPAP_Leak).first()->count() : 0;
    p_profile = previousProfile;

    // Both derived counts must stay at 2: gLineChart skips any event list with
    // one sample or fewer, so a single derived point means no Leak Rate graph
    // at all. This count is a direct function of the pressure channel's extent
    // covering every CPAP_LeakTotal sample - do not relax it without checking
    // what the daily graph actually renders.
    QCOMPARE(detailedDerivedCount, 2);
    QCOMPARE(summaryDerivedCount, 2);
    QVERIFY(hasDerivedLeak);
    QCOMPARE(derivedLeakLists, 1);
    QCOMPARE(derivedLeakCount, 2U);
}

void ApexTests::testFixedPressureSettingMapping()
{
    if (CPAP_Mode == 0) { schema::init(); }

    QTemporaryDir profileDir;
    QVERIFY(profileDir.isValid());
    Profile profile(profileDir.path(), false);
    CPAP machine(&profile, 1);
    Session session(&machine, 1);

    ApfRecord rec;
    rec.rawMinPressure = 20;
    rec.rawMaxPressure = 20;
    rec.minPressure = 10.0f;
    rec.maxPressure = 10.0f;
    ApexLoader::importSettings(&session, rec);

    QCOMPARE(session.settings.value(CPAP_Mode), static_cast<double>(MODE_CPAP));
    QCOMPARE(session.settings.value(CPAP_Pressure), 10.0);
    QVERIFY(!session.settings.contains(CPAP_PressureMin));
    QVERIFY(!session.settings.contains(CPAP_PressureMax));
}

void ApexTests::testMinuteDetailExtendsSession()
{
    if (CPAP_Pressure == 0) { schema::init(); }

    QTemporaryDir profileDir;
    QVERIFY(profileDir.isValid());
    Profile profile(profileDir.path(), false);
    CPAP machine(&profile, 1);
    Session session(&machine, 1);

    constexpr qint64 startMs = 1000000;
    ApexParsing::ApfRecord rec;
    rec.start = QDateTime::fromMSecsSinceEpoch(startMs);
    rec.end = QDateTime::fromMSecsSinceEpoch(startMs + 60000);
    session.really_set_first(startMs);
    session.really_set_last(rec.end.toMSecsSinceEpoch());

    QVector<ApeMinuteRecord> minutes(3);
    ApexLoader::importMinuteDetail(&session, rec, minutes, startMs);

    QCOMPARE(session.last(), startMs + 180000);
    QCOMPARE(session.eventlist[CPAP_Pressure].first()->last(), startMs + 180000);
}
