/* Data folder setup dialogs — choosing, creating and validating the OSCAR data folder
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "datafolderdialog.h"
#include "SleepLib/common.h"
#include "translation.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVBoxLayout>

////////////////////////////////////////////////////////////////////////////////////////////
// Folder classification and validation helpers
////////////////////////////////////////////////////////////////////////////////////////////

DataFolderStatus classifyDataFolder(const QString& path)
{
    QFileInfo info(path);
    if (!info.exists())
        return DataFolderStatus::Missing;
    if (!info.isDir())
        return DataFolderStatus::Other;     // a file is in the way

    QDir dir(path);
    if (QFileInfo::exists(dir.filePath("oscar.db")))
        return DataFolderStatus::Oscar2;
    if (QFileInfo::exists(dir.filePath("Preferences.xml")) && QFileInfo(dir.filePath("Profiles")).isDir())
        return DataFolderStatus::Oscar1;
    // isEmpty() ignores hidden entries, so a folder holding only desktop.ini or .DS_Store
    // still counts as empty — the same view main() takes when deciding to offer migration.
    if (dir.isEmpty())
        return DataFolderStatus::Empty;
    return DataFolderStatus::Other;
}

QString dataFolderNameProblem(const QString& name)
{
    if (name.isEmpty())
        return QObject::tr("Enter a name for the data folder.");
    if (name != name.trimmed())
        return QObject::tr("A folder name cannot begin or end with a space.");
    if (name == "." || name == "..")
        return QObject::tr("\"%1\" is not a valid folder name.").arg(name);
    if (name.endsWith('.'))
        return QObject::tr("A folder name cannot end with a period.");

    static const QString reservedChars = QStringLiteral("\\/:*?\"<>|");
    for (const QChar c : name) {
        if (reservedChars.contains(c) || c.category() == QChar::Other_Control)
            return QObject::tr("A folder name cannot contain any of these characters: %1")
                       .arg(QStringLiteral("\\ / : * ? \" < > |"));
    }

    // Windows device names are reserved with or without an extension (CON, con.txt, ...).
    static const QRegularExpression reservedNames(
        QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\..*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    if (reservedNames.match(name).hasMatch())
        return QObject::tr("\"%1\" is a name reserved by Windows and cannot be used.").arg(name);

    return QString();
}

QString cloudServiceForPath(const QString& path)
{
    // Path segments that sync clients conventionally use for their local mirror. Matched
    // per segment, case-insensitively, so "OneDrive - Contoso" is caught too.
    static const struct { const char* marker; const char* service; } markers[] = {
        { "OneDrive",         "OneDrive"      },
        { "Google Drive",     "Google Drive"  },
        { "GoogleDrive",      "Google Drive"  },
        { "Dropbox",          "Dropbox"       },
        { "iCloud",           "iCloud Drive"  },
        { "Mobile Documents", "iCloud Drive"  },   // ~/Library/Mobile Documents on macOS
    };

    const QStringList segments = QDir::cleanPath(path).split('/', Qt::SkipEmptyParts);
    for (const QString& segment : segments) {
        for (const auto& m : markers) {
            if (segment.contains(QLatin1String(m.marker), Qt::CaseInsensitive))
                return QLatin1String(m.service);
        }
    }
    return QString();
}

////////////////////////////////////////////////////////////////////////////////////////////
// Startup and migration choice dialogs
////////////////////////////////////////////////////////////////////////////////////////////

/*! \brief Margin (device-independent pixels) around the content of the setup dialogs. */
static const int SetupDialogMargin = 28;

/*! \brief Space between the blocks of a setup dialog. */
static const int SetupDialogSpacing = 16;

/*! \brief Minimum width of the setup dialogs; they can be resized wider. */
static const int SetupDialogWidth = 600;

