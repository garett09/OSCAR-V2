/* OSCAR Main
 *
 * Copyright (c) 2019-2026 The OSCAR Team
 * Copyright (c) 2011-2018 Mark Watkins
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#define TEST_MACROS_ENABLEDoff
#include <test_macros.h>
#include <QtGlobal>

#ifdef UNITTEST_MODE
#include "tests/AutoTest.h"
#endif

#include <QApplication>
#include <QGuiApplication>
#include <QMessageBox>
#include <QDebug>
#include <QTranslator>
#include <QSettings>
#include <QFileDialog>
#include <QFontDatabase>
#include <QStandardPaths>
#include <QProgressDialog>
#include <QDialog>
#include <QLabel>
#include <QVBoxLayout>
#include <QThread>
#include <QEventLoop>
#include <QRegularExpression>
#include <QStyleHints>
#include <QStyleFactory>
#include <QLockFile>
#include <QProcess>
#include <QPushButton>
#include <QDateTime>
#include <functional>

#include "version.h"
#include "logger.h"
#include "mainwindow.h"
#include "datafolderdialog.h"
#include "SleepLib/profiles.h"
#include "translation.h"
#include "speedcheck.h"
#include "SleepLib/common.h"
#include "SleepLib/deviceconnection.h"
#include "Graphs/gGraph.h"


#include <ctime>
#include <chrono>

// Gah! I must add the real darn plugin system one day.
#include "SleepLib/loader_plugins/applehealth_loader.h"
#include "SleepLib/loader_plugins/prs1_loader.h"
#include "SleepLib/loader_plugins/cms50_loader.h"
#include "SleepLib/loader_plugins/cms50f37_loader.h"
#include "SleepLib/loader_plugins/md300w1_loader.h"
#include "SleepLib/loader_plugins/zeo_loader.h"
#include "SleepLib/loader_plugins/somnopose_loader.h"
#include "SleepLib/loader_plugins/resmed_loader.h"
#include "SleepLib/loader_plugins/intellipap_loader.h"
#include "SleepLib/loader_plugins/icon_loader.h"
#include "SleepLib/loader_plugins/sleepstyle_loader.h"
#include "SleepLib/loader_plugins/weinmann_loader.h"
#include "SleepLib/loader_plugins/viatom_loader.h"
#include "SleepLib/loader_plugins/prisma_loader.h"
#include "SleepLib/loader_plugins/resvent_loader.h"
#include "SleepLib/loader_plugins/vrem_loader.h"
#include "SleepLib/loader_plugins/bmc_loader.h"
#include "SleepLib/loader_plugins/bmcg3x_loader.h"
#include "SleepLib/loader_plugins/yuwell_loader.h"
#include "SleepLib/loader_plugins/sefam_loader.h"
#include "SleepLib/loader_plugins/apex_loader.h"

#include "database/database_manager.h"
#include "database/database_schema.h"
#include "database/recent_databases.h"
#include "database/profile_repository.h"
#include "database/machine_repository.h"
#include "database/migration_manager.h"
#include "database/graph_layouts_repository.h"
#include "profileimporter.h"
#include "SleepLib/progressdialog.h"

MainWindow *mainwin = nullptr;
extern bool openOk;

// Return a QList of all profile directories in a data directory (sourcePath)
QList<QString> enumerateProfiles(QString sourcePath){    // Find all profiles in that directory
    QDir dir(sourcePath);
    QList<QString> goodProfiles;

    // Just a double check this is not an OSCAR 2.0 data directory
    QFile dbFile(sourcePath + "/oscar.db");
    if (dbFile.exists()) {
        return goodProfiles;
    }

    // Look through the Profiles subdirectory for all profiles present
    QDir profilesDir(sourcePath + "/Profiles");
    QFileInfoList entries = profilesDir.entryInfoList(QDir::AllDirs | QDir::NoDotAndDotDot);

    for (const QFileInfo &fileInfo : entries) {
        if (fileInfo.isDir()) {
            QString profilePath = fileInfo.absoluteFilePath();
            QFile  machinesXml(profilePath + "/machines.xml");

            if (machinesXml.exists()) {
                // Looks like an OSCAR 1.x profile, keep the name
                goodProfiles.append(fileInfo.fileName());
            }
        }
    }

    return goodProfiles;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Migrate OSCAR 1.x data to OSCAR 2.0
////////////////////////////////////////////////////////////////////////////////////////////
bool migrateFromOSCAR(QString destDir) {
    QString homeDocs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)+"/";
    QString sourcePath;
    bool selectingFolder = true;
    bool success = false;
    QList<QString> profileList;

    if (destDir.isEmpty()) {
        qDebug() << "Migration path is empty string";
        return success;
    }

    // Suggest whatever data folder OSCAR 1.x itself has recorded as current on this
    // computer, rather than defaulting to the Documents folder and making the user
    // hunt for it.
    QString startPath = homeDocs;
    QString legacyDataPath = findLegacyOscarDataFolder();
    if (!legacyDataPath.isEmpty()) {
        qDebug() << "Suggesting OSCAR 1.x data folder from its settings:" << legacyDataPath;
        startPath = legacyDataPath;
    }

    while (selectingFolder) {
        sourcePath = QFileDialog::getExistingDirectory(nullptr,
                  QObject::tr("Choose the OSCAR 1.x data folder to migrate")+" "+
                  QObject::tr("or CANCEL to skip migration."),
                  startPath, QFileDialog::ShowDirsOnly | nativeDialogOption());
        qDebug() << "Migration source folder selected: " + sourcePath;
        if (sourcePath.isEmpty()) {
            qDebug() << "No migration source directory selected";
            return false;
        } else {                        // We have a source folder, see if is an OSCAR 1.x folder
            QDir  sourceDir(sourcePath);
            QFile sourcePrefsFile(sourcePath + "/Preferences.xml");
            QDir  sourceDirProfiles(sourcePath + "/Profiles");
            QFile sourceDatabase(sourcePath + "/oscar.db");

            if (!sourcePrefsFile.exists() || !sourceDirProfiles.exists() || sourceDatabase.exists()) {       // It doesn't have a Preferences.xml file or a Profiles directory or has a database in it
                // Not an OSCAR 1.x directory or maybe an OSCAR 2.x directory.. nag the user.
                QMessageBox::warning(nullptr, STR_MessageBox_Error,
                                     QObject::tr("The folder you chose does not contain valid OSCAR 1.x data.") +
                                     "\n\n"+QObject::tr("You cannot migrate from this folder:")+" " + sourcePath,
                                     QMessageBox::Ok);
                continue;   // Nope, don't use it, go around the loop again
            }

            profileList = enumerateProfiles(sourcePath);    // Find all profiles in that directory

            if (profileList.size() == 0) {      // are there not any profiles?
                QMessageBox::warning(nullptr, STR_MessageBox_Error,
                                     QObject::tr("The folder you chose does not contain any OSCAR profiles.") +
                                                 "\n\n"+QObject::tr("You cannot migrate from this folder:")+" " + sourcePath,
                                     QMessageBox::Ok);
                qDebug() << "No profiles found in" << sourcePath;
                continue;
            }
            qDebug() << "Migration folder is" << sourcePath;
            selectingFolder = false;
        }
    }

    // Create a progress dialog for the migration.
    // Range 0-100 shows per-profile progress; the label shows overall n-of-n count.
    QProgressDialog progress(QObject::tr("Migrating Profiles"),
                            QObject::tr("Cancel"),
                            0, 100,
                            nullptr,
                            Qt::WindowSystemMenuHint | Qt::WindowTitleHint);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumWidth(400);
    progress.setWindowTitle(QObject::tr("Migrating OSCAR data"));
    progress.setMinimumDuration(0);
    progress.setAutoReset(false);   // Don't reset when value reaches maximum
    progress.setAutoClose(false);   // Don't hide when value reaches maximum
    progress.show();
    QApplication::processEvents();

    int profilesSucceeded = 0;
    int profilesFailed = 0;
    bool importCancelled = false;
    QStringList failedProfiles;
    QStringList successfulProfiles;
    auto startTime = std::chrono::steady_clock::now();

    for (int i = 0; i < profileList.size(); i++) {
        const QString& profileDir = profileList[i];

        if (importCancelled) {
            qDebug() << "Migration cancelled by user";
            break;
        }

        qDebug() << "Migrating profile" << profileDir << "(" << (i+1) << "of" << profileList.size() << ")";

        QString profileSourcePath = sourcePath + "/Profiles/" + profileDir;

        // Reset progress bar to 0 for this profile, show n-of-n in label.
        progress.setValue(0);
        progress.setLabelText(QObject::tr("Migrating profile: %1\n(%2 of %3)\n\nStarting import...")
                            .arg(profileDir)
                            .arg(i + 1)
                            .arg(profileList.size()));
        QApplication::processEvents();

        // Create ProfileImporter and connect to our existing progress dialog.
        // The progress bar advances per-profile (0-100); the label shows the n-of-n count.
        ProfileImporter importer;

        // Order matters: update the UI before setting the cancel flag so the user sees
        // feedback immediately. Qt delivers direct connections in connection order.
        // Store the connection handle so it can be disconnected at the end of this iteration —
        // progress outlives the loop body and connections accumulate otherwise.
        QMetaObject::Connection cancelUIConn = QObject::connect(&progress, &QProgressDialog::canceled,
                        [&progress, &importCancelled, profileDir, i, &profileList]() {
                            importCancelled = true;
                            progress.setCancelButton(nullptr);  // Remove button — already cancelling
                            progress.setLabelText(
                                QObject::tr("Cancelling: %1\n(%2 of %3)\n\nCleaning up, please wait...")
                                    .arg(profileDir)
                                    .arg(i + 1)
                                    .arg(profileList.size()));
                            progress.show();
                            QApplication::processEvents();
                        });
        QObject::connect(&progress, &QProgressDialog::canceled, &importer, &ProfileImporter::cancel);

        QObject::connect(&importer, &ProfileImporter::progressChanged,
                        [&progress, &importCancelled, profileDir, i, &profileList](int current, int total, const QString& message) {
                            Q_UNUSED(total)
                            // Don't touch the dialog after the user cancels — setValue()
                            // internally calls show(), which would make the dialog reappear.
                            if (importCancelled) return;
                            progress.setValue(current);
                            progress.setLabelText(QObject::tr("Migrating profile: %1\n(%2 of %3)\n\n%4")
                                                .arg(profileDir)
                                                .arg(i + 1)
                                                .arg(profileList.size())
                                                .arg(message));
                            QApplication::processEvents();
                        });

        bool migrationSuccess = importer.importProfile(profileSourcePath, profileDir, nullptr);

        // Disconnect the per-iteration canceled() lambda so it doesn't accumulate
        // across profiles (importer's connection is auto-disconnected on destruction).
        QObject::disconnect(cancelUIConn);

        if (migrationSuccess) {
            profilesSucceeded++;
            successfulProfiles.append(profileDir);
            qDebug() << "Successfully migrated profile:" << profileDir;
        } else {
            if (!importCancelled) {
                profilesFailed++;
                failedProfiles.append(profileDir);
                qWarning() << "Failed to migrate profile:" << profileDir;
                qWarning() << "Error:" << importer.lastError();
            }
        }

        if (!importCancelled) {
            progress.setValue(100);
            QApplication::processEvents();
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime);
    
    progress.close();

    // Report results
    QString resultMessage;
    if (importCancelled) {
        resultMessage = QObject::tr("Migration cancelled.");
        if (profilesSucceeded > 0) {
            resultMessage += "\n\n" + QObject::tr("The following profile(s) were fully imported before cancellation:")
                           + "\n" + successfulProfiles.join("\n");
        } else {
            resultMessage += "\n\n" + QObject::tr("No profiles were imported.");
        }
        qDebug() << resultMessage;
        QMessageBox::information(nullptr, QObject::tr("Migration Cancelled"), resultMessage);
        success = profilesSucceeded > 0;
    } else if (profilesSucceeded > 0 && profilesFailed == 0) {
        resultMessage = QObject::tr("Successfully migrated %1 profile(s) in %2 seconds.")
                       .arg(profilesSucceeded)
                       .arg(elapsed.count());
        resultMessage += "\n\n" + QObject::tr("Imported profiles:") + "\n" + successfulProfiles.join("\n");
        qDebug() << resultMessage;
        QMessageBox::information(nullptr, QObject::tr("Migration Complete"), resultMessage);
        success = true;
    } else if (profilesSucceeded > 0 && profilesFailed > 0) {
        resultMessage = QObject::tr("Migrated %1 profile(s) successfully, but %2 profile(s) failed in %3 seconds.")
                       .arg(profilesSucceeded)
                       .arg(profilesFailed)
                       .arg(elapsed.count());
        if (!successfulProfiles.isEmpty()) {
            resultMessage += "\n\n" + QObject::tr("Imported profiles:") + "\n" + successfulProfiles.join("\n");
        }
        if (!failedProfiles.isEmpty()) {
            resultMessage += "\n\n" + QObject::tr("Failed profiles:") + "\n" + failedProfiles.join("\n");
        }
        qWarning() << resultMessage;
        QMessageBox::warning(nullptr, QObject::tr("Migration Partially Complete"), resultMessage);
        success = true; // Partial success
    } else if (profilesFailed > 0) {
        resultMessage = QObject::tr("Failed to migrate any profiles. All %1 profile(s) failed.")
                       .arg(profilesFailed);
        qCritical() << resultMessage;
        QMessageBox::critical(nullptr, QObject::tr("Migration Failed"), resultMessage);
        success = false;
    }

    return success;
}

// Returns the format version stored in bytes 4-5 of an .shg file, or 0 if too short.
static int peekShgVersion(const QByteArray& data)
{
    if (data.size() < 6) return 0;
    quint16 ver;
    memcpy(&ver, data.constData() + 4, 2);
    return static_cast<int>(ver);
}

// One-shot import of legacy layoutSettings/*.shg named layouts into the DB.
// Reads all .shg bytes and descriptions.txt first, then writes to the DB inside a
// single transaction per view. A crash during the write rolls back the transaction,
// leaving the source files intact so the next launch retries cleanly.
void importLegacyNamedLayouts()
{
    if (!DatabaseManager::instance().isOpen()) return;

    const QString layoutDir = GetAppData() + "/layoutSettings/";
    QDir dir(layoutDir);
    if (!dir.exists()) return;

    GraphLayoutsRepository repo;
    QSqlDatabase db = DatabaseManager::instance().database();
    const QStringList viewNames = { "daily", "overview" };

    for (const QString& viewName : viewNames) {
        // Read descriptions from <viewName>.descriptions.txt
        QMap<QString, QString> descriptions;
        QFile descFile(layoutDir + viewName + ".descriptions.txt");
        if (descFile.open(QFile::ReadOnly)) {
            QTextStream in(&descFile);
            QString line;
            while (in.readLineInto(&line)) {
                int sep = line.indexOf(':');
                if (sep > 0) {
                    descriptions[line.left(sep)] = line.mid(sep + 1);
                }
            }
            descFile.close();
        }

        // Phase 1: read all layout files into memory.
        struct LayoutEntry { int slotIndex; QString desc; int version; QByteArray data; QString filePath; };
        QList<LayoutEntry> entries;
        QRegularExpression re(QString("^%1\\.(layout(\\d+))\\.shg$").arg(viewName));
        const QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Name);
        for (const QFileInfo& fi : files) {
            QRegularExpressionMatch m = re.match(fi.fileName());
            if (!m.hasMatch()) continue;
            QFile f(fi.absoluteFilePath());
            if (!f.open(QFile::ReadOnly)) {
                qWarning() << "importLegacyNamedLayouts: cannot open" << fi.absoluteFilePath() << "- skipping";
                continue;
            }
            LayoutEntry e;
            e.slotIndex = m.captured(2).toInt();
            e.desc = descriptions.value(m.captured(1), m.captured(1));
            e.data = f.readAll();
            e.version = peekShgVersion(e.data);
            e.filePath = fi.absoluteFilePath();
            f.close();
            if (e.version == 0) {
                qWarning() << "importLegacyNamedLayouts: skipping corrupt/undersized file" << fi.absoluteFilePath();
                continue;
            }
            entries.append(e);
        }

        if (entries.isEmpty()) {
            // No layout files for this view — remove the descriptions file now
            // so it doesn't prevent the folder from being cleaned up below.
            descFile.remove();
            continue;
        }

        // Phase 2: write all to DB atomically, then delete source files on success.
        db.transaction();
        bool allOk = true;
        for (const LayoutEntry& e : entries) {
            if (!repo.saveNamedLayout(viewName, e.slotIndex, e.desc, e.version, e.data)) {
                allOk = false;
                break;
            }
        }
        if (allOk) {
            db.commit();
            for (const LayoutEntry& e : entries) {
                QFile::remove(e.filePath);
            }
            descFile.remove();
        } else {
            db.rollback();
        }
    }

    // If layoutSettings dir is now empty, remove it
    if (dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
        dir.rmdir(layoutDir);
    }
}

// One-shot import of per-profile daily.shg / overview.shg files into the DB.
// Reads all layout files into memory first, then writes everything in a single
// transaction. A crash during the write rolls back so source files remain and
// the next launch retries cleanly.
void importLegacyProfileLayouts()
{
    if (!DatabaseManager::instance().isOpen()) return;

    ProfileRepository profRepo;
    GraphLayoutsRepository layoutRepo;
    const QList<ProfileData> profiles = profRepo.findAll();
    const QString profilesBase = GetAppData() + "/Profiles/";
    QSqlDatabase db = DatabaseManager::instance().database();

    // Phase 1: read all layout files into memory.
    struct LayoutEntry { qint64 profileId; QString viewName; int version; QByteArray data; QString filePath; };
    QList<LayoutEntry> entries;
    for (const ProfileData& pd : profiles) {
        const QString profileDir = profilesBase + pd.username;
        for (const QString& viewName : { QString("daily"), QString("overview") }) {
            const QString shgPath = profileDir + "/" + viewName + ".shg";
            QFile f(shgPath);
            if (!f.exists()) continue;
            if (!f.open(QFile::ReadOnly)) continue;
            LayoutEntry e;
            e.profileId = pd.id;
            e.viewName  = viewName;
            e.data      = f.readAll();
            e.version   = peekShgVersion(e.data);
            e.filePath  = shgPath;
            f.close();
            entries.append(e);
        }
    }

    if (entries.isEmpty()) return;

    // Phase 2: write all to DB atomically, then delete source files on success.
    db.transaction();
    bool allOk = true;
    for (const LayoutEntry& e : entries) {
        if (!layoutRepo.saveCurrentLayout(e.profileId, e.viewName, e.version, e.data)) {
            qWarning() << "importLegacyProfileLayouts: failed to import" << e.filePath;
            allOk = false;
            break;
        }
    }
    if (allOk) {
        db.commit();
        for (const LayoutEntry& e : entries) {
            QFile::remove(e.filePath);
        }
    } else {
        db.rollback();
    }
}

#ifdef UNITTEST_MODE

int main(int argc, char* argv[])
{
    initializeStrings();
    qDebug() << STR_TR_OSCAR + " " + getVersion();

    AutoTest::run(argc, argv);
}

#else

/*
 * Run a blocking job on a worker thread while the caller's event loop keeps the
 * UI (a wait or progress dialog) responsive. Returns when the job has finished.
 */
