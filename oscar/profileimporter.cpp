/* Profile Importer Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

//#define DBDEBUG

#include "profileimporter.h"
#include "SleepLib/profiles.h"
#include "SleepLib/machine.h"
#include "SleepLib/session.h"
#include "SleepLib/journal.h"
#include "SleepLib/progressdialog.h"
#include "database/migration_manager.h"
#include "database/database_manager.h"
#include "database/preferences_repository.h"
#include "database/profile_repository.h"
#include "SleepLib/appsettings.h"
#include "SleepLib/common.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QDomDocument>
#include <QDebug>
#include <QApplication>

ProfileImporter::ProfileImporter(QObject *parent)
    : QObject(parent)
    , m_progress(nullptr)
    , m_cancelled(false)
    , m_totalSessions(0)
    , m_loadedSessions(0)
    , m_copyFilesDone(0)
    , m_copyFilesTotal(0)
{
}

ProfileImporter::~ProfileImporter()
{
}

bool ProfileImporter::importProfile(const QString& sourcePath,
                                    const QString& newProfileName,
                                    ProgressDialog* progress)
{
    m_progress = progress;
    m_lastError.clear();
    m_cancelled = false;
    m_totalSessions = 0;
    m_loadedSessions = 0;
    m_lastEventFailFile.clear();
    m_lastEventFailDbError.clear();
    
    reportProgress(0, 100, tr("Validating source profile..."));
    
    if (!validateSourceProfile(sourcePath)) {
        m_lastError = tr("Invalid source profile: %1").arg(m_lastError);
        return false;
    }
    
    // ===== BEGIN SINGLE TRANSACTION FOR ENTIRE IMPORT =====
    // Wrap the entire import process in one transaction for maximum performance
    // If anything fails, everything rolls back cleanly
    DatabaseManager& dbMgr = DatabaseManager::instance();
    if (!dbMgr.transaction()) {
        m_lastError = tr("Failed to begin database transaction: %1").arg(dbMgr.lastError().text());
        return false;
    }
    
    qDebug() << "ProfileImporter: Started database transaction for entire import";
    
    // Create new profile folder
    QString newPath = GetAppData() + "/Profiles/" + newProfileName;
    QDir().mkpath(newPath);
    
    reportProgress(10, 100, tr("Copying profile structure..."));
    
    if (!copyProfileStructure(sourcePath, newPath)) {
        dbMgr.rollback();
        qWarning() << "ProfileImporter: Rolled back transaction due to copyProfileStructure failure";
        rollbackImport(newPath);
        return false;
    }
    
    // Copy machines.xml BEFORE creating Profile so MigrationManager can find it
    QString sourceMachinesXml = sourcePath + "/machines.xml";
    QString destMachinesXml = newPath + "/machines.xml";
    if (!QFile::copy(sourceMachinesXml, destMachinesXml)) {
        m_lastError = tr("Failed to copy machines.xml");
        dbMgr.rollback();
        qWarning() << "ProfileImporter: Rolled back transaction due to machines.xml copy failure";
        rollbackImport(newPath);
        return false;
    }
    
    reportProgress(20, 100, tr("Creating profile in database..."));
    
    // Create profile object - this initializes the database
    Profile* profile = new Profile(newPath, false);
    
    // Set username first, before migration
    profile->user->setUserName(newProfileName);
    
    reportProgress(25, 100, tr("Migrating profile metadata..."));

    // Initialize schema before migrateMetadata (uses channel IDs like Journal_Notes)
    // and before loadSessionsFromFiles (uses schema::channel[] for event parsing).
    // During startup migration, main.cpp's schema::init() has not yet been called.
    schema::init();

    if (!migrateMetadata(profile, sourcePath)) {
        dbMgr.rollback();
        qWarning() << "ProfileImporter: Rolled back transaction due to migrateMetadata failure";
        rollbackImport(newPath);
        delete profile;
        return false;
    }
    
    reportProgress(30, 100, tr("Copying user information..."));
    
    // IMPORTANT: Copy user data AFTER migration to prevent MigrationManager from overwriting it
    // Load source profile to copy user data
    Profile* sourceProfile = new Profile(sourcePath, true);
    if (sourceProfile && sourceProfile->isOpen()) {
//        qDebug() << "ProfileImporter: Source profile opened successfully";
//        qDebug() << "ProfileImporter: Source user firstname:" << sourceProfile->user->firstName();
//        qDebug() << "ProfileImporter: Source user lastname:" << sourceProfile->user->lastName();
//        qDebug() << "ProfileImporter: Source doctor name:" << sourceProfile->doctor->name();
        
        // Copy user info (keep the new username we already set)
        profile->user->setDOB(sourceProfile->user->DOB());
        profile->user->setHeight(sourceProfile->user->height());
        profile->user->setFirstName(sourceProfile->user->firstName());
        profile->user->setLastName(sourceProfile->user->lastName());
        profile->user->setAddress(sourceProfile->user->address());
        profile->user->setPhone(sourceProfile->user->phone());
        profile->user->setEmail(sourceProfile->user->email());
        profile->user->setGender(sourceProfile->user->gender());
        profile->user->setCountry(sourceProfile->user->country());
        // Copy doctor info
        profile->doctor->setName(sourceProfile->doctor->name());
        profile->doctor->setPracticeName(sourceProfile->doctor->practiceName());
        profile->doctor->setAddress(sourceProfile->doctor->address());
        profile->doctor->setPhone(sourceProfile->doctor->phone());
        profile->doctor->setEmail(sourceProfile->doctor->email());
        profile->doctor->setPatientID(sourceProfile->doctor->patientID());
        // Copy preferences (cpap, oxi, session, appearance, general) from source profile.
        // migrateExtendedData() runs on the freshly-created destination profile (which holds
        // only defaults), so we must explicitly overwrite with the source values here.
        ProfileRepository profileRepo;
        ProfileData profileData = profileRepo.findByUsername(newProfileName);
        if (profileData.id > 0) {
            PreferencesRepository prefRepo;
            if (!prefRepo.saveAllPreferences(profileData.id,
                                             sourceProfile->cpap,
                                             sourceProfile->oxi,
                                             sourceProfile->session,
                                             sourceProfile->appearance,
                                             sourceProfile->general)) {
                qWarning() << "ProfileImporter: Failed to save source preferences to database";
            }
        } else {
            qWarning() << "ProfileImporter: Could not find destination profile in database to save preferences";
        }

        // Copy source profile's raw preference hash into the destination profile.
        // profile->Save() calls saveProfilePreferencesToDatabase() which persists p_preferences
        // to the "profile" DB category. That category is authoritative over "session"/"cpap"/etc.
        // per loadExtendedDataFromDatabase(). Without this copy, the new-profile defaults
        // written by Save() would overwrite the source settings on next open.
        static const QSet<QString> skipKeys = {
            STR_GEN_DataFolder, STR_UI_UserName, STR_PREF_VersionString
        };
        for (auto it = sourceProfile->p_preferences.begin();
             it != sourceProfile->p_preferences.end(); ++it) {
            if (!skipKeys.contains(it.key())) {
                profile->p_preferences[it.key()] = it.value();
            }
        }

        // Note: We'll save user/doctor info in Profile::Save() below
        // Don't save here because profile might not be in database yet
    } else {
        qWarning() << "ProfileImporter: Failed to open source profile or profile not open";
    }
    delete sourceProfile;

    // Migrate app-level data that lives in the OSCAR 1.x data root
    // (two levels up from the profile folder: OSCAR_Data/Profiles/ProfileName → OSCAR_Data)
    QString sourceDataPath = QDir::cleanPath(QDir(sourcePath).absolutePath() + "/../..");
    copyLayoutSettings(sourceDataPath);
    if (!migrateAppSettings(sourceDataPath)) {
        qWarning() << "ProfileImporter: App settings migration failed; continuing import";
    }

    // ===== COMMIT TRANSACTION 1: profile metadata (small, fast) =====
    // Profile record, machines, user/doctor info, and preferences are now committed.
    // Session data is loaded separately so each machine's commit is small and the
    // UI stays responsive throughout.
    //
    // Look up the profile's DB id NOW, before the commit, so we can clean up via
    // cascade delete if the commit fails. (The profile row is visible to the same
    // connection inside the uncommitted transaction.)
    ProfileRepository profileRepo2;
    qint64 profileId = profileRepo2.findByUsername(newProfileName).id;

    reportProgress(35, 100, tr("Saving profile metadata..."));
    if (!dbMgr.commit()) {
        m_lastError = tr("Failed to commit profile metadata: %1").arg(dbMgr.lastError().text());
        qWarning() << "ProfileImporter: Failed to commit metadata transaction";
        // Belt-and-braces: remove any metadata that may have been committed outside
        // DatabaseManager's transaction tracking (guard against future regressions).
        if (profileId > 0) {
            ProfileRepository cleanupRepo;
            cleanupRepo.remove(profileId);
        }
        rollbackImport(newPath);
        delete profile;
        return false;
    }

    reportProgress(40, 100, tr("Loading session data from files..."));

    // IMPORTANT: Set p_profile before loading sessions - Session objects need it.
    // NOTE: p_profile is a global that is temporarily replaced here. reportProgress()
    // calls QApplication::processEvents(), which can dispatch Qt events while the swap
    // is active. This is safe in practice because the import dialog is modal (blocking
    // UI interaction with the main window) and no known timer touches p_profile during
    // import. If future timers or background tasks are added that read p_profile, this
    // swap should be eliminated and progress updates driven via queued signals instead.
    extern Profile* p_profile;
    Profile* savedProfile = p_profile;
    p_profile = profile;

    // loadSessionsFromFiles commits one small transaction per machine so that each
    // individual commit covers only one machine's session data rather than the entire
    // import, keeping the UI responsive.
    bool sessionsLoaded = loadSessionsFromFiles(profile, sourcePath);

    if (!sessionsLoaded) {
        p_profile = savedProfile;
        qWarning() << "ProfileImporter: Session loading failed; cleaning up committed metadata";
        if (profileId > 0) {
            ProfileRepository cleanupRepo;
            cleanupRepo.remove(profileId);
        }
        rollbackImport(newPath);
        delete profile;
        return false;
    }

    // ===== TRANSACTION 2: summaries + profile save (small, fast) =====
    if (!dbMgr.transaction()) {
        m_lastError = tr("Failed to begin transaction for summaries: %1").arg(dbMgr.lastError().text());
        p_profile = savedProfile;
        if (profileId > 0) {
            ProfileRepository cleanupRepo;
            cleanupRepo.remove(profileId);
        }
        rollbackImport(newPath);
        delete profile;
        return false;
    }

    reportProgress(85, 100, tr("Calculating daily summaries..."));

    if (!calculateSummaries(profile)) {
        // Don't fail for this - summaries can be regenerated
        qWarning() << "Failed to calculate summaries, but import succeeded";
    }

    reportProgress(92, 100, tr("Saving profile data..."));

    // IMPORTANT: Set opened state to true so Profile::Save() doesn't return early
    profile->setOpened(true);

    // Save profile (still needs p_profile set)
    if (!profile->Save()) {
        m_lastError = tr("Failed to save profile to database");
        qWarning() << "ProfileImporter: Rolled back summaries transaction due to profile Save() failure";
        dbMgr.rollback();
        p_profile = savedProfile;
        if (profileId > 0) {
            ProfileRepository cleanupRepo;
            cleanupRepo.remove(profileId);
        }
        rollbackImport(newPath);
        delete profile;
        return false;
    }

    reportProgress(96, 100, tr("Committing final data..."));

    if (!dbMgr.commit()) {
        m_lastError = tr("Failed to commit final data: %1").arg(dbMgr.lastError().text());
        qWarning() << "ProfileImporter: Failed to commit summaries transaction";
        dbMgr.rollback();
        p_profile = savedProfile;
        if (profileId > 0) {
            ProfileRepository cleanupRepo;
            cleanupRepo.remove(profileId);
        }
        rollbackImport(newPath);
        delete profile;
        return false;
    }

    // Initialize channels for this profile immediately after import.
    // Read channels.dat directly from the source profile so user customizations
    // (colors, labels, thresholds, enabled state) are preserved without copying the file.
    // Falls back to schema defaults when the source has no channels.dat.
    if (!profile->migrateChannelsToDatabase(sourcePath)) {
        if (!profile->initializeChannelsFromSchema()) {
            qWarning() << "ProfileImporter: Failed to initialize channels for" << newProfileName
                       << "- they will be initialized when the profile is first opened";
        }
    }

    reportProgress(100, 100, tr("Import complete!"));

    // Delete profile (still needs p_profile set - destructor may call Session methods)
    delete profile;

    // Restore p_profile AFTER profile is deleted
    p_profile = savedProfile;

    return true;
}

bool ProfileImporter::validateSourceProfile(const QString& path)
{
    QDir dir(path);
    
    if (!dir.exists()) {
        m_lastError = tr("Source path does not exist");
        return false;
    }
    
    if (!QFile::exists(path + "/machines.xml")) {
        m_lastError = tr("Source is not a valid OSCAR profile (missing machines.xml)");
        return false;
    }
    
    return true;
}

bool ProfileImporter::copyProfileStructure(const QString& oldPath, 
                                          const QString& newPath)
{
    QDir oldDir(oldPath);
    QDir newDir(newPath);
    
    // 1. Copy journal folders (structure only, empty)
    if (!copyJournalFolders(oldPath, newPath)) {
        return false;
    }

    // 2. Copy graph layout files from profile root (daily.shg, overview.shg, etc.)
    QStringList shgFiles = oldDir.entryList(QStringList() << "*.shg", QDir::Files);
    for (const QString& shgFile : shgFiles) {
        if (!QFile::copy(oldPath + "/" + shgFile, newPath + "/" + shgFile)) {
            qWarning() << "ProfileImporter::copyProfileStructure: Failed to copy" << shgFile;
            // Not fatal - graph layout will revert to defaults
        }
    }

    // 3. Copy machine folders with their Backup subfolder structure
    QStringList entries = oldDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    
    for (const QString& entry : entries) {
        // Skip journal folders (already handled)
        if (entry.startsWith("Journal_")) continue;
        
        // Skip generated files/folders
        if (entry == "Summaries" || entry == "Events") continue;
        
        // This is a machine folder - create it and its Backup subfolder
        QString oldMachinePath = oldPath + "/" + entry;
        QString newMachinePath = newPath + "/" + entry;
        
        // Create machine folder
        if (!QDir().mkpath(newMachinePath)) {
            m_lastError = tr("Failed to create machine folder: %1").arg(newMachinePath);
            return false;
        }
        
        // Copy Backup subfolder recursively if it exists in source
        QString oldBackupPath = oldMachinePath + "/Backup";
        if (QDir(oldBackupPath).exists()) {
            QString newBackupPath = newMachinePath + "/Backup";
            reportProgress(10, 100, tr("Scanning backup files..."));
            m_copyFilesTotal = countFilesRecursively(oldBackupPath);
            m_copyFilesDone = 0;
            m_copyTimer.start();
            if (!copyDirectoryRecursively(oldBackupPath, newBackupPath)) {
                qWarning() << "Failed to copy Backup folder recursively:" << oldBackupPath;
                // Don't fail - Backup is optional
            }
        }
    }
    
    return true;
}

int ProfileImporter::countFilesRecursively(const QString& path)
{
    int count = 0;
    QDir dir(path);
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& info : entries) {
        if (m_cancelled) break;
        if (info.isDir())
            count += countFilesRecursively(info.filePath());
        else
            count++;
    }
    return count;
}

bool ProfileImporter::copyDirectoryRecursively(const QString& sourcePath, const QString& destPath)
{
    QDir sourceDir(sourcePath);
    if (!sourceDir.exists()) {
        return false;
    }
    
    QDir destDir(destPath);
    if (!destDir.exists()) {
        if (!QDir().mkpath(destPath)) {
            return false;
        }
    }
    
    // Copy all files in this directory
    QStringList files = sourceDir.entryList(QDir::Files);
    for (const QString& fileName : files) {
        if (m_cancelled) return false;

        QString srcFilePath = sourcePath + "/" + fileName;
        QString dstFilePath = destPath + "/" + fileName;

        if (!QFile::copy(srcFilePath, dstFilePath)) {
            qWarning() << "Failed to copy file:" << srcFilePath << "to" << dstFilePath;
        }
        m_copyFilesDone++;
        if (m_copyTimer.elapsed() >= 500) {
            int pct = (m_copyFilesTotal > 0)
                ? 10 + m_copyFilesDone * 10 / m_copyFilesTotal
                : 10;
            reportProgress(pct, 100,
                tr("Copying backup files: %1 / %2")
                    .arg(m_copyFilesDone)
                    .arg(m_copyFilesTotal));
            m_copyTimer.restart();
        }
    }

    // Recursively copy subdirectories
    QStringList subdirs = sourceDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& subdir : subdirs) {
        if (m_cancelled) return false;

        QString srcSubdirPath = sourcePath + "/" + subdir;
        QString dstSubdirPath = destPath + "/" + subdir;

        if (!copyDirectoryRecursively(srcSubdirPath, dstSubdirPath)) {
            qWarning() << "Failed to copy subdirectory:" << srcSubdirPath;
        }
    }
    
    return true;
}

bool ProfileImporter::copyJournalFolders(const QString& oldPath, 
                                        const QString& newPath)
{
    QDir oldDir(oldPath);
    QStringList entries = oldDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    
    for (const QString& entry : entries) {
        if (entry.startsWith("Journal_")) {
            // Create empty journal folder structure (files will be read from source and migrated to database)
            QString newJournalPath = newPath + "/" + entry;
            QDir().mkpath(newJournalPath);
            

        }
    }
    
    return true;
}

bool ProfileImporter::migrateMetadata(Profile* profile, const QString& oldPath)
{
    // machines.xml was already copied to profile folder before Profile creation
    // Now use MigrationManager to migrate it to the database
    MigrationManager migrator;
    
    // Use the Profile* overload which will read machines.xml from profile->path()
    if (!migrator.migrateProfile(profile)) {
        m_lastError = migrator.lastError();
        return false;
    }
    
    // Migration successful - remove machines.xml since all data is now in database
    QString machinesXml = profile->Get("{DataFolder}") + "/machines.xml";
    QFile::remove(machinesXml);
    
    // Reload machines from database now that they're migrated
    if (!profile->OpenMachines()) {
        m_lastError = tr("Failed to load machines from database");
        return false;
    }
    
    // Migrate journal data from source directory (read directly, don't copy files)
    if (!migrateJournalFromSource(profile, oldPath)) {
        qWarning() << "ProfileImporter::migrateMetadata() - Journal migration failed, but continuing";
        // Don't fail the import - journal is not critical
    }
    
    return true;
}

bool ProfileImporter::migrateJournalFromSource(Profile* profile, const QString& sourcePath)
{
//    qDebug() << "ProfileImporter::migrateJournalFromSource() - Starting";
    
    // Get journal machine
    Machine* journalMachine = profile->GetMachine(MT_JOURNAL);
    if (!journalMachine) {
        qDebug() << "ProfileImporter::migrateJournalFromSource() - No journal machine";
        return true;  // Not an error - profile may not have journal
    }
    
    // Verify machine is in database
    qint64 machineDbId = journalMachine->getDatabaseId();
    if (machineDbId == 0) {
        qWarning() << "ProfileImporter::migrateJournalFromSource() - Journal machine not in database";
        return false;
    }
    
//    qDebug() << "ProfileImporter::migrateJournalFromSource() - Journal machine database ID:" << machineDbId;
    
    // Find journal folder in source directory
    QDir sourceDir(sourcePath);
    QStringList entries = sourceDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    
    QString journalFolderName;
    for (const QString& entry : entries) {
        if (entry.startsWith("Journal_")) {
            journalFolderName = entry;
            break;
        }
    }
    
    if (journalFolderName.isEmpty()) {
        qDebug() << "ProfileImporter::migrateJournalFromSource() - No journal folder in source";
        return true;  // Not an error - no journal to migrate
    }
    
    // Read journal .000 files from SOURCE directory
    QString sourceSummariesPath = sourcePath + "/" + journalFolderName + "/Summaries";
    QDir dir(sourceSummariesPath);
    
    if (!dir.exists()) {
        qDebug() << "ProfileImporter::migrateJournalFromSource() - No summaries directory in source";
        return true;  // Not an error
    }
    
    QStringList filters;
    filters << "*.000";
    dir.setNameFilters(filters);
    QStringList files = dir.entryList(QDir::Files);
    
    if (files.isEmpty()) {
//        qDebug() << "ProfileImporter::migrateJournalFromSource() - No .000 files found in source";
        return true;  // Not an error
    }

    // Read the source user's height to backfill Journal_BMI during migration.
    // Height is not yet copied into `profile` at this stage of the import, so
    // open the source profile briefly just to read it.
    double userHeightCm = 0.0;
    {
        Profile* srcProfile = new Profile(sourcePath, true);
        if (srcProfile && srcProfile->isOpen()) {
            userHeightCm = srcProfile->user->height();
        }
        delete srcProfile;
    }

//    qDebug() << "ProfileImporter::migrateJournalFromSource() - Found" << files.size() << ".000 files to migrate";

    // Migrate each file
    int migratedCount = 0;
    int errorCount = 0;

    for (const QString& filename : files) {
        // Parse date from filename
        QString baseName = filename.section(".", 0, -2);  // Remove .000 extension
        bool ok;
        SessionID sessionId = baseName.toLongLong(&ok, 16);
        
        if (!ok) {
            qWarning() << "ProfileImporter::migrateJournalFromSource() - Invalid filename format:" << filename;
            errorCount++;
            continue;
        }
        
        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(sessionId);
        QDate date = dateTime.date();
        if (!date.isValid()) {
            qWarning() << "ProfileImporter::migrateJournalFromSource() - Invalid date for session:" << sessionId;
            errorCount++;
            continue;
        }
        
        // Load the .000 file from SOURCE directory
        QString fullPath = sourceSummariesPath + "/" + filename;
        
        // Create session object
        Session* sess = new Session(journalMachine, sessionId);
        
        // Load the session data from the file (this populates settings, first, last, etc.)
        if (!sess->LoadSummaryFromFile(fullPath)) {
            qWarning() << "ProfileImporter::migrateJournalFromSource() - Failed to load file" << fullPath;
            delete sess;
            errorCount++;
            continue;
        }

        // Validate Journal_Notes: discard if present but wrong type or empty.
        if (sess->settings.contains(Journal_Notes)) {
            QVariant noteVar = sess->settings[Journal_Notes];
            if (noteVar.typeId() != QMetaType::QString || noteVar.toString().isEmpty()) {
                qWarning() << "migrateJournalFromSource: Journal_Notes for session" << sessionId
                           << "has unexpected type or empty value — discarding";
                sess->settings.remove(Journal_Notes);
            }
        }

        // Backfill Journal_BMI: OSCAR 1.7.1 stored weight but never stored BMI.
        // Inject it now so the Overview BMI graph is populated after import.
        if (userHeightCm > 0.0 && sess->settings.contains(Journal_Weight)) {
            double kg = sess->settings[Journal_Weight].toDouble();
            if (kg > 0.0) {
                double h = userHeightCm / 100.0;
                sess->settings[Journal_BMI] = kg / (h * h);
            }
        }

        // Normalise Journal_ZombieMeter (Feelings) to a clean 0–100 range.
        // OSCAR 1.x used two encodings in the same field:
        //   Old encoding: 0–10, where 5 was a sentinel meaning "not set".
        //   New encoding: user_value + 20 (stored 20–120 for 0–100), with 0 meaning "not set".
        // Convert both to a 0–100 range (0 = not set) so the database holds clean values.
        if (sess->settings.contains(Journal_ZombieMeter)) {
            int feelings = sess->settings[Journal_ZombieMeter].toInt();
            int converted;
            if (feelings >= 20) {
                converted = feelings - 20;      // new encoding: strip the +20 offset
            } else if (feelings == 5) {
                converted = 0;                  // old "not set" sentinel
            } else {
                converted = feelings * 10;      // old 1–10 scale → 0–100
            }
            if (converted == 0) {
                sess->settings.remove(Journal_ZombieMeter);   // 0 = not set; omit the key
            } else {
                sess->settings[Journal_ZombieMeter] = converted;
            }
        }

        // IMPORTANT: Store session to database with the machine's database ID
        // The session must be associated with the correct machine_id for foreign keys to work
        if (!sess->StoreToDatabase()) {
            qWarning() << "ProfileImporter::migrateJournalFromSource() - Failed to store to database for" << date.toString();
            delete sess;
            errorCount++;
            continue;
        }
        
//        qDebug() << "ProfileImporter::migrateJournalFromSource() - Migrated" << date.toString() << "to database";
        migratedCount++;
        
        delete sess;
    }
    
//    qDebug() << "ProfileImporter::migrateJournalFromSource() - Migration complete:"
//             << migratedCount << "sessions migrated,"
//             << errorCount << "errors";
    
    return (errorCount == 0);
}

bool ProfileImporter::loadSessionsFromFiles(Profile* profile,
                                           const QString& oldPath)
{
    // NOTE: p_profile is already set by caller (importProfile)
    // We don't set or restore it here

    DatabaseManager& dbMgr = DatabaseManager::instance();

    // Scan for machine folders in old profile
    QDir oldDir(oldPath);
    QStringList entries = oldDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    
    int machineCount = 0;
    for (const QString& entry : entries) {
        // Skip non-machine folders
        if (entry.startsWith("Journal_") || 
            entry == "Summaries" || 
            entry == "Events") {
            continue;
        }
        machineCount++;
    }

    // Prevent possible divide by zero error if this should-be-impossible condition occurs
    if (machineCount == 0) {
        m_lastError = tr("No machine folders found in source profile");
        return false;
    }
    
    int currentMachine = 0;
    QStringList skippedFolders;

    for (const QString& entry : entries) {
        // Skip non-machine folders
        if (entry.startsWith("Journal_") ||
            entry == "Summaries" ||
            entry == "Events") {
            continue;
        }

        currentMachine++;
        QString oldMachinePath = oldPath + "/" + entry;

        reportProgress(40 + (currentMachine * 45 / machineCount), 100,
            tr("Loading machine %1 of %2...").arg(currentMachine).arg(machineCount));

        // Find corresponding machine in profile
        Machine* machine = findMachineByFolderName(profile, entry);
        if (!machine) {
            qWarning() << "Could not find machine for folder" << entry;
            qWarning() << "Available machines:" << profile->m_machlist.size();
            for (Machine* m : profile->m_machlist) {
                qWarning() << "  - Machine hexid:" << m->hexid() << "serial:" << m->serial()
                           << "loader:" << m->loaderName() << "type:" << m->type();
            }
            skippedFolders << entry;
            continue;
        }

//        qDebug() << "Loading sessions for machine:" << machine->hexid() << "from folder:" << entry;

        // Each machine gets its own transaction so the commit covers only one machine's
        // session data, keeping individual commits small and the UI responsive.
        if (!dbMgr.transaction()) {
            m_lastError = tr("Failed to start transaction for machine: %1").arg(entry);
            return false;
        }
        if (!loadMachineSessions(machine, oldMachinePath)) {
            dbMgr.rollback();
            return false;
        }
        int saveProgress = 40 + (m_loadedSessions * 45 / qMax(1, m_totalSessions));
        reportProgress(saveProgress, 100,
            tr("Saving sessions for machine %1 of %2...").arg(currentMachine).arg(machineCount));
        if (!dbMgr.commit()) {
            m_lastError = tr("Failed to commit sessions for machine: %1").arg(entry);
            return false;
        }

//        qDebug() << "Machine now has" << machine->sessionlist.size() << "sessions";
//        qDebug() << "Machine day list size:" << machine->day.size();
    }

    if (!skippedFolders.isEmpty()) {
        // Second pass: process-of-elimination matching.
        // Collect machines that were not matched by any folder in the first pass.
        QSet<Machine*> matchedMachines;
        for (const QString& entry : entries) {
            if (entry.startsWith("Journal_") || entry == "Summaries" || entry == "Events") {
                continue;
            }
            if (skippedFolders.contains(entry)) {
                continue;  // This folder failed to match
            }
            Machine* m = findMachineByFolderName(profile, entry);
            if (m) {
                matchedMachines.insert(m);
            }
        }

        QList<Machine*> unmatchedMachines;
        for (Machine* m : profile->m_machlist) {
            if (!matchedMachines.contains(m)) {
                unmatchedMachines.append(m);
            }
        }

        // If the number of unmatched machines equals the number of skipped folders,
        // try to assign them by machine type.
        if (unmatchedMachines.size() == skippedFolders.size()) {
            QStringList stillSkipped;
            for (const QString& skipped : skippedFolders) {
                QString oldMachinePath = oldPath + "/" + skipped;
                // Find an unmatched machine of type MT_CPAP (ResMed folders are always CPAP)
                Machine* assigned = nullptr;
                for (Machine* candidate : unmatchedMachines) {
                    if (candidate->type() == MT_CPAP) {
                        assigned = candidate;
                        break;
                    }
                }
                if (!assigned && !unmatchedMachines.isEmpty()) {
                    assigned = unmatchedMachines.first();
                }
                if (assigned) {
                    qWarning() << "ProfileImporter: Using process-of-elimination to assign folder"
                               << skipped << "to machine serial:" << assigned->serial()
                               << "hexid:" << assigned->hexid();
                    unmatchedMachines.removeOne(assigned);
                    if (!dbMgr.transaction()) {
                        m_lastError = tr("Failed to start transaction for machine: %1").arg(skipped);
                        return false;
                    }
                    if (!loadMachineSessions(assigned, oldMachinePath)) {
                        dbMgr.rollback();
                        return false;
                    }
                    int saveProgress = 40 + (m_loadedSessions * 45 / qMax(1, m_totalSessions));
                    reportProgress(saveProgress, 100, tr("Saving sessions..."));
                    if (!dbMgr.commit()) {
                        m_lastError = tr("Failed to commit sessions for machine: %1").arg(skipped);
                        return false;
                    }
                } else {
                    stillSkipped << skipped;
                }
            }
            if (!stillSkipped.isEmpty()) {
                m_lastError = tr("Could not match machine folder(s) to imported profile: %1")
                                  .arg(stillSkipped.join(", "));
                return false;
            }
        } else {
            m_lastError = tr("Could not match machine folder(s) to imported profile: %1")
                              .arg(skippedFolders.join(", "));
            return false;
        }
    }

    // Debug: Check profile's overall daylist
//    qDebug() << "ProfileImporter: Total sessions loaded:" << m_loadedSessions;
//    qDebug() << "ProfileImporter: Profile daylist size:" << profile->daylist.size();
//    qDebug() << "ProfileImporter: Profile first day:" << profile->FirstDay();
//    qDebug() << "ProfileImporter: Profile last day:" << profile->LastDay();

    return true;
}

QHash<quint32, bool> ProfileImporter::readSummariesEnabledMap(const QString& machinePath)
{
    QHash<quint32, bool> enabledMap;

    // Sessions.info is the authoritative source: 1.7.1 writes it on every Machine
    // destructor call (OSCAR exit), so it always reflects the latest enabled state.
    // Summaries.xml is only written during SD card imports and can be stale.
    static const quint32 OSCAR_MAGIC       = 0xC73216AB;
    static const quint16 FILETYPE_SESSENABLED = 5;

    QString sessInfoPath = machinePath + "/Sessions.info";
    QFile sessFile(sessInfoPath);
    if (sessFile.open(QIODevice::ReadOnly)) {
        QDataStream in(&sessFile);
        in.setByteOrder(QDataStream::LittleEndian);
        in.setVersion(QDataStream::Qt_5_0);

        quint32 magic;  in >> magic;
        quint16 ftype;  in >> ftype;
        quint16 ver;    in >> ver;

        if (magic == OSCAR_MAGIC && ftype == FILETYPE_SESSENABLED) {
            if (ver == 1) {
                // Legacy: skip a QHash<ChannelID,bool> that used to be here
                QHash<quint32, bool> crap;
                in >> crap;
            }
            int size;
            in >> size;
            for (int i = 0; i < size && !in.atEnd(); ++i) {
                quint32 sid;
                quint8  b;
                in >> sid >> b;
                enabledMap[sid] = (b & 0x1) != 0;
            }
            sessFile.close();
            return enabledMap;
        }
        sessFile.close();
        qWarning() << "ProfileImporter::readSummariesEnabledMap: Sessions.info has unexpected"
                   << "magic/filetype in" << sessInfoPath;
    }

    // Fallback: Summaries.xml (uncompressed — 1.7.1 SaveSummaryCache writes this)
    // or Summaries.xml.gz (compressed — written by older OSCAR builds).
    // These may be stale relative to Sessions.info, but are better than nothing
    // when Sessions.info is absent (very old profiles).
    QByteArray xmlData;
    QString xmlPath = machinePath + "/Summaries.xml";
    QString gzPath  = machinePath + "/Summaries.xml.gz";

    if (QFile::exists(xmlPath)) {
        QFile f(xmlPath);
        if (f.open(QIODevice::ReadOnly)) { xmlData = f.readAll(); f.close(); }
    }
    if (xmlData.isEmpty() && QFile::exists(gzPath)) {
        QFile f(gzPath);
        if (f.open(QIODevice::ReadOnly)) { xmlData = gUncompress(f.readAll()); f.close(); }
    }
    if (xmlData.isEmpty()) {
        return enabledMap;
    }

    QDomDocument doc;
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
    QString errorMsg;
    int errorLine = 0;
    if (!doc.setContent(xmlData, false, &errorMsg, &errorLine)) {
        qWarning() << "ProfileImporter::readSummariesEnabledMap: XML parse error in"
                   << xmlPath << "line" << errorLine << ":" << errorMsg;
        return enabledMap;
    }
#else
    auto result = doc.setContent(xmlData);
    if (!result) {
        qWarning() << "ProfileImporter::readSummariesEnabledMap: XML parse error in"
                   << xmlPath << "line" << result.errorLine << ":" << result.errorMessage;
        return enabledMap;
    }
#endif

    QDomElement root = doc.documentElement();
    if (root.tagName().compare("sessions", Qt::CaseInsensitive) != 0) {
        return enabledMap;
    }

    QDomNodeList sessionNodes = root.childNodes();
    for (int i = 0; i < sessionNodes.size(); ++i) {
        QDomElement e = sessionNodes.at(i).toElement();
        if (e.isNull()) continue;
        bool ok;
        quint32 id = e.attribute("id", "0").toUInt(&ok);
        if (!ok) continue;
        enabledMap[id] = e.attribute("enabled", "1").toInt() != 0;
    }

    return enabledMap;
}

bool ProfileImporter::loadMachineSessions(Machine* machine,
                                         const QString& oldMachinePath)
{
    // Scan for .000 files in Summaries subdirectory
    QString summariesPath = oldMachinePath + "/Summaries";
    QDir summariesDir(summariesPath);
    
    if (!summariesDir.exists()) {
        qDebug() << "ProfileImporter::loadMachineSessions: No Summaries folder for" << oldMachinePath;
        return true;  // Not an error - machine may have no sessions
    }
    
    QStringList filters;
    filters << "*.000";
    QFileInfoList files = summariesDir.entryInfoList(filters, QDir::Files);
    
    if (files.isEmpty()) {
        qDebug() << "ProfileImporter::loadMachineSessions: No session files found in" << summariesPath;
        return true;  // Not an error
    }
    
    m_totalSessions += files.size();

    // Read Summaries.xml(.gz) to recover each session's enabled/disabled state.
    // OSCAR 1.7.1 stores this in the XML index, not in the .000 binary files.
    QHash<quint32, bool> enabledMap = readSummariesEnabledMap(oldMachinePath);

    int sessionFailures = 0;
    int eventFailures = 0;

    for (const QFileInfo& fileInfo : files) {
        if (m_cancelled) {
            m_lastError = tr("Import cancelled by user");
            return false;
        }

        // Parse session ID from filename (hex)
        QString baseName = fileInfo.baseName();
        bool ok;
        SessionID sessionId = baseName.toLongLong(&ok, 16);

        if (!ok) {
            qWarning() << "ProfileImporter::loadMachineSessions: Invalid session filename:" << fileInfo.fileName();
            continue;
        }

        // Create session
        Session* session = new Session(machine, sessionId);

        // Load summary from .000 file (always sets enabled=true; corrected below)
        if (!session->LoadSummaryFromFile(fileInfo.absoluteFilePath())) {
            qWarning() << "ProfileImporter::loadMachineSessions: Failed to load summary:" << fileInfo.fileName();
            delete session;
            continue;
        }

        // Restore enabled/disabled state from Summaries.xml.  The session ID stored
        // in the XML is a quint32 (truncated from quint64), matching how 1.7.1 writes
        // it: el.setAttribute("id", (quint32)sess->session()).
        quint32 xmlId = static_cast<quint32>(sessionId);
        if (enabledMap.contains(xmlId) && !enabledMap[xmlId]) {
            session->setEnabled(false);
        }

        // Load events from .001 file (if exists)
        QString eventsPath = oldMachinePath + "/Events/" + baseName + ".001";
        if (QFile::exists(eventsPath)) {
            if (!session->LoadEventsFromFile(eventsPath)) {
                qWarning() << "ProfileImporter::loadMachineSessions: Failed to load events:" << eventsPath;
                // Continue anyway - summary data is still valid
            }
        }

        // Save session metadata to database
        if (!session->StoreToDatabase()) {
            qWarning() << "ProfileImporter::loadMachineSessions: Failed to store session to database:" << fileInfo.fileName();
            delete session;
            sessionFailures++;
            continue;
        }

        // Store events if they were loaded
        if (session->eventlist.size() > 0) {
            if (!session->StoreEventsToDatabase()) {
                QString dbErr = DatabaseManager::instance().lastError().text();
                qWarning() << "ProfileImporter::loadMachineSessions: Failed to store events for session:"
                           << fileInfo.fileName()
                           << (dbErr.isEmpty() ? QString() : QString("(SQL: %1)").arg(dbErr));
                if (m_lastEventFailFile.isEmpty()) {
                    m_lastEventFailFile = fileInfo.fileName();
                    m_lastEventFailDbError = dbErr;
                }
                eventFailures++;
            }
        }
        
        // Add to machine
//        qDebug() << "ProfileImporter: Adding session" << sessionId << "to machine";
        machine->AddSession(session);
//        qDebug() << "ProfileImporter: Machine now has" << machine->sessionlist.size() << "sessions";
//        qDebug() << "ProfileImporter: Machine day list size:" << machine->day.size();
        
        m_loadedSessions++;
        
        // Update progress every 5 sessions to keep the UI responsive
        if (m_loadedSessions % 5 == 0 || m_loadedSessions == m_totalSessions) {
            int progress = 40 + (m_loadedSessions * 45 / qMax(1, m_totalSessions));
            reportProgress(progress, 100,
                tr("Loaded %1 of %2 sessions...").arg(m_loadedSessions).arg(m_totalSessions));
        }
    }

    if (sessionFailures > 0 || eventFailures > 0) {
        m_lastError = tr("Session persistence failures: %1 session(s) and %2 event set(s) failed to store")
                          .arg(sessionFailures).arg(eventFailures);
        if (!m_lastEventFailFile.isEmpty()) {
            m_lastError += tr(" (first failure: %1").arg(m_lastEventFailFile);
            if (!m_lastEventFailDbError.isEmpty()) {
                m_lastError += tr("; SQL: %1").arg(m_lastEventFailDbError);
            }
            m_lastError += ")";
        }
        return false;
    }

    return true;
}

bool ProfileImporter::calculateSummaries(Profile* profile)
{
    try {
        profile->calculateDailySummaries();
        return true;
    } catch (...) {
        qWarning() << "ProfileImporter::calculateSummaries: Exception while calculating summaries";
        return false;
    }
}

bool ProfileImporter::rollbackImport(const QString& profilePath)
{
    // Delete the partially created profile
    QDir dir(profilePath);
    if (dir.exists()) {
        bool success = dir.removeRecursively();
        if (!success) {
            qWarning() << "Failed to rollback import - could not remove" << profilePath;
        }
        return success;
    }
    return true;
}

Machine* ProfileImporter::findMachineByFolderName(Profile* profile, const QString& folderName)
{
    // Folder name format is typically "Brand_Serial" (e.g., "ResMed_22222439696")
    // Extract the serial number from the folder name
    QString serial = folderName;
    if (folderName.contains('_')) {
        serial = folderName.section('_', -1);  // Get last part after underscore
    }
    
#ifdef DBDEBUG
    qDebug() << "Matching folder:" << folderName << "extracted serial:" << serial;
#endif

    // Try matching by serial number
    for (Machine* machine : profile->m_machlist) {
        QString machineSerial = machine->serial();
#ifdef DBDEBUG
        qDebug() << "  Checking machine serial:" << machineSerial << "hexid:" << machine->hexid();
#endif
        if (machineSerial == serial) {
#ifdef DBDEBUG
            qDebug() << "  MATCH! Found machine by serial";
#endif
            return machine;
        }
    }

    // Try matching by hexid (for older formats)
    for (Machine* machine : profile->m_machlist) {
        QString machineFolder = machine->hexid();
        if (machineFolder == folderName) {
#ifdef DBDEBUG
            qDebug() << "  MATCH! Found machine by hexid";
#endif
            return machine;
        }
    }

    // Try partial match
    for (Machine* machine : profile->m_machlist) {
        QString machineSerial = machine->serial();
        QString machineFolder = machine->hexid();
        if (folderName.contains(machineSerial) || folderName.contains(machineFolder)) {
#ifdef DBDEBUG
            qDebug() << "  MATCH! Found machine by partial match";
#endif
            return machine;
        }
    }

    // Last resort: if there's only one CPAP machine, use it
    QList<Machine*> cpapMachines;
    for (Machine* machine : profile->m_machlist) {
        if (machine->type() == MT_CPAP) {
            cpapMachines.append(machine);
        }
    }

    if (cpapMachines.size() == 1) {
#ifdef DBDEBUG
        qDebug() << "  MATCH! Using only CPAP machine";
#endif
        return cpapMachines.first();
    }
    
    qWarning() << "Could not find machine for folder:" << folderName;
    return nullptr;
}

void ProfileImporter::reportProgress(int current, int total, const QString& message)
{
    emit progressChanged(current, total, message);

    if (m_progress) {
        m_progress->setMessage(message);
        m_progress->setProgressMax(total);
        m_progress->setProgressValue(current);

        // Process events to keep UI responsive
        QApplication::processEvents();
    }
}

/*!
 * \brief Copy the layoutSettings folder from the OSCAR 1.x data root to OSCAR 2.0.
 *
 * layoutSettings is stored at the data-root level (shared across profiles), not inside
 * any individual profile folder.  Only files not already present in the destination are
 * copied, so existing OSCAR 2.0 layouts are never overwritten.
 */
