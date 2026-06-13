# Clock-Sync Accuracy Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make CPAP clock-offset auto-detection accurate to ~1 s per night and sub-second drift-rate across nights, by fixing correlation biases and adding robust cross-night fitting.

**Architecture:** Extract the correlation math in `ClockSyncAnalyzer` into pure static functions (`correlateStreams`, `detectDrops`, `detectSurges`, `combineStreams`, `fitDrift`) that are unit-testable without Profile/Machine/Session. `analyze()` becomes a thin data-collection wrapper. The dialog gains a drift-fit summary and auto-unchecks outlier nights.

**Tech Stack:** Qt 6 / C++17, QTest via existing `CONFIG+=test` build (`tests/AutoTest.h` pattern).

**The 9 improvements (from review):**
1. Per-lag normalization (fixes bias toward small offsets)
2. Physiological-delay compensation (SpO₂ ~25 s, pulse ~15 s)
3. Use real timestamps instead of assuming 1 Hz SpO₂
4. SpO₂ artifact filtering (valid range [50, 100])
5. Window from actual data span (remove 12 h cap)
6. Two-stage scan (5 s coarse → 1 s fine) + parabolic sub-bin peak interpolation
7. Cross-night Theil–Sen drift regression + MAD outlier flagging
8. Confidence = peak prominence z-score + edge-peak rejection (replaces magic 0.35)
9. Second event stream: pulse-rate surges, confidence-weighted combination

---

### Task 1: New analyzer API + tests (red)

**Files:**
- Modify: `oscar/clocksyncanalyzer.h` (new structs `StreamCorrelation`, `NightEstimate`, `DriftFit`; new fields on `NightSyncResult`; new static methods + constants)
- Create: `oscar/tests/clocksynctests.h`, `oscar/tests/clocksynctests.cpp`
- Modify: `oscar/oscar.pro` (register test files in the `test {}` block)

- [x] Step 1: Write `clocksynctests.{h,cpp}` — 14 tests covering: known-offset recovery, large-offset unbiasedness (item 1), sub-bin precision (item 6), noise rejection + empty inputs (item 8), >12 h night (item 5), drop detection basic/artifact/slow-rate (items 3, 4), surge detection (item 9), physio-delay compensation + stream combination (items 2, 9), Theil–Sen slope recovery + outlier flagging (item 7). Synthetic events from seeded `QRandomGenerator` — deterministic.
- [x] Step 2: Declare new public API in `clocksyncanalyzer.h`. Constants: `MIN_ZSCORE=4.0`, `MIN_SCORE=0.10`, `MIN_REF_EVENTS=5`, `COARSE_BIN_MS=5000`, `COARSE_SIGMA_MS=20000`, `FINE_BIN_MS=1000`, `FINE_SIGMA_MS=10000`, `FINE_RANGE_MS=120000`, `SPO2_DELAY_MS=25000`, `PULSE_DELAY_MS=15000`, `STREAM_DISAGREE_MS=60000`.
- [x] Step 3: Register tests in `oscar.pro`.

### Task 2: Implement pure correlation core

**Files:** Modify: `oscar/clocksyncanalyzer.cpp`

- [x] `scanLags()` (file-local): normalized cross-correlation over a lag range; per-lag normalization by overlapping-window energies via prefix sums of squares (item 1).
- [x] `correlateStreams()`: time base from actual event span (item 5); coarse pass ±60 min at 5 s bins; z-score of peak vs lag-curve mean/std + edge rejection (item 8); fine pass ±120 s at 1 s bins around coarse peak; parabolic interpolation through peak ± 1 (item 6).
- [x] `buildDensity()` gains `binMs` parameter.

### Task 3: Implement event detectors

**Files:** Modify: `oscar/clocksyncanalyzer.cpp`

- [x] `detectDrops(timesMs, spo2)`: pure; valid range [50, 100] (item 4); baseline = max valid sample over trailing 60 s **by timestamp** (item 3); ≥3 % drop, nadir timestamp recorded, recovery within 1 % of baseline.
- [x] `detectSurges(timesMs, pulse)`: mirror logic; valid [30, 220] bpm; baseline = min over trailing 45 s; ≥10 bpm rise; recovery within 3 bpm (item 9).
- [x] `collectRefEvents(Session*, ChannelID, bool)` wrapper extracts real timestamps via `EventList::time(i)` (no 1 Hz assumption) and dispatches to the pure detectors. Replaces `collectOxiDropTimes`.

