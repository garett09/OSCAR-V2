# OSCAR Database - Complete Data Dictionary

---

## profiles

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment profile ID |
| username | TEXT | UNIQUE | NO | Unique username/profile name |
| data_folder | TEXT | | NO | Path to profile data folder (portable format) |
| status | TEXT | | NO | 'active', 'missing', or 'archived' |
| status_changed_at | TEXT | | YES | Last status change timestamp |
| created_at | TEXT | | NO | Creation timestamp |
| updated_at | TEXT | | NO | Last update timestamp |

## machines

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

## user_info

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

## doctor_info

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

## profile_preferences

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

## sessions

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

## session_settings

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

## session_channels

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

## session_channel_values 🐛 **NEW IN v7**

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| session_channel_id | INTEGER | FK | NO | → session_channels(id) |
| value | INTEGER | | NO | The distinct value that occurred |
| count | INTEGER | | NO | Number of occurrences of this value |
| time_ms | INTEGER | | NO | Total time in milliseconds this value was held |
| created_at | TEXT | | NO | Creation timestamp |

**Critical for:** Weighted averages, time-based statistics. Without this data, weighted averages default to simple averages, causing significant inaccuracies in pressure and other metrics.

## respiratory_events

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

## session_summaries

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| session_id | INTEGER | FK,UNIQUE | NO | → sessions(id) |
| profile_id | INTEGER | FK | NO | → profiles(id) (denormalized, NEW IN v12) |
| ahi | REAL | | NO | Apnea-Hypopnea Index |
| rdi | REAL | | NO | Respiratory Disturbance Index |
| oahi | REAL | | NO | Obstructive AHI (added in v18); oahi + cahi = ahi |
| cahi | REAL | | NO | Central AHI (added in v18) |
| obstructive_count | INTEGER | | NO | OA count |
| unclassified_count | INTEGER | | NO | UA count (renamed from central_count in v10) |
| hypopnea_count | INTEGER | | NO | H Hypopnea count (unclassified by mechanism) |
| rera_count | INTEGER | | NO | RERA count |
| clear_airway_count | INTEGER | | NO | CA count (added in v10) |
| obstructive_hypopnea_count | INTEGER | | NO | OH count (added in v18) |
| central_hypopnea_count | INTEGER | | NO | CH count (added in v18) |
| all_apnea_count | INTEGER | | NO | A count — undifferentiated apnea (added in v18) |
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

## session_slices

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| session_id | INTEGER | FK | NO | → sessions(id) |
| start_time | INTEGER | | NO | Slice start (Unix timestamp) |
| end_time | INTEGER | | NO | Slice end (Unix timestamp) |
| status | INTEGER | | NO | 0=mask off, 1=mask on |

## channels

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

## channel_options

| Field | Type | Key | Null | Description |
|-------|------|-----|------|-------------|
| id | INTEGER | PK | NO | Auto-increment ID |
| channel_id | INTEGER | | NO | OSCAR channel ID constant |
| option_key | INTEGER | | NO | Numeric key |
| option_value | TEXT | | NO | Text value |
| created_at | TEXT | | NO | Creation timestamp |

## daily_summaries ⭐ NEW (per-machine dimension dropped in v16)

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
| rdi | REAL | | NO | Respiratory Disturbance Index |
| oahi | REAL | | NO | Obstructive AHI (added in v18); oahi + cahi = ahi |
| cahi | REAL | | NO | Central AHI (added in v18) |
| obstructive_count | INTEGER | | NO | OA count |
| central_count | INTEGER | | NO | UA count |
| hypopnea_count | INTEGER | | NO | Hypopnea count (unclassified by mechanism) |
| rera_count | INTEGER | | NO | RERA count |
| clear_airway_count | INTEGER | | NO | CA Clear airway count |
| obstructive_hypopnea_count | INTEGER | | NO | OH count (added in v18) |
| central_hypopnea_count | INTEGER | | NO | CH count (added in v18) |
| all_apnea_count | INTEGER | | NO | A count — undifferentiated apnea (added in v18) |
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

## event_lists ⚡ **NEW IN v8**

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

## event_data ⚡ **NEW IN v8**

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

## report_tree 🌲 **NEW IN v13 (replaces reports/report_contents from v11)**

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
