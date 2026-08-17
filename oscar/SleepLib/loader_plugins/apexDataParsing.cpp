/* Apex Medical XT Auto SD Card Data Parsing Implementation
 *
 * Decodes the Apex Medical XT Auto card format. See apexDataParsing.h for the
 * interface and Notes/loaders/Apex/APEX_XT_AUTO_CARD_ANALYSIS.md for the format
 * itself.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "apexDataParsing.h"

#include <QDebug>

namespace ApexParsing {

QDateTime decodeTimestamp5(const quint8 *bytes)
{
    const int year   = 2000 + bytes[0];
    const int month  = bytes[1];
    const int day    = bytes[2];
    const int hour   = bytes[3];
    const int minute = bytes[4];

    const QDate date(year, month, day);
    const QTime time(hour, minute, 0);
    if (!date.isValid() || !time.isValid()) {
        return QDateTime();
    }
    return QDateTime(date, time, Qt::LocalTime);
}

bool decodeApfRecord(const quint8 *record, ApfRecord &out)
{
    bool allEmpty = true;
    for (int i = 0; i < kApfRecordSize; ++i) {
        if (record[i] != kApfEmptySlotByte) { allEmpty = false; break; }
    }
    if (allEmpty) { return false; }

    const QDateTime start = decodeTimestamp5(record + 0x00);
    const QDateTime end   = decodeTimestamp5(record + 0x05);
    if (!start.isValid() || !end.isValid()) { return false; }

    out.start = start;
    out.end   = end;

    // Offsets per Notes/loaders/Apex/APEX_XT_AUTO_CARD_ANALYSIS.md section 2.
    out.rawMaxPressure  = record[0x10];
    out.rawMinPressure  = record[0x11];
    out.initialPressure = record[0x0F] / 2.0f;
    out.maxPressure     = record[0x10] / 2.0f;
    out.minPressure     = record[0x11] / 2.0f;
    out.averagePressure = record[0x13] / 10.0f;
    out.averageLeak     = static_cast<float>(record[0x14]);

    return true;
}

bool parseApf(const QByteArray &data, QVector<ApfRecord> &out, QString &error)
{
    out.clear();
    if (data.size() < kApfTableStartOffset + kApfRecordSize) {
        error = QStringLiteral("Apex .APF: file too short to contain a header and one record");
        return false;
    }

    const quint8 *bytes = reinterpret_cast<const quint8 *>(data.constData());
    int offset = kApfTableStartOffset;
    // Stop at the first record that fails to decode - either the all-0xFF
    // empty-slot sentinel, or a record with an out-of-range timestamp. Either
    // way nothing further in the table can be trusted (see apexDataParsing.h).
    while (offset + kApfRecordSize <= data.size()) {
        ApfRecord rec;
        if (!decodeApfRecord(bytes + offset, rec)) {
            break;
        }
        out.append(rec);
        offset += kApfRecordSize;
    }
    return true;
}

ApeTableEntry decodeApeTableEntry(const quint8 *entry)
{
    ApeTableEntry out;
    out.markerOk = (entry[0] == kApeSessionMarker);
    out.start    = decodeTimestamp5(entry + 1);
    out.cursor   = static_cast<quint16>(entry[6]) | (static_cast<quint16>(entry[7]) << 8);
    return out;
}

int ringAdvance(int offset, int count)
{
    const int span = kApeRingEnd - kApeRingStart;
    // Normalize into [kApeRingStart, kApeRingEnd) regardless of how far out of
    // range `offset` starts or how large `count` is - mirrors the reference
    // Python decoder's _advance() exactly.
    const int shifted = offset + count - kApeRingStart;
    return kApeRingStart + ((shifted % span) + span) % span;
}

QByteArray ringBytes(const QByteArray &apeData, int offset, int count)
{
    QByteArray result;
    result.reserve(count);
    int pos = ringAdvance(offset, 0);
    for (int i = 0; i < count; ++i) {
        result.append(apeData.at(pos));
        pos = ringAdvance(pos, 1);
    }
    return result;
}

bool decodeApeSessionRun(const QByteArray &apeData, quint16 cursorRaw,
                          QVector<ApeMinuteRecord> &out, QString &error)
{
    out.clear();
    if (apeData.size() != kApeSize) {
        error = QStringLiteral("Apex .APE: wrong file size");
        return false;
    }

    // The +2 skips a 2-byte unknown sub-header preceding every session run.
    const int startPos = ringAdvance(static_cast<int>(cursorRaw), 2);

    int pos = startPos;
    const QByteArray marker = ringBytes(apeData, pos, 3);
    if (static_cast<quint8>(marker.at(0)) != 0xFE
        || static_cast<quint8>(marker.at(1)) != 0xFE
        || static_cast<quint8>(marker.at(2)) != 0xFE) {
        error = QStringLiteral("Apex .APE: missing FE FE FE start marker - stale table entry");
        return false;
    }
    pos = ringAdvance(pos, 3);

    // An optional 4-byte 00 00 00 00 block sometimes follows the start
    // marker. This is byte-for-byte ambiguous with a genuine zero-pressure,
    // zero-event first minute; observed sessions begin at a nonzero initial
    // pressure, so matching zero bytes are treated as the optional prefix.
    const QByteArray maybeZero = ringBytes(apeData, pos, 4);
    bool allZero = true;
    for (int i = 0; i < 4; ++i) {
        if (static_cast<quint8>(maybeZero.at(i)) != 0x00) { allZero = false; break; }
    }
    if (allZero) {
        pos = ringAdvance(pos, 4);
    }

    for (int minute = 0; minute < kApeMaxMinutes; ++minute) {
        const QByteArray chunk = ringBytes(apeData, pos, 4);
        const quint8 b0 = static_cast<quint8>(chunk.at(0));
        const quint8 b1 = static_cast<quint8>(chunk.at(1));
        const quint8 b2 = static_cast<quint8>(chunk.at(2));
        const quint8 b3 = static_cast<quint8>(chunk.at(3));

        if (b0 == 0xFF && b1 == 0xFF && b2 == 0xFF) {
            return true;   // FF FF FF end marker - run decoded successfully
        }

        ApeMinuteRecord rec;
        rec.pressure = b0 / 10.0f;
        // Byte 1 is the event's seconds-within-minute field. Values outside
        // 0-59 mark stale/non-event nibble data: excluding those nibbles is
        // required to reproduce every paired ground-truth session evaluated.
        if (b1 < 60) {
            rec.apnea    = b2 >> 4;       // byte 2 high nibble
            rec.hypopnea = b2 & 0x0F;     // byte 2 low nibble
            rec.snoring  = b3 & 0x0F;     // byte 3 low nibble
        }
        out.append(rec);

        pos = ringAdvance(pos, 4);
    }

    error = QStringLiteral("Apex .APE: no FF FF FF terminator within kApeMaxMinutes minutes - stale table entry");
    out.clear();
    return false;
}

bool parseApe(const QByteArray &apeData, QHash<QDateTime, QVector<ApeMinuteRecord>> &out)
{
    out.clear();
    if (apeData.size() != kApeSize) {
        return false;
    }

    const quint8 *bytes = reinterpret_cast<const quint8 *>(apeData.constData());
    for (int i = 0; i < kApeSessionTableEntries; ++i) {
        const int entryOffset = kApeSessionTableOffset + i * kApeSessionEntrySize;
        const ApeTableEntry entry = decodeApeTableEntry(bytes + entryOffset);
        if (!entry.markerOk || !entry.start.isValid()) {
            continue;
        }

        QVector<ApeMinuteRecord> minutes;
        QString error;
        if (!decodeApeSessionRun(apeData, entry.cursor, minutes, error)) {
            continue;   // stale table entry - this session simply has no detail
        }
        if (out.contains(entry.start)) {
            qWarning() << "Apex .APE: duplicate live session timestamp; replacing earlier detail"
                       << entry.start;
        }
        out.insert(entry.start, minutes);
    }
    return true;
}

}  // namespace ApexParsing
