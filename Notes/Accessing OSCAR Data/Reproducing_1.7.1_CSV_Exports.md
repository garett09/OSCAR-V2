# Reproducing OSCAR 1.7.1's CSV Exports in OSCAR 2.0

**Status:** Analysis & design notes (no committed feature work beyond the
standalone script described in Part 3).
**Audience:** OSCAR developers.
**Scope:** How much of 1.7.1's Daily / Sessions / Details CSV exports can be
reproduced in OSCAR 2.0, by what means (SQL, Python, C++), and — if we build a
user-extensible Python script library — where it should live in an installed
OSCAR.

---

## Background: why the two versions differ

OSCAR 1.7.1 and 2.0 ship a same-named "Export to CSV" feature with fundamentally
different implementations.

- **1.7.1** (`exportcsv.cpp`) computes everything **in memory** from loaded
  `Day` / `Session` / `EventList` objects — `day->calcAHI()`, `day->count()`,
  `sess->percentile()`, and direct iteration of `sess->eventlist`. It has three
  hardcoded modes: **Summary** (per-day), **Sessions** (per-session), and
  **Details** (per-event).

- **2.0** runs **SQL queries** against the SQLite database and writes the result
  columns straight to CSV. Reports are query templates stored in the
  `report_tree` table (loaded from `system_reports.orf`) with
  `#PROFILE_ID` / `#START_DATE` / `#END_DATE` macros. Shipped reports:
  **Daily Summaries**, **Session Statistics**, **Device Settings**. There is
  **no Details report**.

So the question of "can we reproduce 1.7.1" splits by what the data lives as in
2.0: pre-aggregated columns and per-session stats are reachable by SQL; raw
per-sample event/waveform data is stored as compressed BLOBs that SQL cannot
unpack.

---

## Part 1 — Daily & Sessions reports: ~95% reproducible in SQL

Both 1.7.1 reports share a column shape (Summary = per OSCAR-day, Sessions =
per session):

- Fixed: Date, Session Count / Session #, Start, End, Total Time, AHI
- **Count** columns for the *countlist* channels: the AHI/flag events
  (Obstructive, Hypopnea, CA, RERA, VSnore, FlowLimit, SensAwake, NRI, ExP,
  LeakFlag, UserFlag1/2, PressurePulse)
- **Average / Percentile / Max** columns for the *avglist* channels:
  Pressure, PressureSet, IPAP, IPAPSet, EPAP, EPAPSet, FLG

### Reproducibility map (2.0)

| 1.7.1 column group | 2.0 source | Reproducible? |
|---|---|---|
| Date, Session Count, AHI, RDI | `daily_summaries` / `session_summaries` | ✅ exact |
| OA/UA/H/CA/RERA counts | `daily_summaries` / `session_summaries` | ✅ exact |
| Total Time / hours | `mask_on_hours`, `total_hours` | ✅ exact |
| Start / End | `MIN(start_time)`/`MAX(end_time)` via join to `sessions` | ✅ via join |
| Pressure avg / 95th / max | `daily_summaries` (Pressure) or `session_channels` | ✅ |
| Flag counts (VSnore, FlowLimit, …) | `session_channels.count` (per channel, every channel) | ✅ via join/pivot |
| avg/p90/max for PressureSet/IPAP/EPAP/FLG | `session_channels.avg/p90/p95/max` | ✅ per-session; ⚠️ per-day approximate |

**Two caveats:**

1. **Per-day percentiles are approximate.** 1.7.1's Summary computed
   `day->percentile()` over the whole day's merged samples. The DB stores only
   per-*session* p90/p95 (`session_channels`), so a day-level percentile means
   averaging session percentiles — not statistically equivalent. (The existing
   2.0 weekly/monthly queries already accept this via `AVG(pressure_95th)`.)
   Per-**session** percentiles are exact.
2. **Flag / pressure-variant columns need a pivot.** `session_channels` is one
   row per (session, channel); 1.7.1's wide layout needs
   `MAX(CASE WHEN channel_id=… THEN …)` pivoting.

