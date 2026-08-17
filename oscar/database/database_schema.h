/* Database Schema Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file contains the DatabaseSchema class which defines the SQL
 * schema for OSCAR's SQLite database, including tables, indexes, and
 * schema versioning.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef DATABASE_SCHEMA_H
#define DATABASE_SCHEMA_H

#include <QSqlDatabase>
#include <QString>

/*!
 * \class DatabaseSchema
 * \brief Defines and creates the OSCAR database schema
 *
 * This class contains all SQL statements needed to create and maintain
 * the OSCAR database schema. It handles schema creation, versioning,
 * and future upgrades.
 *
 * Current schema version: 14
 * Version 14: All file-based data moved to database
 * - Added app_preferences table (replaces Preferences.xml)
 * - Added graph_layouts table (replaces layoutSettings/ and per-profile .shg files)
 * - Added blob_value column to profile_preferences
 */
class DatabaseSchema
{
public:
    /*!
     * \brief Current schema version number
     *
     * Increment this when schema changes. Used to determine if
     * database upgrades are needed.
     *
     * Version 18: Central / Obstructive hypopnea split
     * - session_summaries and daily_summaries each gained five columns:
     *   obstructive_hypopnea_count, central_hypopnea_count, all_apnea_count
     *   (INTEGER) and oahi, cahi (REAL).
     * - all_apnea_count closes a pre-existing gap: CPAP_AllApnea contributes to
     *   AHI but was never stored, so SQL could not reproduce the app's AHI for
     *   devices that report an undifferentiated apnea.
     * - Purely additive. Pre-v18 rows keep 0 in all five columns until the day is
     *   recalculated or re-imported; no backfill is performed.
     *
     * Version 17: Per-device per-night time corrections (all device types)
     * - Added device_time_corrections table
     * - type: timezone | travel | dst | reset | offset | drift
     *
     * Version 16: One daily summary per profile-day
     * - daily_summaries no longer carries machine_id; the row is a profile-day
     *   rollup that already aggregates across CPAP and oximetry machines.
     * - Dropped machine_id column, its FK to machines, the
     *   idx_daily_summaries_profile_machine index, and changed the natural
     *   key from (profile_id, date, machine_id) to (profile_id, date).
     *
     * Version 15: Remove dead DST field
     * - Dropped dst_enabled column from user_info (stored but never read)
     *
     * Version 14: All file-based settings moved to database
     * - Added app_preferences table (replaces Preferences.xml)
     * - Added graph_layouts unified table (replaces layoutSettings/ named .shg files and
     *   per-profile daily.shg/overview.shg; profile_id NULL = shared named slot)
     * - Added blob_value BLOB column to profile_preferences and app_preferences for
     *   future binary-valued preferences
     *
     * Version 13: Report tree redesign
     * - Replaced reports/report_contents tables with single report_tree table
     * - Hierarchical structure with System/User roots and folders
     * - System reports loaded from external system_reports.orf file
     *
     * Version 12: Profile ID denormalization and schema cleanup
     * - Added profile_id to session_summaries, event_lists, session_settings, session_channels
     * - Added profile_id and channel_id to respiratory_events
     * - Added type field to channels
     * - Removed events_file and summary_file from sessions (no longer needed)
     */
    static const int CURRENT_SCHEMA_VERSION = 19;

    /*!
     * \brief Oldest schema version that can be restored into the current database.
     *
     * Backups with schema_version in [MIN_RESTORE_SCHEMA_VERSION, CURRENT_SCHEMA_VERSION]
     * are accepted.  Tables present in the backup but absent from the current schema are
     * silently skipped during restore; new columns added since the backup was taken receive
     * their DEFAULT values.
     *
     * Update this constant when a schema change is backward-incompatible (e.g. a column
     * that had a DEFAULT loses it, or a table that stores critical data is restructured
     * rather than just extended).  Leave it unchanged for purely additive changes.
     *
     * Version history:
     *   v12 → v13: reports/report_contents removed, report_tree added.  The removed
     *              tables contain only UI configuration that OSCAR regenerates on first run,
     *              so v12 backups restore cleanly into a v13 database.
     */
    static const int MIN_RESTORE_SCHEMA_VERSION = 12;

