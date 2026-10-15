/* Database Manager Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file implements the DatabaseManager singleton class for SQLite
 * database connection management and transaction handling.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "database_manager.h"
#include "database_schema.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QThread>
#include <QMutexLocker>
#include <QCoreApplication>
#include <QSettings>
#include <QFile>
#include <QStorageInfo>

//#define DBDEBUG
/*
 * Get the singleton instance of DatabaseManager
 *
 * Returns: Reference to the single DatabaseManager instance
 */
DatabaseManager& DatabaseManager::instance()
{
    static DatabaseManager instance;
    return instance;
}

/*
 * Private constructor for singleton pattern
 */
DatabaseManager::DatabaseManager()
    : QObject(nullptr)
    , m_initialized(false)
    , m_pendingUpgradeFrom(0)
    , m_inTransaction(false)
    , m_corruptionReported(false)
{
    // Create unique connection name for this thread
    m_connectionName = QString("OSCAR_DB_%1").arg(quintptr(QThread::currentThread()));
}

/*
 * Destructor - closes database connection
 */
DatabaseManager::~DatabaseManager()
{
    close();
}

/*
 * Initialize the database connection
 *
 * Parameters:
 *   databasePath - Full path to the database file
 *
 * Returns: true if successful, false otherwise
 *
 * This method creates the database file if needed, opens a connection,
 * configures settings, and creates the schema if this is a new database.
 *
 * An existing database with an older schema is opened but left as-is:
 * m_pendingUpgradeFrom records its version, m_initialized stays false, and the
 * caller decides (after warning the user) whether to run upgradeSchema() or
 * close() and exit. The upgrade is irreversible, so it must not happen silently.
 */
bool DatabaseManager::initialize(const QString& databasePath)
{
    QMutexLocker locker(&m_mutex);

    if (m_initialized) {
        qDebug() << "DatabaseManager::initialize: Already initialized";
        return true;
    }

    m_databasePath = databasePath;

    // Ensure the directory exists
    QFileInfo fileInfo(databasePath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            qCritical() << "DatabaseManager::initialize: Failed to create directory:" << dir.absolutePath();
            emit databaseError("Failed to create database directory");
            return false;
        }
    }

    // Check if database file already exists
    bool isNewDatabase = !QFile::exists(databasePath);

    // Add database connection
    m_database = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    m_database.setDatabaseName(databasePath);

    // Open the database
    if (!m_database.open()) {
        qCritical() << "DatabaseManager::initialize: Failed to open database:" << m_database.lastError().text();
        emit databaseError("Failed to open database: " + m_database.lastError().text());
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        return false;
    }

    qDebug() << "DatabaseManager::initialize: Database opened:" << databasePath;

    // Configure database settings
    if (!configureConnection(m_database)) {
        qCritical() << "DatabaseManager::initialize: Failed to configure database settings";
        close();
        return false;
    }

    // Create schema if this is a new database
    if (isNewDatabase) {
        qDebug() << "DatabaseManager::initialize: Creating new database schema";
        if (!DatabaseSchema::createSchema(m_database)) {
            qCritical() << "DatabaseManager::initialize: Failed to create database schema";
            close();
            return false;
        }
    } else {
        qDebug() << "DatabaseManager::initialize: Using existing database";
        
        // Check schema version (Schema v12+ policy: no incremental migration)
        int currentVersion = DatabaseSchema::getSchemaVersion(m_database);
        qDebug() << "DatabaseManager::initialize: Current schema version:" << currentVersion;
        
        if (currentVersion < DatabaseSchema::CURRENT_SCHEMA_VERSION) {
            // Leave the upgrade to the caller: it is irreversible and the user
            // must be told first. See upgradeSchema() / completeInitialization().
            qDebug() << "DatabaseManager::initialize: Schema needs upgrade from" << currentVersion
                     << "to" << DatabaseSchema::CURRENT_SCHEMA_VERSION << "- awaiting confirmation";
            m_pendingUpgradeFrom = currentVersion;
            return true;
        } else if (currentVersion > DatabaseSchema::CURRENT_SCHEMA_VERSION) {
            qCritical() << "DatabaseManager::initialize: DB version" << currentVersion
                        << "is newer than this build's schema" << DatabaseSchema::CURRENT_SCHEMA_VERSION;
            emit databaseError(QString(
                "Database Version Too New\n\n"
                "Your database is version %1, but this version of OSCAR only supports up to version %2.\n\n"
                "Please update OSCAR to a newer version.\n\n"
                "OSCAR will now close.")
                .arg(currentVersion)
                .arg(DatabaseSchema::CURRENT_SCHEMA_VERSION));
            close();
            return false;
        }

        qDebug() << "DatabaseManager: Schema version is correct";
    }

    return finishInitialization();
}