**Bottom line:** the common subset (AHI, the five apnea counts, pressure, leak,
hours) is exact and already shipped; the full 1.7.1 column set is reachable but
needs verbose pivots, and true day-level percentiles can only be approximated.

---

## Part 2 — The Details report

### What it does

The 1.7.1 Details mode (`exportcsv.cpp`, the `rb1_details` branch) is an
**event-level raw dump** — one CSV row per individual data point:

```
DateTime, Session, Event, Data/Duration
```

For each session it walks `countlist + avglist` channels, finds each channel's
`EventList`, and for every sample writes the timestamp, session id, channel
code, and the value `ev->data(q)`. The column is "Data/Duration" because for
apnea-type event channels that value *is* the event duration (seconds), while
for flag/waveform channels it is the measured value.

### Resolution characteristics (important)

It writes **every sample** of the included channels — no downsampling — **but**:

- **Restricted channel set.** Only countlist + avglist. The heavy waveforms
  (Flow Rate 25 Hz, Mask Pressure, Snore, Resp Rate, Tidal Volume, Minute Vent,
  leak waveform) are **not** included.
- **Timestamps truncated to whole seconds.** `ev->time(q)` is milliseconds, but
  the code does `/1000` → `fromSecsSinceEpoch` → `Qt::ISODate`
  (`yyyy-MM-ddTHH:mm:ss`). Sub-second precision is lost; channels faster than
  1 Hz produce duplicate timestamps.
- **Values rounded to 2 decimals** (`QString::number(value,'f',2)`).

So: "full sample-count resolution for a limited channel set, at second
granularity and 2-decimal precision" — not "every detail of everything."

### Can SQL reproduce it? Only partially.

Per-sample data in 2.0 lives in two storage models:

1. **Scored respiratory events** (OA/UA/H/RERA/CA) → queryable rows in
   `respiratory_events` with `start_time`/`end_time`/`duration`.
   ✅ Reproducible with plain SQL.
