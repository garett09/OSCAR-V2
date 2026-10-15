# OH/CH capability gating and NULL summary columns — design

**Date:** 2026-09-14
**Status:** implemented 2026-09-15 (branch `oh-ch-gating`); see "As built" below
**Tracking:** GitLab #261

## As built — deviations from the plan

- **§5.5 (c), `daily_summaries` in the migration.** Not deleted wholesale. The CSV Export
  Wizard reads only `daily_summaries` and can export a profile that has not been opened since
  the upgrade, so deleting every row would have blanked those exports. Instead the migration
  NULLs the count/index/statistic columns at *profile* level ("no device of this profile has
  reported the channel"), which equals the machine-level answer whenever the profile's CPAP
  devices agree, and deletes rows only for profiles whose devices disagree on some channel
  (one scores RERA and another does not, say) — those are regenerated on next open. Both
  lookups run off indexed temp tables; the whole v19 migration took ~11 s on a database of
  57,000 sessions and a million `session_channels` rows.
- **§5.6, aggregated SQL.** Beyond `COALESCE`ing the additive sums, the aggregated `RDI`,
  `OAHI` and `CAHI` columns in `system_reports.orf` are `NULL` when the group has no RERA /
  no OH-CH at all, so a report never restates AHI where the tables would store `NULL`.
- **§1.3, flip fix-up.** Daily rows are recomputed by iterating the machine's own `day` map
  through `DailySummaryRepository::calculateAndStoreFromDay()` (an INSERT OR REPLACE), so no
  `invalidateRange()` call is needed.
- The QTest for the capability set lives in `oscar/tests/machinetests.{h,cpp}` (new file, in
  the `test` build only).
**Builds on:** `Notes/specs/2026-08-06-central-obstructive-hypopnea-design.md` (schema v18,
OH/CH channels, OAHI/CAHI), issue #285 (`session_summaries` indices = events / mask-on hours)

## Goal

OAHI and CAHI are only meaningful when the device splits hypopneas by mechanism. Today they
are computed and shown for every device, so a ResMed user sees OAHI == AHI and CAHI == the
clear-airway rate, which implies a classification the device never made. Likewise the
`oahi` / `cahi` / `*_hypopnea_count` columns store 0 for those devices, which SQL cannot
tell apart from "scored none".

After this change:

- OAHI/CAHI appear on the Daily page, printed report and Statistics page only for machines
  that have demonstrably scored an obstructive or central hypopnea.
- Summary columns that do not apply to a machine, or have no data for a row, are `NULL`.
- The Daily events list, flags graph, Statistics per-channel rows and the OAHI/CAHI gate all
  agree: OH/CH exist for a machine iff it has ever scored one.

The event-flags graph itself is unchanged.

---

## 1. Capability model

**Rule:** a machine "reports the hypopnea mechanism" once any of its sessions has a
count > 0 on `CPAP_ObstructiveHypopnea` or `CPAP_CentralHypopnea`. Evidence-based; never
declared by a loader.

Why not a loader-declared flag: `MachineInfo::cap` exists (persisted as
`machines.properties.capabilities`) but no loader sets it and nothing reads it back, and we
do not know that every model handled by the three OH/CH loaders splits hypopneas. A flag set
per loader would assert capability for models that may never emit OH/CH.

Why not `Machine::hasChannel()`: `Session::UpdateSummaries()` adds every `eventlist` key to
`m_availableChannels`, empty lists included, and `StoreToDatabase()` writes a
`session_channels` row for each (count 0). The three OH/CH loaders create the OH/CH lists
unconditionally, so `hasChannel(OH)` is true for every BMC, SEFAM and Prisma machine —
including legacy BMC, which never emits either. See §2.

### 1.1 `Machine` additions

| Member | Meaning |
|---|---|
| `QSet<ChannelID> m_reportedChannels` | channels with count > 0 on at least one session, ever |
| `bool hasReportedEvents(ChannelID) const` | membership test |
| `bool reportsHypopneaMechanism() const` | `hasReportedEvents(OH) \|\| hasReportedEvents(CH)` |
| `void noteReportedChannels(Session *)` | adds the session's non-zero channels |

Generic on purpose: the `NULL` rule in §5 needs the same question for RERA, AllApnea, etc.
Derived, not persisted — SQL consumers can ask the same question of `session_channels`.

### 1.2 Where it is computed — main thread only

1. `Machine::LoadSessionsFromDatabase()` — per session after `LoadFromDatabase()` has filled
   `m_cnt` from `session_channels`. Runs before `Profile::LoadMachineData()` reaches the
   daily-summaries reconcile (verified: `mach->Load()` for every machine precedes it).
2. `Machine::Save()` — a pre-pass over sessions with `IsChanged()`, summing each channel's
   `EventList::count()` directly (not via `Session::count()`, which memoises into `m_cnt`
   before `UpdateSummaries()` runs). This runs **before** `SaveTask`s are queued, so every
   summary row of the batch sees the same, settled answer.

Not from `Machine::updateChannels()`: that is called from `UpdateSummaries()` inside
`SaveTask::run()` on `QThreadPool` workers, and from `BmcLoaderTask::run()`. The existing
`m_availableChannels` writes there are already unsynchronised; this design does not add to
them.

### 1.3 Late-arriving capability ("flip")

A capable device's first import can contain no OH/CH at all — on the development test
database one G3X-family machine has 0 OH and 9 CH across 62 sessions. Rows written before
the first OH/CH would be `NULL` and stay wrong.

In `Machine::Save()`, snapshot `m_reportedChannels` before the pre-pass. If the pre-pass added
channels **and** the machine already has rows (`m_database_id > 0` and any session with
`sessionRowId() != 0`):

1. run the "rebuild summary columns from `session_channels`" routine (§5.4) scoped to this
   machine — the same routine the v19 migration uses for all machines;
2. `DailySummaryRepository::invalidateRange(profile, FirstDay(), LastDay())` for the machine,
   so the post-import daily recompute (`finishAddingSessions()` → all days; `ImportContext::
   Commit()` and the #286 fallback → imported days only) does not leave stale rows.
   Because `Commit()` covers only imported days, the flip handler must itself recompute the
   invalidated range; do not rely on the caller.

Generic: the same path corrects `rera_count` when a ResMed scores its first RERA months in.

---

## 2. Loaders — OH/CH lists created on first event

Scope: the OH/CH `EventList`s only. No other channel, and no other loader, changes.

| Loader | Change |
|---|---|
| `bmc_loader.cpp` `setSessionRespiratoryEvents()` | `oscarOhList` / `oscarChList` start `nullptr`, created on the first `OH` / `CH` event. Legacy cards (which share this writer) never get them; G3X gets them only in sessions that contain one. |
| `sefam_loader.cpp` (log path and memory-image path) | same lazy creation for `oh` / `ch` |
| `prisma_loader.cpp` `AddEvents()` | skip the unconditional trailing `AddEventList(channel, EVL_Event)` for the two OH/CH channels. `AddEvents()` already creates the list on the first event; the trailing call adds a second, empty list even when a full one exists. Leave it in place for every other channel — it is in the loader's original 2021 commit and determines which channels appear in the events list for Prisma. |

Effect: a never-scoring machine no longer gets zero-count OH/CH `session_channels` rows, so
it no longer shows `OH 0.00 / CH 0.00` in the Daily events list, has no OH/CH flags rows,
and gets no `ObstructiveHypopnea` / `CentralHypopnea` rows on the Statistics page. A capable
machine loses nothing: once one event has been seen, `Machine::availableChannels()` (the
union over all sessions) keeps both channels visible at 0.00 on zero nights.

Zero-count rows already in tester databases are removed by the migration (§5.5 b).

---

## 3. Daily page and printed report

- `Daily::getAHI()` — emit the OAHI/CAHI line only when
  `day->machine(MT_CPAP)->reportsHypopneaMechanism()`. A capable machine's zero night still
  shows `OAHI 0.00  CAHI 0.00` (real zeros).
- `reports.cpp` — the same gate for `OAHI= / CAHI=` (currently always printed) **and** for
  `OHI= / CHI=` (currently gated on the day having OH/CH data). Switching the latter to the
  machine gate makes the printed report and the sidebar agree on zero nights.
- `getIndices()`, the pie chart, flags graph and events combo need no change: they read the
  machine union, which §2 makes correct.

One CPAP machine per `Day`, so no mixed-machine case here.

---

## 4. Statistics page

Multi-machine profiles are handled per period (cell), reusing the existing `"-"` convention
that `StatisticsRow::value()` already returns for a period with no days.

- **Rows:** emit the `SC_OAHI` / `SC_CAHI` rows iff at least one CPAP machine with days in
  `first..last` reports the mechanism. Profiles with no capable machine never see them.
- **Cells:** for each period, walk its CPAP days (`GetGoodDay(date, MT_CPAP)` →
  `day->machine(MT_CPAP)`). Every day capable → compute as now; any day from a non-capable
  machine → `"-"`, with a tooltip "Not available for all devices in this period".

After a switch from a non-splitting to a splitting device, "Most Recent", "Last Week" and
"Last 30 Days" fill in as soon as they are pure; "Last 6 Months" / "Last Year" /
"Everything" show `"-"` until they are.

Rejected alternative — compute over the capable-machine days only: gives an honest number,
but for that column OAHI + CAHI would no longer equal the AHI two rows above (different day
set, different hours), exactly the quiet discrepancy the split was designed to avoid.

Not changed: the per-channel rate rows (`ObstructiveHypopnea`, `CentralHypopnea`, and equally
`RERA`, `FlowLimit`, `CSR`, …) divide one channel's count by *all* CPAP hours in the period,
so on a mixed profile they understate any channel only one machine reports. Pre-existing and
not OH/CH-specific; recorded here, left alone.

---

## 5. Database

### 5.1 Struct changes

`SessionSummaryData` and `DailySummaryData`: every metric that can be absent becomes
`std::optional<double>` / `std::optional<int>`. Keys, `session_count`, `hours`, `ahi`,
`is_compliant`, `has_oximetry`, timestamps stay as they are.

One helper per repository file: `static QVariant bindOptional(const std::optional<T>&)`
(`QVariant()` for `nullopt`), and the mirror on read
(`value.isNull() ? std::nullopt : value.toDouble()`).

In-app readers of these structs are `Session::LoadFromDatabase()` → `restoreCount()` (eight
counts; use `value_or(0)`, which reproduces today's behaviour) and two debug prints. Nothing
in the application reads the pressure, leak, oximetry, `rdi`, `oahi` or `cahi` columns back.
`Profile::LoadMachineData()` reads only `DailySummaryData::date`.

### 5.2 The `NULL` rule

| Column(s) | `NULL` when |
|---|---|
| `obstructive_count`, `clear_airway_count`, `hypopnea_count`, `unclassified_count`, `all_apnea_count`, `rera_count`, `obstructive_hypopnea_count`, `central_hypopnea_count` | the row's CPAP machine has never reported that channel (`!hasReportedEvents(id)`); otherwise the count, 0 included |
| `oahi`, `cahi` | `!reportsHypopneaMechanism()` |
| `rdi` | `!hasReportedEvents(CPAP_RERA)` — without RERA the RDI is just the AHI restated |
| `ahi` | never (0 when no events; unchanged) |
| `pressure_*`, `leak_*`, `spo2_*`, `pulse_*` | the row has no data for that channel — the writers' existing `m_wavg.contains()` / `day->channelHasData()` tests, which today fall through to 0 |
| `pressure_95th`, `leak_total_95th` (session) | also when events were not loaded (today "leave at 0") |

Daily rows use `day->machine(MT_CPAP)`; a day with no CPAP machine gets `NULL` in every CPAP
column. `0` therefore always means "measured, none" and `NULL` "not applicable / not
measured".

### 5.3 Writers

- `Session::StoreSummaryToDatabase()` — apply §5.2 using `s_machine`. The pre-pass in §1.2
  guarantees the machine's answer is settled before any `SaveTask` reaches this point.
- `DailySummaryRepository::calculateAndStoreFromDay()` — apply §5.2 using
  `day->machine(MT_CPAP)`.
- Both repositories' `create` / `createOrUpdate` / `update` bind through `bindOptional()`.

No schema change: every affected column is already nullable (`DEFAULT 0`, no `NOT NULL`), and
both writers bind every column explicitly, so the default never applies.

### 5.4 Shared routine: rebuild `session_summaries` metrics from `session_channels`

One function, parameterised by machine (or all machines), used by the v19 migration and the
flip handler. For each session of the machine:

- each count column ← `session_channels.count` for its channel, `0` if the machine has
  reported the channel but this session has no row, `NULL` if the machine never has;
- `ahi` ← `SUM(cph)` over `ahiChannels` (the #285 recompute, moved here from v18);
- `rdi`, `oahi`, `cahi` ← the same sums, `NULL`ed by the §5.2 rule;
- `pressure_*`, `leak_*`, `spo2_*`, `pulse_*` ← `NULL` where the session has no
  `session_channels` row for the source channel, else unchanged.

"Machine never reported channel X" in SQL is `NOT EXISTS (… session_channels sc JOIN
sessions s … WHERE s.machine_id = ? AND sc.channel_id = X AND sc.count > 0)`. Channel ids
are those listed in the v18 migration comment plus `CPAP_Pressure` 0x110C, `CPAP_Leak`
0x1108, `CPAP_LeakTotal` 0x1117, `OXI_Pulse` 0x1800, `OXI_SPO2` 0x1801 (`schema.cpp`).

`cph`, not `count`, is the basis for the indices for the reason recorded in the v18
migration: the INTEGER count columns truncated ResMed summary-only sessions' fractional
counts.

### 5.5 Schema v19

`CURRENT_SCHEMA_VERSION` → 19. Backup/restore compatibility range follows automatically.

- `migrateV17ToV18` — reduced to the five `ADD COLUMN`s per table. Its two backfill
  statements move to v19.
- `migrateV18ToV19`:
  a. §5.4 for every machine — absorbs the #285 recompute (so v18 tester databases, which
     never ran it, are corrected on next open) and applies the `NULL` rule to historical rows.
  b. `DELETE FROM session_channels WHERE channel_id IN (OH, CH) AND count = 0` for
     machines that never reported either — what §2 would have produced. Rows with
     count > 0 are untouched.
  c. `DELETE FROM daily_summaries` — all rows. `daily_summaries` is a cache; with zero rows,
     `Profile::LoadMachineData()` takes its `existingCount == 0` branch and runs
     `calculateDailySummaries()` for the whole profile on first open, after every machine
     has loaded (§1.2). A `daily_summaries` row cannot be rewritten in SQL because the table
     carries no machine id and OSCAR days run noon to noon.

Cost: one full daily recompute per profile on first open after upgrade — the same work every
import already does via `finishAddingSessions()`.

### 5.6 SQL consumers — required audit

`NULL` propagates through `+`; `SUM()` / `AVG()` over an all-`NULL` group return `NULL`.

- `oscar/docs/system_reports.orf` lines 82 and 112 compute RDI as
  `SUM(obstructive_count) + SUM(unclassified_count) + SUM(all_apnea_count) + …`. Once
  `all_apnea_count` is `NULL` on every ResMed day, the whole expression is `NULL` and every
  ResMed profile's system report loses its RDI. Wrap each term in `COALESCE(SUM(x), 0)`;
  audit the rest of the file the same way. `AVG(pressure_95th)` and
  `NULLIF(spo2_avg, 0)` become correct rather than broken.
- `Notes/Database/USEFUL_QUERIES.sql` — same audit.
- Identity checks `oahi + cahi = ahi` need `WHERE oahi IS NOT NULL`.
- `Notes/Database/DATABASE_SCHEMA_REFERENCE.md`, `DATABASE_SCHEMA.md`: v19 row, and a
  "NULL vs 0" paragraph on both summary tables.
- Personal reporting tools and the MCP server read these tables directly and will see
  `NULL` / `None` where they saw 0. Outside the repo; owner's responsibility.

---

## 6. Verification (code review + QtCreator build)

| Case | Expect |
|---|---|
| ResMed-only profile | no OAHI/CAHI line on Daily, none in the printed report, no Statistics rows; `session_summaries.oahi`, `cahi`, `all_apnea_count`, `*_hypopnea_count` all `NULL`; `rera_count` 0/values for models that score RERA, `NULL` otherwise; system-report RDI still populated |
| G3X, SEFAM, Prisma profile | OAHI/CAHI shown, real zeros on zero nights; counts 0 not `NULL` |
| Legacy BMC profile, after Rebuild CPAP Data | nothing OH/CH anywhere: sidebar, events list, flags, Statistics rows, `session_channels` |
| Mixed profile (e.g. ResMed then Prisma) | rows present; recent columns numeric, long-range columns `"-"` with tooltip |
| v18 tester database | opens at v19; `session_summaries` recomputed and `NULL`ed; daily rows regenerated once; row counts before/after equal for `sessions` and `session_summaries` |
| Flip | import one G3X night without CH, confirm `NULL`s; import a night with CH; earlier rows now 0/values and the Daily line appears for the earlier night |
| `sessiontests.cpp` | add a case for `noteReportedChannels()` / `reportsHypopneaMechanism()` |

---

## 7. Reversibility

Every `NULL` decision passes through `bindOptional()` and the §5.2 rule in the two writers.
To revert to 0-when-absent: bind `value_or(0)` in the helper (one line per repository) and
run `UPDATE … SET col = COALESCE(col, 0)` per column in a v20 migration. The capability
gating (§1–§4) is independent of the `NULL` convention and would stay.

---

## 8. Decisions (answered 2026-09-14)

1. **Capability = empirical, with the flip fix-up.** Not loader-declared, not `hasChannel()`.
2. **Statistics: strict per-cell rule** (`"-"` unless every CPAP day in the period is from a
   capable machine); rows shown iff any capable machine has days in the report range.
3. **Loader fix in all three loaders, OH/CH lists only.** Empty lists for other channels are
   left as each loader has them.
4. **`NULL` convention for all summary columns**, accepted for testing with the reversal path
   in §7 recorded.
5. **Schema v19**, with the #285 backfill relocated from v18 to v19.

## Noted, not addressed

- Prisma `PRISMA_EVENT_HYPOPNEA_LEAKAGE` (113) is declared but mapped to no channel; Prisma
  never emits plain `CPAP_Hypopnea`. Unrelated to this change.
- Mixed-profile understatement on per-channel rate rows (§4, last paragraph).
- `Machine::updateChannels()` writes `m_availableChannels` from worker threads (§1.2).
