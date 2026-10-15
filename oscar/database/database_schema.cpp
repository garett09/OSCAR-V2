/* Database Schema Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file implements the DatabaseSchema class which creates and
 * maintains the OSCAR SQLite database schema.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "database_schema.h"
#include "session_summaries_repository.h"
#include "database_manager.h"
#include "reports_initializer.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QSettings>
#include "version.h"

/*
 * Create the complete database schema
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * Creates all tables, indexes, and sets the schema version.
 * Safe to call multiple times - uses IF NOT EXISTS.
 */
bool DatabaseSchema::createSchema(QSqlDatabase& db)
{
    qDebug() << "DatabaseSchema: Creating schema...";

    // Create tables in order (schema_version first, then profiles, then machines)
    if (!createSchemaVersionTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create schema_version table";
        return false;
    }

    if (!createProfilesTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create profiles table";
        return false;
    }

    if (!createMachinesTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create machines table";
        return false;
    }

    if (!createUserInfoTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create user_info table";
        return false;
    }

    if (!createDoctorInfoTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create doctor_info table";
        return false;
    }

    if (!createProfilePreferencesTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create profile_preferences table";
        return false;
    }

    // Session data tables (schema version 3)
    if (!createSessionsTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create sessions table";
        return false;
    }

    if (!createSessionSettingsTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create session_settings table";
        return false;
    }

    if (!createSessionChannelsTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create session_channels table";
        return false;
    }

    if (!createSessionChannelValuesTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create session_channel_values table";
        return false;
    }

    if (!createRespiratoryEventsTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create respiratory_events table";
        return false;
    }

    if (!createSessionSummariesTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create session_summaries table";
        return false;
    }

    if (!createSessionSlicesTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create session_slices table";
        return false;
    }

    // Channel tables (schema version 5)
    if (!createChannelsTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create channels table";
        return false;
    }

    if (!createChannelOptionsTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create channel_options table";
        return false;
    }

    // Daily summaries table (schema version 6)
    if (!createDailySummariesTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create daily_summaries table";
        return false;
    }

    // Event data tables (schema version 8)
    if (!createEventListsTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create event_lists table";
        return false;
    }

    if (!createEventDataTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create event_data table";
        return false;
    }

    // Report tree table (schema version 13 - replaces reports/report_contents)
    if (!createReportTreeTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create report_tree table";
        return false;
    }

    // Global app preferences table (schema version 14 - replaces Preferences.xml)
    if (!createAppPreferencesTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create app_preferences table";
        return false;
    }

    // Unified graph layouts table (schema version 14 - replaces .shg files)
    if (!createGraphLayoutsTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create graph_layouts table";
        return false;
    }

    // Device time corrections table (schema version 17)
    if (!createDeviceTimeCorrectionsTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create device_time_corrections table";
        return false;
    }

    // Night clock offsets table (schema version 18)
    if (!createNightClockOffsetsTable(db)) {
        qCritical() << "DatabaseSchema: Failed to create night_clock_offsets table";
        return false;
    }

    // Create indexes
    if (!createIndexes(db)) {
        qCritical() << "DatabaseSchema: Failed to create indexes";
        return false;
    }

    // Set schema version
    if (!setSchemaVersion(db, CURRENT_SCHEMA_VERSION)) {
        qCritical() << "DatabaseSchema: Failed to set schema version";
        return false;
    }

    // Initialize report tree with root nodes and system reports
    if (!ReportsInitializer::initializeReportTree(db)) {
        qCritical() << "DatabaseSchema: Failed to initialize report tree";
        return false;
    }

    qDebug() << "DatabaseSchema: Schema creation complete";
    return true;
}

/*
 * Get the current schema version from database
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: Schema version number, or 0 if not found/error
 */
int DatabaseSchema::getSchemaVersion(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    if (!query.exec("SELECT version FROM schema_version LIMIT 1")) {
        qWarning() << "DatabaseSchema: Failed to get schema version:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::getSchemaVersion", query);
        return 0;
    }

    if (query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}


/*
 * Upgrade database schema to current version
 *
 * Parameters:
 *   db - Database connection to use (any thread; need not be DatabaseManager's)
 *   fromVersion - Current version in database
 *   progress - Optional callback invoked before each migration step
 *
 * Returns: true if upgrade succeeded, false otherwise
 *
 * Applies incremental, additive migrations. No existing data is destroyed.
 * Each step calls the relevant createXxx / ALTER TABLE helpers and then advances
 * the stored schema version before moving to the next step.
 */
bool DatabaseSchema::upgradeSchema(QSqlDatabase& db, int fromVersion,
                                   const UpgradeProgress& progress)
{
    qDebug() << "DatabaseSchema::upgradeSchema: from" << fromVersion << "to" << CURRENT_SCHEMA_VERSION;

    if (fromVersion == CURRENT_SCHEMA_VERSION) {
        return true;
    }

    // One entry per consecutive version pair, in ascending order; each takes the
    // schema from `from` to `from + 1`. Add a new entry here for every new version.
    struct MigrationStep {
        int from;
        bool (*migrate)(QSqlDatabase&);
    };
    static const MigrationStep steps[] = {
        { 13, &migrateV13ToV14 },
        { 14, &migrateV14ToV15 },
        { 15, &migrateV15ToV16 },
        { 16, &migrateV16ToV17 },
        { 17, &migrateV17ToV18 },
        { 18, &migrateV18ToV19 },
        { 19, &migrateV19ToV20 },
    };
    const int stepsAvailable = int(sizeof(steps) / sizeof(steps[0]));

    // Locate the first step that applies; versions older than the table have no path.
    int first = -1;
    for (int i = 0; i < stepsAvailable; ++i) {
        if (steps[i].from == fromVersion) {
            first = i;
            break;
        }
    }
    if (first < 0) {
        qCritical() << "DatabaseSchema: No migration path from version" << fromVersion;
        return false;
    }

    const int stepCount = stepsAvailable - first;
    for (int i = first; i < stepsAvailable; ++i) {
        const int from = steps[i].from;
        const int to = from + 1;
        if (progress) {
            progress(i - first + 1, stepCount, from, to);
        }
        if (!steps[i].migrate(db)) {
            qCritical() << "DatabaseSchema: v" << from << "->v" << to << "migration failed";
            return false;
        }
        fromVersion = to;
    }

    if (fromVersion != CURRENT_SCHEMA_VERSION) {
        qCritical() << "DatabaseSchema: migration table ends at version" << fromVersion
                    << "but CURRENT_SCHEMA_VERSION is" << CURRENT_SCHEMA_VERSION;
        return false;
    }

    qDebug() << "DatabaseSchema::upgradeSchema: complete";
    return true;
}

/*
 * Create the schema_version table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * This table tracks the schema version for future upgrades.
 */
bool DatabaseSchema::createSchemaVersionTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS schema_version ("
        "    version INTEGER PRIMARY KEY,"
        "    applied_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create schema_version table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createSchemaVersionTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: schema_version table created";
    return true;
}

/*
 * Create the profiles table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The profiles table stores basic user profile information.
 */
