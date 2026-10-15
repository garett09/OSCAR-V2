# OSCAR Architecture Guide

**Last updated:** 2026-10-05 (schema v19)
**Purpose:** Help developers (and AI assistants) understand how the pieces fit together.
For a shorter orientation aimed at new developers, start with `NEW_DEVELOPER_GUIDE.md`.

---

## 1. High-Level Overview

OSCAR is a Qt6/C++17 desktop application that reads sleep therapy data from CPAP/Oximeter SD cards, stores it in an SQLite database, and presents it via an interactive graphing UI.

```
┌──────────────────────────────────────────────────────────┐
│                     MainWindow (UI)                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐  │
│  │ProfileSel│  │  Daily   │  │ Overview │  │ Stats/  │  │
│  │  ector   │  │  View    │  │  View    │  │ Reports │  │
│  └──────────┘  └──────────┘  └──────────┘  └─────────┘  │
│          │            │             │            │        │
│          ▼            ▼             ▼            ▼        │
│  ┌───────────────────────────────────────────────────┐   │
│  │         Graphing Engine (gGraphView/gGraph)       │   │
│  │         Layers: LineChart, Flags, Summary, etc.   │   │
│  └───────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────┘
         │                    ▲
         ▼                    │
┌──────────────────────────────────────────────────────────┐
│                    SleepLib (Data Core)                   │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌────────────┐  │
│  │ Profile │──│ Machine │──│ Session │──│ EventList  │  │
│  │         │  │ (Day*)  │  │         │  │ (waveforms │  │
│  │ daylist │  │ daymap  │  │ events  │  │  & events) │  │
│  └─────────┘  └─────────┘  └─────────┘  └────────────┘  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │ MachineLoader│  │   schema::   │  │ Preferences  │    │
│  │   (plugins)  │  │   Channel    │  │  (key-value) │    │
│  └──────────────┘  └──────────────┘  └──────────────┘    │
└──────────────────────────────────────────────────────────┘
         │                    ▲
         ▼                    │
┌──────────────────────────────────────────────────────────┐
│                  Database Layer (SQLite)                  │
│  DatabaseManager (singleton), Repository classes,        │
│  MigrationManager                                        │
└──────────────────────────────────────────────────────────┘
```

---

## 2. Startup Sequence

The startup sequence in `main.cpp` is:

1. **QApplication** created, graphics engine selected (OpenGL/ANGLE/Software)
2. **Logger** initialized
3. **Command-line args** parsed (`--datadir`, `--profile`/`-p`, `--language`, `--legacy`, etc.)
4. **Data folder** located or created (`GetAppData()` → by default `Documents/OSCAR20_Data`,
   no trailing separator); a `QLockFile` (`oscar.lock`) keeps a second instance out
5. **Database** opened (`DatabaseManager::instance().initialize(dbPath)`). If the stored
   schema is older than `CURRENT_SCHEMA_VERSION`, `confirmAndUpgradeDatabase()` asks the
   user (*Back up and upgrade* / *Upgrade* / *Exit*) and runs the migrations on a worker
   thread. A database newer than the build is refused. After an unclean shutdown an
   integrity check runs.
6. **Preferences** loaded (`p_pref` global, `AppSetting` global) from the `app_preferences` table
7. **Schema** initialized (`schema::init()`) — defines all built-in channels
8. **Loaders registered** — each CPAP/oximeter loader's `::Register()` calls `RegisterLoader()`.
   Steps 7–8 must precede step 9: the 1.x importer needs the channel schema.
9. **Migration** from OSCAR 1.x offered if the data folder is new
10. **Profiles scanned** (`Profiles::Scan()`) — discovers all profiles in the database
11. **MainWindow** `SetupGUI()` called — creates the Profile tab; Welcome/Daily/Overview are
    created per profile in `OpenProfile()`
12. **Event loop** starts (`mainapp.exec()`)

On shutdown: `closeEvent()` → `Profiles::Done()` → `DestroyGraphGlobals()` → `DatabaseManager::close()`

---

## 3. Core Data Model

### Class Hierarchy

