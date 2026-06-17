# Compliance Report — Chart Upgrades Design

**Date:** 2026-06-17
**Branch:** v2-garett
**File touched:** `oscar/compliancereport.cpp` (+ `.h` if new helpers need declaring)
**Scope:** Charts only. No narrative/verdict text, no hero/summary page, no sleep-stage charts.

## Goal

Make the OSCAR compliance report charts more beautiful, clearer, and more detailed so a
sleep physician is impressed by the depth — without sacrificing clinical credibility.

## Research basis (why these choices)

Web research across ResMed AirView (official Report Guide), Philips DreamMapper / Care
Orchestrator, SleepHQ, OSCAR, Nonin oximetry conventions, AASM, and CMS LCD L33718
established the de-facto clinical standard:

- **House style = "Option A" (AirView-faithful).** One vertical bar per night; for
  continuous channels the bar top encodes the 95th percentile with a thin dark **median
  tick** inside; discrete channels (usage, AHI) use solid/stacked bars; thresholds are
  **dashed horizontal lines**. OSCAR already implements this exactly — so we **keep the bar
  geometry** and do **not** adopt box-whisker / floating-capsule / envelope styles (no
  clinical precedent; they reduce recognizability).
- The "wow" comes from **filling gaps**, not restyling bars: shaded reference *zones*
  (standards call for acceptable-vs-out-of-range bands, not just lines), splitting central
  apnea out of the events bar, drawing the prescribed pressure bounds, inline legends, an
  oxygen distribution summary, and a single worst-night oxygen trace.

Key sources: ResMed AirView Report Guide (1018991); CMS LCD L33718; AASM PAP guideline;
Nonin nVISION operator manual; SleepHQ public dashboard.

## House rules (apply to all report text)

- **Plain clinical English only.** No internal codes or jargon in any title, axis, or
  legend. Spell out abbreviations on first use; show thresholds inline
  (e.g. "Oxygen Desaturation Index (≥3% drops)").
- **Honesty before polish.** Never draw data that wasn't measured; never let an artifact
  read as pathology (see Accuracy requirements).
- Reference bands appear **only** on charts with real clinical thresholds (oxygen, events,
  leak). **No invented "normal bands"** on raw breathing channels (minute vent, resp rate,
  tidal volume, Ti/Te).

## Existing architecture (reuse, don't reinvent)

All charts are drawn with QPainter into an SVG and embedded as **true PDF vector** content
via the placeholder → `ChartObjectHandler` path. New charts MUST use the same path.

- `SvgChart` (compliancereport.cpp:729) — canvas/painter/plot-rect boilerplate.
- Drawing helpers: `drawYGridAndLabels` (:500), `drawXGridLines` (:535),
  `drawXAxisLabels` (:549), `drawThresholdLine` (:626), `drawBars` (:636),
  `drawChartTitle` (:703).
- Canvas constants: `CHART_MAX_W=1500`, `CHART_TITLE_H=50`, `CHART_H=410`,
  `CHART_LM=86`, `CHART_RM=150` (:461–467).
- Embedding: `makeChartBlockHtml` (:1024) registers SVG bytes into `chartRegistry` and
  emits `<img src='oscarchart:N'>`; `ChartObjectHandler::drawObject` (:1449) replays it.
- Data access on `Day`: `percentile(chan,p)`, `timeAboveThreshold(chan,thr)`,
  `timeBelowThreshold(chan,thr)`, `count(chan)`, `getSessions(mt)`; raw samples via
  `Session` → `EventList` (`count()`, `time(i)`, `data(i)`) — pattern proven in
  `Graphs/gLineChart.cpp`.

## New shared helpers

1. **`drawReferenceBands(painter, chartRect, yMin, yMax, zones)`** — fills horizontal
   shaded zones behind the plot before bars/grid are drawn. `zones` = list of
   `{lo, hi, color, opacity}`. Drawn first so bars and gridlines sit on top.