bool DatabaseSchema::createProfilesTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS profiles ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    username TEXT UNIQUE NOT NULL,"
        "    data_folder TEXT NOT NULL,"
        "    status TEXT DEFAULT 'active' CHECK(status IN ('active', 'missing', 'archived')),"
        "    status_changed_at TEXT,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create profiles table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createProfilesTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: profiles table created";
    return true;
}

/*
 * Create the machines table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The machines table stores information about CPAP machines and
 * other devices associated with each profile.
 */
bool DatabaseSchema::createMachinesTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS machines ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    profile_id INTEGER NOT NULL,"
        "    machine_id INTEGER NOT NULL,"
        "    loader_name TEXT NOT NULL,"
        "    machine_type INTEGER NOT NULL,"
        "    brand TEXT,"
        "    model TEXT,"
        "    series TEXT,"
        "    serial_number TEXT,"
        "    model_number TEXT,"
        "    last_imported TEXT,"
        "    purge_date TEXT,"
        "    data_version INTEGER DEFAULT 0,"
        "    properties TEXT,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,"
        "    UNIQUE(profile_id, machine_id)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create machines table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createMachinesTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: machines table created";
    return true;
}

/*
 * Create database indexes
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * Creates indexes to optimize common query patterns.
 */
bool DatabaseSchema::createIndexes(QSqlDatabase& db)
{
    QSqlQuery query(db);
    QStringList indexes;

    // Index for profile lookups by username
    indexes << "CREATE INDEX IF NOT EXISTS idx_profiles_username ON profiles(username)";

    // Index for machine lookups by profile
    indexes << "CREATE INDEX IF NOT EXISTS idx_machines_profile ON machines(profile_id)";

    // Index for machine lookups by serial number
    indexes << "CREATE INDEX IF NOT EXISTS idx_machines_serial ON machines(serial_number)";

    // Index for machine lookups by loader name
    indexes << "CREATE INDEX IF NOT EXISTS idx_machines_loader ON machines(loader_name)";

    // Index for user_info lookups by profile
    indexes << "CREATE INDEX IF NOT EXISTS idx_user_info_profile ON user_info(profile_id)";

    // Index for doctor_info lookups by profile
    indexes << "CREATE INDEX IF NOT EXISTS idx_doctor_info_profile ON doctor_info(profile_id)";

    // Index for preference lookups by profile and category
    indexes << "CREATE INDEX IF NOT EXISTS idx_preferences_profile ON profile_preferences(profile_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_preferences_category ON profile_preferences(profile_id, category)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_preferences_key ON profile_preferences(profile_id, category, key)";

    // Session indexes (schema version 3)
    indexes << "CREATE INDEX IF NOT EXISTS idx_sessions_machine ON sessions(machine_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_sessions_time ON sessions(start_time, end_time)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_sessions_enabled ON sessions(machine_id, enabled)";
    indexes << "CREATE UNIQUE INDEX IF NOT EXISTS idx_sessions_unique ON sessions(machine_id, session_id)";
    
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_settings_session ON session_settings(session_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_settings_channel ON session_settings(session_id, channel_id)";
    
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_channels_session ON session_channels(session_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_channels_channel ON session_channels(channel_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_channels_lookup ON session_channels(session_id, channel_id)";
    
    indexes << "CREATE INDEX IF NOT EXISTS idx_respiratory_events_session ON respiratory_events(session_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_respiratory_events_type ON respiratory_events(session_id, event_type)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_respiratory_events_time ON respiratory_events(start_time, end_time)";
    
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_summaries_session ON session_summaries(session_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_summaries_ahi ON session_summaries(ahi)";
    
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_slices_session ON session_slices(session_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_slices_time ON session_slices(start_time, end_time)";

    // Channels indexes (schema version 5)
    indexes << "CREATE INDEX IF NOT EXISTS idx_channels_profile ON channels(profile_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_channels_code ON channels(channel_code)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_channels_lookup ON channels(profile_id, channel_id)";
    
    indexes << "CREATE INDEX IF NOT EXISTS idx_channel_options_channel ON channel_options(channel_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_channel_options_lookup ON channel_options(channel_id, option_key)";

    // Daily summaries indexes (schema version 6; profile-machine index removed in v16)
    indexes << "CREATE INDEX IF NOT EXISTS idx_daily_summaries_profile_date ON daily_summaries(profile_id, date)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_daily_summaries_ahi ON daily_summaries(ahi)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_daily_summaries_compliance ON daily_summaries(profile_id, is_compliant)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_daily_summaries_date_range ON daily_summaries(profile_id, date DESC)";

    // Event data indexes (schema version 8)
    indexes << "CREATE INDEX IF NOT EXISTS idx_event_lists_session ON event_lists(session_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_event_lists_channel ON event_lists(session_id, channel_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_event_lists_type ON event_lists(event_type)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_event_lists_time ON event_lists(session_id, first_time, last_time)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_event_data_eventlist ON event_data(eventlist_id)";

    // Profile ID indexes (schema version 12 - denormalization for query performance)
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_summaries_profile ON session_summaries(profile_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_summaries_profile_date ON session_summaries(profile_id, session_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_settings_profile ON session_settings(profile_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_settings_profile_channel ON session_settings(profile_id, channel_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_channels_profile ON session_channels(profile_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_session_channels_profile_channel ON session_channels(profile_id, channel_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_event_lists_profile ON event_lists(profile_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_event_lists_profile_channel ON event_lists(profile_id, channel_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_respiratory_events_profile ON respiratory_events(profile_id)";
    indexes << "CREATE INDEX IF NOT EXISTS idx_respiratory_events_profile_type ON respiratory_events(profile_id, event_type)";

    // Execute each index creation
    for (const QString& sql : indexes) {
        if (!query.exec(sql)) {
            qWarning() << "DatabaseSchema: Failed to create index:" 
                      << query.lastError().text();
            // Continue with other indexes even if one fails
        }
    }

    qDebug() << "DatabaseSchema: Indexes created";
    return true;
}

/*
 * Set the schema version in the database
 *
 * Parameters:
 *   db - Database connection to use
 *   version - Version number to set
 *
 * Returns: true if successful, false otherwise
 *
 * Inserts or updates the schema version number.
 */
bool DatabaseSchema::setSchemaVersion(QSqlDatabase& db, int version)
{
    QSqlQuery query(db);
    
    // Delete all existing version records first
    if (!query.exec("DELETE FROM schema_version")) {
        qWarning() << "DatabaseSchema: Failed to clear old schema versions:" 
                   << query.lastError().text();
        // Continue anyway - might be empty table
    }
    
    // Insert the current version
    query.prepare("INSERT INTO schema_version (version) VALUES (?)");
    query.addBindValue(version);

    if (!query.exec()) {
        qCritical() << "DatabaseSchema: Failed to set schema version:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::setSchemaVersion", query);
        return false;
    }

    qDebug() << "DatabaseSchema: Schema version set to" << version;
    return true;
}

/*
 * Create the user_info table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The user_info table stores detailed user personal information.
 */
bool DatabaseSchema::createUserInfoTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS user_info ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    profile_id INTEGER NOT NULL UNIQUE,"
        "    dob TEXT,"
        "    first_name TEXT,"
        "    last_name TEXT,"
        "    address TEXT,"
        "    phone TEXT,"
        "    email TEXT,"
        "    country TEXT,"
        "    height REAL,"
        "    gender INTEGER,"
        "    timezone TEXT,"
        "    password_hash TEXT,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create user_info table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createUserInfoTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: user_info table created";
    return true;
}

/*
 * Create the doctor_info table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The doctor_info table stores doctor/medical provider information.
 */
bool DatabaseSchema::createDoctorInfoTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS doctor_info ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    profile_id INTEGER NOT NULL UNIQUE,"
        "    name TEXT,"
        "    phone TEXT,"
        "    email TEXT,"
        "    practice_name TEXT,"
        "    address TEXT,"
        "    patient_id TEXT,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create doctor_info table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createDoctorInfoTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: doctor_info table created";
    return true;
}

/*
 * Create the profile_preferences table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The profile_preferences table stores all profile settings as key-value pairs.
 * This includes CPAP settings, oximetry settings, session settings, etc.
 */
bool DatabaseSchema::createProfilePreferencesTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql =
        "CREATE TABLE IF NOT EXISTS profile_preferences ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    profile_id INTEGER NOT NULL,"
        "    category TEXT NOT NULL,"
        "    key TEXT NOT NULL,"
        "    value TEXT,"
        "    blob_value BLOB,"
        "    data_type TEXT,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,"
        "    UNIQUE(profile_id, category, key)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create profile_preferences table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createProfilePreferencesTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: profile_preferences table created";
    return true;
}

/*
 * Create the sessions table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The sessions table stores core session metadata and timing information.
 * Waveform data continues to be stored in separate files.
 */
bool DatabaseSchema::createSessionsTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS sessions ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    session_id INTEGER NOT NULL,"
        "    machine_id INTEGER NOT NULL,"
        "    start_time INTEGER NOT NULL,"
        "    end_time INTEGER NOT NULL,"
        "    duration INTEGER NOT NULL,"
        "    enabled INTEGER DEFAULT 1,"
        "    summary_only INTEGER DEFAULT 0,"
        "    no_settings INTEGER DEFAULT 0,"
        "    events_loaded INTEGER DEFAULT 0,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (machine_id) REFERENCES machines(id) ON DELETE CASCADE,"
        "    UNIQUE(machine_id, session_id)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create sessions table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createSessionsTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: sessions table created";
    return true;
}

/*
 * Create the session_settings table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The session_settings table stores machine configuration for each session
 * (CPAP mode, pressures, comfort settings, etc.)
 */
bool DatabaseSchema::createSessionSettingsTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS session_settings ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    session_id INTEGER NOT NULL,"
        "    profile_id INTEGER NOT NULL,"
        "    channel_id INTEGER NOT NULL,"
        "    value REAL NOT NULL,"
        "    data_type TEXT,"
        "    json_value TEXT,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,"
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,"
        "    UNIQUE(session_id, channel_id)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create session_settings table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createSessionSettingsTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: session_settings table created";
    return true;
}

/*
 * Create the session_channels table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The session_channels table stores summary statistics for each channel
 * in a session (count, avg, min, max, percentiles, etc.)
 */
bool DatabaseSchema::createSessionChannelsTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS session_channels ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    session_id INTEGER NOT NULL,"
        "    profile_id INTEGER NOT NULL,"
        "    channel_id INTEGER NOT NULL,"
        "    count INTEGER DEFAULT 0,"
        "    sum REAL DEFAULT 0,"
        "    avg REAL DEFAULT 0,"
        "    wavg REAL DEFAULT 0,"
        "    min REAL DEFAULT 0,"
        "    max REAL DEFAULT 0,"
        "    median REAL DEFAULT 0,"
        "    p90 REAL DEFAULT 0,"
        "    p95 REAL DEFAULT 0,"
        "    phys_min REAL DEFAULT 0,"
        "    phys_max REAL DEFAULT 0,"
        "    cph REAL DEFAULT 0,"
        "    sph REAL DEFAULT 0,"
        "    first_time INTEGER,"
        "    last_time INTEGER,"
        "    gain REAL DEFAULT 1.0,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,"
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,"
        "    UNIQUE(session_id, channel_id)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create session_channels table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createSessionChannelsTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: session_channels table created";
    return true;
}

/*
 * Create the session_channel_values table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The session_channel_values table stores detailed value/time summary data
 * for each channel. This data is used to calculate weighted averages and
 * other statistics that require knowledge of how long each distinct value
 * was held. This fixes a critical bug where m_valuesummary and m_timesummary
 * were not being persisted to the database.
 */
bool DatabaseSchema::createSessionChannelValuesTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS session_channel_values ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    session_channel_id INTEGER NOT NULL,"
        "    value INTEGER NOT NULL,"
        "    count INTEGER DEFAULT 0,"
        "    time_ms INTEGER DEFAULT 0,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (session_channel_id) REFERENCES session_channels(id) ON DELETE CASCADE,"
        "    UNIQUE(session_channel_id, value)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create session_channel_values table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createSessionChannelValuesTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: session_channel_values table created";
    return true;
}

/*
 * Create the respiratory_events table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The respiratory_events table stores individual respiratory events
 * (apneas, hypopneas, RERAs, etc.)
 */
bool DatabaseSchema::createRespiratoryEventsTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS respiratory_events ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    session_id INTEGER NOT NULL,"
        "    profile_id INTEGER NOT NULL,"
        "    channel_id INTEGER,"
        "    event_type INTEGER NOT NULL,"
        "    start_time INTEGER NOT NULL,"
        "    end_time INTEGER NOT NULL,"
        "    duration INTEGER NOT NULL,"
        "    desaturation REAL,"
        "    severity INTEGER,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,"
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create respiratory_events table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createRespiratoryEventsTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: respiratory_events table created";
    return true;
}

/*
 * Create the session_summaries table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The session_summaries table stores cached high-level session summaries
 * (AHI, event counts, pressure/leak statistics, etc.)
 */
bool DatabaseSchema::createSessionSummariesTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS session_summaries ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    session_id INTEGER NOT NULL UNIQUE,"
        "    profile_id INTEGER NOT NULL,"
        "    ahi REAL DEFAULT 0,"
        "    rdi REAL DEFAULT 0,"
        "    obstructive_count INTEGER DEFAULT 0,"
        "    unclassified_count INTEGER DEFAULT 0,"
        "    hypopnea_count INTEGER DEFAULT 0,"
        "    rera_count INTEGER DEFAULT 0,"
        "    clear_airway_count INTEGER DEFAULT 0,"
        "    obstructive_hypopnea_count INTEGER DEFAULT 0,"
        "    central_hypopnea_count INTEGER DEFAULT 0,"
        "    all_apnea_count INTEGER DEFAULT 0,"
        "    oahi REAL DEFAULT 0,"
        "    cahi REAL DEFAULT 0,"
        "    pressure_avg REAL,"
        "    pressure_min REAL,"
        "    pressure_max REAL,"
        "    pressure_95th REAL,"
        "    leak_total_avg REAL,"
        "    leak_total_95th REAL,"
        "    leak_total_max REAL,"
        "    spo2_avg REAL,"
        "    spo2_min REAL,"
        "    pulse_avg REAL,"
        "    hours_used REAL DEFAULT 0,"
        "    mask_on_hours REAL DEFAULT 0,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,"
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create session_summaries table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createSessionSummariesTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: session_summaries table created";
    return true;
}

/*
 * Create the session_slices table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The session_slices table stores mask-on/mask-off periods within sessions.
 */
bool DatabaseSchema::createSessionSlicesTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS session_slices ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    session_id INTEGER NOT NULL,"
        "    start_time INTEGER NOT NULL,"
        "    end_time INTEGER NOT NULL,"
        "    status INTEGER NOT NULL,"
        "    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create session_slices table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createSessionSlicesTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: session_slices table created";
    return true;
}

/*
 * Create the channels table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The channels table stores per-profile channel customizations
 * (enabled state, colors, labels, thresholds, etc.)
 */
bool DatabaseSchema::createChannelsTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS channels ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    profile_id INTEGER NOT NULL,"
        "    channel_id INTEGER NOT NULL,"
        "    channel_code TEXT NOT NULL,"
        "    type INTEGER,"
        "    enabled INTEGER NOT NULL DEFAULT 1,"
        "    default_color TEXT,"
        "    fullname TEXT,"
        "    label TEXT,"
        "    description TEXT,"
        "    lower_threshold REAL,"
        "    lower_threshold_color TEXT,"
        "    upper_threshold REAL,"
        "    upper_threshold_color TEXT,"
        "    show_in_overview INTEGER DEFAULT 0,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,"
        "    UNIQUE(profile_id, channel_id)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create channels table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createChannelsTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: channels table created";
    return true;
}

/*
 * Create the channel_options table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The channel_options table stores lookup values for LOOKUP-type channels
 * (e.g., CPAP_Mode: 0="CPAP", 1="APAP", 2="Bi-Level")
 */
bool DatabaseSchema::createChannelOptionsTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS channel_options ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    channel_id INTEGER NOT NULL,"
        "    option_key INTEGER NOT NULL,"
        "    option_value TEXT NOT NULL,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    UNIQUE(channel_id, option_key)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create channel_options table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createChannelOptionsTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: channel_options table created";
    return true;
}