```
Profile (extends Preferences)
  ├── daylist: QMap<QDate, Day*>     ← master day index across all devices
  ├── m_machlist: QList<Machine*>    ← all devices for this profile
  ├── user: UserInfo*                ← personal info (name, DOB, etc.)
  ├── doctor: DoctorInfo*
  ├── cpap: CPAPSettings*
  ├── oxi: OxiSettings*
  ├── appearance: AppearanceSettings*
  ├── general: UserSettings*
  └── session: SessionSettings*

Machine (base class)
  ├── CPAP         ← PAP devices (ResMed, PRS1, etc.)
  ├── Oximeter     ← pulse oximeters
  ├── SleepStage   ← sleep stage detectors (ZEO)
  └── PositionSensor ← body position (Somnopose)
  Members:
  ├── day: QMap<QDate, Day*>         ← per-device day index
  ├── sessionlist: QHash<SessionID, Session*>
  └── info: MachineInfo              ← brand, model, serial, etc.

Day
  ├── sessions: QList<Session*>      ← all sessions for this date
  ├── machines: QHash<MachineType, Machine*>
  └── date: QDate
  Note: A Day belongs to exactly one date. Multiple devices can
  contribute sessions to the same Day.

Session
  ├── eventlist: QHash<ChannelID, QVector<EventList*>>
  ├── settings: QHash<ChannelID, QVariant>   ← device settings
  ├── summary caches: m_cnt, m_sum, m_avg, m_min, m_max, m_cph, etc.
  ├── m_slices: QVector<SessionSlice>        ← mask-on/off periods
  ├── s_first, s_last: qint64               ← time range (ms since epoch)
  └── s_machine: Machine*

EventList
  ├── m_data: QVector<EventStoreType>   ← raw (ungained) data values
  ├── m_time: QVector<quint32>          ← time offsets from m_first (for EVL_Event type)
  ├── m_type: EVL_Waveform | EVL_Event
  ├── m_gain, m_offset                  ← to convert raw → display: value = raw * gain + offset
  ├── m_rate                            ← sample rate (for waveforms)
  └── m_first, m_last: qint64          ← time range (ms since epoch)
```

### Key Relationships

- **Profile → Day**: `Profile::daylist` maps dates to Day objects. This is the master index.
- **Machine → Day**: `Machine::day` maps dates to the same Day objects (a secondary index).
- **Day → Sessions**: A Day contains sessions from potentially multiple Machines.
- **Session → Machine**: Each Session belongs to exactly one Machine.
- **Session → EventLists**: Each channel (ChannelID) can have multiple EventLists (e.g., multiple waveform segments).

### The "OSCAR Day" Concept

An OSCAR day starts at **noon** and runs until noon the following calendar day. This means a sleep session that starts at 11pm on March 15 and ends at 7am on March 16 belongs to the "March 15" day. The `Machine::pickDate()` method handles this mapping.

### Time Units

- All times in Session, EventList, and SessionSlice are **milliseconds since epoch**.
- In the database, `sessions.start_time` is also **milliseconds** since epoch (use `start_time/1000` in SQLite date functions).

---

## 4. The Import Pipeline

### How SD Card Data Gets Into OSCAR

```
User clicks Import
        │
        ▼
MainWindow::on_action_Import_Data_triggered()
        │
        ▼
detectCPAPCards()  ← scans drives, each registered
        │            loader's Detect() is called
        ▼
importCPAPDataCards()
        │
        ▼
For each detected card, MainWindow::importCPAP():
  new ProfileImportContext, loader->SetContext(ctx)
  DatabaseManager::transaction()   ← one transaction for the whole import
        │
        ▼
  MachineLoader::Open(path)
    Loader reads SD card files, creates Sessions
    with EventLists (waveforms, events, settings)
        │
        ▼
  ImportContext::AddSession() → Session::UpdateSummaries()
                              → writes session + events to database
        │
        ▼
  ImportContext::Commit() → Machine::AddSession() for each session
                            (links into Machine::sessionlist, creates/updates
                             Day in Profile::daylist), refreshes daily_summaries
        │
        ▼
  DatabaseManager::commit()  (rollback on failure)
        │
        ▼
finishCPAPImport() → regenerates statistics, reloads UI
```

