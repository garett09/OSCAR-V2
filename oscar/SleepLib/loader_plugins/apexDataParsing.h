/* Apex Medical XT Auto SD Card Data Parsing Header
 *
 * Decodes the Apex Medical XT Auto CPAP SD card's two raw files:
 *   .APF - a fixed-size table of 29-byte per-session summary records.
 *   .APE - a fixed-size circular buffer holding per-minute pressure and
 *          respiratory-event detail for the most recent 18 sessions only.
 *
 * Neither file carries a checksum, CRC, or compression of any kind. This
 * module deliberately contains no OSCAR types. The format is fully specified
 * in Notes/loaders/Apex/APEX_XT_AUTO_CARD_ANALYSIS.md; keeping the decode layer
 * free of SleepLib dependencies lets it be reasoned about and tested on its
 * own.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef APEX_DATA_PARSING_H
#define APEX_DATA_PARSING_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

/*! \namespace ApexParsing
    \brief Pure decoding of the Apex Medical XT Auto SD card format, free of
    OSCAR types. */
namespace ApexParsing {

//! Size of one .APF session-summary record.
constexpr int kApfRecordSize = 29;

//! Byte offset of the first record; bytes before this are an unparsed header.
constexpr int kApfTableStartOffset = 0x02;

//! A record consisting entirely of this byte marks the end of the table.
constexpr quint8 kApfEmptySlotByte = 0xFF;

//! Fixed size of the whole .APE file.
constexpr int kApeSize = 0x5002;   // 20482

//! Byte offset of the 18-entry session table within the .APE file.
constexpr int kApeSessionTableOffset = 0x52;

//! Number of entries in the circular session table (most recent sessions only).
constexpr int kApeSessionTableEntries = 18;

//! Size of one session-table entry.
constexpr int kApeSessionEntrySize = 8;

//! Start (inclusive) of the circular per-minute payload region.
constexpr int kApeRingStart = 0x102;

//! End (exclusive) of the circular per-minute payload region == kApeSize.
constexpr int kApeRingEnd = kApeSize;

//! A session run longer than this many minutes with no FF FF FF terminator
//! is treated as a stale, already-overwritten table entry, not real data.
constexpr int kApeMaxMinutes = 1440;

//! Marker byte identifying a live (not stale) session-table entry.
constexpr quint8 kApeSessionMarker = 0xFE;

/*! \brief Decode the 5-byte (year-2000, month, day, hour, minute) timestamp
    shared by .APF records and .APE session-table entries.
    \return An invalid QDateTime (isValid() == false) if any field is out of
            range - the caller must check this rather than trust the result. */
QDateTime decodeTimestamp5(const quint8 *bytes);

/*! \struct ApfRecord
    \brief One decoded 29-byte record from the .APF session table. */
struct ApfRecord
{
    QDateTime start;
    QDateTime end;

    float initialPressure = 0.0f;   //!< cmH2O, raw/2.0
    float maxPressure     = 0.0f;   //!< cmH2O, raw/2.0
    float minPressure     = 0.0f;   //!< cmH2O, raw/2.0
    float averagePressure = 0.0f;   //!< cmH2O, raw/10.0
    float averageLeak     = 0.0f;   //!< LPM, raw session-average total leak.

    //! Raw pressure bytes, kept alongside the scaled floats so isApap() can
    //! compare exact integers rather than floats derived from raw/2.0.
    quint8 rawMinPressure = 0;
    quint8 rawMaxPressure = 0;

