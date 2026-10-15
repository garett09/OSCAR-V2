/* Database Manager Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file contains the DatabaseManager singleton class which handles
 * SQLite database connections, initialization, and transaction management.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef DATABASE_MANAGER_H
#define DATABASE_MANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QMutex>
#include <QString>

#include "database_schema.h"

/*!
 * \class DatabaseManager
 * \brief Singleton class that manages the SQLite database connection
 *
 * This class provides thread-safe access to the OSCAR SQLite database.
 * It handles connection initialization, transaction management, and
 * provides a single point of access for all database operations.
 *
 * Usage:
 * \code
 * DatabaseManager& dbMgr = DatabaseManager::instance();
 * if (dbMgr.initialize(GetAppData() + "/oscar.db")) {
 *     // Database is ready to use
 * }
 * \endcode
 */
class DatabaseManager : public QObject
{
    Q_OBJECT

public:
    /*!
     * \brief Get the singleton instance of DatabaseManager
     * \return Reference to the DatabaseManager instance
     */
    static DatabaseManager& instance();

    /*!
     * \brief Initialize the database connection
     * \param databasePath Full path to the database file
     * \return true if successful, false otherwise
     *
     * This method:
     * - Creates the database file if it doesn't exist
     * - Opens a connection with WAL mode enabled
     * - Enables foreign key constraints
     * - Creates the schema if needed
     *
     * An existing database whose schema is older than
     * DatabaseSchema::CURRENT_SCHEMA_VERSION is opened but NOT upgraded: this
     * returns true, pendingSchemaUpgradeFrom() reports the on-disk version, and
     * isOpen() stays false until the caller runs upgradeSchema(). The caller is
     * expected to warn the user (the upgrade is irreversible) before doing so.
     */
    bool initialize(const QString& databasePath);

    /*!
     * \brief Schema version awaiting upgrade after initialize()
     * \return The on-disk schema version if it is older than the current one,
     *         otherwise 0 (nothing pending)
     */
    int pendingSchemaUpgradeFrom() const;

    /*!
     * \brief Write a consistent single-file copy of the database
     * \param destPath Full path of the copy to create; must not already exist
     * \param error Receives a human-readable reason on failure
     * \return true if the copy was written
     *
     * Uses VACUUM INTO on a private connection, so the copy includes everything
     * committed to the WAL and needs no -wal/-shm companions. Safe to call from a
     * background thread and while a schema upgrade is pending. Fails fast when the
     * destination volume has less free space than the current database occupies.
     */
    bool snapshotTo(const QString& destPath, QString* error = nullptr);

    /*!
     * \brief Run the pending schema upgrade
     * \param progress Optional per-migration progress callback
     * \return true if the database is now at the current schema version
     *
     * Applies DatabaseSchema::upgradeSchema() on a private connection, so it may
     * run on a background thread while the UI shows progress; it never touches
     * the main connection. On success pendingSchemaUpgradeFrom() becomes 0 and
     * the caller must call completeInitialization() on the thread that called
     * initialize(). On failure the database is left at the last schema version
     * that completed; the caller should close() and exit.
     */
    bool upgradeSchema(const DatabaseSchema::UpgradeProgress& progress = DatabaseSchema::UpgradeProgress());

    /*!
     * \brief Finish initialization after a successful upgradeSchema()
     * \return true if the database is ready for use
     *
     * Runs the startup checks initialize() performs once the schema is current
     * (CSV report versions) and makes isOpen() true. Must be called on the thread
     * that called initialize(). Returns false if an upgrade is still pending.
     */
    bool completeInitialization();

    /*!
     * \brief Close the database connection
     *
     * Call this during application shutdown.
     */
    void close();

    /*!
     * \brief Get the database connection
     * \return QSqlDatabase reference for query execution
     */
    QSqlDatabase database();

    /*!
     * \brief Check if database is open and ready
     * \return true if database is initialized and open
     */
    bool isOpen() const;

    /*!
     * \brief Get the path to the database file
     * \return QString containing full path to database
     */
    QString databasePath() const { return m_databasePath; }

    /*!
     * \brief Begin a database transaction
     * \return true if successful
     */
    bool transaction();

    /*!
     * \brief Commit the current transaction
     * \return true if successful
     */
    bool commit();

    /*!
     * \brief Rollback the current transaction
     * \return true if successful
     */
    bool rollback();

    /*!
     * \brief Check if currently in a transaction
     * \return true if transaction is active
     *
     * This allows nested calls to avoid starting a new transaction
     * when one is already active.
     */
    bool inTransaction() const;

    /*!
     * \brief Get the last database error
     * \return QSqlError object with error details
     */
    QSqlError lastError() const;

    /*!
     * \brief Checkpoint the WAL (Write-Ahead Log) file
     * \return true if successful
     *
     * Forces SQLite to merge the WAL file back into the main database
     * and truncate the WAL. This is useful before/after large operations
     * to improve performance and reclaim disk space.
     *
     * Uses PRAGMA wal_checkpoint(TRUNCATE) which blocks until complete.
     */
    bool checkpointWAL();

    /*!
     * \brief Run a quick integrity check on the database
     * \return true if the database passes the check, false if corruption is detected
     *
     * Runs PRAGMA quick_check, which scans B-tree structure and the free-list
     * without the full cross-reference verification of PRAGMA integrity_check.
     * Suitable for startup checks and pre-VACUUM safety checks.
     */
    bool checkIntegrity();

    /*!
     * \brief Check a failed query for SQLite corruption or I/O errors
     * \param context  Human-readable caller identifier (e.g. "SessionRepository::save")
     * \param query    The query whose lastError() will be inspected
     * \return true if a corruption or I/O error was detected
     *
     * If the error's primary SQLite code is SQLITE_CORRUPT (11) or SQLITE_IOERR (10),
     * emits databaseError() with recovery guidance. Only emits once per session to
     * avoid stacking multiple dialogs when a corruption causes a cascade of failures.
     */
    bool checkQueryError(const QString& context, const QSqlQuery& query);

    /*!
     * \brief Write the clean-shutdown flag for the given database path to QSettings
     * \param dbPath Full path to the database file
     *
     * Called both from main() after a normal close and from switchToDatabase()
     * before spawning a new OSCAR process, to ensure the flag is on disk before
     * the new process reads it. Centralises the QSettings key names so they
     * cannot diverge between callers.
     */
    static void markCleanShutdown(const QString& dbPath);

    /*!
     * \brief Counts total rows in database
     * \return total number of rows. -1 if failure.
     */
    long countRows(QString text);

signals:
    /*!
     * \brief Emitted when a database error occurs
     * \param error Description of the error
     */
    void databaseError(const QString& error);

private:
    // Singleton pattern - private constructor
    DatabaseManager();
    ~DatabaseManager();
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    /*!
     * \brief Configure per-connection settings on any open connection
     * \param db The connection to configure
     *
     * Sets up WAL mode, foreign keys, cache size, etc. Used for the main
     * connection and for the private connection upgradeSchema() opens.
     */
    static bool configureConnection(QSqlDatabase& db);

    /*!
     * \brief Common tail of initialize() and completeInitialization()
     *
     * Runs the CSV report-version check and marks the manager initialized.
     * Caller must hold m_mutex.
     */
    bool finishInitialization();

    QSqlDatabase m_database;
    QString m_connectionName;
    QString m_databasePath;
    bool m_initialized;
    int m_pendingUpgradeFrom;
    bool m_inTransaction;
    bool m_corruptionReported;
    mutable QMutex m_mutex;
};

#endif // DATABASE_MANAGER_H
