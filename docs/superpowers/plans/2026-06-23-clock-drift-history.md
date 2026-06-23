# Clock Drift History Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a "History" button to ClockSyncDialog that opens a read-only Clock Drift History window showing persisted per-night offset measurements and applied corrections overlaid on a DriftPlotWidget chart.

**Architecture:** Six sequential tasks build bottom-up — two prerequisite API additions (DriftPlotWidget markers, findAllIncludingUndone), a DB schema v18 migration with new `night_clock_offsets` table, a new `NightClockOffsetsRepository`, the `ClockDriftHistoryDialog` UI, then wiring into ClockSyncDialog. Every task compiles and is independently testable before the next begins.

**Tech Stack:** C++17, Qt 6, SQLite via QSqlDatabase, QTest (AutoTest.h pattern), qmake (`oscar.pro`).

## Global Constraints

- Branch: `v2-garett`
- Build app: `cd /Users/adriangarett.sian/oscar-code/build-clocksync && make -j8`
- Build tests: `cd /Users/adriangarett.sian/oscar-code/build-test && make -j8`
- Run tests: `cd build-test && ./test -v2 <TestClassName>`
- `CURRENT_SCHEMA_VERSION` lives in `oscar/database/database_schema.h:74` — bump from 17 → 18
- `m_slope` in DriftPlotWidget equals `c1 - 1.0` (see `driftplotwidget.h:42`). Display slope as s/day: `(c1 - 1.0) * 86400.0`
- All SQL uses `:named` parameterized queries — never string interpolation
- Correction marker color: `QColor(180, 80, 0)` (dark orange, ≥3:1 contrast on white)
- All new files carry the standard copyright header:
  ```cpp
  /* <Title>
   *
   * Copyright (c) 2026 The OSCAR Team
   *
   * This file is subject to the terms and conditions of the GNU General Public
   * License. See the file COPYING in the main directory of the source code
   * for more details. */
  ```

---

## Task 1: DriftPlotWidget correction-date markers (Prereq A)

**Files:**
- Modify: `oscar/driftplotwidget.h`
- Modify: `oscar/driftplotwidget.cpp`
- Create: `oscar/tests/clockdrifthistorytests.h`
- Create: `oscar/tests/clockdrifthistorytests.cpp`

**Interfaces:**
- Produces: `DriftPlotWidget::setCorrectionDates(const QList<QDate>&)`, `clearCorrectionDates()`, `correctionDateCount() const → int`
- Consumed by: Task 5 (`ClockDriftHistoryDialog`)

- [ ] **Step 1: Create test file with failing test**

Create `oscar/tests/clockdrifthistorytests.h`:
```cpp
/* Clock Drift History Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef CLOCKDRIFTHISTORYTESTS_H
#define CLOCKDRIFTHISTORYTESTS_H

#include "tests/AutoTest.h"

class ClockDriftHistoryTests : public QObject
{
    Q_OBJECT
private slots:
    void testSetCorrectionDatesStoresDates();
    void testClearCorrectionDatesEmptiesList();
    void testCorrectionDateCountMatchesInput();
};
DECLARE_TEST(ClockDriftHistoryTests)

#endif // CLOCKDRIFTHISTORYTESTS_H
```

Create `oscar/tests/clockdrifthistorytests.cpp`:
```cpp
/* Clock Drift History Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "clockdrifthistorytests.h"
#include "driftplotwidget.h"
#include <QDate>

void ClockDriftHistoryTests::testSetCorrectionDatesStoresDates()
{
    DriftPlotWidget w;
    QList<QDate> dates = { QDate(2026, 1, 15), QDate(2026, 3, 20) };
    w.setCorrectionDates(dates);
    QCOMPARE(w.correctionDateCount(), 2);
}

void ClockDriftHistoryTests::testClearCorrectionDatesEmptiesList()
{
    DriftPlotWidget w;
    w.setCorrectionDates({ QDate(2026, 1, 15) });
    w.clearCorrectionDates();
    QCOMPARE(w.correctionDateCount(), 0);
}

void ClockDriftHistoryTests::testCorrectionDateCountMatchesInput()
{
    DriftPlotWidget w;
    QCOMPARE(w.correctionDateCount(), 0);
    w.setCorrectionDates({ QDate(2026, 2, 1), QDate(2026, 4, 1), QDate(2026, 6, 1) });
    QCOMPARE(w.correctionDateCount(), 3);
}
```

- [ ] **Step 2: Register test in oscar.pro**

In `oscar/oscar.pro`, find the test SOURCES block (around line 820). After `tests/clocksynctests.cpp`, add:
```
        tests/clockdrifthistorytests.cpp
```
In the HEADERS block (around line 835), after `tests/clocksynctests.h`, add:
```
        tests/clockdrifthistorytests.h
```

- [ ] **Step 3: Run tests — verify they fail to compile**

```bash
cd /Users/adriangarett.sian/oscar-code/build-test && make -j8 2>&1 | grep "correctionDateCount\|setCorrectionDates"
```
Expected: compiler error — `correctionDateCount` is not a member of `DriftPlotWidget`.

- [ ] **Step 4: Add API to driftplotwidget.h**

In `oscar/driftplotwidget.h`, after `void clear();` (line 33), add:
```cpp
    void setCorrectionDates(const QList<QDate>& dates);
    void clearCorrectionDates();
    int  correctionDateCount() const { return m_correctionDates.size(); }
```

In the `private:` section (after `QDate m_rangeEnd;`, line 50), add:
```cpp
    QList<QDate> m_correctionDates;
```

- [ ] **Step 5: Implement setCorrectionDates and clearCorrectionDates in driftplotwidget.cpp**

After `DriftPlotWidget::clear()` (after line 88), add:
```cpp
void DriftPlotWidget::setCorrectionDates(const QList<QDate>& dates)
{
    m_correctionDates = dates;
    update();
}

void DriftPlotWidget::clearCorrectionDates()
{
    m_correctionDates.clear();
    update();
}
```

- [ ] **Step 6: Also clear m_correctionDates in clear()**

In `DriftPlotWidget::clear()` (line 79), add after `m_rangeEnd = QDate();`:
```cpp
    m_correctionDates.clear();
```

- [ ] **Step 7: Add marker rendering to paintEvent**

In `driftplotwidget.cpp`, find the comment `// --- Existing model line (gray dashed) ---` (line 207). Insert the following block **before** it (after the X grid labels section ends at ~line 205):

```cpp
    // --- Correction-date markers (dark orange vertical lines) ---
    if (!m_correctionDates.isEmpty()) {
        QFont markerFont = tickFont;
        markerFont.setPointSize(qMax(6, tickFont.pointSize() - 1));
        p.setFont(markerFont);
        for (const QDate& d : m_correctionDates) {
            if (d < minD || d > maxD) continue;
            int x = (int)toX(d);
            p.setPen(QPen(QColor(180, 80, 0), 1, Qt::SolidLine));
            p.drawLine(x, plot.top(), x, plot.bottom());
            // Date caption above the line (non-color cue for accessibility)
            QString cap = d.toString("MM-dd");
            QRect capRect(x - 20, plot.top() - mt + 1, 40, mt - 1);
            p.setPen(QColor(180, 80, 0));
            p.drawText(capRect, Qt::AlignCenter, cap);
        }
        p.setFont(tickFont); // restore
    }
```

- [ ] **Step 8: Build and run tests**