static void runOnWorkerThread(const std::function<void()>& work)
{
    QThread* worker = QThread::create(work);
    QEventLoop loop;
    QObject::connect(worker, &QThread::finished, &loop, &QEventLoop::quit);
    worker->start();
    loop.exec();
    worker->wait();
    delete worker;
}

/*
 * Gate an irreversible database schema upgrade behind the user's consent.
 *
 * Mirrors Profile::DataFormatError(), which does the same when a loader's data
 * format changes: explain what is about to happen, let the user exit instead,
 * and only then proceed. Here the user can also have OSCAR write a complete
 * single-file copy of the database first (VACUUM INTO folds the WAL in, so
 * nothing is missed the way a hand copy of oscar.db alone would be).
 *
 * The copy and the migrations run on a worker thread behind an application-modal
 * progress dialog. The dialog has no Cancel button: each migration is one atomic
 * transaction that cannot be interrupted, and the decision to stop was offered
 * before anything was touched.
 *
 * Returns true when the database is upgraded and DatabaseManager is ready;
 * false when the user chose to exit or the upgrade failed. Failures have been
 * reported to the user before returning; the caller only needs to exit.
 */
static bool confirmAndUpgradeDatabase(const QString& dbPath, int fromVersion)
{
    DatabaseManager& dbMgr = DatabaseManager::instance();
    const int toVersion = DatabaseSchema::CURRENT_SCHEMA_VERSION;
    const QString folder = QDir::toNativeSeparators(QFileInfo(dbPath).absolutePath());

    QString msg = "<font size=+1>"
        + QObject::tr("OSCAR %1 needs to upgrade its database from version %2 to version %3.")
              .arg(getVersion().displayString()).arg(fromVersion).arg(toVersion)
        + "</font><br/><br/>"
        + "<font size=+1>" + QObject::tr("Important:") + "</font> "
        + QObject::tr("Once upgraded, this database <font size=+1>cannot</font> be opened by earlier versions of OSCAR, and the upgrade cannot be undone.")
        + "<br/><br/>"
        + QObject::tr("<b>Back up and upgrade</b> first saves a complete copy of the current database in %1, so you can go back to it if needed.").arg(folder)
        + "<br/><br/>"
        + QObject::tr("<b>Exit</b> leaves the database unchanged, so you can back it up yourself before starting OSCAR again.")
        + "<br/><br/>"
        + "<font size=+1>" + QObject::tr("Are you ready to upgrade?") + "</font>";

    QMessageBox question(QMessageBox::Warning, QObject::tr("Database Upgrade Required"), msg);
    QPushButton* backupButton = question.addButton(QObject::tr("Back up and upgrade"), QMessageBox::AcceptRole);
    question.addButton(QObject::tr("Upgrade"), QMessageBox::AcceptRole);
    QPushButton* exitButton = question.addButton(QObject::tr("Exit"), QMessageBox::RejectRole);
    question.setDefaultButton(backupButton);
    question.setEscapeButton(exitButton);
    question.exec();

    if (question.clickedButton() == exitButton) {
        qDebug() << "Main: User declined the schema upgrade from" << fromVersion << "to" << toVersion;
        return false;
    }
    const bool makeBackup = (question.clickedButton() == backupButton);

    // Busy indicator (range 0,0) plus a label naming the current step: each step is
    // a single transaction, so there is no finer-grained progress to show.
    QProgressDialog progress(QString(), QString(), 0, 0, nullptr,
                             Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
    progress.setWindowTitle(QObject::tr("Upgrading Database"));
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setCancelButton(nullptr);
    progress.setMinimumDuration(0);
    progress.setMinimumWidth(450);
    progress.setAutoClose(false);
    progress.setAutoReset(false);

    // The worker thread reports through this; the dialog must only be touched
    // on the GUI thread, so queue the update.
    auto setStatus = [&progress](const QString& text) {
        QMetaObject::invokeMethod(&progress, [&progress, text]() {
            progress.setLabelText(text);
        }, Qt::QueuedConnection);
    };

    QString backupPath;
    if (makeBackup) {
        backupPath = QFileInfo(dbPath).dir().filePath(
            QString("oscar-backup-v%1-%2.db").arg(fromVersion)
                .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));
        progress.setLabelText(QObject::tr("Backing up the database to %1...").arg(QFileInfo(backupPath).fileName()));
        progress.show();
        QApplication::processEvents();

        bool backupOk = false;
        QString backupError;
        runOnWorkerThread([&]() {
            backupOk = dbMgr.snapshotTo(backupPath, &backupError);
        });

        if (!backupOk) {
            progress.hide();
            QMessageBox retry(QMessageBox::Warning, QObject::tr("Database Backup Failed"),
                QObject::tr("OSCAR could not save a copy of the database:") + "\n\n" + backupError + "\n\n"
                    + QObject::tr("You can upgrade without a backup, or exit and back up the database yourself first."));
            QPushButton* noBackupButton = retry.addButton(QObject::tr("Upgrade without backup"), QMessageBox::AcceptRole);
            QPushButton* retryExitButton = retry.addButton(QObject::tr("Exit"), QMessageBox::RejectRole);
            retry.setDefaultButton(retryExitButton);
            retry.setEscapeButton(retryExitButton);
            retry.exec();
            if (retry.clickedButton() != noBackupButton) {
                qDebug() << "Main: User exited after backup failure:" << backupError;
                return false;
            }
            backupPath.clear();
        }
        qDebug() << "Main: Pre-upgrade database copy:" << (backupPath.isEmpty() ? "none" : backupPath);
    }

    progress.setLabelText(QObject::tr("Upgrading database from version %1 to version %2...").arg(fromVersion).arg(toVersion));
    progress.show();
    QApplication::processEvents();

    bool upgradeOk = false;
    runOnWorkerThread([&]() {
        upgradeOk = dbMgr.upgradeSchema([&](int step, int stepCount, int from, int to) {
            setStatus(QObject::tr("Upgrading database from version %1 to version %2 (step %3 of %4)...")
                          .arg(from).arg(to).arg(step).arg(stepCount));
        });
    });
    progress.hide();

    if (upgradeOk) {
        upgradeOk = dbMgr.completeInitialization();
    }
    if (!upgradeOk) {
        qCritical() << "Main: Database schema upgrade failed";
        QString text = QObject::tr("OSCAR could not upgrade your database from version %1 to version %2.")
                           .arg(fromVersion).arg(toVersion) + "\n\n";
        if (!backupPath.isEmpty()) {
            text += QObject::tr("The copy made before the upgrade is unchanged:") + "\n"
                    + QDir::toNativeSeparators(backupPath) + "\n\n";
        }
        text += QObject::tr("Each completed step has been kept, and OSCAR will try the remaining steps the next time it starts. "
                            "If the problem persists, restore the database from a backup or delete oscar.db and re-import your CPAP data.")
                + "\n\n" + QObject::tr("OSCAR will now close.");
        QMessageBox::critical(nullptr, QObject::tr("Database Upgrade Failed"), text);
        return false;
    }

    qDebug() << "Main: Database schema upgraded from" << fromVersion << "to" << toVersion;
    return true;
}

