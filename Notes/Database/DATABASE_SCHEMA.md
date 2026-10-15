# OSCAR Database Schema

**Version:** Schema Version 19
**Last Updated:** 2026 Q3
**Database Type:** SQLite

---

## Overview

The OSCAR database uses SQLite to store user profiles, machine configurations, session data, and preferences. This document provides a complete reference for all tables, fields, and relationships in schema version 19.

**Key Design Principles:**
- **Profile-centric**: All data organized around user profiles
- **Machine tracking**: Each profile can have multiple CPAP and oximetry devices
- **Session storage**: Detailed session metadata with waveform/event data in database ⚡ NEW IN v8
- **Daily summaries**: Pre-calculated daily statistics for fast reporting
- **Profile ID denormalization**: session_settings, session_channels, session_summaries, event_lists, and respiratory_events carry profile_id for query performance 🔧 NEW IN v12
- **Report tree**: Hierarchical report tree with system and user nodes, loaded from .orf file 🌲 NEW IN v13
- **Flexible preferences**: Key-value storage for settings
- **Cascade deletes**: Removing a profile removes all associated data
- **Database-only mode**: Waveform and event data stored in database BLOBs (replaces .001 files) ⚡ NEW IN v8
- **Migration policy**: Schema version mismatch requires migration after v13.

---

## Schema Version History