/*
 * Create the daily_summaries table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The daily_summaries table stores cached daily aggregate statistics
 * for fast report generation without loading individual sessions.
 */
bool DatabaseSchema::createDailySummariesTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql =
        "CREATE TABLE IF NOT EXISTS daily_summaries ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    profile_id INTEGER NOT NULL,"
        "    date TEXT NOT NULL,"
        "    "
        "    session_count INTEGER DEFAULT 0,"
        "    enabled_session_count INTEGER DEFAULT 0,"
        "    "
        "    total_hours REAL DEFAULT 0,"
        "    mask_on_hours REAL DEFAULT 0,"
        "    "
        "    ahi REAL DEFAULT 0,"
        "    rdi REAL DEFAULT 0,"
        "    obstructive_count INTEGER DEFAULT 0,"
        "    unclassified_count INTEGER DEFAULT 0,"
        "    hypopnea_count INTEGER DEFAULT 0,"
        "    rera_count INTEGER DEFAULT 0,"
        "    clear_airway_count INTEGER DEFAULT 0,"
        "    obstructive_hypopnea_count INTEGER DEFAULT 0,"
        "    central_hypopnea_count INTEGER DEFAULT 0,"
        "    all_apnea_count INTEGER DEFAULT 0,"
        "    oahi REAL DEFAULT 0,"
        "    cahi REAL DEFAULT 0,"
        "    "
        "    pressure_avg REAL,"
        "    pressure_min REAL,"
        "    pressure_max REAL,"
        "    pressure_95th REAL,"
        "    "
        "    leak_total_avg REAL,"
        "    leak_total_95th REAL,"
        "    leak_total_max REAL,"
        "    leak_unintentional_avg REAL,"
        "    "
        "    spo2_avg REAL,"
        "    spo2_min REAL,"
        "    pulse_avg REAL,"
        "    pulse_min REAL,"
        "    pulse_max REAL,"
        "    "
        "    is_compliant INTEGER DEFAULT 0,"
        "    has_oximetry INTEGER DEFAULT 0,"
        "    "
        "    calculated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    sessions_hash TEXT,"
        "    "
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,"
        "    UNIQUE(profile_id, date)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create daily_summaries table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createDailySummariesTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: daily_summaries table created";
    return true;
}

