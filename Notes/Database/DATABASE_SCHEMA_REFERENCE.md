# OSCAR Database Schema Reference
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

### 11. session_summaries
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

### 12. session_slices
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

### 13. channels
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

### 14. channel_options
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

### 15. daily_summaries ⭐ NEW IN v6 (per-machine dimension dropped in v16)
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

### 16. event_lists ⚡ **NEW IN v8 - DATABASE-ONLY MODE**
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

### 17. event_data ⚡ **NEW IN v8 - DATABASE-ONLY MODE**
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

### 18. report_tree 🌲 **NEW IN v13 - REPORT TREE (replaces reports/report_contents)**
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

---

### 19. app_preferences 🗄️ **NEW IN v14 — replaces Preferences.xml**
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

---

### 20. graph_layouts 🗄️ **NEW IN v14 — replaces layoutSettings/*.shg and per-profile *.shg**
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

---

## Complete Data Dictionary

### profiles

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment profile ID |
| username | TEXT | UNIQUE | NO | Unique username/profile name |
| data_folder | TEXT | | NO | Path to profile data folder (portable format) |
| status | TEXT | | NO | 'active', 'missing', or 'archived' |
| status_changed_at | TEXT | | YES | Last status change timestamp |
| created_at | TEXT | | NO | Creation timestamp |
| updated_at | TEXT | | NO | Last update timestamp |

### machines

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Database ID |
| profile_id | INTEGER | FK | NO | → profiles(id) |
| machine_id | INTEGER | | NO | OSCAR internal ID (unique per profile) |
| loader_name | TEXT | | NO | Loader plugin (e.g., "ResMed") |
| machine_type | INTEGER | | NO | 0=CPAP, 1=BiLevel, etc. |
| brand | TEXT | | YES | Manufacturer name |
| model | TEXT | | YES | Device model |
| series | TEXT | | YES | Device series |
| serial_number | TEXT | | YES | Serial number |
| model_number | TEXT | | YES | Model number |
| last_imported | TEXT | | YES | Last import timestamp (ISO 8601) |
| purge_date | TEXT | | YES | Purge cutoff date (ISO 8601) |
| data_version | INTEGER | | NO | Loader data format version |
| properties | TEXT | | YES | JSON additional properties |
| created_at | TEXT | | NO | Creation timestamp |

### user_info

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| profile_id | INTEGER | FK,UNIQUE | NO | → profiles(id) |
| dob | TEXT | | YES | Date of birth (ISO 8601) |
| first_name | TEXT | | YES | First name |
| last_name | TEXT | | YES | Last name |
| address | TEXT | | YES | Physical address |
| phone | TEXT | | YES | Phone number |
| email | TEXT | | YES | Email address |
| country | TEXT | | YES | Country |
| height | REAL | | YES | Height (units per preferences) |
| gender | INTEGER | | YES | 0=Not specified, 1=Male, 2=Female |
| timezone | TEXT | | YES | IANA timezone |
| password_hash | TEXT | | YES | SHA1 password hash |
| created_at | TEXT | | NO | Creation timestamp |
| updated_at | TEXT | | NO | Update timestamp |

### doctor_info

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| profile_id | INTEGER | FK,UNIQUE | NO | → profiles(id) |
| name | TEXT | | YES | Doctor's name |
| phone | TEXT | | YES | Phone number |
| email | TEXT | | YES | Email address |
| practice_name | TEXT | | YES | Practice name |
| address | TEXT | | YES | Practice address |
| patient_id | TEXT | | YES | Patient ID |
| created_at | TEXT | | NO | Creation timestamp |
| updated_at | TEXT | | NO | Update timestamp |

### profile_preferences

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| profile_id | INTEGER | FK | NO | → profiles(id) |
| category | TEXT | | NO | Category (cpap, oximetry, session, etc.) |
| key | TEXT | | NO | Preference key |
| value | TEXT | | YES | Value (as text) |
| data_type | TEXT | | YES | Type hint (int, float, bool, string, date) |
| created_at | TEXT | | NO | Creation timestamp |
| updated_at | TEXT | | NO | Update timestamp |

### sessions

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Database ID |
| session_id | INTEGER | | NO | OSCAR ID (unique per machine) |
| machine_id | INTEGER | FK | NO | → machines(id) |
| start_time | INTEGER | | NO | Start (Unix timestamp) |
| end_time | INTEGER | | NO | End (Unix timestamp) |
| duration | INTEGER | | NO | Duration (milliseconds) |
| enabled | INTEGER | | NO | Enabled flag (0/1) |
| summary_only | INTEGER | | NO | Summary-only flag (0/1) |
| no_settings | INTEGER | | NO | No settings flag (0/1) |
| events_loaded | INTEGER | | NO | Events loaded flag (0/1) |
| created_at | TEXT | | NO | Creation timestamp |
| updated_at | TEXT | | NO | Update timestamp |