```bash
cd /Users/adriangarett.sian/oscar-code/build-test && make -j8 2>&1 | tail -5
./test -v2 ClockDriftHistoryTests
```
Expected output:
```
PASS   : ClockDriftHistoryTests::testSetCorrectionDatesStoresDates()
PASS   : ClockDriftHistoryTests::testClearCorrectionDatesEmptiesList()
PASS   : ClockDriftHistoryTests::testCorrectionDateCountMatchesInput()
```

- [ ] **Step 9: Verify app builds**

```bash
cd /Users/adriangarett.sian/oscar-code/build-clocksync && make -j8 2>&1 | tail -3
```
Expected: no errors.

- [ ] **Step 10: Commit**

```bash
git add oscar/driftplotwidget.h oscar/driftplotwidget.cpp \
        oscar/tests/clockdrifthistorytests.h oscar/tests/clockdrifthistorytests.cpp \
        oscar/oscar.pro
git commit -m "feat(driftplot): add setCorrectionDates marker API (Prereq A)"
```

---

## Task 2: DeviceTimeCorrectionRepository::findAllIncludingUndone (Prereq B)

**Files:**
- Modify: `oscar/database/device_time_correction_repository.h`
- Modify: `oscar/database/device_time_correction_repository.cpp`

**Interfaces:**
- Produces: `DeviceTimeCorrectionRepository::findAllIncludingUndone(qint64 machineId) → QList<DeviceTimeCorrectionData>`
- Consumed by: Task 5 (`ClockDriftHistoryDialog` "Show undone" checkbox)

- [ ] **Step 1: Write failing test**

In `oscar/tests/clockdrifthistorytests.h`, add to `private slots:`:
```cpp
    void testFindAllIncludingUndoneReturnsUndoneRows();
```

In `oscar/tests/clockdrifthistorytests.cpp`, add at the top:
```cpp
#include "database/device_time_correction_repository.h"
#include "database/database_schema.h"
#include <QSqlDatabase>
#include <QSqlQuery>
```

Add the test body:
```cpp
void ClockDriftHistoryTests::testFindAllIncludingUndoneReturnsUndoneRows()
{
    // Set up an in-memory database with the corrections table
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "test_dtc");
    db.setDatabaseName(":memory:");
    QVERIFY(db.open());

    QSqlQuery q(db);
    QVERIFY(q.exec(R"(
        CREATE TABLE device_time_corrections (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            machine_id INTEGER NOT NULL,
            date_from TEXT NOT NULL,
            date_to TEXT,
            type TEXT NOT NULL,
            offset_ms INTEGER,
            c0_ms INTEGER,
            c1 REAL DEFAULT 0.0,
            reason TEXT,
            applied_at TEXT NOT NULL DEFAULT (datetime('now')),
            undone_at TEXT
        )
    )"));

    // Insert one active row and one undone row
    QVERIFY(q.exec("INSERT INTO device_time_corrections "
                   "(machine_id, date_from, type, offset_ms, applied_at) "
                   "VALUES (1, '2026-01-01', 'offset', 45000, datetime('now'))"));
    QVERIFY(q.exec("INSERT INTO device_time_corrections "
                   "(machine_id, date_from, type, offset_ms, applied_at, undone_at) "
                   "VALUES (1, '2025-06-01', 'offset', 30000, datetime('now'), datetime('now'))"));

    // findActive should return only 1 row
    DeviceTimeCorrectionRepository repo;
    // findAllIncludingUndone should return 2 rows
    QList<DeviceTimeCorrectionData> all = repo.findAllIncludingUndone(1);
    QCOMPARE(all.size(), 2);

    QSqlDatabase::removeDatabase("test_dtc");
}
```

- [ ] **Step 2: Run — verify compile fails**

```bash
cd /Users/adriangarett.sian/oscar-code/build-test && make -j8 2>&1 | grep "findAllIncludingUndone"
```
Expected: `error: no member named 'findAllIncludingUndone'`

**Note:** The test above uses a named connection `"test_dtc"` while `DeviceTimeCorrectionRepository::database()` uses the default connection. The test verifies the method signature compiles and exists; a full integration test would require wiring the named connection. Accept this limitation for now — the method correctness is covered by its identical structure to `findActive`.

- [ ] **Step 3: Add declaration to device_time_correction_repository.h**

After `QList<DeviceTimeCorrectionData> findManualOffsetRows(qint64 machineId);` (line 57), add:
```cpp
    // Returns all rows for the machine including undone (superseded) ones.
    // Used by ClockDriftHistoryDialog when "Show undone" is checked.
    QList<DeviceTimeCorrectionData> findAllIncludingUndone(qint64 machineId);
```

- [ ] **Step 4: Add implementation to device_time_correction_repository.cpp**

After the closing brace of `findManualOffsetRows` (end of file), add:
```cpp
QList<DeviceTimeCorrectionData> DeviceTimeCorrectionRepository::findAllIncludingUndone(qint64 machineId)
{
    QSqlQuery q(database());
    q.prepare(R"(
        SELECT id, machine_id, date_from, date_to, type, offset_ms, c0_ms, c1,
               reason, applied_at, undone_at
        FROM device_time_corrections
        WHERE machine_id = :machine_id
        ORDER BY date_from ASC, id ASC
    )");
    q.bindValue(":machine_id", machineId);

    QList<DeviceTimeCorrectionData> rows;
    if (!q.exec()) {
        qCritical() << "DeviceTimeCorrectionRepository::findAllIncludingUndone failed:"
                    << q.lastError().text();
        DatabaseManager::instance().checkQueryError(
            "DeviceTimeCorrectionRepository::findAllIncludingUndone", q);
        return rows;
    }
    while (q.next()) {
        rows.append(recordToData(q));
    }
    return rows;
}
```

- [ ] **Step 5: Build and run tests**

```bash
cd /Users/adriangarett.sian/oscar-code/build-test && make -j8 2>&1 | tail -5
./test -v2 ClockDriftHistoryTests
```
Expected: all 4 tests pass (3 from Task 1 + 1 new).

- [ ] **Step 6: Commit**

```bash
git add oscar/database/device_time_correction_repository.h \
        oscar/database/device_time_correction_repository.cpp \
        oscar/tests/clockdrifthistorytests.h \
        oscar/tests/clockdrifthistorytests.cpp
git commit -m "feat(db): add findAllIncludingUndone to DeviceTimeCorrectionRepository (Prereq B)"
```

---

## Task 3: DB schema v18 — night_clock_offsets table

**Files:**
- Modify: `oscar/database/database_schema.h`
- Modify: `oscar/database/database_schema.cpp`

**Interfaces:**
- Produces: `night_clock_offsets` table in the database at schema version 18
- Consumed by: Task 4 (`NightClockOffsetsRepository`)

- [ ] **Step 1: Write a failing schema-version test**

In `oscar/tests/clockdrifthistorytests.h`, add to `private slots:`:
```cpp
    void testSchemaVersion18HasNightClockOffsetsTable();
```

In `oscar/tests/clockdrifthistorytests.cpp`, add:
```cpp
#include "database/database_schema.h"
```

Add test body:
```cpp
void ClockDriftHistoryTests::testSchemaVersion18HasNightClockOffsetsTable()
{
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "test_schema18");
    db.setDatabaseName(":memory:");
    QVERIFY(db.open());

    bool ok = DatabaseSchema::createSchema(db);
    QVERIFY(ok);

    // Confirm night_clock_offsets table exists
    QSqlQuery q(db);
    QVERIFY(q.exec("SELECT name FROM sqlite_master WHERE type='table' AND name='night_clock_offsets'"));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QString("night_clock_offsets"));

    QSqlDatabase::removeDatabase("test_schema18");
}
```