/*
 * Create the event_lists table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The event_lists table stores metadata for each EventList (one row per EventList).
 * This includes timing, scaling, and dimensional information but not the actual data arrays.
 */
bool DatabaseSchema::createEventListsTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS event_lists ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    session_id INTEGER NOT NULL,"
        "    profile_id INTEGER NOT NULL,"
        "    channel_id INTEGER NOT NULL,"
        "    eventlist_index INTEGER NOT NULL DEFAULT 0,"
        "    event_type INTEGER NOT NULL,"
        "    first_time INTEGER NOT NULL,"
        "    last_time INTEGER NOT NULL,"
        "    count INTEGER NOT NULL,"
        "    rate REAL NOT NULL DEFAULT 0,"
        "    gain REAL NOT NULL DEFAULT 1.0,"
        "    offset REAL NOT NULL DEFAULT 0.0,"
        "    min_value REAL NOT NULL DEFAULT 0.0,"
        "    max_value REAL NOT NULL DEFAULT 0.0,"
        "    dimension TEXT,"
        "    has_second_field INTEGER NOT NULL DEFAULT 0,"
        "    min2_value REAL,"
        "    max2_value REAL,"
        "    data_size INTEGER NOT NULL DEFAULT 0,"
        "    compressed_size INTEGER,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,"
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,"
        "    UNIQUE(session_id, channel_id, eventlist_index)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create event_lists table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createEventListsTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: event_lists table created";
    return true;
}

/*
 * Create the event_data table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The event_data table stores actual binary data for each EventList (one row per EventList).
 * Data is stored as compressed BLOBs for efficiency, with checksums for integrity verification.
 */
bool DatabaseSchema::createEventDataTable(QSqlDatabase& db)
{
    QSqlQuery query(db);
    
    QString sql = 
        "CREATE TABLE IF NOT EXISTS event_data ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    eventlist_id INTEGER NOT NULL,"
        "    data_blob BLOB,"
        "    data_compressed BLOB,"
        "    data2_blob BLOB,"
        "    data2_compressed BLOB,"
        "    time_blob BLOB,"
        "    time_compressed BLOB,"
        "    compression_method INTEGER NOT NULL DEFAULT 0,"
        "    checksum INTEGER,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (eventlist_id) REFERENCES event_lists(id) ON DELETE CASCADE,"
        "    UNIQUE(eventlist_id)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create event_data table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createEventDataTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: event_data table created";
    return true;
}

/*
 * Create the report_tree table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * The report_tree table stores the hierarchical report tree with System
 * and User root nodes. Replaces the old reports/report_contents tables.
 */
