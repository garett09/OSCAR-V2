# CPAP Clock Drift Correction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the constant-offset output of the clock-sync dialog with a linear drift model when the data shows real drift, saved as a time-varying `"drift"` correction row that `Machine::correctionMs` already applies.

**Architecture:** Layer a robust line fit (`fitDriftLine`) on top of the existing `fitConstantOffset` outlier filtering. The cross-correlation validator (already implemented) anchors the line's intercept. A verified reparam (`driftToModel`) converts `(slopeMsPerDay, offsetAtRefMs, refDate)` into the storage model `(c0Ms, c1)`. The dialog picks drift vs constant by slope significance and saves via a new `upsertDrift` repository method.

**Tech Stack:** C++17, Qt 6 (`QtMath`, `QDate`/`QDateTime`, `QVector`), QtTest (AutoTest harness), SQLite via `DeviceTimeCorrectionRepository`.

## Global Constraints

- License header on every file: `Copyright (c) 2026 The OSCAR Team` GPL block (copy verbatim from existing files in the same dir).
- Pure analyzer methods (`fitDriftLine`, `anchorIntercept`, `driftToModel`) must not touch `Profile`/DB — unit-testable in isolation.
- Cross-correlation internals (`correlateStreams`, `scanLags`, `buildDensity`, `detectDrops`, `detectSurges`) are **not** modified.
- Sign convention (single source of truth): `correctionMs` returns an **additive-to-raw** correction; positive `offsetMs`/`midMs`/`offsetAtRefMs` = **CPAP behind reference**. Drift branch applies `additive = −(c0Ms + c1·t_noon)`, `t_noon` = UTC-noon epoch ms of the night (exactly 86 400 000 ms/day spacing).
- Build/run tests:
  ```
  cd /Users/adriangarett.sian/oscar-code/build-test
  /opt/homebrew/bin/qmake ../oscar/oscar.pro CONFIG+=test && make -j8
  ./OSCAR20.app/Contents/MacOS/OSCAR20 <testFunctionName>
  ```
  The AutoTest collector forwards argv to `QTest::qExec`, so `<testFunctionName>` runs only that slot. Omit it to run all tests.

---

## File Structure

- `oscar/clocksyncanalyzer.h` — add `DriftLineFit` + `DriftModel` structs, new constants, three new static method decls.
- `oscar/clocksyncanalyzer.cpp` — implement `driftToModel`, `fitDriftLine` (incl. jump detection), `anchorIntercept`, plus a `medianD` helper.
- `oscar/database/device_time_correction_repository.h` — declare `upsertDrift`.
- `oscar/database/device_time_correction_repository.cpp` — implement `upsertDrift`.
- `oscar/clocksyncdialog.cpp` — branch `onAnalyze`/`onApply` on drift significance; status text; residual table column.
- `oscar/tests/clocksynctests.h` — declare new test slots.
- `oscar/tests/clocksynctests.cpp` — new tests.

---

### Task 1: `driftToModel` reparam + round-trip through `Machine::correctionMs`

**Files:**
- Modify: `oscar/clocksyncanalyzer.h` (add `DriftModel` struct + `driftToModel` decl)
- Modify: `oscar/clocksyncanalyzer.cpp` (implement `driftToModel`)
- Modify: `oscar/tests/clocksynctests.h` (declare `testDriftToModelRoundTrip`)
- Test: `oscar/tests/clocksynctests.cpp`

**Interfaces:**
- Produces: `struct DriftModel { qint64 c0Ms = 0; double c1 = 0.0; };`
- Produces: `static DriftModel ClockSyncAnalyzer::driftToModel(double slopeMsPerDay, qint64 offsetAtRefMs, const QDate& refDate);`

- [ ] **Step 1: Declare the struct + method in the header**

In `oscar/clocksyncanalyzer.h`, add after the `ConstantOffsetFit` struct:

```cpp
//! \brief Storage form of a linear drift correction: applied as
//! additive = -(c0Ms + c1 * t_noon_epoch_ms). c1 is ms-error per ms-epoch.
struct DriftModel {
    qint64 c0Ms = 0;
    double c1   = 0.0;
};
```

In the `ClockSyncAnalyzer` class public section (near the other static decls), add:

```cpp
    //! \brief Convert a fitted drift line to the (c0Ms, c1) storage model that
    //! Machine::correctionMs applies. offsetAtRefMs is additive-to-CPAP at refDate.
    static DriftModel driftToModel(double slopeMsPerDay, qint64 offsetAtRefMs,
                                   const QDate& refDate);
```

- [ ] **Step 2: Write the failing test**

In `oscar/tests/clocksynctests.h`, add inside the `private slots:` block:

```cpp
    void testDriftToModelRoundTrip();
```

In `oscar/tests/clocksynctests.cpp`, add `#include "SleepLib/machine.h"` and `#include "SleepLib/machine_common.h"` near the top includes, then add:

```cpp
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
```

- [ ] **Step 3: Run test to verify it fails**

Run: `./OSCAR20.app/Contents/MacOS/OSCAR20 testDriftToModelRoundTrip` (after the build command in Global Constraints)
Expected: FAIL — `driftToModel` returns default `{0,0.0}`, so additive ≠ expected.