    //! True when the session's min and max pressure differ, i.e. APAP mode.
    bool isApap() const { return rawMinPressure != rawMaxPressure; }
};

/*! \brief Decode one 29-byte .APF record.
    \param record Pointer to at least kApfRecordSize readable bytes.
    \param out    Populated on success.
    \return false if the record is the all-0xFF empty-slot sentinel, or if its
            timestamp fields decode to an invalid date/time. */
bool decodeApfRecord(const quint8 *record, ApfRecord &out);

/*! \brief Parse an entire .APF file into a chronological list of sessions.
    \param data  Full file contents.
    \param out   Populated with one entry per valid record, in file order.
    \param error Set only on a structural failure (file too short to contain
                 even the 2-byte header plus one record).
    \return false only on that structural failure. Parsing stops (without
            error) at the first empty-slot sentinel or the first record that
            fails to decode - either condition means nothing further in the
            table can be trusted. */
bool parseApf(const QByteArray &data, QVector<ApfRecord> &out, QString &error);

/*! \struct ApeMinuteRecord
    \brief One minute of therapy detail decoded from an .APE session run. */
struct ApeMinuteRecord
{
    float pressure = 0.0f;   //!< cmH2O, raw/10.0 - same scale as ApfRecord::averagePressure.
    quint8 apnea    = 0;      //!< Event count in this minute (byte 2 high nibble).
    quint8 hypopnea = 0;      //!< Event count in this minute (byte 2 low nibble).
    quint8 snoring  = 0;      //!< Event count in this minute (byte 3 low nibble).
};

/*! \struct ApeTableEntry
    \brief One decoded 8-byte entry from the .APE session table. */
struct ApeTableEntry
{
    bool      markerOk = false;   //!< True if byte 0 == kApeSessionMarker.
    QDateTime start;                //!< Bytes 1-5, same encoding as ApfRecord.
    quint16   cursor    = 0;        //!< Bytes 6-7, little-endian.
};

/*! \brief Decode one 8-byte .APE session-table entry.
    \param entry Pointer to at least kApeSessionEntrySize readable bytes. */
ApeTableEntry decodeApeTableEntry(const quint8 *entry);

/*! \brief Advance a ring offset, wrapping within [kApeRingStart, kApeRingEnd).
    \param offset Any integer; need not already be in range.
    \param count  Number of bytes to advance (may exceed one ring length). */
int ringAdvance(int offset, int count = 1);

/*! \brief Read count bytes starting at offset, wrapping around the ring.
    \param apeData Full .APE file contents (must be exactly kApeSize bytes).
    \param offset  Starting position; wrapped into ring bounds before reading.
    \param count   Number of bytes to read. */
QByteArray ringBytes(const QByteArray &apeData, int offset, int count);

/*! \brief Decode one session's per-minute detail run from the .APE circular
    payload, given that session's table-entry cursor.
    \param apeData   Full .APE file contents (must be exactly kApeSize bytes).
    \param cursorRaw The little-endian u16 from the session-table entry
                      (ApeTableEntry::cursor). Any value is accepted; cursor+2
                      is wrapped into [kApeRingStart, kApeRingEnd) rather than
                      range-checked.
    \param out       Populated with one ApeMinuteRecord per decoded minute.
    \param error     Human-readable reason on failure.
    \return false if: no FE FE FE marker is found at the wrapped cursor+2
            position; or no FF FF FF terminator is found within kApeMaxMinutes
            minute records. Both conditions mean the table entry is stale (its
            payload has since been overwritten by a newer session) - this is
            normal and expected, not corruption. The caller should silently
            treat that session as having no per-minute detail, not as an
            import failure. */
bool decodeApeSessionRun(const QByteArray &apeData, quint16 cursorRaw,
                          QVector<ApeMinuteRecord> &out, QString &error);

/*! \brief Read the full .APE session table and decode every live entry's
    per-minute detail run.
    \param apeData Full file contents.
    \param out     Keyed by each successfully-decoded entry's start QDateTime
                    (which matches an ApfRecord::start on a real session, see
                    APEX_LOADER_DESIGN.md section 3). Entries with a bad
                    marker, or whose payload run fails to decode via
                    decodeApeSessionRun, are simply absent from `out` - this
                    function does not take .APF records as input and does not
                    attempt matching itself; that happens one layer up, in the
                    loader.
    \return false only if apeData.size() != kApeSize. */
bool parseApe(const QByteArray &apeData, QHash<QDateTime, QVector<ApeMinuteRecord>> &out);

}  // namespace ApexParsing

#endif  // APEX_DATA_PARSING_H