bool DatabaseSchema::createReportTreeTable(QSqlDatabase& db)
{
    QSqlQuery query(db);

    QString sql =
        "CREATE TABLE IF NOT EXISTS report_tree ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    parent_id INTEGER,"
        "    name TEXT NOT NULL,"
        "    node_type TEXT NOT NULL CHECK(node_type IN ('root', 'folder', 'report')),"
        "    source TEXT NOT NULL CHECK(source IN ('system', 'user')),"
        "    description TEXT,"
        "    query TEXT,"
        "    display_order INTEGER DEFAULT 0,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (parent_id) REFERENCES report_tree(id) ON DELETE CASCADE,"
        "    UNIQUE(parent_id, name)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create report_tree table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createReportTreeTable", query);
        return false;
    }

    // Create indexes for report_tree
    if (!query.exec("CREATE INDEX IF NOT EXISTS idx_report_tree_parent ON report_tree(parent_id)")) {
        qWarning() << "DatabaseSchema: Failed to create report_tree parent index:" << query.lastError().text();
    }

    if (!query.exec("CREATE INDEX IF NOT EXISTS idx_report_tree_source ON report_tree(source)")) {
        qWarning() << "DatabaseSchema: Failed to create report_tree source index:" << query.lastError().text();
    }

    if (!query.exec("CREATE INDEX IF NOT EXISTS idx_report_tree_type ON report_tree(node_type)")) {
        qWarning() << "DatabaseSchema: Failed to create report_tree type index:" << query.lastError().text();
    }

    qDebug() << "DatabaseSchema: report_tree table created";
    return true;
}

/*
 * Check and update CSV report version
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * Delegates to ReportsInitializer to handle report version management.
 * This is called at every startup from DatabaseManager::initialize().
 */
bool DatabaseSchema::checkAndUpdateReportVersion(QSqlDatabase& db)
{
    return ReportsInitializer::checkAndUpdateReportVersion(db);
}

/*
 * Create the app_preferences table (schema version 14)
 *
 * Stores all global application preferences (formerly Preferences.xml).
 * Supports both scalar values (TEXT) and future binary values (BLOB).
 * Same shape as profile_preferences minus profile_id.
 */
bool DatabaseSchema::createAppPreferencesTable(QSqlDatabase& db)
{
    QSqlQuery query(db);

    QString sql =
        "CREATE TABLE IF NOT EXISTS app_preferences ("
        "    id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    category   TEXT NOT NULL DEFAULT 'general',"
        "    key        TEXT NOT NULL,"
        "    value      TEXT,"
        "    blob_value BLOB,"
        "    data_type  TEXT,"
        "    created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    updated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    UNIQUE(category, key)"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create app_preferences table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createAppPreferencesTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: app_preferences table created";
    return true;
}

/*
 * Create the graph_layouts table (schema version 14)
 *
 * Unified table for both shared named layouts (profile_id IS NULL) and
 * the per-profile current layout (profile_id NOT NULL, is_current = 1).
 * Data is a raw QDataStream BLOB — identical format to the old .shg files.
 */
bool DatabaseSchema::createGraphLayoutsTable(QSqlDatabase& db)
{
    QSqlQuery query(db);

    QString sql =
        "CREATE TABLE IF NOT EXISTS graph_layouts ("
        "    id             INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    profile_id     INTEGER,"
        "    view_name      TEXT NOT NULL,"
        "    slot_index     INTEGER NOT NULL DEFAULT 0,"
        "    is_current     INTEGER NOT NULL DEFAULT 0,"
        "    description    TEXT,"
        "    format_version INTEGER NOT NULL,"
        "    data           BLOB NOT NULL,"
        "    created_at     TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    updated_at     TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE"
        ")";

    if (!query.exec(sql)) {
        qCritical() << "DatabaseSchema: Failed to create graph_layouts table:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createGraphLayoutsTable", query);
        return false;
    }

    // Unique index for shared named slots (profile_id IS NULL)
    if (!query.exec(
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_graph_layouts_named "
            "ON graph_layouts(view_name, slot_index) WHERE profile_id IS NULL")) {
        qCritical() << "DatabaseSchema: Failed to create idx_graph_layouts_named:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createGraphLayoutsTable", query);
        return false;
    }

    // Unique index for per-profile current layouts
    if (!query.exec(
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_graph_layouts_current "
            "ON graph_layouts(profile_id, view_name) WHERE is_current = 1")) {
        qCritical() << "DatabaseSchema: Failed to create idx_graph_layouts_current:"
                    << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createGraphLayoutsTable", query);
        return false;
    }

    qDebug() << "DatabaseSchema: graph_layouts table created";
    return true;
}

/*
 * Migrate database from schema version 13 to 14
 *
 * All changes are purely additive — no existing data is touched.
 *   1. Create app_preferences table
 *   2. Create graph_layouts table (with partial unique indexes)
 *   3. Add blob_value column to profile_preferences
 *   4. Advance schema_version to 14
 */
bool DatabaseSchema::migrateV13ToV14(QSqlDatabase& db)
{
    qDebug() << "DatabaseSchema: Migrating v13 -> v14";

    if (!db.transaction()) {
        qCritical() << "DatabaseSchema: migrateV13ToV14: failed to start transaction";
        return false;
    }

    if (!createAppPreferencesTable(db)) {
        qCritical() << "DatabaseSchema: migrateV13ToV14: createAppPreferencesTable failed";
        db.rollback();
        return false;
    }

    if (!createGraphLayoutsTable(db)) {
        qCritical() << "DatabaseSchema: migrateV13ToV14: createGraphLayoutsTable failed";
        db.rollback();
        return false;
    }

    // Add blob_value column to profile_preferences (idempotent via IF NOT EXISTS workaround:
    // SQLite has no ALTER TABLE ADD COLUMN IF NOT EXISTS, so we ignore "duplicate column" errors)
    QSqlQuery q(db);
    if (!q.exec("ALTER TABLE profile_preferences ADD COLUMN blob_value BLOB")) {
        QString err = q.lastError().text();
        if (!err.contains("duplicate column", Qt::CaseInsensitive)) {
            qCritical() << "DatabaseSchema: migrateV13ToV14: ALTER TABLE profile_preferences failed:" << err;
            db.rollback();
            return false;
        }
        // Column already exists — safe to continue
    }

    if (!setSchemaVersion(db, 14)) {
        qCritical() << "DatabaseSchema: migrateV13ToV14: setSchemaVersion failed";
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        qCritical() << "DatabaseSchema: migrateV13ToV14: commit failed";
        db.rollback();
        return false;
    }

    qDebug() << "DatabaseSchema: Migration v13->v14 complete";
    return true;
}

/*
 * Migrate database from schema version 14 to 15
 *
 * Removes the dst_enabled column from user_info — it was stored but never
 * read by any computation, making it dead code.
 */
