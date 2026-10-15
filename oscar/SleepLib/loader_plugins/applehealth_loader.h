/* SleepLib Apple Health Loader Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef APPLEHEALTHLOADER_H
#define APPLEHEALTHLOADER_H

#include "SleepLib/machine_loader.h"
#include "applehealthDataParsing.h"

#include <QSet>

#include <functional>

const QString applehealth_class_name = "AppleHealth";
const int applehealth_data_version = 1;

struct AppleHealthImportSummary
{
    int sleepSessions = 0;
    int oxiSessions = 0;
    int weightDays = 0;
    int skippedExisting = 0;
    QHash<QString, int> sleepSourceCounts;
    QHash<QString, int> ignoredVitalsCounts;
    QStringList importedVitalsSources;
    QString chosenSleepSource;
    bool validFile = false;
};

class AppleHealthLoader : public MachineLoader
{
  public:
    AppleHealthLoader();
    virtual ~AppleHealthLoader();

    virtual bool Detect(const QString & path);

    virtual int Open(const QString & path) { Q_UNUSED(path); return 0; }
    virtual int Open(const QStringList & paths) override;
    virtual int OpenFile(const QString & path);
    virtual QStringList getNameFilter() { return QStringList("Apple Health Export (*.xml *.zip)"); }
    static void Register();

    virtual int Version() { return applehealth_data_version; }
    virtual const QString &loaderName() { return applehealth_class_name; }

    virtual MachineInfo newInfo() {
        return MachineInfo(MT_OXIMETER, 0, applehealth_class_name, QObject::tr("Apple"), QObject::tr("Watch"), QString(), QStringLiteral("Vitals"), QObject::tr("Apple Health"), QDateTime::currentDateTime(), applehealth_data_version);
    }

    MachineInfo newInfoSleep() {
        return MachineInfo(MT_SLEEPSTAGE, 0, applehealth_class_name, QObject::tr("Apple"), QObject::tr("Watch Sleep"), QString(), QStringLiteral("Sleep"), QObject::tr("Apple Health"), QDateTime::currentDateTime(), applehealth_data_version);
    }

    void setSleepSourceChooser(
        const std::function<QString(const QHash<QString, int> &)> &chooser)
    {
        m_sleepSourceChooser = chooser;
    }
    void setImportFullHistory(bool fullHistory) { m_importFullHistory = fullHistory; }
    const AppleHealthImportSummary &lastImportSummary() const { return m_lastImportSummary; }

  private:
    QDate nightDate(qint64 timeMs) const;
    Session *buildSleepSession(Machine *mach,
                               const QVector<AppleHealthInterval> &stages,
                               const QVector<AppleHealthSample> &respRate,
                               const QVector<AppleHealthSample> &hrv,
                               const QVector<AppleHealthNightScalar> &breathingDisturbances,
                               const QVector<AppleHealthNightScalar> &wristTemp);
    Session *buildOxiSession(Machine *mach,
                             const QVector<AppleHealthSample> &heartRate,
                             const QVector<AppleHealthSample> &spo2);
    void importSamples(ChannelID channel, const QVector<AppleHealthSample> &samples,
                       qint64 gapThresholdMs);
    void AddEvent(ChannelID channel, qint64 timeMs, EventDataType value);
    void EndEventList(ChannelID channel, qint64 timeMs);
    void AddSample(ChannelID channel, qint64 timeMs, EventDataType value);
    void EndSampleList(ChannelID channel);

    AppleHealthData m_data;
    Session *m_session = nullptr;
    QHash<ChannelID, EventList *> m_importChannels;
    QHash<ChannelID, EventDataType> m_importLastValue;
    std::function<QString(const QHash<QString, int> &)> m_sleepSourceChooser;
    bool m_importFullHistory = false;
    bool m_forwardParserProgress = true;
    bool m_accumulatingImportSummary = false;
    QStringList m_chosenSleepSources;
    QStringList m_importedVitalsSources;
    QSet<QDate> m_importSkippedNights;
    AppleHealthImportSummary m_lastImportSummary;
};

#endif // APPLEHEALTHLOADER_H