- [ ] **Step 4: Implement `driftToModel`**

In `oscar/clocksyncanalyzer.cpp`, add (e.g. after `fitConstantOffset`):

```cpp
DriftModel ClockSyncAnalyzer::driftToModel(double slopeMsPerDay, qint64 offsetAtRefMs,
                                           const QDate& refDate)
{
    DriftModel m;
    // t_noon must match Machine::correctionMs exactly (UTC noon epoch ms).
    qint64 tRef = QDateTime(refDate, QTime(12, 0, 0), Qt::UTC).toMSecsSinceEpoch();
    m.c1 = -slopeMsPerDay / 86400000.0;
    // Define c0 against the SAME truncated qint64(c1*tRef) the application uses, so
    // the model evaluates to exactly -offsetAtRef at refDate (no rounding at the anchor).
    m.c0Ms = -offsetAtRefMs - qint64(m.c1 * double(tRef));
    return m;
}
```

Note for the implementer: `c1` is always tiny (|c1| = slope/86.4e6 ≪ 1), so the legacy `c1 >= 1.0` sentinel stripping in `Machine::reloadCorrectionsFromDb` (machine.cpp:650-652) never triggers for real fits — no collision.

- [ ] **Step 5: Run test to verify it passes**

Run: `./OSCAR20.app/Contents/MacOS/OSCAR20 testDriftToModelRoundTrip`
Expected: PASS (`Totals: ... passed`).

- [ ] **Step 6: Commit**

```bash
cd /Users/adriangarett.sian/oscar-code
git add oscar/clocksyncanalyzer.h oscar/clocksyncanalyzer.cpp oscar/tests/clocksynctests.h oscar/tests/clocksynctests.cpp
git commit -m "feat(clocksync): driftToModel reparam verified via Machine::correctionMs round-trip"
```

---

### Task 2: `fitDriftLine` core (slope, intercept, MAD, significance, start/end spread)

**Files:**
- Modify: `oscar/clocksyncanalyzer.h` (add `DriftLineFit` struct, constants, `fitDriftLine` decl)
- Modify: `oscar/clocksyncanalyzer.cpp` (add `medianD` helper + `fitDriftLine`)
- Modify: `oscar/tests/clocksynctests.h` (declare two test slots)
- Test: `oscar/tests/clocksynctests.cpp`

**Interfaces:**
- Consumes: `NightStartDiff` (existing), `MIN_OFFSET_NIGHTS`, `OFFSET_OUTLIER_FLOOR_MS` (existing).
- Produces: `struct DriftLineFit { ... }` and
  `static DriftLineFit ClockSyncAnalyzer::fitDriftLine(const QVector<NightStartDiff>& nights);`
  Fits over nights with `used == true`; does **not** mutate `used` (`fitConstantOffset` owns outlier rejection).

- [ ] **Step 1: Add the struct + constants + decl to the header**

In `oscar/clocksyncanalyzer.h`, after the `DriftModel` struct add:

```cpp
//! \brief Robust linear drift fit over per-night recording-window midpoints.
//! offset(night) = offsetAtRefMs + slopeMsPerDay * daysFromRef (additive-to-CPAP).
struct DriftLineFit {
    bool   valid          = false;
    double slopeMsPerDay  = 0.0;
    qint64 offsetAtRefMs  = 0;     // intercept at refDate (additive-to-CPAP)
    QDate  refDate;                // first used night
    QDate  lastDate;               // last used night
    qint64 residualMadMs  = 0;     // MAD of residuals about the line
    qint64 startEndSpreadMs = 0;   // medianEnd - medianStart (behavioural-asymmetry note)
    int    nUsed          = 0;
    qint64 driftSpanMs    = 0;     // |slope| * spanDays
    bool   significant    = false; // true => save drift row; false => caller saves constant offset
    bool   hasJump        = false; // sustained residual step (possible clock re-sync); set in Task 3
    QDate  jumpDate;               // start of the late segment at the changepoint
    qint64 jumpMs         = 0;     // signed late-minus-early residual step
    bool   anchored             = false; // intercept refined by cross-correlation (Task 4)
    bool   anchorSlopeDisagrees = false; // cross-corr slope vs window slope mismatch (Task 4)
};
```

In the `ClockSyncAnalyzer` constants block add:

```cpp
    static constexpr double DRIFT_SIGNIFICANCE_K   = 3.0;       // drift must exceed K * residual MAD
    static constexpr qint64 DRIFT_SIGNIFICANCE_FLOOR_MS = 60000LL; // and at least 60 s over the span
    static constexpr int    ANCHOR_MIN_NIGHTS      = 3;         // min cross-corr nights to anchor (Task 4)
    static constexpr qint64 ANCHOR_MAX_SHIFT_MS    = 600000LL;  // implausible anchor shift => skip (Task 4)
    static constexpr double ANCHOR_SLOPE_TOL_MS_PER_DAY = 5000.0; // slope-disagreement warn (Task 4)
    static constexpr int    JUMP_MIN_SIDE          = 3;         // min nights each side of a changepoint (Task 3)
    static constexpr double JUMP_RESID_K           = 4.0;       // residual-step significance multiple (Task 3)
    static constexpr qint64 JUMP_FLOOR_MS          = 120000LL;  // absolute floor for a flagged jump (Task 3)
```