bool DatabaseSchema::migrateV14ToV15(QSqlDatabase& db)
{
    qDebug() << "DatabaseSchema: Migrating v14 -> v15";

    if (!db.transaction()) {
        qCritical() << "DatabaseSchema: migrateV14ToV15: failed to start transaction";
        return false;
    }

    QSqlQuery q(db);
    if (!q.exec("ALTER TABLE user_info DROP COLUMN dst_enabled")) {
        QString err = q.lastError().text();
        // Column may already be absent (e.g. fresh install migrated past v14 cleanly)
        if (!err.contains("no such column", Qt::CaseInsensitive)) {
            qCritical() << "DatabaseSchema: migrateV14ToV15: DROP COLUMN failed:" << err;
            db.rollback();
            return false;
        }
    }

    // Remove orphaned "DST" preference rows that UserInfo::initPref() used to write.
    if (!q.exec("DELETE FROM profile_preferences WHERE key = 'DST'")) {
        qCritical() << "DatabaseSchema: migrateV14ToV15: DELETE profile_preferences failed:"
                    << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::migrateV14ToV15", q);
        db.rollback();
        return false;
    }

    if (!setSchemaVersion(db, 15)) {
        qCritical() << "DatabaseSchema: migrateV14ToV15: setSchemaVersion failed";
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        qCritical() << "DatabaseSchema: migrateV14ToV15: commit failed";
        db.rollback();
        return false;
    }

    qDebug() << "DatabaseSchema: Migration v14->v15 complete";
    return true;
}

bool DatabaseSchema::createDeviceTimeCorrectionsTable(QSqlDatabase& db)
{
    QSqlQuery q(db);
    if (!q.exec(R"(
        CREATE TABLE IF NOT EXISTS device_time_corrections (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            machine_id  INTEGER NOT NULL REFERENCES machines(id) ON DELETE CASCADE,
            date_from   TEXT    NOT NULL,
            date_to     TEXT,
            type        TEXT    NOT NULL CHECK(type IN ('timezone','travel','dst','reset','offset','drift')),
            offset_ms   INTEGER,
            c0_ms       INTEGER,
            c1          REAL DEFAULT 0.0,
            reason      TEXT,
            applied_at  TEXT NOT NULL DEFAULT (datetime('now')),
            undone_at   TEXT
        )
    )")) {
        qCritical() << "DatabaseSchema: Failed to create device_time_corrections:" << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createDeviceTimeCorrectionsTable", q);
        return false;
    }
    if (!q.exec("CREATE INDEX IF NOT EXISTS idx_dtc_machine_date ON device_time_corrections(machine_id, date_from, type, undone_at)")) {
        qCritical() << "DatabaseSchema: Failed to create idx_dtc_machine_date:" << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::createDeviceTimeCorrectionsTable", q);
        return false;
    }
    return true;
}

/*
 * Migrate database from schema version 15 to 16
 *
 * Removes the machine_id column from daily_summaries.  Each row is a
 * profile-day rollup that already aggregates across all machines for that
 * date (one CPAP + zero or more oximetry/auxiliary devices), so the
 * machine_id key fragment was a design mistake.  The natural key changes
 * from (profile_id, date, machine_id) to (profile_id, date).
 *
 * Implementation note: SQLite cannot drop a column referenced by a UNIQUE
 * constraint or non-trivial index, so we follow the standard
 * rebuild-and-rename recipe.  The dedup safeguard
 * (ORDER BY machine_id IS NULL ASC, id ASC) is a no-op in current
 * deployments — every existing row maps cleanly to a unique
 * (profile_id, date) — but it protects against pathological databases
 * that ended up with both a NULL row and a machine-bound row for the
 * same date.
 */
bool DatabaseSchema::migrateV15ToV16(QSqlDatabase& db)
{
    qDebug() << "DatabaseSchema: Migrating v15 -> v16";

    if (!db.transaction()) {
        qCritical() << "DatabaseSchema: migrateV15ToV16: failed to start transaction";
        return false;
    }

    QSqlQuery q(db);

    // 1. Create the replacement table with the v16 shape.
    const char* createNewTable =
        "CREATE TABLE daily_summaries_new ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    profile_id INTEGER NOT NULL,"
        "    date TEXT NOT NULL,"
        "    session_count INTEGER DEFAULT 0,"
        "    enabled_session_count INTEGER DEFAULT 0,"
        "    total_hours REAL DEFAULT 0,"
        "    mask_on_hours REAL DEFAULT 0,"
        "    ahi REAL DEFAULT 0,"
        "    rdi REAL DEFAULT 0,"
        "    obstructive_count INTEGER DEFAULT 0,"
        "    unclassified_count INTEGER DEFAULT 0,"
        "    hypopnea_count INTEGER DEFAULT 0,"
        "    rera_count INTEGER DEFAULT 0,"
        "    clear_airway_count INTEGER DEFAULT 0,"
        "    pressure_avg REAL,"
        "    pressure_min REAL,"
        "    pressure_max REAL,"
        "    pressure_95th REAL,"
        "    leak_total_avg REAL,"
        "    leak_total_95th REAL,"
        "    leak_total_max REAL,"
        "    leak_unintentional_avg REAL,"
        "    spo2_avg REAL,"
        "    spo2_min REAL,"
        "    pulse_avg REAL,"
        "    pulse_min REAL,"
        "    pulse_max REAL,"
        "    is_compliant INTEGER DEFAULT 0,"
        "    has_oximetry INTEGER DEFAULT 0,"
        "    calculated_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        "    sessions_hash TEXT,"
        "    FOREIGN KEY (profile_id) REFERENCES profiles(id) ON DELETE CASCADE,"
        "    UNIQUE(profile_id, date)"
        ")";
    if (!q.exec(createNewTable)) {
        qCritical() << "DatabaseSchema: migrateV15ToV16: CREATE replacement table failed:"
                    << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::migrateV15ToV16", q);
        db.rollback();
        return false;
    }

    // 2. Copy data, dropping machine_id.  INSERT OR REPLACE keyed on the new
    //    UNIQUE(profile_id, date) constraint dedups any pre-existing rows; the
    //    ORDER BY makes machine-bound rows win over NULL ones.
    const char* copyData =
        "INSERT OR REPLACE INTO daily_summaries_new ("
        "    id, profile_id, date,"
        "    session_count, enabled_session_count,"
        "    total_hours, mask_on_hours,"
        "    ahi, rdi, obstructive_count, unclassified_count,"
        "    hypopnea_count, rera_count, clear_airway_count,"
        "    pressure_avg, pressure_min, pressure_max, pressure_95th,"
        "    leak_total_avg, leak_total_95th, leak_total_max, leak_unintentional_avg,"
        "    spo2_avg, spo2_min, pulse_avg, pulse_min, pulse_max,"
        "    is_compliant, has_oximetry, calculated_at, sessions_hash"
        ") "
        "SELECT "
        "    id, profile_id, date,"
        "    session_count, enabled_session_count,"
        "    total_hours, mask_on_hours,"
        "    ahi, rdi, obstructive_count, unclassified_count,"
        "    hypopnea_count, rera_count, clear_airway_count,"
        "    pressure_avg, pressure_min, pressure_max, pressure_95th,"
        "    leak_total_avg, leak_total_95th, leak_total_max, leak_unintentional_avg,"
        "    spo2_avg, spo2_min, pulse_avg, pulse_min, pulse_max,"
        "    is_compliant, has_oximetry, calculated_at, sessions_hash "
        "FROM daily_summaries "
        "ORDER BY (machine_id IS NULL) ASC, id ASC";
    if (!q.exec(copyData)) {
        qCritical() << "DatabaseSchema: migrateV15ToV16: copy data failed:"
                    << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::migrateV15ToV16", q);
        db.rollback();
        return false;
    }

    // 3. Drop the old table and rename the new one into place.
    if (!q.exec("DROP TABLE daily_summaries")) {
        qCritical() << "DatabaseSchema: migrateV15ToV16: DROP old table failed:"
                    << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::migrateV15ToV16", q);
        db.rollback();
        return false;
    }
    if (!q.exec("ALTER TABLE daily_summaries_new RENAME TO daily_summaries")) {
        qCritical() << "DatabaseSchema: migrateV15ToV16: RENAME failed:"
                    << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::migrateV15ToV16", q);
        db.rollback();
        return false;
    }

    // 4. Drop the now-stale per-machine index name (if it survived). The
    //    rebuild above dropped the table-bound indexes; this is belt-and-braces
    //    in case a future SQLite quirk leaves a stranded index entry.
    if (!q.exec("DROP INDEX IF EXISTS idx_daily_summaries_profile_machine")) {
        qCritical() << "DatabaseSchema: migrateV15ToV16: DROP INDEX failed:"
                    << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DatabaseSchema::migrateV15ToV16", q);
        db.rollback();
        return false;
    }

    // 5. Recreate the daily_summaries indexes that the rebuild dropped.
    static const char* recreateIndexes[] = {
        "CREATE INDEX IF NOT EXISTS idx_daily_summaries_profile_date "
            "ON daily_summaries(profile_id, date)",
        "CREATE INDEX IF NOT EXISTS idx_daily_summaries_ahi "
            "ON daily_summaries(ahi)",
        "CREATE INDEX IF NOT EXISTS idx_daily_summaries_compliance "
            "ON daily_summaries(profile_id, is_compliant)",
        "CREATE INDEX IF NOT EXISTS idx_daily_summaries_date_range "
            "ON daily_summaries(profile_id, date DESC)",
    };
    for (const char* indexSql : recreateIndexes) {
        if (!q.exec(indexSql)) {
            qCritical() << "DatabaseSchema: migrateV15ToV16: index rebuild failed:"
                        << q.lastError().text();
            DatabaseManager::instance().checkQueryError("DatabaseSchema::migrateV15ToV16", q);
            db.rollback();
            return false;
        }
    }


    if (!setSchemaVersion(db, 16)) {
        qCritical() << "DatabaseSchema: migrateV15ToV16: setSchemaVersion failed";
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        qCritical() << "DatabaseSchema: migrateV15ToV16: commit failed";
        db.rollback();
        return false;
    }

    qDebug() << "DatabaseSchema: Migration v15->v16 complete";
    return true;
}

