# AirView V2 — Compliance Report Theme

**Date:** 2026-06-28
**Status:** Approved for planning (revised after 3-agent review)
**Branch:** v2-garett
**Design source:** Claude Design project `1d61a8c0-8732-42f2-9700-8306db48608a`, file `Compliance Report.dc.html`.
Decoded reference copy committed at `docs/superpowers/specs/2026-06-28-airview-v2-mockup.html`
(style block ~lines 14–29; chart render logic ~272–355, 384–563, 726–826).

## Goal

Add a selectable visual theme — **AirView V2** — to OSCAR's Compliance Report export: a
fixed-layout 7-page A4 report reproducing the supplied mockup with near pixel-fidelity, drawn
directly with QPainter. The current report ("Classic") stays the default; its output is unchanged
(its stat-gathering is refactored behind a behavior-preserving extract-method).

## Non-goals

- No change to the Classic report's visual output or behavior.
- No new diagnostic computation requiring data OSCAR doesn't capture. Where the mockup shows a
  metric OSCAR lacks for a given profile, the section is **gated and omitted**, never fabricated.
- Not bundling web fonts — AirView V2 uses the OS UI font (San Francisco on macOS).

### Data-sourcing reality (corrects the earlier "all fields already exist" claim)
Per the field-by-field audit, mockup fields fall into three classes:
- **EXISTS** (~75%) — already computed in `compliancereport.cpp`; AirView V2 just re-presents them.
- **DERIVED** — small new calcs from already-gathered data: IQR quartiles (`q1/q3` of the sorted
  usage-hours array), `% time in desat` (`OXI_SPO2Drop` span seconds ÷ recording seconds),
  `pulse change index` (`calcCount(OXI_PulseChange)/hrs`), per-block `95th leak`, oximeter
  `recording avg/night`, oxi `used/not-used` day counts.
- **GATED (may be absent)** — rendered only when real source data exists, else the element/section
  is dropped:
  - **Sleep Architecture** (stages, TST, bedtime, latency, wake, REM-of-TST) — needs an
    `MT_SLEEPSTAGE` device (Zeo / Apple Watch / Dreem). Absent for nearly all CPAP+oximeter users.
  - **Baseline "−99%"** badge and **"Δ vs base"** history column — need `Machine::untreatedAHI()`,
    an optional value the user enters at profile setup; usually unset.
  - All oximetry pages/rows — gated on oximetry data (as Classic already does).

## Decisions (locked during brainstorming + review)

| Decision | Choice |
|---|---|
| Theme model | Selectable; Classic kept as default option |
| Rendering | **QPainter paged** — each A4 page drawn directly to the printer painter |
| Fonts | **San Francisco**: SF Pro (text/labels) + SF Mono (tabular numbers); fallback off-macOS |
| Stat sourcing | **Shared `ReportData`** via extracted `gatherReportData()` — single source of truth |
| Renderer location | New file `oscar/compliancereport_airviewv2.cpp` (+ `.h`) |
| Shared internals | New header `oscar/compliancereport_internal.h` exposing helpers now trapped in an anon namespace |
| **Sleep Architecture** | **Gate & omit** — render only when `MT_SLEEPSTAGE` data exists; else page 6 = Oximeter Statistics only |
| **Baseline / Δ-vs-base** | **Show only if** `untreatedAHI() > 0` — else omit those specific elements (mirrors Classic) |

## Architecture

### Reachability fix — shared internal header (NEW, required)
Every helper AirView V2 wants to reuse currently lives in **one anonymous namespace**
(`compliancereport.cpp:278–1878`) → internal linkage, invisible to a second translation unit.
`getSettingStr` (`:289`) and `renderLowestOxygenNight` (`:1798`) are additionally `static`;
`SettingsChangeBlock` (`:511`) and `StatsRow` (`:524`) are anon-namespace structs.

Create `oscar/compliancereport_internal.h` with **external linkage** declarations for what AirView V2
and `ReportData` need: `SettingsChangeBlock`, `StatsRow`, `FilteredStats`, `Spo2ZonePercents`,
`NightO2Stat`, and free functions `colorForChannel`, `unitForChannel`, `getSettingStr`,
`calcFilteredStats`, `calcDayFilteredStats`, `calcDayAvgPercentile`, `calcDayAvgMax`,
`spo2ZonePercents`, `getSettingsChanges`. Move these out of the anonymous namespace (define them in
`compliancereport.cpp` with external linkage, or a shared `.cpp`). `ReportData` **must** see
`SettingsChangeBlock` (it holds `QList<SettingsChangeBlock>`), so that struct moving to the header is
mandatory, not optional.

