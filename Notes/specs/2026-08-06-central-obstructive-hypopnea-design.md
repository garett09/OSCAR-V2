# Separate Central and Obstructive Hypopnea channels (CH / OH)

Status: **implemented 2026-08-14.** Decisions are recorded in §8 and were folded into
§2–§7 before implementation. The file:line references below are to the pre-change tree and
are kept as a record of the plan; the notes under "As built" record where the finished code
differs from it.

## As built — deviations from the plan

- **Channel IDs** `0x1011` (OH) and `0x1012` (CH), as proposed in §2.2.
- **Colours** are distinct, not inherited from `H` (§2.6, left open): OH `#3090d0`, a
  steel blue near `H`'s blue; CH `#d060d0`, an orchid clearly separable from `CA`'s purple.
  No `COLOR_*` constants were added — the defaults in `schema.cpp` were enough.
- **Bucket dispatch is generic.** Instead of adding a branch per sentinel to the four
  count/sum entry points, `machine_common.h` gained `ahiChannelGroup(ChannelID)`, which maps
  `AllAhiChannels` / `AllOahiChannels` / `AllCahiChannels` to their vectors and returns
  `nullptr` for an ordinary channel. `oahiChannels` is computed in `schema::init()` as
  `ahiChannels − cahiChannels`, so the identity holds by construction (§5).
- **Index strings**: `STR_TR_OH`, `STR_TR_CH`, `STR_TR_OAHI` and `STR_TR_CAHI` were added.
  `STR_TR_OHI` / `STR_TR_CHI` (§2.5) were **not** — `STR_TR_HI` is itself unused, and the
  printed report builds its `OHI=`/`CHI=` labels inline, so they would have been dead
  strings in ~30 translation files.
- **Two extra defects fixed** beyond §4.10's one, both required for the identity to hold:
  the summary-only AHI fallback in `Session::StoreSummaryToDatabase()` (it computed RDI),
  and `Session::LoadFromDatabase()` dropping `unclassified_count` on reload. See
  `Notes/Developer Notes/BUG_FIXES.md`, 2026-08-14.
- **Printed report** (§3.3) shows `OHI=`/`CHI=` only when the day actually has OH or CH
  data, rather than printing `0.00` for every other device. `OAHI=`/`CAHI=` always show.
- **Daily sidebar** (§5.4) shows OAHI and CAHI on one line directly beneath the AHI banner.
- `Notes/Database/*` were also brought forward from v16 to v18 — the v17 row was missing.

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
| 2.5 | `SleepLib/common.h/.cpp:732,761` | New short strings `STR_TR_OH`, `STR_TR_CH`, and index strings `STR_TR_OHI`, `STR_TR_CHI`, `STR_TR_OAHI`, `STR_TR_CAHI`. Short labels **`OH`** and **`CH`** are confirmed (§8 Q5) |
| 2.6 | `common_gui.h/.cpp:13` | Optional: `COLOR_ObstructiveHypopnea` / `COLOR_CentralHypopnea` if the defaults set in schema.cpp are not enough |
| 2.7 | `SleepLib/schema.cpp:401-411` | **No change** — OH/CH must match `H`'s Overview default (§8 Q6), and `CPAP_Hypopnea` is absent from the `setShowInOverview(true)` block, so all three default to *not* shown. If `H`'s default ever changes, change all three together |

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

**Five** new columns per table: three counts (`obstructive_hypopnea_count`,
`central_hypopnea_count`, `all_apnea_count` — see §4.10 for the last) and two stored indices
(`oahi`, `cahi` — §8 Q2), mirroring the existing `ahi`/`rdi` pair.

| # | File | Change |
|---|---|---|
| 4.1 | `database_schema.h:74` | `CURRENT_SCHEMA_VERSION` 17 → 18. Leave `MIN_RESTORE_SCHEMA_VERSION` at 12 |
| 4.2 | `database_schema.cpp:884` | `session_summaries`: add `obstructive_hypopnea_count`, `central_hypopnea_count`, `all_apnea_count` (`INTEGER DEFAULT 0`) and `oahi`, `cahi` (`REAL DEFAULT 0`) |
| 4.3 | `database_schema.cpp:1060` | `daily_summaries`: same five columns |
| 4.4 | `database_schema.cpp` (new `migrateV17ToV18`) + dispatch at `:218-250` | `ALTER TABLE ... ADD COLUMN` × 5 for each table |
| 4.5 | `session_summaries_repository.h:33-37` / `.cpp` | Struct fields + INSERT/UPDATE/SELECT column lists |
| 4.6 | `daily_summary_repository.h:35-39` / `.cpp:42,58-62,291-295,469-473` | Struct fields, SQL, and `calculateForDay()` population — add `day->count(CPAP_AllApnea)` (currently not recorded at all) and `day->calcOAHI()`/`calcCAHI()` beside the existing `calcAHI()`/`calcRDI()` calls at `:279-280` |
| 4.7 | `session.cpp:3374-3404` | `StoreSummaryToDatabase()`: persist all three counts and both indices, **and** correct the summary-only AHI fallback (§4.10) |
| 4.8 | `session.cpp:3272-3313` | `LoadFromDatabase()` summary-only restore: rebuild `m_cnt` for all three channels |
| 4.9 | `Notes/Database/DATABASE_SCHEMA_REFERENCE.md`, `.mw`, `DATABASE_SCHEMA.md`, `DATA_DICTIONARY.md`, `USEFUL_QUERIES.sql` | Document v18 |

