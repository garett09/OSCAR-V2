/* SEFAM SD Card Data Parsing Implementation
 *
 * Decodes the SEFAM card container. See sefamDataParsing.h for the interface
 * and Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md for the format itself.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "sefamDataParsing.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>

#include <cctype>

namespace SefamParsing {

void descramble(QByteArray &data)
{
    for (int i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(static_cast<quint8>(data.at(i)) ^ kObfuscationKey);
    }
}

bool parseHeader(const QByteArray &decoded, FileHeader &out)
{
    // Minimum viable header: "#NN/" + 20 serial + "/" + 12 timestamp + "/"
    if (decoded.size() < kLogHeaderLength) { return false; }

    // The tag is a format version, not a constant — "#03/" on the Rêve Auto,
    // "#02/" on the S.Box AUTO. Accept any two-digit version.
    if (decoded.at(0) != '#' || decoded.at(3) != '/') { return false; }
    if (!isdigit(static_cast<unsigned char>(decoded.at(1)))
        || !isdigit(static_cast<unsigned char>(decoded.at(2)))) { return false; }
    out.formatVersion = decoded.mid(1, 2).toInt();

    const int serialStart = 4;
    const int serialLen   = 20;
    const int stampStart  = serialStart + serialLen + 1;   // skip the '/' after the serial
    const int stampLen    = 12;

    if (decoded.at(serialStart + serialLen) != '/') { return false; }
    if (decoded.size() < stampStart + stampLen + 1) { return false; }
    if (decoded.at(stampStart + stampLen) != '/') { return false; }

    out.serial = QString::fromLatin1(decoded.mid(serialStart, serialLen)).trimmed();

    const QString stamp = QString::fromLatin1(decoded.mid(stampStart, stampLen));
    out.localStart = QDateTime::fromString(stamp, "yyMMddHHmmss");
    if (!out.localStart.isValid()) { return false; }
    // QDateTime maps a two-digit year to 1900+yy; the card is post-2000.
    if (out.localStart.date().year() < 2000) {
        out.localStart = out.localStart.addYears(100);
    }

    // Detect the variant: 32 hex digits then '/' means the long channel header.
    const int hexStart = stampStart + stampLen + 1;
    const int hexLen   = 32;
    out.utcEpoch = 0;
    out.length   = kLogHeaderLength;

    if (decoded.size() >= hexStart + hexLen + 1 && decoded.at(hexStart + hexLen) == '/') {
        const QByteArray hex = decoded.mid(hexStart, hexLen);
        static const QRegularExpression hexOnly("\\A[0-9A-Fa-f]{32}\\z");
        if (hexOnly.match(QString::fromLatin1(hex)).hasMatch()) {
            bool ok = false;
            const qint64 epoch = hex.left(16).toLongLong(&ok, 16);
            if (ok) {
                out.utcEpoch = epoch;
                out.length   = kChannelHeaderLength;
            }
        }
    }
    return true;
}

bool parseIni(const QString &path, QHash<QString, ChannelSpec> &specs, QDateTime &start)
{
    if (!QFile::exists(path)) { return false; }

    QSettings ini(path, QSettings::IniFormat);

    const int year  = ini.value("Start Record/Year").toInt();
    const int month = ini.value("Start Record/Month").toInt();
    const int day   = ini.value("Start Record/Day").toInt();
    const int hour  = ini.value("Start Record/Hour").toInt();
    const int min   = ini.value("Start Record/Min").toInt();
    const int sec   = ini.value("Start Record/Sec").toInt();
    start = QDateTime(QDate(year, month, day), QTime(hour, min, sec));

    // Channels are declared as [Chan0]..[ChanN] with no count field; walk until
    // a group is missing rather than assuming how many a firmware declares.
    for (int i = 0; ; ++i) {
        const QString group = QString("Chan%1").arg(i);
        const QString name  = ini.value(group + "/Name").toString().trimmed();
        if (name.isEmpty()) { break; }

        ChannelSpec spec;
        spec.name = name;
        spec.freq = ini.value(group + "/Freq").toInt();
        spec.bits = ini.value(group + "/Bit").toInt();
        spec.min  = ini.value(group + "/Min").toInt();
        spec.max  = ini.value(group + "/Max").toInt();

        if (spec.freq <= 0 || (spec.bits != 8 && spec.bits != 16)) {
            qWarning() << "Sefam: ignoring channel" << name
                       << "with freq" << spec.freq << "bits" << spec.bits;
            continue;
        }
        specs.insert(spec.name, spec);
    }
    return !specs.isEmpty();
}

bool readChannel(const QString &path, const ChannelSpec &spec,
                 QVector<quint8> &out, int &records, QString &error)
{
    records = 0;
    out.clear();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        error = QString("cannot open %1").arg(path);
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();

    // Only the header is obfuscated. The sample body is plaintext, and its
    // checksums are computed over the bytes exactly as stored — descrambling it
    // would both corrupt the samples and fail every checksum.
    QByteArray head = raw.left(kChannelHeaderLength);
    descramble(head);

    FileHeader hdr;
    if (!parseHeader(head, hdr)) {
        error = "bad or missing #03/ header";
        return false;
    }

    const int payload = raw.size() - hdr.length;
    if (payload < 0) { error = "file shorter than its header"; return false; }
    if (payload == 0) { return true; }        // header-only stub: channel not recorded

    const int recLen = spec.recordBytes();
    if (recLen <= kRecordTrailerBytes) { error = "invalid record length"; return false; }
    if (payload % recLen != 0) {
        // Catches a wrong assumed sample rate. Must not be silently tolerated.
        error = QString("payload %1 is not a multiple of record size %2")
                    .arg(payload).arg(recLen);
        return false;
    }

    const int total       = payload / recLen;
    const int sampleBytes = recLen - kRecordTrailerBytes;

    // 16-bit channels are sized correctly above so the file is not wrongly
    // rejected, but this loader maps no 16-bit channel to OSCAR.
    if (spec.bits != 8) { records = total; return true; }

    out.reserve(total * sampleBytes);
    for (int i = 0; i < total; ++i) {
        const char *rec = raw.constData() + hdr.length + i * recLen;

        quint32 sum = 0;
        for (int b = 0; b < sampleBytes; ++b) {
            sum += static_cast<quint8>(rec[b]);
        }
        const quint8  checksum = static_cast<quint8>(rec[sampleBytes]);
        const quint16 sequence = static_cast<quint16>(
                                   (static_cast<quint8>(rec[sampleBytes + 1]) << 8)
                                 |  static_cast<quint8>(rec[sampleBytes + 2]));

        if (checksum != static_cast<quint8>(sum & 0xFF)
            || sequence != static_cast<quint16>((i + 1) & 0xFFFF)) {
            qWarning() << "Sefam: truncating" << path << "at record" << i
                       << "of" << total << "(checksum or sequence mismatch)";
            break;
        }
        for (int b = 0; b < sampleBytes; ++b) {
            out.append(static_cast<quint8>(rec[b]));
        }
        ++records;
    }
    return true;
}

bool readLog(const QString &path, QVector<LogRecord> &out)
{
    out.clear();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { return false; }
    const QByteArray raw = f.readAll();
    f.close();

    // Header only — the log records that follow are plaintext.
    QByteArray head = raw.left(kChannelHeaderLength);
    descramble(head);

    FileHeader hdr;
    if (!parseHeader(head, hdr)) { return false; }

    const char *p = raw.constData();
    for (int off = hdr.length; off + kLogRecordLength <= raw.size(); off += kLogRecordLength) {
        const char *r = p + off;

        LogRecord rec;
        rec.utcSeconds =  static_cast<qint64>(static_cast<quint8>(r[0]))
                       | (static_cast<qint64>(static_cast<quint8>(r[1])) << 8)
                       | (static_cast<qint64>(static_cast<quint8>(r[2])) << 16)
                       | (static_cast<qint64>(static_cast<quint8>(r[3])) << 24);
        rec.code = static_cast<quint8>(r[8]);
        rec.arg  = static_cast<quint16>((static_cast<quint8>(r[9]) << 8)
                                       | static_cast<quint8>(r[10]));
        rec.payload = QByteArray(r + 11, kLogRecordLength - 11);
        out.append(rec);
    }
    return true;
}

/*! \brief Split the packed comfort/circuit field into its two settings.

    One field carries both Comfort Control Plus and the patient circuit, on both
    models: it is log record byte 21 on the Reve and settings word 9 on the
    S.Box. Sefam Analyze wrote 0x00, 0x38 and 0xB8 for CC+ off, level 1 and
    level 3 with nothing else touched, which places the level in bits 7-6 and an
    enabled marker in bits 5-3; a separate pair of writes moved bit 0 alone
    between 22 mm and 15 mm.

    Both cards holding a vendor report agree, each on both settings at once:
    the S.Box's word 9 of 0xB8 against its printed "Level 3 / 22 mm", and the
    Reve's byte 21 of 0x79 against its printed "Level 2 / 15 mm".

    \param raw   the packed field.
    \param level receives the CC+ level, or 0 when the feature is off.
    \param circuit receives the circuit diameter in mm.

    The vendor offers a third circuit choice, Circuit Select, which bit 0 cannot
    express; it would read as 22 mm. No card exercising it has been seen. */
