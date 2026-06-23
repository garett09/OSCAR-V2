# Clock Drift History — Engineering Design Spec

Date: 2026-06-23
Status: Draft — ready for implementation
Branch: v2-garett

---

## Overview

`ClockSyncDialog` can analyze nightly offsets and save a correction, but there
is no way to see what it measured before, or to audit which corrections are
active. This feature adds a **read-only Clock Drift History window** that
surfaces per-night offset measurements over time alongside the applied-
corrections ledger, both displayed on an extended `DriftPlotWidget` chart.

The feature requires two prerequisite API additions (Prereq A and Prereq B
below) and a new DB table at schema version 18.

---

## Goals

1. Persist per-night offset measurements so they survive across sessions.
2. Show all stored nightly measurements for a CPAP/reference device pair on
   a chart and in a sortable table.
3. Overlay the applied-corrections history on the same chart, with vertical
   markers at each correction's `date_from`.
4. Let the user optionally include undone (superseded) correction rows in the
   corrections table.
5. Allow export of either tab's visible data to CSV.

---

## Non-Goals

- Editing or undoing corrections from this window (use the existing
  `DeviceTimeCorrectionDialog` for that).
- Device selection in the history window (inherited from `ClockSyncDialog`).
- Per-night re-analysis from the history window.
- Any modification of how `Machine::correctionMs` consumes saved corrections.

---

## Architecture

```
ClockSyncDialog
  |-- "History" button (new QPushButton)
  |-- onAnalyze() [modified]:
  |     calls NightClockOffsetsRepository::upsertNights() after analysis
  |
  +-- opens ClockDriftHistoryDialog(cpap, oxi, this)
        |
        |-- top bar: device label | date range | Reset | Export CSV | Show undone checkbox
        |
        |-- QSplitter (Qt::Vertical)
              |
              +-- DriftPlotWidget (60%)
              |     blue dots    : NightClockOffsetsRepository::findByMachines()
              |     red line     : active drift model from DeviceTimeCorrectionRepository
              |     gray dashed  : prior model (undone drift row, if present)
              |     vertical markers: DriftPlotWidget::setCorrectionDates() [Prereq A]
              |
              +-- QTabWidget (40%)
                    Tab 1 "Nightly offsets"    : NightClockOffsetData rows
                    Tab 2 "Applied corrections": DeviceTimeCorrectionData rows
```

---

## Prerequisites (must be built before the history dialog)

Both are missing from the current codebase and block any work on
`ClockDriftHistoryDialog`.

### Prereq A — `DriftPlotWidget::setCorrectionDates(QList<QDate>)`

**Current public API** (from `oscar/driftplotwidget.h`):

```cpp
void setData(const QList<Point>& points);
void setModel(double c0Ms, double slope);
void clearModel();
void setReferenceModel(double c0Ms, double slope);
void clearReferenceModel();
void setRefPoints(const QList<Point>& pts);
void clearRefPoints();
void setDateRange(const QDate& start, const QDate& end);
void clear();
```

No correction-date marker method exists.

**Change required:**

Add to `DriftPlotWidget`:

```cpp
// driftplotwidget.h — public section
void setCorrectionDates(const QList<QDate>& dates);
void clearCorrectionDates();

// driftplotwidget.h — private member
QList<QDate> m_correctionDates;
```

In `paintEvent` (after the X-axis labels, before the data dots), iterate
`m_correctionDates` and for each date:

1. Compute `x = toX(date)` using the lambda already on lines 152–154 of
   `driftplotwidget.cpp`:
   ```cpp
   auto toX = [&](QDate d) -> double {
       return plot.left() + (double)minD.daysTo(d) / days * plot.width();
   };
   ```
2. Draw a thin vertical line from `plot.top()` to `plot.bottom()`.
3. Draw a small date caption just above `plot.top()`.

Accessibility constraints:
- Line color must have >= 3:1 contrast against the white plot background. Use
  a dark orange such as `QColor(180, 80, 0)`.
- The date caption text (non-color cue) is mandatory so color-blind users can
  identify markers without relying on color alone.
- Line style: solid, 1 px.

### Prereq B — `DeviceTimeCorrectionRepository::findAllIncludingUndone(qint64 machineId)`

**Current methods** (from `oscar/database/device_time_correction_repository.h`):

| Method | Filter |
|--------|--------|
| `findActive(machineId)` | `WHERE undone_at IS NULL` |
| `findManualOffsetRows(machineId)` | `WHERE undone_at IS NULL` |

Neither returns undone rows.

**Change required:**