### 4.9a Historical rows are left as they are

**Decided (§8 Q3):** no backfill, no forced recalculation at migration time. Every pre-v18
row keeps 0 in the five new columns, and the migration is a plain set of `ADD COLUMN`s —
fast, and it cannot corrupt anything.

Consequences to document in the release notes rather than engineer around:

- Old SEFAM / G3X / prisma days keep OH and CH folded into `H`. Since `H` sits in the OAHI
  bucket, those days report CAHI as 0 for hypopneas; the split appears only on re-import.
- `all_apnea_count` reads 0 on old days for the few loaders that report `A`, so `oahi` and
  the `.orf` AHI sums understate those days until the day is recalculated.

Both self-heal for any day that gets recalculated or re-imported, so a stale row and a fresh
row can disagree within one profile. Suggest re-import to affected users; do not force it.
`DailySummaryRepository::calculateForDay()` already recalculates on demand, so no new
mechanism is needed.

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
| 5.6 | Database (§4) | **Stored**, not computed (§8 Q2): `oahi` / `cahi` columns in both summary tables, populated from `calcOAHI()`/`calcCAHI()` exactly as `ahi`/`rdi` are. The counts are also present, so SQL can still cross-check: `CAHI = (clear_airway + central_hypopnea) / hours`, `OAHI = (obstructive + obstructive_hypopnea + unclassified + all_apnea + hypopnea) / hours` |

**Verification:** add a check (test or debug assertion) that `OAHI + CAHI == AHI` for every
day, to catch a future AHI channel being added to neither bucket. With v18 the same identity
is checkable in SQL against the stored `ahi` column, which also regression-guards the
`system_reports.orf` sums (§4.10). Restrict any such SQL check to rows written at v18 or
later — pre-v18 rows have 0 in the new columns by design (§4.9a) and will fail it.

---

## 6. Report and export surfaces

| # | File | Change |
|---|---|---|
| 6.1 | `docs/system_reports.orf` | 8 queries reference `hypopnea_count` (lines 36, 70-74, 95-99, 136, 168-171, 193-196, 216-219, 392-400). Add OH/CH/A count columns; add `all_apnea_count` to every hand-rolled AHI/RDI sum (a pre-existing omission, §4.10); surface OAHI/CAHI from the new stored columns for per-row reports, and from the counts for the aggregating (weekly/monthly) ones, matching how `ahi` is already handled |
| 6.2 | `exports/exportcsv.cpp` | No change (uses `ahiChannels`) — verify header/column count in all three report modes |
| 6.3 | `Notes/Database/oscar_details_csv.py`, `Notes/Accessing OSCAR Data/*.md` | Update the documented channel set |

---

## 7. Documentation and translation

- `help/help_en/glossary.html` — add OH/CH/OAHI/CAHI definitions (28 existing hypopnea references).
- `Htmldocs/release_notes.html` — **user writes this.** Two points need covering, both from
  the §8 Q3 "leave existing data as-is" decision: SEFAM / BMC G3X / prisma users who want the
  OH/CH split on past nights must re-import those days, and OAHI may read slightly high on
  pre-upgrade days for the few devices that report undifferentiated `A`.
- `Translations/*.ts` — new `tr()` strings need a regenerate; ~30 language files affected.

---

## 8. Decisions (answered 2026-08-07)

1. **Mutual exclusivity — accepted as designed.** A loader emits *either* `H` *or* `OH`+`CH`
   for a given event, never both. `H` keeps its "unclassified hypopnea" meaning, so BMC G3X
   legitimately populates all three channels (it has a separate unclassified code, 0x01).
   No double-counting results, because each device event maps to exactly one channel.

2. **Store OAHI and CAHI.** `oahi` and `cahi` columns in both `session_summaries` and
   `daily_summaries`, populated the same way `ahi`/`rdi` are. Folded into §4.2–§4.7 and §5.6.

3. **Leave existing data as it is — both gaps.**
   - *OH/CH split (SEFAM / G3X / prisma):* no purge-and-reimport advised. Old days keep both
     kinds folded into `H`.
   - *`all_apnea_count` backfill:* no migration-time recalculation. Few loaders report `A`,
     and re-import may not even be possible for users without the original card. Suggest a
     rebuild to affected users; do not force one.

   Recorded in §4.9a with the user-visible consequences to put in the release notes.

4. **No backport to the 1.7.x line at this time.** 2.0 only.

5. **Short labels `OH` and `CH` confirmed.** No change to `H`, `CA`, `OA`.

6. **Overview default: same as `H`.** `CPAP_Hypopnea` is not in the `setShowInOverview(true)`
   block (`schema.cpp:401-411`), so OH and CH are not added either — all three default to not
   shown, and the user can enable them per-profile in Preferences. Recorded as §2.7.

### Still to settle during implementation

None blocking. Two judgement calls left to whoever implements:

- Exact channel IDs — `0x1011` / `0x1012` proposed, `0x100e` / `0x100f` also free (§2.2).
- Whether OH/CH need distinct default colours or can inherit from `H`'s blue (§2.6). They sit
  adjacent in the event-flags graph and the breakdown pie, so distinct colours are likely
  wanted; `CH` should not be confusable with `CA`'s purple.
