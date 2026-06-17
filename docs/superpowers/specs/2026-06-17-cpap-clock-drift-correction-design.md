# CPAP Clock Drift Correction — Design

Date: 2026-06-17
Status: Approved (brainstorming) — ready for implementation plan
Branch: v2-garett

## Problem

The CPAP records timestamps on a clock that **drifts gradually** (free-running RTC,
roughly linear ppm error). The user's reference device (O2 ring / Viatom, time
synced via its app) is trusted as correct. Accumulated drift is already on the
order of ~24 minutes, and growing.

The shipping "Auto-Detect CPAP Clock Offset" dialog measures a **constant** offset
and saves it as a single open-ended `"offset"` correction row. That is the wrong
model for a drifting clock:

- A constant freezes the *average* offset over the analysis window. It is only
  correct at the window's midpoint date; at the edges it is off by
  `slope × halfRange`, and it grows wrong for every date after the window.
- Historical data before the window is left wrong.
- The user must re-run the tool forever as drift accumulates.

The infrastructure to do this correctly **already exists but is disconnected**:

- `Machine::correctionMs` already applies a time-varying drift model per night:
  `correction(night) = −(c0Ms + c1·t_noon)` where `t_noon` is the UTC-noon epoch
  ms of that night (`oscar/SleepLib/machine.cpp:670-675`).
- The DB model `DeviceTimeCorrectionData` supports a `"drift"` type with `c0Ms`
  (intercept) and `c1` (slope).
- `ClockSyncAnalyzer::fitDrift` already computes a robust slope/intercept — but it,
  `analyze`, `DriftFit`, and `NightSyncResult` have **zero callers** outside the
  analyzer and its tests. Dead code in the production path.
- There is **no** save path that writes a slope: `upsertTyped` only accepts
  `offsetMs`. `c0Ms`/`c1` can only be written via `create()`.

## Goal

When the data shows real drift, fit a **linear drift model** from the
recording-window differences, anchor its absolute level with event
cross-correlation, detect clock *jumps* defensively, and save it as a `"drift"`
correction row that the existing engine applies time-varyingly. Fall back to the
existing constant `"offset"` behavior when drift is not significant.

## Sign / orientation conventions (single source of truth)