Some loaders (BMC, G3X, Resvent) call `Machine::AddSession()` directly instead of
`ImportContext::AddSession()`. `importCPAP()` detects this via
`MachineLoader::dailySummariesCalculated()` and refreshes `daily_summaries` for them.
The progress dialog's Abort button calls `MachineLoader::abortImport()`.

### Loader Registration

All loaders register at startup in `main.cpp`:
```cpp
PRS1Loader::Register();    // Philips Respironics
ResmedLoader::Register();  // ResMed
BmcLoader::Register();     // BMC legacy
BmcG3xLoader::Register();  // BMC G3X modern
// ... etc.
```

Each `::Register()` calls the global `RegisterLoader()` which adds to a global list. `GetLoaders()` retrieves all registered loaders, optionally filtered by `MachineType`.

### Loader Class Hierarchy

```
MachineLoader (QObject)
  └── CPAPLoader
       ├── PRS1Loader
       ├── ResmedLoader
       ├── SleepStyleLoader
       ├── BmcLoader
       ├── BmcG3xLoader
       └── ... (all CPAP loaders)
  Also direct subclasses for non-CPAP:
  ├── CMS50Loader, CMS50F37Loader, MD300W1Loader, ViatomLoader (oximeters)
  ├── ZEOLoader, DreemLoader, AppleHealthLoader (sleep stage)
  ├── SomnoposeLoader (position)
  └── ... etc.
```

The CPAP loaders, oximeter loaders, and `AppleHealthLoader` are registered in `main.cpp`.
The ZEO, Dreem, Somnopose, and M-Series loaders are not; their *Data* menu actions in
`mainwindow.cpp` instantiate them on demand (Apple Health import also runs from a menu
action).

### ImportContext

`ImportContext` is an abstraction layer between loaders and the storage system. The primary implementation is `ProfileImportContext`. Key methods:
- `CreateMachineFromInfo()` — looks up or creates a Machine for the device
- `CreateSession()` — creates an in-memory Session for the loader to populate
- `SessionExists()` — lets the loader skip sessions already imported
- `AddSession()` — computes summaries and writes the session to the database
- `Commit()` — attaches the batch's sessions to their Machines/Days and refreshes
  `daily_summaries` for the affected dates

---

## 5. The Channel System

### schema::Channel

Every data signal (pressure, flow rate, AHI events, SpO2, etc.) is represented by a `schema::Channel` with a unique `ChannelID` (a `quint32`).

Channels are defined in `schema.cpp` via `schema::init()`. Each has:
- **id**: Numeric identifier (e.g., `CPAP_Pressure`, `OXI_SPO2`)
- **type**: `ChanType` — DATA, SETTING, FLAG, MINOR_FLAG, SPAN, WAVEFORM
- **machtype**: Which device type uses this channel (MT_CPAP, MT_OXIMETER, etc.)
- **fullname, label, unit, description**: Display strings (translatable)
- **thresholds**: Upper/lower thresholds for statistical calculations

Global channel variables are declared in `machine_common.h` (e.g., `CPAP_Pressure`, `CPAP_FlowRate`, `OXI_Pulse`).

The global channel list is `schema::channel` — a `ChannelList` that can be indexed by ID or by name string.

Loaders often define additional channels specific to their CPAP machine.

---

## 6. The UI Layer

### Tab Structure

MainWindow contains a QTabWidget with these tabs:
0. **Profile** (`ProfileSelector`) — profile list; shown when no profile is open
1. **Welcome** (`Welcome`) — startup page
2. **Daily** (`Daily`) — single-day view with detailed graphs
3. **Overview** (`Overview`) — multi-day summary charts
4. **Statistics** — HTML page generated by `statistics.cpp` into `ui->statisticsView`
   (the tab itself is defined in `mainwindow.ui`)

A **Help Browser** tab exists in code but is compiled out (`DEFINES += helpless` in
`oscar.pro`). A left-hand `QToolBox` holds the navigation, bookmarks, and records pages.

### Profile Open/Close Flow