#ifndef Q_OS_LINUX
// Due to a bug in Qt, creating multiple QApplication instances in a process
// causes subsequent native file dialog boxes to hang on Fedora 35.
// See https://bugreports.qt.io/browse/QTBUG-90616
//
// Since Linux users can simply use the --legacy command-line argument,
// we can remove the shift-key check that requires those multiple instances.
bool shiftKeyPressedAtLaunch(int argc, char *argv[])
{
    // Reliably detecting the shift key requires a QGuiApplication instance, but
    // we need to create the real QApplication afterwards, so create a temporary
    // instance here.
    QGuiApplication* app = new QGuiApplication(argc, argv);
    Qt::KeyboardModifiers keymodifier = QGuiApplication::queryKeyboardModifiers();
    delete app;
    return keymodifier == Qt::ShiftModifier;
}
#endif


void optionExit(int exitCode, QString error) {
    if (exitCode) {
        error.prepend("Command option error: ");
    }
    qCritical() << error << ( R"(
    Help Menu
    Option                   Description
    -p                       Pauses execution for 1 second
    --profile  <name>        Name of profile. if name does not exist then uses profile tab.
    --l or --language        Force language prompt
    --datadir  <folderName>  Use folderName as Oscar Data folder. For relatve paths: <Documents folder>/<relative path>.
                             If folder does not exist then prompts user.
    --legacy                 Use software graphics engine
    --OpenGL                 Use OpenGL graphics engine (case-insensitive)
    --help                   Displays this menu and exits.
    )" );
    exit (exitCode);
}