void ProfileImporter::copyLayoutSettings(const QString& sourceDataPath)
{
    QString sourceLayoutPath = sourceDataPath + "/layoutSettings";
    QString destLayoutPath   = GetAppData() + "/layoutSettings";

    QDir sourceDir(sourceLayoutPath);
    if (!sourceDir.exists()) {
        qDebug() << "ProfileImporter::copyLayoutSettings: No layoutSettings folder in source";
        return;
    }

    QStringList files = sourceDir.entryList(QDir::Files);
    int copied = 0;
    bool destCreated = false;
    for (const QString& fileName : files) {
        QString destFile = destLayoutPath + "/" + fileName;
        if (!QFile::exists(destFile)) {
            if (!destCreated) {
                QDir().mkpath(destLayoutPath);
                destCreated = true;
            }
            if (QFile::copy(sourceLayoutPath + "/" + fileName, destFile)) {
                copied++;
            } else {
                qWarning() << "ProfileImporter::copyLayoutSettings: Failed to copy" << fileName;
            }
        }
    }
    qDebug() << "ProfileImporter::copyLayoutSettings: Copied" << copied << "of"
             << files.size() << "file(s) from" << sourceLayoutPath;
}

/*!
 * \brief Migrate app-level preferences from OSCAR 1.x Preferences.xml.
 *
 * All user-configurable app settings (graph appearance, tab selection, startup behaviour,
 * etc.) live in the global Preferences.xml (backed by p_pref / AppSetting), not in the
 * profile.  Copy every key from the source file, skipping a handful of OSCAR 2.0-specific
 * tracking values that should not be overwritten.
 */