```
ProfileSelector → user double-clicks profile
  → MainWindow::OpenProfile(name)
    → Profile::OpenMachines()           (loads machine list from DB)
    → Profile::LoadMachineData()        (loads all session summaries)
    → Profile::loadExtendedDataFromDatabase()  (user info, doctor info, prefs)
    → MainWindow::Startup()             (timer-delayed)
      → Daily::ReloadGraphs()
      → Overview::ReloadGraphs()
      → GenerateStatistics()
```

When closing:
```
MainWindow::CloseProfile()
  → Daily::Unload()                    (saves journal changes)
  → Profile::UnloadMachineData()       (frees session memory)
  → Profile::Save()                    (saves preferences)
```

### The Graphing Engine

Located in `oscar/Graphs/`. Key classes:

```
gGraphView (QOpenGLWidget; QWidget in BROKEN_OPENGL_BUILD)
  ├── Contains a list of gGraph objects
  ├── Handles scrolling, zooming, mouse interaction
  └── Renders all graphs in a vertical stack

gGraph (QObject)
  ├── Contains a list of Layer objects
  ├── Has a title, height, group (for linked X-axis zooming)
  └── Handles per-graph layout of its layers

Layer (base class)
  ├── gLineChart        ← waveform/data line plots
  ├── gFlagsLine        ← event flag markers
  ├── gFlagsGroup       ← group of flag lines
  ├── gSummaryChart     ← overview bar/dot charts
  ├── gOverviewGraph    ← overview line charts
  ├── gXAxis            ← X axis labels
  ├── gYAxis            ← Y axis labels
  ├── gLineOverlay      ← overlay markers on waveforms
  └── gSegmentChart     ← pie charts
```

**Data Flow to Graphs:**
1. `Daily::LoadDate(date)` selects the date on the calendar; `Daily::Load(date)` gets the
   `Day*` from `profile->GetDay(date)` and closes the previous day's events
2. Calls `day->OpenEvents()` to load event data from database into memory
3. Each `gGraph` has layers, each layer is given the `Day*` via `Layer::SetDay(day)`
4. Layers query the Day/Sessions for their specific ChannelID data
5. `gGraphView::updateGL()` triggers rendering

---

## 7. The Database Layer

### DatabaseManager

Singleton at `DatabaseManager::instance()`. Wraps a single SQLite connection with WAL mode. Provides `transaction()`, `commit()`, `rollback()`.

### Repository Pattern

Each database table has a corresponding repository class in `oscar/database/`:

| Repository | Table(s) | Purpose |
|-----------|----------|---------|
| `ProfileRepository` | `profiles` | Profile CRUD |
| `MachineRepository` | `machines` | Device CRUD |
| `SessionRepository` | `sessions` | Session metadata |
| `SessionSettingsRepository` | `session_settings` | Per-session device settings |
| `SessionSummariesRepository` | `session_summaries` | Pre-calculated session stats |
| `SessionChannelsRepository` | `session_channels` | Channel availability per session |
| `EventListRepository` | `event_lists` | EventList metadata |
| `EventDataRepository` | `event_data` | Waveform/event BLOB storage |
| `RespiratoryEventsRepository` | `respiratory_events` | Individual respiratory events |
| `SessionSlicesRepository` | `session_slices` | Mask-on/off periods |
| `SessionChannelValuesRepository` | `session_channel_values` | Value/time summaries for weighted averages |
| `DailySummaryRepository` | `daily_summaries` | Pre-calculated daily stats; one row per `(profile_id, date)` across all devices |
| `ChannelRepository` | `channels` | Channel definitions |
| `ChannelOptionsRepository` | `channel_options` | Channel display preferences |
| `UserInfoRepository` | `user_info` | Personal information |
| `DoctorInfoRepository` | `doctor_info` | Doctor information |
| `PreferencesRepository` | `profile_preferences` | Key-value profile settings |
| `AppPreferencesRepository` | `app_preferences` | Application-wide settings (replaced `Preferences.xml` in v14) |
| `GraphLayoutsRepository` | `graph_layouts` | Saved graph layouts (replaced `.shg` files in v14) |
| `DeviceTimeCorrectionRepository` | `device_time_corrections` | Per-device, per-night clock corrections (v17) |
| `ReportTreeRepository` | `report_tree` | Hierarchical CSV-export report tree |

