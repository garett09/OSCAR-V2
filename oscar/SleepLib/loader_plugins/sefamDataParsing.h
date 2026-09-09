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

/*! \name Theoretical mask leak bounds
    The span of the vendor's own mask table (`masks.txt`, shipped with Sefam
    Analyze), which is what the setting is populated from when a clinician picks
    a mask model: 89 masks from 18 to 60 l/min. A decoded value outside this is
    taken as evidence that the byte does not hold a mask leak on that model, and
    the setting is dropped rather than reported wrongly.

    The Nea user manual's "20 to 60 l/min in steps of 2" describes the
    manual-entry menu, not what a selected mask can supply — the table holds one
    mask at 18 and two at 41.
    @{ */
constexpr int kMaskLeakMinLpm = 18;
constexpr int kMaskLeakMaxLpm = 60;
/*! @} */

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

    /*! \brief Device identity and pressure limits. NOT therapy settings.

        This was read as a settings snapshot and used as a fallback for code 2,
        because on the first card examined two of its three pressure bytes --
        4.0 and 20.0 -- happened to be that device's actual minimum and maximum.
        A second card settled it: the same three values appear in every code 13
        record it wrote, including records written months after the prescription
        had been narrowed to 6.0-7.5. They are the model's settable range and its
        default, not what the device was set to. The record also carries a
        six-digit ASCII number that is constant per device.

        Nothing reads it now. Treating it as settings made a session that carried
        only this record report the full pressure range, and left that behind as
        the carried-forward settings for every session after it. */
    kLogDeviceLimits     = 13
};

/*! \struct Settings
    \brief Therapy settings decoded from a log settings record.

    Only fields confirmed against the manufacturer's printed settings, or by a
    controlled single-setting change, are represented — an unverified byte
    displayed as a therapy setting is worse than showing nothing.

    The heated tube setting is deliberately absent. It is confirmed only in the
    file the vendor software writes to push settings onto a card, which is not
    something an ordinary card carries. Record byte 23 is a good candidate for it
    on the Rêve and Néa — it sits immediately after the confirmed humidifier byte,
    exactly as the vendor's file places the tube byte after its humidifier byte,
    it moves independently of the humidifier from night to night on both models,
    and every value seen is inside the vendor's 0–6 range. That is circumstantial,
    and no controlled change can reach it: the model whose settings the vendor
    software can write does not record this byte at all.

    See Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md, sections "Humidifier level —
    byte 22", "Theoretical mask leak — byte 10" and "Comfort Control Plus —
    byte 21". */
struct Settings
{
    bool  valid           = false;
    bool  fixedPressure   = false;  //!< Device is in CPAP, not A-PAP. See byte 17.
    float prescribedPressure = 0.0f;//!< cmH2O; meaningful only when fixedPressure.
    float minPressure     = 0.0f;   //!< cmH2O. The A-PAP band; ignored in CPAP.
    float maxPressure     = 0.0f;   //!< cmH2O. The A-PAP band; ignored in CPAP.
    float rampPressure    = 0.0f;   //!< cmH2O
    int   rampMinutes     = 0;      //!< The patient's own ramp; see rampMinutesFor().
    int   humidifierLevel = -1;     //!< 0 = off; -1 when the record omits it.
    int   maskLeak        = -1;     //!< lpm; -1 when the record omits it.
    int   comfortLevel    = -1;     //!< CC+ level; 0 = off, -1 when the record omits it.
    int   patientCircuit  = -1;     //!< Tube diameter in mm, 15 or 22; -1 when absent.
};

//! Offset of the session archive inside the device memory image, measured from
//! the start of the file (the 155-byte text header precedes the image itself).
constexpr int kArchiveOffset = 0x1009B;

//! Length of the fixed part of an archive block, in bytes: 44 big-endian uint16.
constexpr int kArchiveHeaderLength = 88;

//! Bytes per per-minute record following an archive block header.
constexpr int kArchiveMinuteLength = 6;

//! Every archive block opens with this value, as a big-endian uint16.
constexpr quint16 kArchiveMarker = 0xFEA1;

/*! \struct MinuteRecord
    \brief One minute of a session, decoded from a 6-byte archive record.

    Minute \c n covers [session start + 60n, session start + 60n + 60). That is
    session-relative, NOT aligned to the wall clock: correlating this record's
    pressure against the session's own PRE channel gives r = 0.9996 on a
    session-relative reading and 0.948 on a wall-clock-aligned one. */
struct MinuteRecord
{
    quint8 meanPressure = 0;   //!< Mean mask pressure this minute, tenths of a cmH2O.
    quint8 obstructiveApnea = 0;
    quint8 centralApnea = 0;
    quint8 obstructiveHypopnea = 0;
    quint8 centralHypopnea = 0;
    quint8 flowLimitation = 0;
    quint8 snore = 0;
};