/*
 * Migrate database from schema version 16 to 17
 *
 * Adds the device_time_corrections table for per-device per-night
 * time correction records.
 */
bool DatabaseSchema::migrateV16ToV17(QSqlDatabase& db)
{
    qDebug() << "DatabaseSchema: Migrating v16 -> v17";

    if (!db.transaction()) {
        qCritical() << "DatabaseSchema: migrateV16ToV17: failed to start transaction";
        return false;
    }

    if (!createDeviceTimeCorrectionsTable(db)) {
        qCritical() << "DatabaseSchema: migrateV16ToV17: createDeviceTimeCorrectionsTable failed";
        db.rollback();
        return false;
    }

    if (!setSchemaVersion(db, 17)) {
        qCritical() << "DatabaseSchema: migrateV16ToV17: setSchemaVersion failed";
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        qCritical() << "DatabaseSchema: migrateV16ToV17: commit failed";
        db.rollback();
        return false;
    }

    qDebug() << "DatabaseSchema: Migration v16->v17 complete";
    return true;
}

/*
 * Create the night_clock_offsets table
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * Stores one row per (machine_id, ref_machine_id, date) triple, recording
 * the measured clock offset at the start and end of the night, and whether
 * the measurement was used in drift correction.
 */
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

/*
 * Add the Central / Obstructive hypopnea columns to both summary tables
 *
 * Parameters:
 *   db - Database connection to use
 *
 * Returns: true if successful, false otherwise
 *
 * Adds all_apnea_count as well, which contributes to AHI but had never been stored.
 *
 * Purely additive: five ALTER TABLE ADD COLUMNs per table, each with a DEFAULT so
 * existing rows stay valid. The OH/CH columns are not backfilled — pre-v18 rows read
 * 0 in them until the day is recalculated or re-imported, which is a deliberate
 * decision (recalculating every historical day at upgrade time would be slow and,
 * for days whose original card is long gone, would not recover the OH/CH split
 * anyway).
 *
 * The #285 recompute of session_summaries (all_apnea_count from session_channels;
 * ahi/rdi/oahi/cahi as the sum of session_channels.cph) was originally done here. It
 * now lives in migrateV18ToV19, so that databases already at v18 — which never ran
 * it — are corrected as well.
 */
bool DatabaseSchema::addHypopneaSummaryColumns(QSqlDatabase& db)
{
    static const char* const newColumns[] = {
        "obstructive_hypopnea_count INTEGER DEFAULT 0",
        "central_hypopnea_count INTEGER DEFAULT 0",
        "all_apnea_count INTEGER DEFAULT 0",
        "oahi REAL DEFAULT 0",
        "cahi REAL DEFAULT 0"
    };
    static const char* const tables[] = { "session_summaries", "daily_summaries" };

    QSqlQuery q(db);
    for (const char* table : tables) {
        for (const char* column : newColumns) {
            const QString sql = QString("ALTER TABLE %1 ADD COLUMN %2")
                                    .arg(QString::fromLatin1(table), QString::fromLatin1(column));
            if (!q.exec(sql)) {
                // SQLite has no ADD COLUMN IF NOT EXISTS, so a column that is already
                // present is reported as an error and has to be tolerated to keep the
                // migration re-runnable.
                const QString err = q.lastError().text();
                if (!err.contains("duplicate column", Qt::CaseInsensitive)) {
                    qCritical() << "DatabaseSchema: addHypopneaSummaryColumns:" << sql << "failed:" << err;
                    DatabaseManager::instance().checkQueryError("DatabaseSchema::addHypopneaSummaryColumns", q);
                    return false;
                }
            }
        }
    }
    return true;
}