`ReportRepository` and `ReportContentsRepository` target the pre-v13
`reports`/`report_contents` tables, which are no longer created. Only the legacy
`ReportManager` dialog uses them, and its menu action is hidden.

### Schema Versioning and Migrations

Current schema version: **19** (`DatabaseSchema::CURRENT_SCHEMA_VERSION` in
`database_schema.h`). Full table/column reference and version history:
`Notes/Database/DATABASE_SCHEMA_REFERENCE.md`.

**Policy: every schema change requires a migration that preserves user data.** OSCAR 2.0
is released, so recreating or wiping the database on a version mismatch is not allowed.

How upgrades work:

- `DatabaseManager::initialize()` reads the stored version. If it is older than
  `CURRENT_SCHEMA_VERSION`, it records `pendingSchemaUpgradeFrom()` and does not finish
  opening. If it is newer, it refuses to open and tells the user to update OSCAR.
- `main.cpp` (`confirmAndUpgradeDatabase()`) asks the user before upgrading and offers to
  copy the database to `oscar-backup-v<N>-<timestamp>.db` first.
- `DatabaseSchema::upgradeSchema()` walks a table of consecutive steps
  (`migrateV13ToV14` … `migrateV18ToV19`), advancing the stored version after each one.
  Migrations are additive; no existing data is destroyed. There is no upgrade path from
  versions below 13.
- Backups restore into the current schema if their version is within
  `[MIN_RESTORE_SCHEMA_VERSION, CURRENT_SCHEMA_VERSION]` (currently 12–19).

To change the schema:

1. Bump `CURRENT_SCHEMA_VERSION`.
2. Update the `createXxx` table functions so new databases get the new shape.
3. Write `migrateV<N>ToV<N+1>()` and add it to the `steps[]` table in `upgradeSchema()`.
   Existing rows must keep their data; backfill new columns where the old data allows.
4. Check that backup/restore still handles the previous version.
5. Update `DATABASE_SCHEMA_REFERENCE.md`.

---

## 8. The Preferences System

### Two-Level Architecture

1. **Application-wide** (`p_pref` global, `Preferences` class)
   - Stored in the `app_preferences` table (since schema v14). A leftover
     `Preferences.xml` in the data folder is imported once on first launch and deleted.
   - `QSettings` holds only bootstrap values, such as the data-folder location and the
     recent-databases list
   - Wrapped by `AppSetting` (`AppWideSetting` class) for typed access
   - Contains: language, UI settings, graph settings, etc.

2. **Per-profile** (`Profile` extends `Preferences`)
   - Stored in database (`profile_preferences` table)
   - Accessed via sub-objects: `profile->cpap`, `profile->oxi`, `profile->user`, etc.
   - These sub-objects (`CPAPSettings`, `OxiSettings`, `UserInfo`, `DoctorInfo`, etc.) all inherit from `PrefSettings` which wraps the parent Profile's preference map

### How Preferences Flow

```
PrefSettings base class
  ├── setPref(name, value)  →  (*m_pref)[name] = value
  └── getPref(name)         →  (*m_pref)[name]

Where m_pref points to the Profile (which IS a Preferences).
All sub-objects share the same QHash<QString, QVariant>.
```

---

## 9. Module Dependencies

```
main.cpp
  → MainWindow
      → Daily, Overview, Statistics, Welcome, ProfileSelector
      → gGraphView → gGraph → Layers
  → SleepLib
      → Profile → Machine → Session → EventList
      → MachineLoader (and all loader plugins)
      → schema::Channel
      → Preferences
  → Database
      → DatabaseManager
      → *Repository classes
      → MigrationManager
  → Network (profile sharing; used by ShareDialog)
      → CloudUploader (0x0.st) / CloudDownloader (fetches shared links)
      → OAuth2Handler
      → Dropbox / Google Drive uploaders
```

### Key Global Variables

| Variable | Type | Declared | Purpose |
|----------|------|----------|---------|
| `p_pref` | `Preferences*` | `profiles.h` | Application-wide preferences |
| `p_profile` | `Profile*` | `profiles.h` | Currently open profile |
| `AppSetting` | `AppWideSetting*` | `appsettings.h` | Typed app-wide settings |
| `mainwin` | `MainWindow*` | `main.cpp` | Main window instance |
| `schema::channel` | `ChannelList` | `schema.h` | Global channel registry |

