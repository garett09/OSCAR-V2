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
#include <QtMath>

QList<NightSyncResult> ClockSyncAnalyzer::analyze(Machine* cpap, Machine* oxi,
                                                   const QDate& start, const QDate& end)
{
    QList<NightSyncResult> results;
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
        QList<qint64> cpapTimes, oxiTimes;
        for (Session* s : cpapSessions)
            cpapTimes.append(collectCpapEventTimes(s));
        for (Session* s : oxiSessions)
            oxiTimes.append(collectOxiDropTimes(s));

        NightSyncResult res;
        res.date       = d;
        res.cpapEvents = cpapTimes.size();
        res.oxiDrops   = oxiTimes.size();

        if (cpapTimes.size() < MIN_EVENTS) {
            res.note = QObject::tr("Too few CPAP events (%1) — low-AHI night skipped").arg(cpapTimes.size());
            results.append(res);
            continue;
        }
        if (oxiTimes.size() < MIN_OXI_DROPS) {
            res.note = QObject::tr("Too few SpO₂ drops detected (%1) — skipped").arg(oxiTimes.size());
            results.append(res);
            continue;
        }

        // Build common time reference: start of earliest session
        qint64 t0Ms = cpapTimes[0];
        for (qint64 t : cpapTimes) t0Ms = qMin(t0Ms, t);
        for (qint64 t : oxiTimes)  t0Ms = qMin(t0Ms, t - MAX_LAG_MS);

        // Number of bins to cover 12 hours + 2x search window
        const qint64 windowMs = 12LL * 3600000LL + 2LL * MAX_LAG_MS;
        int nBins = int(windowMs / BIN_MS) + 1;

        QVector<double> cpapDensity = buildDensity(cpapTimes, t0Ms, nBins, SIGMA_MS);
        QVector<double> oxiDensity  = buildDensity(oxiTimes,  t0Ms, nBins, SIGMA_MS);

        // Cross-correlate: scan lag L in [-MAX_LAG_MS, +MAX_LAG_MS]
        // Lag L (in bins): sum A[i] * B[i + L]
        // Positive L means oxi events are shifted forward relative to cpap events
        int maxLagBins = int(MAX_LAG_MS / BIN_MS);
        double bestScore   = -1.0;
        int    bestLagBins = 0;

        // Normalization: zero-lag energy
        double norm = gaussianSum(cpapDensity, cpapDensity, 0) *
                      gaussianSum(oxiDensity,  oxiDensity,  0);
        if (norm < 1e-12) {
            res.note = QObject::tr("Density normalization failed — skipped");
            results.append(res);
            continue;
        }
        norm = qSqrt(norm);

        for (int L = -maxLagBins; L <= maxLagBins; ++L) {
            double score = gaussianSum(cpapDensity, oxiDensity, L) / norm;
            if (score > bestScore) {
                bestScore   = score;
                bestLagBins = L;
            }
        }

        // lagMs > 0 means oxi events are after cpap events by lagMs
        // cpap is behind real time by lagMs (approximately)
        // offsetMs < 0 (CPAP behind = negative per sign convention)
        qint64 lagMs    = qint64(bestLagBins) * qint64(BIN_MS);
        res.offsetMs   = -lagMs;
        res.confidence  = bestScore;
        res.accepted    = (bestScore >= MIN_CONFIDENCE);
        if (res.accepted) {
            res.note = QObject::tr("Detected: %1 ms offset, confidence %2")
                           .arg(res.offsetMs)
                           .arg(res.confidence, 0, 'f', 3);
        } else {
            res.note = QObject::tr("Low confidence (%1) — verify manually").arg(res.confidence, 0, 'f', 3);
        }
        results.append(res);
    }
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

QList<qint64> ClockSyncAnalyzer::collectOxiDropTimes(Session* sess)
{
    // Derive SpO2 drop events from OXI_SPO2 waveform.
    // A drop: SpO2 falls ≥3% from a local baseline within 60s.
    // We record the timestamp of the nadir.
    QList<qint64> times;

    auto it = sess->eventlist.find(OXI_SPO2);
    if (it == sess->eventlist.end()) return times;

    for (EventList* el : it.value()) {
        if (!el || el->count() < 2) continue;

        const int   WINDOW   = 60;   // samples (1Hz data = 60s window)
        const float DROP_THR = 3.0f; // %

        QVector<float> spo2(el->count());
        for (quint32 i = 0; i < el->count(); ++i)
            spo2[i] = float(el->data(i));

        bool inDrop     = false;
        int  dropStart  = 0;
        float peakVal   = 0.0f;
        int   nadirIdx  = 0;
        float nadirVal  = 999.0f;

        for (int i = 0; i < int(el->count()); ++i) {
            float v = spo2[i];
            if (!inDrop) {
                // Look for onset: value WINDOW samples back vs now
                if (i >= WINDOW) {
                    float ref = spo2[i - WINDOW];
                    if ((ref - v) >= DROP_THR) {
                        inDrop    = true;
                        dropStart = i - WINDOW;
                        peakVal   = ref;
                        nadirVal  = v;
                        nadirIdx  = i;
                    }
                }
            } else {
                if (v < nadirVal) { nadirVal = v; nadirIdx = i; }
                // Drop ends when signal recovers to within 1% of peak
                if (v >= peakVal - 1.0f) {
                    (void)dropStart;
                    // Only record if still a real drop
                    if ((peakVal - nadirVal) >= DROP_THR)
                        times.append(el->time(quint32(nadirIdx)));
                    inDrop   = false;
                    nadirVal = 999.0f;
                }
            }
        }
        if (inDrop && (peakVal - nadirVal) >= DROP_THR)
            times.append(el->time(quint32(nadirIdx)));
    }
    return times;
}

QVector<double> ClockSyncAnalyzer::buildDensity(const QList<qint64>& eventTimesMs,
                                                 qint64 t0Ms, int nBins,
                                                 double sigmaMs)
{
    QVector<double> d(nBins, 0.0);
    double sigmaBins = sigmaMs / BIN_MS;
    int    radius    = int(3.0 * sigmaBins) + 1;

    for (qint64 tMs : eventTimesMs) {
        int centerBin = int((tMs - t0Ms) / BIN_MS);
        int lo = qMax(0, centerBin - radius);
        int hi = qMin(nBins - 1, centerBin + radius);
        for (int b = lo; b <= hi; ++b) {
            double dx = double(b - centerBin);
            d[b] += qExp(-0.5 * dx * dx / (sigmaBins * sigmaBins));
        }
    }
    return d;
}

double ClockSyncAnalyzer::gaussianSum(const QVector<double>& a,
                                       const QVector<double>& b, int lagBins)
{
    int n = a.size();
    double sum = 0.0;
    if (lagBins >= 0) {
        int limit = qMin(n, n - lagBins);
        for (int i = 0; i < limit; ++i)
            sum += a[i] * b[i + lagBins];
    } else {
        int offset = -lagBins;
        int limit  = qMin(n, n - offset);
        for (int i = 0; i < limit; ++i)
            sum += a[i + offset] * b[i];
    }
    return sum;
}
