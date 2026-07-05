# Handoff Prompt — Build a Modern OSCAR Fork

> **Before pasting:** copy the OSCAR source into the new empty repo so the agent can
> always read it (this is what prevents it from guessing binary formats):
> ```bash
> # run from inside the new empty repo
> cp -R /Users/adriangarett.sian/oscar-code ./oscar-reference
> echo "oscar-reference/" >> .gitignore   # don't commit GPL source into the fork
> ```
> Verify `./oscar-reference/oscar/SleepLib/`, `./oscar-reference/oscar/compliancereport.cpp`,
> and `./oscar-reference/Notes/` exist. Then paste everything below the `---` line
> into a fresh Claude Code session opened in that repo.

---

You are the tech-lead orchestrator for a new project: a modern-UI fork of **OSCAR**
(OpenSource CPAP Analysis Reporter). I want it as detailed and accurate as the
original — same data fidelity, same device coverage — but with a modern desktop UI,
plus a polished compliance report. You are starting in an empty repository.

## Operating instructions (read first)

- This is a large, multi-wave software project. Use the **CDT orchestration**
  workflow (`cdt:orchestration`): triage → architecture contract → dispatch
  specialists in waves → quality gates → completion mandate → ship. Default to
  **xhigh effort + Opus 4.8**.
- **Do not start coding yet.** First run `superpowers:brainstorming` to confirm scope
  with me, then `superpowers:writing-plans` to produce a phased implementation plan.
  Build only after I approve the plan.
- A read-only copy of OSCAR is at `./oscar-reference/`. **It is the source of truth.**
  When porting a parser or the report, read the actual C++ and reproduce its behavior —
  do not guess binary formats. Treat `./oscar-reference/Notes/` as supplementary
  format docs.
- **License:** OSCAR is GPLv3; this fork is a derivative work. Keep GPLv3, copy
  `./oscar-reference/COPYING` to the repo root, and preserve attribution to Mark
  Watkins (SleepyHead) and The OSCAR Team.

## Anti-hallucination rules (non-negotiable)

These exist because the failure mode for this project is inventing a binary format or
a metric formula that looks plausible and is silently wrong.

1. **Read before you write.** Every binary offset, field size, gain/offset, channel
   code, and report formula MUST come from a file you actually read in
   `./oscar-reference/`. Do not rely on memory of "how CPAP formats usually work."
2. **Cite the source.** When you implement a parser or formula, name the exact file
   and line range you ported it from (e.g. `// ported from resmed_loader.cpp:842-901`).
   A claim with no citation is a guess — remove it or verify it.
3. **If you can't find it, STOP — don't fabricate.** If a format detail isn't in the
   reference, do not invent one. Mark it `// NEEDS_VERIFICATION: <what's missing>`,
   leave that path unimplemented, and ask me. A missing parser is fine; a confidently
   wrong one is not.
4. **Verify, don't assert.** "Done" requires the value-parity test (below) actually
   passing against real OSCAR output — not your belief that it matches. Paste the
   test output. Never claim a number matches without running the comparison.
5. **No fabricated data at runtime.** Missing/unreadable values propagate as
   absent/NaN, never a made-up `0`. (Same rule OSCAR itself follows.)

## What to build

### Stack
- **Shell:** Tauri 2.
- **Frontend:** React + TypeScript + Tailwind. Charts via a **canvas/WebGL** lib
  (e.g. uPlot) — flow-rate waveforms are ~25 Hz, tens of thousands of points per
  session; DOM/SVG charting will not perform.
- **Core:** Rust crate. Device loaders, data model, import engine, stats, SQLite
  persistence. Logic ported from `./oscar-reference/oscar/SleepLib/`.
- **PDF:** compliance report rendered HTML → PDF.

### Architecture
Frontend ↔ Rust core over typed **Tauri IPC commands**:
`import_folder`, `list_days`, `get_day`, `get_trends`, `get_statistics`,
`render_compliance_report`.

### Data model — mirror OSCAR exactly
```
Profile → Machine → Day → Session → EventList(ChannelID, time-series)
```
- Reuse OSCAR's `schema::` **ChannelID codes verbatim** (e.g. `CPAP_Pressure`,
  `CPAP_Leak`, `CPAP_LeakTotal`, `CPAP_FlowRate`, `CPAP_FlowLimit`, `CPAP_Obstructive`,
  `CPAP_ClearAirway`, `CPAP_Hypopnea`, `CPAP_AllApnea`, `CPAP_RERA`, `CPAP_CSR`,
  `CPAP_AHI`, `CPAP_RDI`, `CPAP_Mode`, `CPAP_Ramp`, `OXI_SPO2`, `OXI_Pulse`, …).
  Source list: `./oscar-reference/oscar/SleepLib/schema.cpp`. Keeping the codes
  identical keeps all stats/report math identical.
- **EventList** holds either a waveform (evenly sampled: rate + gain + offset) or an
  event list (timestamp + value). Preserve **gain/offset scaling and endianness** —
  these are the silent-corruption traps. See `./oscar-reference/oscar/SleepLib/event.h`
  and `session.h`.
