# OSCAR 2.x — Orientation for New Developers

**Last updated:** 2026-10-05 (version 2.1.0-beta, database schema v19)

This guide gives you a map of OSCAR: what it does, how a user works with it, how the code
is organized, and how data flows from an SD card to a graph on screen. It is an
orientation, not a reference. Follow the links at the end for detail.

---

## 1. What OSCAR is

OSCAR (Open Source CPAP Analysis Reporter) reads the data that CPAP and related sleep
devices write to their SD cards, stores it permanently, and presents it as graphs,
statistics, and reports. Users are mostly CPAP patients who want to understand their
therapy; some are clinicians or volunteers who help them.

- **Language/framework:** C++17, Qt 6 (currently built with Qt 6.10.2). No Qt5 support,
  no `#if QT_VERSION` guards.
- **Storage:** a single SQLite database (`oscar.db`) holding every profile's data.
- **Platforms:** Windows, macOS, and Linux. The release ships to about 25 build
  environments, so platform-specific APIs are not allowed.
- **Lineage:** OSCAR 1.x stored data in per-session binary files. OSCAR 2.x (this
  codebase) moved everything into SQLite. The 1.7.x line lives in a separate repository
  and only receives occasional backports. "OSCAR" in these notes means 2.x.

---

## 2. How a user works with OSCAR

Knowing the user's workflow makes the code much easier to navigate.

1. **First run.** OSCAR asks where to put its data folder (default
   `Documents/OSCAR20_Data`) and offers to migrate an OSCAR 1.x `OSCAR_Data` folder.
2. **Profiles.** Each person whose data is tracked has a *profile*. The Profile tab
   (`ProfileSelector`) lists them; opening one loads its devices and day summaries.
3. **Import.** The user inserts an SD card and clicks Import (Shift+F2). OSCAR scans
   drives, asks each registered loader whether it recognizes the card, and imports new
   sessions. Oximeters, sleep-stage devices, and Apple Health exports are imported from
   the *Data* menu instead.
4. **Viewing data.** The main tabs are:
   - **Welcome** — summary of the most recent night.
   - **Daily** — one OSCAR day: calendar, event list, per-session details, and a stack of
     zoomable graphs (flow, pressure, leak, events, SpO2, ...).
   - **Overview** — one bar/line chart per metric across a date range.
   - **Statistics** — an HTML table of averages and percentiles over standard periods.
   - A left-hand toolbox with navigation, bookmarks, and records.
5. **Output.** Print reports (`reports.cpp`), the CSV Export Wizard (*File / Export
   Data*), profile backup/restore/share as `.oscar` packages (*File / Profiles*), and
   direct SQL access to `oscar.db` with any SQLite tool.

**The OSCAR day.** A day runs from noon to noon (the split time is a profile setting,
default 12:00). A session starting at 23:00 on March 15 belongs to March 15 even though
most of it falls on March 16. `Machine::pickDate()` does this mapping. Keep it in mind
whenever you work with dates.

---

## 3. Repository layout

| Path | Contents |
|------|----------|
| `oscar/` | The application. `main.cpp` is the entry point; `oscar.pro` is the qmake project. Top-level `.cpp/.h/.ui` files are the UI: main window, tabs, dialogs. |
| `oscar/SleepLib/` | The data core: profiles, devices, days, sessions, events, channels, calculations, preferences, import plumbing. |
| `oscar/SleepLib/loader_plugins/` | One loader per device family. These parse SD cards and files. |
| `oscar/SleepLib/thirdparty/` | Bundled Botan (crypto) and miniz (zip). **Never modify.** |
| `oscar/database/` | SQLite access: `DatabaseManager`, schema and migrations, one repository class per table area. |
| `oscar/database/backup/` | `.oscar` package export/restore. |
| `oscar/Graphs/` | The custom graphing engine (`gGraphView`, `gGraph`, layers). |
| `oscar/exports/` | CSV export wizard and journal-notes export. |
| `oscar/network/` | Cloud upload/download for profile sharing (0x0.st, Dropbox, Google Drive, OAuth2). |
| `oscar/docs/` | Files compiled into the app as resources: `channels.xml`, `graphs.xml`, `system_reports.orf`, tooltips, startup tips, time zones. |
| `oscar/tests/` | Qt Test unit tests (built with `CONFIG+=test`). |
| `Htmldocs/` | About, credits, and release notes shown in the app. |
| `Translations/` | Qt `.ts` translation files, one per language. |
| `Building/` | Per-platform build instructions and scripts (`Windows/`, `MacOS/`, `Linux/`). |
| `Tools/` | Python helpers, mostly for translations and maintenance. |
| `anotDump/`, `dumpSTR/` | Small standalone tools for dumping ResMed EDF files. |
| `Notes/` | Design notes, investigations, plans, specs, and the bug-fix log. See §9. |

