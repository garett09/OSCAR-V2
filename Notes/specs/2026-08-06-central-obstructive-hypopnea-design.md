# Separate Central and Obstructive Hypopnea channels (CH / OH)

Status: **draft for developer review** — 2026-08-06

## Goal

Report Central Hypopnea (CH) and Obstructive Hypopnea (OH) as their own channels when a
device scores them separately, keeping the existing single Hypopnea (H) channel for devices
that do not. Add Obstructive AHI (OAHI) and Central AHI (CAHI) indices.

---

## 1. Devices that already distinguish OH from CH

Three loaders currently decode separate obstructive/central hypopnea codes and fold both
into `CPAP_Hypopnea`. These are the only loaders that change:

| Loader | Where the codes are already decoded |
|---|---|
| SEFAM | `sefam_loader.cpp:715-718` (`kLogObstructiveHypop`, `kLogCentralHypopnea`) |
| BMC Luna G3X | `bmcG3xDataParsing.cpp:165-166, 1128-1129` (`kG3xEvtTypeOH` 0x07, `kG3xEvtTypeCH` 0x08) |
| Löwenstein / Weinmann prisma | `prisma_loader.cpp:414` (`PRISMA_EVENT_OBSTRUCTIVE_HYPOPNEA`, `PRISMA_EVENT_CENTRAL_HYPOPNEA`) |

Unchanged (report a single undifferentiated hypopnea): ResMed, PRS1, F&P Icon and SleepStyle,
Intellipap, Yuwell, Resvent, vTitan vREM, BMC legacy, Weinmann SOMNO.

BMC G3X also carries an *unclassified* hypopnea code (`kG3xEvtTypeUH` 0x01) that must keep
mapping to plain `CPAP_Hypopnea` — so one device can legitimately report all three channels.

---

## 2. Core channel work

| # | File | Change |
|---|---|---|
| 2.1 | `SleepLib/machine_common.h:168`, `machine_common.cpp:19` | Declare/define `CPAP_ObstructiveHypopnea`, `CPAP_CentralHypopnea` |
| 2.2 | `SleepLib/schema.cpp:172` | Add both channel definitions (FLAG, MT_CPAP, SESSION). Free IDs in the flag block: `0x1011`, `0x1012` (also free: `0x100e`, `0x100f`) |
| 2.3 | `SleepLib/schema.cpp:72` `setOrders()` | Insert both next to `CPAP_Hypopnea` so they sort together in the flags graph and index list |
| 2.4 | `SleepLib/schema.cpp:415-425` | Append both to `ahiChannels`; update the "when adding more AHI-contributing channels" checklist comment |
| 2.5 | `SleepLib/common.h/.cpp:732,761` | New short strings `STR_TR_OH`, `STR_TR_CH`, and index strings `STR_TR_OHI`, `STR_TR_CHI`, `STR_TR_OAHI`, `STR_TR_CAHI` |
| 2.6 | `common_gui.h/.cpp:13` | Optional: `COLOR_ObstructiveHypopnea` / `COLOR_CentralHypopnea` if the defaults set in schema.cpp are not enough |

### What this gets for free

Anything that iterates `ahiChannels` / `AllAhiChannels` or walks the schema by channel type
needs **no change**:

- `Day::count/sum` (`day.cpp:645,1134`), `Session::count/rangeCount` (`session.cpp:1782,2063`)
- AHI/RDI calculation and the running-AHI graph (`calcs.cpp:90,990,1006,1048,1109,1146`)
- `Day::calcAHI/calcRDI/calcSHEI/calcTTIA` (`day.h:250-291`)
- Daily event-flags graph (`gFlagsLine.cpp:80-117`) and flow-rate overlay bars (`gLineChart.cpp:220-249`)
- Daily left-sidebar index table (`daily.cpp:1789` — generic over sorted flag channels)
- Overview AHI breakdown (`gAHIChart.h:25`), TTIA chart, daily-summary pie (`gdailysummary.cpp:25`)
- Overview per-channel graphs (`overview.cpp:310` — schema iteration + `showInOverview`)
- CSV export count list (`exportcsv.cpp:164`)
- Daily search tab (`dailySearchTab.cpp:470`)
- Statistics AHI/RDI aggregates (`statistics.cpp:1039`), `welcome.cpp:232`, `reports.cpp:215`
- `session_channels` / `respiratory_events` DB tables (keyed by `channel_id`)
- Preferences → channel colour/label lists
- Backup export (`SELECT *` + explicit-column INSERT, so new columns are additive)

---

## 3. Hard-coded lists that must be extended