### Dialog (`ComplianceReportDialog`)
- Add a **"Report style"** `QComboBox` (`m_styleCombo`): `Classic` (default), `AirView V2`.
  Accessor `ReportStyle reportStyle() const` returning enum `{ Classic, AirViewV2 }`.
- Add slot `onStyleChanged(int)` that disables/hides `m_airViewCheck` when AirView V2 is selected
  (the content-trim checkbox applies to Classic only). Follows the existing `m_rangeCombo` ↔
  `onRangeChanged` wiring pattern (`compliancereport.cpp:151,162,264`). Header edits in the
  `ComplianceReportDialog` class (`compliancereport.h:76–98`).

### Dispatch (`ComplianceReport::exportReport`)
Keep in `exportReport()` (these are NOT pure computation and must not move into `gatherReportData`):
the no-data `QMessageBox` guard (`:1974–1977`), the `QFileDialog` save prompt (`:2008–2015`,
`savePath` feeds the printer), the `getHoursMinutes`/`formatHoursMinutesShort` lambdas
(`:2264–2277`, used by the Classic renderer), and Classic-only chart-SVG generation (`:2293–2297`).

Flow:
1. Build dialog, read style + options.
2. `ReportData rd = gatherReportData(start, end);` (pure-computation subset).
3. Branch: `Classic` → existing renderer reading from `rd`; `AirViewV2` →
   `renderAirViewV2Pdf(rd, printer, options)` in the new file.

### Shared data — `gatherReportData()` + `ReportData`
Make `gatherReportData` a **`static` member of `ComplianceReport`** (preserves the
`Q_DECLARE_TR_FUNCTIONS(ComplianceReport)` `tr()` context the moved code relies on at `:2221,2234,2240`…),
signature `static ReportData gatherReportData(QDate start, QDate end);` (`p_profile` is a global
`extern`, so no `Profile*` param needed).

**Extract scope** = the pure-computation lines, which are NOT a single contiguous block: most live in
`:1979–2298`, but **ODI, T88, and SpO₂-zone percentages are computed later (`:2890–2903`) and only in
the non-AirView branch** — these must also move into `gatherReportData()` or pages 1/5/6 lose their
numbers. It is behavior-preserving for Classic: the moved lines compute the same values; Classic then
reads `rd.*` instead of inline locals.

`ReportData` (in `compliancereport_internal.h`) — every field below maps to existing math unless
marked **[DERIVED]** (small new calc) or **[GATED]** (may be absent):

```
struct SleepStageStats {            // [GATED] only when MT_SLEEPSTAGE data exists
    bool present = false;
    double deepPct, lightPct, remPct, awakePct;     // SC_SS_*_PCT
    double deepMin, lightMin, remMin, awakeMin, tstMin;  // SC_SS_TST etc.
    QString avgBedtime, avgWake; double sleepLatencyMin, remOfTstPct;
    int sleepStageNights;
};

struct ReportData {
    QDate start, end; int totalDays, presentDays, daysUsed, daysNotUsed, daysNoData;
    QMap<QDate,Day*> daysMap, oxiDaysMap; QList<Day*> daysList, allDays;
    int compliantDays; double pctCompliant, pctUsed, complianceThreshold;
    double totalHours, avgHoursUsed, avgHoursTotal, medianHoursUsed;
    double q1HoursUsed, q3HoursUsed;                         // [DERIVED] IQR (p2)
    double AHI, AI, HI, CentralAI, ObstructiveAI, UnknownAI, ReraIndex, RDI;
    double avgCsrPctPerNight, totalCsrMinutes; QString timeInCsrStr, timeInApneaStr;
    double untreatedAHI, baselinePctChange; bool hasBaseline;   // [GATED] untreatedAHI()>0
    ChannelID leakChan; bool isTotalLeak; double medianLeak, p95Leak, maxLeak;
    double leakRedline, pctOverRedline; QString timeAboveRedlineStr;
    bool hasSpO2, hasPulse; int usedDaysOxi, usedDaysPulse, usedDaysOxiAny;
    int oxiNotUsedDays; double oxiRecordingHrsAvg;          // [DERIVED] (p6)
    double minSpO2, avgSpO2Period, t90Secs, t88Secs, spo2RecordingSecs; bool minSpO2Set;
    double odi, dropPct; Spo2ZonePercents spo2Zones;           // ODI/T88/zones from :2890-2903
    double pctTimeInDesat;                                   // [DERIVED] OXI_SPO2Drop secs/recording
    double minPulsePeriod, maxPulsePeriod, avgPulsePeriod; QString t90Str;
    double pulseChangeIndex;                                 // [DERIVED] calcCount(OXI_PulseChange)/hrs
    MachineType oxiSourceType;
    double p95FlowLimit, flIndex;
    QString brand, model, serial, modeStr, pressureSettings, relief, pressDisplay;
    QString humidLevel, rampTime, rampPres, maskDescription;
    Day* lastDay; CPAPMode p1Mode; bool p1IsCpap; int age; QDate dob; QString patientName;
    double avgPressure, p95Pressure, maxPressure;
    QList<SettingsChangeBlock> settingsBlocks;   // extend struct: add p95Leak + deltaAhiVsBase members
    SleepStageStats sleep;                        // [GATED]
};
```
Also extend `SettingsChangeBlock` (`:511–522`) with `double p95Leak` **[DERIVED]** and
`double deltaAhiVsBase` **[GATED]** members for the page-7 history table.