    /*!
     * \brief Create the complete database schema
     * \param db Database connection to use
     * \return true if successful, false otherwise
     *
     * This method creates all tables, indexes, and inserts the
     * schema version. Safe to call on existing database - uses
     * IF NOT EXISTS for all CREATE statements.
     */
    static bool createSchema(QSqlDatabase& db);

    /*!
     * \brief Get the current schema version from database
     * \param db Database connection to use
     * \return Schema version number, or 0 if not found
     */
    static int getSchemaVersion(QSqlDatabase& db);

    /*!
     * \brief Upgrade database schema to current version
     * \param db Database connection to use
     * \param fromVersion Current version in database
     * \return true if successful, false otherwise
     *
     * Applies incremental migrations until the database is at CURRENT_SCHEMA_VERSION.
     * Each version-specific migration is additive; no existing data is destroyed.
     */
    static bool upgradeSchema(QSqlDatabase& db, int fromVersion);

    /*!
     * \brief Check and update CSV report version
     * \param db Database connection to use
     * \return true if successful, false otherwise
     *
     * Checks if OSCAR version changed and reinitializes system reports if needed.
     * This is called at every startup from DatabaseManager::initialize().
     * Report changes don't require schema changes, so this is separate from upgradeSchema().
     */
    static bool checkAndUpdateReportVersion(QSqlDatabase& db);

private:
    // Schema creation methods
    static bool createSchemaVersionTable(QSqlDatabase& db);
    static bool createProfilesTable(QSqlDatabase& db);
    static bool createMachinesTable(QSqlDatabase& db);
    static bool createUserInfoTable(QSqlDatabase& db);
    static bool createDoctorInfoTable(QSqlDatabase& db);
    static bool createProfilePreferencesTable(QSqlDatabase& db);

    // Session data tables (schema version 3)
    static bool createSessionsTable(QSqlDatabase& db);
    static bool createSessionSettingsTable(QSqlDatabase& db);
    static bool createSessionChannelsTable(QSqlDatabase& db);
    static bool createSessionChannelValuesTable(QSqlDatabase& db);  // Schema version 7
    static bool createRespiratoryEventsTable(QSqlDatabase& db);
    static bool createSessionSummariesTable(QSqlDatabase& db);
    static bool createSessionSlicesTable(QSqlDatabase& db);

    // Channel tables (schema version 5)
    static bool createChannelsTable(QSqlDatabase& db);
    static bool createChannelOptionsTable(QSqlDatabase& db);

    // Daily summaries table (schema version 6)
    static bool createDailySummariesTable(QSqlDatabase& db);

    // Event data tables (schema version 8)
    static bool createEventListsTable(QSqlDatabase& db);
    static bool createEventDataTable(QSqlDatabase& db);

    // Report tree table (schema version 13 - replaces reports/report_contents)
    static bool createReportTreeTable(QSqlDatabase& db);

    // Global preferences table (schema version 14 - replaces Preferences.xml)
    static bool createAppPreferencesTable(QSqlDatabase& db);

    // Unified graph layouts table (schema version 14 - replaces .shg files)
    static bool createGraphLayoutsTable(QSqlDatabase& db);

    // Migration from v13 to v14
    static bool migrateV13ToV14(QSqlDatabase& db);

    // Device time corrections table (schema version 16)
    static bool createDeviceTimeCorrectionsTable(QSqlDatabase& db);

    // Migration from v14 to v15
    static bool migrateV14ToV15(QSqlDatabase& db);

    // Migration from v15 to v16
    static bool migrateV15ToV16(QSqlDatabase& db);

    // Migration from v16 to v17
    static bool migrateV16ToV17(QSqlDatabase& db);

    // Night clock offsets table (schema version 18)
    static bool createNightClockOffsetsTable(QSqlDatabase& db);

    // Central / Obstructive hypopnea summary columns (schema version 18)
    static bool addHypopneaSummaryColumns(QSqlDatabase& db);

    // Migration from v17 to v18
    static bool migrateV17ToV18(QSqlDatabase& db);

    // Migration from v18 to v19
    static bool migrateV18ToV19(QSqlDatabase& db);

    static bool createIndexes(QSqlDatabase& db);
    static bool setSchemaVersion(QSqlDatabase& db, int version);
};

#endif // DATABASE_SCHEMA_H
