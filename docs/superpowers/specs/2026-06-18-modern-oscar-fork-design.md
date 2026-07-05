# Modern OSCAR Fork — Design Spec

**Date:** 2026-06-18
**Status:** Approved
**Author:** Adrian Garett Sian

## Goal

A functional fork of OSCAR (OpenSource CPAP Analysis Reporter) with a modern UI,
preserving OSCAR's data fidelity and device coverage. The fork is built in a fresh
codebase. This spec defines what gets built; the deliverable handed to the new
codebase is `MODERN-OSCAR-HANDOFF-PROMPT.md`.

OSCAR is GPLv3. The fork is a derivative work: GPLv3 license and attribution to
Mark Watkins (SleepyHead) and The OSCAR Team must be preserved.

## Tech stack

- **Core (Rust):** device loaders, data model, import engine, stats compute, SQLite
  persistence. Logic ported from `oscar/SleepLib/`.
- **Frontend (React + TypeScript + Tailwind):** Daily, Overview/trends, Statistics,
  Compliance report.
- **Shell:** Tauri 2. IPC commands bridge frontend ↔ Rust core.
- **Charts:** canvas/WebGL-based lib (flow rate ~25 Hz → tens of thousands of points
  per session; SVG/DOM charting will not perform). uPlot or a WebGL renderer.
- **PDF:** compliance report rendered HTML → PDF (Tauri print-to-PDF or headless
  Chromium), reproducing the existing SVG-chart report.

## Architecture

```
┌─────────────────────────────────────────────┐
│ Frontend (React/TS/Tailwind)                  │
│  Daily · Overview · Statistics · Report       │
└───────────────┬───────────────────────────────┘
                │ Tauri IPC (typed commands)
┌───────────────┴───────────────────────────────┐
│ Core (Rust)                                    │
│  import_engine → loaders/* → data model        │
│  stats · report · SQLite store                 │
└────────────────────────────────────────────────┘
```

### IPC commands (initial)
- `import_folder(path) -> ImportSummary`
- `list_days(machine_id, range) -> [DaySummary]`
- `get_day(date) -> DayDetail`  (sessions + channel time-series)
- `get_trends(range, channels) -> TrendSeries`
- `get_statistics(period) -> StatsTable`
- `render_compliance_report(range) -> pdf bytes`

## Data model (mirror OSCAR)

```
Profile → Machine → Day → Session → EventList(ChannelID, time-series)
```

- **ChannelID:** the OSCAR `schema::` codes verbatim, so all downstream math matches.
  CPAP: `CPAP_Pressure, CPAP_EPAP, CPAP_IPAP, CPAP_Leak, CPAP_LeakTotal,
  CPAP_FlowRate, CPAP_FlowLimit, CPAP_Snore, CPAP_RespRate, CPAP_MinuteVent,
  CPAP_TidalVolume, CPAP_Obstructive, CPAP_ClearAirway, CPAP_Apnea, CPAP_Hypopnea,
  CPAP_AllApnea, CPAP_RERA, CPAP_CSR, CPAP_AHI, CPAP_RDI, CPAP_Mode, CPAP_Ramp,
  CPAP_PressureMin, CPAP_PressureMax, …`.
  Oximeter: `OXI_SPO2, OXI_Pulse, OXI_Plethy, OXI_Perf, OXI_PulseChange`.
- **EventList:** OSCAR stores either waveform (evenly-sampled, rate + gain/offset) or
  event (timestamp + value) lists. Preserve both. Gain/offset scaling must match
  OSCAR or values are wrong.
- **Session:** start/end epochs, settings map, per-channel EventLists.
- **Day:** groups sessions of one calendar day (OSCAR's split-at-noon rule for some
  loaders must be preserved — see `bmc_loader` noon bug history).

## Parser scope — all major brands

Each = its own Rust module under `core/src/loaders/`, ported 1:1 from the matching
OSCAR loader in `oscar/SleepLib/loader_plugins/`:

| Brand | OSCAR source | Notes |
|---|---|---|
| ResMed | `resmed_loader.cpp`, `resmed_EDFinfo.cpp`, `edfparser.cpp` | EDF/EDF+ + STR.edf; build first |
| Philips Respironics | `prs1_loader.cpp`, `prs1_parser*.cpp` | System One + DreamStation; binary chunked |
| BMC | `bmc_loader.cpp`, `bmcg3x_loader.cpp`, `bmcDataParsing*.cpp` | preserve noon-split + EVT fixes |
| DeVilbiss IntelliPAP | `intellipap_loader.cpp` | |
| Fisher & Paykel | `icon_loader.cpp`, `sleepstyle_loader.cpp` | |
| Löwenstein/Weinmann | `weinmann_loader.cpp`, `prisma_loader.cpp` | |
| Resvent | `resvent_loader.cpp` | |
| Oximeters | `cms50_loader.cpp`, `cms50f37_loader.cpp`, `md300w1_loader.cpp`, `viatom_loader.cpp` | serial + file |
| Other trackers | `zeo_loader.cpp`, `somnopose_loader.cpp`, `dreem_loader.cpp` | low priority |

Shared `MachineLoader`-style trait: `detect(path) -> bool`, `open(path) -> Result`,
`peek_info(path) -> MachineInfo`. EDF parsing factored into a shared crate (ResMed,
F&P, Löwenstein all use EDF).

## Features (v1)

1. **Daily view** — date picker + vertically stacked, time-synchronized charts (flow
   rate, pressure/EPAP/IPAP, leak, flow limit, snore, event flags, SpO2/pulse) + session
   list + per-day stats panel.
2. **Overview/trends** — date-range trend charts (AHI stacked AI/HI, usage hours,
   pressure percentiles, leak) + calendar usage heatmap.
3. **Statistics page** — period table (week/month/all): min/median/95th/max for AHI,
   pressure, leak, usage, plus usage-compliance %.
4. **Compliance report** — faithful reproduction of `oscar/compliancereport.cpp`:
   - Header: name, DOB, age, date range, baseline-AHI delta.
   - Usage: days used, ≥4h / <4h split, total/avg/median hours.
   - Device block: serial, mode, pressure, EPR, ramp, mask.
   - Therapy: leak (median/95th/max), AI/HI/AHI, apnea breakdown, RERA, RDI, flow
     limit, CSR%.
   - Oxygen summary (when oximetry present): nadir, T90, time-in-zone, worst-night.
   - SVG usage/AHI/leak/SpO2 charts.
   - **SpO2 artifact filtering must be ported exactly** (artifact floor, max
     physiologic fall-rate clamp, nadir computed after artifact exclusion) — see
     `compliancereport.cpp` lines ~281–470. Getting this wrong changes the nadir.

## Build/port order (defines the handoff plan)

1. Project scaffold (Tauri + React + Rust core crate).
2. Data model + EventList + ChannelID enum.
3. Shared EDF parser crate.
4. ResMed loader (first end-to-end vertical slice).
5. Import engine + SQLite persistence.
6. UI shell + Daily view with charts (ResMed data).
7. Stats compute + Statistics page.
8. Overview/trends.
9. Compliance report (HTML→PDF) incl. SpO2 artifact filtering.
10. Remaining loaders (Philips → BMC → others), each behind its detect().

Each step has acceptance: parsed values match OSCAR's for a known sample set.

## Non-goals (v1)

- Cloud sync, multi-user accounts, mobile.
- Editing/annotating raw signals.
- Brands not in OSCAR.

## Reference access

A read-only copy of the OSCAR repo sits beside the new project as `oscar-reference/`.
The new agent reads the real C++ loaders to port them. Format docs in
`oscar-reference/Notes/` supplement.

## Risks

- **Parser fidelity** — binary formats are unforgiving; gain/offset and endianness
  bugs yield silently-wrong data. Mitigate with value-parity tests vs OSCAR output.
- **Chart performance** — 25 Hz waveforms; must use canvas/WebGL.
- **Scope** — all-brand parity is large. ResMed-first vertical slice de-risks the
  architecture before the long tail of loaders.