/*! \struct SessionSummary
    \brief One block of the device's lifetime session archive.

    The S.Box AUTO writes no .LOG. Its events and its therapy settings live only
    in the <serial>.RAM memory image, as a contiguous chain of these blocks —
    one per session for the whole life of the device, oldest first. See
    Notes/loaders/SEFAM_SBOX_CARD_ANALYSIS.md.

    The counts below are the device's own totals. They are reproduced exactly by
    summing the corresponding per-minute fields, verified on every block of both
    cards examined (470 blocks, no mismatch), so either may be used; the loader
    places events from the minutes and uses the totals only as a cross-check. */
struct SessionSummary
{
    int minutes = 0;                    //!< floor(recordCount * 10 / 60) + 1.

    int obstructiveApneas = 0;
    int centralApneas = 0;
    int obstructiveHypopneas = 0;
    int centralHypopneas = 0;
    int snores = 0;
    int flowLimitations = 0;

    bool  settingsValid  = false;
    bool  fixedPressure  = false;       //!< Device is in CPAP, not A-PAP.
    float prescribedPressure = 0.0f;    //!< cmH2O; meaningful only when fixedPressure.
    float minPressure    = 0.0f;        //!< cmH2O. The A-PAP band; ignored in CPAP.
    float maxPressure    = 0.0f;        //!< cmH2O. The A-PAP band; ignored in CPAP.
    float rampPressure   = 0.0f;        //!< cmH2O
    int   rampMinutes    = 0;           //!< 0 when the ramp is off; see rampMinutesFor().
    int   maskLeak       = -1;          //!< lpm; -1 when out of the documented range.
    int   patientCircuit = -1;          //!< Tube diameter in mm, 15 or 22; -1 when absent.
    int   comfortLevel   = -1;          //!< CC+ level; 0 = off, -1 when absent.

    QVector<MinuteRecord> minuteData;
};

/*! \brief Read the session archive out of a <serial>.RAM or .BKP memory image.
    \param path Absolute path to the image.
    \param out  Populated oldest-session-first; cleared first.
    \return false if the file cannot be read or carries no archive.

    The archive is walked as a chain — each block's length is 88 + 6 * minutes,
    and the next block begins immediately after — starting at kArchiveOffset.
    Do NOT locate blocks by scanning for kArchiveMarker: that value also occurs
    inside the per-minute data and a scan invents blocks that are not there.

    A block whose marker, minute count or extent is implausible ends the walk
    rather than failing it, so a truncated or partly overwritten image still
    yields the blocks that precede the damage. */
bool readMemoryImage(const QString &path, QVector<SessionSummary> &out);

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
    On a checksum failure the channel is truncated at the last good record and
    true is returned — a card unplugged mid-write leaves a torn final record and
    everything before it is valid.

    A **skipped sequence number** is different: the device dropped a record and
    carried on, and everything after the hole is good. Each missing record is
    filled with kInvalidSample so that later samples keep their true offset from
    the session start, and \a records counts the filled slots too, so it always
    equals the last sequence number accepted. */
bool readChannel(const QString &path, const ChannelSpec &spec,
                 QVector<quint8> &out, int &records, QString &error);

/*! \brief Read a session's .LOG file.
    \return false if the file is missing, unreadable, or has a bad header.

    Trailing bytes that do not form a whole 49-byte record are ignored. Only the
    header is obfuscated; the records themselves are plaintext. */
bool readLog(const QString &path, QVector<LogRecord> &out);

/*! \brief Decode therapy settings from a log record.
    \param rec A record with code kLogSettingsChange.
    \param out Populated and marked valid on success.
    \return false if the record is not a settings record or the payload is short.

    Payload offsets are relative to record byte 11, which is payload index 0:
    0 = ramp start pressure, 1 = the patient's ramp minutes, 2 = the
    practitioner's ceiling on them, 3 = ramp mode and flags, 4 = maximum
    pressure, 5 = minimum pressure, 6 = therapy mode, 10 = comfort level and
    patient circuit packed together, 11 = humidifier level. Pressures are tenths
    of a cmH2O.

    Neither of the record's own two settings bytes is in the payload: byte 9, the
    high half of the 16-bit argument, is the prescribed pressure, and byte 10,
    its low half, is the theoretical mask leak.

    Code 13 is not a settings record — see kLogDeviceLimits. */
bool parseSettings(const LogRecord &rec, Settings &out);

/*! \brief Read one DATA_nnn directory into a SessionData.
    \return false if the .INI is missing or no channel could be read. */
bool readSession(const QString &dirPath, SessionData &out, QString &error);

}  // namespace SefamParsing

#endif  // SEFAM_DATA_PARSING_H
