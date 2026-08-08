/* SEFAM Loader Implementation
 *
 * Detects and imports SEFAM CPAP SD cards. See sefam_loader.h for the class
 * interface, Notes/loaders/SEFAM_LOADER_DESIGN.md for the design rationale and
 * Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md for the card format.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "sefam_loader.h"
#include "sefamDataParsing.h"

#include "SleepLib/common.h"
#include "SleepLib/importcontext.h"
#include "SleepLib/profiles.h"
#include "SleepLib/schema.h"
#include "SleepLib/session.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QRegularExpression>
#include <QSettings>

using SefamParsing::ChannelSpec;

/*! \name Hypopnea flag placement
    The card records no duration for hypopneas — the log argument is zero in
    about 97% of code 5 and 6 records — so their flags cannot be placed at the
    event start the way apneas can. Left at the log timestamp they render at the
    event end, roughly 15 seconds later than the manufacturer's own report shows
    them, on this patient's most frequent event type.

    These constants shift the flag back by the manufacturer's published mean
    durations for the validated card so most hypopneas land near their true
    start. This is a **display-placement heuristic, not measured data**: real
    durations vary, so individual events are approximate in either direction.
    The stored event duration remains zero, which is why no duration appears in
    the tooltip.

    Remove these once the per-event extent is recovered — Y17 is the suspected
    source. See Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md, open problem 1.
    @{ */
constexpr qint64 kObstructiveHypopneaPlacementMs = 16000;   //!< Vendor mean, 16 s
constexpr qint64 kCentralHypopneaPlacementMs     = 15000;   //!< Vendor mean, 15 s
/*! @} */

/*! \brief Width of the therapy-pressure smoothing window, in seconds.

    Ten seconds spans roughly two and a half breaths at the validated card's
    respiratory rate. That reduces the mask-pressure ripple from about
    1.0 cmH2O peak to peak down to about 0.1 — the channel's own quantisation
    step — while staying short enough to follow the device's pressure responses:
    the validated card slews up to 2.7 cmH2O in ten seconds, and doubling the
    window to twenty seconds more than doubles the tracking error across those
    ramps. See the CPAP_Pressure import in Open(). */
constexpr int kPressureSmoothingSeconds = 10;

/*! \name Blower-off detection
    The card keeps writing 10-second records while the blower is stopped, so a
    session's files span the whole time the machine was powered on, not the time
    therapy was delivered. Those stretches must be excluded or they are counted
    as usage and averaged into the pressure statistics: on the validated card
    one 19:40 session contains 269 s of blower-off, which pulled its mean
    pressure to 3.22 cmH2O against the manufacturer's reported 4.3.

    While stopped the card records a spread of near-zero pressures — 0.0 to 0.4
    cmH2O, not a single sentinel value — and therapy never goes below the
    device's 4.0 cmH2O minimum setting, leaving a wide empty band between the
    two. 2.0 cmH2O sits in that band.

    **This threshold is an assumption, not a value read from the card.** It has
    margin only because these devices bottom out at 4.0 cmH2O; a model whose
    minimum pressure could be set lower would narrow it.

    The minimum duration suppresses the brief dips seen during ramp-down and
    ramp-up transitions, which would otherwise punch spurious holes.
    @{ */
constexpr float kBlowerOffPressure       = 2.0f;   //!< cmH2O; below this = stopped
constexpr int   kBlowerOffMinimumSeconds = 10;     //!< Ignore shorter excursions
/*! @} */

static bool sefam_initialised = false;

SefamLoader::SefamLoader()  { m_type = MT_CPAP; }
SefamLoader::~SefamLoader() = default;

/*! \brief Sort DATA_nnn directory names into chronological order.

    Session directories are not consistently zero-padded across models: the Rêve
    Auto writes DATA_000, the S.Box AUTO writes DATA_0. Plain name sorting puts
    DATA_10 before DATA_2, which would carry therapy settings backwards in time.
    Sort on the numeric suffix, falling back to name order if it is missing. */
static void sortSessionDirs(QStringList &dirs)
{
    std::sort(dirs.begin(), dirs.end(), [](const QString &a, const QString &b) {
        bool aok = false, bok = false;
        const int an = a.section('_', -1).toInt(&aok);
        const int bn = b.section('_', -1).toInt(&bok);
        if (aok && bok && an != bn) { return an < bn; }
        return a < b;
    });
}

