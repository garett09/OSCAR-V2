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

//! \brief Result of cross-correlating one reference event stream against CPAP events.
struct StreamCorrelation {
    qint64 lagMs    = 0;     // raw detected lag; >0 = reference events after CPAP events
    double score    = 0;     // normalized cross-correlation peak [0,1]
    double zscore   = 0;     // peak prominence vs the rest of the lag curve
    bool   accepted = false;
    bool   edgePeak = false; // peak landed at the search-window edge (unreliable)
};

//! \brief Combined per-night offset estimate after physiological-delay compensation.
struct NightEstimate {
    qint64  offsetMs   = 0;
    double  confidence = 0;
    double  zscore     = 0;
    bool    accepted   = false;
    QString note;
};

struct NightSyncResult {
    QDate date;
    qint64 offsetMs = 0;     // negative = CPAP behind reference (positive = ahead)
    double confidence = 0;   // normalized cross-correlation peak [0,1]
    double zscore = 0;       // peak prominence (primary acceptance gate)
    int cpapEvents   = 0;    // CPAP apnea/hypopnea events used
    int oxiDrops     = 0;    // SpO2 drops detected from oximeter
    int pulseSurges  = 0;    // pulse-rate surges detected from oximeter
    bool accepted    = false;
    bool driftOutlier = false; // residual vs cross-night drift fit too large
    QString note;
};

//! \brief Robust cross-night drift model: offset(date) = interceptMs + slopeMsPerDay * days.
struct DriftFit {
    bool   valid = false;
    double slopeMsPerDay = 0;
    double interceptMs   = 0;  // offset at refDate
    QDate  refDate;            // first accepted night
    int    nUsed = 0;
    QList<QDate> outliers;
};

class ClockSyncAnalyzer
{
public:
    static constexpr double MIN_ZSCORE      = 4.0;        // peak prominence gate
    static constexpr double MIN_SCORE       = 0.10;       // absolute correlation floor
    static constexpr int    MIN_EVENTS      = 15;
    static constexpr int    MIN_REF_EVENTS  = 5;
    static constexpr double COARSE_BIN_MS   = 5000.0;
    static constexpr double COARSE_SIGMA_MS = 20000.0;
    static constexpr double FINE_BIN_MS     = 1000.0;
    static constexpr double FINE_SIGMA_MS   = 10000.0;
    static constexpr qint64 FINE_RANGE_MS   = 120000LL;   // fine scan ±2 min around coarse peak
    static constexpr qint64 MAX_LAG_MS      = 3600000LL;  // ±60 min search
    static constexpr qint64 SPO2_DELAY_MS   = 25000LL;    // nominal circulatory + averaging delay
    static constexpr qint64 PULSE_DELAY_MS  = 15000LL;    // nominal arousal-tachycardia delay
    static constexpr qint64 STREAM_DISAGREE_MS = 60000LL;

    static QList<NightSyncResult> analyze(Machine* cpap, Machine* oxi,
                                          const QDate& start, const QDate& end,
                                          DriftFit* fitOut = nullptr);

    // Pure analysis core — unit-testable without profile data:

    //! \brief Two-stage (coarse 5 s / fine 1 s) normalized cross-correlation with
    //! per-lag overlap normalization, z-score confidence and sub-bin interpolation.
    static StreamCorrelation correlateStreams(const QList<qint64>& cpapTimes,
                                              const QList<qint64>& refTimes,
                                              qint64 maxLagMs = MAX_LAG_MS);

    //! \brief SpO2 desaturation nadir timestamps (≥3% below trailing-60 s baseline).
    //! Samples outside [50, 100] %% are treated as artifacts.
    static QList<qint64> detectDrops(const QVector<qint64>& timesMs,
                                     const QVector<float>& spo2);

    //! \brief Pulse-rate surge peak timestamps (≥10 bpm above trailing-45 s baseline).
    //! Samples outside [30, 220] bpm are treated as artifacts.
    static QList<qint64> detectSurges(const QVector<qint64>& timesMs,
                                      const QVector<float>& pulse);

    //! \brief Combine SpO2 and pulse correlations into one per-night offset,
    //! compensating each stream's nominal physiological delay.
    static NightEstimate combineStreams(const StreamCorrelation& spo2,
                                        const StreamCorrelation& pulse);

    //! \brief Theil–Sen regression of accepted offsets vs date; flags MAD outliers
    //! by setting NightSyncResult::driftOutlier in place.
    static DriftFit fitDrift(QList<NightSyncResult>& results);

private:
    static QVector<double> buildDensity(const QList<qint64>& eventTimesMs,
                                        qint64 t0Ms, int nBins,
                                        double binMs, double sigmaMs);
    static QList<qint64>   collectCpapEventTimes(Session* sess);
    static QList<qint64>   collectRefEvents(Session* sess, ChannelID channel, bool drops);
};

#endif // CLOCKSYNCANALYZER_H