#ifdef Q_OS_WIN
/*! \class GraphicsCrashSentinelGuard
    \brief Scope guard around the part of startup that uses OpenGL.

    Arming on construction and disarming on destruction means every normal exit from
    main() clears the sentinel, including the early returns taken when the user cancels
    a startup dialog, while a crash - which never runs destructors - leaves it set for
    the next launch to find. MainWindow::Startup() disarms it explicitly as soon as the
    window has been drawn, so a crash later in the session is not blamed on graphics.
    */
class GraphicsCrashSentinelGuard
{
  public:
    GraphicsCrashSentinelGuard() { armGraphicsCrashSentinel(); }
    ~GraphicsCrashSentinelGuard() { disarmGraphicsCrashSentinel(); }
};
#endif

////////////////////////////////////////////////////////////////////////////////////////////
// Main()
////////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char *argv[]) {
    QString homeDocs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)+"/";
    QCoreApplication::setApplicationName(getAppName() + " 2.0"); // add major version so that QSettings separates this from prior version.
    QCoreApplication::setOrganizationName(getDeveloperName());
    QCoreApplication::setOrganizationDomain(getDeveloperDomain());
//    QGuiApplication::styleHints()->colorScheme();  // Copies OS light or dark style to OSCAR,
                                                     // but supporting dark mode would require an exhaustive change to OSCAR
//    QApplication::setStyle("Fusion");
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor
        );

    QSettings settings;

    ////////////////////////////////////////////////////////////////////////////////////////////
    // Handle graphics mode change, including change after crash
    ////////////////////////////////////////////////////////////////////////////////////////////
    // If shift key was held down when OSCAR was launched, toggle the graphics engine.
    QString forcedEngine = "";
#ifndef Q_OS_LINUX
    // Shift key check is skipped on Linux due to a Qt bug, see comment at shiftKeyPressedAtLaunch().
    if (shiftKeyPressedAtLaunch(argc, argv)){
        GFXEngine currentEngine = (GFXEngine)settings.value(GFXEngineSetting, (unsigned int)GFX_OpenGL).toUInt();
        if (currentEngine == GFX_Software) {
            settings.setValue(GFXEngineSetting, (unsigned int)GFX_OpenGL);
            forcedEngine = "OpenGL Engine forced by shift key at launch (toggled from Software)";
        } else {
            settings.setValue(GFXEngineSetting, (unsigned int)GFX_Software);
            forcedEngine = "Software Engine forced by shift key at launch (toggled from OpenGL)";
        }
    }
#endif
    // This argument needs to be processed before creating the QApplication,
    // based on sample code at https://doc.qt.io/qt-5/qapplication.html#details
    for (int i = 1; i < argc; ++i) {
        if (!qstricmp(argv[i], "--legacy")) {
            settings.setValue(GFXEngineSetting, (unsigned int)GFX_Software);
            forcedEngine = "Software Engine forced by --legacy command line switch";
        } else if (!qstricmp(argv[i], "--OpenGL")) {
            settings.setValue(GFXEngineSetting, (unsigned int)GFX_OpenGL);
            forcedEngine = "OpenGL Engine forced by --OpenGL command line switch";
        }
    }
#ifdef Q_OS_WIN
    // A sentinel left over from the previous run means OSCAR died while it was setting
    // up or drawing its window, which on Windows is nearly always the graphics engine.
    bool oscarCrashed = false;
    if (settings.value(GFXCrashSentinelSetting).toBool()) {
        oscarCrashed = true;
    }
    if (oscarCrashed) {
        settings.setValue(GFXEngineSetting, (unsigned int)GFX_Software);
        forcedEngine = "Software Engine forced by previous crash";
        settings.remove(GFXCrashSentinelSetting);
        settings.sync();
    }
