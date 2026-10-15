/* Data folder setup dialogs — choosing, creating and validating the OSCAR data folder
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef DATAFOLDERDIALOG_H
#define DATAFOLDERDIALOG_H

#include <QDialog>
#include <QString>

class QLineEdit;
class QLabel;
class QPushButton;

/*!
 * \file datafolderdialog.h
 * \brief Dialogs and helpers used when OSCAR needs a data folder.
 *
 * OSCAR needs a data folder at startup (first run, or when the folder it last used has
 * disappeared) and when the user asks for a new database from the File menu. Earlier
 * versions handed the user a single directory picker titled "Choose or create a folder",
 * and many users returned the intended *parent* folder without creating anything inside
 * it, so OSCAR's database landed in their Documents folder. These dialogs separate
 * *creating* a new folder (OSCAR creates it from a name and a location) from *finding*
 * an existing one, and validate the result before it is used.
 */

/*! \brief What a candidate data folder currently contains. */
enum class DataFolderStatus {
    Missing,    //!< The path does not exist.
    Empty,      //!< The folder exists and contains nothing.
    Oscar2,     //!< The folder holds an OSCAR 2 database (oscar.db).
    Oscar1,     //!< The folder holds OSCAR 1.x data (Preferences.xml and Profiles/, no oscar.db).
    Other       //!< The folder exists and holds unrelated files.
};

/*! \brief Classify \a path as one of the DataFolderStatus values. */
DataFolderStatus classifyDataFolder(const QString& path);

/*!
 * \brief Check whether \a name is usable as a data folder name on every platform OSCAR runs on.
 * \return An empty string if the name is acceptable, otherwise a translated explanation.
 *
 * Applies the Windows rules everywhere (reserved characters and names, no trailing space or
 * period) because a data folder may later be copied to, or synchronised with, a Windows PC.
 */
QString dataFolderNameProblem(const QString& name);

/*!
 * \brief Heuristic test for a location managed by a cloud synchronisation service.
 * \return The name of the service the path appears to belong to, or an empty string.
 *
 * Cloud clients can lock or replace the SQLite database while OSCAR has it open, so
 * callers warn (but do not refuse) when this returns a non-empty string.
 */
QString cloudServiceForPath(const QString& path);

/*! \brief Outcome of the startup choice dialogs. */
enum class StartupChoice {
    FirstUse,       //!< Create a new data folder.
    FindExisting,   //!< Locate an existing OSCAR 2 data folder.
    UseThisFolder,  //!< Keep the recorded data folder and start a new, empty database in it.
    Cancel          //!< Exit OSCAR.
};

/*!
 * \brief Show the "OSCAR 2 Startup" dialog offering to create or find a data folder.
 * \param missingPath The folder OSCAR expected to find, or empty on a genuine first run.
 *                    When given it is shown so the user knows what OSCAR was looking for.
 */
StartupChoice showStartupChoice(const QString& missingPath);

/*!
 * \brief Show the "OSCAR Database Not Found" dialog for a data folder that has lost its oscar.db.
 * \param folder The recorded data folder: it exists and holds files, but no oscar.db.
 * \return FindExisting, UseThisFolder (start an empty database in \a folder) or Cancel.
 *
 * Opening the folder as it is would make the database layer create a new, empty oscar.db
 * there without a word, so the user's data would appear to have vanished. This asks first
 * and explains how to put a backup copy back (GitLab #316).
 */
StartupChoice showDatabaseMissingChoice(const QString& folder);

/*! \brief Outcome of the migration choice dialog. */
enum class MigrationChoice {
    Migrate,    //!< Select an OSCAR 1.x folder and migrate its profiles.
    Skip,       //!< Continue with the new, empty data folder.
    Cancel      //!< Abandon setup: the new folder is removed and OSCAR exits.
};

/*! \brief Show the "OSCAR 1.x Data Migration" dialog offered once a new data folder exists. */
MigrationChoice showMigrationChoice();

/*!
 * \class DataFolderCreateDialog
 * \brief Dialog that creates a new OSCAR data folder from a folder name and a location.
 *
 * The user edits the folder name and the parent location separately; OSCAR joins them and
 * creates the folder itself, so the user never has to create a folder inside a file picker.
 * Before creating, the dialog rejects names that are not valid folder names, locations that
 * do not exist, and targets that already hold OSCAR 1.x data, an OSCAR 2 database, or any
 * other files. It warns when the location looks cloud-managed, then creates the folder and
 * confirms it is writable. On acceptance folderPath() returns the created folder.
 */
class DataFolderCreateDialog : public QDialog
{
    Q_OBJECT
public:
    /*! \brief Which explanatory text and button captions the dialog shows. */
    enum class Mode {
        Startup,        //!< First-run setup, including the OSCAR 1.x migration note.
        NewDatabase     //!< File ▸ Database ▸ New from a running OSCAR.
    };

    /*!
     * \param mode      Startup or NewDatabase wording.
     * \param location  Initial parent location (an existing directory).
     * \param name      Initial folder name; may be empty to force the user to choose one.
     * \param parent    Parent widget, or nullptr before the main window is shown.
     */
    DataFolderCreateDialog(Mode mode, const QString& location, const QString& name,
                           QWidget* parent = nullptr);

    /*! \brief Absolute path of the folder created; valid only after exec() returned Accepted. */
    QString folderPath() const { return m_folderPath; }

private slots:
    void onBrowse();            //!< Directory picker for the location field.
    void onInputChanged();      //!< Live validation of the name and location fields.
    void onCreate();            //!< Validate the target, create it, and accept().

private:
    /*! \brief Location and name joined into a cleaned absolute path. */
    QString targetPath() const;

    /*! \brief Show a Create-time problem and keep the dialog open. */
    void rejectTarget(const QString& message);

    Mode         m_mode;
    QLineEdit*   m_nameEdit;
    QLineEdit*   m_locationEdit;
    QLabel*      m_statusLabel;
    QPushButton* m_createButton;
    QString      m_folderPath;
};

#endif // DATAFOLDERDIALOG_H