static void decodeComfortAndCircuit(int raw, int &level, int &circuit)
{
    level   = (raw & 0x38) ? (((raw >> 6) & 0x03) + 1) : 0;
    circuit = (raw & 0x01) ? 15 : 22;
}

bool parseSettings(const LogRecord &rec, Settings &out)
{
    const auto byteAt = [&rec](int index) -> int {
        return static_cast<quint8>(rec.payload.at(index));
    };

    if (rec.code == kLogSettingsChange) {
        if (rec.payload.size() < 6) { return false; }

        // Payload index i is record byte 11 + i. The field order below is the
        // S.Box's settings table, whose six vendor-printed snapshots vary every
        // pressure and ramp field independently; the Reve's own records cannot
        // establish it, because on that device the minimum pressure and the ramp
        // start pressure are both 4.0 and the ramp duration and its echo are both
        // 45. Reading the first of each pair therefore looks correct on every
        // card held and is wrong in general -- and index 1 is the dangerous one,
        // since the S.Box writes 0 there whenever the ramp mode is I.Ramp, which
        // would suppress the ramp settings below.
        out.rampPressure = byteAt(0) / 10.0f;   // record byte 11
        out.rampMinutes  = byteAt(2);           // record byte 13
        out.maxPressure  = byteAt(4) / 10.0f;   // record byte 15
        out.minPressure  = byteAt(5) / 10.0f;   // record byte 16

        // Record byte 22. Two cards from the same device, differing only in the
        // humidifier level, differ in this byte and no other settings byte.
        if (rec.payload.size() > 11) { out.humidifierLevel = byteAt(11); }

        // Record byte 21 carries Comfort Control Plus and the patient circuit
        // together. The CC+ half was pinned on this model first: a card from the
        // same device with the level moved from 2 to 3 moved the field from 1 to
        // 2, and the measured expiratory pressure relief rose past the range of
        // all 34 preceding nights while the flow amplitude and the leak stayed
        // put. The circuit half arrived later, from the S.Box -- and bit 0 reads
        // 15 mm here, which is what this card's own report prints.
        if (rec.payload.size() > 10) {
            decodeComfortAndCircuit(byteAt(10), out.comfortLevel, out.patientCircuit);
        }

        // Record byte 10, the low half of the record's 16-bit argument -- which
        // is a duration on apnoea records but two more settings bytes here. The
        // theoretical mask leak is its low seven bits, in plain lpm: a card from
        // the same device with the setting moved from 36 to 34 read 0xA4 then
        // 0x22. Bit 7 changed at the same time and is not part of the value.
        //
        // Gated so that a model putting something else in this byte reports no
        // mask leak rather than a wrong one. The bound is the vendor's own mask
        // table (masks.txt, shipped with Sefam Analyze), which is what the
        // setting is populated from: 89 masks spanning 18 to 60 lpm. The Nea
        // user manual's "20 to 60 in steps of 2" describes the manual-entry
        // menu, not the values a selected mask can supply -- the table holds a
        // mask at 18, and two at 41.
        const int leak = rec.arg & 0x7F;
        if (leak >= kMaskLeakMinLpm && leak <= kMaskLeakMaxLpm) { out.maskLeak = leak; }

        out.valid = true;
        return true;
    }
    if (rec.code == kLogSettingsSnapshot) {
        if (rec.payload.size() < 6) { return false; }
        out.minPressure = byteAt(3) / 10.0f;
        out.maxPressure = byteAt(5) / 10.0f;
        out.valid = true;
        return true;
    }
    return false;
}

