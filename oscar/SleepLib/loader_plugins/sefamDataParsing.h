/* SEFAM SD Card Data Parsing Header
 *
 * Decodes the SEFAM CPAP SD card container: an XOR-0xBF obfuscated fixed-length
 * text header followed by a plaintext body of 10-second sample records, each
 * carrying a checksum and sequence number, plus 49-byte .LOG event records.
 *
 * Note that the obfuscation covers the HEADER ONLY. Sample and log bodies are
 * stored in the clear and their checksums are computed over the bytes as
 * stored, so descrambling a body corrupts it and fails every checksum.
 *
 * This module deliberately contains no OSCAR types. The card format is fully
 * specified in Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md and validated against
 * the manufacturer's analyzer output; keeping the decode layer free of SleepLib
 * dependencies lets it be reasoned about on its own.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef SEFAM_DATA_PARSING_H
#define SEFAM_DATA_PARSING_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

/*! \namespace SefamParsing
    \brief Pure decoding of the SEFAM SD card format, free of OSCAR types. */
namespace SefamParsing {

//! The text header of every .LOG and channel file is XORed with this constant.
//! The body that follows the header is NOT obfuscated.
constexpr quint8 kObfuscationKey = 0xBF;

//! Header length for channel data files: "#03/" + 20 serial + "/" + 12 date + "/" + 32 hex + "/".
constexpr int kChannelHeaderLength = 71;

//! Header length for .LOG files: as above but without the 32-hex field.
constexpr int kLogHeaderLength = 38;

//! Each sample record holds exactly this many seconds of data.
constexpr int kRecordSeconds = 10;

//! Bytes following the samples in each record: 1 checksum + 2 big-endian sequence.
constexpr int kRecordTrailerBytes = 3;

//! Sample value meaning "no valid data" on FLW, PRE and LK.
constexpr quint8 kInvalidSample = 0xFF;

//! Fixed size of one .LOG event record.
constexpr int kLogRecordLength = 49;

/*! \struct FileHeader
    \brief The decoded fixed-length text header at the start of every card file. */
struct FileHeader
{
    QString   serial;                  //!< Full serial, pad spaces trimmed.
    QDateTime localStart;              //!< Device local wall-clock start time.
    qint64    utcEpoch = 0;            //!< UTC epoch seconds; 0 when the short header is used.
    int       length   = 0;            //!< 71 or 38, whichever variant was detected.
    int       formatVersion = 0;       //!< The NN from the "#NN/" tag. 3 = Rêve, 2 = S.Box.
};

/*! \brief XOR every byte with kObfuscationKey, in place. Self-inverse.

    Pass only the header region. Applying this to a sample or log body corrupts
    the data and invalidates the record checksums, which are computed over the
    bytes as stored on the card. */
void descramble(QByteArray &data);

/*! \brief Parse a descrambled file header.
    \param decoded Descrambled bytes; at least kChannelHeaderLength are examined.
    \param out     Populated on success.
    \return true if the header carries a "#NN/" tag and a parseable timestamp.

    The leading tag is a two-digit format version, not a constant: the Rêve Auto
    writes "#03/" and the S.Box AUTO "#02/". Any two-digit version is accepted —
    the field is recorded in FileHeader::formatVersion for diagnostics but does
    not gate parsing, since both observed versions share the same layout and the
    header length is detected independently.

    The header length is detected, never assumed: after the 12-character
    timestamp and its delimiter, 32 hex digits followed by '/' indicate the
    71-byte channel variant; anything else means the 38-byte .LOG variant.
    Do not locate the end of the header by scanning for the fourth '/' — a
    payload byte can descramble to '/' and shift the parse. */
bool parseHeader(const QByteArray &decoded, FileHeader &out);

/*! \struct ChannelSpec
    \brief One [ChanN] block from a session's .INI manifest. */
struct ChannelSpec
{
    QString name;             //!< FLW, PRE, LK, DET, NSD, SPO, ...
    int     freq = 0;         //!< Samples per second.
    int     bits = 8;         //!< Bits per sample; .PLS declares 16.
    int     min  = 0;         //!< Declared physical minimum.
    int     max  = 0;         //!< Declared physical maximum.

