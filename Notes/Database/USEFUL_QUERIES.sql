-- OSCAR Database Useful Queries
-- Queries to inspect profiles, machines, sessions, and summaries

-- ============================================
-- MACHINE TYPE CONVERSION
-- ============================================
-- The machines.machine_type field is an integer. Use this CASE statement
-- to convert it to a readable string in your queries:
--
-- CASE machine_type
--     WHEN 1 THEN 'CPAP'
--     WHEN 2 THEN 'OXI'
--     WHEN 3 THEN 'SLEEP'
--     WHEN 4 THEN 'JOUR'
--     WHEN 5 THEN 'POS'
--     ELSE CAST(machine_type AS TEXT)
-- END as machine_type_name
--
-- Machine Type Codes:
--   1 = CPAP (Continuous Positive Airway Pressure devices)
--   2 = OXI (Oximetry devices)
--   3 = SLEEP (Sleep stage tracking devices)
--   4 = JOUR (Journal/diary entries)
--   5 = POS (Body position sensors)
--   Other = Display the number as-is

-- ============================================
-- PROFILES AND MACHINES
-- ============================================

-- View all profiles with their basic info
SELECT 
    id,
    username,
    id,
    created_at,
    updated_at
FROM profiles
ORDER BY username;

-- View all machines with their details
SELECT 
    id,
    profile_id,
    serial_number,
    model,
    brand,
    machine_type,
    CASE machine_type
        WHEN 1 THEN 'CPAP'
        WHEN 2 THEN 'OXI'
        WHEN 3 THEN 'SLEEP'
        WHEN 4 THEN 'JOURNAL'
        WHEN 5 THEN 'POS'
        ELSE CAST(machine_type AS TEXT)
    END as machine_type_name,
    created_at
FROM machines
ORDER BY profile_id, created_at;

-- View profiles with their machines (JOIN)
SELECT 
    p.id as profile_id,
    p.username as profile_name,
    p.username,
    m.id as machine_id,
    m.serial_number,
    m.model,
    m.brand,
    CASE m.machine_type
        WHEN 1 THEN 'CPAP'
        WHEN 2 THEN 'OXI'
        WHEN 3 THEN 'SLEEP'
        WHEN 4 THEN 'JOURNAL'
        WHEN 5 THEN 'POS'
        ELSE CAST(m.machine_type AS TEXT)
    END as machine_type,
    m.created_at as machine_added
FROM profiles p
LEFT JOIN machines m ON p.id = m.profile_id
ORDER BY p.username, m.created_at;

-- Count machines per profile
SELECT 
    p.id as profile_id,
    p.name as profile_name,
    COUNT(m.id) as machine_count
FROM profiles p
LEFT JOIN machines m ON p.id = m.profile_id
GROUP BY p.id, p.name
ORDER BY machine_count DESC, p.name;

-- ============================================
-- SESSIONS
-- ============================================

-- View all sessions with machine info
SELECT 
    s.id as db_session_id,
    s.session_id,
    datetime(s.start_time/1000, 'unixepoch', 'localtime') as start_time,
    datetime(s.end_time/1000, 'unixepoch', 'localtime') as end_time,
    ROUND(s.duration / 3600000.0, 2) as duration_hours,
    s.enabled,
    m.serial_number,
    m.model,
    p.username as profile_name
FROM sessions s
JOIN machines m ON s.machine_id = m.id
JOIN profiles p ON m.profile_id = p.id
ORDER BY s.start_time DESC
LIMIT 50;

-- Count sessions per machine
SELECT 
    m.serial_number,
    m.model,
    p.username as profile_name,
    COUNT(s.id) as session_count,
    MIN(datetime(s.start_time/1000, 'unixepoch', 'localtime')) as first_session,
    MAX(datetime(s.start_time/1000, 'unixepoch', 'localtime')) as last_session
FROM machines m
JOIN profiles p ON m.profile_id = p.id
LEFT JOIN sessions s ON m.id = s.machine_id
GROUP BY m.id, m.serial_number, m.model, p.username
ORDER BY p.username, m.serial_number;

-- ============================================
-- SESSION SUMMARIES
-- ============================================