### QPainter paged engine (new file)
Open `QPainter painter(&printer)`; draw each active page, `printer.newPage()` between. Output stays
vector. The printer is `QPrinter::ScreenResolution` (~96 dpi → A4 width ≈ **794 px**, matching the
mockup's 794-px artboard, so the layout scale `s = pageRect.width()/794.0 ≈ 1.0`). AirView V2 sets
its **own** page margins before constructing the painter (Classic's 8/4 mm at `:2305` are for
`QTextDocument`; AirView V2 likely wants ~0 for the full-bleed masthead). No conflict with Classic:
the dispatch is either/or, and `ChartObjectHandler::drawObject` (`:1896–1904`) already proves the
QPainter+`QSvgRenderer`→printer vector path works.

## Design system (`compliancereport_airviewv2.cpp` internal helpers)

### Fonts + typography scale
```
sans → "SF Pro Text"/"SF Pro Display"  (fallback "Helvetica Neue", app default)
mono → "SF Mono"  (fallback "Menlo", "Courier New") — tabular figures for ALL numbers
```
Enumerate the mockup's scale (artboard px at s≈1.0): **sizes** 25 (scorecard value), 21 (OSCAR
wordmark), 17 (stage value), 16 (compliance value), 15/13 (report title large/compact), 13.5 (info
cell value), 12 (KV), 11.5 (band), 11/10.5/10/9.5/9/8.5/8 (descending labels/axes/footer).
**weights** 400/500/600/700. **letter-spacing** by context: band title `.14em`, masthead tagline
`.2em`, p1 report title `.16em`, chart title `.09em`, table `th` `.07em`, scorecard/cell labels
`.08–.1em`, device wordmark `.03–.04em`. **uppercase** on: band titles, info-cell labels, scorecard
labels, chart titles, table headers, sub-labels, oximeter sub-headers, stage labels, masthead
tagline + report title. (Scale fonts by devicePixelRatio for any canvas-drawn glyphs per the Retina
note in project learnings.)