#endif

    GFXEngine gfxEngine = (GFXEngine)qMin((unsigned int)settings.value(GFXEngineSetting, (unsigned int)GFX_OpenGL).toUInt(), (unsigned int)MaxGFXEngine);
    switch (gfxEngine) {
    case GFX_OpenGL:
        QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
        // Share a single OpenGL context across all QOpenGLWidget instances
        // (Daily, Overview, snapshot and undocked graph views). Qt5's QGLWidget
        // shared-context constructor argument was dropped in the Qt6 port, leaving
        // each gGraphView with its own context; on macOS this forces every view
        // after the first onto a slow per-frame read-back path, causing jerky
        // scrolling. This is the Qt6-native equivalent of the old shared context.
        QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
        break;
    case GFX_Software:
    default:
        QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
    }

    QApplication mainapp(argc, argv);
    // The application name above keeps its historical " 2.0" because it keys the QSettings
    // store and AppDataLocation; changing it would orphan existing settings. The name users
    // see (e.g. appended to window titles on Linux) covers every 2.x release.
    QGuiApplication::setApplicationDisplayName(getAppName() + " 2");
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    mainapp.styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
    QStringList args = mainapp.arguments();

    // Everything shown before the preferences are loaded (language choice, data folder
    // setup, startup warnings) would otherwise use the platform default font, which on
    // Windows is a point smaller than OSCAR's own default. Use OSCAR's default size now;
    // setApplicationFont() applies the user's preference once it is available.
    {
        QFont startupFont = QApplication::font();
        if (startupFont.pointSize() < DefaultApplicationFontSize) {
            startupFont.setPointSize(DefaultApplicationFontSize);
            QApplication::setFont(startupFont);
        }
    }

#ifdef Q_OS_WIN
    // QMessageBox must come after the application is created. The graphics engine has to be selected before.
    if (oscarCrashed) {
        QMessageBox::warning(nullptr, STR_MessageBox_Error,
                             QObject::tr("OSCAR crashed due to an incompatibility with your graphics hardware.") + "\n\n" +
                             QObject::tr("To resolve this, OSCAR has reverted to a slower but more compatible method of drawing."),
                             QMessageBox::Ok);
    }
#endif

    ////////////////////////////////////////////////////////////////////////////////////////////
    // Initialize logger
    ////////////////////////////////////////////////////////////////////////////////////////////
    // After initializing the logger, any qDebug() messages will be queued but not written to console
    // until MainWindow is constructed below. In spite of that, we initialize the logger here so that
    // the intervening messages show up in the debug pane.
    //
    // The only time this is really noticeable is when initTranslations() presents its language
    // selection QDialog, which waits indefinitely for user input before MainWindow is constructed.
    initializeLogger();

    ////////////////////////////////////////////////////////////////////////////////////////////
    // Handle command line options
    ////////////////////////////////////////////////////////////////////////////////////////////
    bool force_data_dir = false;
    QString load_profile; // null profile means no --profile param
    QString dataDirArg;   // resolved --datadir path, empty if not given
    // Data folder recorded before this launch, so a cancelled first-time setup can put it back.
    const QString appDataAtLaunch = settings.value("Settings/AppData").toString();
    for (int i = 1; i < args.size(); i++) {
        if ((args[i] == "--language") || (args[i] == "--l") ) {
            settings.setValue(LangSetting,"");
        } else if ( (args[i] == "-nop") || (args[i] == "") || (args[i].compare("--legacy", Qt::CaseInsensitive) == 0) || (args[i].compare("--OpenGL", Qt::CaseInsensitive) == 0) ){
            // do nothing. internal calls that current don't have any further functions in main.
        } else if (args[i] == "-p") {
            QThread::msleep(1000);
        } else if (args[i] == "--profile") {
            if ((i+1) < args.size())
                load_profile = args[++i];
            else {
                // Just view all profiles.
                load_profile = " ";
            }
        } else if (args[i] == "--datadir") { // mltam's idea
            QString datadir, datadirwas ;
            if ((i+1) < args.size()) {
                datadirwas = datadir = args[++i];
                bool havefullpath = false;
                if (datadir.length() >= 2) {
                    havefullpath =    (datadir.at(1) == QLatin1Char(':')) // Allow a Windows drive letter
                                   || (datadir.at(0) == '/')              // or Linux full path
                                   || (datadir.at(0) == '\\');
                }
                if (!havefullpath) {
                    datadir = homeDocs+datadir;
                    qDebug() << "--datadir was:" << datadirwas << "; --datadir is:" << datadir;
                }
                // In memory only for now: the folder is recorded in settings once it is
                // known to exist (or has been created), so that cancelling the setup
                // dialogs for a missing folder leaves the previously recorded one in place.
                dataDirArg = datadir;
                SetAppData(datadir, false);
//            force_data_dir = true;
            } else {
                optionExit(2,"Missing argument to --datadir\n");
            }
        } else if (QString(args[i]).contains("help",Qt::CaseInsensitive)) {
            optionExit(0,QString(""));
        } else {
            optionExit(3,QString("Invalid Argument: '%1'").arg(args[i]));
        }

    }   // end of for args loop

    qDebug().noquote() << "OSCAR starting" << QDateTime::currentDateTime().toString();
    qDebug() << "APP-NAME:" << QCoreApplication::applicationName();
    qDebug() << "APP-PATH:" << QCoreApplication::applicationDirPath();
    qDebug() << "APP-RESOURCES:" << appResourcePath();


#ifdef QT_DEBUG
    QString relinfo = " debug";
#else
    QString relinfo = "";
#endif
    relinfo = "("+QSysInfo::kernelType()+" "+QSysInfo::currentCpuArchitecture()+relinfo+")";
    relinfo = STR_AppName + " " + getVersion() + " " + relinfo;

    qDebug().noquote() << relinfo;
    qDebug().noquote() << "Built with Qt" << QT_VERSION_STR << "on" << getBuildDateTime();
    addBuildInfo(relinfo);  // immediately add it to the build info that's accessible from the UI

    SetDateFormat();

    ////////////////////////////////////////////////////////////////////////////////////////////
    // Language Selection
    ////////////////////////////////////////////////////////////////////////////////////////////
    initTranslations();

    initializeStrings(); // This must be called AFTER translator is installed, but before mainwindow is setup

#ifdef Q_OS_WIN
    // Arm the sentinel before the first use of OpenGL. Everything from here until the
    // window has been drawn touches it: the version probe below, prepOpenGL() in
    // MainWindow::SetupGUI(), and TestWindowsOpenGL() at the top of Startup(), which
    // disarms it again. If any of that kills the process, the next launch finds the flag
    // and reverts to the software engine; without this the crash simply repeats on every
    // launch, leaving the user no way in (GitLab #277).
    GraphicsCrashSentinelGuard graphicsCrashSentinel;
#endif

    mainwin = new MainWindow;