### Task 4: Combination + drift fit

**Files:** Modify: `oscar/clocksyncanalyzer.cpp`

- [x] `combineStreams(spo2, pulse)`: subtract physiological delays per stream (item 2); if both accepted and |disagreement| > 60 s use stronger z only, else z-weighted mean (item 9).
- [x] `fitDrift(results)`: Theil–Sen (median pairwise slope, median intercept) over accepted nights; MAD residual outliers flagged `driftOutlier` with threshold `max(3.5·1.4826·MAD, 5 s)` (item 7).

### Task 5: Rewire `analyze()` + dialog

**Files:**
- Modify: `oscar/clocksyncanalyzer.cpp` (`analyze()` uses new core, optional `DriftFit*` out-param)
- Modify: `oscar/clocksyncdialog.h` (add `DriftFit m_fit;`)
- Modify: `oscar/clocksyncdialog.cpp` (drift summary in status, z-score in confidence column, auto-uncheck drift outliers)

- [x] `analyze()` collects CPAP events + SpO₂ drops + pulse surges, per-night `correlateStreams` × 2 + `combineStreams`, then `fitDrift`.
- [x] Dialog: checkbox default `accepted && !driftOutlier`; status shows drift slope s/day, nights used, outlier count.

### Task 6: Verify

- [x] Build test target: `cd build-test && qmake ../oscar/oscar.pro CONFIG+=test && make -j10 && ./test` → all ClockSyncTests PASS. (Pre-existing unrelated suites may skip on missing data.)
- [x] Build app target: `cd build-clocksync && make -j10` → links clean.
- [x] Commit (only clocksync files + plan, not unrelated working-tree changes).

---

## Revision 2 (2026-06-13): simplify to constant offset

Empirical finding from the user's oscar.db: the Viatom is the trusted clock; the
ResMed is ~23.4 min behind it; the offset is a **constant** (28/31 nights at 22–25 min,
<1 min stdev over a month), not a drift. Three independent estimates converge —
legacy migrated correction (+24 min on ResMed), session-start-difference median
(~23.4 min), and the apnea↔SpO₂ cross-correlation (−22.4 min on the one high-AHI night
it fired). The cross-correlation only produced a result on 3/31 nights (low-AHI), and
when it did it merely re-derived the start-difference; per adversarial review its
`buildDensity` is never mean-subtracted, so its NCC peak is driven by recording-window
overlap (≈ the start-diff) rather than independent physiological coupling.

**Decision (user chose option #1): SIMPLIFY + FIX.**

- New PRIMARY estimator: `collectStartDiffs` + `fitConstantOffset` — median of per-night
  midpoints of (refStart−cpapStart, refEnd−cpapEnd), MAD outlier rejection. One constant.
- Apply-direction BUG FIX: write the offset to the **CPAP** (ResMed), positive sign
  (matches legacy +24 min row), and supersede prior offset/drift rows (incl. the legacy
  migrated row) to avoid double-correction. Old code wrote to the Viatom (correct clock).
- Cross-correlation demoted to an optional `chkValidate` toggle (default off) that only
  *flags* nights disagreeing with the window offset — it never drives the saved value.
- Tests: 4 new `fitConstantOffset` cases (clean / artifact-rejection / too-few / start-end
  spread). All 20 ClockSyncTests pass; app + test targets build clean.

## Revision 3 (2026-06-13): don't trust the reference blindly — jump guard

We never independently verified the Viatom is the absolute truth; we assume it (the user's
statement + the fact the ring re-syncs from a phone while the ResMed free-runs). The Viatom
is only correct as of its last phone sync, so it can drift too. Added a guard so a shifted
reference clock surfaces instead of being silently propagated into the CPAP:

- `ConstantOffsetFit` gains `unstable` + `shiftMs`. `fitConstantOffset` compares the
  early-half vs late-half median of the chronologically-ordered kept nights; a step
  > `OFFSET_JUMP_MS` (5 min) sets `unstable` (catches a mid-range re-sync that the single
  median would smear into a meaningless in-between value).
- Dialog: `onAnalyze` shows a ⚠ warning when `unstable`. `onApply` also compares the new
  offset against the previously-saved correction (sum of active non-drift rows); if either
  the intra-range step or the cross-run change exceeds 5 min, it pops a confirm dialog
  (Save / Cancel, default Cancel) before overwriting.
- Tests: `testFitConstantOffsetStableNoJump`, `testFitConstantOffsetDetectsMidRangeStep`.