/*!
 * \brief Common frame for the setup dialogs: window title, margins, and a heading line.
 *
 * The heading repeats the window title in bold, slightly larger than body text, so the
 * screen reads as a titled page rather than a bare message. The returned layout is the
 * dialog's main vertical layout; callers add their body below the heading.
 *
 * These are plain QDialogs rather than QMessageBoxes on purpose: once an application
 * stylesheet is set (MainWindow does this), Qt re-applies the platform theme's per-class
 * fonts, and on Windows that leaves QMessageBox text at the 9 pt "message box font"
 * while the rest of OSCAR is at 10 pt. QDialog, QLabel and QPushButton have no such
 * theme entry, so they follow the application font.
 */
static QVBoxLayout* makeSetupLayout(QDialog* dialog, const QString& heading)
{
    dialog->setWindowTitle(heading);
    dialog->setMinimumWidth(SetupDialogWidth);

    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(SetupDialogMargin, SetupDialogMargin, SetupDialogMargin, SetupDialogMargin);
    layout->setSpacing(SetupDialogSpacing);

    auto* headingLabel = new QLabel(heading, dialog);
    QFont headingFont = headingLabel->font();
    headingFont.setBold(true);
    if (headingFont.pointSize() > 0)
        headingFont.setPointSize(headingFont.pointSize() + 2);
    headingLabel->setFont(headingFont);
    headingLabel->setAlignment(Qt::AlignHCenter);
    layout->addWidget(headingLabel);
    return layout;
}

/*!
 * \brief Word-wrapped body text for a setup dialog.
 * \param format Qt::PlainText for prose; Qt::RichText when the caller has built HTML
 *               (bulleted lists need it for a hanging indent). Rich-text callers must
 *               pass translated strings through QString::toHtmlEscaped().
 */
static QLabel* makeBodyLabel(const QString& text, QWidget* parent, Qt::TextFormat format = Qt::PlainText)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(format);
    label->setWordWrap(true);
    return label;
}

/*!
 * \class SetupChoiceDialog
 * \brief A setup dialog with a heading, explanatory text and a row of choice buttons.
 *
 * Replaces QMessageBox for the startup and migration screens (see makeSetupLayout()).
 * \a defaultChoice is the button Enter activates and the one that starts with focus; the
 * buttons keep Qt's auto-default behaviour, so the default frame follows the focus as the
 * user tabs between them. The last choice is Cancel, which is also what Escape and
 * closing the window return.
 */
class SetupChoiceDialog : public QDialog
{
public:
    SetupChoiceDialog(const QString& heading, const QString& body, const QStringList& choices,
                      int defaultChoice = 0, Qt::TextFormat bodyFormat = Qt::PlainText,
                      QWidget* parent = nullptr)
        : QDialog(parent)
        , m_choice(choices.size() - 1)
    {
        QVBoxLayout* layout = makeSetupLayout(this, heading);
        layout->addWidget(makeBodyLabel(body, this, bodyFormat));
        layout->addStretch();

        auto* buttonRow = new QHBoxLayout();
        buttonRow->addStretch();
        for (int i = 0; i < choices.size(); i++) {
            auto* button = new QPushButton(choices.at(i), this);
            connect(button, &QPushButton::clicked, this, [this, i]() { m_choice = i; accept(); });
            buttonRow->addWidget(button);
            if (i == defaultChoice) {
                button->setDefault(true);
                button->setFocus();
            }
        }
        layout->addLayout(buttonRow);
    }

    /*! \brief Index of the choice taken; valid after exec(). */
    int choice() const { return m_choice; }

private:
    int m_choice;
};

