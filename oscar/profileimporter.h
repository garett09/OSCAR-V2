/* Profile Importer Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file contains the ProfileImporter class which handles
 * importing profiles from file-based OSCAR (OSCAR_Data) to
 * the new SQL-based OSCAR 2.0 (OSCAR20_Data).
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef PROFILEIMPORTER_H
#define PROFILEIMPORTER_H

#include <QString>
#include <QObject>
#include <QElapsedTimer>

class Profile;
class Machine;
class ProgressDialog;

/*!
 * \class ProfileImporter
 * \brief Manages importing profiles from file-based OSCAR to database-based OSCAR 2.0
 *
 * This class orchestrates the complete import process:
 * 1. Validates source profile
 * 2. Copies profile folder structure
 * 3. Loads session data from .000 and .001 files
 * 4. Migrates metadata (machines.xml, journal, etc.)
 * 5. Calculates daily summaries
 * 6. Handles errors and rollback
 *
 * Usage:
 * \code
 * ProfileImporter importer;
 * connect(&importer, &ProfileImporter::progressChanged, progressDialog, &ProgressDialog::setProgress);
 * bool success = importer.importProfile(sourcePath, newProfileName, progressDialog);
 * \endcode
 */
class ProfileImporter : public QObject
{
    Q_OBJECT
    
public:
    ProfileImporter(QObject *parent = nullptr);
    ~ProfileImporter();
    
    /*!
     * \brief Import a profile from file-based OSCAR to database
     * \param sourcePath Path to source profile folder (e.g., ~/OSCAR_Data/Profiles/User)
     * \param newProfileName Name for the new profile in OSCAR 2.0
     * \param progress Optional progress dialog for user feedback
     * \return true if successful, false otherwise
     *
     * This orchestrates the complete import process and provides progress updates.
     */
    bool importProfile(const QString& sourcePath,
                      const QString& newProfileName,
                      ProgressDialog* progress = nullptr);
    
    /*!
     * \brief Get the last error message
     * \return Error message from last operation, or empty string
     */
    QString lastError() const { return m_lastError; }
    bool wasCancelled() const { return m_cancelled; }

public slots:
    /*! \brief Request cancellation of an in-progress import. Safe to call from a signal. */
    void cancel() { m_cancelled = true; }

signals:
    /*!
     * \brief Emitted when import progress changes
     * \param current Current progress value
     * \param total Maximum progress value
     * \param message Status message describing current operation
     */
    void progressChanged(int current, int total, const QString& message);
    
private:
    QString m_lastError;
    ProgressDialog* m_progress;
    bool m_cancelled;
    int m_totalSessions;
    int m_loadedSessions;
    QString m_lastEventFailFile;    // filename of the first session whose events failed to store
    QString m_lastEventFailDbError; // SQL error from that failure
    QElapsedTimer m_copyTimer;      // throttles progress updates during recursive file copy
    int m_copyFilesDone;            // files copied so far in current backup copy
    int m_copyFilesTotal;           // total files to copy (from pre-scan)
    
    // Phase 1: Copy folder structure
    bool copyProfileStructure(const QString& oldPath, const QString& newPath);
    bool copyJournalFolders(const QString& oldPath, const QString& newPath);
    bool copyDirectoryRecursively(const QString& sourcePath, const QString& destPath);
    int countFilesRecursively(const QString& path);
    
    // Phase 2: Load sessions from files
    bool loadSessionsFromFiles(Profile* profile, const QString& oldPath);
    bool loadMachineSessions(Machine* machine, const QString& oldMachinePath);
    
    // Phase 3: Migrate metadata
    bool migrateMetadata(Profile* profile, const QString& oldPath);
    bool migrateJournalFromSource(Profile* profile, const QString& sourcePath);
    
    // Phase 4: Calculate summaries
    bool calculateSummaries(Profile* profile);
    
    // Validation
    bool validateSourceProfile(const QString& path);
    
    // Rollback on failure
    bool rollbackImport(const QString& profilePath);
    
    // Helper to find machine by folder name
    Machine* findMachineByFolderName(Profile* profile, const QString& folderName);

    // Read Sessions.info (primary) or Summaries.xml (fallback) from a 1.7.1 machine
    // folder and return a map of SessionID → enabled state (true = enabled).
    // Sessions.info is authoritative: 1.7.1 writes it on every exit; Summaries.xml
    // is only updated during SD card imports and can be stale.
    // Returns an empty map if neither file is found (all sessions treated as enabled).
    QHash<quint32, bool> readSummariesEnabledMap(const QString& machinePath);
    
    // Progress reporting helpers
    void reportProgress(int current, int total, const QString& message);

    // Migrate data from OSCAR 1.x data root (shared across all profiles)
    void copyLayoutSettings(const QString& sourceDataPath);
    bool migrateAppSettings(const QString& sourceDataPath);
};

#endif // PROFILEIMPORTER_H
