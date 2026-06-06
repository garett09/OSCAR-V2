/* Clock Sync Analyzer Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef CLOCKSYNCANALYZER_H
#define CLOCKSYNCANALYZER_H

#include <QDate>
#include <QList>
#include <QVector>
#include <QString>
#include "SleepLib/machine.h"

struct NightSyncResult {
    QDate date;
    qint64 offsetMs = 0;   // negative = CPAP behind reference (positive = ahead)
    double confidence = 0; // normalized cross-correlation peak [0,1]
    int cpapEvents   = 0;  // CPAP apnea/hypopnea events used
    int oxiDrops     = 0;  // SpO2 drops detected from oximeter
    bool accepted    = false;
    QString note;
};

class ClockSyncAnalyzer
{
public:
    static constexpr double MIN_CONFIDENCE = 0.35;
    static constexpr int    MIN_EVENTS     = 15;
    static constexpr int    MIN_OXI_DROPS  = 5;
    static constexpr double BIN_MS         = 5000.0;
    static constexpr double SIGMA_MS       = 20000.0;
    static constexpr qint64 MAX_LAG_MS     = 3600000LL;  // ±60 min search

    static QList<NightSyncResult> analyze(Machine* cpap, Machine* oxi,
                                          const QDate& start, const QDate& end);

private:
    static QVector<double> buildDensity(const QList<qint64>& eventTimesMs,
                                        qint64 t0Ms, int nBins,
                                        double sigmaMs);
    static QList<qint64>   collectCpapEventTimes(Session* sess);
    static QList<qint64>   collectOxiDropTimes(Session* sess);
    static double          gaussianSum(const QVector<double>& a,
                                       const QVector<double>& b, int lagBins);
};

#endif // CLOCKSYNCANALYZER_H