-- View session summaries with readable dates
SELECT 
    ss.id,
    datetime(s.start_time/1000, 'unixepoch', 'localtime') as session_date,
    ROUND(ss.ahi, 2) as ahi,
    ROUND(ss.hours_used, 2) as hours,
    ss.obstructive_count as OA,
    ss.clear_airway_count as CA,
    ss.unclassified_count as UA,
    ss.hypopnea_count as H,
    ROUND(ss.pressure_avg, 2) as pressure_avg,
    ROUND(ss.leak_total_avg, 2) as leak_avg,
    m.model,
    p.username as profile_name
FROM session_summaries ss
JOIN sessions s ON ss.session_id = s.id
JOIN machines m ON s.machine_id = m.id
JOIN profiles p ON m.profile_id = p.id
ORDER BY s.start_time DESC
LIMIT 50;

-- Calculate AHI statistics per machine
SELECT 
    m.serial_number,
    m.model,
    p.username as profile_name,
    COUNT(ss.id) as nights,
    ROUND(AVG(ss.ahi), 2) as avg_ahi,
    ROUND(MIN(ss.ahi), 2) as min_ahi,
    ROUND(MAX(ss.ahi), 2) as max_ahi,
    ROUND(AVG(ss.hours_used), 2) as avg_hours
FROM session_summaries ss
JOIN sessions s ON ss.session_id = s.id
JOIN machines m ON s.machine_id = m.id
JOIN profiles p ON m.profile_id = p.id
WHERE ss.ahi > 0  -- Only include sessions with valid AHI
GROUP BY m.id, m.serial_number, m.model, p.username
ORDER BY p.username, m.serial_number;

-- Recent sessions with full summary data
SELECT 
    datetime(s.start_time/1000, 'unixepoch', 'localtime') as date,
    ROUND(ss.ahi, 2) as AHI,
    ROUND(ss.rdi, 2) as RDI,
    ROUND(ss.oahi, 2) as OAHI,
    ROUND(ss.cahi, 2) as CAHI,
    ss.obstructive_count as OA,
    ss.clear_airway_count as CA,
    ss.unclassified_count as UA,
    ss.all_apnea_count as A,
    ss.hypopnea_count as H,
    ss.obstructive_hypopnea_count as OH,
    ss.central_hypopnea_count as CH,
    ss.rera_count as RERA,
    ROUND(ss.pressure_avg, 2) as P_avg,
    ROUND(ss.pressure_95th, 2) as P_95,
    ROUND(ss.leak_total_avg, 2) as Leak_avg,
    ROUND(ss.leak_total_95th, 2) as Leak_95,
    ROUND(ss.hours_used, 2) as Hours,
    p.username as profile
FROM session_summaries ss
JOIN sessions s ON ss.session_id = s.id
JOIN machines m ON s.machine_id = m.id
JOIN profiles p ON m.profile_id = p.id
ORDER BY s.start_time DESC
LIMIT 30;

-- ============================================
-- COMPLIANCE TRACKING
-- ============================================

-- Usage compliance (hours per night)
SELECT 
    date(s.start_time/1000, 'unixepoch', 'localtime') as date,
    ROUND(ss.hours_used, 2) as hours,
    CASE 
        WHEN ss.hours_used >= 4 THEN 'Compliant'
        ELSE 'Non-compliant'
    END as compliance,
    p.username as profile
FROM session_summaries ss
JOIN sessions s ON ss.session_id = s.id
JOIN machines m ON s.machine_id = m.id
JOIN profiles p ON m.profile_id = p.id
WHERE s.enabled = 1
ORDER BY s.start_time DESC
LIMIT 90;

-- Monthly compliance summary
SELECT 
    strftime('%Y-%m', s.start_time/1000, 'unixepoch', 'localtime') as month,
    p.username as profile,
    COUNT(*) as nights,
    SUM(CASE WHEN ss.hours_used >= 4 THEN 1 ELSE 0 END) as compliant_nights,
    ROUND(100.0 * SUM(CASE WHEN ss.hours_used >= 4 THEN 1 ELSE 0 END) / COUNT(*), 1) as compliance_pct,
    ROUND(AVG(ss.hours_used), 2) as avg_hours,
    ROUND(AVG(ss.ahi), 2) as avg_ahi