/*
 * Migrate database from schema version 17 to 18
 *
 * Adds the night_clock_offsets table for per-night clock offset measurements,
 * and the Central / Obstructive hypopnea columns to both summary tables.
 */
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

    if (!addHypopneaSummaryColumns(db)) {
        qCritical() << "DatabaseSchema: migrateV17ToV18: addHypopneaSummaryColumns failed";
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

/*
 * Migrate database from schema version 18 to 19.
 * Repair both independently introduced v18 schema additions idempotently.
 */
bool DatabaseSchema::migrateV18ToV19(QSqlDatabase& db)
{
    qDebug() << "DatabaseSchema: Migrating v18 -> v19";

    if (!db.transaction()) {
        qCritical() << "DatabaseSchema: migrateV18ToV19: failed to start transaction";
        return false;
    }

    if (!createNightClockOffsetsTable(db)) {
        qCritical() << "DatabaseSchema: migrateV18ToV19: createNightClockOffsetsTable failed";
        db.rollback();
        return false;
    }

    if (!addHypopneaSummaryColumns(db)) {
        qCritical() << "DatabaseSchema: migrateV18ToV19: addHypopneaSummaryColumns failed";
        db.rollback();
        return false;
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

/* Reconcile both branches' v19 schemas and apply upstream metric repairs. */
bool DatabaseSchema::migrateV19ToV20(QSqlDatabase& db)
{
    qDebug() << "DatabaseSchema: Migrating v19 -> v20";

    if (!db.transaction()) {
        qCritical() << "DatabaseSchema: migrateV19ToV20: failed to start transaction";
        return false;
    }

    // v19 was assigned independently by both branches. Repair both schema
    // additions before rebuilding metrics, regardless of which branch upgraded it.
    if (!createNightClockOffsetsTable(db) || !addHypopneaSummaryColumns(db)) {
        db.rollback();
        return false;
    }

    if (!SessionSummariesRepository::rebuildFromChannels(db, 0)) {
        qCritical() << "DatabaseSchema: migrateV19ToV20: session_summaries rebuild failed";
        db.rollback();
        return false;
    }

    // One pass over session_channels builds the two lookup tables every later statement
    // keys on; both are indexed so each per-row EXISTS is a point lookup. Channel ids
    // follow schema.cpp: ClearAirway 4097, Obstructive 4098, Hypopnea 4099, Apnea 4100,
    // RERA 4102, AllApnea 4112, ObstructiveHypopnea 4113, CentralHypopnea 4114.
    QStringList sql;
    sql << "DROP TABLE IF EXISTS temp.machine_reported"
        << "CREATE TEMP TABLE machine_reported AS"
           " SELECT DISTINCT s.machine_id, sc.channel_id"
           " FROM session_channels sc JOIN sessions s ON s.id = sc.session_id"
           " WHERE sc.count > 0 AND sc.channel_id IN (4097, 4098, 4099, 4100, 4102, 4112, 4113, 4114)"
        << "CREATE INDEX temp.idx_machine_reported ON machine_reported(machine_id, channel_id)"
        << "DROP TABLE IF EXISTS temp.profile_reported"
        << "CREATE TEMP TABLE profile_reported AS"
           " SELECT DISTINCT m.profile_id, mr.channel_id"
           " FROM machine_reported mr JOIN machines m ON m.id = mr.machine_id"
        << "CREATE INDEX temp.idx_profile_reported ON profile_reported(profile_id, channel_id)";

    // session_channels: zero-count OH/CH rows of machines that never scored either.
    sql << "DELETE FROM session_channels WHERE channel_id IN (4113, 4114) AND count = 0"
           " AND session_id IN (SELECT s.id FROM sessions s WHERE NOT EXISTS"
           "   (SELECT 1 FROM machine_reported mr WHERE mr.machine_id = s.machine_id"
           "    AND mr.channel_id IN (4113, 4114)))";

    // daily_summaries: NULL where no device of the profile reports the channel.
    const QString profileReported =
        "EXISTS (SELECT 1 FROM profile_reported pr"
        " WHERE pr.profile_id = daily_summaries.profile_id AND pr.channel_id %1)";
    struct DailyCol { const char * column; const char * channel; };
    static const DailyCol dailyCols[] = {
        { "clear_airway_count", "= 4097" }, { "obstructive_count",  "= 4098" },
        { "hypopnea_count",     "= 4099" }, { "unclassified_count", "= 4100" },
        { "rera_count",         "= 4102" }, { "all_apnea_count",    "= 4112" },
        { "obstructive_hypopnea_count", "= 4113" }, { "central_hypopnea_count", "= 4114" },
        { "rdi",  "= 4102" },
        { "oahi", "IN (4113, 4114)" }, { "cahi", "IN (4113, 4114)" },
    };
    for (const DailyCol & c : dailyCols) {
        sql << QString("UPDATE daily_summaries SET %1 = NULL WHERE NOT %2")
               .arg(QString::fromLatin1(c.column))
               .arg(profileReported.arg(QString::fromLatin1(c.channel)));
    }
    // Continuous statistics on daily rows were written as 0 when the day had no such
    // channel. Therapy pressure and pulse are never 0, and a night with zero average
    // and zero maximum leak does not occur on a real device, so 0 across the group is
    // "no data". leak_unintentional_avg is left alone: 0 can be genuine there.
    sql << "UPDATE daily_summaries SET pressure_avg = NULL, pressure_min = NULL, pressure_max = NULL,"
           " pressure_95th = NULL WHERE pressure_avg = 0 AND pressure_max = 0"
        << "UPDATE daily_summaries SET leak_total_avg = NULL, leak_total_95th = NULL, leak_total_max = NULL"
           " WHERE leak_total_avg = 0 AND leak_total_max = 0"
        << "UPDATE daily_summaries SET spo2_avg = NULL, spo2_min = NULL WHERE spo2_avg = 0"
        << "UPDATE daily_summaries SET pulse_avg = NULL, pulse_min = NULL, pulse_max = NULL WHERE pulse_avg = 0";

    // Profiles whose CPAP devices (those with sessions) disagree on some channel: a daily
    // row there cannot be rewritten in SQL, so the rows go and are regenerated on open.
    sql << "DELETE FROM daily_summaries WHERE profile_id IN ("
           "WITH mach AS (SELECT id, profile_id FROM machines WHERE machine_type = 1"
           "   AND id IN (SELECT DISTINCT machine_id FROM sessions)),"
           " per AS (SELECT mach.profile_id, mr.channel_id, COUNT(DISTINCT mr.machine_id) AS n"
           "   FROM mach JOIN machine_reported mr ON mr.machine_id = mach.id"
           "   GROUP BY mach.profile_id, mr.channel_id),"
           " tot AS (SELECT profile_id, COUNT(*) AS total FROM mach GROUP BY profile_id)"
           " SELECT DISTINCT per.profile_id FROM per JOIN tot ON tot.profile_id = per.profile_id"
           " WHERE per.n < tot.total)"
        << "DROP TABLE IF EXISTS temp.profile_reported"
        << "DROP TABLE IF EXISTS temp.machine_reported";

    QSqlQuery q(db);
    for (const QString & statement : sql) {
        if (!q.exec(statement)) {
            qCritical() << "DatabaseSchema: migrateV19ToV20:" << statement.left(80)
                        << "failed:" << q.lastError().text();
            DatabaseManager::instance().checkQueryError("DatabaseSchema::migrateV19ToV20", q);
            db.rollback();
            return false;
        }
    }

    if (!setSchemaVersion(db, 20)) {
        qCritical() << "DatabaseSchema: migrateV19ToV20: setSchemaVersion failed";
        db.rollback();
        return false;
    }
    if (!db.commit()) {
        qCritical() << "DatabaseSchema: migrateV19ToV20: commit failed";
        db.rollback();
        return false;
    }

    qDebug() << "DatabaseSchema: Migration v19->v20 complete";
    return true;
}