---

## 4. Architecture at a glance

```
┌─────────────────────────── UI (oscar/*.cpp) ───────────────────────────┐
│ MainWindow ── ProfileSelector · Welcome · Daily · Overview · Statistics │
│              dialogs: Preferences, Import, Backup/Restore, CSV export… │
│                         │                                              │
│              Graphs/: gGraphView → gGraph → Layers                     │
└─────────────────────────┬──────────────────────────────────────────────┘
                          │ reads Day/Session objects
┌─────────────────────────▼─────────── SleepLib ─────────────────────────┐
│ Profile → Machine → Day → Session → EventList                          │
│ schema::Channel registry · calcs (flow analysis, AHI, leaks)           │
│ MachineLoader plugins → ImportContext                                  │
└─────────────────────────┬──────────────────────────────────────────────┘
                          │ StoreToDatabase / LoadFromDatabase
┌─────────────────────────▼────────────── database ──────────────────────┐
│ DatabaseManager (singleton, one SQLite connection, WAL)                │
│ *Repository classes · DatabaseSchema (create + versioned migrations)   │
└────────────────────────────────────────────────────────────────────────┘
```

### Startup (`oscar/main.cpp`)

1. Create the `QApplication`, pick the graphics backend, start the logger.
2. Parse command-line options (`--datadir`, `--profile`/`-p`, `--language`, `--legacy`).
3. Locate or create the data folder (`GetAppData()`, no trailing separator); take a
   `QLockFile` so only one instance uses it.
4. Open `oscar.db` via `DatabaseManager::instance().initialize()`. If the schema is
   older than `DatabaseSchema::CURRENT_SCHEMA_VERSION`, the user is asked before the
   upgrade runs (on a worker thread, behind a progress dialog).
5. `schema::init()` defines the built-in channels, then every CPAP and oximeter loader
   is registered (`PRS1Loader::Register()`, `ResmedLoader::Register()`, ...). This must
   happen before OSCAR 1.x migration, which needs the channel schema.
6. Offer OSCAR 1.x migration if the folder is new.
7. `Profiles::Scan()`, then `MainWindow::SetupGUI()`, then the event loop.

### Opening a profile (`MainWindow::OpenProfile`)

`Profile::OpenMachines()` loads the device list, `Profile::LoadMachineData()` loads
session *summaries* (not waveforms) into memory and builds the day index, then the
Welcome/Daily/Overview tabs are created and `GenerateStatistics()` builds the Statistics
page. `MainWindow::CloseProfile()` unloads it again.

---

## 5. The data model (`oscar/SleepLib/`)

| Class | File | Role |
|-------|------|------|
| `Profile` | `profiles.h` | One user. Owns the devices and the master `QMap<QDate, Day*>`. Also a `Preferences` map, exposed through typed sub-objects (`profile->cpap`, `->oxi`, `->session`, `->user`, ...). Has range calculations such as `calcAvg()` and `calcCount()`. |
| `Machine` | `machine.h` | One physical device (CPAP, oximeter, sleep-stage, position, journal; see `MachineType`). Holds `MachineInfo` (brand, model, serial, loader name) and its sessions. Historically "machine"; the UI says "device". |
| `Day` | `day.h` | All sessions, from any device, that fall in one OSCAR day. Aggregates statistics across them. |
| `Session` | `session.h` | One continuous recording. Holds settings, summary caches (count, sum, avg, min, max, percentiles, counts-per-hour), mask-on/off slices, and event lists. |
| `EventList` | `event.h` | A time series for one channel: either a waveform (fixed rate) or timestamped events. Values are stored raw with `gain`/`offset`. |
| `schema::Channel` | `schema.h`, `machine_common.h` | Describes every signal: ID, type (`DATA`, `SETTING`, `FLAG`, `MINOR_FLAG`, `SPAN`, `WAVEFORM`), device type, names, units, colours. Global IDs like `CPAP_Pressure`, `CPAP_Obstructive`, `OXI_SPO2` are declared in `machine_common.h`. Loaders add their own channels in `initChannels()`. |

**Conventions to know:**

