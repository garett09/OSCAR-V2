/* Clock Sync Analyzer Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "clocksynctests.h"
#include "clocksyncanalyzer.h"
#include <QRandomGenerator>

// Synthetic apnea-like event train: `count` events with random gaps
// averaging `meanGapMs`. Deterministic for a given seed.
static QList<qint64> makeEvents(int count, qint64 startMs, qint64 meanGapMs, quint32 seed)
{
    QRandomGenerator rng(seed);
    QList<qint64> times;
    qint64 cur = startMs;
    for (int i = 0; i < count; ++i) {
        cur += meanGapMs / 2 + qint64(rng.bounded(quint32(meanGapMs)));
        times.append(cur);
    }
    return times;
}

// Copy of an event train shifted by offsetMs with uniform jitter in
// [-jitterMs, +jitterMs], emulating physiological timing noise.
static QList<qint64> shiftedJittered(const QList<qint64>& in, qint64 offsetMs,
                                     qint64 jitterMs, quint32 seed)
{
    QRandomGenerator rng(seed);
    QList<qint64> out;
    for (qint64 t : in)
        out.append(t + offsetMs + qint64(rng.bounded(quint32(2 * jitterMs + 1))) - jitterMs);
    return out;
}

void ClockSyncTests::testCorrelateRecoversKnownOffset()
{
    // ~8 hours of events, reference stream shifted +37 s with 8 s jitter.
    QList<qint64> cpap = makeEvents(60, 0, 8LL * 60000LL, 1);
    QList<qint64> ref  = shiftedJittered(cpap, 37000, 8000, 2);

    StreamCorrelation r = ClockSyncAnalyzer::correlateStreams(cpap, ref);
    QVERIFY(r.accepted);
    QVERIFY(!r.edgePeak);
    QVERIFY(qAbs(r.lagMs - 37000) < 4000);
}

void ClockSyncTests::testCorrelateLargeOffsetUnbiased()
{
    // 45-minute shift must be recovered as accurately as a small one.
    // (The old zero-lag normalization suppressed scores at large lags.)
    QList<qint64> cpap = makeEvents(60, 0, 8LL * 60000LL, 3);
    QList<qint64> ref  = shiftedJittered(cpap, 2700000, 8000, 4);

    StreamCorrelation r = ClockSyncAnalyzer::correlateStreams(cpap, ref);
    QVERIFY(r.accepted);
    QVERIFY(qAbs(r.lagMs - 2700000) < 4000);
}

void ClockSyncTests::testCorrelateSubBinPrecision()
{
    // Low jitter: parabolic interpolation should land well inside one
    // coarse bin (5 s) of the true 37.4 s offset.
    QList<qint64> cpap = makeEvents(80, 0, 6LL * 60000LL, 5);
    QList<qint64> ref  = shiftedJittered(cpap, 37400, 1500, 6);

    StreamCorrelation r = ClockSyncAnalyzer::correlateStreams(cpap, ref);
    QVERIFY(r.accepted);
    QVERIFY(qAbs(r.lagMs - 37400) < 1500);
}

void ClockSyncTests::testCorrelateRejectsNoise()
{
    // Two unrelated event trains: no peak should pass the z-score gate.
    QList<qint64> a = makeEvents(60, 0, 8LL * 60000LL, 7);
    QList<qint64> b = makeEvents(50, 900000, 9LL * 60000LL, 8);

    StreamCorrelation r = ClockSyncAnalyzer::correlateStreams(a, b);
    QVERIFY(!r.accepted);
}

void ClockSyncTests::testCorrelateEmptyInputs()
{
    QList<qint64> some = makeEvents(20, 0, 60000, 9);
    QList<qint64> none;

    QVERIFY(!ClockSyncAnalyzer::correlateStreams(none, some).accepted);
    QVERIFY(!ClockSyncAnalyzer::correlateStreams(some, none).accepted);
    QVERIFY(!ClockSyncAnalyzer::correlateStreams(none, none).accepted);
}

void ClockSyncTests::testCorrelateLongNight()
{
    // ~16.7 h of events: the old implementation truncated everything
    // past a fixed 12 h window.
    QList<qint64> cpap = makeEvents(100, 0, 10LL * 60000LL, 10);
    QList<qint64> ref  = shiftedJittered(cpap, 30000, 5000, 11);

    StreamCorrelation r = ClockSyncAnalyzer::correlateStreams(cpap, ref);
    QVERIFY(r.accepted);
    QVERIFY(qAbs(r.lagMs - 30000) < 3000);
}

void ClockSyncTests::testDetectDropsBasic()
{
    // 1 Hz SpO2: baseline 96 %, one 6 % desaturation with nadir ~129 s.
    QVector<qint64> t;
    QVector<float>  v;
    for (int i = 0; i < 300; ++i) {
        t.append(qint64(i) * 1000);
        float val = 96.0f;
        if (i >= 120 && i < 130)      val = 96.0f - 0.6f * (i - 119);  // fall to 90
        else if (i >= 130 && i < 150) val = 90.0f;                      // nadir plateau
        else if (i >= 150 && i < 162) val = 90.0f + 0.5f * (i - 149);   // recovery
        v.append(val);
    }

    QList<qint64> drops = ClockSyncAnalyzer::detectDrops(t, v);
    QCOMPARE(drops.size(), 1);
    QVERIFY(drops[0] >= 125000 && drops[0] <= 150000);
}

void ClockSyncTests::testDetectDropsIgnoresArtifacts()
{
    // Finger-off dropout (zeros) must not register as a desaturation.
    QVector<qint64> t;
    QVector<float>  v;
    for (int i = 0; i < 300; ++i) {
        t.append(qint64(i) * 1000);
        v.append((i >= 100 && i < 110) ? 0.0f : 96.0f);
    }

    QList<qint64> drops = ClockSyncAnalyzer::detectDrops(t, v);
    QCOMPARE(drops.size(), 0);
}

void ClockSyncTests::testDetectDropsSlowSampleRate()
{
    // 4 s sampling (Wellue/Viatom style): window math must use timestamps,
    // not a fixed sample count.
    QVector<qint64> t;
    QVector<float>  v;
    for (int i = 0; i < 100; ++i) {
        t.append(qint64(i) * 4000);
        float val = 96.0f;
        if (i >= 40 && i < 44)      val = 96.0f - 1.5f * (i - 39);  // fall to 90
        else if (i >= 44 && i < 50) val = 90.0f;                     // nadir
        else if (i >= 50 && i < 56) val = 90.0f + 1.0f * (i - 49);   // recovery
        v.append(val);
    }

    QList<qint64> drops = ClockSyncAnalyzer::detectDrops(t, v);
    QCOMPARE(drops.size(), 1);
    QVERIFY(drops[0] >= 160000 && drops[0] <= 200000);
}

void ClockSyncTests::testDetectSurgesBasic()
{
    // 1 Hz pulse: baseline 60 bpm, one arousal surge to 74 bpm.
    QVector<qint64> t;
    QVector<float>  v;
    for (int i = 0; i < 300; ++i) {
        t.append(qint64(i) * 1000);
        float val = 60.0f;
        if (i >= 100 && i < 106)      val = 60.0f + 2.0f * (i - 99);   // rise to ~72-74
        else if (i >= 106 && i < 112) val = 74.0f;                      // peak plateau
        else if (i >= 112 && i < 120) val = 74.0f - 1.5f * (i - 111);   // recovery
        v.append(val);
    }

    QList<qint64> surges = ClockSyncAnalyzer::detectSurges(t, v);
    QCOMPARE(surges.size(), 1);
    QVERIFY(surges[0] >= 100000 && surges[0] <= 115000);
}

void ClockSyncTests::testCombinePhysioDelay()
{
    // A measured lag equal to the nominal physiological delay means the
    // clocks are actually in sync: offset must come out as 0.
    StreamCorrelation spo2;
    spo2.lagMs = ClockSyncAnalyzer::SPO2_DELAY_MS;
    spo2.score = 0.5; spo2.zscore = 8.0; spo2.accepted = true;
    StreamCorrelation pulse;  // not accepted

    NightEstimate est = ClockSyncAnalyzer::combineStreams(spo2, pulse);
    QVERIFY(est.accepted);
    QCOMPARE(est.offsetMs, qint64(0));

    // Two agreeing streams: z-weighted mean of the delay-compensated offsets.
    pulse.lagMs = ClockSyncAnalyzer::PULSE_DELAY_MS + 20000;  // offset -20 s
    pulse.score = 0.4; pulse.zscore = 8.0; pulse.accepted = true;
    spo2.lagMs  = ClockSyncAnalyzer::SPO2_DELAY_MS + 10000;   // offset -10 s

    est = ClockSyncAnalyzer::combineStreams(spo2, pulse);
    QVERIFY(est.accepted);
    QCOMPARE(est.offsetMs, qint64(-15000));  // equal weights
}

void ClockSyncTests::testCombineDisagreement()
{
    // Streams >60 s apart: trust the stronger peak only.
    StreamCorrelation spo2;
    spo2.lagMs = ClockSyncAnalyzer::SPO2_DELAY_MS;            // offset 0
    spo2.score = 0.6; spo2.zscore = 10.0; spo2.accepted = true;
    StreamCorrelation pulse;
    pulse.lagMs = ClockSyncAnalyzer::PULSE_DELAY_MS + 120000; // offset -120 s
    pulse.score = 0.3; pulse.zscore = 5.0; pulse.accepted = true;

    NightEstimate est = ClockSyncAnalyzer::combineStreams(spo2, pulse);
    QVERIFY(est.accepted);
    QCOMPARE(est.offsetMs, qint64(0));
}

void ClockSyncTests::testFitDriftRecoversSlope()
{
    // 20 nights with 1500 ms/day drift and ±300 ms noise.
    QList<NightSyncResult> results;
    QDate d0(2026, 5, 1);
    QRandomGenerator rng(12);
    for (int i = 0; i < 20; ++i) {
        NightSyncResult r;
        r.date     = d0.addDays(i);
        r.offsetMs = 10000 + 1500 * i + qint64(rng.bounded(601u)) - 300;
        r.accepted = true;
        results.append(r);
    }

    DriftFit fit = ClockSyncAnalyzer::fitDrift(results);
    QVERIFY(fit.valid);
    QCOMPARE(fit.nUsed, 20);
    QVERIFY(qAbs(fit.slopeMsPerDay - 1500.0) < 150.0);
    QVERIFY(fit.outliers.isEmpty());
}

void ClockSyncTests::testFitDriftFlagsOutliers()
{
    QList<NightSyncResult> results;
    QDate d0(2026, 5, 1);
    QRandomGenerator rng(13);
    for (int i = 0; i < 20; ++i) {
        NightSyncResult r;
        r.date     = d0.addDays(i);
        r.offsetMs = 10000 + 1500 * i + qint64(rng.bounded(601u)) - 300;
        if (i == 5 || i == 14) r.offsetMs += 40000;  // two bad nights
        r.accepted = true;
        results.append(r);
    }

    DriftFit fit = ClockSyncAnalyzer::fitDrift(results);
    QVERIFY(fit.valid);
    QVERIFY(qAbs(fit.slopeMsPerDay - 1500.0) < 150.0);  // robust to outliers
    QCOMPARE(fit.outliers.size(), 2);
    QVERIFY(results[5].driftOutlier);
    QVERIFY(results[14].driftOutlier);
    QVERIFY(!results[6].driftOutlier);
}
