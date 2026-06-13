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

//! \brief One night's recording-window offset between reference and CPAP (raw clocks).
//! This is the PRIMARY estimator: for a constant clock offset it is far more robust
//! than apnea/SpO₂ cross-correlation and is available on every paired night.
struct NightStartDiff {
    QDate  date;
    qint64 startDiffMs = 0;   // refFirst - cpapFirst (raw timestamps)
    qint64 endDiffMs   = 0;   // refLast  - cpapLast  (raw timestamps)
    qint64 midMs       = 0;   // (start+end)/2 — best per-night clock-offset estimate
    bool   used        = true;// false = rejected as a pairing/outlier night
    // Optional cross-correlation validator (only filled when validation is enabled):
    bool   validated       = false; // cross-correlation actually ran for this night
    bool   validatorAgrees = false; // |validatorOffsetMs - midMs| within tolerance
    qint64 validatorOffsetMs = 0;   // additive-to-CPAP offset from event cross-corr
    double validatorZ      = 0;
    int    cpapEvents      = 0;
    int    refEvents       = 0;
};

//! \brief Robust constant offset fitted across nights (median of per-night midpoints).
//! offsetMs is the signed amount to ADD to the CPAP's raw timestamps to put it on the
//! reference clock (positive = CPAP is behind the reference).
struct ConstantOffsetFit {
    bool   valid = false;
    qint64 offsetMs = 0;
    qint64 madMs = 0;             // median absolute deviation of kept nights (spread)
    qint64 startEndSpreadMs = 0;  // medianEnd - medianStart (behavioural-asymmetry check)
    int    nUsed = 0;
    int    nOutliers = 0;
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
    static constexpr int    MIN_OFFSET_NIGHTS  = 5;       // min paired nights for a constant fit
    static constexpr qint64 OFFSET_OUTLIER_FLOOR_MS = 300000LL; // never flag within ±5 min of median
    static constexpr qint64 VALIDATOR_TOL_MS   = 120000LL; // cross-corr agrees within ±2 min

    // --- PRIMARY estimator: constant offset from recording-window differences ---

    //! \brief Per-night reference-vs-CPAP recording-window differences over the range.
    static QVector<NightStartDiff> collectStartDiffs(Machine* cpap, Machine* ref,
                                                     const QDate& start, const QDate& end);

    //! \brief Robust constant offset (median midpoint + MAD outlier rejection).
    //! Sets NightStartDiff::used in place. Pure — unit-testable.
    static ConstantOffsetFit fitConstantOffset(QVector<NightStartDiff>& nights);

    //! \brief Optional high-AHI cross-correlation cross-check for one night; fills the
    //! validator fields and compares against the night's window midpoint.
    static void validateNight(Machine* cpap, Machine* ref, NightStartDiff& night);

    // --- Secondary: event cross-correlation (kept as an optional validator only) ---
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