- `correctionMs` returns an **additive-to-raw** correction (offset branch does
  `total += offsetMs`; `offsetMs` documented "add to CPAP raw timestamps,
  positive = CPAP behind reference").
- Therefore the drift branch `total −= (c0Ms + c1·t_noon)` means
  `additive = −(c0Ms + c1·t_noon)`.
- `NightStartDiff.midMs = rFirst − cFirst`, positive = CPAP behind reference —
  **same orientation** as `offsetMs`. The whole chain is consistent.
- `offsetAdd(night)` (what we fit) = additive-to-CPAP offset, positive = CPAP behind.

## The reparam (the bug-prone core) — VERIFIED

We fit `offsetAdd(night) = offsetAtRefMs + slopeMsPerDay · daysFromRef`.

Let `t = t_noon(night)` (epoch ms, UTC noon), `t_ref = t_noon(refDate)`,
`k = slopeMsPerDay / 86 400 000`.

Set `additive = offsetAdd`:

```
−(c0Ms + c1·t) = offsetAtRefMs + k·(t − t_ref)
```

Match coefficients:

```
c1   = − slopeMsPerDay / 86 400 000
c0Ms =   (slopeMsPerDay / 86 400 000) · t_ref  −  offsetAtRefMs
```

Endpoint checks:
- night = refDate → `additive = offsetAtRefMs` ✓
- night = refDate + N days → `additive = offsetAtRefMs + slopeMsPerDay·N` ✓
  (UTC-noon spacing is exactly 86 400 000 ms/day, so DST never contaminates the
  day step.)

Numerics: `c0Ms ≈ +8e7`, `c1·t_noon ≈ −8e7` cancel to the ~1.4e6 ms result; double
keeps ~16 sig digits so the cancellation leaves ~1e-8 ms error. Safe. The
round-trip test asserts **ms accuracy**, not just sign (the legacy `c1 = slope+1.0`
sentinel at machine.cpp:650-652 proves this conversion has been mis-stored before).

## Pipeline

1. **`collectStartDiffs(cpap, ref, start, end)`** → per-night `NightStartDiff`
   with `midMs`. **Unchanged.** Already chronological.

2. **`fitDriftLine(QVector<NightStartDiff>&) → DriftLineFit` (NEW, pure).**
   Theil–Sen slope + median intercept + MAD residual outlier rejection. Reuses
   the algorithm currently in `fitDrift`, but fed from `NightStartDiff.midMs`
   instead of the dead `NightSyncResult` path. Sets `NightStartDiff.used` in place.
   Returns: `slopeMsPerDay`, `offsetAtRefMs`, `refDate`, `residualMadMs`, `nUsed`,
   `outliers`, plus jump fields (below). Slope is robust to behavioral start/stop
   scatter because behavioral delta is just zero-mean noise around the line.

3. **Intercept anchor (cross-correlation).** On nights with enough CPAP apnea
   events and ref desat/surge events (`MIN_EVENTS` / `MIN_REF_EVENTS`),
   `correlateStreams` + `combineStreams` give a **behavior-free** absolute offset
   per night (already implemented; subtracts the nominal physiological delay).
   Trust the **slope** from step 2 (high coverage); shift the **intercept** to
   best-fit the cross-corr offsets:
   `offsetAtRefMs ← median(crossCorr_i − slope · x_i)`.
   This removes a consistent behavioral start/stop bias from the absolute level.
   - **Slope sanity check:** if a line fit to the cross-corr offsets has a slope
     that disagrees with the recording-window slope beyond tolerance, warn (guards
     the behavioral-trend bias of step 2).
   - **Anchor fallback:** if anchor-eligible nights `< ANCHOR_MIN_NIGHTS`, skip the
     anchor, keep the recording-window intercept, and warn "intercept may carry a
     behavioral start/stop bias."

4. **Jump guard (defensive — user does not know if the clock jumps).** After the
   line fit, test residuals for a *sustained same-signed block* (changepoint), not
   lone outliers. If found → set `hasJump`, estimate jump date and size →
   **warn + require confirmation** ("Save anyway"), matching the existing dialog
   warning pattern. Do **not** silently fit across a jump, and do **not** hard-block
   (auto-segmentation is out of scope; see below).

5. **Constant-vs-drift decision.** Compute `driftSpanMs = |slopeMsPerDay| ·
   spanDays`. Use the **drift** row iff
   `driftSpanMs > max(3 · residualMadMs, DRIFT_SIGNIFICANCE_FLOOR_MS)`
   (floor ≈ 60 s). Otherwise the slope is not significant → save the existing
   constant `"offset"` row. Avoids flip-flop and tiny-slope extrapolation noise.

## New / changed components

- `ClockSyncAnalyzer::fitDriftLine(QVector<NightStartDiff>&) → DriftLineFit`
  — pure, unit-testable. (New `DriftLineFit` struct, or extend the existing
  `DriftFit`; whichever keeps the dead `analyze()` path from re-entangling.)
- `ClockSyncAnalyzer::anchorIntercept(...)` — pure; folds cross-corr offsets into
  the intercept and produces the slope-disagreement / fallback flags.
- `ClockSyncAnalyzer::driftToModel(slopeMsPerDay, offsetAtRefMs, refDate)
  → {c0Ms, c1}` — pure; **the reparam landmine**; heavily tested.
- `DeviceTimeCorrectionRepository::upsertDrift(machineId, dateFrom, dateTo,
  c0Ms, c1)` — supersede actives + `create()` a `"drift"` row. (`upsertTyped`
  cannot carry a slope.)
- `ClockSyncDialog::onApply` — branch: significant slope → `upsertDrift`; else
  existing `upsertTyped("offset")`. Supersede **all** active rows first (unchanged;
  prevents double-correction across offset/drift/legacy rows).
- `ClockSyncDialog` status + table UI (below).
- Cross-correlation internals (`correlateStreams`, `scanLags`, `buildDensity`,
  `detectDrops`, `detectSurges`) — **untouched**; already sound.

## Save-row semantics

- `dateFrom` = first fitted (used) night.
- `dateTo` = empty (open-ended forward) so the model extrapolates to future nights.
- Nights **before** `dateFrom` get no correction; if CPAP data exists earlier,
  surface a "backward gap" note (do not extrapolate backward across an unknown
  possible jump).
- Open-ended forward means the model keeps extrapolating; status advises
  "re-run periodically" since real drift rate can change over long spans.

## UI (clocksyncdialog)

Status text shows:
- slope as **s/day and min/month**,
- offset-as-of-today,
- fitted span + N nights used,
- residual spread (MAD),
- anchor result (applied / skipped-with-reason),
- jump warning (if any),
- backward-gap / extrapolation notes.

Results table gains a **residual** column (per-night `midMs` minus the fitted
line). Uncheck-to-exclude + recompute behavior stays.

## Testing

1. **`driftToModel` round-trip** through the real `Machine::correctionMs` at
   multiple dates, including offsets of opposite sign — assert **ms accuracy**.
2. **`fitDriftLine` recovers slope** from noisy recording-window midpoints
   (extend the existing `testFitDriftRecoversSlope`).
3. **Jump detection:** synthetic mid-range step → `hasJump` true, warn path taken.
4. **Anchor:** inject a constant behavioral bias into `midMs`, give unbiased
   cross-corr offsets → anchored intercept removes ~the injected bias; slope
   unchanged.
5. **Significance threshold:** pick `offset` vs `drift` correctly on flat vs
   sloped synthetic data.
6. **Anchor fallback:** few high-AHI nights → anchor skipped + warn flag set.
7. All existing `clocksynctests` stay green.

## Scope cuts (YAGNI)

- **No auto-segmentation.** Jumps are detected and warned only; fitting a line per
  segment is future work.
- **No drift-line chart.** Status text + residual column are enough.
- **No changes to cross-correlation internals.**
- The dead `analyze()` / `fitDrift` / `NightSyncResult` path is not revived as a
  feature; its slope algorithm is lifted into `fitDriftLine`.

## Known limitations (stated, accepted)

- A behavioral start/stop *trend* over time would fold into the slope; mitigated
  by the cross-corr slope-disagreement warning, not eliminated.
- Linear model extrapolated far beyond the fitted span can deviate (RTC aging,
  temperature); mitigated by the "re-run periodically" advisory.
- Jump detection is heuristic: it warns and lets the user override rather than
  guaranteeing detection.

## New constants (proposed; tune in implementation)

- `ANCHOR_MIN_NIGHTS` — minimum cross-corr-eligible nights to apply the anchor.
- `DRIFT_SIGNIFICANCE_FLOOR_MS` ≈ 60 000 — absolute floor for drift-vs-constant.
- Jump-detection thresholds (sustained-block length, residual step size).