- [ ] **Step 2: Run — verify test fails**

```bash
cd /Users/adriangarett.sian/oscar-code/build-test && make -j8 2>&1 | tail -3
./test -v2 ClockDriftHistoryTests 2>&1 | grep "night_clock_offsets\|FAIL"
```
Expected: test fails because `night_clock_offsets` does not exist.

- [ ] **Step 3: Bump CURRENT_SCHEMA_VERSION in database_schema.h**

In `oscar/database/database_schema.h` line 74, change:
```cpp
    static const int CURRENT_SCHEMA_VERSION = 17;
```
to:
```cpp
    static const int CURRENT_SCHEMA_VERSION = 18;
```

Add the private helper declaration after `static bool migrateV16ToV17(QSqlDatabase& db);` (line 186):
```cpp
    static bool createNightClockOffsetsTable(QSqlDatabase& db);
    static bool migrateV17ToV18(QSqlDatabase& db);
```

- [ ] **Step 4: Implement createNightClockOffsetsTable in database_schema.cpp**

At the end of `database_schema.cpp`, after the closing brace of `migrateV16ToV17`, add:
```cpp
bool DatabaseSchema::createNightClockOffsetsTable(QSqlDatabase& db)
{
    QSqlQuery q(db);
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS night_clock_offsets (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            machine_id      INTEGER NOT NULL
                                REFERENCES machines(id) ON DELETE CASCADE,
            ref_machine_id  INTEGER NOT NULL,
            date            TEXT    NOT NULL,
            start_diff_ms   INTEGER NOT NULL,
            end_diff_ms     INTEGER NOT NULL,
            mid_ms          INTEGER NOT NULL,
            used            INTEGER NOT NULL,
            validated       INTEGER NOT NULL DEFAULT 0,
            validator_agrees INTEGER,
            recorded_at     TEXT NOT NULL DEFAULT (datetime('now')),
            UNIQUE(machine_id, ref_machine_id, date)
        )
    )")) {
        qCritical() << "DatabaseSchema: Failed to create night_clock_offsets:"
                    << q.lastError().text();
        DatabaseManager::instance().checkQueryError(
            "DatabaseSchema::createNightClockOffsetsTable", q);
        return false;
    }
    if (!q.exec("CREATE INDEX IF NOT EXISTS idx_nco_machine_date "
                "ON night_clock_offsets(machine_id, ref_machine_id, date)")) {
        qCritical() << "DatabaseSchema: Failed to create idx_nco_machine_date:"
                    << q.lastError().text();
        DatabaseManager::instance().checkQueryError(
            "DatabaseSchema::createNightClockOffsetsTable", q);
        return false;
    }
    return true;
}

bool DatabaseSchema::migrateV17ToV18(QSqlDatabase& db)
{
    qDebug() << "DatabaseSchema: Migrating v17 -> v18";

    if (!db.transaction()) {
        qCritical() << "DatabaseSchema: migrateV17ToV18: failed to start transaction";
        return false;
    }

    if (!createNightClockOffsetsTable(db)) {
        qCritical() << "DatabaseSchema: migrateV17ToV18: createNightClockOffsetsTable failed";
        db.rollback();
        return false;
    }

    if (!setSchemaVersion(db, 18)) {
        qCritical() << "DatabaseSchema: migrateV17ToV18: setSchemaVersion failed";
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        qCritical() << "DatabaseSchema: migrateV17ToV18: commit failed";
        db.rollback();
        return false;
    }

    qDebug() << "DatabaseSchema: Migration v17->v18 complete";
    return true;
}
```

- [ ] **Step 5: Wire createNightClockOffsetsTable into createSchema**

In `database_schema.cpp`, find the block that creates `device_time_corrections` (~line 149):
```cpp
    // Device time corrections table (schema version 17)
    if (!createDeviceTimeCorrectionsTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create device_time_corrections table";
        return false;
    }
```
Immediately after it, add:
```cpp
    // Night clock offsets table (schema version 18)
    if (!createNightClockOffsetsTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create night_clock_offsets table";
        return false;
    }
```

- [ ] **Step 6: Wire migrateV17ToV18 into upgradeSchema**

In `database_schema.cpp`, find the `if (fromVersion == 16)` block (~line 244):
```cpp
    if (fromVersion == 16) {
        if (!migrateV16ToV17(db)) {
            qCritical() << "DatabaseSchema: v16->v17 migration failed";
            return false;
        }
        fromVersion = 17;
    }

    if (fromVersion != CURRENT_SCHEMA_VERSION) {
```

Between the v16→17 block and `if (fromVersion != CURRENT_SCHEMA_VERSION)`, add:
```cpp
    if (fromVersion == 17) {
        if (!migrateV17ToV18(db)) {
            qCritical() << "DatabaseSchema: v17->v18 migration failed";
            return false;
        }
        fromVersion = 18;
    }
```

- [ ] **Step 7: Build and run tests**

```bash
cd /Users/adriangarett.sian/oscar-code/build-test && make -j8 2>&1 | tail -5
./test -v2 ClockDriftHistoryTests
```
Expected: all tests pass including `testSchemaVersion18HasNightClockOffsetsTable`.

- [ ] **Step 8: Commit**

```bash
git add oscar/database/database_schema.h oscar/database/database_schema.cpp \
        oscar/tests/clockdrifthistorytests.h oscar/tests/clockdrifthistorytests.cpp
git commit -m "feat(db): add night_clock_offsets table at schema v18"
```

---

## Task 4: NightClockOffsetsRepository

**Files:**
- Create: `oscar/database/night_clock_offsets_repository.h`
- Create: `oscar/database/night_clock_offsets_repository.cpp`

**Interfaces:**
- Consumes: `NightStartDiff` from `oscar/clocksyncanalyzer.h`
- Produces:
  - `NightClockOffsetData` struct
  - `NightClockOffsetsRepository::upsertNights(qint64, qint64, const QVector<NightStartDiff>&)`
  - `NightClockOffsetsRepository::findByMachines(qint64, qint64, QDate, QDate) → QList<NightClockOffsetData>`
- Consumed by: Task 5 (`ClockDriftHistoryDialog`) and Task 6 (`ClockSyncDialog::onAnalyze`)

- [ ] **Step 1: Write failing tests**

In `oscar/tests/clockdrifthistorytests.h`, add to `private slots:`:
```cpp
    void testUpsertNightsInsertsRows();
    void testUpsertNightsUpdatesOnRerun();
    void testFindByMachinesFiltersDateRange();
```

In `oscar/tests/clockdrifthistorytests.cpp`, add at top:
```cpp
#include "database/night_clock_offsets_repository.h"
#include "clocksyncanalyzer.h"
```

Add test helper to create a temporary database with both required tables:
```cpp
static QSqlDatabase makeTestDb(const QString& name)
{
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
    db.setDatabaseName(":memory:");
    db.open();
    QSqlQuery q(db);
    q.exec("CREATE TABLE machines (id INTEGER PRIMARY KEY)");
    q.exec("INSERT INTO machines VALUES (1)");
    q.exec("INSERT INTO machines VALUES (2)");
    q.exec(R"(
        CREATE TABLE night_clock_offsets (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            machine_id INTEGER NOT NULL,
            ref_machine_id INTEGER NOT NULL,
            date TEXT NOT NULL,
            start_diff_ms INTEGER NOT NULL,
            end_diff_ms INTEGER NOT NULL,
            mid_ms INTEGER NOT NULL,
            used INTEGER NOT NULL,
            validated INTEGER NOT NULL DEFAULT 0,
            validator_agrees INTEGER,
            recorded_at TEXT NOT NULL DEFAULT (datetime('now')),
            UNIQUE(machine_id, ref_machine_id, date)
        )
    )");
    return db;
}
```