Add to `DeviceTimeCorrectionRepository`:

```cpp
// device_time_correction_repository.h — public section
QList<DeviceTimeCorrectionData> findAllIncludingUndone(qint64 machineId);
```

Implementation: same SELECT as `findActive` (all columns mapped through
`recordToData`) but **without** the `undone_at IS NULL` predicate. Order by
`date_from ASC, id ASC`. This drives the "Show undone" checkbox in
`ClockDriftHistoryDialog`.

---

## New DB Schema — `night_clock_offsets` (schema version 18)

### Table definition

```sql
CREATE TABLE IF NOT EXISTS night_clock_offsets (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    machine_id      INTEGER NOT NULL
                        REFERENCES machines(id) ON DELETE CASCADE,
    ref_machine_id  INTEGER NOT NULL,
    date            TEXT    NOT NULL,   -- YYYY-MM-DD (session date)
    start_diff_ms   INTEGER NOT NULL,
    end_diff_ms     INTEGER NOT NULL,
    mid_ms          INTEGER NOT NULL,
    used            INTEGER NOT NULL,   -- 1 = kept by fit, 0 = outlier
    validated       INTEGER NOT NULL DEFAULT 0,
    validator_agrees INTEGER,           -- NULL if not validated
    recorded_at     TEXT NOT NULL DEFAULT (datetime('now')),
    UNIQUE(machine_id, ref_machine_id, date)
);
```

The UNIQUE constraint on `(machine_id, ref_machine_id, date)` means re-running
"Analyze Nights" for the same pair and date is a safe upsert — it replaces
the prior measurement without creating duplicates.

### Index

```sql
CREATE INDEX IF NOT EXISTS idx_nco_machine_date
    ON night_clock_offsets(machine_id, ref_machine_id, date);
```

### Migration

Add `migrateV17ToV18` in `database_schema.cpp` following the existing pattern:

```cpp
bool DatabaseSchema::migrateV17ToV18(QSqlDatabase& db)
{
    if (!db.transaction()) { ... return false; }
    if (!createNightClockOffsetsTable(db)) { db.rollback(); return false; }
    if (!setSchemaVersion(db, 18)) { db.rollback(); return false; }
    if (!db.commit()) { db.rollback(); return false; }
    return true;
}
```

Update `CURRENT_SCHEMA_VERSION` in `database_schema.h` from 17 to 18.

Add the `migrateV17ToV18` case to `upgradeSchema()`:

```cpp
if (fromVersion == 17) {
    if (!migrateV17ToV18(db)) {
        qCritical() << "DatabaseSchema: v17->v18 migration failed";
        return false;
    }
    fromVersion = 18;
}
```

Also call `createNightClockOffsetsTable` from `createSchema()` (and declare
the private helper in `database_schema.h`).

---

## New Repository — `NightClockOffsetsRepository`

### Data struct (`night_clock_offsets_repository.h`)

```cpp
struct NightClockOffsetData {
    qint64  id             = 0;
    qint64  machineId      = 0;
    qint64  refMachineId   = 0;
    QString date;              // "YYYY-MM-DD"
    qint64  startDiffMs    = 0;
    qint64  endDiffMs      = 0;
    qint64  midMs          = 0;
    bool    used           = true;
    bool    validated      = false;
    int     validatorAgrees = -1;  // -1 = NULL (not validated), 0 = disagrees, 1 = agrees
    QString recordedAt;
};
```

### Public API

```cpp
class NightClockOffsetsRepository
{
public:
    // Upsert a batch of NightStartDiff results for a machine pair.
    // Called by ClockSyncDialog::onAnalyze() after analysis completes.
    void upsertNights(qint64 machineId, qint64 refMachineId,
                      const QVector<NightStartDiff>& nights);

    // Retrieve stored measurements for a machine pair, optionally filtered
    // to a date range. Pass invalid QDate for either bound to skip it.
    QList<NightClockOffsetData> findByMachines(qint64 machineId,
                                               qint64 refMachineId,
                                               const QDate& from = QDate(),
                                               const QDate& to   = QDate());

private:
    QSqlDatabase database();
    NightClockOffsetData recordToData(QSqlQuery& q);
};
```

`upsertNights` maps each `NightStartDiff` to a row and executes:

```sql
INSERT INTO night_clock_offsets
    (machine_id, ref_machine_id, date, start_diff_ms, end_diff_ms,
     mid_ms, used, validated, validator_agrees)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(machine_id, ref_machine_id, date)
    DO UPDATE SET
        start_diff_ms    = excluded.start_diff_ms,
        end_diff_ms      = excluded.end_diff_ms,
        mid_ms           = excluded.mid_ms,
        used             = excluded.used,
        validated        = excluded.validated,
        validator_agrees = excluded.validator_agrees,
        recorded_at      = datetime('now');
```