| # | File:line | Change |
|---|---|---|
| 3.1 | `daily.cpp:371-379` | Event Breakdown pie: `AddSlice()` entries for OH and CH |
| 3.2 | `daily.cpp:2129-2131` | The `values[]` sum that decides pie-chart vs "0.0!" image must include OH and CH |
| 3.3 | `reports.cpp:226` | `hi` currently `ExP + Hypopnea`; add OH/CH indices and the new OAHI/CAHI to the printed stats line at `reports.cpp:283` |
| 3.4 | `statistics.cpp:784-787` | Add `ObstructiveHypopnea` / `CentralHypopnea` SC_CPH rows; add OAHI and CAHI rows |
| 3.5 | `session.cpp:3499-3506` | Add both to `primaryRespiratoryChannels` (sets `respiratory_events.event_type = 1`) |
| 3.6 | `tests/sessiontests.cpp:150` | Add `CHANNELNAME()` entries |
| 3.7 | `docs/graphs.xml` | Design-reference only (not read at runtime) — update for accuracy |

---

## 4. Database (schema v17 → v18)

Post-release, so this needs a migration with no data loss.

**Three** new count columns per table, not two — see §4.10 for why `all_apnea_count` is in
scope.

| # | File | Change |
|---|---|---|
| 4.1 | `database_schema.h:74` | `CURRENT_SCHEMA_VERSION` 17 → 18. Leave `MIN_RESTORE_SCHEMA_VERSION` at 12 |
| 4.2 | `database_schema.cpp:884` | `session_summaries`: add `obstructive_hypopnea_count`, `central_hypopnea_count`, `all_apnea_count` (+ `oahi`, `cahi` if stored — see open question 2) |
| 4.3 | `database_schema.cpp:1060` | `daily_summaries`: same columns |
| 4.4 | `database_schema.cpp` (new `migrateV17ToV18`) + dispatch at `:218-250` | `ALTER TABLE ... ADD COLUMN` × 3 for each table; `INTEGER DEFAULT 0` |
| 4.5 | `session_summaries_repository.h:33-37` / `.cpp` | Struct fields + INSERT/UPDATE/SELECT column lists |
| 4.6 | `daily_summary_repository.h:35-39` / `.cpp:42,58-62,291-295,469-473` | Struct fields, SQL, and `calculateForDay()` population (`day->count(CPAP_AllApnea)` is currently not recorded at all) |
| 4.7 | `session.cpp:3374-3404` | `StoreSummaryToDatabase()`: persist all three counts, **and** correct the summary-only AHI fallback (§4.10) |
| 4.8 | `session.cpp:3272-3313` | `LoadFromDatabase()` summary-only restore: rebuild `m_cnt` for all three channels |
| 4.9 | `Notes/Database/DATABASE_SCHEMA_REFERENCE.md`, `.mw`, `DATABASE_SCHEMA.md`, `DATA_DICTIONARY.md`, `USEFUL_QUERIES.sql` | Document v18 |

Existing rows migrate with 0 for the new columns; historical data keeps everything in `H`.
Note that `all_apnea_count` will read 0 for every pre-v18 day even where the device did
report `A` — the value was never stored, so it cannot be backfilled from the summary tables.
It is recoverable only by recalculating the day from `session_channels`, which
`DailySummaryRepository::calculateForDay()` already does on demand; whether to force a
one-off recalculation pass at migration time is part of open question 3.

### 4.10 Why `all_apnea_count` is included

The summary tables have no `all_apnea_count` column, so `CPAP_AllApnea` is stored nowhere
even though `Day::calcAHI()` counts it (`daily_summary_repository.cpp:291-295`,
`session.cpp:3389-3404`). Two pre-existing consequences, both of which would otherwise break
the OAHI + CAHI == AHI identity outside the app:

- The hand-rolled AHI in `system_reports.orf` (`obstructive + unclassified + hypopnea +
  clear_airway`) disagrees with the stored `ahi` column for any device that reports `A`.
- `Session::StoreSummaryToDatabase()`'s summary-only AHI fallback (`session.cpp:3373-3383`)
  omits `CPAP_AllApnea` **and** adds `CPAP_RERA`, so it computes RDI, not AHI.

**Decided:** `all_apnea_count` rides along in the v18 migration, and the summary-only fallback
is corrected in the same change. `A` belongs to the OAHI bucket (§5), so without it OAHI could
not be computed in SQL.

---

## 5. OAHI / CAHI

**Decided:** OAHI includes unclassified apnea, and **OAHI + CAHI must equal AHI**. Every
AHI-contributing channel therefore falls into exactly one of the two buckets:

| Index | Channels |
|---|---|
| **CAHI** | Clear Airway (CA), Central Hypopnea (CH) |
| **OAHI** | Obstructive Apnea (OA), Obstructive Hypopnea (OH), Unclassified Apnea (UA), Apnea (A), Hypopnea (H) |

Consequence worth stating plainly for reviewers: the identity forces the *unclassified*
channels — UA, A and plain H — into OAHI, so a device that scores nothing centrally reports
OAHI == AHI and CAHI == 0. That matches the usual clinical assumption that unclassified
events are predominantly obstructive, but it does mean OAHI is "AHI minus the central part"
rather than a strictly obstructive count.

**Implementation note.** Rather than hard-coding either list, define a `cahiChannels` vector
in `schema.cpp` next to `ahiChannels`, and derive OAHI as `ahiChannels − cahiChannels`. The
identity then stays true automatically if another AHI-contributing channel is added later,
and the existing "when adding more AHI-contributing channels" checklist only needs one extra
step ("decide which bucket it belongs to").