### Palette (named `QColor` constants)
Core: `NAVY #0B3D74`, `ACCENT #1F6FB2`, `GREEN #2E8B57`, `AMBER #C9881F`, `RED #C0392B`,
`INK #1A2733`. Ink/grey ramp: `GREY1 #54616B`, `GREY2 #8693A0`, `GREY3 #9AA6B0`,
`CAPTION #6B7884`, `BODY #454F58`, `TBODY #38454F`, `LEG #97A2AD`, `METH #7A8794`.
Hairlines/borders: `HAIR1 #C7D2DA`, `HAIR2 #DCE4EA`, `HAIR3 #E6ECF1`, `TBORDER #ECF1F4`.
Fills: `FILL_BLUE #F4F8FB`, `FILL_GREY #F7FAFC`, `TH_BG #EAF2F9`, `TINT_AMBER #FCF8F0`,
`GAUGE_TRK #EEF2F5`. Gridlines: `GRID #EEF0F5`, `GRID_DOT #ECF1F4`, `BASE #C7D2DA`.
Event colors: `EV_CENTRAL #6E6FE6`, `EV_HYP #AEB0F2`, `EV_OBS #312E81`, `EV_UNK #C5C9D4`.
Gauge fills/ticks: leak `#2FA37A`, event `#6E6FE6`, median tick `#17684C`, target tick `#C9881F`,
redline tick `#C0392B`. Band subtitle text `#AFC6DD`. Amber-dark value text `#9A6F1A`/`#955018`.
SpO₂ night tints: `<88 #F0D2CB`, `<90 #F3E2C4`, normal `#C2E2EC`; ticks `#18647E`/`#C0392B`.
Sleep-stage: deep `#0B3D74`, light `#5E93C4`, REM `#C9881F`, awake `#C2CDD6`.
**Per-channel box-whisker triples** (min–max bar / median–95th band / median tick):
leak `#C7E7D8 / #2FA37A / #17684C`, minute-vent `#C9DCF4 / #3B82C4 / #1E4E79`,
resp-rate `#DCCDF1 / #8B5CC4 / #4A2B7A`, tidal-vol `#F6DCBE / #DD8636 / #955018`,
pulse `#F4CED3 / #D2596A / #8A2A38`.

### Draw-helper inventory
- `drawMasthead(p, rect, large)` — waveform mark + "OSCAR / Sleep Therapy Insights" wordmark. **Large**
  (p1): right side = report title + "Generated … · OSCAR <ver>". **Compact** (p2–7): wordmark with a
  left-border separator before the tagline; right side = report-section title + patient name + date range.
- `drawBand(p, rect, title, subtitle)` — navy bar, uppercase `.14em` title, right subtitle in `#AFC6DD`.
- `drawCard(p, rect, joinTop)` — `HAIR1` border; `joinTop` suppresses the top border to butt against a band.
- `drawScorecardCell(p, rect, dotColor, label, value, unit, caption, tint)` — status dot, big mono value,
  `10px #8693A0` unit suffix, `#6B7884` caption; cell bg = `TINT_AMBER` for warn metrics else `FILL_GREY`.
- `drawKVRow(p, rect, label, value, {leadingSwatch})` — label (sans) left, value (mono) right, bottom
  hairline; optional leading colored swatch (p1 Respiratory Events rows).
- `drawInfoGrid(p, rect, cells[])` — patient/device box: bordered grid of label(`.icl` uppercase)/value(`.icv`) cells.
- `drawGauge(p, rect, fillPct, fillColor, ticks[])` — `GAUGE_TRK` rounded track + fill + redline/target ticks + scale labels.
- `drawDistributionBar(p, rect, segments[])` — segmented proportion bar (p5 Overnight O₂ ≥95/90–95/<90;
  p6 sleep stage Deep/Light/REM/Awake, ~28px tall) with stage colors above.
- `drawStatTable(p, rect, cols, rows)` — `TH_BG` navy uppercase `9.5px .07em` header, zebra `FILL_GREY`
  rows, right-aligned mono numeric cells, **bold median col** (`#1A2733` w600), amber-dark min cell
  (`#9A6F1A`), `TBORDER` row separators, `TBODY` text.