- All in-memory and stored timestamps are **milliseconds since the Unix epoch**.
- **Lazy loading:** only summaries are in memory after a profile opens.
  `Day::OpenEvents()` → `Session::OpenEvents()` loads waveforms/events from the database
  when the Daily view shows a day; `Day::CloseEvents()` frees them.
- **Summaries:** `Session::UpdateSummaries()` computes statistics from the event lists
  at import time and they are stored in `session_summaries`. Per-day rollups live in
  `daily_summaries` and feed Overview and Statistics.
- **Derived data:** `calcs.cpp` computes what the device doesn't supply: respiratory
  rate, tidal/minute volume, flow limitation, AHI-over-time, leak, SpO2 drops and pulse
  changes, user-defined event flagging (`FlowParser`, `calcRespRate`, `calcLeaks`, ...).
- **Journal:** notes, weight, bookmarks, etc. are stored as sessions of a device of type
  `MT_JOURNAL` (`journal.cpp`, `Daily::GetJournalSession()`).
- **Preferences:** app-wide settings are in `p_pref` / `AppSetting` (`appsettings.h`),
  persisted in the `app_preferences` table. Per-profile settings live in
  `profile_preferences`. `QSettings` only stores bootstrap data such as the data-folder
  location and recent databases.

Key globals: `p_profile` (open profile), `p_pref`, `AppSetting`, `mainwin`,
`schema::channel`.

---

## 6. Importing data: loaders

### Loader interface (`machine_loader.h`)

Every loader derives from `MachineLoader`, CPAP loaders from `CPAPLoader`. The core
overrides are:

- `Detect(path)` — return true if this loader recognizes the card or folder. Must be
  cheap and must not false-positive on other brands' cards.
- `PeekInfo(path)` — read model and serial without importing.
- `Open(path)` — import. Returns the number of new sessions, or a negative value on error.
- `initChannels()` — register device-specific channels.
- `Version()` / `loaderName()` — identify the loader. When a loader's version changes,
  OSCAR asks the user to purge that device's data and re-import it from the `Backup`
  folder (`profiles.cpp`).

### The import pipeline

```
MainWindow::on_action_Import_Data_triggered()
  → detectCPAPCards()            every registered loader's Detect() is tried on each drive
  → importCPAPDataCards()
      → importCPAP()             per card:
          create ProfileImportContext, attach it to the loader
          DatabaseManager::transaction()          (one transaction per import)
          loader->Open(path)
              → ctx->CreateMachineFromInfo()      find or create the Machine
              → ctx->SessionExists(id)            skip sessions already imported
              → ctx->CreateSession(id), fill it with settings and EventLists
              → ctx->AddSession(session)          UpdateSummaries() + write to DB
          ctx->Commit()                           attach sessions to Days, refresh daily_summaries
          DatabaseManager::commit()
  → finishCPAPImport()           regenerate statistics, reload tabs
```

Large loaders (ResMed, PRS1) parallelize per-session work with `queTask()`/`runTasks()`
on the global `QThreadPool`. Some older loaders still call `Machine::AddSession()`
directly instead of using `ImportContext`; `importCPAP()` compensates for them. The
progress dialog's Abort button calls `MachineLoader::abortImport()`, and loaders must
check for it so Cancel takes effect within seconds.

Most loaders also copy the card into a per-device `Backup` folder under the profile
directory, so the database can be rebuilt (*Data / Rebuild CPAP Data*) without the
original card.

### Loaders by family

| Loader | Device(s) |
|--------|-----------|
| `resmed_loader`, `resmed_EDFinfo`, `edfparser` | ResMed S9, AirSense/AirCurve 10/11, Lumis (EDF files) |
| `prs1_loader`, `prs1_parser*` | Philips Respironics System One and DreamStation (encrypted DS2 uses `crypto.cpp`/Botan) |
| `mseries_loader` | Respironics M-Series (file import from *Data* menu) |
| `icon_loader`, `sleepstyle_loader` | Fisher & Paykel ICON and SleepStyle |
| `intellipap_loader` | DeVilbiss IntelliPAP and DV6 |
| `prisma_loader`, `weinmann_loader` | Löwenstein / Weinmann prisma line; older Weinmann devices |
| `resvent_loader` | Resvent iBreeze and Hoffrichter Point 3 |
| `bmc_loader`, `bmcg3x_loader` (+ `*DataParsing`) | BMC legacy formats and BMC G3/G3X `.00x` format |
| `apex_loader` | Apex Medical XT |
| `sefam_loader` | SEFAM devices |
| `vrem_loader` | vTitan vREM |
| `yuwell_loader` | Yuwell |
| `cms50_loader`, `cms50f37_loader`, `md300w1_loader`, `viatom_loader` | Pulse oximeters (not CPAP) |
| `zeo_loader`, `dreem_loader`, `applehealth_loader` | Sleep stages (Apple Health also imports vitals and weight) |
| `somnopose_loader` | Body position |

