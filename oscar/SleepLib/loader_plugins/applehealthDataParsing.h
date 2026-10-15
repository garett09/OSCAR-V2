/* SleepLib Apple Health Data Parsing Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef APPLEHEALTH_DATA_PARSING_H
#define APPLEHEALTH_DATA_PARSING_H

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <functional>

struct AppleHealthSample
{
    qint64 timeMs;
    float value;
    int sourceId;
};

struct AppleHealthInterval
{
    qint64 startMs;
    qint64 endMs;
    int stage;
    QString source;
};

struct AppleHealthNightScalar
{
    qint64 startMs;
    qint64 endMs;
    double value;
    int sourceId;
};

struct AppleHealthWeight
{
    qint64 timeMs;
    double kg;
};

struct AppleHealthData
{
    QVector<AppleHealthInterval> sleepStages;
    QVector<AppleHealthSample> heartRate;
    QVector<AppleHealthSample> spo2;
    QVector<AppleHealthSample> respRate;
    QVector<AppleHealthSample> hrv;
    QVector<AppleHealthNightScalar> breathingDisturbances;
    QVector<AppleHealthNightScalar> wristTemp;
    QVector<AppleHealthWeight> weights;
    QHash<QString, int> sleepSourceCounts;
    QStringList sourceNames;
    QHash<QString, int> vitalsSourceCounts;
    QHash<QString, qint64> typeCounts;
    qint64 recordsSeen = 0;
};

bool isAppleWatchSleepSource(const QString &sourceName);
bool isAppleFirstPartySource(const QString &sourceName);
bool isAllowedVitalsSource(const QString &sourceName, const QString &chosenSleepSource);
QString autoMatchedSleepSource(const QHash<QString, int> &sourceCounts);

class AppleHealthParser
{
  public:
    AppleHealthParser();

    void setCutoff(qint64 epochMsUtc);
    void setProgressCallback(std::function<void(qint64 bytesRead, qint64 bytesTotal)> cb);
    bool parse(const QString &path, AppleHealthData &out);
    QString errorString() const;

  private:
    qint64 m_cutoffMs = 0;
    QString m_coarseCutoffDate;
    QString m_error;
    std::function<void(qint64, qint64)> m_progressCallback;
};

#endif // APPLEHEALTH_DATA_PARSING_H