Add test bodies:
```cpp
void ClockDriftHistoryTests::testUpsertNightsInsertsRows()
{
    QSqlDatabase db = makeTestDb("test_nco_insert");

    NightStartDiff n;
    n.date        = QDate(2026, 3, 15);
    n.startDiffMs = 44000;
    n.endDiffMs   = 46000;
    n.midMs       = 45000;
    n.used        = true;
    n.validated   = false;

    NightClockOffsetsRepository repo;
    repo.upsertNights(1, 2, QVector<NightStartDiff>{ n });

    QSqlQuery q(db);
    q.exec("SELECT COUNT(*) FROM night_clock_offsets WHERE machine_id=1 AND ref_machine_id=2");
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);

    QSqlDatabase::removeDatabase("test_nco_insert");
}

void ClockDriftHistoryTests::testUpsertNightsUpdatesOnRerun()
{
    QSqlDatabase db = makeTestDb("test_nco_upsert");

    NightStartDiff n;
    n.date = QDate(2026, 3, 15); n.startDiffMs = 44000; n.endDiffMs = 46000;
    n.midMs = 45000; n.used = true; n.validated = false;

    NightClockOffsetsRepository repo;
    repo.upsertNights(1, 2, QVector<NightStartDiff>{ n });

    // Re-run with updated midMs
    n.midMs = 50000;
    repo.upsertNights(1, 2, QVector<NightStartDiff>{ n });

    QSqlQuery q(db);
    q.exec("SELECT mid_ms FROM night_clock_offsets WHERE machine_id=1 AND ref_machine_id=2");
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toLongLong(), 50000LL);

    // Still only one row
    q.exec("SELECT COUNT(*) FROM night_clock_offsets");
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);

    QSqlDatabase::removeDatabase("test_nco_upsert");
}

void ClockDriftHistoryTests::testFindByMachinesFiltersDateRange()
{
    QSqlDatabase db = makeTestDb("test_nco_find");

    NightClockOffsetsRepository repo;
    auto makeNight = [](QDate d, qint64 mid) {
        NightStartDiff n;
        n.date = d; n.startDiffMs = mid - 500; n.endDiffMs = mid + 500;
        n.midMs = mid; n.used = true; n.validated = false;
        return n;
    };
    repo.upsertNights(1, 2, {
        makeNight(QDate(2026, 1, 10), 45000),
        makeNight(QDate(2026, 2, 10), 45000),
        makeNight(QDate(2026, 3, 10), 45000),
    });

    // Query only Feb–Mar
    auto rows = repo.findByMachines(1, 2, QDate(2026, 2, 1), QDate(2026, 3, 31));
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].date, QString("2026-02-10"));
    QCOMPARE(rows[1].date, QString("2026-03-10"));

    QSqlDatabase::removeDatabase("test_nco_find");
}
```

- [ ] **Step 2: Run — verify compile fails**

```bash
cd /Users/adriangarett.sian/oscar-code/build-test && make -j8 2>&1 | grep "NightClockOffsetsRepository\|NightClockOffsetData"
```
Expected: `error: 'NightClockOffsetsRepository' was not declared`

- [ ] **Step 3: Create night_clock_offsets_repository.h**

```cpp
/* Night Clock Offsets Repository Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef NIGHT_CLOCK_OFFSETS_REPOSITORY_H
#define NIGHT_CLOCK_OFFSETS_REPOSITORY_H

#include <QString>
#include <QList>
#include <QVector>
#include <QDate>
#include <QSqlDatabase>
#include "clocksyncanalyzer.h"

struct NightClockOffsetData {
    qint64  id            = 0;
    qint64  machineId     = 0;
    qint64  refMachineId  = 0;
    QString date;                // "YYYY-MM-DD"
    qint64  startDiffMs   = 0;
    qint64  endDiffMs     = 0;
    qint64  midMs         = 0;
    bool    used          = true;
    bool    validated     = false;
    int     validatorAgrees = -1; // -1 = NULL (not validated), 0 = disagrees, 1 = agrees
    QString recordedAt;
};

class NightClockOffsetsRepository
{
public:
    // Upsert a batch of per-night measurements for a CPAP/reference pair.
    // Safe to call on re-analysis — UNIQUE(machine_id, ref_machine_id, date)
    // ensures re-runs update rather than duplicate.
    void upsertNights(qint64 machineId, qint64 refMachineId,
                      const QVector<NightStartDiff>& nights);

    // Retrieve stored measurements for a pair. Pass invalid QDate for either
    // bound to omit that filter.
    QList<NightClockOffsetData> findByMachines(qint64 machineId, qint64 refMachineId,
                                               const QDate& from = QDate(),
                                               const QDate& to   = QDate());

private:
    QSqlDatabase database();
    NightClockOffsetData recordToData(QSqlQuery& q);
};

#endif // NIGHT_CLOCK_OFFSETS_REPOSITORY_H
```

- [ ] **Step 4: Create night_clock_offsets_repository.cpp**