Oximeter import has its own UI (`oximeterimport.cpp`, `SleepLib/serialoximeter.cpp`).
`deviceconnection.cpp` with `xmlreplay.cpp` can record and replay serial-device traffic
for tests.

**Rule:** a change to one loader must not affect any other loader unless that is the
explicit goal. Card formats and detection fingerprints are documented in
`Notes/loaders/`.

---

## 7. The database layer (`oscar/database/`)

- **`DatabaseManager`** — singleton wrapping one SQLite connection in WAL mode. Provides
  `transaction()`, `commit()`, `rollback()`, integrity checks, WAL checkpoints, and
  `snapshotTo()` for backups.
- **`DatabaseSchema`** (`database_schema.{h,cpp}`) — creates tables and runs versioned,
  additive migrations up to `CURRENT_SCHEMA_VERSION` (currently **19**). Released
  databases must never lose data. A schema change means a new version and a migration
  step, plus an update to the schema reference document.
- **Repositories** — one class per table group (`SessionRepository`,
  `SessionSummariesRepository`, `EventDataRepository`, `DailySummaryRepository`,
  `MachineRepository`, `PreferencesRepository`, `GraphLayoutsRepository`,
  `ReportTreeRepository`, ...). UI and SleepLib code go through these rather than writing
  raw SQL.
- **Main tables:** `profiles`, `machines`, `sessions`, `session_settings`,
  `session_summaries`, `session_channels`, `session_slices`, `event_lists` +
  `event_data` (waveform/event BLOBs), `respiratory_events`, `daily_summaries`,
  `app_preferences`, `profile_preferences`, `graph_layouts`, `report_tree`,
  `device_time_corrections`.
- **`MigrationManager`, `profileimporter.cpp`** — convert OSCAR 1.x file-based profiles.
- **`backup/`** — `ProfileBackup`/`ProfileRestore` build and read `.oscar` packages (a
  ZIP with a `manifest.json`, one SQL export file per table, and, for "Everything"
  backups, the SD-card backup folders), used by *Backup Profile*, *Restore
  Profile*, and *Share Profile*.

The data folder holds `oscar.db` (with `-wal`/`-shm` files while open), a `Logs` folder,
and `Profiles/<name>/<device>/Backup` folders with the SD-card copies.

---

## 8. The UI and graphing engine

| Area | Files |
|------|-------|
| Main window, menus, import orchestration | `mainwindow.{h,cpp,ui}` |
| Daily view (and search tab) | `daily.*`, `dailySearchTab.*`, `sessionbar.*` |
| Overview | `overview.*` |
| Statistics page (HTML) | `statistics.*` |
| Welcome page | `welcome.*` |
| Printing | `reports.*` |
| Preferences | `preferencesdialog.*` |
| CSV Export Wizard | `exports/report_exporter.*`, `database/report_tree_*`, `database/orf_file_io.*`, `docs/system_reports.orf`, `sqleditor.*` |
| Profiles | `profileselector.*`, `newprofile.*`, `importprofile.*`, `backupdialog.*`, `restoredialog.*`, `sharedialog.*` |
| Clock drift / time corrections | `driftanalysisdialog.*`, `driftplotwidget.*`, `devicetimecorrectiondialog.*` |
| Update check, about, logging | `checkupdates.*`, `aboutdialog.*`, `logger.*` |

**Graphs (`oscar/Graphs/`).** OSCAR draws its own graphs instead of using Qt Charts.
`gGraphView` is the scrollable container, `gGraph` is one graph with a title, height, and
linked-zoom group, and each graph is built from `Layer` subclasses: `gLineChart`
(waveforms and values), `gFlagsLine` (event flags), `gLineOverlay`, `gSummaryChart` and
its variants (Overview bars such as `gAHIChart`, `gUsageChart`, `gPressureChart`),
`gXAxis`/`gYAxis`, `gSegmentChart`, `MinutesAtPressure`, and others. A layer receives a
`Day*` and draws the channels it was configured for. The user's graph order, heights, and
visibility are saved in the `graph_layouts` table.

