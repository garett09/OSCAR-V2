/* Clock Sync Analyzer Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "clocksyncanalyzer.h"
#include "SleepLib/profiles.h"
#include "SleepLib/day.h"
#include "SleepLib/session.h"
#include "SleepLib/machine_common.h"
#include "SleepLib/schema.h"
#include <QtMath>
#include <algorithm>

namespace {

struct LagScan {
    QVector<double> scores;
    int    bestIdx   = -1;
    double bestScore = -1.0;
    int    lagLoBins = 0;
};

// Normalized cross-correlation of a against b for each lag in [lagLoBins, lagHiBins].
// Each lag is normalized by the energy of the overlapping window only (prefix sums
// of squares), so large lags are not penalized for their smaller overlap.
LagScan scanLags(const QVector<double>& a, const QVector<double>& b,
                 int lagLoBins, int lagHiBins)
{
    LagScan out;
    int n = a.size();
    if (n == 0 || b.size() != n || lagHiBins < lagLoBins) return out;

    QVector<double> pa(n + 1, 0.0), pb(n + 1, 0.0);
    for (int i = 0; i < n; ++i) {
        pa[i + 1] = pa[i] + a[i] * a[i];
        pb[i + 1] = pb[i] + b[i] * b[i];
    }

    out.lagLoBins = lagLoBins;
    out.scores.resize(lagHiBins - lagLoBins + 1);
    for (int L = lagLoBins; L <= lagHiBins; ++L) {
        int idx = L - lagLoBins;
        double dot = 0.0, ea = 0.0, eb = 0.0;
        if (L >= 0) {
            if (L >= n) { out.scores[idx] = 0.0; continue; }
            for (int i = 0; i < n - L; ++i) dot += a[i] * b[i + L];
            ea = pa[n - L];
            eb = pb[n] - pb[L];
        } else {
            int o = -L;
            if (o >= n) { out.scores[idx] = 0.0; continue; }
            for (int i = 0; i < n - o; ++i) dot += a[i + o] * b[i];
            ea = pa[n] - pa[o];
            eb = pb[n - o];
        }
        double denom = qSqrt(ea * eb);
        double s = (denom > 1e-9) ? dot / denom : 0.0;
        out.scores[idx] = s;
        if (s > out.bestScore) { out.bestScore = s; out.bestIdx = idx; }
    }
    return out;
}

} // namespace

StreamCorrelation ClockSyncAnalyzer::correlateStreams(const QList<qint64>& cpapTimes,
                                                      const QList<qint64>& refTimes,
                                                      qint64 maxLagMs)
{
    StreamCorrelation r;
    if (cpapTimes.isEmpty() || refTimes.isEmpty() || maxLagMs <= 0) return r;

    // Common time base covering all events plus the search window on both sides.
    qint64 t0 = cpapTimes[0], tMax = cpapTimes[0];
    for (qint64 t : cpapTimes) { t0 = qMin(t0, t); tMax = qMax(tMax, t); }
    for (qint64 t : refTimes)  { t0 = qMin(t0, t - maxLagMs); tMax = qMax(tMax, t + maxLagMs); }

    // --- Coarse pass: full ±maxLagMs search at COARSE_BIN_MS resolution ---
    int nCoarse = int((tMax - t0) / qint64(COARSE_BIN_MS)) + 1;
    QVector<double> ca = buildDensity(cpapTimes, t0, nCoarse, COARSE_BIN_MS, COARSE_SIGMA_MS);
    QVector<double> cb = buildDensity(refTimes,  t0, nCoarse, COARSE_BIN_MS, COARSE_SIGMA_MS);
    int maxLagCoarse = int(maxLagMs / qint64(COARSE_BIN_MS));
    LagScan coarse = scanLags(ca, cb, -maxLagCoarse, maxLagCoarse);
    if (coarse.bestIdx < 0) return r;

    // Peak prominence: z-score of the peak against the whole lag curve.
    int m = coarse.scores.size();
    double mean = 0.0;
    for (double s : coarse.scores) mean += s;
    mean /= m;
    double var = 0.0;
    for (double s : coarse.scores) var += (s - mean) * (s - mean);
    double sd = qSqrt(var / qMax(1, m - 1));
    r.zscore   = (sd > 1e-12) ? (coarse.bestScore - mean) / sd : 0.0;
    r.edgePeak = (coarse.bestIdx == 0 || coarse.bestIdx == m - 1);

    qint64 coarseLagMs = qint64(coarse.lagLoBins + coarse.bestIdx) * qint64(COARSE_BIN_MS);

    // --- Fine pass: FINE_BIN_MS resolution within ±FINE_RANGE_MS of the coarse peak ---
    int nFine = int((tMax - t0) / qint64(FINE_BIN_MS)) + 1;
    QVector<double> fa = buildDensity(cpapTimes, t0, nFine, FINE_BIN_MS, FINE_SIGMA_MS);
    QVector<double> fb = buildDensity(refTimes,  t0, nFine, FINE_BIN_MS, FINE_SIGMA_MS);
    int fineLo = int((coarseLagMs - FINE_RANGE_MS) / qint64(FINE_BIN_MS));
    int fineHi = int((coarseLagMs + FINE_RANGE_MS) / qint64(FINE_BIN_MS));
    LagScan fine = scanLags(fa, fb, fineLo, fineHi);

    double lagMsF = double(coarseLagMs);
    if (fine.bestIdx >= 0) {
        r.score = fine.bestScore;
        // Parabolic interpolation through the peak and its neighbours → sub-bin lag.
        double frac = 0.0;
        int bi = fine.bestIdx;
        if (bi > 0 && bi < fine.scores.size() - 1) {
            double y0 = fine.scores[bi - 1], y1 = fine.scores[bi], y2 = fine.scores[bi + 1];
            double den = y0 - 2.0 * y1 + y2;
            if (qAbs(den) > 1e-12) frac = qBound(-0.5, 0.5 * (y0 - y2) / den, 0.5);
        }
        lagMsF = (double(fine.lagLoBins + bi) + frac) * FINE_BIN_MS;
    } else {
        r.score = coarse.bestScore;
    }
    r.lagMs    = qRound64(lagMsF);
    r.accepted = !r.edgePeak && r.zscore >= MIN_ZSCORE && r.score >= MIN_SCORE;
    return r;
}

QList<qint64> ClockSyncAnalyzer::detectDrops(const QVector<qint64>& timesMs,
                                             const QVector<float>& spo2)
{
    QList<qint64> out;
    const float  DROP_THR    = 3.0f;   // % below baseline = desaturation
    const float  RECOVER_THR = 1.0f;   // % below baseline = recovered
    const qint64 BASELINE_MS = 60000;  // trailing baseline window
    const float  VALID_LO = 50.0f, VALID_HI = 100.0f;

    int n = timesMs.size();
    if (n != spo2.size() || n < 2) return out;

    bool   inDrop   = false;
    float  peakVal  = 0.0f;
    qint64 nadirT   = 0;
    float  nadirVal = 999.0f;

    for (int i = 0; i < n; ++i) {
        float v = spo2[i];
        if (v < VALID_LO || v > VALID_HI) {
            // Artifact (finger off / dropout): reset state, never count as a drop.
            inDrop = false;
            nadirVal = 999.0f;
            continue;
        }
        if (!inDrop) {
            // Baseline = highest valid sample in the trailing window, by timestamp.
            float base = -1.0f;
            for (int j = i - 1; j >= 0 && timesMs[i] - timesMs[j] <= BASELINE_MS; --j) {
                float bv = spo2[j];
                if (bv >= VALID_LO && bv <= VALID_HI) base = qMax(base, bv);
            }
            if (base >= 0.0f && (base - v) >= DROP_THR) {
                inDrop   = true;
                peakVal  = base;
                nadirVal = v;
                nadirT   = timesMs[i];
            }
        } else {
            if (v < nadirVal) { nadirVal = v; nadirT = timesMs[i]; }
            if (v >= peakVal - RECOVER_THR) {
                if ((peakVal - nadirVal) >= DROP_THR) out.append(nadirT);
                inDrop   = false;
                nadirVal = 999.0f;
            }
        }
    }
    if (inDrop && (peakVal - nadirVal) >= DROP_THR) out.append(nadirT);
    return out;
}

QList<qint64> ClockSyncAnalyzer::detectSurges(const QVector<qint64>& timesMs,
                                              const QVector<float>& pulse)
{
    QList<qint64> out;
    const float  RISE_THR    = 10.0f;  // bpm above baseline = surge
    const float  RECOVER_THR = 3.0f;   // bpm above baseline = recovered
    const qint64 BASELINE_MS = 45000;  // trailing baseline window
    const float  VALID_LO = 30.0f, VALID_HI = 220.0f;

    int n = timesMs.size();
    if (n != pulse.size() || n < 2) return out;

    bool   inSurge = false;
    float  baseVal = 0.0f;
    qint64 peakT   = 0;
    float  peakVal = -1.0f;

    for (int i = 0; i < n; ++i) {
        float v = pulse[i];
        if (v < VALID_LO || v > VALID_HI) {
            inSurge = false;
            peakVal = -1.0f;
            continue;
        }
        if (!inSurge) {
            // Baseline = lowest valid sample in the trailing window, by timestamp.
            float base = -1.0f;
            for (int j = i - 1; j >= 0 && timesMs[i] - timesMs[j] <= BASELINE_MS; --j) {
                float bv = pulse[j];
                if (bv < VALID_LO || bv > VALID_HI) continue;
                base = (base < 0.0f) ? bv : qMin(base, bv);
            }
            if (base >= 0.0f && (v - base) >= RISE_THR) {
                inSurge = true;
                baseVal = base;
                peakVal = v;
                peakT   = timesMs[i];
            }
        } else {
            if (v > peakVal) { peakVal = v; peakT = timesMs[i]; }
            if (v <= baseVal + RECOVER_THR) {
                if ((peakVal - baseVal) >= RISE_THR) out.append(peakT);
                inSurge = false;
                peakVal = -1.0f;
            }
        }
    }
    if (inSurge && (peakVal - baseVal) >= RISE_THR) out.append(peakT);
    return out;
}

NightEstimate ClockSyncAnalyzer::combineStreams(const StreamCorrelation& spo2,
                                                const StreamCorrelation& pulse)
{
    NightEstimate est;
    // The body reacts AFTER the respiratory event, so each measured lag includes a
    // physiological component on top of the clock offset. Subtract the nominal delay.
    qint64 spo2Off  = -(spo2.lagMs  - SPO2_DELAY_MS);
    qint64 pulseOff = -(pulse.lagMs - PULSE_DELAY_MS);

    if (spo2.accepted && pulse.accepted) {
        if (qAbs(spo2Off - pulseOff) > STREAM_DISAGREE_MS) {
            // Streams disagree badly — trust the stronger peak only.
            const bool useSpo2 = spo2.zscore >= pulse.zscore;
            est.offsetMs   = useSpo2 ? spo2Off : pulseOff;
            est.confidence = useSpo2 ? spo2.score : pulse.score;
            est.zscore     = useSpo2 ? spo2.zscore : pulse.zscore;
            est.accepted   = true;
            est.note = QObject::tr("SpO₂/pulse disagree (%1 s) — used %2")
                           .arg(qAbs(spo2Off - pulseOff) / 1000)
                           .arg(useSpo2 ? QObject::tr("SpO₂") : QObject::tr("pulse"));
        } else {
            double w1 = spo2.zscore, w2 = pulse.zscore;
            est.offsetMs   = qRound64((double(spo2Off) * w1 + double(pulseOff) * w2) / (w1 + w2));
            est.confidence = qMax(spo2.score, pulse.score);
            est.zscore     = qMax(spo2.zscore, pulse.zscore);
            est.accepted   = true;
            est.note = QObject::tr("SpO₂ + pulse combined");
        }
    } else if (spo2.accepted) {
        est.offsetMs   = spo2Off;
        est.confidence = spo2.score;
        est.zscore     = spo2.zscore;
        est.accepted   = true;
        est.note = QObject::tr("SpO₂ drops only");
    } else if (pulse.accepted) {
        est.offsetMs   = pulseOff;
        est.confidence = pulse.score;
        est.zscore     = pulse.zscore;
        est.accepted   = true;
        est.note = QObject::tr("Pulse surges only");
    } else {
        est.confidence = qMax(spo2.score, pulse.score);
        est.zscore     = qMax(spo2.zscore, pulse.zscore);
    }
    return est;
}

DriftFit ClockSyncAnalyzer::fitDrift(QList<NightSyncResult>& results)
{
    DriftFit fit;
    QVector<double> xs, ys;
    QVector<int> idx;
    QDate ref;
    for (int i = 0; i < results.size(); ++i) {
        const NightSyncResult& r = results[i];
        if (!r.accepted) continue;
        if (!ref.isValid()) ref = r.date;
        xs.append(double(ref.daysTo(r.date)));
        ys.append(double(r.offsetMs));
        idx.append(i);
    }
    int n = xs.size();
    if (n < 3) return fit;

    // Theil–Sen: median of all pairwise slopes, then median intercept.
    QVector<double> slopes;
    slopes.reserve(n * (n - 1) / 2);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (xs[j] != xs[i]) slopes.append((ys[j] - ys[i]) / (xs[j] - xs[i]));
    if (slopes.isEmpty()) return fit;
    std::sort(slopes.begin(), slopes.end());
    double slope = slopes[slopes.size() / 2];

    QVector<double> inter(n);
    for (int i = 0; i < n; ++i) inter[i] = ys[i] - slope * xs[i];
    QVector<double> sortedInter = inter;
    std::sort(sortedInter.begin(), sortedInter.end());
    double intercept = sortedInter[n / 2];

    // Flag outlier nights by the MAD of the fit residuals.
    QVector<double> absResid(n);
    for (int i = 0; i < n; ++i)
        absResid[i] = qAbs(ys[i] - (intercept + slope * xs[i]));
    QVector<double> sortedAbs = absResid;
    std::sort(sortedAbs.begin(), sortedAbs.end());
    double mad   = sortedAbs[n / 2];
    double sigma = 1.4826 * mad;
    double threshold = qMax(3.5 * sigma, 5000.0);  // never flag residuals under 5 s
    for (int i = 0; i < n; ++i) {
        if (absResid[i] > threshold) {
            results[idx[i]].driftOutlier = true;
            fit.outliers.append(results[idx[i]].date);
        }
    }

    fit.valid         = true;
    fit.slopeMsPerDay = slope;
    fit.interceptMs   = intercept;
    fit.refDate       = ref;
    fit.nUsed         = n;
    return fit;
}

QList<NightSyncResult> ClockSyncAnalyzer::analyze(Machine* cpap, Machine* oxi,
                                                   const QDate& start, const QDate& end,
                                                   DriftFit* fitOut)
{
    QList<NightSyncResult> results;
    if (fitOut) *fitOut = DriftFit();
    if (!p_profile || !cpap || !oxi) return results;

    for (QDate d = start; d <= end; d = d.addDays(1)) {
        auto it = p_profile->daylist.find(d);
        if (it == p_profile->daylist.end()) continue;
        Day* day = it.value();
        if (!day) continue;

        // Gather sessions for the specific machines
        QList<Session*> cpapSessions, oxiSessions;
        for (auto it2 = day->begin(); it2 != day->end(); ++it2) {
            Session* s = *it2;
            if (!s || s->isEmpty()) continue;
            if (s->machine() == cpap) cpapSessions.append(s);
            else if (s->machine() == oxi) oxiSessions.append(s);
        }
        if (cpapSessions.isEmpty() || oxiSessions.isEmpty()) continue;

        // Ensure events are loaded
        for (Session* s : cpapSessions)
            if (!s->eventsLoaded()) s->LoadEventsFromDatabase();
        for (Session* s : oxiSessions)
            if (!s->eventsLoaded()) s->LoadEventsFromDatabase();

        // Collect raw event times (not drift-corrected)
        QList<qint64> cpapTimes, dropTimes, surgeTimes;
        for (Session* s : cpapSessions)
            cpapTimes.append(collectCpapEventTimes(s));
        for (Session* s : oxiSessions) {
            dropTimes.append(collectRefEvents(s, OXI_SPO2, true));
            surgeTimes.append(collectRefEvents(s, OXI_Pulse, false));
        }

        NightSyncResult res;
        res.date        = d;
        res.cpapEvents  = cpapTimes.size();
        res.oxiDrops    = dropTimes.size();
        res.pulseSurges = surgeTimes.size();

        if (cpapTimes.size() < MIN_EVENTS) {
            res.note = QObject::tr("Too few CPAP events (%1) — low-AHI night skipped").arg(cpapTimes.size());
            results.append(res);
            continue;
        }
        if (dropTimes.size() < MIN_REF_EVENTS && surgeTimes.size() < MIN_REF_EVENTS) {
            res.note = QObject::tr("Too few SpO₂ drops (%1) and pulse surges (%2) — skipped")
                           .arg(dropTimes.size()).arg(surgeTimes.size());
            results.append(res);
            continue;
        }

        StreamCorrelation spo2C, pulseC;
        if (dropTimes.size() >= MIN_REF_EVENTS)
            spo2C = correlateStreams(cpapTimes, dropTimes);
        if (surgeTimes.size() >= MIN_REF_EVENTS)
            pulseC = correlateStreams(cpapTimes, surgeTimes);

        NightEstimate est = combineStreams(spo2C, pulseC);
        res.offsetMs   = est.offsetMs;
        res.confidence = est.confidence;
        res.zscore     = est.zscore;
        res.accepted   = est.accepted;
        if (est.accepted) {
            res.note = QObject::tr("Detected: %1 ms offset, z=%2 (%3)")
                           .arg(res.offsetMs)
                           .arg(res.zscore, 0, 'f', 1)
                           .arg(est.note);
        } else {
            res.note = QObject::tr("Low confidence (z=%1) — verify manually")
                           .arg(res.zscore, 0, 'f', 1);
        }
        results.append(res);
    }

    DriftFit fit = fitDrift(results);
    for (NightSyncResult& r : results) {
        if (r.driftOutlier)
            r.note += QObject::tr(" — drift outlier, auto-unchecked");
    }
    if (fitOut) *fitOut = fit;
    return results;
}

QList<qint64> ClockSyncAnalyzer::collectCpapEventTimes(Session* sess)
{
    QList<qint64> times;
    static const ChannelID codes[] = {
        CPAP_Obstructive, CPAP_Hypopnea, CPAP_ClearAirway, CPAP_Apnea, CPAP_AllApnea
    };
    for (ChannelID code : codes) {
        auto it = sess->eventlist.find(code);
        if (it == sess->eventlist.end()) continue;
        for (EventList* el : it.value()) {
            if (!el || el->type() != EVL_Event) continue;
            for (quint32 i = 0; i < el->count(); ++i)
                times.append(el->time(i));
        }
    }
    return times;
}

QList<qint64> ClockSyncAnalyzer::collectRefEvents(Session* sess, ChannelID channel, bool drops)
{
    QList<qint64> out;
    auto it = sess->eventlist.find(channel);
    if (it == sess->eventlist.end()) return out;
    for (EventList* el : it.value()) {
        if (!el || el->count() < 2) continue;
        int n = int(el->count());
        // Real timestamps from the EventList — works for any sample rate.
        QVector<qint64> t(n);
        QVector<float>  v(n);
        for (int i = 0; i < n; ++i) {
            t[i] = el->time(quint32(i));
            v[i] = float(el->data(quint32(i)));
        }
        out.append(drops ? detectDrops(t, v) : detectSurges(t, v));
    }
    return out;
}

QVector<double> ClockSyncAnalyzer::buildDensity(const QList<qint64>& eventTimesMs,
                                                 qint64 t0Ms, int nBins,
                                                 double binMs, double sigmaMs)
{
    QVector<double> d(nBins, 0.0);
    double sigmaBins = sigmaMs / binMs;
    int    radius    = int(3.0 * sigmaBins) + 1;

    for (qint64 tMs : eventTimesMs) {
        int centerBin = int((tMs - t0Ms) / qint64(binMs));
        if (centerBin < -radius || centerBin >= nBins + radius) continue;
        int lo = qMax(0, centerBin - radius);
        int hi = qMin(nBins - 1, centerBin + radius);
        for (int b = lo; b <= hi; ++b) {
            double dx = double(b - centerBin);
            d[b] += qExp(-0.5 * dx * dx / (sigmaBins * sigmaBins));
        }
    }
    return d;
}