StartupChoice showStartupChoice(const QString& missingPath)
{
    // Rich text so the two alternatives are real list items: indented, with wrapped lines
    // aligned under the text rather than the bullet, and "or" set between margin and bullet.
    const QString lead   = QObject::tr("You are seeing this message because either").toHtmlEscaped();
    const QString first  = (QObject::tr("This is the first time you have used OSCAR 2.") + " " +
                            QObject::tr("OSCAR will need to create a data folder for you.")).toHtmlEscaped();
    const QString second = (QObject::tr("OSCAR 2 could not find the OSCAR 2 data folder you last used.") + " " +
                            QObject::tr("You will need to help OSCAR find the OSCAR 2 data folder.")).toHtmlEscaped();
    const QString either = QObject::tr("or").toHtmlEscaped();

    QString text = "<p style=\"margin:0\">" + lead + "</p>"
                   "<ul style=\"margin-top:10px;margin-bottom:0\"><li>" + first + "</li></ul>"
                   "<p style=\"margin-top:6px;margin-bottom:6px;margin-left:0px\">" + either + "</p>"
                   "<ul style=\"margin-top:0;margin-bottom:0\"><li>" + second + "</li></ul>";
    if (!missingPath.isEmpty()) {
        text += "<p style=\"margin-top:14px;margin-bottom:0\">" + QObject::tr("OSCAR was looking for:").toHtmlEscaped() +
                " " + QDir::toNativeSeparators(missingPath).toHtmlEscaped() + "</p>";
    }

    // A genuine first run defaults to creating; a folder OSCAR remembers but cannot find
    // defaults to finding it.
    const int defaultChoice = missingPath.isEmpty() ? 0 : 1;
    SetupChoiceDialog dialog(QObject::tr("OSCAR 2 Startup"), text,
                             { QObject::tr("Create OSCAR 2 data folder"),
                               QObject::tr("Find my OSCAR 2 data folder"),
                               QObject::tr("Cancel") },
                             defaultChoice, Qt::RichText);
    dialog.exec();

    switch (dialog.choice()) {
    case 0:  return StartupChoice::FirstUse;
    case 1:  return StartupChoice::FindExisting;
    default: return StartupChoice::Cancel;
    }
}

StartupChoice showDatabaseMissingChoice(const QString& folder)
{
    const QString text =
        QObject::tr("OSCAR's data folder is still there, but the database file it contains (oscar.db) is missing:") +
        "\n\n" + QDir::toNativeSeparators(QDir(folder).filePath("oscar.db")) + "\n\n" +
        QObject::tr("The file may have been moved, renamed or deleted, or removed by a cloud synchronisation or disk clean-up tool.") +
        "\n\n" +
        QObject::tr("If you have a backup copy of oscar.db (for example in Time Machine, File History, or your cloud service's deleted files), "
                    "choose Exit, put the copy back in this folder, and start OSCAR again.") + "\n\n" +
        QObject::tr("If your data folder is somewhere else, choose Find.") + " " +
        QObject::tr("Choose Create only if you want to start again with an empty database; your CPAP data will then need to be imported again.");

    SetupChoiceDialog dialog(QObject::tr("OSCAR Database Not Found"), text,
                             { QObject::tr("Find my OSCAR 2 data folder"),
                               QObject::tr("Create new empty database"),
                               QObject::tr("Exit") });
    dialog.exec();

    switch (dialog.choice()) {
    case 0:  return StartupChoice::FindExisting;
    case 1:  return StartupChoice::UseThisFolder;
    default: return StartupChoice::Cancel;
    }
}