2. **`drawLegend(painter, chartRect, items)`** — compact swatch+label row.
   `items` = list of `{glyph, color, label}` where glyph ∈ {filledRect, line, dashedLine,
   tick}. **Placement:** top-right inside the plot area, single row, small font; must not
   overlap data. If a chart's right margin already carries a threshold annotation
   (AHI 5, leak 24), place the legend top-left under the title instead. Collision is a
   per-chart choice, defaulted in each render function.

## Chart-by-chart changes

### 1. Events per hour (AHI) — split out Central apnea
`renderAhiChart` (:1109). Today: `ai=(Obstructive+ClearAirway+Apnea)/hrs`,
`hi=Hypopnea/hrs`, two-segment stack.

- Recompute three segments per night:
  - **Obstructive** = `(count(CPAP_Obstructive)+count(CPAP_Apnea))/hrs` (Apnea =
    unclassified obstructive-type).
  - **Central** = `count(CPAP_ClearAirway)/hrs`.
  - **Hypopnea** = `count(CPAP_Hypopnea)/hrs`.
- Stack bottom→top: Obstructive, Central, Hypopnea. Colors: Obstructive `#4f46e5`,
  Central `#fb7185` (deliberately distinct so treatment-emergent central apnea stands out),
  Hypopnea `#a5b4fc`. Keep rounded-top overdraw technique already in the function.
- Keep the AHI=5 dashed success line (`#10b981`) and "AHI 5" annotation.
- Add a shaded **green "controlled" zone 0–5** behind the bars (only if `yMax≥5`).
- Add inline legend: Obstructive / Central / Hypopnea.
- yMax logic unchanged (driven by total per-night AHI).

### 2. Leak rate — good-seal zone + legend
`renderLeakChart` (:1200). Keep amber bar to 95th percentile + dark median tick + red
dashed limit line.

- Add reference bands: green `0 → redline` (good seal), light-red tint `redline → yMax`.
- Add inline legend: 95th percentile (amber swatch) / median (dark tick) / leak limit
  (red dashed, value).

### 3. Pressure — prescribed bounds, branched by mode
`renderPressureChart` (:1247). Keep blue bar to 95th percentile + median tick +
auto-floored yMin.

- **Fixed CPAP:** draw a single dashed line at the set pressure, labeled
  "Set pressure X.X". (Mode/pressure already available from the settings blocks.)
- **APAP / auto:** draw two dashed bounds at set-min and set-max + a faint shaded window
  between them. Read prescribed min/max from settings (`CPAP_PressureMin`/`CPAP_PressureMax`
  or `settings_min/settings_max`; confirm exact channels at implementation). If a bar's
  95th percentile reaches set-max, that is "pressure capping" — leave it visually obvious
  (bar touches the upper bound); no extra callout required for v1.
- Bounds must be included when computing yMax/yMin so they're always on-canvas.
- Add inline legend: 95th percentile / median / set pressure (or set min–max).

### 4. SpO2 nightly chart — clinical bands + legend
`renderChannelBarChart` (:1296), SpO2 branch. Keep current behavior (bar to **median P50**,
tick at **P5 nightly nadir**, 90% dashed line, yMin auto-floor).

- Add reference bands: green ≥95, amber 90–95, light-red <90.
- Add a second dashed line at **88%** (CMS oxygen threshold), lighter than the 90% line.
- Add inline legend: nightly median / nightly low / 90% & 88% thresholds.
- Other continuous channels (minute vent, resp rate, tidal volume, pulse): **legend only,
  no bands.**

### 5. Overnight Oxygen Summary — NEW panel (HTML block)
New section in the oximetry area. Rendered as an HTML block (horizontal stacked bar +
stat grid) consistent with the existing stats tables — not a vector chart — because it's a
single distribution bar plus numbers.

- **Time-in-range bar** (% of total SpO2 recording time):
  - ≥95% (green `#22c55e`), 90–95% (amber `#eab308`), <90% (red `#ef4444`).
  - Derive from `timeAboveThreshold(OXI_SPO2, 95/90)` against total recording time
    (`spo2RecordingSecs`, already computed :1750). Segment widths = percentages.