bool ProfileImporter::migrateAppSettings(const QString& sourceDataPath)
{
    QString sourcePrefFile = sourceDataPath + "/Preferences.xml";
    if (!QFile::exists(sourcePrefFile)) {
        qDebug() << "ProfileImporter::migrateAppSettings: No Preferences.xml in source data folder";
        return true;
    }

    Preferences sourcePref("Preferences", sourcePrefFile);
    if (!sourcePref.Open()) {
        qWarning() << "ProfileImporter::migrateAppSettings: Failed to open" << sourcePrefFile;
        return false;
    }

    // Keys that must NOT be overwritten with source values:
    //   VersionString    — keep OSCAR 2.0's own version identifier
    //   Profile          — last-opened profile name may differ in OSCAR 2.0; leave as-is
    //   Skipped*Version  — update-skip tracking is version-specific to this installation
    //   UpdatesLastChecked — let OSCAR 2.0 do its own update check
    static const QSet<QString> skipKeys = {
        STR_PREF_VersionString,
        STR_GEN_Profile,
        STR_GEN_SkippedReleaseVersion,
        STR_GEN_SkippedTestVersion,
        STR_GEN_UpdatesLastChecked,
    };

    int copied = 0;
    for (auto it = sourcePref.begin(); it != sourcePref.end(); ++it) {
        if (!skipKeys.contains(it.key())) {
            p_pref->Set(it.key(), it.value());
            copied++;
        }
    }

    if (copied > 0) {
        if (!p_pref->Save()) {
            qWarning() << "ProfileImporter::migrateAppSettings: Failed to save migrated app settings";
            return false;
        }
        qDebug() << "ProfileImporter::migrateAppSettings: Migrated" << copied
                 << "app settings from" << sourcePrefFile;
        // Refresh AppSetting's in-memory cache so migrated values (e.g. GraphHeight)
        // are visible immediately without requiring a restart.
        delete AppSetting;
        AppSetting = new AppWideSetting(p_pref);
    }
    return true;
}