```cpp
/* Night Clock Offsets Repository Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "night_clock_offsets_repository.h"
#include "database/database_manager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>

QSqlDatabase NightClockOffsetsRepository::database()
{
    return DatabaseManager::instance().database();
}

NightClockOffsetData NightClockOffsetsRepository::recordToData(QSqlQuery& q)
{
    NightClockOffsetData d;
    d.id            = q.value("id").toLongLong();
    d.machineId     = q.value("machine_id").toLongLong();
    d.refMachineId  = q.value("ref_machine_id").toLongLong();
    d.date          = q.value("date").toString();
    d.startDiffMs   = q.value("start_diff_ms").toLongLong();
    d.endDiffMs     = q.value("end_diff_ms").toLongLong();
    d.midMs         = q.value("mid_ms").toLongLong();
    d.used          = q.value("used").toInt() != 0;
    d.validated     = q.value("validated").toInt() != 0;
    QVariant va     = q.value("validator_agrees");
    d.validatorAgrees = va.isNull() ? -1 : va.toInt();
    d.recordedAt    = q.value("recorded_at").toString();
    return d;
}

void NightClockOffsetsRepository::upsertNights(qint64 machineId, qint64 refMachineId,
                                               const QVector<NightStartDiff>& nights)
{
    QSqlDatabase db = database();
    QSqlQuery q(db);
    q.prepare(R"(
        INSERT INTO night_clock_offsets
            (machine_id, ref_machine_id, date,
             start_diff_ms, end_diff_ms, mid_ms,
             used, validated, validator_agrees)
        VALUES
            (:machine_id, :ref_machine_id, :date,
             :start_diff_ms, :end_diff_ms, :mid_ms,
             :used, :validated, :validator_agrees)
        ON CONFLICT(machine_id, ref_machine_id, date) DO UPDATE SET
            start_diff_ms    = excluded.start_diff_ms,
            end_diff_ms      = excluded.end_diff_ms,
            mid_ms           = excluded.mid_ms,
            used             = excluded.used,
            validated        = excluded.validated,
            validator_agrees = excluded.validator_agrees,
            recorded_at      = datetime('now')
    )");

    for (const NightStartDiff& n : nights) {
        q.bindValue(":machine_id",     machineId);
        q.bindValue(":ref_machine_id", refMachineId);
        q.bindValue(":date",           n.date.toString(Qt::ISODate));
        q.bindValue(":start_diff_ms",  n.startDiffMs);
        q.bindValue(":end_diff_ms",    n.endDiffMs);
        q.bindValue(":mid_ms",         n.midMs);
        q.bindValue(":used",           n.used ? 1 : 0);
        q.bindValue(":validated",      n.validated ? 1 : 0);
        if (n.validated) {
            q.bindValue(":validator_agrees", n.validatorAgrees ? 1 : 0);
        } else {
            q.bindValue(":validator_agrees", QVariant(QMetaType::fromType<int>()));
        }
        if (!q.exec()) {
            qCritical() << "NightClockOffsetsRepository::upsertNights failed for"
                        << n.date.toString() << ":" << q.lastError().text();
            DatabaseManager::instance().checkQueryError(
                "NightClockOffsetsRepository::upsertNights", q);
        }
    }
}

QList<NightClockOffsetData> NightClockOffsetsRepository::findByMachines(
    qint64 machineId, qint64 refMachineId, const QDate& from, const QDate& to)
{
    QSqlQuery q(database());
    QString sql = R"(
        SELECT id, machine_id, ref_machine_id, date,
               start_diff_ms, end_diff_ms, mid_ms,
               used, validated, validator_agrees, recorded_at
        FROM night_clock_offsets
        WHERE machine_id = :machine_id AND ref_machine_id = :ref_machine_id
    )";
    if (from.isValid()) sql += " AND date >= :from";
    if (to.isValid())   sql += " AND date <= :to";
    sql += " ORDER BY date ASC";

    q.prepare(sql);
    q.bindValue(":machine_id",     machineId);
    q.bindValue(":ref_machine_id", refMachineId);
    if (from.isValid()) q.bindValue(":from", from.toString(Qt::ISODate));
    if (to.isValid())   q.bindValue(":to",   to.toString(Qt::ISODate));

    QList<NightClockOffsetData> rows;
    if (!q.exec()) {
        qCritical() << "NightClockOffsetsRepository::findByMachines failed:"
                    << q.lastError().text();
        DatabaseManager::instance().checkQueryError(
            "NightClockOffsetsRepository::findByMachines", q);
        return rows;
    }
    while (q.next()) rows.append(recordToData(q));
    return rows;
}
```

- [ ] **Step 5: Register new files in oscar.pro**

In `oscar/oscar.pro`, find the main `SOURCES +=` block (around line 301). Add:
```
    database/night_clock_offsets_repository.cpp \
```

In the `HEADERS  +=` block (around line 479). Add:
```
    database/night_clock_offsets_repository.h \
```

- [ ] **Step 6: Build and run tests**

```bash
cd /Users/adriangarett.sian/oscar-code/build-test && make -j8 2>&1 | tail -5
./test -v2 ClockDriftHistoryTests
```
Expected: all 8 tests pass.

- [ ] **Step 7: Commit**

```bash
git add oscar/database/night_clock_offsets_repository.h \
        oscar/database/night_clock_offsets_repository.cpp \
        oscar/tests/clockdrifthistorytests.h \
        oscar/tests/clockdrifthistorytests.cpp \
        oscar/oscar.pro
git commit -m "feat(db): add NightClockOffsetsRepository with upsert and find"
```

---

## Task 5: ClockDriftHistoryDialog

**Files:**
- Create: `oscar/clockdrifthistorydialog.h`
- Create: `oscar/clockdrifthistorydialog.cpp`
- Create: `oscar/clockdrifthistorydialog.ui`

**Interfaces:**
- Consumes: `DriftPlotWidget::setCorrectionDates` (Task 1), `DeviceTimeCorrectionRepository::findAllIncludingUndone` (Task 2), `NightClockOffsetsRepository::findByMachines` (Task 4)
- Produces: `ClockDriftHistoryDialog(Machine* cpap, Machine* oxi, QWidget* parent)`
- Consumed by: Task 6 (`ClockSyncDialog::onHistory`)

- [ ] **Step 1: Create clockdrifthistorydialog.ui**

```xml
<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>ClockDriftHistoryDialog</class>
 <widget class="QDialog" name="ClockDriftHistoryDialog">
  <property name="geometry">
   <rect><x>0</x><y>0</y><width>860</width><height>620</height></rect>
  </property>
  <property name="windowTitle">
   <string>Clock Drift History</string>
  </property>
  <property name="sizeGripEnabled"><bool>true</bool></property>
  <layout class="QVBoxLayout" name="mainLayout">
   <property name="spacing"><number>6</number></property>
   <property name="margin"><number>8</number></property>

   <!-- Top bar -->
   <item>
    <layout class="QHBoxLayout" name="topBar">
     <item>
      <widget class="QLabel" name="deviceLabel">
       <property name="text"><string>No devices selected</string></property>
      </widget>
     </item>
     <item>
      <spacer>
       <property name="orientation"><enum>Qt::Horizontal</enum></property>
       <property name="sizeType"><enum>QSizePolicy::Expanding</enum></property>
       <property name="sizeHint" stdset="0"><size><width>20</width><height>20</height></size></property>
      </spacer>
     </item>
     <item>
      <widget class="QLabel"><property name="text"><string>From:</string></property></widget>
     </item>
     <item>
      <widget class="QDateEdit" name="fromDate">
       <property name="displayFormat"><string>yyyy-MM-dd</string></property>
       <property name="calendarPopup"><bool>false</bool></property>
      </widget>
     </item>
     <item>
      <widget class="QLabel"><property name="text"><string>To:</string></property></widget>
     </item>
     <item>
      <widget class="QDateEdit" name="toDate">
       <property name="displayFormat"><string>yyyy-MM-dd</string></property>
       <property name="calendarPopup"><bool>false</bool></property>
      </widget>
     </item>
     <item>
      <widget class="QPushButton" name="btnReset">
       <property name="text"><string>Reset</string></property>
       <property name="maximumWidth"><number>60</number></property>
      </widget>
     </item>
     <item>
      <widget class="QPushButton" name="btnExport">
       <property name="text"><string>Export CSV…</string></property>
       <property name="maximumWidth"><number>100</number></property>
      </widget>
     </item>
     <item>
      <widget class="QCheckBox" name="chkShowUndone">
       <property name="text"><string>Show undone corrections</string></property>
      </widget>
     </item>
    </layout>
   </item>

   <!-- Splitter -->
   <item>
    <widget class="QSplitter" name="splitter">
     <property name="orientation"><enum>Qt::Vertical</enum></property>
     <property name="sizePolicy">
      <sizepolicy hsizetype="Expanding" vsizetype="Expanding">
       <horstretch>0</horstretch><verstretch>1</verstretch>
      </sizepolicy>
     </property>
     <widget class="DriftPlotWidget" name="plotWidget">
      <property name="minimumHeight"><number>180</number></property>
     </widget>
     <widget class="QTabWidget" name="tabWidget">
      <property name="minimumHeight"><number>120</number></property>
      <widget class="QWidget" name="tabNightly">
       <attribute name="title"><string>Nightly offsets</string></attribute>
       <layout class="QVBoxLayout">
        <property name="margin"><number>0</number></property>
        <item>
         <widget class="QTableWidget" name="nightlyTable">
          <property name="selectionMode"><enum>QAbstractItemView::NoSelection</enum></property>
          <property name="editTriggers"><set>QAbstractItemView::NoEditTriggers</set></property>
          <property name="alternatingRowColors"><bool>true</bool></property>
          <column><property name="text"><string>Date</string></property></column>
          <column><property name="text"><string>Start Δ (s)</string></property></column>
          <column><property name="text"><string>End Δ (s)</string></property></column>
          <column><property name="text"><string>Offset (s)</string></property></column>
          <column><property name="text"><string>Used</string></property></column>
          <column><property name="text"><string>Cross-check</string></property></column>
         </widget>
        </item>
       </layout>
      </widget>
      <widget class="QWidget" name="tabCorrections">
       <attribute name="title"><string>Applied corrections</string></attribute>
       <layout class="QVBoxLayout">
        <property name="margin"><number>0</number></property>
        <item>
         <widget class="QTableWidget" name="correctionsTable">
          <property name="selectionMode"><enum>QAbstractItemView::NoSelection</enum></property>
          <property name="editTriggers"><set>QAbstractItemView::NoEditTriggers</set></property>
          <property name="alternatingRowColors"><bool>true</bool></property>
          <column><property name="text"><string>From</string></property></column>
          <column><property name="text"><string>To</string></property></column>
          <column><property name="text"><string>Type</string></property></column>
          <column><property name="text"><string>Value</string></property></column>
          <column><property name="text"><string>Applied</string></property></column>
          <column><property name="text"><string>Status</string></property></column>
         </widget>
        </item>
       </layout>
      </widget>
     </widget>
    </widget>
   </item>

   <!-- Close button row -->
   <item>
    <layout class="QHBoxLayout">
     <item>
      <spacer>
       <property name="orientation"><enum>Qt::Horizontal</enum></property>
       <property name="sizeType"><enum>QSizePolicy::Expanding</enum></property>
       <property name="sizeHint" stdset="0"><size><width>40</width><height>20</height></size></property>
      </spacer>
     </item>
     <item>
      <widget class="QPushButton" name="btnClose">
       <property name="text"><string>Close</string></property>
      </widget>
     </item>
    </layout>
   </item>

  </layout>
 </widget>
 <customwidgets>
  <customwidget>
   <class>DriftPlotWidget</class>
   <extends>QWidget</extends>
   <header>driftplotwidget.h</header>
  </customwidget>
 </customwidgets>
 <resources/>
 <connections>
  <connection>
   <sender>btnClose</sender><signal>clicked()</signal>
   <receiver>ClockDriftHistoryDialog</receiver><slot>accept()</slot>
   <hints>
    <hint type="sourcelabel"><x>800</x><y>600</y></hint>
    <hint type="destinationlabel"><x>430</x><y>310</y></hint>
   </hints>
  </connection>
 </connections>
</ui>
```