/*
 * Schema version awaiting upgrade after initialize()
 *
 * Returns: the on-disk schema version when it is older than
 * DatabaseSchema::CURRENT_SCHEMA_VERSION, otherwise 0.
 */
int DatabaseManager::pendingSchemaUpgradeFrom() const
{
    QMutexLocker locker(&m_mutex);
    return m_pendingUpgradeFrom;
}

/*
 * Write a consistent single-file copy of the database
 *
 * Parameters:
 *   destPath - Full path of the copy to create; must not already exist
 *   error - Receives a human-readable reason on failure (may be nullptr)
 *
 * Returns: true if the copy was written
 *
 * VACUUM INTO writes a fresh, compacted database containing every committed
 * transaction, including those still in the WAL, so the copy is a single file
 * a user can restore by renaming. It runs on a private connection (Qt SQL
 * connections are per-thread) so it can be called from a worker thread and does
 * not need m_initialized — the caller in main() uses it before a schema upgrade.
 *
 * Free space is checked first against the current database size: VACUUM INTO on
 * a multi-GB database otherwise runs for minutes before failing with a disk-full
 * error.
 */
bool DatabaseManager::snapshotTo(const QString& destPath, QString* error)
{
    auto fail = [&](const QString& reason) {
        qWarning() << "DatabaseManager::snapshotTo:" << reason;
        if (error) *error = reason;
        return false;
    };

    if (m_databasePath.isEmpty()) {
        return fail(tr("No database is open."));
    }
    if (QFile::exists(destPath)) {
        return fail(tr("The file %1 already exists.").arg(QDir::toNativeSeparators(destPath)));
    }

    const qint64 needed = QFileInfo(m_databasePath).size() + QFileInfo(m_databasePath + "-wal").size();
    const QStorageInfo volume(QFileInfo(destPath).absolutePath());
    if (volume.isValid() && volume.bytesAvailable() < needed) {
        return fail(tr("Not enough free space: the copy needs about %1 MB but only %2 MB is available.")
                        .arg(needed / (1024 * 1024))
                        .arg(volume.bytesAvailable() / (1024 * 1024)));
    }

    const QString connName = QString("OSCAR_SNAPSHOT_%1").arg(quintptr(QThread::currentThread()));
    bool ok = false;
    QString reason;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(m_databasePath);
        if (!db.open()) {
            reason = db.lastError().text();
        } else {
            QSqlQuery query(db);
            QString safePath = destPath;
            safePath.replace("'", "''");
            ok = query.exec(QString("VACUUM INTO '%1'").arg(safePath));
            if (!ok) {
                reason = query.lastError().text();
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);

    if (!ok) {
        QFile::remove(destPath);  // never leave a partial copy behind
        return fail(reason);
    }
    qDebug() << "DatabaseManager::snapshotTo: wrote" << destPath;
    return true;
}

/*
 * Run the pending schema upgrade
 *
 * Parameters:
 *   progress - Optional callback invoked before each migration step
 *
 * Returns: true if every migration succeeded
 *
 * Migrations run on a private connection so this can execute on a worker
 * thread while the UI shows progress; m_database belongs to the thread that
 * called initialize() and is left untouched. SQLite notices the schema change
 * on the main connection's next statement, so nothing needs re-opening. The
 * private connection gets the same per-connection settings as the main one
 * (foreign_keys in particular, which some migrations rely on).
 *
 * m_mutex is not held: the migrations call checkQueryError() on this object,
 * and they can run for minutes.
 */
bool DatabaseManager::upgradeSchema(const DatabaseSchema::UpgradeProgress& progress)
{
    const int fromVersion = pendingSchemaUpgradeFrom();
    if (fromVersion == 0) {
        qWarning() << "DatabaseManager::upgradeSchema: no upgrade pending";
        return false;
    }

    const QString connName = QString("OSCAR_UPGRADE_%1").arg(quintptr(QThread::currentThread()));
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(m_databasePath);
        if (!db.open()) {
            qCritical() << "DatabaseManager::upgradeSchema: Cannot open private connection:"
                        << db.lastError().text();
        } else if (!configureConnection(db)) {
            qCritical() << "DatabaseManager::upgradeSchema: Failed to configure private connection";
            db.close();
        } else {
            ok = DatabaseSchema::upgradeSchema(db, fromVersion, progress);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);

    if (!ok) {
        qCritical() << "DatabaseManager::upgradeSchema: Schema upgrade failed";
        return false;
    }

    QMutexLocker locker(&m_mutex);
    m_pendingUpgradeFrom = 0;
    qDebug() << "DatabaseManager::upgradeSchema: Schema upgrade successful";
    return true;
}

/*
 * Finish initialization after a successful upgradeSchema()
 *
 * Returns: true if the database is ready for use
 *
 * Must run on the thread that called initialize(), because it uses m_database.
 */
bool DatabaseManager::completeInitialization()
{
    if (pendingSchemaUpgradeFrom() != 0) {
        qWarning() << "DatabaseManager::completeInitialization: schema upgrade still pending";
        return false;
    }
    QMutexLocker locker(&m_mutex);
    return finishInitialization();
}

/*
 * Common tail of initialize() and completeInitialization()
 *
 * Returns: true (the report-version check is not fatal)
 *
 * Caller holds m_mutex.
 */
bool DatabaseManager::finishInitialization()
{
    // Check and update CSV report versions (runs at every startup)
    // This is separate from schema upgrades because report changes don't require schema changes
    qDebug() << "DatabaseManager::initialize: Checking CSV report versions...";
    if (!DatabaseSchema::checkAndUpdateReportVersion(m_database)) {
        qWarning() << "DatabaseManager::initialize: Failed to check/update report versions";
        // Not a critical error - continue initialization
    }

    m_initialized = true;
    qDebug() << "DatabaseManager::initialize: Initialization complete";
    return true;
}

/*
 * Close the database connection
 *
 * Call this during application shutdown to properly close the database.
 * Also used when a pending schema upgrade is declined, so the guard is on the
 * connection being open rather than on m_initialized.
 */
void DatabaseManager::close()
{
    QMutexLocker locker(&m_mutex);

    if (m_database.isOpen()) {
        // Update query-planner statistics before closing. PRAGMA optimize is
        // lightweight (analyses only tables with stale stats) and takes effect
        // on the next open, improving query planning over time.
        QSqlQuery query(m_database);
        if (!query.exec("PRAGMA optimize")) {
            qWarning() << "DatabaseManager::close: PRAGMA optimize failed:" << query.lastError().text();
        } else {
            qDebug() << "DatabaseManager::close: PRAGMA optimize complete";
        }

        // Merge all WAL frames into the main file and truncate the WAL so that
        // only oscar.db needs to be synced by cloud-storage tools (e.g. Dropbox).
        if (!query.exec("PRAGMA wal_checkpoint(TRUNCATE)")) {
            qWarning() << "DatabaseManager::close: WAL checkpoint failed:" << query.lastError().text();
        } else {
            qDebug() << "DatabaseManager::close: WAL checkpoint complete";
        }

        qDebug() << "DatabaseManager::close: Closing database connection";
        m_database.close();
    }

    // Only remove database if QCoreApplication still exists
    // to avoid Qt warnings during shutdown
    if (QCoreApplication::instance()) {
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
    }
    
    m_initialized = false;
    m_pendingUpgradeFrom = 0;
}

/*
 * Get the database connection for executing queries
 *
 * Returns: QSqlDatabase object for query execution
 */
QSqlDatabase DatabaseManager::database()
{
    return m_database;
}

/*
 * Check if database is open and ready
 *
 * Returns: true if database is initialized and open, false otherwise
 */
bool DatabaseManager::isOpen() const
{
    QMutexLocker locker(&m_mutex);
    return m_initialized && m_database.isOpen();
}

/*
 * Begin a database transaction
 *
 * Returns: true if successful, false otherwise
 *
 * All database modifications between transaction() and commit() will
 * be atomic. If an error occurs, call rollback() to undo changes.
 * 
 * If already in a transaction, returns true without starting a new one.
 */
bool DatabaseManager::transaction()
{
    QMutexLocker locker(&m_mutex);

    if (m_inTransaction) {
        // Already in a transaction, don't nest
        return true;
    }
    
    if (!m_database.transaction()) {
        qWarning() << "DatabaseManager::transaction(): Failed to start transaction:" << m_database.lastError().text();
        return false;
    }
    
    qDebug() << "DatabaseManager::transaction(): started transaction";
//    countRows ("transaction starting");
    m_inTransaction = true;
    return true;
}
long DatabaseManager::countRows(QString text) {
    long totalRows = 0;
#ifdef DBDEBUG
    QSqlQuery query(m_database);

    if (!query.exec("SELECT name FROM sqlite_master WHERE type='table'")) {
        qDebug() << "DatabaseManager::countRows: Error querying tables:" << query.lastError().text();
        return 0;
    }

    while (query.next()) {
        QString tableName = query.value(0).toString();

        QSqlQuery countQuery(m_database);
        QString sql = QString("SELECT COUNT(*) FROM %1").arg(tableName);

        if (!countQuery.exec(sql)) {
            qDebug() << "DatabaseManager::countRows: Error counting rows in" << tableName << ":"
                     << countQuery.lastError().text();
            continue;
        }

        if (countQuery.next()) {
            int rowCount = countQuery.value(0).toInt();
            totalRows += rowCount;
            qDebug().noquote() << "DatabaseManager::countRows" << text
                               << "for table" << tableName << ":" << rowCount << "rows";
        }
    }
#else
    Q_UNUSED(text)
#endif
    return totalRows;
}

/*
 * Commit the current transaction
 *
 * Returns: true if successful, false otherwise
 *
 * Makes all changes since transaction() permanent.
 */
bool DatabaseManager::commit()
{
    QMutexLocker locker(&m_mutex);
    
    if (!m_inTransaction) {
        // Not in a transaction, nothing to commit
        return true;
    }
    
    countRows ("before commit");

    if (!m_database.commit()) {
        qWarning() << "DatabaseManager::commit: Failed to commit transaction:" << m_database.lastError().text();
        m_inTransaction = false;  // Clear flag even on error
        return false;
    }
    
    qDebug() << "DatabaseManager::commit: committed transaction";
    countRows ("after commit");
    m_inTransaction = false;
    return true;
}

/*
 * Rollback the current transaction
 *
 * Returns: true if successful, false otherwise
 *
 * Undoes all changes since transaction() was called.
 */
bool DatabaseManager::rollback()
{
    QMutexLocker locker(&m_mutex);
    
    if (!m_inTransaction) {
        // Not in a transaction, nothing to rollback
        return true;
    }
    
    if (!m_database.rollback()) {
        qWarning() << "DatabaseManager::rollback: Failed to rollback transaction:" << m_database.lastError().text();
        m_inTransaction = false;  // Clear flag even on error
        return false;
    }
    
    qDebug() << "DatabaseManager::rollback: rolled back transaction";
    m_inTransaction = false;
    return true;
}

/*
 * Check if currently in a transaction
 *
 * Returns: true if transaction is active, false otherwise
 *
 * This allows repositories to avoid starting nested transactions.
 */
bool DatabaseManager::inTransaction() const
{
    return m_inTransaction;
}

/*
 * Check a failed query for SQLite corruption or I/O errors.
 *
 * SQLite primary error codes are in the low byte of the native code, so both
 * primary codes (e.g. 10, 11) and extended codes (e.g. 266, 267) are handled
 * by masking with 0xFF.
 *
 *   SQLITE_IOERR   = 10  — disk read/write failure
 *   SQLITE_CORRUPT = 11  — database file is malformed
 *
 * Emits databaseError() at most once per session so a corruption cascade does
 * not stack multiple dialogs.
 *
 * Returns: true if a corruption or I/O error was detected.
 */
bool DatabaseManager::checkQueryError(const QString& context, const QSqlQuery& query)
{
    QSqlError err = query.lastError();
    if (!err.isValid())
        return false;

    bool ok;
    int code    = err.nativeErrorCode().toInt(&ok);
    int primary = ok ? (code & 0xFF) : -1;

    if (primary != 10 && primary != 11)
        return false;

    qCritical() << "DatabaseManager::checkQueryError: corruption/IO error in"
                << context << "— native code:" << err.nativeErrorCode()
                << "—" << err.text();

    if (!m_corruptionReported) {
        m_corruptionReported = true;
        emit databaseError(
            tr("A database corruption or I/O error was detected in %1.\n\n"
               "Error: %2\n\n"
               "Recommended actions:\n"
               "  • Run Help → Troubleshooting → Check Database Integrity\n"
               "  • Restore from a recent backup if problems are found")
                .arg(context, err.text()));
    }
    return true;
}

/*
 * Write the clean-shutdown flag for dbPath to QSettings and sync to disk.
 *
 * This is a static method so it can be called from switchToDatabase() before
 * spawning a new OSCAR process — the new process reads these keys at startup,
 * so they must be on disk before startDetached() returns.
 *
 * Also called from main() as part of normal shutdown.
 */
void DatabaseManager::markCleanShutdown(const QString& dbPath)
{
    QSettings settings;
    settings.setValue("db/CleanShutdown", true);
    settings.setValue("db/LastShutdownPath", dbPath);
    settings.sync();
    qDebug() << "DatabaseManager::markCleanShutdown: clean shutdown marked for" << dbPath;
}

/*
 * Run a quick integrity check on the database
 *
 * Returns: true if the database passes PRAGMA quick_check, false if corruption
 * is detected or the check cannot be run.
 *
 * PRAGMA quick_check scans the B-tree structure and free-list of every page.
 * It returns a single "ok" row when clean, or one row per problem found.
 * It is faster than PRAGMA integrity_check (which also cross-checks indexes)
 * and suitable for startup checks and pre-VACUUM safety guards.
 *
 * This method opens a temporary, private SQLite connection so it is safe to
 * call from any thread — Qt SQL connections are per-thread and m_database
 * belongs to the thread that called initialize(). The caller is responsible
 * for running this on a background thread so the UI stays responsive (a check
 * on a large database can take several minutes).
 */
bool DatabaseManager::checkIntegrity()
{
    if (m_databasePath.isEmpty()) {
        qWarning() << "DatabaseManager::checkIntegrity: No database path set";
        return false;
    }

    // Unique connection name per thread so concurrent calls don't collide.
    QString connName = QString("OSCAR_INTEGRITY_%1")
                           .arg(quintptr(QThread::currentThread()));
    bool passed = false;

    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(m_databasePath);

        if (!db.open()) {
            qWarning() << "DatabaseManager::checkIntegrity: Cannot open temporary connection:"
                       << db.lastError().text();
        } else {
            qDebug() << "DatabaseManager::checkIntegrity: Running PRAGMA quick_check...";
            QSqlQuery query(db);

            if (!query.exec("PRAGMA quick_check")) {
                qWarning() << "DatabaseManager::checkIntegrity: PRAGMA quick_check failed:"
                           << query.lastError().text();
            } else {
                QStringList issues;
                while (query.next()) {
                    QString val = query.value(0).toString();
                    if (val != "ok")
                        issues << val;
                }

                if (issues.isEmpty()) {
                    qDebug() << "DatabaseManager::checkIntegrity: Database passed quick_check";
                    passed = true;
                } else {
                    qWarning() << "DatabaseManager::checkIntegrity: Database failed quick_check with"
                               << issues.size() << "issue(s):";
                    for (const QString& issue : issues)
                        qWarning() << "  -" << issue;
                }
            }
            db.close();
        }
    }

    QSqlDatabase::removeDatabase(connName);
    return passed;
}