void SefamLoader::Register()
{
    if (sefam_initialised) { return; }
    qDebug() << "Registering SefamLoader";
    RegisterLoader(new SefamLoader());
    sefam_initialised = true;
}

ChannelID SEFAM_HumidLevel = 0;

/*! \brief Register the SEFAM-specific settings channels.

    No run-once guard here, deliberately. schema::resetChannels() destroys every
    registered channel and then re-invokes initChannels() on each loader, which
    happens when a profile is created and when the user resets channel defaults;
    a guard would make the second call a no-op and leave the channel missing for
    the rest of the session. ChannelList::add() already rejects duplicate ids.

    There is no generic humidifier channel to reuse: CPAP_HumidSetting resolves
    through schema::channel["HumidSet"], and nothing ever registers a channel of
    that name, so it is an empty channel. Hence the per-manufacturer channel,
    the same pattern the ResMed and PRS1 loaders use. */
void SefamLoader::initChannels()
{
    using namespace schema;

    // 0xe5xx is unused by every other loader — see the "Ensure your channel ID
    // is unique" note in schema.cpp.
    // The displayed label is deliberately the same string every other loader
    // uses for this setting — see GitLab #263.
    Channel *chan = new Channel(SEFAM_HumidLevel = 0xe500, SETTING, MT_CPAP, SESSION,
                                "SEFAM_HumidLevel", QObject::tr("Humidifier"),
                                QObject::tr("Humidifier level"),
                                QObject::tr("Humidity Level"), "", LOOKUP, Qt::black);
    channel.add(GRP_CPAP, chan);

    // Only the off position is named. Daily prints the raw number for any value
    // with no option, so the numbered levels need no entries and a level beyond
    // the vendor manual's range of 10 would still display honestly.
    chan->addOption(0, STR_TR_Off);
}