2. **Everything else** — the flag channels and the pressure/FLG waveforms →
   compressed binary BLOBs in `event_data` (`data_compressed` /
   `time_compressed`, qCompress'd `qint16` / `quint32` arrays).
   ❌ **Not reproducible in SQL.** SQLite cannot decompress or unpack these into
   per-sample rows.

---

## Part 3 — Standalone Python script (the chosen approach)

Python can reach what SQL can't, because it can decompress and unpack the BLOBs.
No Qt or OSCAR build required. A script has been written:

> **`Notes/Database/oscar_details_csv.py`** — standalone, read-only,
> stdlib-only (`sqlite3` / `zlib` / `array`). Reproduces 1.7.1's Details export
> exactly.

### Verified `event_data` decode format

All facts confirmed against the 2.0 source (`event_data_repository.cpp`,
`event.cpp`, `machine_common.h`):

| Element | Fact | Source |
|---|---|---|
| Compression | `qCompress(data, 6)`, only if ≥500 B **and** saves >10% | `compressIfBeneficial` |
| Compressed vs raw | either `*_compressed` or `*_blob` populated, never both | `storeEventListData` |
| Primary samples | little-endian `qint16` (`EventStoreType`) | `serializeInt16Array`, `machine_common.h` |
| Time array | little-endian `quint32`, **events only** (not waveforms) | `serializeEventList` |
| Real value | `value(i) = raw_int16[i] * gain` — **gain only, no offset** | `event.cpp:59` |
| Event timestamp | `first_time + time_delta[i]` (ms) | `event.cpp:51` |
| Waveform timestamp | `first_time + int(i * rate)` (ms) | `event.cpp:54` |

**The one non-obvious gotcha:** `qCompress` is *not* plain zlib — it prepends a
4-byte **big-endian uncompressed-length header** before the zlib stream.
`zlib.decompress(blob)` fails; `zlib.decompress(blob[4:])` works.

### How the script matches 1.7.1

- Columns `DateTime,Session,Event,Data/Duration`.
- Exact 1.7.1 channel set in iteration order. `ahiChannels` =
  ClearAirway(0x1001), AllApnea(0x1010), Obstructive(0x1002), Hypopnea(0x1003),
  Apnea(0x1004); then the explicit flags and the pressure family. IDs taken from
  `schema.cpp` and `prs1_loader.cpp` (`PressurePulse` = 0x1009).
- **2.0 divergence:** 2.0's `ahiChannels` also carries
  ObstructiveHypopnea(0x1011) and CentralHypopnea(0x1012), so 2.0's own CSV
  export has two more count columns than 1.7.1's. The script keeps the 1.7.1 set
  on purpose; a database imported from a SEFAM, BMC Luna G3X or Loewenstein
  prisma device under 2.0 will hold hypopnea events it does not emit.
- `Event` column uses the compiled-in schema code
  (`schema::channel[key].code()`), used directly rather than the per-profile
  `channels` table so it cannot diverge.
- Whole-second **local** ISO timestamps and 2-decimal values — 1.7.1's
  precision loss preserved deliberately.
- Row order: session (by start) → channel (list order) → eventlist → sample.
- OSCAR-day grouping via the noon split (`--day-split-hour`, default 12) for the
  date filter; the `DateTime` column is wall-clock, so a session on OSCAR-day D
  can legitimately show early-morning timestamps dated D+1.

### Tested

Against a real `oscar.db`:
- Multi-profile guard lists profiles when ambiguous.
- Blue Dragon 2025: 278,924 rows / 866 sessions / 4280 eventlists; apnea
  durations render correctly (`Obstructive,39.00` = a 39-second event); waveform
  channels (FLG, EPAP, Pressure) dominate the row count, as in 1.7.1.
- Date-range filtering and read-only (`mode=ro`) access confirmed.

### Usage

```
python oscar_details_csv.py --db <path/to/oscar.db> \
    [--profile USERNAME | --profile-id N] \
    [--start YYYY-MM-DD] [--end YYYY-MM-DD] \
    [--output out.csv] [--day-split-hour 12]
```

### Design stance

If a user wants **higher-resolution** output (millisecond timestamps, full
precision, the high-rate waveforms 1.7.1 omits, the secondary data field), they
write their own script. This one stays locked to 1.7.1 behaviour by design.

---

## Part 4 — Optional C++ integration

Porting the legacy in-memory Details code back into OSCAR 2.0 is feasible as a
**separate menu item that applies to the currently open profile**.

- **It ports cleanly** because 2.0 preserved the `Session` / `EventList` / `Day`
  API and `Session::OpenEvents()` (session.cpp:137) falls through to
  `LoadEvents()` → `LoadEventsFromDatabase()` (session.cpp:910). I.e. it
  hydrates the in-memory EventLists from the `event_data` BLOBs, so the old walk
  (`ev->time(q)` / `ev->data(q)` / `schema::channel[key].code()`) runs
  essentially unchanged — it just sources data from the DB instead of `.001`
  files.
- **It is naturally profile-scoped** — the old code already operates on
  `p_profile`, `p_profile->daylist`, `p_profile->GetDay(date, MT_CPAP)`. No
  profile parameter to thread through.

Things to handle if built:
- **Memory.** `OpenEvents()` decodes every in-range session's BLOBs into RAM.
  Keep the old `if (daily_date != date) sess->TrashEvents();` pattern to bound
  the footprint.
- **Cancellation.** Per CLAUDE.md, Cancel must bite within a few seconds. The
  old loop only did `processEvents()` per day; add a cancel flag checked in the
  inner loops.
- Keep this a **separate path** from the SQL `ExportCSV` dialog — the two
  paradigms (SQL result dump vs. in-memory event walk) shouldn't be entangled.

---

## Part 5 — If we build a user-extensible Python script library

The standalone script is the first of what could become a small library of
analysis tools users can add to. Two questions: how to structure it, and where
it lives in an *installed* OSCAR.

### Where (installed OSCAR)

The scripts are **tools, not data**, so they must not live in a database's data
directory:

- **Application install dir** (`Program Files\OSCAR\`, `OSCAR.app`, distro
  package path) — **wrong**: often read-only, replaced on every upgrade.
- **OSCAR data directory** (`GetAppData()`, the folder with `oscar.db` and
  `Profiles/`) — **wrong**: `GetAppData()` is **per-database**. OSCAR 2.0
  supports multiple databases (`mainwindow.cpp` `switchToDatabase()` /
  `RecentDatabases`), so a script library here would be duplicated and divergent
  across a multi-database user's data dirs.
- **Per-user application location** — **right**, and already used by OSCAR:
  `help.cpp:34` calls
  `QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)`.

```
QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Scripts/"
```

| Platform | Resolved path |
|---|---|
| Windows | `C:\Users\<user>\AppData\Roaming\OSCAR\Scripts\` |
| macOS | `~/Library/Application Support/OSCAR/Scripts/` |
| Linux | `~/.local/share/OSCAR/Scripts/` |

This gives **one** shared library per user: database-independent, survives app
upgrades, user-writable without admin.

### Runtime model

The library is shared and fixed; OSCAR passes the **currently-open** database
(`GetAppData()`) to whichever script the user runs, via the `--db` argument the
script already accepts. Multiple databases → still one library, pointed at the
active DB each time. Tools live with the app; the data is named at invocation.

### Two-tier search path (optional)

If OSCAR ships and maintains its own scripts, mirror the existing `report_tree`
system/user split:

- **Bundled scripts** — read-only, shipped with the app (install dir or Qt
  resources), refreshed on upgrade. Host the shared `oscar_db.py` helper here.
- **User scripts** — `AppDataLocation/Scripts/`, never touched by upgrades.

OSCAR scans both and merges the list. A shared `oscar_db.py` (read-only connect,
profile resolve, the BLOB decode helpers, channel-ID maps, `oscar_day()`,
common CLI args) keeps new scripts short.

### The real constraint: Python availability

OSCAR ships as a self-contained C++/Qt binary; a typical CPAP user has **no
Python interpreter** and isn't comfortable in a terminal. So a script library is
inherently a **power-user feature**.

| Approach | Cost | Verdict |
|---|---|---|
| Bring-your-own Python (detect `python3` on PATH; explain install if absent) | Low (docs) | **Recommended** |
| Bundle a Python runtime | High — fattens all ~25 platform builds, macOS notarization, security-update burden | Avoid |
| No runner, just the folder + README | Lowest | Fine as a v1 |

**Recommendation:** seed `AppDataLocation/Scripts/` with the helper lib +
bundled scripts + a README, and gate any in-app runner behind "is Python on
PATH?" — never bundle an interpreter.

---

## Summary

- **Daily & Sessions:** ~95% reproducible in SQL; exact for the common subset,
  pivots needed for the full column set, day-level percentiles only approximate.
- **Details:** a per-sample dump of a restricted channel set at second/2-decimal
  resolution. The respiratory-event subset is SQL-reproducible; the
  waveform/flag samples live in compressed `event_data` BLOBs that SQL cannot
  unpack.
- **Chosen solution:** a standalone, read-only Python script
  (`Notes/Database/oscar_details_csv.py`) that decodes the BLOBs and reproduces
  1.7.1's Details output exactly. Higher resolution is left to user-written
  scripts.
- **C++ option:** the legacy in-memory walk ports cleanly as a separate,
  profile-scoped menu item (`OpenEvents()` now loads from the DB); mind memory,
  trashing, and cancellation.
- **Script library, if built:** lives in
  `QStandardPaths::AppDataLocation/Scripts/` (per-user, database-independent,
  upgrade-safe — already used by `help.cpp`), with the active DB passed via
  `--db`; optionally a two-tier bundled/user split mirroring `report_tree`.