/*
 * Get the last database error
 *
 * Returns: QSqlError object with error details
 */
QSqlError DatabaseManager::lastError() const
{
    return m_database.lastError();
}

/*
 * Checkpoint the WAL (Write-Ahead Log) file
 *
 * Returns: true if successful, false otherwise
 *
 * This method forces SQLite to merge the WAL file back into the main
 * database and truncate the WAL. This is particularly useful before
 * and after large delete operations to:
 * - Start with a smaller WAL (faster processing)
 * - Immediately reclaim disk space after deletion
 * - Reduce memory pressure
 */
bool DatabaseManager::checkpointWAL()
{
    QMutexLocker locker(&m_mutex);
    
    if (!m_initialized || !m_database.isOpen()) {
        qWarning() << "DatabaseManager::checkpointWAL() - Database not initialized";
        return false;
    }
    
    QSqlQuery query(m_database);
    
    qDebug() << "DatabaseManager: Checkpointing WAL...";
    
    // PRAGMA wal_checkpoint(TRUNCATE) forces WAL to merge and truncate
    // This blocks until complete, ensuring all changes are committed
    if (!query.exec("PRAGMA wal_checkpoint(TRUNCATE)")) {
        qWarning() << "DatabaseManager: WAL checkpoint failed:" << query.lastError().text();
        return false;
    }
    
    // Query returns three values: (busy, log_frames, checkpointed_frames)
    // busy: 0 if successful, 1 if blocked
    // log_frames: Number of frames in WAL after checkpoint
    // checkpointed_frames: Number of frames checkpointed
    if (query.next()) {
        int busy = query.value(0).toInt();
        int logFrames = query.value(1).toInt();
        int checkpointedFrames = query.value(2).toInt();
        
        if (busy == 0) {
            qDebug() << "DatabaseManager: WAL checkpoint complete -" 
                     << checkpointedFrames << "frames checkpointed,"
                     << logFrames << "frames remain in WAL";
        } else {
            qWarning() << "DatabaseManager: WAL checkpoint blocked by another process";
            return false;
        }
    }
    
    return true;
}