UI conventions: give visible feedback when the user clicks something, honour Cancel
within a few seconds, use `QLocale` for date formatting (Qt 6 `QDate::toString` ignores
the system locale), and prefer hiding over disabling-with-stylesheet.

---

## 9. Where to find things in `Notes/`

`Notes/` is part of the public repository: never put serial numbers, personal names, or
sample-data identifiers in it.

| Path | What's there |
|------|--------------|
| `Notes/Developer Notes/BUG_FIXES.md` | Log of every non-trivial bug fix: symptom, root cause, fix. Search it before debugging something that looks familiar. |
| `Notes/Developer Notes/ARCHITECTURE.md` | Deeper architecture walkthrough: startup, import pipeline, data model, graphs, repositories, and how to add a schema migration. |
| `Notes/Developer Notes/DATABASE_INTEGRATION_GUIDE.md`, `SIGNALS_AND_SLOTS_GUIDE.md`, `PERFORMANCE_INSTRUMENTATION_GUIDE.md` | How-to guides for specific subsystems. |
| `Notes/Database/DATABASE_SCHEMA_REFERENCE.md` | Every table and column, with version history. |
| `Notes/Database/Database-ER-Diagram.png`, `USEFUL_QUERIES.sql`, `HOW_TO_USE_QUERIES.md`, `DATA_DICTIONARY.md` | Schema diagram and ready-made SQL. |
| `Notes/Accessing OSCAR Data/` | Reading `oscar.db` from Python, including decoding `event_data` BLOBs. |
| `Notes/loaders/` | Per-brand SD-card analyses, `SD_CARD_FINGERPRINTS.md`, `LOADER_DETECTION_PATTERNS.md`. |
| `Notes/specs/`, `Notes/plans/` | Dated design specs and implementation plans for features. |
| `Notes/Wiki/` | Backup of the user-facing wiki pages, useful for seeing the user's view. |
| `Notes/Getting Started with OSCAR 2.0.md` | User-facing introduction to 2.0. |
| `Notes/temp/` | Older notes that were moved out of the way. |

---

## 10. Building, testing, and contributing

- **Build:** open `oscar/oscar.pro` in Qt Creator with a Qt 6.10.x kit and build. Platform
  notes are in `Building/Windows/BUILD-WIN-Qt6.md`, `Building/MacOS/BUILD-mac.md`, and
  `Building/Linux/BUILD_Linux.md`. When you add a source file, add it to `oscar.pro`.
- **Version:** set in `oscar/VERSION`.
- **Unit tests:** run qmake with `CONFIG+=test` to build the `test` target from
  `oscar/tests/` (loader, session, crypto, and device-connection tests; sample data in
  `oscar/tests/testdata`).
- **API docs:** `makedoxy.bat` runs Doxygen over `oscar/` into `doxydoc/`.
- **Translations:** user-visible strings go through `tr()`. `.ts` files in
  `Translations/` are updated in batches with `lupdate`.
- **Source control:** GitLab, https://gitlab.com/Seeker4/OSCAR-code. Work on `master`;
  see `CONTRIBUTING.md` for branching and merge conflicts.
- **Bugs:** tracked in GitLab Issues. Reference the issue with `Closes #N` in the commit
  body and add an entry to `BUG_FIXES.md`.

### Code conventions

- C++17. Must compile cleanly with Qt 6.10.2, using Qt 6 APIs only.
- Cross-platform only; no OS-specific APIs.
- Every `.h` and `.cpp` gets a header describing the module and Doxygen comments for
  classes and functions. New files carry "Copyright (c) 2026 The OSCAR Team".
- Match the style of the surrounding code.
- Do not modify `oscar/SleepLib/thirdparty/`.
- When fixing a bug, look for *all* root causes before fixing. Past fixes often missed a
  second cause.

---

## 11. Suggested first steps

1. Build and run OSCAR, create a profile, and import a sample SD card. Click through
   Daily, Overview, and Statistics.
2. Open `oscar.db` in an SQLite browser and run a few queries from `USEFUL_QUERIES.sql`
   to see how sessions, summaries, and event data are stored.
3. Read `main.cpp`, then `MainWindow::OpenProfile()` and `MainWindow::importCPAP()`.
4. Pick one loader you have data for and step through `Detect()` and `Open()` in the
   debugger, following a session into `ImportContext::AddSession()` and
   `Session::StoreToDatabase()`.
5. Follow a day onto the screen: `Daily::Load()` → `Day::OpenEvents()` → the graph
   layers.
6. Skim `BUG_FIXES.md` to see which areas tend to break and why.