    //! Bytes per 10-second record, including the 3-byte trailer.
    int recordBytes() const {
        return freq * kRecordSeconds * (bits / 8) + kRecordTrailerBytes;
    }
};

/*! \struct LogRecord
    \brief One 49-byte .LOG event record.

    Layout: uint32 LE UTC epoch, uint32 zero (upper half of a 64-bit time_t),
    uint8 event code, uint16 big-endian argument, then 38 bytes that are zero
    except on the settings records (codes 2 and 13). */
struct LogRecord
{
    qint64     utcSeconds = 0;
    quint8     code       = 0;
    quint16    arg        = 0;
    QByteArray payload;             //!< Record bytes 11..48 inclusive.
};

/*! \enum LogCode
    \brief Event codes, confirmed against the manufacturer's analyzer output.

    Counts over the validated 159 h card matched the manufacturer's per-session
    figures for every code below. See Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md. */
enum LogCode {
    kLogSettingsChange   = 2,       //!< Carries therapy settings in payload.
    kLogObstructiveApnea = 3,       //!< arg = duration in 0.1 s.
    kLogCentralApnea     = 4,       //!< arg = duration in 0.1 s.
    kLogObstructiveHypop = 5,       //!< arg is zero in ~97% of records.
    kLogCentralHypopnea  = 6,       //!< arg is zero in ~97% of records.
    kLogSnore            = 7,
    kLogFlowLimitation   = 8,
    kLogSettingsSnapshot = 13       //!< Also carries settings; fallback for code 2.
};

/*! \struct Settings
    \brief Therapy settings decoded from a log settings record.

    Only fields confirmed against the manufacturer's printed settings are
    represented. The comfort-level, patient-circuit and mask-leak bytes are
    still positional guesses that no card examined so far can vary, so they are
    deliberately absent — an unverified byte displayed as a therapy setting is
    worse than showing nothing.

    The humidifier level is the exception: a second card from the same device,
    with only that one setting changed, moved exactly one settings byte by
    exactly the amount the setting moved. See Notes/loaders/
    SEFAM_REVE_CARD_ANALYSIS.md, "Humidifier level — byte 22". */
struct Settings
{
    bool  valid           = false;
    float minPressure     = 0.0f;   //!< cmH2O
    float maxPressure     = 0.0f;   //!< cmH2O
    float rampPressure    = 0.0f;   //!< cmH2O
    int   rampMinutes     = 0;
    int   humidifierLevel = -1;     //!< 0 = off; -1 when the record omits it.
};

/*! \struct SessionData
    \brief Everything decoded from one DATA_nnn directory. */
struct SessionData
{
    QString                          dirName;        //!< e.g. "DATA_000"
    FileHeader                       header;
    int                              recordCount = 0;
    QHash<QString, ChannelSpec>      channels;       //!< Declared schema.
    QHash<QString, QVector<quint8>>  samples;        //!< Raw bytes per populated channel.
    QVector<LogRecord>               log;            //!< Decoded .LOG records.
    Settings                         settings;       //!< From a log settings record.
};

/*! \brief Parse a session's .INI manifest.
    \param path  Absolute path to DATA_nnn.INI.
    \param specs Populated with one entry per declared [ChanN] block.
    \param start Populated from [Start Record] (device local time).
    \return false if the file cannot be read or declares no channels.

    The .INI is plain text and is NOT obfuscated. It is the device's schema, so
    channel names not mapped by this loader are expected, not erroneous. */
bool parseIni(const QString &path, QHash<QString, ChannelSpec> &specs, QDateTime &start);

/*! \brief Read one channel data file, verifying every record.
    \param path     Absolute path to DATA_nnn.<EXT>.
    \param spec     Declared rate and width for this channel.
    \param out      Concatenated sample bytes (8-bit channels only).
    \param records  Number of records accepted.
    \param error    Human-readable reason on failure.
    \return false if the file is unreadable, the header is bad, or the payload
            size is not a whole number of records.

    A record is [samples][1-byte 8-bit sum checksum][2-byte big-endian sequence].
    On a checksum or sequence mismatch the channel is truncated at the last good
    record and true is returned — a card unplugged mid-write leaves a torn final
    record and everything before it is valid. */
bool readChannel(const QString &path, const ChannelSpec &spec,
                 QVector<quint8> &out, int &records, QString &error);

/*! \brief Read a session's .LOG file.
    \return false if the file is missing, unreadable, or has a bad header.

    Trailing bytes that do not form a whole 49-byte record are ignored. Only the
    header is obfuscated; the records themselves are plaintext. */
bool readLog(const QString &path, QVector<LogRecord> &out);

/*! \brief Decode therapy settings from a log record.
    \param rec A record with code kLogSettingsChange or kLogSettingsSnapshot.
    \param out Populated and marked valid on success.
    \return false if the record is not a settings record or the payload is short.

    Payload offsets are relative to record byte 11, which is payload index 0.
    Code 2:  index 0 = min pressure, 1 = ramp minutes, 4 = max pressure,
             5 = ramp pressure. Pressures are tenths of a cmH2O.
    Code 13: index 3 = min pressure, index 5 = max pressure; it carries no ramp
             fields, so those stay zero. */
bool parseSettings(const LogRecord &rec, Settings &out);

/*! \brief Read one DATA_nnn directory into a SessionData.
    \return false if the .INI is missing or no channel could be read. */
bool readSession(const QString &dirPath, SessionData &out, QString &error);

}  // namespace SefamParsing

#endif  // SEFAM_DATA_PARSING_H