**Note:** `events_file` and `summary_file` removed in v12.

### session_settings

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| session_id | INTEGER | FK | NO | → sessions(id) |
| profile_id | INTEGER | FK | NO | → profiles(id) (denormalized, NEW IN v12) |
| channel_id | INTEGER | | NO | Channel ID |
| value | REAL | | NO | Setting value |
| data_type | TEXT | | YES | Data type hint |
| json_value | TEXT | | YES | JSON value for complex data types (NEW IN v9) |
| created_at | TEXT | | NO | Creation timestamp |

### session_channels

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| session_id | INTEGER | FK | NO | → sessions(id) |
| profile_id | INTEGER | FK | NO | → profiles(id) (denormalized, NEW IN v12) |
| channel_id | INTEGER | | NO | Channel ID |
| count | INTEGER | | NO | Data point count |
| sum | REAL | | NO | Sum of values |
| avg | REAL | | NO | Average |
| wavg | REAL | | NO | Weighted average |
| min | REAL | | NO | Minimum |
| max | REAL | | NO | Maximum |
| median | REAL | | NO | Median (50th percentile) ¹ |
| p90 | REAL | | NO | 90th percentile ¹ |
| p95 | REAL | | NO | 95th percentile ¹ |
| phys_min | REAL | | NO | Physical minimum |
| phys_max | REAL | | NO | Physical maximum |
| cph | REAL | | NO | Count per hour |
| sph | REAL | | NO | Sum per hour |
| first_time | INTEGER | | YES | First occurrence timestamp |
| last_time | INTEGER | | YES | Last occurrence timestamp |
| gain | REAL | | NO | Scale factor |
| created_at | TEXT | | NO | Creation timestamp |