QString SefamLoader::findSerialDir(const QString &path)
{
    // Card layout: <root>/<digits><letter>/<digits>/DATA_nnn/
    // Accept the caller pointing at the root, the model directory, or the
    // serial directory itself, so import works from any of the three.
    static const QRegularExpression modelRe("\\A\\d+[A-Za-z]\\z");
    static const QRegularExpression serialRe("\\A\\d+\\z");

    QDir dir(path);
    if (!dir.exists()) { return QString(); }

    // Already the serial directory?
    if (!dir.entryList(QStringList("DATA_*"), QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
        return dir.absolutePath();
    }

    const QStringList level1 = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &a : level1) {
        QDir sub(dir.absoluteFilePath(a));

        // <root>/<model>/<serial>/DATA_nnn
        if (modelRe.match(a).hasMatch()) {
            const QStringList level2 = sub.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &b : level2) {
                if (!serialRe.match(b).hasMatch()) { continue; }
                QDir leaf(sub.absoluteFilePath(b));
                if (!leaf.entryList(QStringList("DATA_*"),
                                    QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
                    return leaf.absolutePath();
                }
            }
        }

        // <model>/<serial>/DATA_nnn — caller pointed at the model directory
        if (serialRe.match(a).hasMatch()
            && !sub.entryList(QStringList("DATA_*"),
                              QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
            return sub.absolutePath();
        }
    }
    return QString();
}

bool SefamLoader::Detect(const QString &path)
{
    const QString serialDir = findSerialDir(path);
    if (serialDir.isEmpty()) { return false; }

    QDir dir(serialDir);
    const QStringList sessions =
        dir.entryList(QStringList("DATA_*"), QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    // Confirm the "#03/" tag in at least one channel header. A directory tree of
    // the right shape from another vendor must not produce a false positive.
    for (const QString &s : sessions) {
        QDir sess(dir.absoluteFilePath(s));
        if (!QFile::exists(sess.absoluteFilePath(s + ".INI"))) { continue; }

        QFile f(sess.absoluteFilePath(s + ".FLW"));
        if (!f.open(QIODevice::ReadOnly)) { continue; }
        QByteArray head = f.read(SefamParsing::kChannelHeaderLength);
        f.close();

        SefamParsing::descramble(head);
        SefamParsing::FileHeader hdr;
        if (SefamParsing::parseHeader(head, hdr)) {
            qDebug() << "SefamLoader::Detect matched" << serialDir
                     << "serial" << hdr.serial;
            return true;
        }
    }
    return false;
}

/*! \brief Turn an .INI "Created By" value into a display model name.
    \return e.g. "REVE_AUTO " -> "Rêve Auto"; empty if the input was empty.

    Only names actually observed on a card are translated. Anything else falls
    back to the raw trimmed value so an unknown model still shows something
    meaningful rather than a blank. */
static QString sefamModelName(const QString &createdBy)
{
    // Normalise: the S.Box writes "S.Box_AUTO", so strip punctuation before
    // matching rather than listing every spelling.
    QString key = createdBy.trimmed().toUpper();
    if (key.isEmpty()) { return QString(); }
    key.remove('.').remove('_').remove(' ');

    if (key == "REVEAUTO") { return QString::fromUtf8("Rêve Auto"); }
    if (key == "SBOXAUTO") { return QStringLiteral("S.Box Auto"); }
    return createdBy.trimmed();
}

MachineInfo SefamLoader::PeekInfo(const QString &path)
{
    MachineInfo info = newInfo();
    const QString serialDir = findSerialDir(path);
    if (serialDir.isEmpty()) { return info; }

    QDir dir(serialDir);

    // The hardware model code (e.g. "1279R") is the parent directory name. It is
    // kept as a property rather than in `series`, because `series` drives the
    // device-pixmap lookup in MachineLoader::getPixmap() and must stay "Sefam".
    const QString modelCode = QFileInfo(dir.absolutePath()).dir().dirName();
    info.properties[QStringLiteral("ModelCode")] = modelCode;

    const QStringList sessions =
        dir.entryList(QStringList("DATA_*"), QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &s : sessions) {
        QDir sess(dir.absoluteFilePath(s));
        QFile f(sess.absoluteFilePath(s + ".FLW"));
        if (!f.open(QIODevice::ReadOnly)) { continue; }
        QByteArray head = f.read(SefamParsing::kChannelHeaderLength);
        f.close();

        SefamParsing::descramble(head);
        SefamParsing::FileHeader hdr;
        if (!SefamParsing::parseHeader(head, hdr)) { continue; }
        info.serial = hdr.serial;

        // Model name comes from the .INI, which is plain text and not obfuscated.
        QSettings ini(sess.absoluteFilePath(s + ".INI"), QSettings::IniFormat);
        const QString model = sefamModelName(ini.value("Create Info/Created By").toString());
        info.modelnumber = model.isEmpty() ? modelCode : model;
        info.model       = info.modelnumber;

        // Firmware version, for diagnostics on untested models.
        const QString fw = ini.value("Create Info/Version").toString().trimmed();
        if (!fw.isEmpty()) { info.properties[QStringLiteral("Firmware")] = fw; }
        break;
    }
    if (info.modelnumber.isEmpty()) {
        info.modelnumber = modelCode;
        info.model       = modelCode;
    }
    return info;
}

/*! \brief Centred moving average over the valid samples of a waveform.

    Invalid samples are excluded from every window rather than counted as zero,
    which would drag the average down around a drop-out. The output keeps the
    input's validity exactly — a smoothed sample is produced only where the
    source has one — so the smoothed channel covers the same span as its source
    and importWaveform() splits both at the same gaps. Letting the window bridge
    a gap instead would extend the smoothed channel half a window past each end
    of every drop-out, and the two graphs would no longer line up.

    Near the ends of the signal, and either side of a gap, the window is clipped
    to the available valid samples, so those outputs average over fewer inputs.

    \param values     Input samples in the channel's storage units.
    \param valid      Per-sample validity, parallel to \a values.
    \param window     Window width in samples; rounded up to odd so it centres.
    \param[out] out       Smoothed samples, same length as \a values.
    \param[out] outValid  Validity of \a out, same length as \a values. */
static void movingAverage(const QVector<qint16> &values, const QVector<bool> &valid,
                          int window, QVector<qint16> &out, QVector<bool> &outValid)
{
    const int n = qMin(values.size(), valid.size());
    out.resize(n);
    outValid.resize(n);
    if (n == 0) { return; }

    if (window < 1)     { window = 1; }
    if (window % 2 == 0) { ++window; }
    const int half = window / 2;

    // Running sum, so the cost per sample does not grow with the window width.
    qint64 sum   = 0;
    int    count = 0;
    auto include = [&](int i) {
        if (i >= 0 && i < n && valid.at(i)) { sum += values.at(i); ++count; }
    };
    auto exclude = [&](int i) {
        if (i >= 0 && i < n && valid.at(i)) { sum -= values.at(i); --count; }
    };

    for (int i = 0; i <= half && i < n; ++i) { include(i); }

    for (int i = 0; i < n; ++i) {
        if (i > 0) {
            exclude(i - half - 1);      // sample that just left the window
            include(i + half);          // sample that just entered it
        }
        // count is necessarily non-zero wherever the source sample is valid,
        // because that sample is itself inside the window.
        outValid[i] = valid.at(i);
        out[i] = outValid[i]
               ? static_cast<qint16>(qRound(static_cast<double>(sum) / count))
               : 0;
    }
}

/*! \brief Find the stretches of a session where the blower was not running.

    Detected from the pressure channel, which is the only direct evidence the
    card gives: see the kBlowerOffPressure notes for the threshold's basis and
    its limits. Samples with no data at all count as stopped too, which also
    picks up the 10-second sentinel every session opens with.

    \param pre   Raw `PRE` samples, in tenths of a cmH2O.
    \param freq  `PRE` sample rate in Hz, from the .INI.
    \returns Sorted, non-overlapping `[start, end)` spans in milliseconds from
             the session start. Empty if the blower ran throughout. */
static QVector<QPair<qint64, qint64>> findBlowerOffSpans(const QVector<quint8> &pre, int freq)
{
    QVector<QPair<qint64, qint64>> spans;
    if (freq <= 0 || pre.isEmpty()) { return spans; }

    const int    threshold   = static_cast<int>(kBlowerOffPressure * 10.0f);   // raw tenths
    const int    minSamples  = kBlowerOffMinimumSeconds * freq;
    const qint64 msPerSample = 1000 / freq;

    int i = 0;
    const auto stopped = [&](int k) {
        const quint8 v = pre.at(k);
        return v == SefamParsing::kInvalidSample || v < threshold;
    };

    while (i < pre.size()) {
        if (!stopped(i)) { ++i; continue; }
        int j = i;
        while (j < pre.size() && stopped(j)) { ++j; }
        if (j - i >= minSamples) {
            spans.append(qMakePair(static_cast<qint64>(i) * msPerSample,
                                   static_cast<qint64>(j) * msPerSample));
        }
        i = j;
    }
    return spans;
}

/*! \brief Mark samples invalid wherever they fall inside one of \a spans.

    Applied to every waveform so the blower-off stretches leave a genuine gap in
    all channels at once, instead of a flat near-zero line that gets averaged
    into the statistics. Spans are in milliseconds from the session start, so
    one set works for channels of differing rates.

    \param valid   Validity flags to clear, modified in place.
    \param spans   Blower-off spans from findBlowerOffSpans().
    \param rateMs  Sample interval of this channel, in milliseconds. */
static void clearSpans(QVector<bool> &valid,
                       const QVector<QPair<qint64, qint64>> &spans, int rateMs)
{
    if (rateMs <= 0) { return; }
    for (const QPair<qint64, qint64> &span : spans) {
        const int from = static_cast<int>(span.first / rateMs);
        // Round the end up so a span never leaves a partly-covered sample behind.
        const int to   = static_cast<int>((span.second + rateMs - 1) / rateMs);
        for (int i = qMax(0, from); i < qMin(to, static_cast<int>(valid.size())); ++i) {
            valid[i] = false;
        }
    }
}

/*! \brief Record mask-on and mask-off spans so usage time excludes blower-off.

    `Session::hours()` sums only the MaskOn slices when any slice is present,
    and falls back to the whole session span when none is. Without this the
    session reports the time the machine was powered on rather than the time it
    was treating — 19:40 instead of about 15:10 for the worst session on the
    validated card. `bmc_loader.cpp` does the same thing for the same reason.

    Both statuses are recorded, as prs1_loader does: `Day` counts only MaskOn,
    while gSessionTimesChart draws every slice and renders the off ones black.

    \param session   Session to append slices to.
    \param startMs   Session start.
    \param endMs     Session end.
    \param offSpans  Blower-off spans, in milliseconds from \a startMs. */
static void addMaskSlices(Session *session, qint64 startMs, qint64 endMs,
                          const QVector<QPair<qint64, qint64>> &offSpans)
{
    if (offSpans.isEmpty()) { return; }      // ran throughout: the fallback is correct

    QVector<SessionSlice> slices;
    qint64 cursor = startMs;
    for (const QPair<qint64, qint64> &span : offSpans) {
        const qint64 offStart = qBound(startMs, startMs + span.first,  endMs);
        const qint64 offEnd   = qBound(startMs, startMs + span.second, endMs);
        if (offStart > cursor) { slices.append(SessionSlice(cursor, offStart, MaskOn)); }
        if (offEnd   > offStart) { slices.append(SessionSlice(offStart, offEnd, MaskOff)); }
        cursor = qMax(cursor, offEnd);
    }
    if (cursor < endMs) { slices.append(SessionSlice(cursor, endMs, MaskOn)); }

    // An all-off session would leave hours() with nothing to sum, and an empty
    // slice list silently means "no slice information" — which would report the
    // full span, the opposite of the intent. Leave the slices off and say so.
    bool anyOn = false;
    for (const SessionSlice &s : slices) {
        if (s.status == MaskOn) { anyOn = true; break; }
    }
    if (!anyOn) {
        qWarning() << "Sefam: session" << session->session()
                   << "is entirely blower-off; usage time will be its full span";
        return;
    }
    session->m_slices = slices;
}

void SefamLoader::importWaveform(Session *session, const QVector<qint16> &values,
                                 const QVector<bool> &valid, ChannelID chan,
                                 EventDataType gain, int rateMs, qint64 startMs)
{
    const int n = qMin(values.size(), valid.size());
    int i = 0;
    while (i < n) {
        while (i < n && !valid.at(i)) { ++i; }      // skip a run of invalid samples
        if (i >= n) { break; }

        const int runStart = i;
        while (i < n && valid.at(i)) { ++i; }
        const int runLen = i - runStart;

        EventList *el = session->AddEventList(chan, EVL_Waveform, gain, 0.0,
                                              0.0, 0.0, rateMs);
        // AddWaveform takes a mutable pointer, so copy the run out.
        QVector<qint16> run = values.mid(runStart, runLen);
        el->AddWaveform(startMs + static_cast<qint64>(runStart) * rateMs,
                        run.data(), runLen,
                        static_cast<qint64>(runLen) * rateMs);
    }
}

int SefamLoader::Open(const QString &path)
{
    Q_ASSERT(m_ctx);
    if (!Detect(path)) { return -1; }

    const MachineInfo info = PeekInfo(path);
    qDebug() << "SefamLoader::Open — brand" << info.brand
             << "model" << info.model
             << "serial" << info.serial
             << "modelcode" << info.properties.value(QStringLiteral("ModelCode"))
             << "firmware" << info.properties.value(QStringLiteral("Firmware"));

    m_ctx->CreateMachineFromInfo(info);
    Machine *mach = p_profile->CreateMachine(info);

    // Only one model has been validated end-to-end against a manufacturer report.
    // Others import on a best-effort basis but must announce themselves.
    if (info.properties.value(QStringLiteral("ModelCode")) != sefam_validated_model) {
        MachineInfo untested = info;      // the signal takes a non-const reference
        emit deviceIsUntested(untested);
    }

    backupData(mach, path);

    const QString serialDir = findSerialDir(path);
    QDir dir(serialDir);
    QStringList dirs =
        dir.entryList(QStringList("DATA_*"), QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    sortSessionDirs(dirs);   // chronological, not lexical — see above

    emit updateMessage(QObject::tr("Reading SEFAM card..."));
    emit setProgressMax(dirs.size());
    emit setProgressValue(0);

    int imported = 0, skipped = 0, progress = 0;
    qint64 totalSpanMs = 0, totalUsageMs = 0;

    // The device writes a settings record only occasionally — 4 of the 31
    // sessions on the validated card carry one. Settings are therefore carried
    // forward from the most recent record at or before each session, which is
    // what the manufacturer's own software does (its report lists identical
    // settings on every session row). Directory names sort chronologically, so
    // iterating in order makes this safe even if settings change mid-card:
    // a session never inherits settings recorded after it.
    SefamParsing::Settings lastKnown;

    for (const QString &d : dirs) {
        if (isAborted()) { break; }
        emit setProgressValue(++progress);
        QCoreApplication::processEvents();

        SefamParsing::SessionData data;
        QString error;
        if (!SefamParsing::readSession(dir.absoluteFilePath(d), data, error)) {
            qWarning() << "Sefam:" << d << "skipped —" << error;
            ++skipped;
            continue;
        }

        const SessionID sid = data.header.utcEpoch != 0
                            ? static_cast<SessionID>(data.header.utcEpoch)
                            : static_cast<SessionID>(data.header.localStart.toSecsSinceEpoch());
        if (mach->SessionExists(sid)) { continue; }

        const qint64 startMs = data.header.localStart.toMSecsSinceEpoch();
        const qint64 endMs   = startMs
                             + static_cast<qint64>(data.recordCount)
                               * SefamParsing::kRecordSeconds * 1000LL;

        Session *session = new Session(mach, sid);
        session->SetChanged(true);
        session->really_set_first(startMs);
        session->really_set_last(endMs);

        if (data.settings.valid) { lastKnown = data.settings; }

        if (lastKnown.valid) {
            // A-PAP is the only mode observed on any SEFAM card examined.
            session->settings[CPAP_Mode]        = MODE_APAP;
            session->settings[CPAP_PressureMin] = lastKnown.minPressure;
            session->settings[CPAP_PressureMax] = lastKnown.maxPressure;
            if (lastKnown.rampMinutes > 0) {
                session->settings[CPAP_RampTime]     = lastKnown.rampMinutes;
                session->settings[CPAP_RampPressure] = lastKnown.rampPressure;
            }
            // Only the settings-change record carries this; a snapshot record
            // leaves it at -1 and the session then shows no humidifier level
            // rather than an inherited one.
            if (lastKnown.humidifierLevel >= 0) {
                session->settings[SEFAM_HumidLevel] = lastKnown.humidifierLevel;
            }
        }
        // Sessions before the first settings record on a card carry no settings
        // rather than inheriting from the future.

        const ChannelSpec flwSpec = data.channels.value("FLW");
        const ChannelSpec preSpec = data.channels.value("PRE");
        const ChannelSpec lkSpec  = data.channels.value("LK");
        const QVector<quint8> &flw = data.samples.value("FLW");
        const QVector<quint8> &pre = data.samples.value("PRE");
        const QVector<quint8> &lk  = data.samples.value("LK");

        // The card records through blower-off stretches, so find them once from
        // the pressure channel and exclude them from every waveform and from
        // usage time. Left in, they are counted as therapy: they drag the mean
        // pressure down and inflate the session length.
        const QVector<QPair<qint64, qint64>> offSpans =
            findBlowerOffSpans(pre, preSpec.freq);
        addMaskSlices(session, startMs, endMs, offSpans);

        // Scored events are deliberately left alone. Only 7 of 1568 on the
        // validated card fall inside a blower-off span, and they sit at the
        // detection boundaries; dropping them would disturb event counts that
        // were validated against the manufacturer's report to within one event.

        // Pressure: raw byte is tenths of a cmH2O, so store it as-is with gain 0.1.
        //
        // PRE is a MASK pressure — it carries the breathing ripple, about
        // 1.0 cmH2O peak to peak on the validated card — so it is imported as
        // CPAP_MaskPressure. The card carries no separate therapy-pressure
        // signal: PRE is the only channel declared in cmH2O, every other
        // populated channel (DET, NSD, Y17) is a bit field, and no log record
        // reports a pressure. CPAP_Pressure is therefore derived here by
        // averaging the ripple away, which OSCAR needs because that channel —
        // not CPAP_MaskPressure — drives the Pressure graph, the overview trend
        // and the pressure statistics.
        //
        // A moving average is used rather than a median because it is unbiased.
        // Measured against a breath-synchronous reference over 24 sessions, a
        // 10 s average leaves the session average pressure unchanged, so the
        // figures still match the manufacturer's report; a median of the same
        // width tracks the device's pressure ramps slightly better but shifts
        // every session average up by about 0.03 cmH2O, which nearly doubles
        // its overall error.
        if (!pre.isEmpty() && preSpec.freq > 0) {
            QVector<qint16> v(pre.size());
            QVector<bool>   ok(pre.size());
            for (int k = 0; k < pre.size(); ++k) {
                ok[k] = (pre.at(k) != SefamParsing::kInvalidSample);
                v[k]  = static_cast<qint16>(pre.at(k));
            }
            const int rateMs = 1000 / preSpec.freq;
            clearSpans(ok, offSpans, rateMs);
            importWaveform(session, v, ok, CPAP_MaskPressure, 0.1f, rateMs, startMs);

            QVector<qint16> smoothed;
            QVector<bool>   smoothedOk;
            movingAverage(v, ok, preSpec.freq * kPressureSmoothingSeconds,
                          smoothed, smoothedOk);
            importWaveform(session, smoothed, smoothedOk, CPAP_Pressure, 0.1f,
                           rateMs, startMs);
        }

        // Total leak: the device reports leak including the intentional mask vent.
        if (!lk.isEmpty() && lkSpec.freq > 0) {
            QVector<qint16> v(lk.size());
            QVector<bool>   ok(lk.size());
            for (int k = 0; k < lk.size(); ++k) {
                ok[k] = (lk.at(k) != SefamParsing::kInvalidSample);
                v[k]  = static_cast<qint16>(lk.at(k));
            }
            const int rateMs = 1000 / lkSpec.freq;
            clearSpans(ok, offSpans, rateMs);
            importWaveform(session, v, ok, CPAP_LeakTotal, 0.6f, rateMs, startMs);
        }

        // Flow: the card reports TOTAL flow, which sits about 22 L/min above zero
        // because it includes the mask vent. OSCAR's breath detector
        // (FlowParser::calcPeaks) hard-codes a zero crossing line, so it must be
        // given patient flow. Subtracting the device's own leak estimate centres
        // the signal on zero and yields a plausible breath rate; leaving it as
        // total flow produces no detected breaths at all, and hence no
        // respiratory rate, tidal volume, minute ventilation or Ti/Te.
        // Stored in tenths of a L/min so gain is an exact 0.1.
        if (!flw.isEmpty() && flwSpec.freq > 0 && !lk.isEmpty() && lkSpec.freq > 0) {
            const float flwGain = (flwSpec.max - flwSpec.min) / 255.0f;   // 460/255
            const float flwZero = -flwSpec.min / flwGain;                 // raw value meaning 0 L/min
            QVector<qint16> v(flw.size());
            QVector<bool>   ok(flw.size());
            for (int k = 0; k < flw.size(); ++k) {
                const int lkIndex = static_cast<int>(
                    static_cast<qint64>(k) * lkSpec.freq / flwSpec.freq);
                const bool haveLeak = lkIndex < lk.size()
                                   && lk.at(lkIndex) != SefamParsing::kInvalidSample;
                ok[k] = (flw.at(k) != SefamParsing::kInvalidSample) && haveLeak;
                if (!ok[k]) { v[k] = 0; continue; }

                const float totalFlow  = (flw.at(k) - flwZero) * flwGain;
                const float leakFlow   = lk.at(lkIndex) * 0.6f;
                v[k] = static_cast<qint16>(qRound((totalFlow - leakFlow) * 10.0f));
            }
            const int rateMs = 1000 / flwSpec.freq;
            clearSpans(ok, offSpans, rateMs);
            importWaveform(session, v, ok, CPAP_FlowRate, 0.1f, rateMs, startMs);
        }
        // DET, NSD and Y17 are deliberately not imported — see the design spec.

        // Events are placed relative to the session start so no timezone
        // arithmetic is needed. This requires the UTC epoch, which the short
        // header variant does not carry.
        if (!data.log.isEmpty() && data.header.utcEpoch != 0) {
            EventList *oa    = session->AddEventList(CPAP_Obstructive, EVL_Event);
            EventList *ca    = session->AddEventList(CPAP_ClearAirway, EVL_Event);
            EventList *hyp   = session->AddEventList(CPAP_Hypopnea,    EVL_Event);
            EventList *snore = session->AddEventList(CPAP_VSnore,      EVL_Event);
            EventList *fl    = session->AddEventList(CPAP_FlowLimit,   EVL_Event);

            for (const SefamParsing::LogRecord &rec : data.log) {
                const qint64 offsetMs = (rec.utcSeconds - data.header.utcEpoch) * 1000LL;
                if (offsetMs < 0 || offsetMs > (endMs - startMs)) { continue; }
                const qint64 when = startMs + offsetMs;

                // The log timestamp is the event END: reading it as a start
                // produces impossible overlaps between consecutive events, and
                // apnea flags placed there render visibly late against the
                // manufacturer's own waveform report.
                //
                // OSCAR flags are drawn as a bare vertical line at the event
                // timestamp — gFlagsLine's FLAG branch ignores the duration
                // entirely (it surfaces only in the tooltip). ResMed, the
                // reference loader, passes the EDF+ annotation *onset*, so the
                // convention is flag-at-start. Subtract the duration to match.
                const qint64 durMs = static_cast<qint64>(rec.arg) * 100;   // arg is 0.1 s

                switch (rec.code) {
                case SefamParsing::kLogObstructiveApnea:
                    oa->AddEvent(when - durMs, rec.arg / 10.0f); break;
                case SefamParsing::kLogCentralApnea:
                    ca->AddEvent(when - durMs, rec.arg / 10.0f); break;
                // OSCAR has no central hypopnea channel; bmcG3xDataParsing and
                // prisma_loader both fold both kinds into CPAP_Hypopnea. The
                // stored duration stays zero because the card does not report
                // one; only the flag placement is corrected, using the vendor's
                // published mean durations (see the constants above).
                case SefamParsing::kLogObstructiveHypop:
                    hyp->AddEvent(when - kObstructiveHypopneaPlacementMs, 0); break;
                case SefamParsing::kLogCentralHypopnea:
                    hyp->AddEvent(when - kCentralHypopneaPlacementMs, 0); break;
                case SefamParsing::kLogSnore:
                    snore->AddEvent(when, 0); break;
                case SefamParsing::kLogFlowLimitation:
                    fl->AddEvent(when, 0); break;
                default:
                    break;      // administrative codes are parsed but not imported
                }
            }
        } else if (!data.log.isEmpty()) {
            qWarning() << "Sefam:" << d
                       << "has no UTC epoch in its header — events skipped";
        }

        if (session->eventlist.isEmpty()) {
            qWarning() << "Sefam:" << d << "skipped — no valid samples";
            delete session;
            ++skipped;
            continue;
        }

        session->UpdateSummaries();
        totalSpanMs  += endMs - startMs;
        totalUsageMs += static_cast<qint64>(session->hours() * 3600000.0);
        mach->AddSession(session);
        ++imported;
    }

    mach->Save();
    finishAddingSessions();

    // Span is the time the machine was powered on, usage the time it was
    // treating; the difference is the blower-off total. Logged separately
    // because the manufacturer's report distinguishes them the same way.
    qDebug() << "Sefam TOTAL sessions imported" << imported << "skipped" << skipped
             << "span hours" << totalSpanMs / 3600000.0
             << "usage hours" << totalUsageMs / 3600000.0;
    return imported;
}

bool SefamLoader::backupData(Machine *mach, const QString &path)
{
    const QString serialDir = findSerialDir(path);
    if (serialDir.isEmpty()) { return false; }

    // Reconstruct the card's <modelcode>/<serial>/ layout under the backup root
    // rather than copying whatever the user happened to select. Import accepts
    // the card root, the model directory or the serial directory, so copying the
    // selection verbatim would produce a backup whose shape depends on how the
    // user navigated — and PeekInfo reads the model code from the serial
    // directory's parent, which would then be "Backup".
    const QDir    src(serialDir);
    const QString serialName = src.dirName();
    const QString modelCode  = QFileInfo(src.absolutePath()).dir().dirName();

    QDir backupRoot(mach->getBackupPath());
    const QDir dst(backupRoot.absoluteFilePath(modelCode + "/" + serialName));

    // Compare QDir objects rather than strings: separators differ on Windows.
    // This must compare the real source and destination, not just the selected
    // path against the backup root — importing from a path nested inside the
    // backup folder would otherwise pass the check, and copyPath() with
    // overwrite removes each destination file before copying, which for a
    // self-copy destroys the backup.
    if (src == dst) {
        rebuild_from_backups = true;
        create_backups = false;
    } else {
        rebuild_from_backups = false;
        create_backups = p_profile->session->backupCardData();
    }

    if (rebuild_from_backups || !create_backups) { return true; }

    QDir dir;
    if (!dir.exists(dst.absolutePath()) && !dir.mkpath(dst.absolutePath())) {
        qWarning() << "Sefam: could not create backup directory" << dst.absolutePath();
        return false;
    }

    emit updateMessage(QObject::tr("Creating data backup..."));
    QCoreApplication::processEvents();

    // The whole card is copied, including the encrypted .RAM/.BKP images. They
    // are not read by this loader, but they are the only place the format's
    // remaining unknowns could ever be answered from, so a backup that dropped
    // them would destroy the one artefact a future investigation would need.
    // A SEFAM card is around 31 MB, of which the images are about 3 MB.
    copyPath(src.absolutePath(), dst.absolutePath(), true);

    qDebug() << "Sefam: backed up card to" << dst.absolutePath();
    return true;
}
