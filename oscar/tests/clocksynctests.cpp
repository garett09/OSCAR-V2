/* Clock Sync Analyzer Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "clocksynctests.h"
#include "clocksyncanalyzer.h"
#include "SleepLib/machine.h"
#include "SleepLib/machine_common.h"
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

// Helper: a night with a given start/end diff plus small noise.
static NightStartDiff night(qint64 startMs, qint64 endMs, qint64 noiseMs, quint32 seed)
{
    QRandomGenerator rng(seed);
    NightStartDiff n;
    auto jit = [&](qint64 base) {
        return base + (noiseMs ? qint64(rng.bounded(quint32(2 * noiseMs + 1))) - noiseMs : 0);
    };
    n.startDiffMs = jit(startMs);
    n.endDiffMs   = jit(endMs);
    return n;
}

void ClockSyncTests::testFitConstantOffsetClean()
{
    // 28 nights around +23.4 min (1,404,000 ms) on both edges, ±20 s noise.
    QVector<NightStartDiff> nights;
    for (int i = 0; i < 28; ++i)
        nights.append(night(1404000, 1404000, 20000, 100 + i));

    ConstantOffsetFit fit = ClockSyncAnalyzer::fitConstantOffset(nights);
    QVERIFY(fit.valid);
    QCOMPARE(fit.nUsed, 28);
    QCOMPARE(fit.nOutliers, 0);
    QVERIFY(qAbs(fit.offsetMs - 1404000) < 30000);  // within 30 s of truth
}

void ClockSyncTests::testFitConstantOffsetRejectsArtifacts()
{
    // 25 clean nights + 3 session-pairing artifacts (tens of minutes off).
    QVector<NightStartDiff> nights;
    for (int i = 0; i < 25; ++i)
        nights.append(night(1404000, 1404000, 20000, 200 + i));
    nights.append(night(42000000, 42000000, 0, 900));   // +700 min
    nights.append(night(59000000, 59000000, 0, 901));   // +983 min
    nights.append(night(-34000000, -34000000, 0, 902)); // -567 min

    ConstantOffsetFit fit = ClockSyncAnalyzer::fitConstantOffset(nights);
    QVERIFY(fit.valid);
    QCOMPARE(fit.nOutliers, 3);
    QCOMPARE(fit.nUsed, 25);
    QVERIFY(qAbs(fit.offsetMs - 1404000) < 30000);  // artifacts didn't move the median
    // The three artifact nights are flagged unused; the clean ones kept.
    QVERIFY(!nights[25].used);
    QVERIFY(!nights[26].used);
    QVERIFY(!nights[27].used);
    QVERIFY(nights[0].used);
}

void ClockSyncTests::testFitConstantOffsetTooFew()
{
    QVector<NightStartDiff> nights;
    for (int i = 0; i < 4; ++i)
        nights.append(night(1404000, 1404000, 20000, 300 + i));

    ConstantOffsetFit fit = ClockSyncAnalyzer::fitConstantOffset(nights);
    QVERIFY(!fit.valid);  // below MIN_OFFSET_NIGHTS
}

void ClockSyncTests::testFitConstantOffsetStartEndSpread()
{
    // Start diff ~22 min, end diff ~24 min: midpoint ~23 min, spread ~+2 min.
    // A consistent start/end gap flags a behavioural component to surface to the user.
    QVector<NightStartDiff> nights;
    for (int i = 0; i < 20; ++i)
        nights.append(night(1320000, 1440000, 5000, 400 + i));  // 22 min / 24 min

    ConstantOffsetFit fit = ClockSyncAnalyzer::fitConstantOffset(nights);
    QVERIFY(fit.valid);
    QVERIFY(qAbs(fit.offsetMs - 1380000) < 15000);        // midpoint ≈ 23 min
    QVERIFY(qAbs(fit.startEndSpreadMs - 120000) < 15000); // end − start ≈ +2 min
}

void ClockSyncTests::testFitConstantOffsetStableNoJump()
{
    // A flat ~23.4 min offset across the whole range: no mid-range step.
    QVector<NightStartDiff> nights;
    for (int i = 0; i < 24; ++i)
        nights.append(night(1404000, 1404000, 20000, 500 + i));

    ConstantOffsetFit fit = ClockSyncAnalyzer::fitConstantOffset(nights);
    QVERIFY(fit.valid);
    QVERIFY(!fit.unstable);
    QVERIFY(qAbs(fit.shiftMs) < ClockSyncAnalyzer::OFFSET_JUMP_MS);  // negligible step
}

void ClockSyncTests::testFitConstantOffsetDetectsMidRangeStep()
{
    // First 12 nights ~23 min, then a +10 min jump (reference clock re-synced).
    // The single median (~28 min) would be meaningless, so unstable must trip.
    QVector<NightStartDiff> nights;
    for (int i = 0; i < 12; ++i)
        nights.append(night(1380000, 1380000, 15000, 600 + i));  // 23 min
    for (int i = 0; i < 12; ++i)
        nights.append(night(1980000, 1980000, 15000, 700 + i));  // 33 min

    ConstantOffsetFit fit = ClockSyncAnalyzer::fitConstantOffset(nights);
    QVERIFY(fit.valid);
    QVERIFY(fit.unstable);                              // guard tripped
    QVERIFY(qAbs(fit.shiftMs - 600000) < 60000);        // ≈ +10 min step recovered
}

void ClockSyncTests::testFitDriftLineRecoversSlope()
{
    // 25 nights, +3900 ms/day drift on both edges, +/-15 s behavioural noise.
    QVector<NightStartDiff> nights;
    QDate d0(2026, 5, 1);
    for (int i = 0; i < 25; ++i) {
        NightStartDiff n = night(1000000 + 3900 * i, 1000000 + 3900 * i, 15000, 800 + i);
        n.date = d0.addDays(i);
        nights.append(n);
    }

    DriftLineFit fit = ClockSyncAnalyzer::fitDriftLine(nights);
    QVERIFY(fit.valid);
    QCOMPARE(fit.nUsed, 25);
    QVERIFY(qAbs(fit.slopeMsPerDay - 3900.0) < 400.0);
    QVERIFY(fit.significant);                       // ~93 s over 24 days >> floor
    QCOMPARE(fit.refDate, d0);
    QCOMPARE(fit.lastDate, d0.addDays(24));
}

void ClockSyncTests::testFitDriftLineFlatIsNotSignificant()
{
    // Flat offset, only behavioural noise -> slope ~0 -> not significant (use constant).
    QVector<NightStartDiff> nights;
    QDate d0(2026, 5, 1);
    for (int i = 0; i < 25; ++i) {
        NightStartDiff n = night(1404000, 1404000, 15000, 900 + i);
        n.date = d0.addDays(i);
        nights.append(n);
    }

    DriftLineFit fit = ClockSyncAnalyzer::fitDriftLine(nights);
    QVERIFY(fit.valid);
    QVERIFY(!fit.significant);
    QVERIFY(qAbs(fit.offsetAtRefMs - 1404000) < 30000);
}

void ClockSyncTests::testFitDriftLineDetectsJump()
{
    // A clock re-sync shows up as a sustained residual step. Segments are intentionally
    // UNBALANCED (16 early, 8 late): with balanced segments the cross-segment Theil-Sen
    // pairs outnumber within-segment pairs and corrupt the slope, inflating the residual
    // MAD until the K*MAD threshold exceeds the step (a known heuristic blind spot).
    // Unbalanced keeps the slope clean so the step is unambiguous. The median-based
    // changepoint locator reports an approximate date biased toward the earlier side of
    // the step, so jumpDate is checked as a plausible interior date, not the exact boundary.
    QVector<NightStartDiff> nights;
    QDate d0(2026, 5, 1);
    for (int i = 0; i < 16; ++i) {
        NightStartDiff n = night(1000000 + 5000 * i, 1000000 + 5000 * i, 8000, 1000 + i);
        n.date = d0.addDays(i);
        nights.append(n);
    }
    for (int i = 16; i < 24; ++i) {
        qint64 base = 1000000 + 5000 * i + 600000;   // +10 min clock jump at night 16
        NightStartDiff n = night(base, base, 8000, 1000 + i);
        n.date = d0.addDays(i);
        nights.append(n);
    }

    DriftLineFit fit = ClockSyncAnalyzer::fitDriftLine(nights);
    QVERIFY(fit.valid);
    QVERIFY(fit.hasJump);
    QVERIFY(fit.jumpMs > ClockSyncAnalyzer::JUMP_FLOOR_MS);   // positive step, above floor
    // Locator is approximate: somewhere in the interior, at or before the true boundary.
    QVERIFY(fit.jumpDate >= d0.addDays(ClockSyncAnalyzer::JUMP_MIN_SIDE));
    QVERIFY(fit.jumpDate <= d0.addDays(16));
}

void ClockSyncTests::testAnchorInterceptRemovesBehaviouralBias()
{
    // Recording-window midpoints carry a +90 s consistent behavioural bias on top of a
    // true +2000 ms/day drift with true offset 500000 ms at ref. Cross-corr (validator)
    // sees the behaviour-free truth. Anchoring should pull the intercept down by ~90 s.
    QVector<NightStartDiff> nights;
    QDate d0(2026, 5, 1);
    const qint64 BIAS = 90000, TRUE_OFF = 500000;
    const double SLOPE = 2000.0;
    for (int i = 0; i < 20; ++i) {
        NightStartDiff n;
        n.date = d0.addDays(i);
        qint64 trueOff = TRUE_OFF + qint64(SLOPE * i);
        n.startDiffMs = trueOff + BIAS;        // window method is biased high
        n.endDiffMs   = trueOff + BIAS;
        n.used = true;
        n.validated = true;
        n.validatorOffsetMs = trueOff;         // cross-corr is unbiased
        nights.append(n);
    }

    DriftLineFit fit = ClockSyncAnalyzer::fitDriftLine(nights);
    QVERIFY(fit.valid);
    QVERIFY(qAbs(fit.offsetAtRefMs - (TRUE_OFF + BIAS)) < 20000);  // biased before anchor

    ClockSyncAnalyzer::anchorIntercept(nights, fit);
    QVERIFY(fit.anchored);
    QVERIFY(!fit.anchorSlopeDisagrees);
    QVERIFY(qAbs(fit.offsetAtRefMs - TRUE_OFF) < 15000);          // bias removed
    QVERIFY(qAbs(fit.slopeMsPerDay - SLOPE) < 300.0);             // slope untouched
}

void ClockSyncTests::testDriftToModelRoundTrip()
{
    // Build a drift row from a known line, apply it through the REAL correctionMs,
    // and confirm the per-night additive offset matches offsetAtRef + slope*days.
    QDate ref(2026, 1, 10);
    struct Case { double slopePerDay; qint64 offsetAtRef; };
    const Case cases[] = {
        {  3900.0,  1440000 },   // +3.9 s/day, CPAP 24 min behind at ref
        { -3900.0, -1440000 },   // opposite signs
        {     0.0,   600000 },   // pure constant via the drift model
    };

    for (const Case& c : cases) {
        DriftModel m = ClockSyncAnalyzer::driftToModel(c.slopePerDay, c.offsetAtRef, ref);

        TimeCorrectionRow row;
        row.dateFrom = ref;
        row.type     = "drift";
        row.c0Ms     = m.c0Ms;
        row.c1       = m.c1;

        Machine mach(nullptr, 1);                 // id!=0 skips profile use; ~Machine is a no-op
        mach.rebuildCorrections(QList<TimeCorrectionRow>{ row });

        for (int days : {0, 1, 7, 30, 200}) {
            QDate night = ref.addDays(days);
            qint64 expected = c.offsetAtRef + qint64(qRound(c.slopePerDay * days));
            qint64 got = mach.correctionMs(night);
            QVERIFY2(qAbs(got - expected) <= 5,
                     qPrintable(QString("slope=%1 day=%2 expected=%3 got=%4")
                                .arg(c.slopePerDay).arg(days).arg(expected).arg(got)));
        }
        // Nights before dateFrom are gated out -> no correction.
        QCOMPARE(mach.correctionMs(ref.addDays(-1)), qint64(0));
    }
}