MigrationChoice showMigrationChoice()
{
    QString text = QObject::tr("OSCAR 2 uses a different data folder than OSCAR 1.x, so it cannot share the folder used by OSCAR 1.x.") + " " +
                   QObject::tr("OSCAR can migrate your OSCAR 1.x data into the new OSCAR 2 data folder.") + "\n\n" +
                   QObject::tr("Click Migrate to select the OSCAR 1.x folder that you wish to migrate to your new OSCAR 2 data folder.") + "\n\n" +
                   QObject::tr("Your old OSCAR 1.x data folder will not be changed, so you can still run OSCAR 1.x.") + "\n\n" +
                   QObject::tr("Cancel removes the new OSCAR 2 data folder and exits OSCAR.");

    SetupChoiceDialog dialog(QObject::tr("OSCAR 1.x Data Migration"), text,
                             { QObject::tr("Migrate"),
                               QObject::tr("Skip migration"),
                               QObject::tr("Cancel") });
    dialog.exec();

    switch (dialog.choice()) {
    case 0:  return MigrationChoice::Migrate;
    case 1:  return MigrationChoice::Skip;
    default: return MigrationChoice::Cancel;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////
// DataFolderCreateDialog
////////////////////////////////////////////////////////////////////////////////////////////

DataFolderCreateDialog::DataFolderCreateDialog(Mode mode, const QString& location,
                                               const QString& name, QWidget* parent)
    : QDialog(parent)
    , m_mode(mode)
{
    QVBoxLayout* layout = makeSetupLayout(this, mode == Mode::Startup ? tr("OSCAR 2 Initial Setup")
                                                                      : tr("New Database"));

    QString intro;
    if (mode == Mode::Startup) {
        intro = tr("OSCAR requires a folder to store data and control information.") + " " +
                tr("We recommend you accept the default name provided.") + "\n\n" +
                tr("OSCAR's data folder can be located anywhere on your computer, with the default being your Documents folder.") + " " +
                tr("We recommend you choose a location that is not managed by a cloud service such as Google Drive, OneDrive, and similar products.") + "\n\n" +
                tr("OSCAR 1.x and 2.x cannot use the same data folder as the data structures are different.") + " " +
                tr("If you have OSCAR 1.x data, you will be able to migrate it to OSCAR 2 in the next step.");
    } else {
        intro = tr("OSCAR will create a new database folder with an empty database.") + " " +
                tr("We recommend you choose a location that is not managed by a cloud service such as Google Drive, OneDrive, and similar products.") + "\n\n" +
                tr("OSCAR will restart using the new database.") + " " +
                tr("You can return to your current database with File ▸ Database ▸ Open or the Recent list.") + "\n\n" +
                tr("Name the data folder and select the folder location.");
    }
    layout->addWidget(makeBodyLabel(intro, this));

    auto* form = new QFormLayout();
    m_nameEdit = new QLineEdit(name, this);
    form->addRow(tr("Data folder name:"), m_nameEdit);

    auto* locationRow = new QHBoxLayout();
    m_locationEdit = new QLineEdit(QDir::toNativeSeparators(location), this);
    auto* browseButton = new QPushButton(tr("Browse..."), this);
    locationRow->addWidget(m_locationEdit, 1);
    locationRow->addWidget(browseButton);
    form->addRow(tr("Folder location:"), locationRow);
    layout->addLayout(form);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
    layout->addStretch();               // keeps the buttons at the bottom if resized taller

    auto* buttonRow = new QHBoxLayout();
    m_createButton = new QPushButton(mode == Mode::Startup ? tr("Create OSCAR folder")
                                                           : tr("Create new database"), this);
    m_createButton->setDefault(true);   // Enter in either field creates the folder
    auto* cancelButton = new QPushButton(tr("Cancel"), this);
    buttonRow->addStretch();
    buttonRow->addWidget(m_createButton);
    buttonRow->addWidget(cancelButton);
    layout->addLayout(buttonRow);

    connect(m_nameEdit,     &QLineEdit::textChanged,  this, &DataFolderCreateDialog::onInputChanged);
    connect(m_locationEdit, &QLineEdit::textChanged,  this, &DataFolderCreateDialog::onInputChanged);
    connect(browseButton,   &QPushButton::clicked,    this, &DataFolderCreateDialog::onBrowse);
    connect(m_createButton, &QPushButton::clicked,    this, &DataFolderCreateDialog::onCreate);
    connect(cancelButton,   &QPushButton::clicked,    this, &QDialog::reject);

    onInputChanged();
    if (name.isEmpty())
        m_nameEdit->setFocus();
}

QString DataFolderCreateDialog::targetPath() const
{
    const QString location = QDir::fromNativeSeparators(m_locationEdit->text().trimmed());
    return QDir::cleanPath(QDir(location).filePath(m_nameEdit->text()));
}

void DataFolderCreateDialog::onBrowse()
{
    QString start = QDir::fromNativeSeparators(m_locationEdit->text().trimmed());
    if (!QFileInfo(start).isDir())
        start = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);

    QString dir = QFileDialog::getExistingDirectory(this, tr("Choose the location for the OSCAR data folder"),
                                                    start, QFileDialog::ShowDirsOnly | nativeDialogOption());
    if (!dir.isEmpty())
        m_locationEdit->setText(QDir::toNativeSeparators(dir));
}

void DataFolderCreateDialog::onInputChanged()
{
    QString problem = dataFolderNameProblem(m_nameEdit->text());
    if (problem.isEmpty() && m_locationEdit->text().trimmed().isEmpty())
        problem = tr("Choose a location for the data folder.");

    if (!problem.isEmpty()) {
        m_statusLabel->setText(problem);
        m_createButton->setEnabled(false);
    } else {
        // Show exactly what will be created so there is no doubt where the data will live.
        m_statusLabel->setText(tr("OSCAR will create:") + " " + QDir::toNativeSeparators(targetPath()));
        m_createButton->setEnabled(true);
    }
}

void DataFolderCreateDialog::rejectTarget(const QString& message)
{
    QMessageBox::warning(this, windowTitle(), message);
}

void DataFolderCreateDialog::onCreate()
{
    const QString location = QDir::fromNativeSeparators(m_locationEdit->text().trimmed());
    QFileInfo locationInfo(location);
    if (location.isEmpty() || !locationInfo.isAbsolute() || !locationInfo.isDir()) {
        rejectTarget(tr("The folder location does not exist.") + "\n\n" +
                     QDir::toNativeSeparators(location) + "\n\n" +
                     tr("Use Browse to choose an existing folder for the location."));
        return;
    }

    const QString target = targetPath();
    const QString nativeTarget = QDir::toNativeSeparators(target);
    const DataFolderStatus status = classifyDataFolder(target);

    switch (status) {
    case DataFolderStatus::Oscar1:
        rejectTarget(tr("This folder contains OSCAR 1.x data. OSCAR 2 cannot use an OSCAR 1.x data folder.") + "\n\n" +
                     nativeTarget + "\n\n" + tr("Choose a different folder name or location."));
        return;
    case DataFolderStatus::Oscar2:
        rejectTarget(tr("This folder already contains an OSCAR 2 database.") + "\n\n" + nativeTarget + "\n\n" +
                     (m_mode == Mode::Startup
                          ? tr("To use it, go back and choose \"Find my OSCAR 2 data folder\".")
                          : tr("To use it, choose File ▸ Database ▸ Open.")));
        return;
    case DataFolderStatus::Other:
        rejectTarget(tr("Something with this name already exists in that location, and it is not an empty folder.") + "\n\n" +
                     nativeTarget + "\n\n" + tr("OSCAR needs a new or empty folder. Choose a different folder name or location."));
        return;
    case DataFolderStatus::Missing:
    case DataFolderStatus::Empty:
        break;
    }

    const QString cloud = cloudServiceForPath(target);
    if (!cloud.isEmpty()) {
        if (QMessageBox::warning(this, tr("Cloud-managed location"),
                                 tr("The location you chose appears to be managed by %1.").arg(cloud) + "\n\n" +
                                 tr("Cloud services can change or lock OSCAR's database while OSCAR is using it, which can corrupt your data.") + " " +
                                 tr("We recommend a folder that is not synchronised with a cloud service.") + "\n\n" +
                                 tr("Use this location anyway?"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
    }

    QDir targetDir(target);
    if (!targetDir.mkpath(".")) {
        rejectTarget(tr("OSCAR was unable to create the folder.") + "\n\n" + nativeTarget + "\n\n" +
                     tr("Check that you have permission to create folders in this location."));
        return;
    }

    // Confirm the folder is writable now, while the user can still pick somewhere else.
    QFile testFile(target + "/testfile.txt");
    if (!testFile.open(QFile::ReadWrite)) {
        const QString error = testFile.errorString();
        if (status == DataFolderStatus::Missing)
            QDir().rmdir(target);           // leave nothing behind that we created
        rejectTarget(tr("OSCAR is unable to write to the folder.") + "\n\n" + nativeTarget + "\n\n" +
                     tr("Error") + ": " + error);
        return;
    }
    testFile.close();
    testFile.remove();

    m_folderPath = target;
    qDebug() << "DataFolderCreateDialog: created data folder" << target;
    accept();
}