- `drawChartLegend(p, rect, items[])` — flex row of rounded-square swatches + `9px #6B7884` labels;
  supports dashed-line markers (mean/redline). Swatch colors must match the per-chart data colors
  (resolve the mockup's grey-placeholder swatches to the real channel triples).
- `drawInfoCallout(p, rect, text)` — `FILL_BLUE` bg, 3px `ACCENT` left border, "i" glyph + `BODY` text.
- `drawSleepStageCards(p, rect, stats)` — 4-col swatch + uppercase label + mono value + sub-time. **[GATED]**
- `drawOximeterSubheader(p, rect, text)` — `ACCENT` uppercase `.1em` ("Usage & saturation" / "Pulse & desaturation").
- `drawFooter(p, rect, pageNo, pageCount, methodology)` — disclaimer left, "n / N" right; **page-7 variant**
  adds a full-width "**Methodology.** …" paragraph (`#7A8794`, `8.5px/1.5`) above the standard footer.
- Chart primitives (re-style of existing render logic):
  - `drawUsageBars` — bars green/amber by compliance, top corners rounded ~1.5px, `1.5px dashed GREEN`
    4 h threshold line with white-bg label + green "4" y-tick.
  - `drawAhiStacked` — stacked event bars + dashed mean line (`#312E81` 0.9px `"4 4"` opacity .55) + legend.
  - `drawBoxWhisker(channel)` — per-channel min–max bar / median–95th band / median tick (leak, MV, RR, TV,
    pulse) using the channel's color triple; redline dashed where applicable; RR adds a green normal-range
    band (`#2E8B57` opacity .08, legend "normal 12–20").
  - `drawPressureChart` — fixed/auto per mode.
  - `drawSpo2Chart` — line + **dual dashed thresholds** (90% `#C9881F` `"5 4"` op.7, 88% `#C0392B` `"5 4"`
    op.6), colored "90"/"88" axis ticks, bold red period-low marker, per-night low tints (`#F0D2CB`/`#F3E2C4`/`#C2E2EC`).
  - `drawLowestOxygenNight` — single-night detail.
  - Gridlines: horizontal solid `GRID #EEF0F5`; mid dotted `GRID_DOT #ECF1F4` `"2 3"`; baseline/tick-stubs `BASE #C7D2DA`.
  - Cull spans by full extent (x1Px), not onset, per project learnings.

## Page-by-page layout → data sources

All values come from `ReportData`. Page numbering ("n / N") is computed after the active page set is
resolved so gated/omitted pages leave no gaps.

1. **Compliance Summary** — large masthead; `drawInfoGrid` patient/device box (patient, DOB, age,
   period, nights / device, serial, mode, pressure+EPR, mask); 6-cell scorecard (Adherence
   `pctCompliant`, Residual AHI `AHI`, Avg usage `avgHoursUsed`+median, Leak 95th `p95Leak`+redline,
   O₂ nadir `minSpO2`+T90, Central idx `CentralAI`+CSR; **baseline "−99%" caption only if `hasBaseline`**);
   Usage bar chart + 6 compliance cells; Mask & Seal gauge + leak KV rows; Respiratory Events gauge +
   event-type KV rows (leading swatches).
2. **Therapy & Device Settings** — Therapy card (leak med/95th/max, AHI, AI·HI, apneas by type, RERA,
   RDI, 95% FL, CSR); Usage Detail card (days used, nights ≥/<4 h, total hours, averages, median,
   **IQR `q1–q3` [DERIVED]**); Device Settings 2-col grid (model, mode, set pressure, EPR, ramp, mask,
   humidifier+level, climate, temperature, temp-enable, response, antibacterial filter, smart start,
   patient view — all from existing `setStr`/`getSettingStr`); clinical info callout.
3. **Efficacy Trends** — AHI stacked bars (mean line, legend); Mask Leak box-whisker + redline;
   Mask Pressure (fixed/auto per mode).
4. **Respiratory Channels** — Minute Ventilation, Respiratory Rate (with normal band), Tidal Volume
   box-whisker trends; honors the dialog's selected channels; each gated on channel presence.
5. **Oximetry** — SpO₂ chart (dual thresholds), Pulse chart, Overnight Oxygen Summary
   (`drawDistributionBar` ≥95/90–95/<90 + KV: ODI₃ `odi`/`dropPct`, T90 `t90Secs`, T88 `t88Secs`,
   **% time in desat [DERIVED]**, lowest/avg SpO₂), Lowest-Oxygen Night detail. **Whole page gated on
   `usedDaysOxiAny > 0`.**
6. **Sleep Architecture & Oximeter Statistics** —
   **[GATED] Sleep Architecture** (`drawSleepStageCards` + `drawDistributionBar`, TST, bedtime,
   latency, wake, REM-of-TST) rendered **only when `rd.sleep.present`**; otherwise omitted and the
   page is Oximeter Statistics only.
   Oximeter Statistics (under `drawOximeterSubheader`s): total/used days, days not used
   **[DERIVED]**, avg hours/night + recording avg **[DERIVED]**, average/min SpO₂, desaturation drop
   index (= `odi`), avg/min/max pulse, **pulse change index [DERIVED]**.
7. **Diagnostics & History** — Detailed Channel Statistics `drawStatTable` (Channel/Unit/Min/Median/
   95th/Max for Pressure, MinVent, RespRate, Leak, Snore, Insp/Exp time, Tidal Vol, Pulse, SpO₂);
   Additional Indices (cumulative time in apnea, time in CSR, over-redline time+%, total hours, min
   SpO₂, T90, 95% FL); Glossary (static); Therapy History `drawStatTable` (Period / Days /
   Mode·Pressure / AHI / **95th Leak [DERIVED]** / Min SpO₂ / **Δ-vs-base [GATED]** from
   `settingsBlocks`); page-7 footer with Methodology paragraph.

## Conditional rendering / data-absence behavior (field-specific)

- **Oximetry pages 5 & oximeter-stats** — gated on `usedDaysOxiAny > 0` / per-row oxi channel presence.
- **Sleep Architecture (p6)** — gated on `rd.sleep.present` (≥1 night of `MT_SLEEPSTAGE` data); else omitted.
- **Baseline badge (p1) & Δ-vs-base column (p7)** — gated on `rd.hasBaseline` (`untreatedAHI() > 0`).
  "base" = the diagnostic `untreatedAHI` (matches the mockup's single-row "−99%"), not the first block.
- **Pressure box/chart** — gated on mode (`p1IsCpap` → show set pressure only).
- **Channel trend tiles (p4)** — only for selected channels that have data.
- Absent scalar values render as "—". No fabricated zeros; missing/empty propagates explicitly
  (project anti-hallucination learning).

## Files changed

| File | Change |
|---|---|
| `oscar/compliancereport_internal.h` | **New.** External-linkage decls: `ReportData`, `SleepStageStats`, hoisted `SettingsChangeBlock` (+ new `p95Leak`/`deltaAhiVsBase`), `StatsRow`, `FilteredStats`, `Spo2ZonePercents`, `NightO2Stat`, and the reused free functions. |
| `oscar/compliancereport.h` | Add `ReportStyle` enum, dialog `m_styleCombo` + `reportStyle()` + `onStyleChanged`; declare `static ReportData ComplianceReport::gatherReportData(...)` and `renderAirViewV2Pdf(...)`. |
| `oscar/compliancereport.cpp` | Move reused helpers/structs out of the anon namespace; extract `gatherReportData()` (incl. ODI/T88/zones at :2890–2903; add IQR + new derived calcs); Classic reads from `ReportData`; add style combo + dispatch. |
| `oscar/compliancereport_airviewv2.cpp` | **New.** QPainter paged renderer + design-system helpers + 7 page builders. |
| `oscar/oscar.pro` | Add `compliancereport_airviewv2.cpp` to `SOURCES` (~:304); add `compliancereport_airviewv2.h` and `compliancereport_internal.h` to `HEADERS` (~:484). No CMake exists. |

## Verification plan

1. **Full profile w/ oximetry (no sleep-stage)** — pages 1–5, 7 render; page 6 = Oximeter Stats only
   (sleep card omitted, no gaps); numbers match Classic for the same range (regression on the extraction).
2. **Profile WITH `MT_SLEEPSTAGE` data** — page 6 shows the sleep card with real values.
3. **CPAP-only, no oximeter** — pages 5–6 omitted cleanly; page numbering correct; no NaN/empty.
4. **No diagnostic AHI entered** — p1 baseline caption and p7 Δ column absent; with it entered, both appear.
5. **Short 3-day range** — charts compact, pagination intact.
6. **Cross-check Classic** — Classic output visually identical before/after the `gatherReportData()` extraction.
7. **Off-macOS font fallback** — SF→Helvetica/Menlo doesn't break layout (metrics-based, not fixed widths).

## Risks & mitigations

- **`gatherReportData()` regressing Classic** → behavior-preserving move; verify with check #6; remember
  ODI/T88/zones live outside the main block (:2890–2903).
- **Anon-namespace hoist breaking the build / ODR** → move definitions (not just decls) carefully; build after each move.
- **Fabricated sleep/baseline data** → resolved by gating (decisions above); the mockup's sleep numbers were invented.
- **SF unavailable off-macOS** → fallback chain; layout uses font metrics.
- **Large new file** → one responsibility per helper; split page builders.
- **Chart span culling / Retina font scaling** → apply the two fixes recorded in project learnings.