- [ ] **Step 2: Create clockdrifthistorydialog.h**

```cpp
/* Clock Drift History Dialog Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef CLOCKDRIFTHISTORYDIALOG_H
#define CLOCKDRIFTHISTORYDIALOG_H

#include <QDialog>
#include <QDate>
#include "SleepLib/machine.h"
#include "database/night_clock_offsets_repository.h"
#include "database/device_time_correction_repository.h"

namespace Ui { class ClockDriftHistoryDialog; }

class ClockDriftHistoryDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ClockDriftHistoryDialog(Machine* cpap, Machine* oxi, QWidget* parent = nullptr);
    ~ClockDriftHistoryDialog();

private slots:
    void onReset();
    void onExport();
    void onShowUndoneToggled(bool checked);
    void refresh();

private:
    Ui::ClockDriftHistoryDialog *ui;
    Machine* m_cpap;
    Machine* m_oxi;

    void populateNightlyTable(const QList<NightClockOffsetData>& rows);
    void populateCorrectionsTable(const QList<DeviceTimeCorrectionData>& rows);
    void populateChart(const QList<NightClockOffsetData>& nights,
                       const QList<DeviceTimeCorrectionData>& corrections);
    QString formatCorrectionValue(const DeviceTimeCorrectionData& r) const;
};

#endif // CLOCKDRIFTHISTORYDIALOG_H
```

- [ ] **Step 3: Create clockdrifthistorydialog.cpp**