| Version | Date | Changes |
|---------|------|---------|
| 1 | 2025 Q4 | Initial schema with profiles, machines, user_info, doctor_info |
| 2 | 2025 Q4 | Added profile_preferences table |
| 3 | 2025 Q4 | Added session tables (sessions, session_settings, session_channels, respiratory_events, session_summaries, session_slices) |
| 4 | 2025 Q4 | Added profile status tracking (status, status_changed_at) |
| 5 | 2025 Q4 | Added channels tables (channels, channel_options) |
| 6 | 2025 Q4 | Added daily_summaries table for fast reporting |
| 7 | 2026 Q1 | 🐛 **BUG FIX**: Added session_channel_values table to persist value/time summaries (fixes incorrect weighted averages) |
| 8 | 2026 Q1 | ⚡ **MAJOR CHANGE**: Added event_lists and event_data tables - waveform/event data now stored in database instead of .001 files |
| 9 | 2026 Q1 | 📝 **ENHANCEMENT**: Added json_value column to session_settings for journal migration and complex data types |
| 10 | 2026 Q1 | 🔧 **SEMANTIC FIX**: Renamed central_count to unclassified_count (semantically correct) and added clear_airway_count to session_summaries |
| 11 | 2026 Q1 | 📊 **NEW FEATURE**: Added reports and report_contents tables for CSV export report management with macro-based query templates |
| 12 | 2026 Q1 | 🔧 **DENORMALIZATION**: Added profile_id to session_settings, session_channels, session_summaries, event_lists; added profile_id and channel_id to respiratory_events; added type to channels; removed events_file and summary_file from sessions. |
| 13 | 2026 Q1 | 🌲 **REPORT TREE REDESIGN**: Replaced reports/report_contents with single report_tree table; hierarchical structure with System/User roots; system reports loaded from external .orf file |
| 14 | 2026 Q2 | 🗄️ **FILE-TO-DB MIGRATION**: Added `app_preferences` table (replaces Preferences.xml); added `graph_layouts` table (replaces layoutSettings/*.shg and per-profile daily.shg/overview.shg); added `blob_value BLOB` column to `profile_preferences`. Legacy files imported once on first launch then deleted. |
| 15 | 2026 Q2 | 🧹 **CLEANUP**: Dropped `dst_enabled` column from `user_info` (stored but never read); deleted orphaned `DST` rows from `profile_preferences`. |
| 16 | 2026 Q2 | 🔧 **DESIGN FIX**: Dropped `machine_id` column, its FK to `machines`, and `idx_daily_summaries_profile_machine` from `daily_summaries`. Natural key is now `(profile_id, date)`. Each row is a profile-day rollup that already aggregates across all machines for that date — the per-machine dimension was a design mistake never used by callers. |
| 17 | 2026 Q3 | 🕐 **NEW FEATURE**: Added `device_time_corrections` table for per-device per-night time corrections (timezone, travel, dst, reset, offset, drift). |
| 18 | 2026 Q3 | 📊 **NEW FEATURE**: Central / Obstructive hypopnea split. Added `obstructive_hypopnea_count`, `central_hypopnea_count`, `all_apnea_count` (INTEGER) and `oahi`, `cahi` (REAL) to both `session_summaries` and `daily_summaries`. `all_apnea_count` closes a pre-existing gap — `CPAP_AllApnea` contributes to AHI but was never stored, so SQL sums could not reproduce the app's AHI for devices reporting an undifferentiated apnea. Purely additive; pre-v18 rows read 0 in all five columns and are **not** backfilled. |
| 19 | 2026 Q3 | 🔧 **SEMANTICS**: Summary metrics that do not apply are now `NULL`, not `0`, in `session_summaries` and `daily_summaries`: event counts for channels the device has never reported, `oahi`/`cahi` unless the device splits hypopneas by mechanism, `rdi` unless it reports RERA, and pressure/leak/oximetry statistics a row has no data for. No column changes. The #285 index recompute moved here from v18. Zero-count OH/CH `session_channels` rows of devices that never scored either are removed. `daily_summaries` rows of profiles whose CPAP devices disagree on some channel are regenerated on next open. A v18 backup restored into v19 keeps its `0`s. |

---

## Table Definitions

### 1. schema_version
Tracks database schema version for migrations.

```sql
CREATE TABLE schema_version (
    version INTEGER PRIMARY KEY,
    applied_at TEXT DEFAULT CURRENT_TIMESTAMP
)
```

### 2. profiles
Core profile table storing user account information.

```sql
CREATE TABLE profiles (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT UNIQUE NOT NULL,
    data_folder TEXT NOT NULL,
    status TEXT DEFAULT 'active' CHECK(status IN ('active', 'missing', 'archived')),
    status_changed_at TEXT,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT DEFAULT CURRENT_TIMESTAMP
)
```

### 3. machines
CPAP machines and devices associated with profiles.

```sql
CREATE TABLE machines (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    profile_id INTEGER NOT NULL,
    machine_id INTEGER NOT NULL,
    loader_name TEXT NOT NULL,
    machine_type INTEGER NOT NULL,
    brand TEXT,
    model TEXT,
    series TEXT,
    serial_number TEXT,
    model_number TEXT,
    last_imported TEXT,
    purge_date TEXT,
    data_version INTEGER DEFAULT 0,
    properties TEXT,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,
    UNIQUE(profile_id, machine_id)
)
```

### 4. user_info
Personal information for each profile.

```sql
CREATE TABLE user_info (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    profile_id INTEGER NOT NULL UNIQUE,
    dob TEXT,
    first_name TEXT,
    last_name TEXT,
    address TEXT,
    phone TEXT,
    email TEXT,
    country TEXT,
    height REAL,
    gender INTEGER,
    timezone TEXT,
    password_hash TEXT,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE
)
```

### 5. doctor_info
Doctor/medical provider information.

```sql
CREATE TABLE doctor_info (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    profile_id INTEGER NOT NULL UNIQUE,
    name TEXT,
    phone TEXT,
    email TEXT,
    practice_name TEXT,
    address TEXT,
    patient_id TEXT,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE
)
```

### 6. profile_preferences
Profile settings as key-value pairs.

```sql
CREATE TABLE profile_preferences (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    profile_id INTEGER NOT NULL,
    category TEXT NOT NULL,
    key TEXT NOT NULL,
    value TEXT,
    data_type TEXT,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,
    UNIQUE(profile_id, category, key)
)
```

**Categories:** cpap, oximetry, session, appearance, general

### 7. sessions
Core session metadata and timing.

```sql
CREATE TABLE sessions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL,
    machine_id INTEGER NOT NULL,
    start_time INTEGER NOT NULL,
    end_time INTEGER NOT NULL,
    duration INTEGER NOT NULL,
    enabled INTEGER DEFAULT 1,
    summary_only INTEGER DEFAULT 0,
    no_settings INTEGER DEFAULT 0,
    events_loaded INTEGER DEFAULT 0,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (machine_id) REFERENCES machines(id) ON DELETE CASCADE,
    UNIQUE(machine_id, session_id)
)
```

**Note:** `events_file` and `summary_file` columns were removed in v12 (legacy .001 file references no longer needed).

### 8. session_settings
Machine configuration for each session.

```sql
CREATE TABLE session_settings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL,
    profile_id INTEGER NOT NULL,
    channel_id INTEGER NOT NULL,
    value REAL NOT NULL,
    data_type TEXT,
    json_value TEXT,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,
    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,
    UNIQUE(session_id, channel_id)
)
```

**Note:** The `json_value` column (added in v9) stores complex data types as JSON for journal migration support. The `profile_id` column (added in v12) denormalizes the profile association for query performance.

### 9. session_channels
Summary statistics for each channel in a session.

```sql
CREATE TABLE session_channels (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL,
    profile_id INTEGER NOT NULL,
    channel_id INTEGER NOT NULL,
    count INTEGER DEFAULT 0,
    sum REAL DEFAULT 0,
    avg REAL DEFAULT 0,
    wavg REAL DEFAULT 0,
    min REAL DEFAULT 0,
    max REAL DEFAULT 0,
    median REAL DEFAULT 0,
    p90 REAL DEFAULT 0,
    p95 REAL DEFAULT 0,
    phys_min REAL DEFAULT 0,
    phys_max REAL DEFAULT 0,
    cph REAL DEFAULT 0,
    sph REAL DEFAULT 0,
    first_time INTEGER,
    last_time INTEGER,
    gain REAL DEFAULT 1.0,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,
    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,
    UNIQUE(session_id, channel_id)
)
```

**Note:** `profile_id` added in v12 for query performance denormalization.

### 10. session_channel_values 🐛 **NEW IN v7 - BUG FIX**
Detailed value/time summary data for each channel.

```sql
CREATE TABLE session_channel_values (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_channel_id INTEGER NOT NULL,
    value INTEGER NOT NULL,
    count INTEGER DEFAULT 0,
    time_ms INTEGER DEFAULT 0,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (session_channel_id) REFERENCES session_channels(id) ON DELETE CASCADE,
    UNIQUE(session_channel_id, value)
)
```

**Purpose:** Stores the `m_valuesummary` and `m_timesummary` hash data from the Session class. This data tracks how many times each distinct value occurred (`count`) and for how long (`time_ms`) for each channel. This information is critical for calculating weighted averages and other time-based statistics. **This table fixes a critical bug where this data was not being persisted to the database, causing incorrect statistics when sessions were loaded.**

### 11. respiratory_events
Individual respiratory events (apneas, hypopneas, RERAs).

```sql
CREATE TABLE respiratory_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL,
    profile_id INTEGER NOT NULL,
    channel_id INTEGER,
    event_type INTEGER NOT NULL,
    start_time INTEGER NOT NULL,
    end_time INTEGER NOT NULL,
    duration INTEGER NOT NULL,
    desaturation REAL,
    severity INTEGER,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,
    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE
)
```

**Event Types:** 0=Obstructive, 1=Unclassified, 2=Hypopnea, 3=RERA, 4=Clear Airway, 5=User-flagged

**Note:** `profile_id` and `channel_id` added in v12. `channel_id` references the OSCAR channel ID constant (not a FK to the channels table).

### 12. session_summaries
Cached high-level session summaries.

```sql
CREATE TABLE session_summaries (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL UNIQUE,
    profile_id INTEGER NOT NULL,
    ahi REAL DEFAULT 0,
    rdi REAL DEFAULT 0,
    oahi REAL DEFAULT 0,
    cahi REAL DEFAULT 0,
    obstructive_count INTEGER DEFAULT 0,
    unclassified_count INTEGER DEFAULT 0,
    hypopnea_count INTEGER DEFAULT 0,
    rera_count INTEGER DEFAULT 0,
    clear_airway_count INTEGER DEFAULT 0,
    obstructive_hypopnea_count INTEGER DEFAULT 0,
    central_hypopnea_count INTEGER DEFAULT 0,
    all_apnea_count INTEGER DEFAULT 0,
    pressure_avg REAL,
    pressure_min REAL,
    pressure_max REAL,
    pressure_95th REAL,
    leak_total_avg REAL,
    leak_total_95th REAL,
    leak_total_max REAL,
    spo2_avg REAL,
    spo2_min REAL,
    pulse_avg REAL,
    hours_used REAL DEFAULT 0,
    mask_on_hours REAL DEFAULT 0,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,
    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE
)
```

**Note:** In schema v10, `central_count` was renamed to `unclassified_count` (semantically correct), and `clear_airway_count` was added. The old `central_count` column is retained for backward compatibility but ignored by new code.

### 13. session_slices
Mask-on/mask-off periods within sessions.

```sql
CREATE TABLE session_slices (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL,
    start_time INTEGER NOT NULL,
    end_time INTEGER NOT NULL,
    status INTEGER NOT NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
)
```

**Status:** 0=mask off, 1=mask on

### 14. channels
Per-profile channel customizations.

```sql
CREATE TABLE channels (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    profile_id INTEGER NOT NULL,
    channel_id INTEGER NOT NULL,
    channel_code TEXT NOT NULL,
    type INTEGER,
    enabled INTEGER NOT NULL DEFAULT 1,
    default_color TEXT,
    fullname TEXT,
    label TEXT,
    description TEXT,
    lower_threshold REAL,
    lower_threshold_color TEXT,
    upper_threshold REAL,
    upper_threshold_color TEXT,
    show_in_overview INTEGER DEFAULT 0,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,
    UNIQUE(profile_id, channel_id)
)
```

### 15. channel_options
Lookup values for LOOKUP-type channels.

```sql
CREATE TABLE channel_options (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    channel_id INTEGER NOT NULL,
    option_key INTEGER NOT NULL,
    option_value TEXT NOT NULL,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(channel_id, option_key)
)
```

### 16. daily_summaries ⭐ NEW IN v6 (per-machine dimension dropped in v16)
Pre-calculated daily aggregate statistics for fast reporting. One row per
(profile, date) — the row already aggregates across all machines that
contributed sessions to that OSCAR day.

```sql
CREATE TABLE daily_summaries (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    profile_id INTEGER NOT NULL,
    date TEXT NOT NULL,

    session_count INTEGER DEFAULT 0,
    enabled_session_count INTEGER DEFAULT 0,

    total_hours REAL DEFAULT 0,
    mask_on_hours REAL DEFAULT 0,

    ahi REAL DEFAULT 0,
    rdi REAL DEFAULT 0,
    oahi REAL DEFAULT 0,
    cahi REAL DEFAULT 0,
    obstructive_count INTEGER DEFAULT 0,
    unclassified_count INTEGER DEFAULT 0,
    hypopnea_count INTEGER DEFAULT 0,
    rera_count INTEGER DEFAULT 0,
    clear_airway_count INTEGER DEFAULT 0,
    obstructive_hypopnea_count INTEGER DEFAULT 0,
    central_hypopnea_count INTEGER DEFAULT 0,
    all_apnea_count INTEGER DEFAULT 0,

    pressure_avg REAL,
    pressure_min REAL,
    pressure_max REAL,
    pressure_95th REAL,

    leak_total_avg REAL,
    leak_total_95th REAL,
    leak_total_max REAL,
    leak_unintentional_avg REAL,

    spo2_avg REAL,
    spo2_min REAL,
    pulse_avg REAL,
    pulse_min REAL,
    pulse_max REAL,

    is_compliant INTEGER DEFAULT 0,
    has_oximetry INTEGER DEFAULT 0,

    calculated_at TEXT DEFAULT CURRENT_TIMESTAMP,
    sessions_hash TEXT,

    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,
    UNIQUE(profile_id, date)
)
```

**Purpose:** Dramatically speeds up Overview and Statistics screens by pre-calculating daily aggregates.

**v16 change:** The `machine_id` column, its FK to `machines`, and the
`idx_daily_summaries_profile_machine` index were removed; the natural key
changed from `(profile_id, date, machine_id)` to `(profile_id, date)`.

### 17. event_lists ⚡ **NEW IN v8 - DATABASE-ONLY MODE**
EventList metadata for waveform and event data.

```sql
CREATE TABLE event_lists (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL,
    profile_id INTEGER NOT NULL,
    channel_id INTEGER NOT NULL,
    eventlist_index INTEGER NOT NULL DEFAULT 0,
    event_type INTEGER NOT NULL,
    first_time INTEGER NOT NULL,
    last_time INTEGER NOT NULL,
    count INTEGER NOT NULL,
    rate REAL NOT NULL DEFAULT 0,
    gain REAL NOT NULL DEFAULT 1.0,
    offset REAL NOT NULL DEFAULT 0.0,
    min_value REAL NOT NULL DEFAULT 0.0,
    max_value REAL NOT NULL DEFAULT 0.0,
    dimension TEXT,
    has_second_field INTEGER NOT NULL DEFAULT 0,
    min2_value REAL,
    max2_value REAL,
    data_size INTEGER NOT NULL DEFAULT 0,
    compressed_size INTEGER,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,
    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,
    UNIQUE(session_id, channel_id, eventlist_index)
)
```

**Note:** `profile_id` added in v12 for query performance denormalization.

**Purpose:** Stores metadata for each EventList (one row per EventList). Replaces .001 file headers. Includes timing, scaling, dimensional information, and compression statistics.

**Event Types:** 0=Waveform (`EVL_Waveform`), 1=Event (`EVL_Event`). Values match the C++ `EventListType` enum in `oscar/SleepLib/event.h`.

### 18. event_data ⚡ **NEW IN v8 - DATABASE-ONLY MODE**
Binary waveform and event data storage.

```sql
CREATE TABLE event_data (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    eventlist_id INTEGER NOT NULL,
    data_blob BLOB,
    data_compressed BLOB,
    data2_blob BLOB,
    data2_compressed BLOB,
    time_blob BLOB,
    time_compressed BLOB,
    compression_method INTEGER NOT NULL DEFAULT 0,
    checksum INTEGER,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (eventlist_id) REFERENCES event_lists(id) ON DELETE CASCADE,
    UNIQUE(eventlist_id)
)
```

**Purpose:** Stores actual binary data arrays for each EventList (one row per EventList). Data is stored in compressed BLOB format using qCompress. Replaces .001 file data sections.

**Compression:** Data is compressed only if it saves >10% space. Either `data_blob` OR `data_compressed` is populated (not both).

**Checksum:** CRC16 checksum of primary data for integrity verification.

### 19. report_tree 🌲 **NEW IN v13 - REPORT TREE (replaces reports/report_contents)**
Hierarchical report tree with System and User roots.

```sql
CREATE TABLE report_tree (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    parent_id INTEGER,
    name TEXT NOT NULL,
    node_type TEXT NOT NULL CHECK(node_type IN ('root', 'folder', 'report')),
    source TEXT NOT NULL CHECK(source IN ('system', 'user')),
    description TEXT,
    query TEXT,
    display_order INTEGER DEFAULT 0,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (parent_id) REFERENCES report_tree(id) ON DELETE CASCADE,
    UNIQUE(parent_id, name)
)
```

**Purpose:** Stores the report tree used in the Reports UI. Replaces the flat `reports`/`report_contents` tables (v11) with a hierarchical structure supporting root nodes, folders, and report leaf nodes.

**Node Types:**
- `root` — top-level System or User root (parent_id = NULL)
- `folder` — grouping node (e.g., "Daily Summaries")
- `report` — leaf node with a SQL query template

**Source:**
- `system` — managed by OSCAR; populated/updated from the external `system_reports.orf` file on each startup
- `user` — created by the user; preserved across OSCAR upgrades

**Tree Structure Example:**
```
System (root, source=system)
├── Daily Summaries (folder)
│   ├── Days (report)
│   ├── Weeks (report)
│   └── Months (report)
└── Session Statistics (folder)
    ├── Sessions (report)
    └── Days (report)
User (root, source=user)
└── My Custom Report (report)
```

**Query Templates:** Report leaf nodes store SQL with macros (`#PROFILE_ID`, `#START_DATE`, `#END_DATE`) replaced at runtime.

**Not Profile-Specific:** `report_tree` is global to the database and is **not** included in profile backups. System nodes are auto-populated from the `.orf` file; user nodes are preserved across upgrades.

### 20. app_preferences 🗄️ **NEW IN v14 — replaces Preferences.xml**
Global application preferences (not profile-specific).

```sql
CREATE TABLE app_preferences (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    category    TEXT NOT NULL DEFAULT 'general',
    key         TEXT NOT NULL,
    value       TEXT,
    blob_value  BLOB,
    data_type   TEXT,
    created_at  TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at  TEXT DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(category, key)
)
```

**data_type values:** `'string'`, `'int'`, `'float'`, `'bool'`, `'datetime'`, `'date'`, `'time'`, `'blob'`

**Seeding:** On first launch after upgrade, legacy `Preferences.xml` is read and its contents inserted, then the XML file is deleted.

### 21. graph_layouts 🗄️ **NEW IN v14 — replaces layoutSettings/*.shg and per-profile *.shg**
Unified storage for both named graph layout slots (shared, cross-profile) and per-profile current layouts.

```sql
CREATE TABLE graph_layouts (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    profile_id     INTEGER,
    view_name      TEXT NOT NULL,
    slot_index     INTEGER NOT NULL,
    is_current     INTEGER NOT NULL DEFAULT 0,
    description    TEXT,
    format_version INTEGER NOT NULL,
    data           BLOB NOT NULL,
    created_at     TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at     TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE
)

CREATE UNIQUE INDEX idx_graph_layouts_named
    ON graph_layouts(view_name, slot_index) WHERE profile_id IS NULL;
CREATE UNIQUE INDEX idx_graph_layouts_current
    ON graph_layouts(profile_id, view_name) WHERE is_current = 1;
```

**Row types (discriminated by profile_id and is_current):**
- `profile_id IS NULL, is_current=0` — shared named layout slot (user-saved, cross-profile)
- `profile_id NOT NULL, is_current=1` — per-profile current layout (restored on open)

**data:** Raw `QDataStream` binary payload identical to old `.shg` file format (magic `0x41756728`, version 5+).

**Seeding:** On first launch after upgrade, legacy `layoutSettings/*.shg` files and per-profile `daily.shg`/`overview.shg` files are imported, then deleted.