In the public method block add:

```cpp
    //! \brief Robust linear drift fit over the used nights' recording-window midpoints.
    static DriftLineFit fitDriftLine(const QVector<NightStartDiff>& nights);
```

- [ ] **Step 2: Write the failing tests**

In `oscar/tests/clocksynctests.h` `private slots:` add:

```cpp
    void testFitDriftLineRecoversSlope();
    void testFitDriftLineFlatIsNotSignificant();
```

In `oscar/tests/clocksynctests.cpp` add (the `night(...)` helper already exists in this file; it sets `startDiffMs`/`endDiffMs`/leaves `used == true`):

```cpp
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
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `./OSCAR20.app/Contents/MacOS/OSCAR20 testFitDriftLineRecoversSlope testFitDriftLineFlatIsNotSignificant`
Expected: FAIL — `fitDriftLine` not defined / link error until implemented.

- [ ] **Step 4: Implement `medianD` + `fitDriftLine`**

In `oscar/clocksyncanalyzer.cpp`, in the existing anonymous `namespace { ... }` that holds `medianI64`, add:

```cpp
// Median of a copy of doubles (sorted in place). Returns 0 for empty.
double medianD(QVector<double> v)
{
    if (v.isEmpty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
```

Then add the method (e.g. after `fitConstantOffset`):

```cpp
DriftLineFit ClockSyncAnalyzer::fitDriftLine(const QVector<NightStartDiff>& nights)
{
    DriftLineFit fit;

    QVector<double> xs, ys, starts, ends;
    QDate ref, last;
    for (const NightStartDiff& n : nights) {
        if (!n.used) continue;
        qint64 mid = (n.startDiffMs + n.endDiffMs) / 2;
        if (!ref.isValid()) ref = n.date;
        last = n.date;                       // nights are chronological (collectStartDiffs order)
        xs.append(double(ref.daysTo(n.date)));
        ys.append(double(mid));
        starts.append(double(n.startDiffMs));
        ends.append(double(n.endDiffMs));
    }
    int n = xs.size();
    if (n < MIN_OFFSET_NIGHTS) return fit;

    // Theil-Sen slope: median of pairwise slopes.
    QVector<double> slopes;
    slopes.reserve(n * (n - 1) / 2);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (xs[j] != xs[i]) slopes.append((ys[j] - ys[i]) / (xs[j] - xs[i]));
    if (slopes.isEmpty()) return fit;
    std::sort(slopes.begin(), slopes.end());
    double slope = slopes[slopes.size() / 2];

    // Median intercept at ref (xs are days-from-ref, so intercept = median(y - slope*x)).
    QVector<double> inter(n);
    for (int i = 0; i < n; ++i) inter[i] = ys[i] - slope * xs[i];
    double intercept = medianD(inter);

    // Residual MAD about the line.
    QVector<double> absResid(n);
    for (int i = 0; i < n; ++i) absResid[i] = qAbs(ys[i] - (intercept + slope * xs[i]));
    double mad = medianD(absResid);

    fit.valid           = true;
    fit.slopeMsPerDay   = slope;
    fit.offsetAtRefMs   = qRound64(intercept);
    fit.refDate         = ref;
    fit.lastDate        = last;
    fit.residualMadMs   = qRound64(mad);
    fit.startEndSpreadMs = qRound64(medianD(ends) - medianD(starts));
    fit.nUsed           = n;

    double spanDays = double(ref.daysTo(last));
    fit.driftSpanMs = qRound64(qAbs(slope) * spanDays);
    fit.significant = fit.driftSpanMs >
                      qMax(qint64(DRIFT_SIGNIFICANCE_K * mad), DRIFT_SIGNIFICANCE_FLOOR_MS);
    return fit;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `./OSCAR20.app/Contents/MacOS/OSCAR20 testFitDriftLineRecoversSlope testFitDriftLineFlatIsNotSignificant`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
cd /Users/adriangarett.sian/oscar-code
git add oscar/clocksyncanalyzer.h oscar/clocksyncanalyzer.cpp oscar/tests/clocksynctests.h oscar/tests/clocksynctests.cpp
git commit -m "feat(clocksync): fitDriftLine robust slope/intercept + drift-significance gate"
```

---

### Task 3: Jump detection in `fitDriftLine`

**Files:**
- Modify: `oscar/clocksyncanalyzer.cpp` (`fitDriftLine`: compute residual changepoint)
- Modify: `oscar/tests/clocksynctests.h` (declare test slot)
- Test: `oscar/tests/clocksynctests.cpp`

**Interfaces:**
- Consumes: `DriftLineFit.hasJump`, `.jumpDate`, `.jumpMs` (declared in Task 2), `JUMP_MIN_SIDE`, `JUMP_RESID_K`, `JUMP_FLOOR_MS`.
- Produces: `fitDriftLine` now sets the jump fields.

- [ ] **Step 1: Write the failing test**

In `oscar/tests/clocksynctests.h` `private slots:` add:

```cpp
    void testFitDriftLineDetectsJump();
```

In `oscar/tests/clocksynctests.cpp` add:

```cpp
void ClockSyncTests::testFitDriftLineDetectsJump()
{
    // First 12 nights ~+5 s drift trend, then a +10 min step (clock re-sync), then continues.
    QVector<NightStartDiff> nights;
    QDate d0(2026, 5, 1);
    for (int i = 0; i < 12; ++i) {
        NightStartDiff n = night(1000000 + 5000 * i, 1000000 + 5000 * i, 8000, 1000 + i);
        n.date = d0.addDays(i);
        nights.append(n);
    }
    for (int i = 12; i < 24; ++i) {
        qint64 base = 1000000 + 5000 * i + 600000;  // +10 min jump
        NightStartDiff n = night(base, base, 8000, 1000 + i);
        n.date = d0.addDays(i);
        nights.append(n);
    }

    DriftLineFit fit = ClockSyncAnalyzer::fitDriftLine(nights);
    QVERIFY(fit.valid);
    QVERIFY(fit.hasJump);
    QCOMPARE(fit.jumpDate, d0.addDays(12));
    QVERIFY(qAbs(fit.jumpMs) > ClockSyncAnalyzer::JUMP_FLOOR_MS);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./OSCAR20.app/Contents/MacOS/OSCAR20 testFitDriftLineDetectsJump`
Expected: FAIL — `hasJump` is false (no detection yet).

- [ ] **Step 3: Implement changepoint detection**

In `oscar/clocksyncanalyzer.cpp`, inside `fitDriftLine`, immediately **before** `return fit;`, insert:

```cpp
    // Jump guard: a real clock re-sync shows up as a sustained level shift in the
    // residuals (a tilted single line splits them into same-signed blocks). Scan every
    // split with >= JUMP_MIN_SIDE nights on each side; flag the largest significant step.
    if (n >= 2 * JUMP_MIN_SIDE) {
        QVector<double> resid(n);
        for (int i = 0; i < n; ++i) resid[i] = ys[i] - (intercept + slope * xs[i]);

        double bestAbs = 0.0, bestStep = 0.0;
        int bestK = -1;
        for (int k = JUMP_MIN_SIDE; k <= n - JUMP_MIN_SIDE; ++k) {
            double earlyMed = medianD(resid.mid(0, k));
            double lateMed  = medianD(resid.mid(k, n - k));
            double step = lateMed - earlyMed;
            if (qAbs(step) > bestAbs) { bestAbs = qAbs(step); bestStep = step; bestK = k; }
        }
        double jumpThresh = qMax(JUMP_RESID_K * mad, double(JUMP_FLOOR_MS));
        if (bestK >= 0 && bestAbs > jumpThresh) {
            // Map split index k (in the used-nights sequence) back to its date.
            int seen = 0;
            for (const NightStartDiff& nn : nights) {
                if (!nn.used) continue;
                if (seen == bestK) { fit.jumpDate = nn.date; break; }
                ++seen;
            }
            fit.hasJump = true;
            fit.jumpMs  = qRound64(bestStep);
        }
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./OSCAR20.app/Contents/MacOS/OSCAR20 testFitDriftLineDetectsJump`
Expected: PASS.

- [ ] **Step 5: Re-run the Task 2 tests to confirm no regression**

Run: `./OSCAR20.app/Contents/MacOS/OSCAR20 testFitDriftLineRecoversSlope testFitDriftLineFlatIsNotSignificant`
Expected: PASS (clean trends must NOT trip `hasJump`).

- [ ] **Step 6: Commit**

```bash
cd /Users/adriangarett.sian/oscar-code
git add oscar/clocksyncanalyzer.cpp oscar/tests/clocksynctests.h oscar/tests/clocksynctests.cpp
git commit -m "feat(clocksync): flag sustained residual step as a possible clock jump"
```

---

### Task 4: `anchorIntercept` — cross-correlation refines the absolute level

**Files:**
- Modify: `oscar/clocksyncanalyzer.h` (declare `anchorIntercept`)
- Modify: `oscar/clocksyncanalyzer.cpp` (implement `anchorIntercept`)
- Modify: `oscar/tests/clocksynctests.h` (declare test slot)
- Test: `oscar/tests/clocksynctests.cpp`

**Interfaces:**
- Consumes: `NightStartDiff.validated`, `.validatorOffsetMs` (filled by existing `validateNight`); `DriftLineFit.refDate`, `.slopeMsPerDay`, `.offsetAtRefMs`, `.anchored`, `.anchorSlopeDisagrees`; constants `ANCHOR_MIN_NIGHTS`, `ANCHOR_MAX_SHIFT_MS`, `ANCHOR_SLOPE_TOL_MS_PER_DAY`.
- Produces: `static void ClockSyncAnalyzer::anchorIntercept(const QVector<NightStartDiff>& nights, DriftLineFit& fit);`

- [ ] **Step 1: Declare the method**

In `oscar/clocksyncanalyzer.h` public block:

```cpp
    //! \brief Shift a fitted drift line's intercept onto the behaviour-free cross-corr
    //! offsets (validatorOffsetMs) of validated nights, keeping the window slope.
    static void anchorIntercept(const QVector<NightStartDiff>& nights, DriftLineFit& fit);
```

- [ ] **Step 2: Write the failing test**

In `oscar/tests/clocksynctests.h` `private slots:` add:

```cpp
    void testAnchorInterceptRemovesBehaviouralBias();
```

In `oscar/tests/clocksynctests.cpp` add:

```cpp
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
```

- [ ] **Step 3: Run test to verify it fails**

Run: `./OSCAR20.app/Contents/MacOS/OSCAR20 testAnchorInterceptRemovesBehaviouralBias`
Expected: FAIL — `anchorIntercept` is a no-op / unlinked; `fit.anchored` stays false.

- [ ] **Step 4: Implement `anchorIntercept`**

In `oscar/clocksyncanalyzer.cpp`:

```cpp
void ClockSyncAnalyzer::anchorIntercept(const QVector<NightStartDiff>& nights, DriftLineFit& fit)
{
    if (!fit.valid) return;

    QVector<double> xs, ys;
    for (const NightStartDiff& n : nights) {
        if (!n.used || !n.validated) continue;
        xs.append(double(fit.refDate.daysTo(n.date)));
        ys.append(double(n.validatorOffsetMs));
    }
    if (xs.size() < ANCHOR_MIN_NIGHTS) return;   // too few cross-corr nights -> keep window intercept

    // Refined intercept: median residual of cross-corr offsets against the window slope.
    QVector<double> shifts(xs.size());
    for (int i = 0; i < xs.size(); ++i) shifts[i] = ys[i] - fit.slopeMsPerDay * xs[i];
    qint64 anchored = qRound64(medianD(shifts));

    // Sanity: does a line through the cross-corr offsets share the window slope?
    QVector<double> ccSlopes;
    for (int i = 0; i < xs.size(); ++i)
        for (int j = i + 1; j < xs.size(); ++j)
            if (xs[j] != xs[i]) ccSlopes.append((ys[j] - ys[i]) / (xs[j] - xs[i]));
    if (!ccSlopes.isEmpty()) {
        std::sort(ccSlopes.begin(), ccSlopes.end());
        double ccSlope = ccSlopes[ccSlopes.size() / 2];
        fit.anchorSlopeDisagrees =
            qAbs(ccSlope - fit.slopeMsPerDay) > ANCHOR_SLOPE_TOL_MS_PER_DAY;
    }

    // Only apply a plausible shift; a wild jump means the streams disagree -> leave & warn.
    if (qAbs(anchored - fit.offsetAtRefMs) <= ANCHOR_MAX_SHIFT_MS) {
        fit.offsetAtRefMs = anchored;
        fit.anchored = true;
    } else {
        fit.anchorSlopeDisagrees = true;
    }
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./OSCAR20.app/Contents/MacOS/OSCAR20 testAnchorInterceptRemovesBehaviouralBias`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
cd /Users/adriangarett.sian/oscar-code
git add oscar/clocksyncanalyzer.h oscar/clocksyncanalyzer.cpp oscar/tests/clocksynctests.h oscar/tests/clocksynctests.cpp
git commit -m "feat(clocksync): anchor drift intercept on cross-correlation offsets"
```

---

### Task 5: `upsertDrift` repository save path

**Files:**
- Modify: `oscar/database/device_time_correction_repository.h` (declare `upsertDrift`)
- Modify: `oscar/database/device_time_correction_repository.cpp` (implement `upsertDrift`)

**Interfaces:**
- Consumes: existing `create(const DeviceTimeCorrectionData&)`.
- Produces: `qint64 DeviceTimeCorrectionRepository::upsertDrift(qint64 machineId, const QString& dateFrom, const QString& dateTo, qint64 c0Ms, double c1);`

Note: no DB-backed unit-test harness exists for this repository (other repo methods are untested too), and the reparam→application math is already proven in Task 1 via `Machine::correctionMs`. This task is a thin, self-evident wrapper verified by a clean build + the end-to-end manual check in Task 7. Do not invent a DB test harness here (YAGNI).

- [ ] **Step 1: Declare the method**

In `oscar/database/device_time_correction_repository.h`, after the `upsertTyped` overloads:

```cpp
    // Supersede any active row for this (machine, date range, 'drift') and insert a new
    // drift row carrying the (c0Ms, c1) model. dateTo="" = open-ended.
    qint64  upsertDrift(qint64 machineId, const QString& dateFrom, const QString& dateTo,
                        qint64 c0Ms, double c1);
```

- [ ] **Step 2: Implement the method**

In `oscar/database/device_time_correction_repository.cpp`, after `upsertTyped`:

```cpp
qint64 DeviceTimeCorrectionRepository::upsertDrift(qint64 machineId, const QString& dateFrom,
                                                   const QString& dateTo, qint64 c0Ms, double c1)
{
    QSqlQuery q(database());
    if (dateTo.isEmpty()) {
        q.prepare(R"(
            UPDATE device_time_corrections
            SET undone_at = datetime('now')
            WHERE machine_id = :machine_id
              AND date_from = :date_from AND date_to IS NULL
              AND type = 'drift' AND undone_at IS NULL
        )");
    } else {
        q.prepare(R"(
            UPDATE device_time_corrections
            SET undone_at = datetime('now')
            WHERE machine_id = :machine_id
              AND date_from = :date_from AND date_to = :date_to
              AND type = 'drift' AND undone_at IS NULL
        )");
        q.bindValue(":date_to", dateTo);
    }
    q.bindValue(":machine_id", machineId);
    q.bindValue(":date_from",  dateFrom);
    if (!q.exec()) {
        qCritical() << "DeviceTimeCorrectionRepository::upsertDrift mark-undone failed:" << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DeviceTimeCorrectionRepository::upsertDrift", q);
        return -1;
    }

    DeviceTimeCorrectionData row;
    row.machineId = machineId;
    row.dateFrom  = dateFrom;
    row.dateTo    = dateTo;
    row.type      = "drift";
    row.c0Ms      = c0Ms;
    row.c1        = c1;
    return create(row);
}
```

- [ ] **Step 3: Build to verify it compiles**

Run: `cd /Users/adriangarett.sian/oscar-code/build-test && /opt/homebrew/bin/qmake ../oscar/oscar.pro CONFIG+=test && make -j8`
Expected: build succeeds, no errors referencing `upsertDrift`.

- [ ] **Step 4: Commit**

```bash
cd /Users/adriangarett.sian/oscar-code
git add oscar/database/device_time_correction_repository.h oscar/database/device_time_correction_repository.cpp
git commit -m "feat(db): upsertDrift writes a (c0Ms, c1) drift correction row"
```

---

### Task 6: Wire the dialog to choose drift vs constant and save it

**Files:**
- Modify: `oscar/clocksyncdialog.cpp` (`onAnalyze`, `populateTable`, `onApply`)

**Interfaces:**
- Consumes: `ClockSyncAnalyzer::fitDriftLine`, `anchorIntercept`, `driftToModel`, `DriftLineFit`, `DriftModel`; `DeviceTimeCorrectionRepository::upsertDrift`; existing `collectStartDiffs`, `fitConstantOffset`, `validateNight`.
- Produces: dialog member `DriftLineFit m_drift;` (add to `clocksyncdialog.h` alongside the existing `m_fit`/`m_nights`).

Behaviour: `fitConstantOffset` remains the primary outlier filter and constant estimator; `fitDriftLine` runs over the same `used` nights as an overlay. If the drift is significant, present + save the drift model; otherwise the existing constant path is unchanged.

- [ ] **Step 1: Add the member**

In `oscar/clocksyncdialog.h`, next to the existing `ConstantOffsetFit m_fit;` declaration, add:

```cpp
    DriftLineFit m_drift;
```

Ensure `clocksyncdialog.h` includes the analyzer header (it does via `clocksyncanalyzer.h` in the .cpp; if the struct is referenced in the header, add `#include "clocksyncanalyzer.h"` to `clocksyncdialog.h`).

- [ ] **Step 2: Compute the drift fit in `onAnalyze`**

In `oscar/clocksyncdialog.cpp` `onAnalyze`, after the existing lines:

```cpp
    m_nights = ClockSyncAnalyzer::collectStartDiffs(cpap, oxi, start, end);
    m_fit    = ClockSyncAnalyzer::fitConstantOffset(m_nights);
```

add:

```cpp
    m_drift  = ClockSyncAnalyzer::fitDriftLine(m_nights);   // overlay on the kept nights
```

Then, inside the existing `if (ui->chkValidate->isChecked()) { ... }` block, **after** the per-night `validateNight` loop, add:

```cpp
        ClockSyncAnalyzer::anchorIntercept(m_nights, m_drift);
```

- [ ] **Step 3: Extend the status text for the drift case**

In `onAnalyze`, locate the success block that builds `QString msg = tr("Constant offset: ...")`. Wrap the messaging so a significant drift reports drift instead. Replace the block from `QString msg = tr("Constant offset: ...")` down to `ui->statusLabel->setText(msg);` with:

```cpp
    QString msg;
    if (m_drift.valid && m_drift.significant) {
        double sPerDay   = m_drift.slopeMsPerDay / 1000.0;
        double minPerMon = m_drift.slopeMsPerDay * 30.0 / 60000.0;
        qint64 today     = m_drift.offsetAtRefMs +
                           qint64(m_drift.slopeMsPerDay * double(m_drift.refDate.daysTo(QDate::currentDate())));
        msg = tr("Drift: %1 s/day (≈ %2 min/month). Offset today ≈ %3 min. "
                 "Fitted over %4 nights, %5 to %6 (±%7 s scatter).")
                  .arg(sPerDay, 0, 'f', 1)
                  .arg(minPerMon, 0, 'f', 1)
                  .arg(today / 60000.0, 0, 'f', 1)
                  .arg(m_drift.nUsed)
                  .arg(m_drift.refDate.toString(Qt::ISODate))
                  .arg(m_drift.lastDate.toString(Qt::ISODate))
                  .arg(m_drift.residualMadMs / 1000.0, 0, 'f', 1);
        if (m_drift.anchored)
            msg += " " + tr("Level anchored to event cross-correlation.");
        else
            msg += " " + tr("Level not anchored (few high-AHI nights) — may include a "
                            "behavioural start/stop bias.");
        if (m_drift.anchorSlopeDisagrees)
            msg += " ⚠ " + tr("Cross-correlation slope disagrees — verify before saving.");
        if (m_drift.hasJump)
            msg += " ⚠ " + tr("Possible clock jump (~%1 s step) near %2 — the single drift "
                              "line may be wrong across it; consider narrowing the range.")
                              .arg(m_drift.jumpMs / 1000.0, 0, 'f', 0)
                              .arg(m_drift.jumpDate.toString(Qt::ISODate));
        msg += " " + tr("Saves a drift correction to the CPAP. Re-run periodically as drift grows.");
    } else {
        msg = tr("Constant offset: %1 s (median of %2 nights, ± %3 s).")
                  .arg(m_fit.offsetMs / 1000.0, 0, 'f', 1)
                  .arg(m_fit.nUsed)
                  .arg(m_fit.madMs / 1000.0, 0, 'f', 1);
        if (m_fit.nOutliers > 0)
            msg += " " + tr("%1 outlier night(s) excluded.").arg(m_fit.nOutliers);
        if (m_fit.unstable)
            msg += " ⚠ " + tr("WARNING: offset steps by ~%1 s mid-range — the reference clock "
                              "likely drifted or re-synced. Narrow the date range to one stable "
                              "period before saving.").arg(m_fit.shiftMs / 1000.0, 0, 'f', 0);
        if (qAbs(m_fit.startEndSpreadMs) > 90000)
            msg += " " + tr("Note: start/end differ by %1 s — some of this gap may be when you "
                            "start/stop each device, not clock offset.")
                             .arg(m_fit.startEndSpreadMs / 1000.0, 0, 'f', 0);
        msg += " " + tr("Saves one correction to the CPAP. Uncheck nights to exclude, then Save.");
    }
    ui->statusLabel->setText(msg);
```

(The constant branch is the original text verbatim — only the `if/else` wrapper is new.)

- [ ] **Step 4: Add a residual column to the table**

In `populateTable`, after the cross-check column is set (`t->setItem(row, 5, item(xcheck));`), compute and show the per-night residual against the drift line. Replace the `QString note;` block start with a residual write into column 6 is already used for the note — instead append the residual into the existing note text. Change the note assembly to:

```cpp
        QString note;
        if (!n.used) note = tr("excluded (pairing outlier)");
        else if (n.validated && !n.validatorAgrees)
            note = tr("event cross-check disagrees — possible session mismatch");
        if (n.used && m_drift.valid) {
            qint64 fitVal = m_drift.offsetAtRefMs +
                            qint64(m_drift.slopeMsPerDay * double(m_drift.refDate.daysTo(n.date)));
            qint64 resid  = n.midMs - fitVal;
            const QString r = tr("resid %1 s").arg(resid / 1000.0, 0, 'f', 1);
            note = note.isEmpty() ? r : (note + " — " + r);
        }
        t->setItem(row, 6, item(note));
```

(Removes the old standalone `t->setItem(row, 6, item(note));` — keep only this one.)

- [ ] **Step 5: Branch `onApply` to save drift when significant**

In `onApply`, after recomputing over the checked nights:

```cpp
    ConstantOffsetFit fit = ClockSyncAnalyzer::fitConstantOffset(checked);
```

add:

```cpp
    DriftLineFit drift = ClockSyncAnalyzer::fitDriftLine(checked);
    if (ui->chkValidate->isChecked())
        ClockSyncAnalyzer::anchorIntercept(checked, drift);
    const bool useDrift = drift.valid && drift.significant;
```

In the warnings section, add a jump warning when saving drift (alongside the existing `fit.unstable` / `prevApplied` warnings — only when `useDrift`):

```cpp
    if (useDrift && drift.hasJump)
        warnings << tr("A possible clock jump (~%1 s step) was detected near %2. A single "
                       "drift line may be wrong across it — consider narrowing the range.")
                       .arg(drift.jumpMs / 1000.0, 0, 'f', 0)
                       .arg(drift.jumpDate.toString(Qt::ISODate));
    if (useDrift && drift.anchorSlopeDisagrees)
        warnings << tr("The event cross-correlation slope disagrees with the recording-window "
                       "slope — the drift rate may be unreliable. Verify before saving.");
```

Replace the single save call:

```cpp
    repo.upsertTyped(cpap->getDatabaseId(), fromDate.toString(Qt::ISODate),
                     QString(), "offset", fit.offsetMs);
```

with:

```cpp
    QString savedDesc;
    if (useDrift) {
        DriftModel model = ClockSyncAnalyzer::driftToModel(drift.slopeMsPerDay,
                                                           drift.offsetAtRefMs, drift.refDate);
        repo.upsertDrift(cpap->getDatabaseId(), fromDate.toString(Qt::ISODate),
                         QString(), model.c0Ms, model.c1);
        savedDesc = tr("drift %1 s/day").arg(drift.slopeMsPerDay / 1000.0, 0, 'f', 1);
    } else {
        repo.upsertTyped(cpap->getDatabaseId(), fromDate.toString(Qt::ISODate),
                         QString(), "offset", fit.offsetMs);
        savedDesc = tr("constant offset %1 s").arg(fit.offsetMs / 1000.0, 0, 'f', 1);
    }
```

Update the final status line (replace the existing `ui->statusLabel->setText(tr("Saved constant offset %1 s to %2 ...")...)` call) with:

```cpp
    Machine::reloadCorrectionsFromDb(cpap);
    ui->statusLabel->setText(tr("Saved %1 to %2 (from %3 onward). "
                                "%4 prior correction(s) superseded.")
                                 .arg(savedDesc)
                                 .arg(ui->cpapCombo->currentText())
                                 .arg(fromDate.toString(Qt::ISODate))
                                 .arg(superseded));
```

(The `fromDate` for the drift case is the earliest checked used night — the existing loop that computes `fromDate` is correct as-is. `drift.refDate` equals that same earliest used night, so the saved model's anchor matches `dateFrom`.)

- [ ] **Step 6: Build and run the full clocksync suite**

Run: `cd /Users/adriangarett.sian/oscar-code/build-test && /opt/homebrew/bin/qmake ../oscar/oscar.pro CONFIG+=test && make -j8 && ./OSCAR20.app/Contents/MacOS/OSCAR20`
Expected: build succeeds; all `ClockSyncTests` pass (existing + new). Note any non-clocksync test failures are pre-existing and out of scope — confirm the `ClockSyncTests` block is green.

- [ ] **Step 7: Commit**

```bash
cd /Users/adriangarett.sian/oscar-code
git add oscar/clocksyncdialog.h oscar/clocksyncdialog.cpp
git commit -m "feat(clocksync): dialog saves linear drift when significant, constant otherwise"
```

---

### Task 7: Final verification (full suite + manual app check)

**Files:** none (verification only)

- [ ] **Step 1: Run the complete test binary**

Run: `cd /Users/adriangarett.sian/oscar-code/build-test && ./OSCAR20.app/Contents/MacOS/OSCAR20`
Expected: every `ClockSyncTests` slot passes, including the six new ones:
`testDriftToModelRoundTrip`, `testFitDriftLineRecoversSlope`, `testFitDriftLineFlatIsNotSignificant`, `testFitDriftLineDetectsJump`, `testAnchorInterceptRemovesBehaviouralBias`, and the pre-existing suite.

- [ ] **Step 2: Build the real app (non-test) and smoke-test the dialog**

Run: `cd /Users/adriangarett.sian/oscar-code/build-clocksync && /opt/homebrew/bin/qmake ../oscar/oscar.pro CONFIG+=debug && make -j8`
Then launch, open the Auto-Detect CPAP Clock Offset dialog against the real CPAP + O2-ring data over a multi-week range:
- Confirm the status reports a **drift** rate (s/day, min/month) and an offset-today near the expected ~24 min.
- Confirm Save writes a `drift` row (re-open the dialog: prior corrections show as superseded; offset-today recomputes consistently).
- Confirm a graph view shows oximetry overlaid on CPAP events aligned across the whole range, not just the window midpoint.

- [ ] **Step 3: Final commit (if any verification fixups were needed)**

```bash
cd /Users/adriangarett.sian/oscar-code
git add -A && git commit -m "test(clocksync): verify drift correction end-to-end"
```

---

## Self-Review

**Spec coverage:**
- Linear drift model + verified reparam → Task 1. ✅
- Robust slope/intercept fit from recording-window midpoints → Task 2. ✅
- Constant-vs-drift significance threshold (`|slope|·span > max(3·MAD, 60s)`) → Task 2. ✅
- Defensive jump detection (warn, not block) → Task 3 (detect) + Task 6 (warn+override in onApply). ✅
- Cross-correlation intercept anchor + slope-disagreement + few-nights fallback → Task 4. ✅
- Save path that writes a slope (`upsertDrift`) → Task 5. ✅
- Dialog branch drift/constant + status (s/day, min/month, span, scatter, anchor, jump) + residual column → Task 6. ✅
- date_from = first used night, open-ended forward, "re-run periodically" advisory → Task 6 status + onApply `fromDate`. ✅
- Existing constant behaviour + tests untouched (fitConstantOffset still owns outlier rejection) → Tasks 2/6 design note. ✅
- Cross-correlation internals untouched → Global Constraints. ✅

**Placeholder scan:** No TBD/TODO/"handle edge cases"; every code step shows full code; constants given concrete values.

**Type consistency:** `DriftLineFit`/`DriftModel` fields and the three method signatures are identical across Tasks 1–6; `upsertDrift` signature matches between Task 5 (decl/impl) and Task 6 (call); `medianD` defined once (Task 2) and reused (Tasks 3, 4).

**Known limitations carried from spec (accepted, not gaps):** behavioural *trend* can bias slope (guarded by anchor slope-disagreement warning); long extrapolation can deviate (re-run advisory); jump detection is heuristic (warn + override). No auto-segmentation, no drift chart (YAGNI).
