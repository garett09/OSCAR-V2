/* SleepLib Apple Health Loader Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include <QDebug>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QTime>

#include <algorithm>
#include <limits>
#include <memory>

#include "applehealth_loader.h"
#include "SleepLib/journal.h"
#include "SleepLib/machine.h"
#include "SleepLib/session.h"
#include "zip.h"

namespace {

static QVector<AppleHealthSample> samplesInWindow(
    const QVector<AppleHealthSample> &samples, qint64 startMs, qint64 endMs)
{
    QVector<AppleHealthSample> filtered;
    filtered.reserve(samples.size());
    for (const AppleHealthSample &sample : samples) {
        if (sample.timeMs >= startMs && sample.timeMs <= endMs) {
            filtered.append(sample);
        }
    }
    return filtered;
}

} // namespace

AppleHealthLoader::AppleHealthLoader()
{
    m_type = MT_OXIMETER;
}

AppleHealthLoader::~AppleHealthLoader()
{
}

bool AppleHealthLoader::Detect(const QString & path)
{
    Q_UNUSED(path);
    return false;
}

int AppleHealthLoader::Open(const QStringList &paths)
{
    m_lastImportSummary = AppleHealthImportSummary();
    m_chosenSleepSources.clear();
    m_importSkippedNights.clear();
    const bool previousForwarding = m_forwardParserProgress;
    const bool previousAccumulating = m_accumulatingImportSummary;
    m_forwardParserProgress = (paths.size() == 1);
    m_accumulatingImportSummary = true;
    const int result = MachineLoader::Open(paths);
    m_accumulatingImportSummary = previousAccumulating;
    m_forwardParserProgress = previousForwarding;
    return result;
}

int AppleHealthLoader::OpenFile(const QString & filename)
{
    if (!m_accumulatingImportSummary) {
        m_lastImportSummary = AppleHealthImportSummary();
        m_chosenSleepSources.clear();
        m_importSkippedNights.clear();
    }
    m_data = AppleHealthData();
    m_session = nullptr;
    m_importChannels.clear();
    m_importLastValue.clear();

    QString importFilename = filename;
    std::unique_ptr<QTemporaryDir> tempDir;
    if (filename.endsWith(".zip", Qt::CaseInsensitive)) {
        tempDir = std::make_unique<QTemporaryDir>();
        if (!tempDir->isValid()) {
            qWarning() << "AppleHealthLoader::OpenFile: could not create temporary directory";
            return -1;
        }

        UnzipFile archive;
        if (!archive.Open(filename)) {
            qWarning() << "AppleHealthLoader::OpenFile: could not open ZIP archive:" << filename;
            return -1;
        }
        const QString standardEntry = QStringLiteral("apple_health_export/export.xml");
        const QVector<UnzipEntry> entries = archive.ListEntries();
        QString exportEntry;
        for (const UnzipEntry &entry : entries) {
            if (entry.name == standardEntry) {
                exportEntry = entry.name;
                break;
            }
        }
        if (exportEntry.isEmpty()) {
            qint64 largestSize = -1;
            for (const UnzipEntry &entry : entries) {
                const QString fileName = QFileInfo(entry.name).fileName();
                if (!entry.name.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)
                    || fileName.contains(QStringLiteral("_cda"), Qt::CaseInsensitive)) {
                    continue;
                }
                if (entry.uncompressedSize > largestSize) {
                    exportEntry = entry.name;
                    largestSize = entry.uncompressedSize;
                }
            }
        }
        if (exportEntry.isEmpty()) {
            qWarning() << "AppleHealthLoader::OpenFile: could not find export XML in:" << filename;
            return -1;
        }
        if (exportEntry != standardEntry) {
            qWarning() << "AppleHealthLoader::OpenFile: using ZIP export entry:" << exportEntry;
        }
        importFilename = tempDir->path() + QStringLiteral("/export.xml");
        // export.xml can be hundreds of MB; without progress the UI looks hung during extraction.
        int lastPercent = -1;
        if (!archive.ExtractEntry(exportEntry, importFilename,
                                  [this, &lastPercent](qint64 bytesWritten, qint64 bytesTotal) {
                                      if (!m_forwardParserProgress || bytesTotal <= 0) {
                                          return;
                                      }
                                      const int percent = qBound(
                                          0, static_cast<int>((bytesWritten * 100) / bytesTotal), 100);
                                      if (percent != lastPercent) {
                                          lastPercent = percent;
                                          emit setProgressValue(percent);
                                      }
                                  })) {
            qWarning() << "AppleHealthLoader::OpenFile: could not extract" << exportEntry
                       << "from:" << filename;
            return -1;
        }
    }

    QFileInfo fileInfo(importFilename);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        qDebug() << "AppleHealthLoader::OpenFile: file does not exist:" << importFilename;
        return -1;
    }

    QFile file(importFilename);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "AppleHealthLoader::OpenFile: could not open:" << importFilename;
        return -1;
    }

    const QByteArray prefix = file.read(8192);
    if (!prefix.contains("HealthData") && !prefix.contains("HealthKit Export")) {
        qDebug() << "AppleHealthLoader::OpenFile: file does not look like an Apple Health export:" << importFilename;
        return -1;
    }
    file.close();

    AppleHealthParser parser;
    parser.setProgressCallback([this](qint64 bytesRead, qint64 bytesTotal) {
        if (m_forwardParserProgress && bytesTotal > 0) {
            const int percent = qBound(0, static_cast<int>((bytesRead * 100) / bytesTotal), 100);
            emit setProgressValue(percent);
        }
    });
    qint64 cutoffMs = 0;
    if (!m_importFullHistory && p_profile != nullptr) {
        const QDate firstCpapDay = p_profile->FirstDay(MT_CPAP);
        if (firstCpapDay.isValid() && p_profile->FindDay(firstCpapDay, MT_CPAP) != nullptr) {
            cutoffMs = QDateTime(firstCpapDay.addDays(-7), QTime(0, 0), Qt::LocalTime)
                           .toMSecsSinceEpoch();
        }
    }
    parser.setCutoff(cutoffMs);
    if (!parser.parse(importFilename, m_data)) {
        qWarning() << "AppleHealthLoader::OpenFile:" << parser.errorString();
        m_data = AppleHealthData();
        return -1;
    }

    QString chosenSource = autoMatchedSleepSource(m_data.sleepSourceCounts);
    if (m_sleepSourceChooser && m_data.sleepSourceCounts.size() > 1) {
        const QString requestedSource = m_sleepSourceChooser(m_data.sleepSourceCounts);
        if (requestedSource.isEmpty()) {
            // Chooser dismissed: abort before anything is written.
            abort();
            m_data = AppleHealthData();
            return 0;
        }
        chosenSource = requestedSource;
    }
    QVector<AppleHealthInterval> selectedStages;
    selectedStages.reserve(m_data.sleepStages.size());
    if (!chosenSource.isEmpty()) {
        for (const AppleHealthInterval &interval : m_data.sleepStages) {
            if (interval.source == chosenSource) {
                selectedStages.append(interval);
            }
        }
    }
    m_data.sleepStages = selectedStages;
    m_lastImportSummary.validFile = true;
    for (auto it = m_data.sleepSourceCounts.cbegin(); it != m_data.sleepSourceCounts.cend(); ++it) {
        m_lastImportSummary.sleepSourceCounts[it.key()] += it.value();
    }
    if (!chosenSource.isEmpty() && !m_chosenSleepSources.contains(chosenSource)) {
        m_chosenSleepSources.append(chosenSource);
        m_lastImportSummary.chosenSleepSource = m_chosenSleepSources.join(QStringLiteral(", "));
    }

    if (!m_data.sleepStages.isEmpty()) {
        qDebug() << "AppleHealthLoader::OpenFile: stages:" << m_data.sleepStages.size();
    }
    if (!m_data.heartRate.isEmpty()) {
        qDebug() << "AppleHealthLoader::OpenFile: hr:" << m_data.heartRate.size();
    }
    if (!m_data.spo2.isEmpty()) {
        qDebug() << "AppleHealthLoader::OpenFile: spo2:" << m_data.spo2.size();
    }
    if (!m_data.respRate.isEmpty()) {
        qDebug() << "AppleHealthLoader::OpenFile: resp:" << m_data.respRate.size();
    }
    if (!m_data.hrv.isEmpty()) {
        qDebug() << "AppleHealthLoader::OpenFile: hrv:" << m_data.hrv.size();
    }
    if (!m_data.breathingDisturbances.isEmpty()) {
        qDebug() << "AppleHealthLoader::OpenFile: bd:"
                 << m_data.breathingDisturbances.size();
    }
    if (!m_data.wristTemp.isEmpty()) {
        qDebug() << "AppleHealthLoader::OpenFile: wristTemp:" << m_data.wristTemp.size();
    }
    qDebug() << "AppleHealthLoader::OpenFile: recordsSeen:" << m_data.recordsSeen;
    qDebug() << "AppleHealthLoader::OpenFile: sleepSourceCounts:" << m_data.sleepSourceCounts;

    QMap<QDate, QVector<AppleHealthInterval>> stagesByNight;
    QMap<QDate, QVector<AppleHealthSample>> heartRateByNight;
    QMap<QDate, QVector<AppleHealthSample>> spo2ByNight;
    QMap<QDate, QVector<AppleHealthSample>> respRateByNight;
    QMap<QDate, QVector<AppleHealthSample>> hrvByNight;
    QMap<QDate, QVector<AppleHealthNightScalar>> disturbancesByNight;
    QMap<QDate, QVector<AppleHealthNightScalar>> wristTempByNight;

    for (const AppleHealthInterval &interval : m_data.sleepStages) {
        if (interval.endMs <= interval.startMs || interval.stage < Stage_Awake || interval.stage > Stage_Deep) {
            continue;
        }
        const QDate night = nightDate(interval.startMs);
        stagesByNight[night].append(interval);
    }
    for (const AppleHealthSample &sample : m_data.heartRate) {
        const QDate night = nightDate(sample.timeMs);
        heartRateByNight[night].append(sample);
    }
    for (const AppleHealthSample &sample : m_data.spo2) {
        spo2ByNight[nightDate(sample.timeMs)].append(sample);
    }
    for (const AppleHealthSample &sample : m_data.respRate) {
        respRateByNight[nightDate(sample.timeMs)].append(sample);
    }
    for (const AppleHealthSample &sample : m_data.hrv) {
        hrvByNight[nightDate(sample.timeMs)].append(sample);
    }
    for (const AppleHealthNightScalar &scalar : m_data.breathingDisturbances) {
        disturbancesByNight[nightDate(scalar.startMs)].append(scalar);
    }
    for (const AppleHealthNightScalar &scalar : m_data.wristTemp) {
        wristTempByNight[nightDate(scalar.startMs)].append(scalar);
    }

    const auto sortSamples = [](auto &samplesByNight) {
        for (auto it = samplesByNight.begin(); it != samplesByNight.end(); ++it) {
            std::sort(it.value().begin(), it.value().end(),
                      [](const AppleHealthSample &a, const AppleHealthSample &b) {
                          return a.timeMs < b.timeMs;
                      });
        }
    };
    sortSamples(heartRateByNight);
    sortSamples(spo2ByNight);
    sortSamples(respRateByNight);
    sortSamples(hrvByNight);
    for (auto it = stagesByNight.begin(); it != stagesByNight.end(); ++it) {
        std::sort(it.value().begin(), it.value().end(),
                  [](const AppleHealthInterval &a, const AppleHealthInterval &b) {
                      if (a.startMs != b.startMs) {
                          return a.startMs < b.startMs;
                      }
                      return a.endMs < b.endMs;
                  });
    }

    const QVector<AppleHealthSample> noSamples;
    const QVector<AppleHealthNightScalar> noScalars;
    Machine *sleepMach = nullptr;
    Machine *oxiMach = nullptr;
    QSet<QDate> existingSleepNights;
    QSet<QDate> existingOxiNights;
    for (Machine *machine : p_profile->GetMachines(MT_SLEEPSTAGE)) {
        if (machine->loaderName() == applehealth_class_name) {
            sleepMach = machine;
        }
        for (Session *session : machine->sessionlist) {
            existingSleepNights.insert(nightDate(session->first()));
        }
    }
    for (Machine *machine : p_profile->GetMachines(MT_OXIMETER)) {
        if (machine->loaderName() == applehealth_class_name) {
            oxiMach = machine;
        }
        for (Session *session : machine->sessionlist) {
            existingOxiNights.insert(nightDate(session->first()));
        }
    }
    bool sleepChanged = false;
    bool oxiChanged = false;
    int imported = 0;
    QSet<QDate> skippedExistingNights;

    // Stages are published only after a night ends, so the newest night is never partial.
    for (auto nightIt = stagesByNight.cbegin(); nightIt != stagesByNight.cend(); ++nightIt) {
        const QDate night = nightIt.key();
        const auto heartRateIt = heartRateByNight.constFind(night);
        const auto spo2It = spo2ByNight.constFind(night);
        const auto respRateIt = respRateByNight.constFind(night);
        const auto hrvIt = hrvByNight.constFind(night);
        const auto disturbancesIt = disturbancesByNight.constFind(night);
        const auto wristTempIt = wristTempByNight.constFind(night);

        const QVector<AppleHealthInterval> &stages = nightIt.value();
        const QVector<AppleHealthSample> &heartRate =
            heartRateIt != heartRateByNight.cend() ? heartRateIt.value() : noSamples;
        const QVector<AppleHealthSample> &spo2 =
            spo2It != spo2ByNight.cend() ? spo2It.value() : noSamples;
        const QVector<AppleHealthSample> &respRate =
            respRateIt != respRateByNight.cend() ? respRateIt.value() : noSamples;
        const QVector<AppleHealthSample> &hrv =
            hrvIt != hrvByNight.cend() ? hrvIt.value() : noSamples;
        const QVector<AppleHealthNightScalar> &disturbances =
            disturbancesIt != disturbancesByNight.cend() ? disturbancesIt.value() : noScalars;
        const QVector<AppleHealthNightScalar> &wristTemp =
            wristTempIt != wristTempByNight.cend() ? wristTempIt.value() : noScalars;

        const qint64 windowStartMs = stages.constFirst().startMs;
        qint64 windowEndMs = stages.constFirst().endMs;
        for (const AppleHealthInterval &interval : stages) {
            windowEndMs = std::max(windowEndMs, interval.endMs);
        }

        // Watch samples run all day; trim them so the Daily view axis spans only sleep.
        const QVector<AppleHealthSample> filteredHeartRate =
            samplesInWindow(heartRate, windowStartMs, windowEndMs);
        const QVector<AppleHealthSample> filteredSpo2 =
            samplesInWindow(spo2, windowStartMs, windowEndMs);
        const QVector<AppleHealthSample> filteredRespRate =
            samplesInWindow(respRate, windowStartMs, windowEndMs);
        const QVector<AppleHealthSample> filteredHrv =
            samplesInWindow(hrv, windowStartMs, windowEndMs);

        if (existingSleepNights.contains(night)) {
            skippedExistingNights.insert(night);
        } else {
            if (sleepMach == nullptr) {
                sleepMach = p_profile->CreateMachine(newInfoSleep());
            }
            Session *session = buildSleepSession(
                sleepMach, stages, filteredRespRate, filteredHrv, disturbances, wristTemp);
            if (session != nullptr) {
                session->SetChanged(true);
                session->UpdateSummaries();
                if (sleepMach->AddSession(session)) {
                    sleepChanged = true;
                    ++imported;
                    ++m_lastImportSummary.sleepSessions;
                } else {
                    delete session;
                }
            } else {
                skippedExistingNights.insert(night);
            }
        }

        if (!filteredHeartRate.isEmpty() || !filteredSpo2.isEmpty()) {
            if (existingOxiNights.contains(night)) {
                skippedExistingNights.insert(night);
            } else {
                if (oxiMach == nullptr) {
                    oxiMach = p_profile->CreateMachine(newInfo());
                }
                Session *session = buildOxiSession(oxiMach, filteredHeartRate, filteredSpo2);
                if (session != nullptr) {
                    session->SetChanged(true);
                    session->UpdateSummaries();
                    if (oxiMach->AddSession(session)) {
                        oxiChanged = true;
                        ++imported;
                        ++m_lastImportSummary.oxiSessions;
                    } else {
                        delete session;
                    }
                } else {
                    skippedExistingNights.insert(night);
                }
            }
        }
    }

    m_importSkippedNights.unite(skippedExistingNights);
    m_lastImportSummary.skippedExisting = m_importSkippedNights.size();

    QMap<QDate, AppleHealthWeight> weightsByNight;
    for (const AppleHealthWeight &weight : m_data.weights) {
        const QDate night = nightDate(weight.timeMs);
        const auto existing = weightsByNight.constFind(night);
        if (existing == weightsByNight.cend() || existing.value().timeMs <= weight.timeMs) {
            weightsByNight.insert(night, weight);
        }
    }

    Machine *journalMach = nullptr;
    bool journalChanged = false;
    for (auto it = weightsByNight.cbegin(); it != weightsByNight.cend(); ++it) {
        Session *journal = GetOrCreateJournalSession(it.key());
        // A weight already in the journal is the user's; imports never overwrite it
        // (as Journal::RestoreDay, same epsilon).
        if (journal == nullptr
            || (journal->settings.contains(Journal_Weight)
                && journal->settings[Journal_Weight].toDouble() > 0.0001)) {
            continue;
        }

        const double kg = it.value().kg;
        journal->settings[Journal_Weight] = kg;
        const double heightCm = p_profile->user->height();
        if (heightCm > 0.0001) {
            const double heightM = heightCm / 100.0;
            journal->settings[Journal_BMI] = kg / (heightM * heightM);
        }
        journal->settings[LastUpdated] = QDateTime::currentDateTime();
        journal->SetChanged(true);
        journalMach = journal->machine();
        journalChanged = true;
        ++imported;
        ++m_lastImportSummary.weightDays;
    }

    if (sleepChanged) {
        sleepMach->Save();
        sleepMach->SaveSummaryCache();
    }
    if (oxiChanged) {
        oxiMach->Save();
        oxiMach->SaveSummaryCache();
    }
    if (journalChanged) {
        journalMach->Save();
        journalMach->SaveSummaryCache();
    }
    if (imported > 0) {
        p_profile->StoreMachines();
    }

    m_data = AppleHealthData();
    m_session = nullptr;
    m_importChannels.clear();
    m_importLastValue.clear();
    return imported;
}

QDate AppleHealthLoader::nightDate(qint64 timeMs) const
{
    const QDateTime localTime = QDateTime::fromMSecsSinceEpoch(timeMs, Qt::LocalTime);
    return localTime.time() < QTime(12, 0) ? localTime.date().addDays(-1) : localTime.date();
}

Session *AppleHealthLoader::buildSleepSession(
    Machine *mach, const QVector<AppleHealthInterval> &stages,
    const QVector<AppleHealthSample> &respRate, const QVector<AppleHealthSample> &hrv,
    const QVector<AppleHealthNightScalar> &breathingDisturbances,
    const QVector<AppleHealthNightScalar> &wristTemp)
{
    const qint64 firstMs = stages.constFirst().startMs;
    qint64 lastMs = stages.constFirst().endMs;
    for (const AppleHealthInterval &interval : stages) {
        lastMs = std::max(lastMs, interval.endMs);
    }

    const SessionID sessionId = static_cast<SessionID>(firstMs / 1000L);
    if (mach->SessionExists(sessionId)) {
        return nullptr;
    }

    Session *session = new Session(mach, sessionId);
    m_session = session;
    m_importChannels.clear();
    m_importLastValue.clear();
    session->really_set_first(firstMs);
    session->really_set_last(lastMs);

    static constexpr qint64 runToleranceMs = 15LL * 60LL * 1000LL;
    qint64 runStartMs = stages.constFirst().startMs;
    qint64 runEndMs = stages.constFirst().endMs;
    for (int i = 1; i < stages.size(); ++i) {
        const AppleHealthInterval &interval = stages.at(i);
        // Gap from the run's furthest end, so nested intervals can't open an overlapping slice.
        if (interval.startMs - runEndMs > runToleranceMs) {
            session->m_slices.append(SessionSlice(runStartMs, runEndMs, MaskOn));
            runStartMs = interval.startMs;
            runEndMs = interval.endMs;
        } else {
            runEndMs = std::max(runEndMs, interval.endMs);
        }
    }
    session->m_slices.append(SessionSlice(runStartMs, runEndMs, MaskOn));

    qint64 stageTimeMs[Stage_Deep + 1] = {0, 0, 0, 0, 0};
    int awakenings = 0;
    bool foundSleep = false;
    for (const AppleHealthInterval &interval : stages) {
        stageTimeMs[interval.stage] += interval.endMs - interval.startMs;
        if (interval.stage == Stage_Awake) {
            if (foundSleep) {
                ++awakenings;
            }
        } else {
            foundSleep = true;
        }

        AddEvent(SLEEP_Stage, interval.startMs, -interval.stage);
        EndEventList(SLEEP_Stage, interval.endMs);
    }

    importSamples(AW_RespRate, respRate, 30LL * 60LL * 1000LL);
    importSamples(AW_HRV, hrv, 0);

    session->settings[SLEEP_TimeInWake] = stageTimeMs[Stage_Awake] / 60000L;
    session->settings[SLEEP_TimeInREM] = stageTimeMs[Stage_REM] / 60000L;
    session->settings[SLEEP_TimeInLight] = stageTimeMs[Stage_Light] / 60000L;
    session->settings[SLEEP_TimeInDeep] = stageTimeMs[Stage_Deep] / 60000L;
    session->settings[SLEEP_Awakenings] = awakenings;

    if (!breathingDisturbances.isEmpty()) {
        const auto latest = std::max_element(
            breathingDisturbances.cbegin(), breathingDisturbances.cend(),
            [](const AppleHealthNightScalar &a, const AppleHealthNightScalar &b) {
                return a.startMs < b.startMs;
            });
        session->settings[AW_BreathingDisturbances] = latest->value;
    }
    if (!wristTemp.isEmpty()) {
        const auto latest = std::max_element(
            wristTemp.cbegin(), wristTemp.cend(),
            [](const AppleHealthNightScalar &a, const AppleHealthNightScalar &b) {
                return a.startMs < b.startMs;
            });
        session->settings[AW_WristTemp] = latest->value;
    }

    return session;
}

Session *AppleHealthLoader::buildOxiSession(
    Machine *mach, const QVector<AppleHealthSample> &heartRate,
    const QVector<AppleHealthSample> &spo2)
{
    qint64 firstMs = std::numeric_limits<qint64>::max();
    qint64 lastMs = 0;
    const auto updateTimes = [&firstMs, &lastMs](const QVector<AppleHealthSample> &samples) {
        if (!samples.isEmpty()) {
            firstMs = std::min(firstMs, samples.constFirst().timeMs);
            lastMs = std::max(lastMs, samples.constLast().timeMs);
        }
    };
    updateTimes(heartRate);
    updateTimes(spo2);

    const SessionID sessionId = static_cast<SessionID>(firstMs / 1000L);
    if (mach->SessionExists(sessionId)) {
        return nullptr;
    }

    Session *session = new Session(mach, sessionId);
    m_session = session;
    m_importChannels.clear();
    m_importLastValue.clear();
    session->really_set_first(firstMs);
    session->really_set_last(lastMs);

    importSamples(OXI_Pulse, heartRate, 10LL * 60LL * 1000LL);
    importSamples(OXI_SPO2, spo2, 45LL * 60LL * 1000LL);

    return session;
}

void AppleHealthLoader::importSamples(ChannelID channel,
                                      const QVector<AppleHealthSample> &samples,
                                      qint64 gapThresholdMs)
{
    qint64 previousMs = 0;
    for (const AppleHealthSample &sample : samples) {
        if (previousMs != 0 && gapThresholdMs > 0
            && sample.timeMs - previousMs > gapThresholdMs) {
            EndSampleList(channel);
        }
        AddSample(channel, sample.timeMs, sample.value);
        previousMs = sample.timeMs;
    }
    EndSampleList(channel);
}

void AppleHealthLoader::AddEvent(ChannelID channel, qint64 timeMs, EventDataType value)
{
    EventList *eventList = m_importChannels[channel];
    if (eventList == nullptr) {
        eventList = m_session->AddEventList(channel, EVL_Event, 1, 0, -5, 0);
        Q_ASSERT(eventList);
        m_importChannels[channel] = eventList;
    }
    eventList->AddEvent(timeMs, value);
    m_importLastValue[channel] = value;
}

void AppleHealthLoader::EndEventList(ChannelID channel, qint64 timeMs)
{
    EventList *eventList = m_importChannels[channel];
    if (eventList != nullptr) {
        eventList->AddEvent(timeMs, m_importLastValue[channel]);
        m_importChannels[channel] = nullptr;
    }
}

void AppleHealthLoader::AddSample(ChannelID channel, qint64 timeMs, EventDataType value)
{
    static const EventDataType gain = 0.1F;
    EventList *eventList = m_importChannels[channel];
    if (eventList == nullptr) {
        eventList = m_session->AddEventList(channel, EVL_Event, gain);
        Q_ASSERT(eventList);
        m_importChannels[channel] = eventList;
    }
    eventList->AddEvent(timeMs, qRound(value / gain));
}

void AppleHealthLoader::EndSampleList(ChannelID channel)
{
    if (m_importChannels[channel] != nullptr) {
        m_importChannels[channel] = nullptr;
    }
}

static bool applehealth_initialized = false;

void AppleHealthLoader::Register()
{
    if (applehealth_initialized) { return; }

    qDebug("Registering AppleHealthLoader");
    RegisterLoader(new AppleHealthLoader());
    applehealth_initialized = true;
}