FROM session_summaries ss
JOIN sessions s ON ss.session_id = s.id
JOIN machines m ON s.machine_id = m.id
JOIN profiles p ON m.profile_id = p.id
WHERE s.enabled = 1
GROUP BY strftime('%Y-%m', s.start_time/1000, 'unixepoch', 'localtime'), p.username
ORDER BY month DESC, p.username;

-- ============================================
-- DATA QUALITY CHECKS
-- ============================================

-- Check for sessions without summaries
SELECT 
    s.id,
    s.session_id,
    datetime(s.start_time/1000, 'unixepoch', 'localtime') as start_time,
    m.serial_number,
    p.username as profile_name,
    'NO SUMMARY' as issue
FROM sessions s
JOIN machines m ON s.machine_id = m.id
JOIN profiles p ON m.profile_id = p.id
LEFT JOIN session_summaries ss ON s.id = ss.session_id
WHERE ss.id IS NULL
ORDER BY s.start_time DESC;

-- Check for summaries with zero values (might indicate missing data)
SELECT 
    datetime(s.start_time/1000, 'unixepoch', 'localtime') as date,
    ss.ahi,
    ss.hours_used,
    ss.obstructive_count,
    ss.clear_airway_count,
    ss.hypopnea_count,
    m.serial_number,
    p.username as profile,
    'All zeros - check data' as note
FROM session_summaries ss
JOIN sessions s ON ss.session_id = s.id
JOIN machines m ON s.machine_id = m.id
JOIN profiles p ON m.profile_id = p.id
WHERE ss.ahi = 0
  AND ss.obstructive_count = 0
  AND ss.clear_airway_count = 0
  AND ss.hypopnea_count = 0
  AND ss.obstructive_hypopnea_count = 0
  AND ss.central_hypopnea_count = 0
  AND ss.all_apnea_count = 0
ORDER BY s.start_time DESC;

-- Verify the OAHI + CAHI = AHI identity (schema v18+).
-- Every AHI-contributing channel belongs to exactly one bucket, so any row listed
-- here means a channel was added to ahiChannels without being placed in a bucket.
-- Only rows written at v18 or later can satisfy this: earlier rows have 0 in the new
-- columns by design and are excluded by the mask_on_hours/ahi guard below only in the
-- trivial case, so restrict by date if the profile predates the upgrade.
SELECT
    ds.date,
    ROUND(ds.ahi, 4)  as AHI,
    ROUND(ds.oahi, 4) as OAHI,
    ROUND(ds.cahi, 4) as CAHI,
    ROUND(ds.oahi + ds.cahi - ds.ahi, 4) as difference
FROM daily_summaries ds
WHERE ds.mask_on_hours > 0
  AND (ds.oahi > 0 OR ds.cahi > 0)          -- skip un-migrated rows
  AND ABS(ds.oahi + ds.cahi - ds.ahi) > 0.005
ORDER BY ds.date DESC;

-- ============================================
-- QUICK STATS
-- ============================================

-- Database summary counts
SELECT 
    'Profiles' as table_name, 
    COUNT(*) as count 
FROM profiles
UNION ALL
SELECT 
    'Machines', 
    COUNT(*) 
FROM machines
UNION ALL
SELECT 
    'Sessions', 
    COUNT(*) 
FROM sessions
UNION ALL
SELECT 
    'Session Summaries', 
    COUNT(*) 
FROM session_summaries
UNION ALL
SELECT 
    'Session Settings', 
    COUNT(*) 
FROM session_settings
UNION ALL
SELECT 
    'Session Channels', 
    COUNT(*) 
FROM session_channels
UNION ALL
SELECT 
    'Respiratory Events' as table_name, 
    COUNT(*) as count 
FROM respiratory_events
UNION ALL
SELECT 
    'Daily Summaries', 
    COUNT(*) 
FROM daily_summaries
UNION ALL
SELECT 
    'Event Data', 
    COUNT(*) 
FROM event_data
UNION ALL
SELECT 
    'Event Lists', 
    COUNT(*) 
FROM event_lists;

-- ============================================
-- ADDITIONAL QUERY FILES
-- ============================================
-- For more specialized queries, see:
--   - QUERY_RECENT_SESSION_SETTINGS.sql - Detailed queries for session settings with channel names
--   - HOW_TO_USE_QUERIES.md - Guide for using SQL queries with OSCAR database
