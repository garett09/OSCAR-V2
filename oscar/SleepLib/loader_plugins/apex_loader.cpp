/* Apex Medical XT Auto Loader Implementation
 *
 * Native import for the Apex XT Auto raw APAPDATA format. See
 * Notes/loaders/Apex/APEX_LOADER_DESIGN.md for the complete design.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPixmap>
#include <QtMath>

#include "apex_loader.h"

#include "SleepLib/common.h"
#include "SleepLib/importcontext.h"
#include "SleepLib/profiles.h"
#include "SleepLib/session.h"

namespace {

const QString kApfFilename = QStringLiteral("00000000.APF");
const QString kApeFilename = QStringLiteral("00000000.APE");
constexpr qint64 kApfFileSize = 21250;

bool hasApexSummaryFile(const QDir &dir)
{
    return QFileInfo(dir.absoluteFilePath(kApfFilename)).isFile();
}

QString findChildDirectory(const QDir &parent, const QString &name)
{
    const QFileInfoList children = parent.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &child : children) {
        if (child.fileName().compare(name, Qt::CaseInsensitive) == 0) {
            return child.absoluteFilePath();
        }
    }
    return QString();
}

bool readFile(const QString &filename, QByteArray &data)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) { return false; }
    data = file.readAll();
    return file.error() == QFileDevice::NoError;
}

} // namespace

static bool apex_initialised = false;

ApexLoader::ApexLoader()
{
    const QString icon = QStringLiteral(":/icons/apex-xt-auto.png");
    const QString series = newInfo().series;
    m_pixmap_paths[series] = icon;
    m_pixmaps[series] = QPixmap(icon);
    m_type = MT_CPAP;
}

ApexLoader::~ApexLoader() = default;

void ApexLoader::Register()
{
    if (apex_initialised) { return; }
    qDebug() << "Registering ApexLoader";
    RegisterLoader(new ApexLoader());
    apex_initialised = true;
}

MachineInfo ApexLoader::newInfo()
{
    return MachineInfo(MT_CPAP, 0, apex_class_name,
                       QStringLiteral("Apex Medical"),
                       QStringLiteral("XT Auto"),
                       QStringLiteral("XT Auto"),
                       QString(),
                       QStringLiteral("apex-xt-auto"),
                       QDateTime::currentDateTime(), apex_data_version);
}

QString ApexLoader::findDataDir(const QString &path)
{
    QDir selected(path);
    if (!selected.exists()) { return QString(); }

    // Files may be directly in the selected directory.
    if (hasApexSummaryFile(selected)) { return selected.absolutePath(); }

    // The user may also select APAPDATA itself.
    const QString directData = findChildDirectory(selected, QStringLiteral("00000000"));
    if (!directData.isEmpty()) {
        const QDir dir(directData);
        if (hasApexSummaryFile(dir)) { return dir.absolutePath(); }
    }

    // Normal card layout: <selected>/APAPDATA/00000000/. Enumerate rather
    // than constructing a case-sensitive path so copied cards and SD folders
    // carrying Hidden/System attributes work consistently on every platform.
    const QString apapData = findChildDirectory(selected, QStringLiteral("APAPDATA"));
    if (!apapData.isEmpty()) {
        const QDir apapDir(apapData);
        const QString nestedData = findChildDirectory(apapDir, QStringLiteral("00000000"));
        if (!nestedData.isEmpty()) {
            const QDir dir(nestedData);
            if (hasApexSummaryFile(dir)) { return dir.absolutePath(); }
        }
    }

    return QString();
}

bool ApexLoader::validateDataDir(const QString &dataPath, QByteArray *apfData)
{
    if (dataPath.isEmpty()) { return false; }

    const QDir dir(dataPath);
    const QFileInfo apfInfo(dir.absoluteFilePath(kApfFilename));
    if (apfInfo.size() != kApfFileSize) { return false; }

    QByteArray apf;
    if (!readFile(apfInfo.absoluteFilePath(), apf)
        || apf.size() < ApexParsing::kApfTableStartOffset + ApexParsing::kApfRecordSize) {
        return false;
    }

    ApexParsing::ApfRecord firstRecord;
    const quint8 *apfBytes = reinterpret_cast<const quint8 *>(apf.constData());
    if (!ApexParsing::decodeApfRecord(
            apfBytes + ApexParsing::kApfTableStartOffset, firstRecord)) {
        return false;
    }

    if (apfData) { *apfData = apf; }
    return true;
}

bool ApexLoader::Detect(const QString &path)
{
    const QString dataPath = findDataDir(path);
    if (!validateDataDir(dataPath)) { return false; }
    qDebug() << "ApexLoader::Detect matched" << dataPath;
    return true;
}

MachineInfo ApexLoader::PeekInfo(const QString &path)
{
    Q_UNUSED(path)
    // The raw card format contains neither a serial number nor firmware data.
    return newInfo();
}

bool ApexLoader::backupData(Machine *mach, const QString &path)
{
    const QString dataPath = findDataDir(path);
    if (dataPath.isEmpty()) { return false; }
    return backupDataDir(mach, dataPath);
}

bool ApexLoader::backupDataDir(Machine *mach, const QString &dataPath)
{
    const QDir src(dataPath);
    const QDir backupRoot(mach->getBackupPath());
    const QDir dst(backupRoot.absoluteFilePath(QStringLiteral("APAPDATA/00000000")));

    // Importing the canonical backup path must never invoke copyPath() on
    // itself: overwrite mode removes a destination file before copying it.
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
        qWarning() << "ApexLoader: could not create backup directory"
                   << dst.absolutePath();
        return false;
    }

    emit updateMessage(QObject::tr("Creating data backup..."));
    QCoreApplication::processEvents();

    copyPath(src.absolutePath(), dst.absolutePath(), true);
    qDebug() << "ApexLoader: backed up card data to" << dst.absolutePath();
    return true;
}

void ApexLoader::importSessionAverages(Session *session,
                                       const ApexParsing::ApfRecord &rec)
{
    const qint64 startMs = rec.start.toMSecsSinceEpoch();
    const qint64 endMs = rec.end.toMSecsSinceEpoch();
    importLeakChannel(session, rec, startMs, endMs);

    // Older sessions have no minute samples, so represent the only pressure
    // value the device supplies as a flat average-pressure trace.
    EventList *pressure = session->AddEventList(CPAP_Pressure, EVL_Event, 0.1f);
    const qint16 averagePressure = static_cast<qint16>(
        qRound(rec.averagePressure * 10.0f));
    pressure->AddEvent(startMs, averagePressure);
    pressure->AddEvent(endMs, averagePressure);

    // The APF record has graphable session-average pressure/leak channels even
    // when its older minute-detail ring entry is no longer available.
}

void ApexLoader::importSettings(Session *session, const ApexParsing::ApfRecord &rec)
{
    if (rec.isApap()) {
        session->settings[CPAP_Mode] = MODE_APAP;
        session->settings[CPAP_PressureMin] = rec.minPressure;
        session->settings[CPAP_PressureMax] = rec.maxPressure;
    } else {
        session->settings[CPAP_Mode] = MODE_CPAP;
        session->settings[CPAP_Pressure] = rec.minPressure;
    }
}

void ApexLoader::importLeakChannel(Session *session,
                                   const ApexParsing::ApfRecord &rec,
                                   qint64 startMs, qint64 endMs)
{
    if (endMs <= startMs) { return; }

    // OSCAR derives unintentional CPAP_Leak from CPAP_LeakTotal and pressure.
    // Apex exposes only a per-session average total leak, so identical boundary
    // values produce an honest flat summary trace rather than invented detail.
    EventList *totalLeak = session->AddEventList(CPAP_LeakTotal, EVL_Event, 1.0f);
    const qint16 leak = static_cast<qint16>(qRound(rec.averageLeak));
    totalLeak->AddEvent(startMs, leak);
    totalLeak->AddEvent(endMs, leak);
}

void ApexLoader::importMinuteDetail(
    Session *session, const ApexParsing::ApfRecord &rec,
    const QVector<ApexParsing::ApeMinuteRecord> &minutes, qint64 startMs)
{
    constexpr qint64 kMinuteMs = 60000;
    const qint64 sampleDuration = static_cast<qint64>(minutes.size()) * kMinuteMs;

    // The device writes fewer minute records than the session's wall-clock
    // span - typically one to three short - so the pressure waveform below ends
    // before rec.end. calcLeaks() derives CPAP_Leak only at timestamps where it
    // can look up a pressure, so a leak sample past the last minute sample is
    // silently discarded, leaving a one-sample list that gLineChart refuses to
    // draw at all. Ending both synthetic traces together costs at most the
    // final partial minute and keeps the derived leak graph intact.
    importLeakChannel(session, rec, startMs,
                      minutes.isEmpty() ? rec.end.toMSecsSinceEpoch()
                                        : qMin(rec.end.toMSecsSinceEpoch(),
                                               startMs + sampleDuration));

    QVector<qint16> pressure;
    pressure.reserve(minutes.size());
    for (const ApexParsing::ApeMinuteRecord &minute : minutes) {
        // The decoder exposes cmH2O; store tenths with gain 0.1 so the
        // waveform retains the device's original one-decimal precision.
        pressure.append(static_cast<qint16>(qRound(minute.pressure * 10.0f)));
    }

    if (!pressure.isEmpty()) {
        EventList *pressureEvents = session->AddEventList(
            CPAP_Pressure, EVL_Waveform, 0.1f, 0.0f, 0.0f, 0.0f, kMinuteMs);
        pressureEvents->AddWaveform(startMs, pressure.data(), pressure.size(),
                                    sampleDuration);
        session->really_set_last(qMax(rec.end.toMSecsSinceEpoch(),
                                      startMs + sampleDuration));
    }

    EventList *apneaEvents = nullptr;
    EventList *hypopneaEvents = nullptr;
    EventList *snoreEvents = nullptr;

    for (int i = 0; i < minutes.size(); ++i) {
        const ApexParsing::ApeMinuteRecord &minute = minutes.at(i);
        const qint64 when = startMs + static_cast<qint64>(i) * kMinuteMs;

        if (minute.apnea) {
            if (!apneaEvents) {
                apneaEvents = session->AddEventList(CPAP_Apnea, EVL_Event);
            }
            for (quint8 count = 0; count < minute.apnea; ++count) {
                apneaEvents->AddEvent(when, 0);
            }
        }
        if (minute.hypopnea) {
            if (!hypopneaEvents) {
                hypopneaEvents = session->AddEventList(CPAP_Hypopnea, EVL_Event);
            }
            for (quint8 count = 0; count < minute.hypopnea; ++count) {
                hypopneaEvents->AddEvent(when, 0);
            }
        }
        if (minute.snoring) {
            if (!snoreEvents) {
                snoreEvents = session->AddEventList(CPAP_VSnore, EVL_Event);
            }
            for (quint8 count = 0; count < minute.snoring; ++count) {
                snoreEvents->AddEvent(when, 0);
            }
        }
    }
}

int ApexLoader::Open(const QString &path)
{
    Q_ASSERT(m_ctx);
    const QString dataPath = findDataDir(path);
    QByteArray apfData;
    if (!validateDataDir(dataPath, &apfData)) { return -1; }

    const MachineInfo info = PeekInfo(path);
    m_ctx->CreateMachineFromInfo(info);
    Machine *mach = p_profile->CreateMachine(info);

    backupDataDir(mach, dataPath);

    const QDir dir(dataPath);
    QVector<ApexParsing::ApfRecord> records;
    QString error;
    if (!ApexParsing::parseApf(apfData, records, error)) {
        qWarning() << "ApexLoader:" << error;
        return -1;
    }

    QHash<QDateTime, QVector<ApexParsing::ApeMinuteRecord>> detailByStart;
    QByteArray apeData;
    if (!readFile(dir.absoluteFilePath(kApeFilename), apeData)
        || !ApexParsing::parseApe(apeData, detailByStart)) {
        qWarning() << "ApexLoader: .APE detail unavailable; importing summaries only";
        detailByStart.clear();
    }

    emit updateMessage(QObject::tr("Reading Apex Medical card..."));
    emit setProgressMax(records.size());
    emit setProgressValue(0);

    int imported = 0;
    int progress = 0;
    for (const ApexParsing::ApfRecord &rec : records) {
        if (isAborted()) { break; }
        emit setProgressValue(++progress);
        QCoreApplication::processEvents();

        if (rec.end <= rec.start) {
            qWarning() << "ApexLoader: skipping session with non-positive duration"
                       << rec.start << rec.end;
            continue;
        }

        const SessionID sid = static_cast<SessionID>(rec.start.toSecsSinceEpoch());
        if (mach->SessionExists(sid)) { continue; }

        Session *session = new Session(mach, sid);
        session->SetChanged(true);
        session->really_set_first(rec.start.toMSecsSinceEpoch());
        session->really_set_last(rec.end.toMSecsSinceEpoch());

        importSettings(session, rec);

        const auto detail = detailByStart.constFind(rec.start);
        if (detail != detailByStart.cend() && !detail.value().isEmpty()) {
            importMinuteDetail(session, rec, detail.value(), rec.start.toMSecsSinceEpoch());
        } else {
            importSessionAverages(session, rec);
        }
        session->UpdateSummaries();
        if (mach->AddSession(session)) {
            ++imported;
        } else {
            delete session;
        }
    }

    mach->Save();
    finishAddingSessions();
    qDebug() << "Apex TOTAL sessions imported" << imported;
    return imported;
}