---

## 10. Key Files Quick Reference

| Area | Key Files |
|------|-----------|
| Entry point | `oscar/main.cpp` |
| Main window | `oscar/mainwindow.{h,cpp}` |
| Daily view | `oscar/daily.{h,cpp}` |
| Overview | `oscar/overview.{h,cpp}` |
| Statistics | `oscar/statistics.{h,cpp}` |
| Reports/printing | `oscar/reports.{h,cpp}` |
| Profile system | `oscar/SleepLib/profiles.{h,cpp}` |
| Machine/Device | `oscar/SleepLib/machine.{h,cpp}` |
| Session | `oscar/SleepLib/session.{h,cpp}` |
| Events | `oscar/SleepLib/event.{h,cpp}` |
| Day container | `oscar/SleepLib/day.{h,cpp}` |
| Channel schema | `oscar/SleepLib/schema.{h,cpp}`, `machine_common.h` |
| Loader base | `oscar/SleepLib/machine_loader.{h,cpp}` |
| Import context | `oscar/SleepLib/importcontext.{h,cpp}` |
| Preferences | `oscar/SleepLib/preferences.{h,cpp}`, `appsettings.{h,cpp}` |
| Database core | `oscar/database/database_manager.{h,cpp}` |
| DB schema | `oscar/database/database_schema.{h,cpp}` |
| Loader plugins | `oscar/SleepLib/loader_plugins/` (one per device brand) |
| Graph engine | `oscar/Graphs/gGraphView.{h,cpp}`, `gGraph.{h,cpp}`, `layer.{h,cpp}` |
| Graph types | `oscar/Graphs/gLineChart.{h,cpp}`, `gSummaryChart.{h,cpp}`, etc. |
| Backup/Restore | `oscar/database/backup/profile_backup.{h,cpp}`, `profile_restore.{h,cpp}` |
| Network/Cloud | `oscar/network/cloud_uploader.h`, etc.; `oscar/sharedialog.{h,cpp}` |
| Journal | `oscar/SleepLib/journal.{h,cpp}` |
| CSV export | `oscar/exports/report_exporter.{h,cpp}` (CSV Export Wizard), `oscar/database/report_tree_repository.{h,cpp}`, `oscar/database/orf_file_io.{h,cpp}`, `oscar/docs/system_reports.orf`; 1.7.x-style export: `oscar/exports/exportcsv.*` |
| Profile import (OSCAR 1.x) | `oscar/profileimporter.{h,cpp}`, `oscar/importprofile.{h,cpp}`, `oscar/database/migration_manager.{h,cpp}` |
| Schema upgrades | `oscar/database/database_schema.cpp` (`upgradeSchema()`), `confirmAndUpgradeDatabase()` in `oscar/main.cpp` |
| Time corrections | `oscar/devicetimecorrectiondialog.*`, `oscar/driftanalysisdialog.*`, `oscar/database/device_time_correction_repository.*` |
| Build config | `oscar/oscar.pro` |

---

## 11. Common Patterns

### Loading Data On Demand

Session event data is **not** loaded at startup — only summary data is. When the user navigates to a specific day, `Day::OpenEvents()` loads event data from the database. When leaving the day, `Day::CloseEvents()` / `Session::TrashEvents()` frees the memory.

### The UpdateSummaries Flow

After import, `Session::UpdateSummaries()` is called to calculate statistics (min, max, avg, percentiles, counts-per-hour, etc.) from the raw EventList data. These summaries are cached in the Session and stored in the `session_summaries` database table.

### Journal Sessions

Journal data (daily notes, weight, zombie meter, bookmarks) is stored as a special Session belonging to a Machine of type `MT_JOURNAL`. The Daily view creates/loads journal sessions via `Daily::GetJournalSession()`.

### How Calculations Reach the UI

```
Profile::calcAvg(channel, machineType, startDate, endDate)
  → iterates Profile::daylist in date range
    → Day::avg(channel)
      → iterates Day::sessions
        → Session::avg(channel)
          → builds from EventList data or returns cached value
```

The Statistics page and Overview charts use these Profile-level calculation methods.