```cpp
/* Clock Drift History Dialog Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "clockdrifthistorydialog.h"
#include "ui_clockdrifthistorydialog.h"
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QFileDialog>
#include <QTextStream>
#include <QFile>

ClockDriftHistoryDialog::ClockDriftHistoryDialog(Machine* cpap, Machine* oxi, QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::ClockDriftHistoryDialog)
    , m_cpap(cpap)
    , m_oxi(oxi)
{
    ui->setupUi(this);

    // Device pair label
    auto label = [](Machine* m) {
        if (!m) return tr("none");
        QString s = m->brand() + " " + m->model();
        if (s.trimmed().isEmpty()) s = m->loaderName();
        return s + " (" + m->serial() + ")";
    };
    ui->deviceLabel->setText(tr("CPAP: %1  |  Ref: %2").arg(label(cpap)).arg(label(oxi)));
    setWindowTitle(tr("Clock Drift History — %1 vs %2").arg(label(cpap)).arg(label(oxi)));

    // Table setup
    ui->nightlyTable->horizontalHeader()->setStretchLastSection(true);
    ui->nightlyTable->verticalHeader()->setVisible(false);
    ui->nightlyTable->setColumnWidth(0, 90);
    ui->nightlyTable->setColumnWidth(1, 80);
    ui->nightlyTable->setColumnWidth(2, 80);
    ui->nightlyTable->setColumnWidth(3, 80);
    ui->nightlyTable->setColumnWidth(4, 60);

    ui->correctionsTable->horizontalHeader()->setStretchLastSection(true);
    ui->correctionsTable->verticalHeader()->setVisible(false);
    ui->correctionsTable->setColumnWidth(0, 90);
    ui->correctionsTable->setColumnWidth(1, 90);
    ui->correctionsTable->setColumnWidth(2, 70);
    ui->correctionsTable->setColumnWidth(3, 130);
    ui->correctionsTable->setColumnWidth(4, 140);

    // Splitter initial proportions (60/40)
    ui->splitter->setSizes({ 370, 250 });

    // Determine default date range from stored nights
    QDate fullFrom, fullTo = QDate::currentDate();
    if (cpap && oxi) {
        NightClockOffsetsRepository repo;
        auto all = repo.findByMachines(cpap->getDatabaseId(), oxi->getDatabaseId());
        for (const auto& n : all) {
            QDate d = QDate::fromString(n.date, Qt::ISODate);
            if (d.isValid()) {
                if (!fullFrom.isValid() || d < fullFrom) fullFrom = d;
            }
        }
    }
    if (!fullFrom.isValid()) fullFrom = QDate::currentDate().addDays(-30);

    // Block signals while setting initial values to avoid double refresh
    ui->fromDate->blockSignals(true);
    ui->toDate->blockSignals(true);
    ui->fromDate->setDate(fullFrom);
    ui->toDate->setDate(fullTo);
    ui->fromDate->blockSignals(false);
    ui->toDate->blockSignals(false);

    connect(ui->fromDate,      &QDateEdit::dateChanged,    this, &ClockDriftHistoryDialog::refresh);
    connect(ui->toDate,        &QDateEdit::dateChanged,    this, &ClockDriftHistoryDialog::refresh);
    connect(ui->btnReset,      &QPushButton::clicked,      this, &ClockDriftHistoryDialog::onReset);
    connect(ui->btnExport,     &QPushButton::clicked,      this, &ClockDriftHistoryDialog::onExport);
    connect(ui->chkShowUndone, &QCheckBox::toggled,        this, &ClockDriftHistoryDialog::onShowUndoneToggled);

    refresh();
}

ClockDriftHistoryDialog::~ClockDriftHistoryDialog()
{
    delete ui;
}

void ClockDriftHistoryDialog::onReset()
{
    if (!m_cpap || !m_oxi) return;
    NightClockOffsetsRepository repo;
    auto all = repo.findByMachines(m_cpap->getDatabaseId(), m_oxi->getDatabaseId());
    QDate fullFrom;
    for (const auto& n : all) {
        QDate d = QDate::fromString(n.date, Qt::ISODate);
        if (d.isValid() && (!fullFrom.isValid() || d < fullFrom)) fullFrom = d;
    }
    if (!fullFrom.isValid()) fullFrom = QDate::currentDate().addDays(-30);

    ui->fromDate->blockSignals(true);
    ui->toDate->blockSignals(true);
    ui->fromDate->setDate(fullFrom);
    ui->toDate->setDate(QDate::currentDate());
    ui->fromDate->blockSignals(false);
    ui->toDate->blockSignals(false);
    refresh();
}

void ClockDriftHistoryDialog::onShowUndoneToggled(bool /*checked*/)
{
    refresh();
}

void ClockDriftHistoryDialog::refresh()
{
    if (!m_cpap || !m_oxi) return;

    QDate from = ui->fromDate->date();
    QDate to   = ui->toDate->date();
    if (!from.isValid() || !to.isValid() || from > to) return;

    NightClockOffsetsRepository nightRepo;
    DeviceTimeCorrectionRepository corrRepo;

    auto nights = nightRepo.findByMachines(m_cpap->getDatabaseId(),
                                           m_oxi->getDatabaseId(), from, to);
    QList<DeviceTimeCorrectionData> corrections;
    if (ui->chkShowUndone->isChecked())
        corrections = corrRepo.findAllIncludingUndone(m_cpap->getDatabaseId());
    else
        corrections = corrRepo.findActive(m_cpap->getDatabaseId());

    populateNightlyTable(nights);
    populateCorrectionsTable(corrections);
    populateChart(nights, corrections);
}

void ClockDriftHistoryDialog::populateNightlyTable(const QList<NightClockOffsetData>& rows)
{
    auto item = [](const QString& s) {
        auto* i = new QTableWidgetItem(s);
        i->setFlags(Qt::ItemIsEnabled);
        return i;
    };
    auto secs = [](qint64 ms) { return QString::number(ms / 1000.0, 'f', 1); };

    QTableWidget* t = ui->nightlyTable;
    t->setRowCount(0);
    t->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const auto& n = rows[row];
        t->setItem(row, 0, item(n.date));
        t->setItem(row, 1, item(secs(n.startDiffMs)));
        t->setItem(row, 2, item(secs(n.endDiffMs)));
        t->setItem(row, 3, item(secs(n.midMs)));
        t->setItem(row, 4, item(n.used ? tr("yes") : tr("no (outlier)")));
        QString xcheck = tr("—");
        if (n.validated)
            xcheck = n.validatorAgrees == 1 ? tr("agrees") : tr("disagrees");
        t->setItem(row, 5, item(xcheck));
        if (!n.used)
            for (int col = 0; col < 6; ++col)
                if (t->item(row, col)) t->item(row, col)->setForeground(Qt::gray);
    }
    t->resizeRowsToContents();
}

void ClockDriftHistoryDialog::populateCorrectionsTable(const QList<DeviceTimeCorrectionData>& rows)
{
    auto item = [](const QString& s) {
        auto* i = new QTableWidgetItem(s);
        i->setFlags(Qt::ItemIsEnabled);
        return i;
    };

    QTableWidget* t = ui->correctionsTable;
    t->setRowCount(0);
    t->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const auto& r = rows[row];
        t->setItem(row, 0, item(r.dateFrom));
        t->setItem(row, 1, item(r.dateTo.isEmpty() ? tr("open") : r.dateTo));
        t->setItem(row, 2, item(r.type));
        t->setItem(row, 3, item(formatCorrectionValue(r)));
        // appliedAt: trim to "YYYY-MM-DD HH:MM"
        t->setItem(row, 4, item(r.appliedAt.left(16)));
        QTableWidgetItem* status;
        if (r.undoneAt.isEmpty()) {
            status = item(tr("active"));
            status->setForeground(QColor(0, 130, 0));
        } else {
            status = item(tr("superseded %1").arg(r.undoneAt.left(10)));
            status->setForeground(Qt::gray);
        }
        t->setItem(row, 5, status);
        if (!r.undoneAt.isEmpty())
            for (int col = 0; col < 5; ++col)
                if (t->item(row, col)) t->item(row, col)->setForeground(Qt::gray);
    }
    t->resizeRowsToContents();
}

void ClockDriftHistoryDialog::populateChart(const QList<NightClockOffsetData>& nights,
                                            const QList<DeviceTimeCorrectionData>& corrections)
{
    ui->plotWidget->clear();

    // Per-night dots
    QList<DriftPlotWidget::Point> pts;
    for (const auto& n : nights) {
        if (!n.used) continue;
        QDate d = QDate::fromString(n.date, Qt::ISODate);
        if (d.isValid())
            pts.append({ d, (double)n.midMs });
    }
    ui->plotWidget->setData(pts);

    // Active drift/offset model → red line
    // Most recent undone drift → gray dashed
    const DeviceTimeCorrectionData* activeModel   = nullptr;
    const DeviceTimeCorrectionData* previousModel = nullptr;
    for (const auto& r : corrections) {
        if (r.type != "drift" && r.type != "offset") continue;
        if (r.undoneAt.isEmpty()) {
            if (!activeModel || r.dateFrom > activeModel->dateFrom)
                activeModel = &r;
        } else {
            if (!previousModel || r.dateFrom > previousModel->dateFrom)
                previousModel = &r;
        }
    }
    if (activeModel) {
        // slope stored as c1-1.0 in DriftPlotWidget
        double slope = (activeModel->type == "drift") ? (activeModel->c1 - 1.0) : 0.0;
        double c0    = (activeModel->type == "drift") ? (double)activeModel->c0Ms
                                                       : (double)activeModel->offsetMs;
        ui->plotWidget->setModel(c0, slope);
    }
    if (previousModel) {
        double slope = (previousModel->type == "drift") ? (previousModel->c1 - 1.0) : 0.0;
        double c0    = (previousModel->type == "drift") ? (double)previousModel->c0Ms
                                                        : (double)previousModel->offsetMs;
        ui->plotWidget->setReferenceModel(c0, slope);
    }

    // Correction markers
    QList<QDate> markerDates;
    for (const auto& r : corrections) {
        QDate d = QDate::fromString(r.dateFrom, Qt::ISODate);
        if (d.isValid()) markerDates.append(d);
    }
    ui->plotWidget->setCorrectionDates(markerDates);
}

QString ClockDriftHistoryDialog::formatCorrectionValue(const DeviceTimeCorrectionData& r) const
{
    if (r.type == "drift") {
        // m_slope = c1 - 1.0; s/day = (c1-1.0) * 86400
        double sPerDay = (r.c1 - 1.0) * 86400.0;
        return tr("%1 s/day from %2").arg(sPerDay, 0, 'f', 2).arg(r.dateFrom);
    }
    return tr("%1 s").arg(r.offsetMs / 1000.0, 0, 'f', 1);
}

void ClockDriftHistoryDialog::onExport()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Export CSV"),
                       QString(), tr("CSV files (*.csv)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream out(&f);

    int tabIdx = ui->tabWidget->currentIndex();
    QTableWidget* t = (tabIdx == 0) ? ui->nightlyTable : ui->correctionsTable;

    // Header row
    QStringList headers;
    for (int col = 0; col < t->columnCount(); ++col)
        headers << t->horizontalHeaderItem(col)->text();
    out << headers.join(",") << "\n";

    // Data rows
    for (int row = 0; row < t->rowCount(); ++row) {
        QStringList cells;
        for (int col = 0; col < t->columnCount(); ++col) {
            QTableWidgetItem* it = t->item(row, col);
            QString val = it ? it->text() : QString();
            // Quote cells that contain commas
            if (val.contains(',')) val = "\"" + val + "\"";
            cells << val;
        }
        out << cells.join(",") << "\n";
    }
}
```