¹ `median`, `p90`, and `p95` are 0 for oximetry sessions imported before the
2026-04-25 bug fix (issue #88). A bug in `Machine::Save()` caused these values
to be overwritten with 0 whenever `Profile::Save()` was called on a session whose
events were not loaded in memory. Data imported after the fix is correct.
Re-import is required to repair pre-fix rows; the discontinuity is accepted.

### session_channel_values 🐛 **NEW IN v7**

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| session_channel_id | INTEGER | FK | NO | → session_channels(id) |
| value | INTEGER | | NO | The distinct value that occurred |
| count | INTEGER | | NO | Number of occurrences of this value |
| time_ms | INTEGER | | NO | Total time in milliseconds this value was held |
| created_at | TEXT | | NO | Creation timestamp |

**Critical for:** Weighted averages, time-based statistics. Without this data, weighted averages default to simple averages, causing significant inaccuracies in pressure and other metrics.

### respiratory_events

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| session_id | INTEGER | FK | NO | → sessions(id) |
| profile_id | INTEGER | FK | NO | → profiles(id) (denormalized, NEW IN v12) |
| channel_id | INTEGER | | YES | OSCAR channel ID constant (NEW IN v12) |
| event_type | INTEGER | | NO | 0=OA, 1=UA, 2=H, 3=RERA, 4=CAA, 5=User |
| start_time | INTEGER | | NO | Start (Unix timestamp) |
| end_time | INTEGER | | NO | End (Unix timestamp) |
| duration | INTEGER | | NO | Duration (seconds) |
| desaturation | REAL | | YES | O₂ desaturation (%) |
| severity | INTEGER | | YES | Severity rating |
| created_at | TEXT | | NO | Creation timestamp |

### session_summaries

**NULL vs 0 (v19).** `0` means measured and none; `NULL` means not applicable or not measured. Aggregate with `COALESCE(SUM(col), 0)` when adding columns together — `SUM()` over an all-NULL group is `NULL` and `NULL + x` is `NULL`.

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| session_id | INTEGER | FK,UNIQUE | NO | → sessions(id) |
| profile_id | INTEGER | FK | NO | → profiles(id) (denormalized, NEW IN v12) |
| ahi | REAL | | NO | Apnea-Hypopnea Index |
| rdi | REAL | | YES | Respiratory Disturbance Index; NULL unless the device reports RERA (v19) |
| oahi | REAL | | YES | Obstructive AHI (added in v18); oahi + cahi = ahi; NULL unless the device splits hypopneas by mechanism (v19) |
| cahi | REAL | | YES | Central AHI (added in v18); NULL unless the device splits hypopneas by mechanism (v19) |
| obstructive_count | INTEGER | | YES | OA count; NULL when the device never reports it (v19) |
| unclassified_count | INTEGER | | YES | UA count (renamed from central_count in v10); NULL when the device never reports it (v19) |
| hypopnea_count | INTEGER | | YES | H Hypopnea count (unclassified by mechanism); NULL when the device never reports it (v19) |
| rera_count | INTEGER | | YES | RERA count; NULL when the device never reports it (v19) |
| clear_airway_count | INTEGER | | YES | CA count (added in v10); NULL when the device never reports it (v19) |
| obstructive_hypopnea_count | INTEGER | | YES | OH count (added in v18); NULL when the device never reports it (v19) |
| central_hypopnea_count | INTEGER | | YES | CH count (added in v18); NULL when the device never reports it (v19) |
| all_apnea_count | INTEGER | | YES | A count — undifferentiated apnea (added in v18); NULL when the device never reports it (v19) |
| pressure_avg | REAL | | YES | Average pressure (cmH₂O) |
| pressure_min | REAL | | YES | Min pressure |
| pressure_max | REAL | | YES | Max pressure |
| pressure_95th | REAL | | YES | 95th percentile pressure |
| leak_total_avg | REAL | | YES | Average leak (L/min) |
| leak_total_95th | REAL | | YES | 95th percentile leak |
| leak_total_max | REAL | | YES | Max leak |
| spo2_avg | REAL | | YES | Average SpO₂ (%) |
| spo2_min | REAL | | YES | Min SpO₂ |
| pulse_avg | REAL | | YES | Average pulse (BPM) |
| hours_used | REAL | | NO | Hours used |
| mask_on_hours | REAL | | NO | Mask-on hours |
| created_at | TEXT | | NO | Creation timestamp |
| updated_at | TEXT | | NO | Update timestamp |

**Note:** The old `central_count` column is retained in the database for backward compatibility but new code uses `unclassified_count`.

### session_slices

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| session_id | INTEGER | FK | NO | → sessions(id) |
| start_time | INTEGER | | NO | Slice start (Unix timestamp) |
| end_time | INTEGER | | NO | Slice end (Unix timestamp) |
| status | INTEGER | | NO | 0=mask off, 1=mask on |

### channels

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| profile_id | INTEGER | FK | NO | → profiles(id) |
| channel_id | INTEGER | | NO | OSCAR channel ID constant |
| channel_code | TEXT | | NO | Code (e.g., "CPAP_Pressure") |
| type | INTEGER | | YES | Channel type (NEW IN v12) |
| enabled | INTEGER | | NO | Enabled flag (0/1) |
| default_color | TEXT | | YES | Color (#RRGGBB) |
| fullname | TEXT | | YES | Full name |
| label | TEXT | | YES | Short label |
| description | TEXT | | YES | Description |
| lower_threshold | REAL | | YES | Lower threshold value |
| lower_threshold_color | TEXT | | YES | Lower threshold color |
| upper_threshold | REAL | | YES | Upper threshold value |
| upper_threshold_color | TEXT | | YES | Upper threshold color |
| show_in_overview | INTEGER | | NO | Show in overview (0/1) |
| created_at | TEXT | | NO | Creation timestamp |
| updated_at | TEXT | | NO | Update timestamp |

### channel_options

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| channel_id | INTEGER | | NO | OSCAR channel ID constant |
| option_key | INTEGER | | NO | Numeric key |
| option_value | TEXT | | NO | Text value |
| created_at | TEXT | | NO | Creation timestamp |

### daily_summaries ⭐ NEW (per-machine dimension dropped in v16)

**NULL vs 0 (v19).** `0` means measured and none; `NULL` means not applicable or not measured. Aggregate with `COALESCE(SUM(col), 0)` when adding columns together — `SUM()` over an all-NULL group is `NULL` and `NULL + x` is `NULL`.

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| profile_id | INTEGER | FK | NO | → profiles(id) |
| date | TEXT | | NO | Date (YYYY-MM-DD); UNIQUE with profile_id |
| session_count | INTEGER | | NO | Total sessions |
| enabled_session_count | INTEGER | | NO | Enabled sessions |
| total_hours | REAL | | NO | Total CPAP hours |
| mask_on_hours | REAL | | NO | Mask-on hours |
| ahi | REAL | | NO | Apnea-Hypopnea Index |
| rdi | REAL | | YES | Respiratory Disturbance Index; NULL unless the device reports RERA (v19) |
| oahi | REAL | | YES | Obstructive AHI (added in v18); oahi + cahi = ahi; NULL unless the device splits hypopneas by mechanism (v19) |
| cahi | REAL | | YES | Central AHI (added in v18); NULL unless the device splits hypopneas by mechanism (v19) |
| obstructive_count | INTEGER | | YES | OA count; NULL when the device never reports it (v19) |
| unclassified_count | INTEGER | | YES | UA count (renamed from central_count in v10); NULL when the device never reports it (v19) |
| hypopnea_count | INTEGER | | YES | Hypopnea count (unclassified by mechanism); NULL when the device never reports it (v19) |
| rera_count | INTEGER | | YES | RERA count; NULL when the device never reports it (v19) |
| clear_airway_count | INTEGER | | YES | CA Clear airway count; NULL when the device never reports it (v19) |
| obstructive_hypopnea_count | INTEGER | | YES | OH count (added in v18); NULL when the device never reports it (v19) |
| central_hypopnea_count | INTEGER | | YES | CH count (added in v18); NULL when the device never reports it (v19) |
| all_apnea_count | INTEGER | | YES | A count — undifferentiated apnea (added in v18); NULL when the device never reports it (v19) |
| pressure_avg | REAL | | YES | Average pressure (cmH₂O) |
| pressure_min | REAL | | YES | Min pressure |
| pressure_max | REAL | | YES | Max pressure |
| pressure_95th | REAL | | YES | 90th percentile pressure |
| leak_total_avg | REAL | | YES | Average leak (L/min) |
| leak_total_95th | REAL | | YES | 90th percentile leak |
| leak_total_max | REAL | | YES | Max leak |
| leak_unintentional_avg | REAL | | YES | Average unintentional leak |
| spo2_avg | REAL | | YES | Average SpO₂ (%) |
| spo2_min | REAL | | YES | Min SpO₂ |
| pulse_avg | REAL | | YES | Average pulse (BPM) |
| pulse_min | REAL | | YES | Min pulse |
| pulse_max | REAL | | YES | Max pulse |
| is_compliant | INTEGER | | NO | Compliance flag (≥4 hours) |
| has_oximetry | INTEGER | | NO | Has oximetry data |
| calculated_at | TEXT | | NO | Calculation timestamp |
| sessions_hash | TEXT | | YES | Cache invalidation hash |

**Performance Impact:** Queries that previously took 2-3 seconds now complete in <100ms.

### event_lists ⚡ **NEW IN v8**

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| session_id | INTEGER | FK | NO | → sessions(id) |
| profile_id | INTEGER | FK | NO | → profiles(id) (denormalized, NEW IN v12) |
| channel_id | INTEGER | | NO | OSCAR channel ID |
| eventlist_index | INTEGER | | NO | Index when multiple EventLists per channel |
| event_type | INTEGER | | NO | 0=Waveform, 1=Event (matches `EventListType` enum) |
| first_time | INTEGER | | NO | First timestamp (Unix ms) |
| last_time | INTEGER | | NO | Last timestamp (Unix ms) |
| count | INTEGER | | NO | Number of data points |
| rate | REAL | | NO | Sample rate (ms) for waveforms |
| gain | REAL | | NO | Scale factor (default 1.0) |
| offset | REAL | | NO | Offset value (default 0.0) |
| min_value | REAL | | NO | Minimum value |
| max_value | REAL | | NO | Maximum value |
| dimension | TEXT | | YES | Units (e.g., "cmH₂O", "L/min") |
| has_second_field | INTEGER | | NO | Has secondary data array (0/1) |
| min2_value | REAL | | YES | Min of secondary field |
| max2_value | REAL | | YES | Max of secondary field |
| data_size | INTEGER | | NO | Uncompressed size (bytes) |
| compressed_size | INTEGER | | YES | Compressed size (bytes) |
| created_at | TEXT | | NO | Creation timestamp |

**Replaces:** .001 file headers. Each row represents one EventList from the Session class.

### event_data ⚡ **NEW IN v8**

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| eventlist_id | INTEGER | FK,UNIQUE | NO | → event_lists(id) |
| data_blob | BLOB | | YES | Uncompressed primary data (qint16 array) |
| data_compressed | BLOB | | YES | Compressed primary data (qCompress level 6) |
| data2_blob | BLOB | | YES | Uncompressed secondary data |
| data2_compressed | BLOB | | YES | Compressed secondary data |
| time_blob | BLOB | | YES | Uncompressed time deltas (quint32 array) |
| time_compressed | BLOB | | YES | Compressed time deltas |
| compression_method | INTEGER | | NO | 0=none, 1=qCompress |
| checksum | INTEGER | | YES | CRC16 checksum of primary data |
| created_at | TEXT | | NO | Creation timestamp |

**Replaces:** .001 file data sections. **Compression:** Only one of each pair (blob/compressed) is populated. Compression used only if >10% space savings. **Typical compression:** 40-60% for CPAP waveform data.

### report_tree 🌲 **NEW IN v13 (replaces reports/report_contents from v11)**

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| parent_id | INTEGER | FK | YES | → report_tree(id); NULL for root nodes |
| name | TEXT | | NO | Node name (unique within parent) |
| node_type | TEXT | | NO | 'root', 'folder', or 'report' |
| source | TEXT | | NO | 'system' or 'user' |
| description | TEXT | | YES | Node description |
| query | TEXT | | YES | SQL query template (report nodes only) |
| display_order | INTEGER | | NO | Sort order for UI display |
| created_at | TEXT | | NO | Creation timestamp |
| updated_at | TEXT | | NO | Last update timestamp |

**System nodes:** Managed by OSCAR; populated/updated from the external `system_reports.orf` file on each startup. **User nodes:** Created by the user; preserved across OSCAR upgrades. **Query macros:** `#PROFILE_ID`, `#START_DATE`, `#END_DATE` replaced at runtime.

---

## Database Indexes

### Profile & Machine Indexes
```sql
idx_profiles_username ON profiles(username)
idx_machines_profile ON machines(profile_id)
idx_machines_serial ON machines(serial_number)
idx_machines_loader ON machines(loader_name)
```

### User & Doctor Indexes
```sql
idx_user_info_profile ON user_info(profile_id)
idx_doctor_info_profile ON doctor_info(profile_id)
```

### Preferences Indexes
```sql
idx_preferences_profile ON profile_preferences(profile_id)
idx_preferences_category ON profile_preferences(profile_id, category)
idx_preferences_key ON profile_preferences(profile_id, category, key)
```

### Session Indexes
```sql
idx_sessions_machine ON sessions(machine_id)
idx_sessions_time ON sessions(start_time, end_time)
idx_sessions_enabled ON sessions(machine_id, enabled)
```

### Session Data Indexes
```sql
idx_session_settings_session ON session_settings(session_id)
idx_session_settings_channel ON session_settings(session_id, channel_id)
idx_session_channels_session ON session_channels(session_id)
idx_session_channels_channel ON session_channels(channel_id)
idx_session_channels_lookup ON session_channels(session_id, channel_id)
```

### Event Indexes
```sql
idx_respiratory_events_session ON respiratory_events(session_id)
idx_respiratory_events_type ON respiratory_events(session_id, event_type)
idx_respiratory_events_time ON respiratory_events(start_time, end_time)
```

### Summary & Slice Indexes
```sql
idx_session_summaries_session ON session_summaries(session_id)
idx_session_summaries_ahi ON session_summaries(ahi)
idx_session_slices_session ON session_slices(session_id)
idx_session_slices_time ON session_slices(start_time, end_time)
```

### Channel Indexes
```sql
idx_channels_profile ON channels(profile_id)
idx_channels_code ON channels(channel_code)
idx_channels_lookup ON channels(profile_id, channel_id)
idx_channel_options_channel ON channel_options(channel_id)
idx_channel_options_lookup ON channel_options(channel_id, option_key)
```

### Daily Summaries Indexes ⭐ NEW IN v6 (profile_machine index dropped in v16)
```sql
idx_daily_summaries_profile_date ON daily_summaries(profile_id, date)
idx_daily_summaries_ahi ON daily_summaries(ahi)
idx_daily_summaries_compliance ON daily_summaries(profile_id, is_compliant)
idx_daily_summaries_date_range ON daily_summaries(profile_id, date DESC)
```

### Event Data Indexes ⚡ NEW IN v8
```sql
idx_event_lists_session ON event_lists(session_id)
idx_event_lists_channel ON event_lists(session_id, channel_id)
idx_event_lists_type ON event_lists(event_type)
idx_event_lists_time ON event_lists(session_id, first_time, last_time)
idx_event_data_eventlist ON event_data(eventlist_id)
```

### Profile ID Denormalization Indexes 🔧 NEW IN v12
```sql
idx_session_summaries_profile ON session_summaries(profile_id)
idx_session_summaries_profile_date ON session_summaries(profile_id, session_id)
idx_session_settings_profile ON session_settings(profile_id)
idx_session_settings_profile_channel ON session_settings(profile_id, channel_id)
idx_session_channels_profile ON session_channels(profile_id)
idx_session_channels_profile_channel ON session_channels(profile_id, channel_id)
idx_event_lists_profile ON event_lists(profile_id)
idx_event_lists_profile_channel ON event_lists(profile_id, channel_id)
idx_respiratory_events_profile ON respiratory_events(profile_id)
idx_respiratory_events_profile_type ON respiratory_events(profile_id, event_type)
```

### Report Tree Indexes 🌲 NEW IN v13
```sql
idx_report_tree_parent ON report_tree(parent_id)
idx_report_tree_source ON report_tree(source)
idx_report_tree_type ON report_tree(node_type)
```

---

## Foreign Key Relationships

### Entity Relationship Diagram

```
profiles (1) ──┬─< machines (N)
               ├─< user_info (1)
               ├─< doctor_info (1)
               ├─< profile_preferences (N)
               ├─< channels (N)
               ├─< daily_summaries (N)
               ├─< session_settings (N)    [denormalized profile_id, v12]
               ├─< session_channels (N)    [denormalized profile_id, v12]
               ├─< session_summaries (N)   [denormalized profile_id, v12]
               ├─< event_lists (N)         [denormalized profile_id, v12]
               └─< respiratory_events (N)  [denormalized profile_id, v12]

machines (1) ──── sessions (N)

sessions (1) ──┬─< session_settings (N)
               ├─< session_channels (N)
               ├─< respiratory_events (N)
               ├─< session_summaries (1)
               ├─< session_slices (N)
               └─< event_lists (N)

event_lists (1) ─< event_data (1)

session_channels (1) ─< session_channel_values (N)

report_tree (1) ─< report_tree (N)   [self-referencing parent_id, v13]

channel_options (N) - standalone (references channel_id constant)
report_tree (N) - global (not profile-specific) 🌲 NEW IN v13
```

### Foreign Key Details

| Child Table | FK Column | Parent Table | Parent Column | On Delete |
|-------------|-----------|--------------|---------------|-----------|
| machines | profile_id | profiles | id | CASCADE |
| user_info | profile_id | profiles | id | CASCADE |
| doctor_info | profile_id | profiles | id | CASCADE |
| profile_preferences | profile_id | profiles | id | CASCADE |
| channels | profile_id | profiles | id | CASCADE |
| daily_summaries | profile_id | profiles | id | CASCADE |
| sessions | machine_id | machines | id | CASCADE |
| session_settings | session_id | sessions | id | CASCADE |
| session_settings | profile_id | profiles | id | CASCADE |
| session_channels | session_id | sessions | id | CASCADE |
| session_channels | profile_id | profiles | id | CASCADE |
| session_channel_values | session_channel_id | session_channels | id | CASCADE |
| respiratory_events | session_id | sessions | id | CASCADE |
| respiratory_events | profile_id | profiles | id | CASCADE |
| session_summaries | session_id | sessions | id | CASCADE |
| session_summaries | profile_id | profiles | id | CASCADE |
| session_slices | session_id | sessions | id | CASCADE |
| event_lists | session_id | sessions | id | CASCADE |
| event_lists | profile_id | profiles | id | CASCADE |
| event_data | eventlist_id | event_lists | id | CASCADE |
| report_tree | parent_id | report_tree | id | CASCADE |

**Cascade Delete Behavior:**
- Deleting **profile** removes: machines, user_info, doctor_info, preferences, channels, daily_summaries, session_settings, session_channels, session_summaries, event_lists, respiratory_events (all denormalized profile_id FKs)
- Deleting **machine** removes: sessions (and their data). `daily_summaries` rows survive, since v16 they no longer reference a machine.
- Deleting **session** removes: settings, channels, events, summaries, slices, event_lists (which cascades to event_data)
- Deleting **event_list** removes: event_data (waveform/event binary data)
- Deleting **report_tree node** removes: all child nodes (self-referencing cascade)

---

## Data Storage Philosophy

**⚡ Database-Only Approach (v8+):**
- **Database**: ALL data including waveforms, events, metadata, settings, summaries, daily aggregates
- **Files**: DEPRECATED - .001 files no longer used for event/waveform data

**Rationale:**
- SQLite excellent for structured metadata, queries, AND binary data (BLOBs)
- Database storage provides ACID transactions, referential integrity, and atomic operations
- Compressed BLOBs achieve 40-60% compression, comparable to .001 file format
- Single-file database simplifies backup, restore, and data management
- Eliminates file synchronization issues between database and .001 files
- Daily summaries enable lightning-fast reports without loading sessions

**Migration:** Existing .001 files are read once during upgrade, then data is migrated to event_data table and .001 files can be deleted.

---

## Performance Considerations

- **Indexes** optimize common query patterns
- **Foreign keys** ensure referential integrity
- **Cascade deletes** simplify data management
- **Session summaries** provide fast statistics per session
- **Daily summaries** provide ultra-fast statistics per day (20-100x faster)
- **BLOB compression** reduces database size by 40-60% for waveform data ⚡ NEW IN v8
- **Transactional safety** ensures data integrity across all operations ⚡ NEW IN v8

---

## Schema v6 Highlights

The daily_summaries table (new in v6) provides:
- **Pre-calculated daily statistics** for all CPAP metrics
- **Automatic population** during data load
- **35 statistics per day** including AHI, RDI, pressure, leak, oximetry
- **Fast queries** for Overview and Statistics screens (<100ms vs 2-3 seconds)
- **One row per profile-day** (machine_id removed in v16; see v16 highlights below)
- **Cache invalidation** via sessions_hash field

## Schema v7 Highlights 🐛 **BUG FIX**

The session_channel_values table (new in v7) provides:
- **Fixes critical bug** where value/time summaries were not persisted to database
- **Stores m_valuesummary and m_timesummary** hash data for each channel
- **Enables accurate weighted averages** and time-based statistics
- **Impact**: Without this table, weighted averages defaulted to simple averages, causing significant inaccuracies in pressure statistics and other metrics
- **Automatic migration** from schema version 6 to 7
- **Note**: Existing sessions will need to be re-saved to populate this data (happens automatically on next import)

## Schema v8 Highlights ⚡ **MAJOR CHANGE - DATABASE-ONLY MODE**

The event_lists and event_data tables (new in v8) provide revolutionary database-only storage:

**What Changed:**
- **Eliminates .001 files** - All waveform/event data now stored in database BLOBs
- **Two new tables**: event_lists (metadata) and event_data (binary data)
- **Compressed storage** - qCompress level 9 achieves 40-60% compression
- **Transactional integrity** - All data protected by ACID transactions
- **Simpler data model** - Single database file instead of database + thousands of .001 files

**Benefits:**
- **Data integrity** - Foreign key constraints ensure waveform data consistency with sessions
- **Atomic operations** - Import/delete operations are fully transactional
- **Simplified backup** - Single database file contains ALL user data
- **Better performance** - Eliminates file system overhead for thousands of small files
- **Cross-platform** - No file path issues, permissions problems, or filename limitations
- **Checksum verification** - CRC16 checksums ensure data integrity

**Migration Strategy:**
- **Automatic** - Existing .001 files read once on first load after upgrade
- **Data preserved** - All waveform/event data migrated to event_data table
- **Backward compatible** - Can still read old .001 files if database migration fails
- **File cleanup** - After successful migration, .001 files can be safely deleted

**Storage Efficiency:**
- Compression ratio: 40-60% (comparable to .001 format)
- Typical session: 50-200 KB compressed in database
- Overhead per EventList: ~200 bytes metadata in event_lists table
- Net result: Similar or smaller database size compared to .001 files

**Performance Impact:**
- Load times: Comparable to .001 file loading
- Memory usage: Unchanged (data still decompressed to memory)
- Query flexibility: Can now query waveform metadata without loading full data
- Transaction safety: Significantly improved vs file-based storage

**Note**: Re-import CPAP data after upgrade to migrate from .001 files to database storage.

## Schema v9 Highlights 📝 **ENHANCEMENT**

The `json_value` column in `session_settings` (added in v9) provides:

**What Changed:**
- **New column**: Added `json_value TEXT` column to `session_settings` table
- **Complex data support**: Enables storing structured/complex data as JSON
- **Journal migration**: Supports migrating journal-type data to the database

**Use Cases:**
- **Bookmark data**: User-created bookmarks with timestamps and descriptions
- **Session notes**: Rich text or structured notes attached to sessions
- **Custom fields**: Any complex settings that don't fit the simple value model
- **Inter-session data**: Data spanning multiple sessions (e.g., sleep diary entries)

**Benefits:**
- **Flexibility**: JSON format accommodates diverse data structures
- **Future-proofing**: Supports new data types without schema changes
- **Consolidation**: Moves more data from files into the database
- **Query capability**: JSON fields can be queried using SQLite's JSON functions

**Migration Impact:**
- **Automatic upgrade**: Existing databases get the new column added
- **Backward compatible**: Old data without json_value continues to work
- **No data loss**: Existing session_settings records remain unchanged

## Schema v10 Highlights 🔧 **SEMANTIC FIX**

The schema v10 changes provide semantic correctness for apnea event classification:

**What Changed:**
- **Renamed field**: `central_count` → `unclassified_count` in both `session_summaries` and `daily_summaries` tables
- **New field**: Added `clear_airway_count` to `session_summaries` table
- **Data preservation**: Existing `central_count` data copied to `unclassified_count` during migration
- **Backward compatibility**: Old `central_count` column retained but ignored by new code

**Why This Change:**
- **Semantic accuracy**: Most CPAP machines cannot distinguish between true Central Apneas and other unclassified events
- **Correct terminology**: "Unclassified Apnea" (UA) is more accurate than assuming all are "Central Apnea" (CA)
- **Clear Airway support**: Enables proper tracking of Clear Airway Apneas (CAA) which some advanced machines can detect

**Impact:**
- **AHI calculation unchanged**: Both CA and UA events count toward AHI
- **Better reporting**: Users see more accurate event classifications
- **Future-proofing**: Supports advanced machines that can differentiate event types

**Migration:**
- **Automatic**: Schema upgrade copies central_count → unclassified_count
- **No data loss**: All existing event counts preserved
- **UI updates**: Reports and statistics screens updated to use new terminology

## Schema v11 Highlights 📊 **NEW FEATURE - CSV EXPORT REPORTS**

The schema v11 changes introduced database-driven CSV export reports (later redesigned in v13):

**What Changed:**
- **New tables**: Added `reports` and `report_contents` tables
- **Macro-based queries**: SQL templates with runtime substitution (#PROFILE_ID, #START_DATE, #END_DATE)
- **Report varieties**: Each report supports multiple aggregation levels (Days, Weeks, Months)

**Note:** These tables were replaced by `report_tree` in v13. See Schema v13 Highlights.

---

## Schema v12 Highlights 🔧 **DENORMALIZATION AND CLEANUP**

**What Changed:**
- **Profile ID denormalization**: Added `profile_id` column to `session_settings`, `session_channels`, `session_summaries`, and `event_lists` for direct profile-level queries without joining through machines/sessions
- **Respiratory events enriched**: Added `profile_id` and `channel_id` to `respiratory_events`
- **Channels type**: Added `type INTEGER` column to `channels`
- **Sessions simplified**: Removed `events_file` and `summary_file` columns (legacy .001 file references no longer needed)
- **No-migration policy introduced**: Schema version mismatch now requires a fresh database and data reimport. Incremental migrations are no longer supported.

**Why Denormalization:**
- Enables efficient queries like "all sessions for profile X" without joining through machines
- Allows direct profile-level filtering on event_lists, session_summaries, and respiratory_events
- Significant query performance improvement for reporting screens

**Impact of No-Migration Policy:**
- Users upgrading from v11 or earlier must reimport their CPAP data
- Eliminates complex migration code and reduces maintenance burden
- Ensures data integrity — no partial or broken upgrade states

---

## Schema v13 Highlights 🌲 **REPORT TREE REDESIGN**

**What Changed:**
- **Replaced `reports`/`report_contents`** (v11) with a single self-referencing `report_tree` table
- **Hierarchical structure**: Root nodes → Folders → Report leaf nodes
- **System/User split**: `source` column distinguishes OSCAR-managed vs user-created nodes
- **External orf file**: System reports loaded from `system_reports.orf` on each startup (allows report updates without schema changes)

**Benefits:**
- **Flexibility**: Supports unlimited nesting depth (folders within folders)
- **Separation**: System and user reports clearly delineated
- **Maintainability**: System reports updated via `.orf` file, not schema migrations
- **Simpler schema**: One table instead of two for the same functionality

**Tree Initialization:**
- At first install: root nodes and system reports populated from `.orf` file
- At startup: system reports refreshed if OSCAR version changed; user reports untouched
- User-created reports: preserved across all OSCAR upgrades

---

## Schema v16 Highlights 🔧 **DAILY SUMMARIES — REMOVE PER-MACHINE DIMENSION**

**What Changed:**
- Dropped `machine_id` column, its FK to `machines`, and the
  `idx_daily_summaries_profile_machine` index from `daily_summaries`.
- Natural key changed from `UNIQUE(profile_id, date, machine_id)` to
  `UNIQUE(profile_id, date)`.
- `DailySummaryRepository` API methods that previously took an optional
  `machineId` parameter (`findByProfileAndDate`, `findRange`,
  `calculateAndStore`, `calculateAndStoreFromDay`, `exists`) now take only
  the profile/date arguments.
- Restore code's special-case for remapping `daily_summaries.machine_id`
  was removed; v15 backups still restore cleanly because the existing
  `validColumns` filter (built from `PRAGMA table_info`) silently drops
  the column.

**Why:**
A `daily_summaries` row is a profile-day rollup. The `Day` object it
mirrors already aggregates across all machines that contributed sessions
to that OSCAR day (one CPAP plus zero or more oximetry/auxiliary devices).
The `machine_id` column was a design mistake: every caller passed `0`
intending "combined", but the calculation code silently rewrote that to
the first enabled CPAP session's machine id, contradicting the original
"NULL = combined" documentation. The corresponding read-side helpers
(`findByProfileAndDate` with default args, `findRange`, `exists`) queried
`machine_id IS NULL` and so could never find the rows the writer stored.
None of those finders had any in-tree callers, which is why the mismatch
was invisible.

**Migration:**
`migrateV15ToV16` rebuilds the table (SQLite cannot drop a column inside
a UNIQUE constraint without a rebuild). Data is copied via
`INSERT OR REPLACE` ordered to prefer machine-bound rows over NULL rows
on the (profile_id, date) key. In current deployments every existing row
already maps cleanly to a unique (profile_id, date) tuple, so the dedup
is defensive only.

---

**Document Version:** 8.1
**Schema Version:** 16
**Generated:** 2026 Q2