---

## UI Spec — `ClockDriftHistoryDialog`

### Window

- Class: `QDialog`, modal (application modal).
- Window title: "Clock Drift History — \<CPAP label\> vs \<ref label\>".
- Minimum size: 800 × 560 px. Resizable.

### Top bar (`QHBoxLayout`)

| Widget | Details |
|--------|---------|
| `QLabel` deviceLabel | Read-only: "CPAP: ResMed AirSense 10 (SN) | Ref: Viatom O2 Ring (SN)". Not editable. |
| `QDateEdit` fromDate | Default: earliest date in `night_clock_offsets` for this pair. |
| `QDateEdit` toDate | Default: today. |
| `QPushButton` btnReset | Resets both date edits to full range. |
| `QPushButton` btnExport | "Export CSV" — exports currently visible tab's rows. |
| `QCheckBox` chkShowUndone | "Show undone corrections". Drives Tab 2 query. |

### QSplitter (Qt::Vertical)

**Top pane (~60% initial height):** `DriftPlotWidget`

- Blue filled dots: `NightClockOffsetData::midMs` for each date (used nights).
- Gray hollow dots: `midMs` for `used == false` (outlier) nights.
- Red solid line: active drift correction model from `DeviceTimeCorrectionRepository::findActive()`.
- Gray dashed line: most recent undone drift row's model (if any), drawn via `setReferenceModel`.
- Dark orange vertical markers: `setCorrectionDates()` called with each correction's `date_from`
  (Prereq A).

**Bottom pane (~40% initial height):** `QTabWidget`

Tab 1 — "Nightly offsets":

| Column | Source field | Format |
|--------|-------------|--------|
| Date | `NightClockOffsetData::date` | YYYY-MM-DD |
| Start delta (s) | `startDiffMs / 1000.0` | 1 decimal place |
| End delta (s) | `endDiffMs / 1000.0` | 1 decimal place |
| Offset (s) | `midMs / 1000.0` | 1 decimal place |
| Used | `used` | "yes" / "no (outlier)" |
| Cross-check | `validated`, `validatorAgrees` | "—" / "agrees" / "disagrees" |

Rows where `used == false` are rendered in gray (same convention as
`ClockSyncDialog::populateTable`). All cells are read-only
(`Qt::ItemIsEnabled` only, no `Qt::ItemIsEditable`).

Tab 2 — "Applied corrections":

| Column | Source field | Format |
|--------|-------------|--------|
| From | `DeviceTimeCorrectionData::dateFrom` | YYYY-MM-DD |
| To | `dateTo` | YYYY-MM-DD or "open" |
| Type | `type` | as stored ('offset', 'drift', etc.) |
| Value | `offsetMs` or `c0Ms/c1` | constant: "N s"; drift: "N s/day from YYYY-MM-DD" |
| Applied | `appliedAt` | YYYY-MM-DD HH:MM |
| Status | `undoneAt` | "active" (green) / "superseded YYYY-MM-DD" (gray) |

When `chkShowUndone` is unchecked, query uses `findActive(machineId)`.
When checked, query uses `findAllIncludingUndone(machineId)` (Prereq B).

### Export CSV

Triggered by "Export CSV" button. Exports the **currently selected tab's**
visible rows (respecting the date range filter) to a user-chosen `.csv` file
via `QFileDialog::getSaveFileName`. Column headers match the table headers.
No confirmation dialog; the file-save dialog is sufficient.

---

## Data Flow

1. **Analysis writes measurements.**
   `ClockSyncDialog::onAnalyze()` (existing method) is modified to call
   `NightClockOffsetsRepository::upsertNights(cpap->getDatabaseId(),
   oxi->getDatabaseId(), m_nights)` after `populateTable()` returns. This is
   the sole write path. The analysis itself is unchanged.

2. **User opens history.**
   "History" button added to `ClockSyncDialog` connects to a new slot
   `onHistory()`. That slot constructs `ClockDriftHistoryDialog(currentCpap(),
   currentOxi(), this)` and calls `exec()`.

3. **Dialog loads initial data.**
   On construction (or `showEvent`), the dialog calls:
   - `NightClockOffsetsRepository::findByMachines(cpapId, oxiId)` — all
     stored nights, no date filter — to determine the default `from` date.
   - Then re-queries with the full range to populate the chart and Tab 1.
   - `DeviceTimeCorrectionRepository::findActive(cpapId)` to populate Tab 2
     and the chart's model lines and correction markers.