Work required:

| # | File | Change |
|---|---|---|
| 5.1 | `SleepLib/schema.cpp:415-425`, `machine_common.h/.cpp` | Add `cahiChannels` vector (CA + CH) and, for symmetry with `AllAhiChannels`, sentinel IDs `AllCahiChannels` / `AllOahiChannels` handled in `Day::count`, `Day::sum`, `Session::count`, `Session::rangeCount` |
| 5.2 | `SleepLib/day.h:249-291` | `calcOAHI()` / `calcCAHI()` alongside `calcAHI()`/`calcRDI()` |
| 5.3 | `statistics.cpp` | New `StatCalcType` values and handling in the row-value switch (`:2165`) |
| 5.4 | `daily.cpp` `getAHI()`/`getIndices()` | Show OAHI/CAHI in the left sidebar |
| 5.5 | `reports.cpp:283` | Add to the printed report stats line |
| 5.6 | Database (§4) | Store per-session/per-day, or compute in SQL from the counts — with `all_apnea_count` added in v18 the SQL route is now complete: `CAHI = (clear_airway + central_hypopnea) / hours`, `OAHI = (obstructive + obstructive_hypopnea + unclassified + all_apnea + hypopnea) / hours` |

**Verification:** add a check (test or debug assertion) that `OAHI + CAHI == AHI` for every
day, to catch a future AHI channel being added to neither bucket. With v18 the same identity
is checkable in SQL against the stored `ahi` column, which also regression-guards the
`system_reports.orf` sums (§4.10).

---

## 6. Report and export surfaces

| # | File | Change |
|---|---|---|
| 6.1 | `docs/system_reports.orf` | 8 queries reference `hypopnea_count` (lines 36, 70-74, 95-99, 136, 168-171, 193-196, 216-219, 392-400). Add OH/CH/A columns, add `all_apnea_count` to every hand-rolled AHI/RDI sum (a pre-existing omission, §4.10), and add OAHI/CAHI index columns |
| 6.2 | `exports/exportcsv.cpp` | No change (uses `ahiChannels`) — verify header/column count in all three report modes |
| 6.3 | `Notes/Database/oscar_details_csv.py`, `Notes/Accessing OSCAR Data/*.md` | Update the documented channel set |

---

## 7. Documentation and translation

- `help/help_en/glossary.html` — add OH/CH/OAHI/CAHI definitions (28 existing hypopnea references).
- `Htmldocs/release_notes.html` — user writes this.
- `Translations/*.ts` — new `tr()` strings need a regenerate; ~30 language files affected.

---

## 8. Open questions for review

1. **Mutual exclusivity.** The plan assumes a loader emits *either* `H` *or* `OH`+`CH` for a
   given event, never both, otherwise AHI double-counts. BMC G3X is the awkward case: it has
   a genuinely unclassified hypopnea code alongside OH and CH, so it will populate all three.
   Confirm that is acceptable and that `H` keeps its "unclassified hypopnea" meaning.

2. **Store or compute OAHI/CAHI?** Storing them in `session_summaries` / `daily_summaries`
   costs two more columns each but keeps the `.orf` report SQL simple and consistent with
   the existing `ahi`/`rdi` columns. Recommend storing.

3. **Historical data — two separate gaps.**
   1. *OH/CH split.* Days already imported from SEFAM / G3X / prisma keep OH and CH folded
      into `H`; only re-imported data will split. Because plain `H` sits in the OAHI bucket,
      pre-migration days report all their hypopneas as obstructive — CAHI will understate the
      central share on old data and jump when the same card is re-imported. Do we advise a
      purge-and-reimport for those three loaders in the release notes, or leave existing data
      as-is? (Recoverable only by re-reading the SD card.)
   2. *`all_apnea_count` backfill.* Affects **every** loader that reports `A`, not just the
      three above. The count was never stored, so the summary tables cannot backfill
      themselves — but `session_channels` carries a per-channel `count` row for every channel
      a session recorded (`session.cpp:3184-3203`), so
      `DailySummaryRepository::calculateForDay()` can regenerate it without the SD card.
      Summary-only sessions are the exception: their counts come back through the §4.8 restore
      path, which must therefore handle `A` as well. Options: recalculate all days once during the v18 migration (slow on large profiles,
      but correct immediately); recalculate lazily as days are opened; or leave stale and
      accept that `.orf` reports understate AHI on old days until a day is touched. This is
      the more consequential of the two, since it changes an existing published number.

4. **1.7.1 backport?** 1.7.1 has no database layer, so §4 does not apply, but §2, §3 and §5
   would. Is a backport wanted?

5. **Channel labels.** Proposed short labels `OH` and `CH`; note `CH` is visually close to
   `CA`. Confirm, or pick alternatives (`HO`/`HC`, `OHY`/`CHY`).

6. **Default overview visibility.** Should OH/CH default to `showInOverview()`? `H` currently
   does not.