bool readMemoryImage(const QString &path, QVector<SessionSummary> &out)
{
    out.clear();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { return false; }
    const QByteArray raw = f.readAll();
    f.close();

    // The image is stored in the clear: unlike the Reve's, it is neither
    // obfuscated nor encrypted, so nothing is descrambled here.
    const auto be16 = [&raw](int offset) -> quint16 {
        return static_cast<quint16>((static_cast<quint8>(raw.at(offset)) << 8)
                                   | static_cast<quint8>(raw.at(offset + 1)));
    };

    int off = kArchiveOffset;
    while (off + kArchiveHeaderLength <= raw.size()) {
        if (be16(off) != kArchiveMarker) { break; }

        const int minutes = be16(off + 24 * 2);
        // One block per session, so a minute count beyond a couple of days means
        // the chain has been lost rather than that a session ran that long.
        if (minutes <= 0 || minutes > 4320) { break; }

        const int blockLen = kArchiveHeaderLength + minutes * kArchiveMinuteLength;
        if (off + blockLen > raw.size()) { break; }

        SessionSummary s;
        s.minutes              = minutes;
        s.obstructiveApneas    = be16(off + 13 * 2);
        s.centralApneas        = be16(off + 14 * 2);
        s.obstructiveHypopneas = be16(off + 15 * 2);
        s.centralHypopneas     = be16(off + 16 * 2);
        s.snores               = be16(off + 17 * 2);
        s.flowLimitations      = be16(off + 18 * 2);

        // Words 29..40 repeat the settings record that the separate settings
        // table holds, rotated so that its last two words come first. Taking
        // them from here rather than from that table gives the settings in force
        // for THIS session, which matters: settings changed three times inside
        // six days on one card examined.
        const int   rampPressure = be16(off + 31 * 2);
        const int   rampMinutes  = be16(off + 33 * 2);
        const int   maxPressure  = be16(off + 35 * 2);
        const int   minPressure  = be16(off + 36 * 2);
        if (minPressure > 0 && maxPressure >= minPressure && maxPressure <= 400) {
            s.settingsValid = true;
            s.minPressure   = minPressure / 10.0f;
            s.maxPressure   = maxPressure / 10.0f;
            s.rampMinutes   = rampMinutes;
            s.rampPressure  = rampPressure / 10.0f;

            // Word 30 is the rotated record's second word, the same field the
            // Reve carries in log record byte 10 and on the same encoding: the
            // theoretical mask leak in lpm, in the low seven bits. Confirmed on
            // the Reve by a controlled 36 -> 34 change, and on this model by
            // card A's own vendor report. Range-gated as in parseSettings().
            const int maskLeak = be16(off + 30 * 2) & 0x7F;
            if (maskLeak >= kMaskLeakMinLpm && maskLeak <= kMaskLeakMaxLpm) {
                s.maskLeak = maskLeak;
            }

            // Word 40 is settings-table word 9, which packs Comfort Control Plus
            // and the patient circuit into one field -- see
            // decodeComfortAndCircuit(). This card's vendor report prints
            // "Level 3" and "22 mm" and the stored 0xB8 gives both.
            decodeComfortAndCircuit(be16(off + 40 * 2), s.comfortLevel,
                                    s.patientCircuit);
        }

        s.minuteData.resize(minutes);
        const char *rec = raw.constData() + off + kArchiveHeaderLength;
        for (int m = 0; m < minutes; ++m, rec += kArchiveMinuteLength) {
            MinuteRecord &r = s.minuteData[m];
            r.meanPressure = static_cast<quint8>(rec[0]);

            // Byte 2 is four independent two-bit counters, one per event type,
            // so a minute holding two of the same kind is not lost. Reading the
            // bits as mere presence flags undercounts and looks almost right.
            const quint8 events = static_cast<quint8>(rec[2]);
            r.obstructiveApnea     =  events       & 0x03;
            r.centralApnea         = (events >> 2) & 0x03;
            r.obstructiveHypopnea  = (events >> 4) & 0x03;
            r.centralHypopnea      = (events >> 6) & 0x03;

            // Bit 7 of both of these is a flag of some other kind, not part of
            // the count. Left in, the totals disagree with the block header on
            // roughly a fifth of sessions.
            r.flowLimitation = static_cast<quint8>(rec[3]) & 0x0F;
            r.snore          = static_cast<quint8>(rec[4]) & 0x7F;
        }

        out.append(s);
        off += blockLen;
    }
    return !out.isEmpty();
}