- Preserve OSCAR's day-grouping rules (incl. the noon-split behavior some loaders
  rely on — see BMC loader history).

### Parsers — all major brands
A shared `MachineLoader`-style trait: `detect(path) -> bool`, `open(path)`,
`peek_info(path)`. Factor EDF parsing into a shared module (ResMed, F&P, Löwenstein
use it). Port each from its OSCAR counterpart in
`./oscar-reference/oscar/SleepLib/loader_plugins/`:

| Brand | OSCAR source files |
|---|---|
| ResMed (build first) | `resmed_loader.cpp`, `resmed_EDFinfo.cpp`, `edfparser.cpp` |
| Philips Respironics | `prs1_loader.cpp`, `prs1_parser.cpp`, `prs1_parser_{asv,vent,xpap}.cpp` |
| BMC | `bmc_loader.cpp`, `bmcg3x_loader.cpp`, `bmcDataParsing*.cpp` |
| DeVilbiss IntelliPAP | `intellipap_loader.cpp` |
| Fisher & Paykel | `icon_loader.cpp`, `sleepstyle_loader.cpp` |
| Löwenstein / Weinmann | `weinmann_loader.cpp`, `prisma_loader.cpp` |
| Resvent | `resvent_loader.cpp` |
| Oximeters | `cms50_loader.cpp`, `cms50f37_loader.cpp`, `md300w1_loader.cpp`, `viatom_loader.cpp` |
| Other trackers (low priority) | `zeo_loader.cpp`, `somnopose_loader.cpp`, `dreem_loader.cpp` |

**Robustness rule (from OSCAR lessons):** one malformed record/row must never abort
the whole parse; missing values propagate as absent/NaN, never a fabricated 0;
implausible binary scaling degrades to a visible floor, not sign-flipped garbage.

### Features (v1)
1. **Daily view** — date picker + vertically stacked, **time-synchronized** charts
   (flow rate, pressure/EPAP/IPAP, leak, flow limit, snore, event flags, SpO2/pulse)
   + session list + per-day stats panel.
2. **Overview/trends** — date-range trends (AHI stacked AI/HI, usage hours, pressure
   percentiles, leak) + calendar usage heatmap.
3. **Statistics** — period table (week / month / all): min/median/95th/max for AHI,
   pressure, leak, usage + compliance %.
4. **Compliance report** — faithfully reproduce
   `./oscar-reference/oscar/compliancereport.cpp`:
   - Header: name, DOB, age, date range, baseline-AHI → current delta.
   - Usage: days used, ≥4h / <4h split, total/avg/median hours.
   - Device: serial, mode, set pressure, EPR, ramp, mask.
   - Therapy: leak (median/95th/max), AI/HI/AHI, apnea breakdown, RERA, RDI, flow
     limit, CSR%.
   - Oxygen summary (when oximetry present): nadir, T90, time-in-zone, worst night.
   - SVG usage / AHI / leak / SpO2 charts.
   - **Port the SpO2 artifact filtering exactly** (artifact floor, max physiologic
     fall-rate clamp ~4 %/s, nadir computed *after* artifact exclusion) —
     `compliancereport.cpp` ≈ lines 281–470. This determines the reported nadir;
     a naive min() is wrong.

   A sample of my target output is in `./oscar-reference/Compliance_Report_*.pdf` —
   match its look and field set.

## Build / port order (turn this into the plan)
1. Scaffold: Tauri 2 + React/TS/Tailwind + Rust core crate; CI; GPLv3 + attribution.
2. Data model + EventList + ChannelID enum.
3. Shared EDF parser module.
4. **ResMed loader — first end-to-end vertical slice.**
5. Import engine + SQLite persistence.
6. UI shell + Daily view with charts (real ResMed data).
7. Stats compute + Statistics page.
8. Overview/trends.
9. Compliance report (HTML→PDF) incl. SpO2 artifact filtering.
10. Remaining loaders, gated behind their `detect()`: Philips → BMC → F&P →
    Löwenstein → DeVilbiss → Resvent → oximeters → other trackers.

## Acceptance per step
For every parser and the report, **parsed values must match OSCAR's output** for a
known sample data set. Write value-parity tests (load same data, assert channel
values, stats, and report numbers equal OSCAR's). No step is "done" until its numbers
match and tests pass — verify before claiming completion (`superpowers:verification-before-completion`).

## Non-goals (v1)
Cloud sync, accounts, mobile, raw-signal editing, brands OSCAR doesn't support.

## First actions
1. Confirm you can read `./oscar-reference/oscar/SleepLib/` and the report source.
   **If you cannot, STOP and tell me — do not proceed from memory.**
2. Run `superpowers:brainstorming` to confirm scope/edge cases with me.
3. Run `superpowers:writing-plans` to produce the phased plan from the order above.
4. Wait for my approval, then build wave by wave with quality gates.