4. **Date range change.**
   Either `QDateEdit` signal `dateChanged` triggers `refresh()`:
   - Re-queries `NightClockOffsetsRepository::findByMachines` with the new range.
   - Re-populates `DriftPlotWidget` (calls `setData`, `setModel`/`clearModel`,
     `setReferenceModel`/`clearReferenceModel`, `setCorrectionDates`) and Tab 1.
   - Tab 2 and the correction model are re-queried on every refresh for
     consistency.

5. **"Show undone" toggle.**
   `chkShowUndone::toggled` triggers `refresh()`. On the Tab 2 query it
   switches between `findActive` and `findAllIncludingUndone`.

6. **Reset.**
   `btnReset` resets both date edits to full-range (min date from stored
   measurements, max date = today), which triggers `refresh()` via the
   `dateChanged` signals.

7. **Export.**
   `btnExport::clicked` determines the current tab index, collects the visible
   table rows, and writes them to CSV.

---

## New Files

| File | Purpose |
|------|---------|
| `oscar/clockdrifthistorydialog.h` | Dialog class declaration |
| `oscar/clockdrifthistorydialog.cpp` | Dialog implementation |
| `oscar/clockdrifthistorydialog.ui` | Qt Designer form (top bar + splitter + tabs) |
| `oscar/database/night_clock_offsets_repository.h` | `NightClockOffsetData` struct + `NightClockOffsetsRepository` class |
| `oscar/database/night_clock_offsets_repository.cpp` | `upsertNights` + `findByMachines` implementation |

---

## Modified Files

| File | Change |
|------|--------|
| `oscar/driftplotwidget.h` | Add `setCorrectionDates` / `clearCorrectionDates` declarations and `m_correctionDates` member (Prereq A) |
| `oscar/driftplotwidget.cpp` | Implement marker rendering in `paintEvent` (Prereq A) |
| `oscar/database/device_time_correction_repository.h` | Add `findAllIncludingUndone` declaration (Prereq B) |
| `oscar/database/device_time_correction_repository.cpp` | Implement `findAllIncludingUndone` (Prereq B) |
| `oscar/database/database_schema.h` | Add `createNightClockOffsetsTable` private declaration; bump `CURRENT_SCHEMA_VERSION` from 17 to 18 |
| `oscar/database/database_schema.cpp` | Add `createNightClockOffsetsTable`, `migrateV17ToV18`; wire into `createSchema` and `upgradeSchema` |
| `oscar/clocksyncdialog.h` | Add `NightClockOffsetsRepository` member; add `onHistory()` private slot |
| `oscar/clocksyncdialog.cpp` | Add `#include "database/night_clock_offsets_repository.h"`; connect "History" button; call `upsertNights` in `onAnalyze`; implement `onHistory` |
| `oscar/clocksyncdialog.ui` | Add "History" QPushButton (objectName: `btnHistory`) next to existing buttons |
| `oscar/oscar.pro` (or CMakeLists) | Add new `.h`/`.cpp`/`.ui` files to build |

---

## Open Questions

1. **Retention policy.** Should `night_clock_offsets` rows be pruned when the
   corresponding session is deleted, or retained indefinitely? The foreign key
   `ON DELETE CASCADE` on `machine_id` handles device deletion, but not per-
   night session deletion.

2. **Multiple reference devices.** The dialog inherits the single oxi combo
   selection from `ClockSyncDialog`. If a user switches reference devices over
   time, the history window will only show nights measured against the currently
   selected oxi. Is that acceptable, or should the history window show all
   reference machines for a given CPAP?

3. **Chart Y-axis scale.** When the date range spans a correction boundary
   (e.g., an applied offset shifts the data by minutes), the Y range can be
   very large. Should `DriftPlotWidget` auto-scale per the existing padding
   logic (lines 144–147 of `driftplotwidget.cpp`), or should there be a manual
   scale lock option in the history dialog?

4. **Tab 2 "Value" column formatting.** Drift rows carry `c0Ms` and `c1` which
   are not immediately human-readable. Proposed: convert to `slopeMsPerDay`
   using `slopeMsPerDay = -c1 * 86_400_000` and display as "N s/day from
   date_from". Confirm this is the right presentation.

5. **Build system.** The project uses both `oscar.pro` and `CMakeLists.txt`.
   Confirm which is authoritative for the v2-garett branch so the new files
   are wired into the correct build file.