// Moved buildInfo calls to after translation is available as makeBuildInfo includes tr() calls

    QStringList info = makeBuildInfo(forcedEngine);
    for (int i = 0; i < info.size(); ++i) {
        qDebug().noquote() << info.at(i);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    // OpenGL Detection
    ////////////////////////////////////////////////////////////////////////////////////////////
    float glVersion = getOpenGLVersion();
    qDebug().noquote() << "OpenGL version:" << getOpenGLVersionString();

#ifdef BROKEN_OPENGL_BUILD
    // This build draws its graphs with plain QWidgets, so the OpenGL version is moot.
    Q_UNUSED(glVersion)
#elif defined(Q_OS_WIN)
    // OSCAR's graphs need OpenGL 2.0. A stub 1.1 driver, or no usable driver at all,
    // cannot back a QOpenGLWidget: creating one crashes rather than failing cleanly, so
    // switch engines before any widget is built. The engine is chosen by an application
    // attribute that can only be set before QApplication is constructed, hence the
    // restart. Only done while OpenGL is the selected engine, so it can happen once.
    if ((glVersion < 2.0f) && (currentGFXEngine() == GFX_OpenGL)) {
        qWarning().noquote() << "Insufficient OpenGL support (" + getOpenGLVersionString()
                                + "); switching to the software graphics engine";
        setCurrentGFXEngine(GFX_Software);
        QMessageBox::warning(nullptr, STR_MessageBox_Warning,
                             QObject::tr("Your graphics hardware does not support the version of OpenGL that OSCAR needs.") + "\n\n" +
                             QObject::tr("OSCAR will restart using a slower but more compatible method of drawing."),
                             QMessageBox::Ok);
        // Carry the original command line over so --datadir and --profile survive the
        // restart, but drop --OpenGL: it would put the engine back and restart us again.
        QStringList restartArgs;
        restartArgs << "-p";        // gives this process time to exit first
        for (int i = 1; i < args.size(); i++) {
            if ((args[i] != "-p") && (args[i].compare("--OpenGL", Qt::CaseInsensitive) != 0))
                restartArgs << args[i];
        }
        if (!QProcess::startDetached(QApplication::applicationFilePath(), restartArgs)) {
            QMessageBox::warning(nullptr, STR_MessageBox_Warning,
                                 QObject::tr("OSCAR was unable to restart itself. Please start OSCAR again."),
                                 QMessageBox::Ok);
        }
        return 0;
    }
#else
    if (glVersion < 2.0f)
        qWarning().noquote() << "Insufficient OpenGL support (" + getOpenGLVersionString() + ")";
#endif

    ////////////////////////////////////////////////////////////////////////////////////////////
    // Datafolder location Selection
    ////////////////////////////////////////////////////////////////////////////////////////////
    bool haveNewFolder = false;

    // GetAppData() is now --datadir if given, else the folder recorded in settings, else
    // (first run) a fallback that is never used: the setup dialog decides. Nothing has been
    // written to settings yet: that happens only once a folder is confirmed below, so
    // cancelling here leaves the previously recorded folder — or a genuine first-run
    // state — untouched.
    const bool firstRun = dataDirArg.isEmpty() && !settings.contains("Settings/AppData");
    if (firstRun)
        qDebug() << "First time: no data folder recorded";

    // A recorded folder that still exists and holds files, but no oscar.db and no OSCAR 1.x
    // data, has lost its database (moved, deleted, or removed by a cloud sync or clean-up
    // tool). Opening it as it is would make DatabaseManager create a new, empty oscar.db
    // without a word, and the user's data would appear to have vanished (GitLab #316).
    // An empty folder is not a loss: it is a new data folder and carries on as before.
    const bool databaseMissing = !firstRun && classifyDataFolder(GetAppData()) == DataFolderStatus::Other;
    if (databaseMissing)
        qWarning().noquote() << "Data folder" << GetAppData() << "exists but has no oscar.db";

    if (!force_data_dir && (firstRun || databaseMissing || !QDir(GetAppData()).exists())) {
        // Either a first run, or the recorded folder or its database has gone (moved,
        // deleted, drive or cloud mirror not mounted). Creating a folder and finding one
        // are separate paths: the single "choose or create" picker used before led many
        // users to hand back the intended parent folder, putting the database in Documents.
        const QString missingPath = firstRun ? QString() : QDir::cleanPath(GetAppData());
        // The setup dialog always suggests the default folder name; the location is where
        // the missing folder used to be, or Documents on a first run.
        const QString suggestedLocation = missingPath.isEmpty() ? QDir::cleanPath(homeDocs)
                                                                : QFileInfo(missingPath).absolutePath();
        QString chosen;

        while (chosen.isEmpty()) {
            const StartupChoice choice = databaseMissing ? showDatabaseMissingChoice(missingPath)
                                                         : showStartupChoice(missingPath);

            if (choice == StartupChoice::Cancel) {
                QMessageBox::information(nullptr, QObject::tr("Exiting"),
                                         (databaseMissing ? QObject::tr("OSCAR will exit without changing your data folder.")
                                                          : QObject::tr("As you did not select a data folder, OSCAR will exit.")) + "\n" +
                                         QObject::tr("Next time you run OSCAR, you will be asked again."));
                return 0;
            }

            if (choice == StartupChoice::UseThisFolder) {
                // The user has chosen, knowingly, to start again with an empty database here.
                chosen = missingPath;
                qWarning().noquote() << "User chose to create a new empty database in" << chosen;
                continue;
            }

            if (choice == StartupChoice::FirstUse) {
                DataFolderCreateDialog dlg(DataFolderCreateDialog::Mode::Startup,
                                           suggestedLocation, STR_AppData);
                if (dlg.exec() == QDialog::Accepted)
                    chosen = dlg.folderPath();
                continue;                       // Cancel returns to the startup choice
            }

            // StartupChoice::FindExisting — locate an existing OSCAR 2 data folder.
            QString startDir = missingPath.isEmpty() ? homeDocs : QFileInfo(missingPath).absolutePath();
            for (;;) {
                QString datadir = QFileDialog::getExistingDirectory(nullptr,
                                      QObject::tr("Locate your OSCAR 2 data folder"), startDir,
                                      QFileDialog::ShowDirsOnly | nativeDialogOption());
                if (datadir.isEmpty())
                    break;                      // Cancel returns to the startup choice

                const DataFolderStatus status = classifyDataFolder(datadir);
                if (status == DataFolderStatus::Oscar2) {
                    chosen = datadir;
                    break;
                }
                QString why = (status == DataFolderStatus::Oscar1)
                                  ? QObject::tr("This folder contains OSCAR 1.x data, not OSCAR 2 data.")
                                  : QObject::tr("This folder does not contain an OSCAR 2 database.");
                QMessageBox::warning(nullptr, QObject::tr("Not an OSCAR 2 data folder"),
                                     why + "\n\n" + QDir::toNativeSeparators(datadir) + "\n\n" +
                                     QObject::tr("Choose another folder, or Cancel to return to the previous screen."));
                startDir = datadir;
            }
        }

        SetAppData(chosen);
        qDebug() << "Data folder selected:" << chosen;
    }
    else {
        qDebug() << "AppData folder already exists, so ...";
        if (!dataDirArg.isEmpty())
            SetAppData(dataDirArg);         // --datadir folder exists: record it for next time
    }
    qDebug().noquote() << "Using " + GetAppData() + " as OSCAR data folder";

    QString path = GetAppData();
    addBuildInfo(QObject::tr("Data directory:") + " <a href=\"file:///" + path + "\">" + path + "</a>");
    addBuildInfo(QObject::tr("Database schema:") + " v" + QString::number(DatabaseSchema::CURRENT_SCHEMA_VERSION));

    QDir newDir(GetAppData());
    
    // Make sure the data directory exists.
    if (!newDir.mkpath(".")) {
        QMessageBox::warning(nullptr, QObject::tr("Exiting"),
                             QObject::tr("Unable to create the OSCAR data folder at")+"\n"+
                             GetAppData());
        return 0;
    }

    if (newDir.isEmpty())
        haveNewFolder = true;

    // Make sure we can write to the data directory
    QFile testFile(GetAppData()+"/testfile.txt");
    if (testFile.exists())
        testFile.remove();
    if (!testFile.open(QFile::ReadWrite)) {
        QString errMsg = QObject::tr("Unable to write to OSCAR data directory") + " " + GetAppData() + "\n\n" +
                         QObject::tr("Error code") + ": " + QString::number(testFile.error()) + " - " + testFile.errorString() + "\n\n" +
                         QObject::tr("OSCAR cannot continue and is exiting.") + "\n";
        qCritical() << errMsg;
        QMessageBox::critical(nullptr, QObject::tr("Exiting"), errMsg);
        return 0;
    }
    else
        testFile.remove();

    // Prevent two OSCAR instances from opening the same database folder at the same
    // time — concurrent access would cause SQLite write conflicts and profile corruption.
    QLockFile lockFile(GetAppData() + "/oscar.lock");
    lockFile.setStaleLockTime(0);  // never treat a lock as stale
    if (!lockFile.tryLock()) {
        qint64 pid = 0;
        QString hostname, appname;
        lockFile.getLockInfo(&pid, &hostname, &appname);
        QString msg = QObject::tr("This OSCAR database folder is already open in another instance of OSCAR.") + "\n\n" +
                      QObject::tr("Folder:") + " " + QDir::toNativeSeparators(GetAppData()) + "\n\n" +
                      QObject::tr("Close the other OSCAR instance before opening this database.");
        if (pid > 0)
            msg += "\n\n" + QObject::tr("(Other instance process ID: %1)").arg(pid);
        QMessageBox::warning(nullptr, STR_MessageBox_Warning, msg);
        return 0;
    }

    // Begin logging to file now that there's a data folder.
    if (!logger->logToFile()) {
        QMessageBox::warning(nullptr, STR_MessageBox_Warning,
             QObject::tr("Unable to write to debug log. You can still use the debug pane (Help/Troubleshooting/Show Debug Pane) but the debug log will not be written to disk."));
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Initialize database (must be before preferences so Open() can route to DB)
    ///////////////////////////////////////////////////////////////////////////////////////////
    QString dbPath = GetAppData() + "/oscar.db";

    // Dirty-shutdown detection: set the flag false now (dirty) and restore it to
    // true only after a clean close. If OSCAR crashes, the flag stays false and we
    // run an integrity check on the next launch — but only on the same database that
    // was open when the crash occurred (to avoid false checks when switching databases).
    const QString DBCleanShutdownKey = "db/CleanShutdown";
    const QString DBLastPathKey      = "db/LastShutdownPath";
    bool    prevShutdownClean = settings.value(DBCleanShutdownKey, true).toBool();
    QString prevShutdownPath  = settings.value(DBLastPathKey).toString();
    settings.setValue(DBCleanShutdownKey, false);
    settings.setValue(DBLastPathKey, dbPath);
    settings.sync();

    // qApp as context: the schema upgrade runs on a worker thread and may emit
    // this (via checkQueryError), so the dialog must be queued to the GUI thread.
    QObject::connect(&DatabaseManager::instance(), &DatabaseManager::databaseError, qApp,
                     [](const QString& error) {
                         qCritical() << "Database Manager Error:" << error;
                         QMessageBox::critical(nullptr, STR_MessageBox_Error,
                                               QObject::tr("Database Error") + "\n\n" + error);
                     });

    if (!DatabaseManager::instance().initialize(dbPath)) {
        qCritical() << "Main: Database initialization failed";
        return 0;
    }

    // An older schema was found. The upgrade is irreversible, so ask first (and
    // offer a copy of the database); the user may exit instead, as with a loader
    // data-format change. Either way the connection is closed in order, so the
    // shutdown counts as clean and no integrity check runs on the next launch.
    const int pendingFrom = DatabaseManager::instance().pendingSchemaUpgradeFrom();
    if (pendingFrom > 0 && !confirmAndUpgradeDatabase(dbPath, pendingFrom)) {
        DatabaseManager::instance().close();
        DatabaseManager::markCleanShutdown(dbPath);
        return 0;
    }

    qDebug() << "Main: Database initialized successfully!";
    qDebug() << "Main: Database file:" << dbPath;

    // If the previous session ended unexpectedly on this same database, run a quick
    // integrity check. A progress dialog is shown because the check can take several
    // minutes on large databases.
    if (prevShutdownClean) {
        qDebug() << "Main: Clean shutdown flag OK — skipping integrity check";
    } else if (prevShutdownPath != dbPath) {
        qDebug() << "Main: Dirty shutdown flag set for a different database — skipping integrity check";
    } else {
        qDebug() << "Main: Previous session ended unexpectedly — running integrity check";

        QDialog integrityWait;
        integrityWait.setWindowTitle(QObject::tr("Check Database Integrity"));
        integrityWait.setWindowModality(Qt::ApplicationModal);
        integrityWait.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
        QVBoxLayout waitLayout(&integrityWait);
        QLabel waitLabel(QObject::tr("Checking database integrity, please wait..."), &integrityWait);
        waitLabel.setMargin(20);
        waitLayout.addWidget(&waitLabel);
        integrityWait.setMinimumWidth(400);
        integrityWait.adjustSize();
        integrityWait.setFixedSize(integrityWait.size());
        integrityWait.show();
        QApplication::processEvents();

        bool integrityOk = false;
        runOnWorkerThread([&integrityOk]() {
            integrityOk = DatabaseManager::instance().checkIntegrity();
        });

        integrityWait.hide();

        if (!integrityOk) {
            qCritical() << "Main: Database integrity check failed after unclean shutdown";
            QMessageBox::StandardButton btn = QMessageBox::critical(
                nullptr,
                QObject::tr("Database Integrity Warning"),
                QObject::tr(
                    "OSCAR detected that the previous session ended unexpectedly, "
                    "and the database integrity check found problems.\n\n"
                    "Some data may be missing or corrupted.\n\n"
                    "Recommended actions:\n"
                    "  • Restore entire database from a recent system backup\n"
                    "  • Restore each profile from a recent backup (File → Restore Profile)\n"
                    "  • Re-import data from your CPAP SD card(s)\n\n"
                    "For advanced recovery options, see the OSCAR documentation.\n\n"
                    "You may continue, but some data may be incomplete or incorrect."),
                QMessageBox::Ok | QMessageBox::Close,
                QMessageBox::Close);
            if (btn == QMessageBox::Close) {
                return 0;
            }
        } else {
            qDebug() << "Main: Database passed integrity check after unclean shutdown";
        }
    }

    // Always add the current database to the recent list so it appears when the
    // user switches to another database. add() promotes it to the top if already
    // present and is a no-op on duplicates, so this is safe on every launch.
    RecentDatabases::add(GetAppData());

    importLegacyNamedLayouts();
    importLegacyProfileLayouts();

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Initialize preferences system (Don't use p_pref before this point!)
    ///////////////////////////////////////////////////////////////////////////////////////////
    p_pref = new Preferences("Preferences");
    p_pref->Open();
    AppSetting = new AppWideSetting(p_pref);

    // Apply Fusion theme if enabled in preferences
    if (AppSetting->useFusionTheme()) {
        QApplication::setStyle(QStyleFactory::create("Fusion"));
    }

    QString language = settings.value(LangSetting, "").toString();
    AppSetting->setLanguage(language);
    if (!load_profile.isEmpty()) AppSetting->setProfileName(load_profile);

    // Set fonts from preferences file
    qDebug() << "Main: App font before Prefs setting" << QApplication::font();
    validateAllFonts();
    setApplicationFont();

    // one-time translate GraphSnapshots to ShowPieChart
    p_pref->Rename(STR_AS_GraphSnapshots, STR_AS_ShowPieChart);

    p_pref->Erase(STR_AppName);
    p_pref->Erase(STR_GEN_SkipLogin);

    ////////////////////////////////////////////////////////////////////////////////////////////
    // Register Importer Modules for autoscanner
    //
    // This MUST run before the OSCAR 1.x migration below. The importer reads the legacy
    // channels.dat and resolves each entry against schema::channel, which is populated only
    // here by schema::init() and each loader's initChannels(). When registration ran after
    // migration the schema was still empty during import, so every channel customization
    // (colors, labels, thresholds, enabled/overview state) was silently skipped (#140).
    ////////////////////////////////////////////////////////////////////////////////////////////
    schema::init();
    PRS1Loader::Register();
    ResmedLoader::Register();
    IntellipapLoader::Register();
    SleepStyleLoader::Register();
    FPIconLoader::Register();
    WeinmannLoader::Register();
    CMS50Loader::Register();
    CMS50F37Loader::Register();
    MD300W1Loader::Register();
    ViatomLoader::Register();
    PrismaLoader::Register();
    ResventLoader::Register();
    BmcLoader::Register();
    BmcG3xLoader::Register();
    VREMLoader::Register();
    YuwellLoader::Register();
    SefamLoader::Register();
    ApexLoader::Register();
    AppleHealthLoader::Register();

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Migrate from OSCAR 1.x if needed
    ///////////////////////////////////////////////////////////////////////////////////////////
    if (haveNewFolder)
    {
        const MigrationChoice choice = showMigrationChoice();

        if (choice == MigrationChoice::Cancel) {
            // The user has backed out of setting up this folder. Release everything that
            // holds a file open inside it, remove it, and point settings back at whatever
            // was in use before, so the next launch does not come straight back here.
            const QString newFolder = GetAppData();
            qDebug() << "Setup cancelled at the migration step; removing new data folder" << newFolder;
            DatabaseManager::instance().close();
            lockFile.unlock();
            shutdownLogger();                   // closes the debug log inside the folder
            if (!QDir(newFolder).removeRecursively())
                qWarning() << "Unable to remove" << newFolder;
            RecentDatabases::remove(newFolder);

            // Prefer the folder recorded before this launch (after a Database ▸ New relaunch
            // that is the database the user came from). If nothing was recorded, or it was
            // this same folder, fall back to the most recently used remaining database, and
            // to a first run if there is none.
            QString restore = appDataAtLaunch;
            if (restore.isEmpty() || QDir::cleanPath(restore).compare(QDir::cleanPath(newFolder), Qt::CaseInsensitive) == 0) {
                const QStringList recent = RecentDatabases::entries();
                restore = recent.isEmpty() ? QString() : recent.first();
            }
            if (restore.isEmpty())
                settings.remove("Settings/AppData");
            else
                settings.setValue("Settings/AppData", restore);
            // Put the dirty-shutdown flags back as they were for the previous database.
            settings.setValue(DBCleanShutdownKey, prevShutdownClean);
            settings.setValue(DBLastPathKey, prevShutdownPath);
            settings.sync();
            return 0;
        }

        if (choice == MigrationChoice::Migrate) {
            migrateFromOSCAR( GetAppData() );              // doesn't matter if no migration
            // Migrate any .shg files copied in by the 1.x importer
            importLegacyNamedLayouts();
            importLegacyProfileLayouts();
        }
    }

#ifndef NO_CHECKUPDATES
    ////////////////////////////////////////////////////////////////////////////////////////////
    // Check when last checked for updates..
    ////////////////////////////////////////////////////////////////////////////////////////////
    QDateTime lastchecked, today = QDateTime::currentDateTime();

    bool check_updates = false;

    if (!getVersion().IsReleaseVersion()) {
        // If test build, force update autocheck, no more than 7 day interval, show test versions
        AppSetting->setUpdatesAutoCheck(true);
        AppSetting->setUpdateCheckFrequency(min(AppSetting->updateCheckFrequency(), 7));
        AppSetting->setAllowEarlyUpdates(true);
    }

    if (AppSetting->updatesAutoCheck()) {
        int update_frequency = AppSetting->updateCheckFrequency();
        int days = 1000;
        lastchecked = AppSetting->updatesLastChecked();

        if (lastchecked.isValid()) {
            days = lastchecked.secsTo(today);
            days /= 86400;
        }

        if (days >= update_frequency) {
            check_updates = true;
        }
    }
#endif

    Version settingsVersion = Version(AppSetting->versionString());
    Version currentVersion = getVersion();
    if (currentVersion.IsValid() == false) {
        // The defined version MUST be valid, otherwise comparisons between versions will fail.
        QMessageBox::critical(nullptr, STR_MessageBox_Error, QObject::tr("Version \"%1\" is invalid, cannot continue!").arg(currentVersion));
        return 0;
    }
    if (currentVersion > settingsVersion) {
        AppSetting->setShowAboutDialog(1);
//      release_notes();
//      check_updates = false;
    } else if (currentVersion < settingsVersion) {
        if (QMessageBox::warning(nullptr, STR_MessageBox_Error,
                                 QObject::tr("The version of OSCAR you are running (%1) is OLDER than the one used to create this data (%2).")
                                    .arg(currentVersion.displayString())
                                    .arg(settingsVersion.displayString())
                                 +"\n\n"+
                                 QObject::tr("It is likely that doing this will cause data corruption, are you sure you want to do this?"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::No) {

            return 0;
        }
    }

    AppSetting->setVersionString(getVersion());

    // Begin logging device connection activity.
    QString connectionsLogDir = GetLogDir() + "/connections";
    rotateLogs(connectionsLogDir);  // keep a limited set of previous logs
    if (!QDir(connectionsLogDir).mkpath(".")) {
        qWarning().noquote() << "Main: Unable to create directory" << connectionsLogDir;
    }

    QFile deviceLog(connectionsLogDir + "/devices.xml");
    if (deviceLog.open(QFile::ReadWrite)) {
        qDebug().noquote() << "Main: Logging device connections to" << deviceLog.fileName();
        DeviceConnectionManager::getInstance().record(&deviceLog);
    } else {
        qWarning().noquote() << "Main: Unable to start device connection logging to" << deviceLog.fileName();
    }

    schema::setOrders(); // could be called in init...

    // Scan for user profiles
    SpeedCheck sc(500, "Scanning for profiles");
    Profiles::Scan();
    sc.check();

#ifndef NO_CHECKUPDATES
    if (check_updates) {
        mainwin->CheckForUpdates(false);
    }
#endif

    sc.restart(400,"SetupGUI");
    mainwin->SetupGUI();
    sc.check();
    mainwin->show();

    int result = mainapp.exec();

    // Release the lock immediately so a restarted instance (e.g. after a language change)
    // can acquire it without racing against our potentially-slow widget/DB cleanup below.
    lockFile.unlock();

    DeviceConnectionManager::getInstance().record(nullptr);

    // Un-parent the machine loaders from the main window before deleting it.
    // Loaders are parented to MainWindow in Startup() but are owned by the global
    // m_loaders list and freed once by Profiles::Done() → DestroyLoaders() below.
    // closeEvent() normally un-parents them, but RestartApplication() exits via
    // QApplication::exit() and bypasses closeEvent(); without this, "delete mainwin"
    // would delete the still-parented loaders as child objects and DestroyLoaders()
    // would then delete them again — a double-free crashing on shutdown/restart.
    // Doing it here, at the sole point where MainWindow is destroyed, covers every
    // exit path.
    for (auto & loader : GetLoaders()) {
        loader->setParent(nullptr);
    }

    // Delete the main window explicitly while Qt is still fully alive.
    // closeEvent() has already run for a normal quit (closing the profile, saving
    // window geometry, and shutting down the logger) but the widget tree is still
    // alive. Deleting it here — while globals like AppSetting and the graph fonts
    // are still valid — ensures widget destructors don't access freed memory, which
    // would corrupt state and crash QApplication's teardown.
    delete mainwin;
    mainwin = nullptr;

    // Now that all widgets are gone, free the global objects that widget code
    // may have referenced (AppSetting, p_pref, loaders, graph fonts/images).
    // These were previously freed in closeEvent, but that ran before widget
    // destruction, causing use-after-free during QApplication::~QApplication.
    Profiles::Done();
    DestroyGraphGlobals();

    // Close the database explicitly while Qt is still alive.
    // DatabaseManager is a Meyer's singleton whose destructor fires after main() returns,
    // by which point libQt6Sql has already been torn down on some platforms (e.g. Ubuntu 24),
    // causing a SIGSEGV when the destructor tries to run "PRAGMA optimize" via QSqlQuery.
    // Closing here sets m_initialized=false so the destructor becomes a no-op.
    DatabaseManager::instance().close();

    // Mark clean shutdown so the next launch skips the integrity check.
    DatabaseManager::markCleanShutdown(dbPath);

    return result;
}

#endif // !UNITTEST_MODE