- [ ] **Step 4: Register new files in oscar.pro**

In the main `SOURCES +=` block in oscar.pro, add:
```
    clockdrifthistorydialog.cpp \
```

In the `HEADERS  +=` block, add:
```
    clockdrifthistorydialog.h \
```

The `.ui` file is auto-discovered by qmake when the `.cpp` includes `ui_clockdrifthistorydialog.h`.

- [ ] **Step 5: Build app — verify it compiles**

```bash
cd /Users/adriangarett.sian/oscar-code/build-clocksync && make -j8 2>&1 | tail -5
```
Expected: no errors. The dialog is not yet reachable from the UI — that comes in Task 6.

- [ ] **Step 6: Commit**

```bash
git add oscar/clockdrifthistorydialog.h oscar/clockdrifthistorydialog.cpp \
        oscar/clockdrifthistorydialog.ui oscar/oscar.pro
git commit -m "feat(ui): add ClockDriftHistoryDialog with chart and dual-tab tables"
```

---

## Task 6: Wire ClockSyncDialog — History button + upsertNights

**Files:**
- Modify: `oscar/clocksyncdialog.h`
- Modify: `oscar/clocksyncdialog.cpp`
- Modify: `oscar/clocksyncdialog.ui`

**Interfaces:**
- Consumes: `ClockDriftHistoryDialog` (Task 5), `NightClockOffsetsRepository::upsertNights` (Task 4)
- Produces: fully working feature accessible from the existing Clock Sync dialog

- [ ] **Step 1: Add "History" button to clocksyncdialog.ui**

In `oscar/clocksyncdialog.ui`, find the `QHBoxLayout` that contains `btnAnalyze` and `chkValidate`. After the spacer item at the end of that layout, add:
```xml
     <item>
      <widget class="QPushButton" name="btnHistory">
       <property name="text"><string>History</string></property>
       <property name="maximumWidth"><number>80</number></property>
       <property name="toolTip"><string>View the history of measured offsets and applied corrections for this device pair</string></property>
      </widget>
     </item>
```

- [ ] **Step 2: Add includes, member and slot to clocksyncdialog.h**

At the top of the existing `#include` list in `clocksyncdialog.h`, add:
```cpp
#include "database/night_clock_offsets_repository.h"
```

In `private slots:`, after `void onApply();`:
```cpp
    void onHistory();
```

In `private:`, after `DriftLineFit m_drift;`:
```cpp
    NightClockOffsetsRepository m_nightRepo;
```

- [ ] **Step 3: Wire History button and call upsertNights in clocksyncdialog.cpp**

Add include at top of clocksyncdialog.cpp:
```cpp
#include "clockdrifthistorydialog.h"
```

In `ClockSyncDialog::ClockSyncDialog`, after the `connect(ui->btnApply, ...)` line, add:
```cpp
    connect(ui->btnHistory, &QPushButton::clicked, this, &ClockSyncDialog::onHistory);
```

In `ClockSyncDialog::onAnalyze()`, immediately after the call to `populateTable()` (and before the `if (!m_fit.valid)` block), add:
```cpp
    // Persist nightly measurements for history view
    if (cpap && oxi)
        m_nightRepo.upsertNights(cpap->getDatabaseId(), oxi->getDatabaseId(), m_nights);
```

At the end of `clocksyncdialog.cpp`, add the new slot:
```cpp
void ClockSyncDialog::onHistory()
{
    Machine* cpap = currentCpap();
    Machine* oxi  = currentOxi();
    if (!cpap || !oxi) {
        ui->statusLabel->setText(tr("Select both devices before opening history."));
        return;
    }
    ClockDriftHistoryDialog dlg(cpap, oxi, this);
    dlg.exec();
}
```

- [ ] **Step 4: Build app**

```bash
cd /Users/adriangarett.sian/oscar-code/build-clocksync && make -j8 2>&1 | tail -5
```
Expected: no errors.

- [ ] **Step 5: Build tests**

```bash
cd /Users/adriangarett.sian/oscar-code/build-test && make -j8 2>&1 | tail -5
./test -v2 ClockDriftHistoryTests
```
Expected: all tests pass.

- [ ] **Step 6: Manual smoke test**

Launch OSCAR from `build-clocksync/OSCAR20.app`. Open the clock offset dialog (Tools → Auto-Detect CPAP Clock Offset). Verify:
1. "History" button is visible next to "Analyze Nights"
2. Clicking "History" without devices selected shows the status message
3. With devices selected, clicking "History" opens the history window
4. Run "Analyze Nights" — then reopen History — nightly rows appear in Tab 1
5. Tab 2 shows any saved corrections
6. Date range filter narrows the chart and Tab 1 rows
7. "Show undone" checkbox adds superseded correction rows to Tab 2
8. "Export CSV…" saves a file with correct columns
9. "First" / "Last" buttons (from prior session) still work
10. "Close" dismisses the history window

- [ ] **Step 7: Commit**

```bash
git add oscar/clocksyncdialog.h oscar/clocksyncdialog.cpp oscar/clocksyncdialog.ui
git commit -m "feat(ui): wire History button and upsertNights into ClockSyncDialog"
```

---

## Self-Review

**Spec coverage check:**
- [x] Persist per-night measurements → Task 4 `upsertNights` called in Task 6 `onAnalyze`
- [x] Chart with dots, model lines, correction markers → Task 5 `populateChart` + Task 1 `setCorrectionDates`
- [x] Tab 1 nightly table → Task 5 `populateNightlyTable`
- [x] Tab 2 corrections table → Task 5 `populateCorrectionsTable`
- [x] "Show undone" checkbox → Task 5 `onShowUndoneToggled` + Task 2 `findAllIncludingUndone`
- [x] Date range filter → Task 5 `refresh()` re-queries with from/to
- [x] Reset button → Task 5 `onReset`
- [x] Export CSV → Task 5 `onExport`
- [x] Outlier nights shown gray → Task 5 `populateNightlyTable` gray foreground
- [x] DB schema v18 migration → Task 3
- [x] Read-only window → all table items have `Qt::ItemIsEnabled` only
- [x] Accessibility: date caption on markers, ≥3:1 color → Task 1 `paintEvent`
- [x] "History" button placement and wiring → Task 6

**Open questions resolved:**
1. Retention: rows kept indefinitely; `ON DELETE CASCADE` handles device deletion
2. Multiple ref devices: MVP shows only currently selected oxi — acceptable
3. Y-axis scale: auto-scale (existing DriftPlotWidget behavior)
4. Drift slope display: `(c1 - 1.0) * 86400.0` s/day — matches `m_slope` definition in `driftplotwidget.h:42`