/*
 * Configure per-connection settings
 *
 * Parameters:
 *   db - An open connection (the main one, or a private one for upgrades)
 *
 * Returns: true if successful, false otherwise
 *
 * This method enables:
 * - Foreign key constraints
 * - WAL (Write-Ahead Logging) mode for better concurrency
 * - Optimized cache size
 * - Faster synchronous mode
 */
bool DatabaseManager::configureConnection(QSqlDatabase& db)
{
    QSqlQuery query(db);

    // Enable foreign key constraints
    if (!query.exec("PRAGMA foreign_keys = ON")) {
        qWarning() << "DatabaseManager: Failed to enable foreign keys:" << query.lastError().text();
        return false;
    }

    // Enable WAL mode; pragma returns the resulting mode as a row, not an error code.
    if (!query.exec("PRAGMA journal_mode = WAL")) {
        qWarning() << "DatabaseManager: Failed to enable WAL mode:" << query.lastError().text();
    } else if (query.next() && query.value(0).toString() != "wal") {
        qWarning() << "DatabaseManager: WAL mode not active (journal_mode ="
                   << query.value(0).toString() << ") — possibly on a network filesystem";
    }

    // Set synchronous mode to NORMAL for better performance
    if (!query.exec("PRAGMA synchronous = NORMAL")) {
        qWarning() << "DatabaseManager: Failed to set synchronous mode:" << query.lastError().text();
        // Not critical, continue anyway
    }

    // Set cache size to 64MB for better performance
    if (!query.exec("PRAGMA cache_size = -64000")) {
        qWarning() << "DatabaseManager: Failed to set cache size:" << query.lastError().text();
        // Not critical, continue anyway
    }

    // Store temp tables in memory for better performance
    if (!query.exec("PRAGMA temp_store = MEMORY")) {
        qWarning() << "DatabaseManager: Failed to set temp_store:" << query.lastError().text();
        // Not critical, continue anyway
    }

    qDebug() << "DatabaseManager: Database settings configured";
    return true;
}