bool readSession(const QString &dirPath, SessionData &out, QString &error)
{
    QDir dir(dirPath);
    out.dirName = dir.dirName();

    const QString iniPath = dir.absoluteFilePath(out.dirName + ".INI");
    QDateTime iniStart;
    if (!parseIni(iniPath, out.channels, iniStart)) {
        error = "missing or empty .INI";
        return false;
    }

    bool haveHeader = false;
    for (auto it = out.channels.constBegin(); it != out.channels.constEnd(); ++it) {
        const ChannelSpec &spec = it.value();
        const QString path = dir.absoluteFilePath(out.dirName + "." + spec.name);
        if (!QFile::exists(path)) { continue; }

        if (!haveHeader) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                QByteArray head = f.read(kChannelHeaderLength);
                f.close();
                descramble(head);
                if (parseHeader(head, out.header)) { haveHeader = true; }
            }
        }

        QVector<quint8> samples;
        int records = 0;
        QString chanError;
        if (!readChannel(path, spec, samples, records, chanError)) {
            qWarning() << "Sefam:" << out.dirName << spec.name << "skipped —" << chanError;
            continue;
        }
        if (samples.isEmpty()) { continue; }     // stub file: channel not recorded

        out.samples.insert(spec.name, samples);
        out.recordCount = qMax(out.recordCount, records);
    }

    if (!haveHeader) { error = "no readable channel header"; return false; }
    if (out.samples.isEmpty()) { error = "no populated channels"; return false; }

    const QString logPath = dir.absoluteFilePath(out.dirName + ".LOG");
    if (QFile::exists(logPath) && !readLog(logPath, out.log)) {
        qWarning() << "Sefam:" << out.dirName << "log unreadable — events skipped";
    }

    // Prefer a full settings-change record; fall back to a snapshot, which
    // carries pressures but no ramp fields.
    for (const LogRecord &rec : out.log) {
        if (rec.code == kLogSettingsChange && parseSettings(rec, out.settings)) { break; }
    }
    if (!out.settings.valid) {
        for (const LogRecord &rec : out.log) {
            if (rec.code == kLogSettingsSnapshot && parseSettings(rec, out.settings)) { break; }
        }
    }
    return true;
}

}  // namespace SefamParsing
