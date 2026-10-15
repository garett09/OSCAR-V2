# OH/CH Capability Gating and NULL Summary Columns — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show OAHI/CAHI only for machines that have scored an obstructive or central hypopnea, store `NULL` instead of `0` for summary columns that do not apply, and move the database to schema v19.

**Architecture:** A derived, per-machine set of "channels ever reported with count > 0" (`Machine::m_reportedChannels`) is the single source of truth for capability. It is computed on the main thread at load and in a pre-pass in `Machine::Save()` before parallel `SaveTask`s write summary rows. The Daily page, printed report, Statistics page and both summary writers consult it. Summary structs use `std::optional` so the repositories can bind SQL `NULL`. One SQL routine rebuilds `session_summaries` from `session_channels`; the v19 migration and the "capability arrived late" fix-up share it.

**Tech Stack:** C++17, Qt 6.10.2, SQLite via QtSql. No new dependencies.

**Spec:** `Notes/specs/2026-09-14-oh-ch-capability-gating-design.md` (GitLab #261)

**Execution status (2026-09-15):** all ten tasks executed on branch `oh-ch-gating`; the SQL
checks in Tasks 4, 6 and 10 were run against a copy of the test database and pass. Task 6's
`daily_summaries` rule was refined while executing (profile-level NULLs for every profile,
deletion only where a profile's devices disagree on a channel, indexed temp tables); the
final form is recorded in the spec's "As built" section. Awaiting the QtCreator build.

## Global Constraints

- C++17, compiles clean with Qt 6.10.2; no `#if QT_VERSION` guards; cross-platform APIs only.
- Doxygen comments on every new function and member; file headers unchanged (existing files).
- Never touch `oscar/SleepLib/thirdparty`.
- Loader changes are limited to the OH/CH `EventList`s in `bmc_loader.cpp`, `sefam_loader.cpp`, `prisma_loader.cpp`. No other channel or loader changes.
- No new `tr()` strings beyond the one Statistics tooltip; the user runs `lupdate` in batches.
- The developer builds in QtCreator; this session cannot compile. Every task ends with a code-review checklist and, where possible, a Python/sqlite3 check against a **copy** of the test database. Never modify the real test database.
- Do not commit without the user's OK. "Commit" steps are checkpoints to offer, not actions to take.
- Release notes (`Htmldocs/release_notes.html`) are the user's; do not edit.
- Nothing in `Notes/` may contain serial numbers, contributor names or sample-folder names.

Channel ids used in SQL (from `schema.cpp`): ClearAirway 4097 (0x1001), Obstructive 4098, Hypopnea 4099, Apnea 4100, RERA 4102 (0x1006), AllApnea 4112 (0x1010), ObstructiveHypopnea 4113 (0x1011), CentralHypopnea 4114 (0x1012), Pressure 4364 (0x110C), Leak 4360 (0x1108), LeakTotal 4375 (0x1117), OXI_Pulse 6144 (0x1800), OXI_SPO2 6145 (0x1801). `MT_CPAP == 1`.

---

## File map

| File | Responsibility in this change |
|---|---|
| `oscar/SleepLib/machine.h/.cpp` | `m_reportedChannels`, `noteReportedChannels()`, `hasReportedEvents()`, `reportsHypopneaMechanism()`; load-time population; `Save()` pre-pass and flip fix-up |
| `oscar/tests/machinetests.h/.cpp` (new), `oscar/oscar.pro` | QTest for the capability set |
| `oscar/database/session_summaries_repository.h/.cpp` | `std::optional` fields, `bindOptional()`, NULL-aware read, `rebuildFromChannels()` |
| `oscar/database/daily_summary_repository.h/.cpp` | `std::optional` fields, `bindOptional()`, NULL-aware read, NULL rule in `calculateFromDay()` |
| `oscar/SleepLib/session.cpp` | NULL rule in `StoreSummaryToDatabase()`; `value_or(0)` in `restoreCount()` |
| `oscar/database/database_schema.h/.cpp` | v19: slim `migrateV17ToV18`, new `migrateV18ToV19` |
| `oscar/SleepLib/loader_plugins/{bmc,sefam,prisma}_loader.cpp` | OH/CH lists created on first event |
| `oscar/daily.cpp`, `oscar/reports.cpp` | machine gate on OAHI/CAHI (and OHI/CHI) |
| `oscar/statistics.cpp` | row gate and per-cell `"-"` |
| `oscar/docs/system_reports.orf`, `Notes/Database/USEFUL_QUERIES.sql` | NULL-safe sums |
| `Notes/Database/DATABASE_SCHEMA_REFERENCE.md`, `DATABASE_SCHEMA.md`, `Notes/Developer Notes/BUG_FIXES.md` | documentation |

---

### Task 1: Capability set on `Machine`

**Files:**
- Modify: `oscar/SleepLib/machine.h:129-135` (next to `hasChannel()`), `:295-305` (members)
- Modify: `oscar/SleepLib/machine.cpp:1400-1450` (`LoadSessionsFromDatabase()`), after `updateChannels()` (~`:1381`)
- Create: `oscar/tests/machinetests.h`, `oscar/tests/machinetests.cpp`
- Modify: `oscar/oscar.pro:821-845` (test `SOURCES` / `HEADERS`)

**Interfaces:**
- Produces: `bool Machine::hasReportedEvents(ChannelID) const`, `bool Machine::reportsHypopneaMechanism() const`, `void Machine::noteReportedChannels(Session *)`.
- Consumes: `Session::m_cnt` (public `QHash<ChannelID, EventDataType>`), `Session::eventlist` (public `QHash<ChannelID, QVector<EventList *>>`), `EventList::count()`.

- [ ] **Step 1: Write the failing test**

`oscar/tests/machinetests.h`:
```cpp
/* Machine Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "tests/AutoTest.h"

//! \brief Tests for Machine's derived per-channel capability set.
class MachineTests : public QObject
{
    Q_OBJECT
private slots:
    void testReportedChannelsFromEventLists();
    void testReportedChannelsFromCounts();
    void testEmptyListDoesNotReport();
};
DECLARE_TEST(MachineTests)
```

`oscar/tests/machinetests.cpp`:
```cpp
/* Machine Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "machinetests.h"
#include "SleepLib/machine.h"
#include "SleepLib/session.h"

// An import-time session: events live in EventLists, m_cnt is still empty.
void MachineTests::testReportedChannelsFromEventLists()
{
    Machine mach(nullptr, 1);
    Session sess(&mach, 1);
    EventList *ch = sess.AddEventList(CPAP_CentralHypopnea, EVL_Event);
    ch->AddEvent(1000, 0);

    QVERIFY(!mach.reportsHypopneaMechanism());
    mach.noteReportedChannels(&sess);
    QVERIFY(mach.hasReportedEvents(CPAP_CentralHypopnea));
    QVERIFY(!mach.hasReportedEvents(CPAP_ObstructiveHypopnea));
    QVERIFY(mach.reportsHypopneaMechanism());
}

// A database-loaded session: EventLists are empty, m_cnt came from session_channels.
void MachineTests::testReportedChannelsFromCounts()
{
    Machine mach(nullptr, 2);
    Session sess(&mach, 1);
    sess.m_cnt[CPAP_ObstructiveHypopnea] = 3;
    sess.m_cnt[CPAP_RERA] = 0;

    mach.noteReportedChannels(&sess);
    QVERIFY(mach.hasReportedEvents(CPAP_ObstructiveHypopnea));
    QVERIFY(!mach.hasReportedEvents(CPAP_RERA));      // count 0 is not evidence
    QVERIFY(mach.reportsHypopneaMechanism());
}

// The empty lists the OH/CH loaders used to create must not count as evidence.
void MachineTests::testEmptyListDoesNotReport()
{
    Machine mach(nullptr, 3);
    Session sess(&mach, 1);
    sess.AddEventList(CPAP_ObstructiveHypopnea, EVL_Event);
    sess.AddEventList(CPAP_CentralHypopnea, EVL_Event);

    mach.noteReportedChannels(&sess);
    QVERIFY(!mach.reportsHypopneaMechanism());
}
```

`oscar/oscar.pro` test block: add `tests/machinetests.cpp` to `SOURCES` and `tests/machinetests.h` to `HEADERS`, keeping the existing alphabetical-ish order (after `dreemtests`).

- [ ] **Step 2: Confirm it fails**

Build target `test` in QtCreator (`CONFIG+=test`). Expected: compile error — `noteReportedChannels`, `hasReportedEvents`, `reportsHypopneaMechanism` undeclared. (If the test build is not run this session, note that in the task summary; the review checklist below still applies.)

- [ ] **Step 3: Implement**

`machine.h`, after `hasSetting()`:
```cpp
    //! \brief True if any session of this device has ever recorded at least one event
    //! on \p code. Unlike hasChannel(), an empty EventList is not evidence: loaders that
    //! pre-create lists (BMC, SEFAM, prisma) make hasChannel() true for channels the
    //! device has never scored.
    inline bool hasReportedEvents(ChannelID code) const {
        return m_reportedChannels.contains(code);
    }

    //! \brief True once the device has scored an obstructive or central hypopnea, i.e. it
    //! splits hypopneas by mechanism and OAHI/CAHI are meaningful for it. Derived from
    //! the data, never declared by a loader. See Notes/specs/2026-09-14-oh-ch-capability-gating-design.md.
    inline bool reportsHypopneaMechanism() const {
        return hasReportedEvents(CPAP_ObstructiveHypopnea) || hasReportedEvents(CPAP_CentralHypopnea);
    }

    //! \brief Record which of \p sess's channels carry at least one event.
    //! Reads both the in-memory EventLists (an import-time session) and m_cnt (a
    //! session loaded from session_channels). Main thread only: called from
    //! LoadSessionsFromDatabase() and the pre-pass in Save(), never from the
    //! SaveTask workers.
    void noteReportedChannels(Session * sess);
```
Member, next to `m_availableChannels`:
```cpp
    QSet<ChannelID> m_reportedChannels;   //!< channels with count > 0 on some session, ever
```
Add `#include <QSet>` if not already included.

`machine.cpp`, after `updateChannels()`:
```cpp
void Machine::noteReportedChannels(Session * sess)
{
    for (auto it = sess->m_cnt.cbegin(); it != sess->m_cnt.cend(); ++it) {
        if (it.value() > 0) m_reportedChannels.insert(it.key());
    }
    for (auto it = sess->eventlist.cbegin(); it != sess->eventlist.cend(); ++it) {
        if (m_reportedChannels.contains(it.key())) continue;
        for (EventList * el : it.value()) {
            if (el && el->count() > 0) {
                m_reportedChannels.insert(it.key());
                break;
            }
        }
    }
}
```

`LoadSessionsFromDatabase()`, inside the success branch:
```cpp
        if (sess->LoadFromDatabase()) {
            noteReportedChannels(sess);      // before AddSession: m_cnt is populated, eventlist empty
            if (AddSession(sess, true)) {
```

- [ ] **Step 4: Review checklist**

- `m_cnt` and `eventlist` are public on `Session` (session.h `public:` at line 60; `m_cnt` line 265). No friend declaration needed.
- `Machine(nullptr, id)` with non-zero id does not touch `profile` in the constructor (`machine.cpp:93-110`).
- `noteReportedChannels()` has no callers on worker threads.

- [ ] **Step 5: Checkpoint** — offer commit "Add Machine::noteReportedChannels and the derived capability set".

---

### Task 2: `std::optional` summary structs and NULL-aware repositories

**Files:**
- Modify: `oscar/database/session_summaries_repository.h:22-60`, `.cpp:27-200`
- Modify: `oscar/database/daily_summary_repository.h:20-70`, `.cpp:30-80` (`create`), `:474-520` (`mapResultToData`)
- Modify: `oscar/SleepLib/session.cpp:3274-3302` (`restoreCount` callers)

**Interfaces:**
- Produces: nullable fields typed `std::optional<double>` / `std::optional<int>`; `template<class T> static QVariant bindOptional(const std::optional<T>&)`; readers set `std::nullopt` on SQL NULL.
- Consumes: nothing from other tasks. Task 3 fills these fields; Task 6 relies on the read side.

- [ ] **Step 1: Change the structs**

`session_summaries_repository.h` — add `#include <optional>`; replace the metric members:
```cpp
    // Primary metrics. NULL (nullopt) means "not applicable to this device" — see
    // Notes/specs/2026-09-14-oh-ch-capability-gating-design.md §5.2. ahi is always present.
    double ahi = 0.0;                            // Apnea-Hypopnea Index
    std::optional<double> rdi;                   // Respiratory Disturbance Index; NULL unless the device reports RERA
    std::optional<double> oahi;                  // Obstructive AHI (v18); NULL unless the device splits hypopneas
    std::optional<double> cahi;                  // Central AHI (v18)

    // Event counts. NULL when the device has never reported the channel; 0 = scored none.
    std::optional<int> obstructiveCount;
    std::optional<int> unclassifiedCount;
    std::optional<int> hypopneaCount;
    std::optional<int> reraCount;
    std::optional<int> clearAirwayCount;
    std::optional<int> obstructiveHypopneaCount; // v18
    std::optional<int> centralHypopneaCount;     // v18
    std::optional<int> allApneaCount;            // v18; CPAP_AllApnea, an AHI contributor

    // Continuous statistics. NULL when the session has no data for the channel.
    std::optional<double> pressureAvg, pressureMin, pressureMax, pressure95th;
    std::optional<double> leakTotalAvg, leakTotal95th, leakTotalMax;
    std::optional<double> spo2Avg, spo2Min, pulseAvg;
```
Keep `id`, `sessionId`, `profileId`, `hoursUsed`, `maskOnHours`, `createdAt`, `updatedAt` as they are.

`daily_summary_repository.h` — same treatment: `ahi` stays `double`; `rdi`, `oahi`, `cahi`, the eight counts, `pressure*`, `leakTotal*`, `leakUnintentionalAvg`, `spo2*`, `pulse*` become `std::optional`. `isCompliant`, `hasOximetry`, hours and counts of sessions unchanged.

- [ ] **Step 2: Add the bind helper (both repositories, file-local)**

At the top of each `.cpp`, after the includes:
```cpp
#include <optional>

namespace {
//! \brief Bind an optional metric: a SQL NULL when it is absent, the value otherwise.
template <class T>
QVariant bindOptional(const std::optional<T> & v)
{
    return v ? QVariant::fromValue(*v) : QVariant();
}

//! \brief Read a REAL column that may be NULL.
std::optional<double> optionalDouble(const QVariant & v)
{
    return v.isNull() ? std::nullopt : std::optional<double>(v.toDouble());
}

//! \brief Read an INTEGER column that may be NULL.
std::optional<int> optionalInt(const QVariant & v)
{
    return v.isNull() ? std::nullopt : std::optional<int>(v.toInt());
}
} // namespace
```

- [ ] **Step 3: Rewire the writers and readers**

`session_summaries_repository.cpp` `create()` and `update()`: every `query.addBindValue(data.X)` for an optional field becomes `query.addBindValue(bindOptional(data.X))`. `ahi`, `hoursUsed`, `maskOnHours`, ids unchanged. `findBySession()`: `data.rdi = optionalDouble(query.value("rdi"));` etc.; counts via `optionalInt`.

`daily_summary_repository.cpp` `create()` and `mapResultToData()`: same substitution for the optional fields.

- [ ] **Step 4: Fix the only in-app readers**

`session.cpp:3295-3302` — `restoreCount(CPAP_Obstructive, summaryData.obstructiveCount.value_or(0));` for all eight calls. The `restoreCount` lambda signature (`int storedCount`) is unchanged; `value_or(0)` reproduces today's behaviour exactly (NULL → 0 → "no stored count").

- [ ] **Step 5: Review checklist**

- `grep -n "summaryData\.\|data\.\(oahi\|cahi\|rdi\|.*Count\|pressure\|leak\|spo2\|pulse\)" oscar/SleepLib/*.cpp oscar/database/*.cpp oscar/exports/*.cpp` — every remaining use either binds through `bindOptional()` or calls `.value_or()`. `profiles.cpp:1105` reads only `ds.date` and is unaffected.
- `QVariant::fromValue(int)` / `fromValue(double)` bind as INTEGER / REAL — same as before.
- Nothing else constructs `SessionSummaryData` / `DailySummaryData` with positional initialisers (`grep -rn "SessionSummaryData{\|DailySummaryData{"`).

- [ ] **Step 6: Checkpoint** — offer commit "Make summary-table metrics nullable in the repositories".

---

### Task 3: Apply the NULL rule in the two writers

**Files:**
- Modify: `oscar/SleepLib/session.cpp:3364-3445` (`StoreSummaryToDatabase()`)
- Modify: `oscar/database/daily_summary_repository.cpp:270-355` (`calculateFromDay()`)

**Interfaces:**
- Consumes: Task 1 `Machine::hasReportedEvents()`, `reportsHypopneaMechanism()`; Task 2 optional fields.
- Produces: rows following spec §5.2. Task 5 depends on this being in place before its pre-pass matters.

- [ ] **Step 1: `Session::StoreSummaryToDatabase()`**

Replace the index block and the count/statistics blocks with:
```cpp
    // AHI, RDI, OAHI and CAHI are events per hour of mask-on time ... (keep the existing
    // comment through "count() walks ahiChannels, so this stays correct as channels are added.")
    //
    // NULL rule (spec §5.2): a count column is NULL when this device has never reported
    // the channel, 0 when it has but this session scored none. oahi/cahi are NULL unless
    // the device splits hypopneas by mechanism; rdi is NULL unless it reports RERA, since
    // without RERA the RDI is just the AHI restated. s_machine's answer was settled by the
    // pre-pass in Machine::Save() before any SaveTask reached this point.
    const double indexHours = hours();
    const bool mechanism = s_machine->reportsHypopneaMechanism();
    const bool reportsRera = s_machine->hasReportedEvents(CPAP_RERA);
    if (indexHours > 0) {
        const double ahiEvents = count(AllAhiChannels);
        sessionSummaryData.ahi = ahiEvents / indexHours;
        if (reportsRera) sessionSummaryData.rdi = (ahiEvents + count(CPAP_RERA)) / indexHours;
        if (mechanism) {
            sessionSummaryData.oahi = count(AllOahiChannels) / indexHours;
            sessionSummaryData.cahi = count(AllCahiChannels) / indexHours;
        }
    } else {
        if (reportsRera) sessionSummaryData.rdi = 0.0;
        if (mechanism) { sessionSummaryData.oahi = 0.0; sessionSummaryData.cahi = 0.0; }
    }

    // Event counts. They can be fractional (ResMed summary-only sessions store STR
    // index * hours), so round to the nearest event rather than letting the int fields
    // truncate; this matches what LoadFromDatabase() restores.
    auto storeCount = [&](std::optional<int> & field, ChannelID id) {
        if (!s_machine->hasReportedEvents(id)) return;          // never reported: NULL
        field = qRound(m_cnt.value(id, 0));
    };
    storeCount(sessionSummaryData.obstructiveCount,         CPAP_Obstructive);
    storeCount(sessionSummaryData.clearAirwayCount,         CPAP_ClearAirway);
    storeCount(sessionSummaryData.hypopneaCount,            CPAP_Hypopnea);
    storeCount(sessionSummaryData.obstructiveHypopneaCount, CPAP_ObstructiveHypopnea);
    storeCount(sessionSummaryData.centralHypopneaCount,     CPAP_CentralHypopnea);
    storeCount(sessionSummaryData.reraCount,                CPAP_RERA);
    storeCount(sessionSummaryData.unclassifiedCount,        CPAP_Apnea);
    // CPAP_AllApnea is the undifferentiated apnea a few devices report. It counts
    // towards AHI, so leaving it unstored made SQL sums over these columns disagree
    // with the stored ahi for those devices (schema v18 added the column).
    storeCount(sessionSummaryData.allApneaCount,            CPAP_AllApnea);

    // Continuous statistics: present only when the session has the channel.
    if (m_wavg.contains(CPAP_Pressure)) {
        sessionSummaryData.pressureAvg = m_wavg[CPAP_Pressure];
        if (m_min.contains(CPAP_Pressure)) sessionSummaryData.pressureMin = m_min[CPAP_Pressure];
        if (m_max.contains(CPAP_Pressure)) sessionSummaryData.pressureMax = m_max[CPAP_Pressure];
        // The percentile needs the events; without them it is unknown, not 0.
        if (s_events_loaded) sessionSummaryData.pressure95th = percentile(CPAP_Pressure, 0.95);
    }
    if (m_wavg.contains(CPAP_LeakTotal)) {
        sessionSummaryData.leakTotalAvg = m_wavg[CPAP_LeakTotal];
        if (m_max.contains(CPAP_LeakTotal)) sessionSummaryData.leakTotalMax = m_max[CPAP_LeakTotal];
        if (s_events_loaded) sessionSummaryData.leakTotal95th = percentile(CPAP_LeakTotal, 0.95);
    }
    if (m_wavg.contains(OXI_SPO2)) {
        sessionSummaryData.spo2Avg = m_wavg[OXI_SPO2];
        if (m_min.contains(OXI_SPO2)) sessionSummaryData.spo2Min = m_min[OXI_SPO2];
    }
    if (m_wavg.contains(OXI_Pulse)) sessionSummaryData.pulseAvg = m_wavg[OXI_Pulse];
```

- [ ] **Step 2: `DailySummaryRepository::calculateFromDay()`**

Replace from "Use Day's built-in AHI/RDI calculation methods" through the oximetry block:
```cpp
    // Which device this day belongs to decides what is applicable (spec §5.2). A day with
    // no CPAP machine leaves every CPAP column NULL.
    Machine * cpap = day->machine(MT_CPAP);
    const bool mechanism   = cpap && cpap->reportsHypopneaMechanism();
    const bool reportsRera = cpap && cpap->hasReportedEvents(CPAP_RERA);

    if (cpap && data.totalHours > 0) {
        auto finite = [](EventDataType v) { return !qIsNaN(v) && !qIsInf(v); };
        EventDataType ahi = day->calcAHI();
        if (finite(ahi)) data.ahi = ahi;
        if (reportsRera) { EventDataType rdi = day->calcRDI();  if (finite(rdi))  data.rdi = rdi; }
        if (mechanism) {
            EventDataType oahi = day->calcOAHI();  if (finite(oahi)) data.oahi = oahi;
            EventDataType cahi = day->calcCAHI();  if (finite(cahi)) data.cahi = cahi;
        }
    }

    // Event counts. Day::count() is a float sum and can be fractional (ResMed
    // summary-only sessions store STR index * hours), so round to the nearest event.
    // NULL when the device has never reported the channel, 0 when it scored none today.
    auto storeCount = [&](std::optional<int> & field, ChannelID id) {
        if (!cpap || !cpap->hasReportedEvents(id)) return;
        field = qRound(day->count(id));
    };
    storeCount(data.obstructiveCount,         CPAP_Obstructive);
    storeCount(data.unclassifiedCount,        CPAP_Apnea);
    storeCount(data.hypopneaCount,            CPAP_Hypopnea);
    storeCount(data.reraCount,                CPAP_RERA);
    storeCount(data.clearAirwayCount,         CPAP_ClearAirway);
    storeCount(data.obstructiveHypopneaCount, CPAP_ObstructiveHypopnea);
    storeCount(data.centralHypopneaCount,     CPAP_CentralHypopnea);
    // CPAP_AllApnea contributes to AHI but was never stored before schema v18, which
    // is why SQL sums over these columns used to disagree with the stored ahi.
    storeCount(data.allApneaCount,            CPAP_AllApnea);

    // Continuous statistics: present only when the day has the channel.
    if (day->channelHasData(CPAP_Pressure)) {
        data.pressureAvg  = day->wavg(CPAP_Pressure);
        data.pressureMin  = day->Min(CPAP_Pressure);
        data.pressureMax  = day->Max(CPAP_Pressure);
        data.pressure95th = day->p90(CPAP_Pressure);
    }
    if (day->channelHasData(CPAP_Leak)) {
        data.leakTotalAvg  = day->wavg(CPAP_Leak);
        data.leakTotal95th = day->p90(CPAP_Leak);
        data.leakTotalMax  = day->Max(CPAP_Leak);
    }
    if (day->channelHasData(CPAP_LeakFlag)) data.leakUnintentionalAvg = day->wavg(CPAP_LeakFlag);
    if (day->channelHasData(OXI_SPO2)) {
        data.hasOximetry = true;
        data.spo2Avg = day->wavg(OXI_SPO2);
        data.spo2Min = day->Min(OXI_SPO2);
    }
    if (day->channelHasData(OXI_Pulse)) {
        data.hasOximetry = true;
        data.pulseAvg = day->wavg(OXI_Pulse);
        data.pulseMin = day->Min(OXI_Pulse);
        data.pulseMax = day->Max(OXI_Pulse);
    }
```
Add `#include "SleepLib/machine.h"` if `Machine` is only forward-declared there.

- [ ] **Step 3: Review checklist**

- `s_machine` is never null in `StoreSummaryToDatabase()` (it already calls `s_machine->getProfileId()` first).
- `day->machine(MT_CPAP)` returns `nullptr` for oximetry-only days; every use above is null-guarded.
- `Day::calcRDI()`, `calcOAHI()`, `calcCAHI()` exist in `day.h` (`:249-291`).
- The `CPAP_AllApnea` comment survives (it documents why the column exists).

- [ ] **Step 4: Checkpoint** — offer commit "Store NULL for summary metrics a device cannot report".

---

### Task 4: `SessionSummariesRepository::rebuildFromChannels()`

**Files:**
- Modify: `oscar/database/session_summaries_repository.h` (public static), `.cpp` (new function at end)

**Interfaces:**
- Produces: `static bool SessionSummariesRepository::rebuildFromChannels(QSqlDatabase& db, qint64 machineId)` — `machineId == 0` means every machine. Runs inside whatever transaction the caller holds; does not begin or commit one.
- Consumes: `session_channels`, `sessions`. Used by Task 5 (flip) and Task 6 (migration).

- [ ] **Step 1: Declare**

```cpp
    /*!
     * \brief Rebuild every metric column of session_summaries from session_channels.
     *
     * Applies the NULL rule of Notes/specs/2026-09-14-oh-ch-capability-gating-design.md
     * §5.2 to rows that already exist: a count column is NULL when the row's machine has
     * never reported the channel (no session_channels row with count > 0 on any of its
     * sessions), else the session's count or 0; ahi is the sum of session_channels.cph
     * over the AHI channels (#285); rdi / oahi / cahi likewise, NULLed when the machine
     * reports no RERA / no hypopnea mechanism; pressure, leak and oximetry statistics are
     * NULLed when the session has no row for the source channel.
     *
     * Used by the v18→v19 migration (all machines) and by Machine::Save() when a machine
     * reports a channel for the first time after rows were already stored.
     *
     * \param db        Connection to use; the caller owns the transaction.
     * \param machineId machines.id to restrict to, or 0 for every machine.
     * \return true if every statement succeeded.
     */
    static bool rebuildFromChannels(QSqlDatabase& db, qint64 machineId);
```

- [ ] **Step 2: Implement**

```cpp
bool SessionSummariesRepository::rebuildFromChannels(QSqlDatabase& db, qint64 machineId)
{
    // Every statement is scoped by this predicate on the session's machine. With
    // machineId == 0 it is always true, so one text serves both callers.
    const QString scope = machineId > 0
        ? QString(" AND s.machine_id = %1").arg(machineId)
        : QString();

    // Which channels each machine has ever reported (count > 0 on some session).
    // A temp table keeps the per-row CASEs below to a cheap indexed lookup.
    QStringList sql;
    sql << "DROP TABLE IF EXISTS temp.machine_reported"
        << "CREATE TEMP TABLE machine_reported AS"
           " SELECT DISTINCT s.machine_id, sc.channel_id"
           " FROM session_channels sc JOIN sessions s ON s.id = sc.session_id"
           " WHERE sc.count > 0" + scope
        << "CREATE INDEX temp.idx_machine_reported ON machine_reported(machine_id, channel_id)";

    // Count columns: NULL if the machine never reported the channel, else the row's
    // count or 0. Column / channel pairs follow schema.cpp ids.
    struct CountCol { const char * column; int channel; };
    static const CountCol countCols[] = {
        { "clear_airway_count",         4097 }, { "obstructive_count",          4098 },
        { "hypopnea_count",             4099 }, { "unclassified_count",         4100 },
        { "rera_count",                 4102 }, { "all_apnea_count",            4112 },
        { "obstructive_hypopnea_count", 4113 }, { "central_hypopnea_count",     4114 },
    };
    for (const CountCol & c : countCols) {
        sql << QString(
            "UPDATE session_summaries SET %1 = CASE WHEN EXISTS ("
            "   SELECT 1 FROM sessions s JOIN machine_reported mr ON mr.machine_id = s.machine_id"
            "   WHERE s.id = session_summaries.session_id AND mr.channel_id = %2)"
            " THEN COALESCE((SELECT sc.count FROM session_channels sc"
            "   WHERE sc.session_id = session_summaries.session_id AND sc.channel_id = %2), 0)"
            " ELSE NULL END"
            " WHERE session_id IN (SELECT s.id FROM sessions s WHERE 1=1%3)")
            .arg(QString::fromLatin1(c.column)).arg(c.channel).arg(scope);
    }

    // Indices from cph (events per mask-on hour as a REAL; the INTEGER counts truncate
    // ResMed summary-only sessions' fractional counts, see the v18 migration note).
    // Groups follow ahiChannels / oahiChannels / cahiChannels in schema.cpp.
    const QString sumCph = "COALESCE((SELECT SUM(sc.cph) FROM session_channels sc"
                           " WHERE sc.session_id = session_summaries.session_id"
                           " AND sc.channel_id IN (%1)), 0)";
    const QString reported = "EXISTS (SELECT 1 FROM sessions s JOIN machine_reported mr"
                             " ON mr.machine_id = s.machine_id"
                             " WHERE s.id = session_summaries.session_id AND mr.channel_id IN (%1))";
    sql << QString("UPDATE session_summaries SET"
                   " ahi = %1,"
                   " rdi = CASE WHEN %2 THEN %3 ELSE NULL END,"
                   " oahi = CASE WHEN %4 THEN %5 ELSE NULL END,"
                   " cahi = CASE WHEN %4 THEN %6 ELSE NULL END"
                   " WHERE session_id IN (SELECT s.id FROM sessions s WHERE 1=1%7)")
           .arg(sumCph.arg("4097, 4098, 4099, 4100, 4112, 4113, 4114"))
           .arg(reported.arg("4102"))
           .arg(sumCph.arg("4097, 4098, 4099, 4100, 4102, 4112, 4113, 4114"))
           .arg(reported.arg("4113, 4114"))
           .arg(sumCph.arg("4098, 4099, 4100, 4112, 4113"))
           .arg(sumCph.arg("4097, 4114"))
           .arg(scope);

    // Continuous statistics: NULL when the session has no row for the source channel.
    // pressure_95th additionally: a stored 0 with pressure present means the percentile
    // was never computed (events not loaded), and therapy pressure is never 0.
    struct StatCols { const char * columns; int channel; };
    static const StatCols statCols[] = {
        { "pressure_avg = NULL, pressure_min = NULL, pressure_max = NULL, pressure_95th = NULL", 4364 },
        { "leak_total_avg = NULL, leak_total_95th = NULL, leak_total_max = NULL",                4375 },
        { "spo2_avg = NULL, spo2_min = NULL",                                                    6145 },
        { "pulse_avg = NULL",                                                                    6144 },
    };
    for (const StatCols & c : statCols) {
        sql << QString(
            "UPDATE session_summaries SET %1"
            " WHERE NOT EXISTS (SELECT 1 FROM session_channels sc"
            "   WHERE sc.session_id = session_summaries.session_id AND sc.channel_id = %2)"
            " AND session_id IN (SELECT s.id FROM sessions s WHERE 1=1%3)")
            .arg(QString::fromLatin1(c.columns)).arg(c.channel).arg(scope);
    }
    sql << QString("UPDATE session_summaries SET pressure_95th = NULL"
                   " WHERE pressure_95th = 0 AND pressure_avg IS NOT NULL"
                   " AND session_id IN (SELECT s.id FROM sessions s WHERE 1=1%1)").arg(scope)
        << "DROP TABLE IF EXISTS temp.machine_reported";

    QSqlQuery q(db);
    for (const QString & statement : sql) {
        if (!q.exec(statement)) {
            qCritical() << "SessionSummariesRepository::rebuildFromChannels failed:"
                        << q.lastError().text() << "in" << statement.left(80);
            DatabaseManager::instance().checkQueryError("SessionSummariesRepository::rebuildFromChannels", q);
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 3: Verify the SQL against a copy of the test database (runnable now)**

Write `scratchpad/verify_rebuild.py`: copy the test DB to the scratchpad, run the same statements with `scope = ""` via `sqlite3`, then assert for a ResMed machine: `oahi IS NULL`, `all_apnea_count IS NULL`, `obstructive_count IS NOT NULL`; for the G3X machine with CH events: `cahi IS NOT NULL` and `oahi + cahi` within 1e-6 of `ahi` on every row; for every row `ahi IS NOT NULL`. Print row counts before and after (must be equal). Fix the SQL text in the C++ if any assertion fails.

- [ ] **Step 4: Checkpoint** — offer commit "Add SessionSummariesRepository::rebuildFromChannels".

---

### Task 5: `Machine::Save()` pre-pass and late-capability fix-up

**Files:**
- Modify: `oscar/SleepLib/machine.cpp:1255-1300` (`Save()`, before the `SaveTask` loop)

**Interfaces:**
- Consumes: Task 1 `noteReportedChannels()`, Task 4 `rebuildFromChannels()`, `DailySummaryRepository::calculateAndStoreFromDay()`, `Machine::day` (per-machine `QMap<QDate, Day*>`), `getProfileId()`.

- [ ] **Step 1: Implement the pre-pass**

In `Save()`, after the `SaveToDatabase()` block and before `for (s = sessionlist.begin(); ...) queTask(new SaveTask(...))`:
```cpp
    // Settle this device's reported-channel set before any SaveTask runs: the tasks
    // write session_summaries in parallel and each row's NULL columns depend on it
    // (Session::StoreSummaryToDatabase). The pre-pass also detects a device reporting
    // a channel for the first time after rows already exist — a G3X's first central
    // hypopnea weeks in, a ResMed's first RERA — and rebuilds its earlier rows.
    const QSet<ChannelID> before = m_reportedChannels;
    bool hasStoredRows = false;
    for (s = sessionlist.begin(); s != sessionlist.end(); s++) {
        if ((*s)->IsChanged()) noteReportedChannels(*s);
        if ((*s)->sessionRowId() != 0) hasStoredRows = true;
    }
    if (hasStoredRows && m_database_id > 0 && m_reportedChannels != before) {
        qDebug() << "Machine::Save(): device reports new channels; rebuilding stored summaries for machine" << m_database_id;
        QSqlDatabase db = DatabaseManager::instance().database();
        if (SessionSummariesRepository::rebuildFromChannels(db, m_database_id)) {
            // Daily rows: recompute this device's days. calculateAndStoreFromDay() is an
            // INSERT OR REPLACE, so no invalidation is needed. The post-import pass may
            // cover only imported days (ImportContext::Commit), so do not rely on it.
            DailySummaryRepository dailyRepo;
            const qint64 profileId = getProfileId();
            for (auto it = day.begin(); it != day.end(); ++it) {
                Day * d = it.value();
                if (d && d->hasEnabledSessions()) dailyRepo.calculateAndStoreFromDay(d, profileId);
            }
        }
    }
```
Add `#include "../database/session_summaries_repository.h"` and `"../database/daily_summary_repository.h"` to `machine.cpp` if absent.

- [ ] **Step 2: Review checklist**

- `Machine::day` is the per-machine map iterated by `Profile::calculateDailySummaries()` (`profiles.cpp:3110`), so this recomputes exactly this device's days.
- `sessionRowId()` is the accessor used at `machine.cpp:1333`.
- The pre-pass reads `eventlist` on the main thread before `runTasks()`; no worker has started.
- Sessions that are new this import are still stored by their own `SaveTask` afterwards, with the settled set — the rebuild touching them first is harmless.
- `QSet<ChannelID>` comparison and copy are cheap (tens of entries).

- [ ] **Step 3: Checkpoint** — offer commit "Settle reported channels before saving and rebuild rows when capability arrives late".

---

### Task 6: Schema v19

**Files:**
- Modify: `oscar/database/database_schema.h:37-87` (version comment, `CURRENT_SCHEMA_VERSION = 19`, declare `migrateV18ToV19`)
- Modify: `oscar/database/database_schema.cpp:251-263` (dispatch), `:1745-1866` (v18 slimmed, v19 added)

**Interfaces:**
- Consumes: Task 4 `rebuildFromChannels(db, 0)`.

- [ ] **Step 1: Header**

Add above the "Version 18" paragraph:
```cpp
     * Version 19: NULL for summary metrics that do not apply (GitLab #261)
     * - No column changes. session_summaries and daily_summaries now store NULL, not 0,
     *   for counts of channels the device has never reported, for oahi/cahi unless the
     *   device splits hypopneas by mechanism, for rdi unless it reports RERA, and for
     *   pressure/leak/oximetry statistics a row has no data for.
     * - The #285 index recompute that v18 used to carry runs here instead, so v18
     *   databases that never received it are corrected.
     * - Zero-count OH/CH session_channels rows of devices that never scored either are
     *   removed (the loaders no longer create them).
     * - daily_summaries: single-CPAP-device profiles are rewritten in SQL; profiles with
     *   more than one CPAP device have their rows deleted and regenerated on next open.
```
Set `CURRENT_SCHEMA_VERSION = 19;`. Declare `static bool migrateV18ToV19(QSqlDatabase& db);` after `migrateV17ToV18`.

- [ ] **Step 2: Dispatch**

After the `fromVersion == 17` block:
```cpp
    if (fromVersion == 18) {
        if (!migrateV18ToV19(db)) {
            qCritical() << "DatabaseSchema: v18->v19 migration failed";
            return false;
        }
        fromVersion = 19;
    }
```

- [ ] **Step 3: Slim `migrateV17ToV18`**

Delete the `backfills[]` array and its loop and the "recomputed session_summaries indices" debug line. Rewrite the function comment: keep the first paragraph (additive columns); replace the "Two session_summaries backfills…" paragraph with:
```
 * The #285 index recompute originally done here now lives in migrateV18ToV19, so that
 * databases already at v18 (which never ran it) are corrected as well.
```

- [ ] **Step 4: Add `migrateV18ToV19`**

```cpp
/*
 * Migrate database from schema version 18 to 19
 *
 * No column changes. Applies the NULL convention of
 * Notes/specs/2026-09-14-oh-ch-capability-gating-design.md to rows that already exist:
 *  - session_summaries: SessionSummariesRepository::rebuildFromChannels() for every
 *    machine. This also carries the #285 recompute of ahi/rdi/oahi/cahi from
 *    session_channels.cph, which v18 used to do and which v18 tester databases missed.
 *  - session_channels: zero-count OH/CH rows are removed for machines that never
 *    scored either; the loaders no longer create the empty lists that produced them.
 *  - daily_summaries: the table carries no machine id, so a row can only be rewritten
 *    in SQL where the profile's answer is the machine's answer — profiles with a single
 *    CPAP device. Their count, index and statistic columns are NULLed by the same rule.
 *    Profiles with more than one CPAP device have their rows deleted; the table is a
 *    cache, and Profile::LoadMachineData() regenerates it on the next open (the
 *    existingCount == 0 branch), after every machine has loaded.
 */
bool DatabaseSchema::migrateV18ToV19(QSqlDatabase& db)
{
    qDebug() << "DatabaseSchema: Migrating v18 -> v19";

    if (!db.transaction()) {
        qCritical() << "DatabaseSchema: migrateV18ToV19: failed to start transaction";
        return false;
    }

    if (!SessionSummariesRepository::rebuildFromChannels(db, 0)) {
        qCritical() << "DatabaseSchema: migrateV18ToV19: session_summaries rebuild failed";
        db.rollback();
        return false;
    }

    // Machines that have ever scored an OH or CH (channel ids 4113 / 4114).
    static const char* const mechanismMachines =
        "SELECT DISTINCT s.machine_id FROM session_channels sc JOIN sessions s ON s.id = sc.session_id"
        " WHERE sc.channel_id IN (4113, 4114) AND sc.count > 0";
    // Profiles with exactly one CPAP device (machine_type 1).
    static const char* const singleCpapProfiles =
        "SELECT profile_id FROM machines WHERE machine_type = 1 GROUP BY profile_id HAVING COUNT(*) = 1";
    // Profiles with more than one CPAP device.
    static const char* const multiCpapProfiles =
        "SELECT profile_id FROM machines WHERE machine_type = 1 GROUP BY profile_id HAVING COUNT(*) > 1";
    // "Some device of this profile has reported channel X."
    static const char* const profileReported =
        "EXISTS (SELECT 1 FROM session_channels sc JOIN sessions s ON s.id = sc.session_id"
        " JOIN machines m ON m.id = s.machine_id"
        " WHERE m.profile_id = daily_summaries.profile_id AND sc.channel_id %1 AND sc.count > 0)";

    QStringList sql;
    sql << QString("DELETE FROM session_channels WHERE channel_id IN (4113, 4114) AND count = 0"
                   " AND session_id IN (SELECT id FROM sessions WHERE machine_id NOT IN (%1))")
           .arg(mechanismMachines);

    struct CountCol { const char * column; const char * channel; };
    static const CountCol countCols[] = {
        { "clear_airway_count", "= 4097" }, { "obstructive_count", "= 4098" },
        { "hypopnea_count", "= 4099" },     { "unclassified_count", "= 4100" },
        { "rera_count", "= 4102" },         { "all_apnea_count", "= 4112" },
        { "obstructive_hypopnea_count", "= 4113" }, { "central_hypopnea_count", "= 4114" },
        { "rdi", "= 4102" },
        { "oahi", "IN (4113, 4114)" },      { "cahi", "IN (4113, 4114)" },
    };
    for (const CountCol & c : countCols) {
        sql << QString("UPDATE daily_summaries SET %1 = NULL WHERE profile_id IN (%2) AND NOT %3")
               .arg(QString::fromLatin1(c.column), QString::fromLatin1(singleCpapProfiles),
                    QString::fromLatin1(profileReported).arg(QString::fromLatin1(c.channel)));
    }
    // Continuous statistics on daily rows were written as 0 when the day had no such
    // channel. Therapy pressure and pulse are never 0, and a night with zero total leak
    // and zero maximum leak does not occur on a real device, so 0 across the group is
    // "no data". leak_unintentional_avg is left alone: 0 can be genuine there.
    sql << "UPDATE daily_summaries SET pressure_avg = NULL, pressure_min = NULL, pressure_max = NULL,"
           " pressure_95th = NULL WHERE pressure_avg = 0 AND pressure_max = 0"
        << "UPDATE daily_summaries SET leak_total_avg = NULL, leak_total_95th = NULL, leak_total_max = NULL"
           " WHERE leak_total_avg = 0 AND leak_total_max = 0"
        << "UPDATE daily_summaries SET spo2_avg = NULL, spo2_min = NULL WHERE spo2_avg = 0"
        << "UPDATE daily_summaries SET pulse_avg = NULL, pulse_min = NULL, pulse_max = NULL WHERE pulse_avg = 0"
        << QString("DELETE FROM daily_summaries WHERE profile_id IN (%1)").arg(multiCpapProfiles);

    QSqlQuery q(db);
    for (const QString & statement : sql) {
        if (!q.exec(statement)) {
            qCritical() << "DatabaseSchema: migrateV18ToV19:" << statement.left(80) << "failed:" << q.lastError().text();
            DatabaseManager::instance().checkQueryError("DatabaseSchema::migrateV18ToV19", q);
            db.rollback();
            return false;
        }
    }

    if (!setSchemaVersion(db, 19)) {
        qCritical() << "DatabaseSchema: migrateV18ToV19: setSchemaVersion failed";
        db.rollback();
        return false;
    }
    if (!db.commit()) {
        qCritical() << "DatabaseSchema: migrateV18ToV19: commit failed";
        db.rollback();
        return false;
    }
    qDebug() << "DatabaseSchema: Migration v18->v19 complete";
    return true;
}
```
Add `#include "session_summaries_repository.h"` to `database_schema.cpp`.

- [ ] **Step 5: Verify against a copy of the test database (runnable now)**

Extend `scratchpad/verify_rebuild.py` (or a second script) to run the v19 statements after the rebuild on a fresh copy and assert: `schema_version` row count unchanged (the script does not set it); `session_channels` rows with `channel_id IN (4113,4114) AND count = 0` remain only for machines in `mechanismMachines`; for a single-CPAP ResMed profile every `daily_summaries.oahi IS NULL` and `obstructive_count IS NOT NULL`; for multi-CPAP profiles `daily_summaries` is empty; `sessions` and `session_summaries` counts unchanged.

- [ ] **Step 6: Review checklist**

- `rebuildFromChannels()` and the v19 statements run inside the migration's transaction (no nested `transaction()` calls).
- `DatabaseSchema::createSessionSummariesTable` / `createDailySummariesTable` unchanged: the columns are already nullable.
- Backup restore: `MIN_RESTORE_SCHEMA_VERSION` stays 12; a v18 backup restored into v19 has 0s, which is the pre-migration state — note this in `DATABASE_SCHEMA_REFERENCE.md` (Task 10).

- [ ] **Step 7: Checkpoint** — offer commit "Schema v19: NULL summary metrics that do not apply; move #285 recompute".

---

### Task 7: Loaders — create OH/CH lists on first event

**Files:**
- Modify: `oscar/SleepLib/loader_plugins/bmc_loader.cpp:393-420`
- Modify: `oscar/SleepLib/loader_plugins/sefam_loader.cpp:1036-1075`, `:1088-1125`
- Modify: `oscar/SleepLib/loader_plugins/prisma_loader.cpp:453-466`

- [ ] **Step 1: BMC**

```cpp
    // Only the G3X parser produces OH/CH; legacy BMC cards report a single
    // undifferentiated hypopnea. The lists are created on the first such event so a
    // device that never scores one is not recorded as able to (GitLab #261).
    EventList* oscarOhList   = nullptr;
    EventList* oscarChList   = nullptr;
    ...
        case BmcRespiratoryEventType::OH:
            if (!oscarOhList) oscarOhList = oscarSession->AddEventList(CPAP_ObstructiveHypopnea, EVL_Event);
            oscarOhList->AddEvent(bmcEvent.EndTime.toMSecsSinceEpoch(), bmcEvent.DurationSeconds); break;
        case BmcRespiratoryEventType::CH:
            if (!oscarChList) oscarChList = oscarSession->AddEventList(CPAP_CentralHypopnea, EVL_Event);
            oscarChList->AddEvent(bmcEvent.EndTime.toMSecsSinceEpoch(), bmcEvent.DurationSeconds); break;
```

- [ ] **Step 2: SEFAM, log path (`:1036`)**

`oh` / `ch` start `nullptr`; in the switch:
```cpp
                case SefamParsing::kLogObstructiveHypop:
                    if (!oh) oh = session->AddEventList(CPAP_ObstructiveHypopnea, EVL_Event);
                    oh->AddEvent(when - kObstructiveHypopneaPlacementMs, 0); break;
                case SefamParsing::kLogCentralHypopnea:
                    if (!ch) ch = session->AddEventList(CPAP_CentralHypopnea, EVL_Event);
                    ch->AddEvent(when - kCentralHypopneaPlacementMs, 0); break;
```
Add one comment above the declarations: `// OH/CH lists are created on the first event: an empty list would record the device as able to score them (GitLab #261).`

- [ ] **Step 3: SEFAM, memory-image path (`:1088`)**

`oh` / `ch` start `nullptr`; replace the two `place()` calls:
```cpp
                if (r.obstructiveHypopnea) {
                    if (!oh) oh = session->AddEventList(CPAP_ObstructiveHypopnea, EVL_Event);
                    place(oh, minuteMs, r.obstructiveHypopnea);
                }
                if (r.centralHypopnea) {
                    if (!ch) ch = session->AddEventList(CPAP_CentralHypopnea, EVL_Event);
                    place(ch, minuteMs, r.centralHypopnea);
                }
```
The `ohTotal` / `chTotal` accumulation and the header cross-check below it are unchanged.

- [ ] **Step 4: Prisma**

```cpp
    // Every channel keeps an (empty) list so it appears in the events list from the first
    // import — except OH and CH, whose presence is read as "this device splits hypopneas".
    // Those two are created above, on the first event only (GitLab #261).
    if (channel != CPAP_ObstructiveHypopnea && channel != CPAP_CentralHypopnea) {
        session->AddEventList(channel, EVL_Event);
    }
```

- [ ] **Step 5: Review checklist**

- No other channel's list creation changed in any of the three files (`git diff` shows only OH/CH lines).
- SEFAM: `oh`/`ch` are not dereferenced outside the guarded sites (search each function body).
- BMC: `default:` branch and the PB/RERA handling untouched.

- [ ] **Step 6: Checkpoint** — offer commit "Create OH/CH event lists only when the device scores one".

---

### Task 8: Daily page and printed report

**Files:**
- Modify: `oscar/daily.cpp:1813-1828` (`getAHI()`)
- Modify: `oscar/reports.cpp:288-295`

- [ ] **Step 1: `Daily::getAHI()`**

```cpp
    // Obstructive and central breakdown of the same index, shown only for a device that
    // scores hypopneas by mechanism — for any other device OAHI would just restate AHI
    // (GitLab #261). The two always sum to AHI. RERA is excluded even in RDI mode, since
    // an arousal is neither obstructive nor central.
    Machine * cpap = day->machine(MT_CPAP);
    if (!isBrick && hours > 0 && cpap && cpap->reportsHypopneaMechanism()) {
```
Rest of the block unchanged.

- [ ] **Step 2: `reports.cpp`**

```cpp
            // Only a device that scores hypopneas by mechanism reports these; for any
            // other device OAHI would just restate AHI, so the whole group stays off
            // the line rather than printing 0.00 (GitLab #261). Gated on the device, not
            // on today's data, so a capable device's zero night prints 0.00 like the
            // Daily sidebar does.
            if (cpap->reportsHypopneaMechanism()) {
                stats += QObject::tr("OHI=%1 CHI=%2 ").arg(ohi, 0, 'f', 2).arg(chi, 0, 'f', 2);
                stats += QObject::tr("OAHI=%1 CAHI=%2 ").arg(oahi, 0, 'f', 2).arg(cahi, 0, 'f', 2);
            }
```
`cpap` is the `Machine *` already in scope there (used at `:297` for `loaderName()`).

- [ ] **Step 3: Review checklist**

- `Daily::getAHI()` has `day` in scope and `#include "SleepLib/machine.h"` is already present (it calls `cpap->hasChannel()` at `:2108`).
- `reports.cpp`: `cpap` is non-null on this path (it is dereferenced two lines later already).

- [ ] **Step 4: Checkpoint** — offer commit "Show OAHI/CAHI only for devices that split hypopneas".

---

### Task 9: Statistics page

**Files:**
- Modify: `oscar/statistics.cpp:1084-1092` (helpers), `:783-784` (row construction), `:2144-2147` (`value()`)

- [ ] **Step 1: Helpers, next to `calcOAHI` / `calcCAHI`**

```cpp
// How many CPAP days in [start, end] belong to a device that splits hypopneas by
// mechanism, and how many CPAP days there are at all. OAHI/CAHI are shown for a period
// only when the two are equal: mixing a capable device's days with another device's
// would either restate AHI or break OAHI + CAHI == AHI for that column (GitLab #261).
static void mechanismDays(QDate start, QDate end, int & capable, int & total)
{
    capable = 0;
    total = 0;
    for (QDate date = start; date <= end; date = date.addDays(1)) {
        Day * day = p_profile->GetGoodDay(date, MT_CPAP);
        if (!day) continue;
        Machine * mach = day->machine(MT_CPAP);
        total++;
        if (mach && mach->reportsHypopneaMechanism()) capable++;
    }
}

static bool anyMechanismDays(QDate start, QDate end)
{
    int capable, total;
    mechanismDays(start, end, capable, total);
    return capable > 0;
}
```
These must be defined before `StatisticsRow::value()` and before the constructor that builds `rows` — place them where `calcOAHI` lives (`:1084`) and add forward declarations above the constructor if the constructor precedes them in the file (it does: `:742`).

- [ ] **Step 2: Row construction (`:783`)**

```cpp
    // OAHI/CAHI rows exist only when some device in the profile splits hypopneas by
    // mechanism; individual periods that mix devices show "-" (see StatisticsRow::value).
    if (anyMechanismDays(p_profile->FirstDay(MT_CPAP), p_profile->LastDay(MT_CPAP))) {
        rows.push_back(StatisticsRow(STR_TR_OAHI, SC_OAHI, MT_CPAP));
        rows.push_back(StatisticsRow(STR_TR_CAHI, SC_CAHI, MT_CPAP));
    }
```
`Statistics` is a local constructed inside `MainWindow::GenerateStatistics()` (`mainwindow.cpp:3493`) after the `p_profile` null check, so the profile's days are loaded when the constructor runs; no other change is needed.

- [ ] **Step 3: `StatisticsRow::value()` (`:2144`)**

```cpp
    } else if (calc == SC_OAHI || calc == SC_CAHI) {
        int capable, total;
        mechanismDays(start, end, capable, total);
        if (capable == total) {
            value = QString("%1").arg(calc == SC_OAHI ? calcOAHI(start, end) : calcCAHI(start, end), 0, 'f', decimals);
        } else {
            value = QString("<span title='%1'>-</span>")
                        .arg(QObject::tr("Not available for all devices in this period"));
        }
```
`daysUsed` has already excluded `total == 0` (the function returned `"-"`).

- [ ] **Step 4: Review checklist**

- `Profile::GetGoodDay(QDate, MachineType)` is the accessor `calcCount()` uses (`profiles.cpp:1939`).
- The `"-"` for an empty period keeps its existing plain form; only the mixed case carries the tooltip.
- Per-channel `SC_CPH` rows for `ObstructiveHypopnea` / `CentralHypopnea` are untouched (Task 7 makes their `channelAvailable()` gate honest).

- [ ] **Step 5: Checkpoint** — offer commit "Statistics: OAHI/CAHI per period only when every device in it splits hypopneas".

---

### Task 10: SQL consumers and documentation

**Files:**
- Modify: `oscar/docs/system_reports.orf` (reports at lines 74, 106, 189, 217, 247, 391, 482)
- Modify: `Notes/Database/USEFUL_QUERIES.sql:280-300` (identity check)
- Modify: `Notes/Database/DATABASE_SCHEMA_REFERENCE.md:26-48` (version table), `:797-870` and `:875-940` (column tables)
- Modify: `Notes/Database/DATABASE_SCHEMA.md:48` (version table)
- Modify: `Notes/Developer Notes/BUG_FIXES.md` (append)

- [ ] **Step 1: `system_reports.orf` — mechanical rule**

In every aggregated report, each `SUM(<count column>)` that is a term of a `+` expression becomes `COALESCE(SUM(<count column>), 0)`. Standalone `SUM(ds.rera_count) as RERA` style columns stay as they are — `NULL` there correctly means "this device does not report it". Per-index single-column ratios (`SUM(rera_count) / NULLIF(SUM(mask_on_hours), 0)`) stay as they are for the same reason. Weighted averages already guard with `CASE WHEN x IS NOT NULL` in the Monthly Summary; apply the same denominator guard to the `by Week` / `by Month` `Pressure_Avg` / `Leak_Avg` expressions:
```sql
  ROUND(SUM(ds.pressure_avg * ds.mask_on_hours)
        / NULLIF(SUM(CASE WHEN ds.pressure_avg IS NOT NULL THEN ds.mask_on_hours END), 0), 2) as Pressure_Avg,
```
Add one comment line under the `by Week` report header: `-- Count sums are COALESCEd: a column is NULL for devices that never report that event (schema v19).`

- [ ] **Step 2: Run every system report against the migrated copy (runnable now)**

Script: parse `system_reports.orf` (`Query: <<SQL … SQL` blocks), substitute `#PROFILE_ID` with a single-CPAP ResMed profile id from the copy and `#START_DATE`/`#END_DATE` with `'2000-01-01'`/`'2100-01-01'`, execute each with `sqlite3`, and assert: no exception; for the `by Week`, `by Month` and `Monthly Summary` reports every returned `AHI` and `RDI` is non-NULL where `Hours > 0`. Repeat for the G3X profile. Fix any report the assertion catches.

- [ ] **Step 3: `USEFUL_QUERIES.sql`**

The identity check at `:285-295`: add `AND ds.oahi IS NOT NULL` to its `WHERE`, with the comment `-- NULL means the device does not split hypopneas (v19); the identity only applies where it does`. Audit any other `+` over count columns in the file the same way as Step 1.

- [ ] **Step 4: Schema documentation**

`DATABASE_SCHEMA_REFERENCE.md` and `DATABASE_SCHEMA.md` version tables — append:
```
| 19 | 2026 Q3 | 🔧 **SEMANTICS**: Summary metrics that do not apply are now `NULL`, not `0`, in `session_summaries` and `daily_summaries`: event counts for channels the device has never reported, `oahi`/`cahi` unless the device splits hypopneas by mechanism, `rdi` unless it reports RERA, and pressure/leak/oximetry statistics a row has no data for. No column changes. The #285 index recompute moved here from v18. Zero-count OH/CH `session_channels` rows of devices that never scored either are removed. `daily_summaries` of profiles with several CPAP devices are regenerated on next open. A v18 backup restored into v19 keeps its `0`s. |
```
Column tables for both `session_summaries` and `daily_summaries`: change the `Null` cell to `YES` for `rdi`, `oahi`, `cahi`, the eight count columns and the statistic columns, and add a paragraph above each table:
```
**NULL vs 0 (v19).** `0` means measured and none; `NULL` means not applicable or not measured. Aggregate with `COALESCE(SUM(col), 0)` when adding columns together — `SUM()` over an all-NULL group is `NULL` and `NULL + x` is `NULL`.
```

- [ ] **Step 5: `BUG_FIXES.md`**

Append under today's date:
```
### 2026-09-15 — OAHI/CAHI shown for devices that cannot report them (GitLab #261)

OAHI/CAHI were computed and displayed for every device, restating AHI for any device that
does not split hypopneas by mechanism, and legacy BMC devices listed OH/CH at 0.00 because
the shared BMC event writer pre-created the lists. Capability is now derived from the data
(`Machine::reportsHypopneaMechanism()`), the Daily page, printed report and Statistics page
gate on it, the three OH/CH loaders create those lists on the first event only, and the
summary tables store NULL for metrics that do not apply (schema v19).
```

- [ ] **Step 6: Checkpoint** — offer commit "NULL-safe system reports and v19 documentation".

---

## Verification (after the QtCreator build)

| Case | Expect |
|---|---|
| ResMed-only profile | no OAHI/CAHI line on Daily, none in the printed report, no Statistics rows; `session_summaries.oahi`, `cahi`, `all_apnea_count`, `*_hypopnea_count` NULL; system-report RDI populated |
| G3X, SEFAM, Prisma profile | OAHI/CAHI shown, 0.00 on zero nights; counts 0, not NULL |
| Legacy BMC profile after Rebuild CPAP Data | nothing OH/CH anywhere: sidebar, events list, flags, Statistics rows, `session_channels` |
| Mixed profile | rows present; recent columns numeric, long-range columns `"-"` with tooltip |
| v18 tester database | opens at v19; `session_summaries` recomputed; multi-device profiles' daily rows regenerated once; `sessions` / `session_summaries` counts unchanged |
| Flip | one G3X night without CH → NULLs; a night with CH → earlier rows now 0/values, Daily line appears for the earlier night |
| `test` target | `MachineTests` passes |