- **Indices** (reuse existing values; consolidate the scattered page-5/page-6 numbers):
  - Oxygen Desaturation Index — `spo2DropIndex` (:2396), **labeled with the actual drop
    threshold** OSCAR/the device used (confirm at implementation; do not hard-code "≥3%"
    if the source differs).
  - Time below 90% — `t90Secs` (:1746).
  - Time below 88% — NEW: `timeBelowThreshold(OXI_SPO2, 88)` integrated over the period
    (mirror the existing t90 computation).
  - Lowest oxygen — `minSpO2`; Average — `avgSpO2Period`.
- Caption: "covers the whole report period · source: <oximeter device name>".
- This is **additional** to the existing per-night SpO2 bar chart, not a replacement.

### 6. Lowest-Oxygen Night — NEW single trace (SVG vector chart)
New render function `renderLowestOxygenNight(...)`. One whole-night SpO2 line for the
worst night of the period.

- **Night selection:** among days in range with oximetry data, pick the day with the
  lowest nadir (minimum SpO2 above the artifact floor, excluding probe-loss/0%). Tie-break
  by greatest time below 90%.
- **Data:** chosen `Day` → `getSessions(<oximetry source type>)` → `EventList(OXI_SPO2)`;
  iterate `time(i)/data(i)`. Mark scored desaturations from the `OXI_SPO2Drop` FLAG channel
  at their timestamps. Pulse companion (`OXI_Pulse`) is **optional for v1** — include only
  if it doesn't crowd the chart.
- **X axis:** real clock time from first to last sample across the night's sessions.
- **Y axis:** auto-floored (e.g. `min(nadir-5, 88)` up to 100), 90% red dashed + 88% amber
  dashed reference lines.
- **Gaps drawn as gaps:** break the polyline across inter-session gaps and probe-loss spans
  (values at/below `SPO2_ARTIFACT_FLOOR` / 0%). Never connect across a gap; never plot a
  dropout as a desaturation to zero.
- **Label:** "Lowest-Oxygen Night — <date> (lowest reading X%)" + sub-caption
  "single night · source: <device>".
- Embed via the existing vector path (`makeChartBlockHtml` + `chartRegistry`).

## Accuracy requirements (non-negotiable)

- **ODI threshold stated.** Label the desaturation index with the real drop criterion in
  use; do not imply AASM ODI3 vs CMS ODI4 incorrectly.
- **Gaps as gaps.** Probe-loss and missing data are blank, never zero, never interpolated.
  Reuse the existing artifact-floor / probe-loss handling (T90 already excludes 0%).
- **Provenance labeled.** The trace and oxygen summary name the source oximeter and the
  trace is labeled as a single night.
- **No invented data.** Bands are fixed clinical ranges; the trace plots real samples only.

## Out of scope

Sleep-stage charts (hypnogram / stage donut); plain-language verdict text; compliance
pass/fail badge; hero/scorecard page; restyling bars to box-whisker/capsule/envelope;
small-multiple or typical-night oxygen views.

## Testing / verification

- Build OSCAR; generate the compliance report for a profile with oximetry data and verify
  each chart visually against this design (the author's own export is a good fixture).
- Verify mode branching: a fixed-CPAP profile shows one set-pressure line; an APAP profile
  shows min/max bounds + window.
- Verify the worst-night picker selects the true lowest-nadir night and that probe-loss
  spans render as gaps.
- Verify vector output stays sharp on PDF zoom (no rasterization regressions).
- Confirm reference bands sit behind, not over, the bars and gridlines.

## Open implementation details (decide in-code, low risk)

- Exact channels for APAP set-min/max and the configured desaturation drop threshold.
- Legend placement per chart (top-right vs top-left) to avoid collisions.
- Whether the bottom margin / canvas height needs a small increase to host legends.
- Pulse companion inclusion on the trace.
