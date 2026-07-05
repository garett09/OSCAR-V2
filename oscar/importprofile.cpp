/* Import Profile Dialog Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file contains the ImportProfile dialog implementation for importing
 * profiles from file-based OSCAR (OSCAR_Data) to SQL-based OSCAR 2.0.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "importprofile.h"
#include "ui_importprofile.h"
#include "translation.h"
#include "common_gui.h"
#include "SleepLib/common.h"
#include "SleepLib/preferences.h"
#include "database/profile_repository.h"
#include "database/app_preferences_repository.h"
#include "database/database_manager.h"
#include <QFileDialog>
#include <QDir>
#include <QMessageBox>
#include <QFileInfo>

ImportProfile::ImportProfile(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ImportProfile)
{
    ui->setupUi(this);
    ui->importButton->setEnabled(false);
    loadSettings();
}

ImportProfile::~ImportProfile()
{
    saveSettings();
    delete ui;
}

void ImportProfile::loadSettings()
{
    AppPreferencesRepository repo;
    const auto rows = repo.loadByCategory("ImportProfile");
    for (const AppPrefData& row : rows) {
        if (row.key == "LastPath") m_lastImportPath = row.value;
    }
}

void ImportProfile::saveSettings()
{
    if (!m_selectedPath.isEmpty()) {
        AppPreferencesRepository repo;
        repo.save("ImportProfile", "LastPath", QFileInfo(m_selectedPath).absolutePath());
    }
}

QString ImportProfile::getDefaultImportPath()
{
    // If we have a remembered path, use it
    if (!m_lastImportPath.isEmpty() && QDir(m_lastImportPath).exists()) {
        return m_lastImportPath;
    }

    // Ask OSCAR 1.x's own settings where its data folder actually is, rather than
    // guessing a relative path below -- the 1.x install may not be a sibling folder.
    QString legacyDataPath = findLegacyOscarDataFolder();
    if (!legacyDataPath.isEmpty()) {
        QString legacyProfilesPath = legacyDataPath + "/Profiles";
        if (QDir(legacyProfilesPath).exists()) {
            return legacyProfilesPath;
        }
    }

    // Try ../OSCAR_Data/Profiles relative to current data directory
    QString appData = GetAppData();  // Current OSCAR20_Data location
    QDir currentDir(appData);
    
    // Go up one level and look for OSCAR_Data
    if (currentDir.cdUp()) {
        QString oscarDataPath = currentDir.absolutePath() + "/OSCAR_Data/Profiles";
        if (QDir(oscarDataPath).exists()) {
            return oscarDataPath;
        }
    }
    
    // Fall back to home directory
    return QDir::homePath();
}

void ImportProfile::on_sourcePathButton_clicked()
{
    QString startPath = getDefaultImportPath();
    
    QString path = QFileDialog::getExistingDirectory(
        this,
        tr("Select Profile Folder from OSCAR 1.x"),
        startPath,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks | nativeDialogOption()
    );
    
    if (path.isEmpty()) return;
    
    // Show message box during validation and size calculation
    QString folderName = QFileInfo(path).fileName();
    QMessageBox* examiningBox = new QMessageBox(this);
    examiningBox->setWindowTitle(tr("Examining Profile"));
    examiningBox->setText(tr("Examining %1...\n\nPlease wait...").arg(folderName));
    examiningBox->setStandardButtons(QMessageBox::NoButton);
    examiningBox->setModal(true);
    examiningBox->show();
    QApplication::processEvents();  // Update UI immediately
    
    // Validate that this looks like a profile folder
    QDir dir(path);
    if (!dir.exists("machines.xml")) {
        examiningBox->close();
        delete examiningBox;
        QMessageBox::warning(this, tr("Invalid Profile"),
            tr("The selected folder does not appear to be a valid OSCAR profile.\n"
               "Please select a folder that contains machines.xml"));
        return;
    }
    
    // Check profile size and warn if > 2GB
    qint64 sizeBytes = calculateProfileSize(path);
    
    // Close the examining message box
    examiningBox->close();
    delete examiningBox;
    double sizeGB = sizeBytes / 1073741824.0;
    if (sizeGB > 2.0) {  // 2GB
        int ret = QMessageBox::warning(this, tr("Large Profile"),
            tr("This profile is %1 GB in size.\n"
               "Import may take a significant amount of time.\n\n"
               "Do you want to continue?").arg(QString::number(sizeGB, 'f', 1)),
            QMessageBox::Yes | QMessageBox::No);
        if (ret != QMessageBox::Yes) {
            return;
        }
    }
    
    m_selectedPath = path;
    
    // Update UI with selected profile
    ui->profileName->setText(folderName);
    
    ui->sourcePathLabel->setText(tr("Source: %1").arg(path));
    updateStatus(tr("Ready to import. Enter a name for the new profile."));
}

qint64 ImportProfile::calculateProfileSize(const QString &path)
{
    qint64 totalSize = 0;
    QDir dir(path);
    
    QFileInfoList entries = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, 
        QDir::DirsFirst
    );
    
    for (const QFileInfo &info : entries) {
        if (info.isFile()) {
            totalSize += info.size();
        } else if (info.isDir()) {
            totalSize += calculateProfileSize(info.absoluteFilePath());
        }
    }
    
    return totalSize;
}

void ImportProfile::on_profileName_textChanged(const QString &text)
{
    Q_UNUSED(text);
    m_newProfileName = ui->profileName->text().trimmed();
    ui->importButton->setEnabled(validateSelection());
}

bool ImportProfile::validateSelection()
{
    if (m_selectedPath.isEmpty()) {
        updateStatus(tr("Please select a source profile folder."));
        return false;
    }
    
    if (m_newProfileName.isEmpty()) {
        updateStatus(tr("Please enter a profile name."));
        return false;
    }
    
    // Check if profile name already exists (filesystem or database)
    ProfileRepository profileRepo;
    auto nameConflicts = [&](const QString& name) -> bool {
        if (QDir(GetAppData() + "/Profiles/" + name).exists()) return true;
        ProfileData pd = profileRepo.findByUsername(name);
        return pd.id > 0;
    };
    if (nameConflicts(m_newProfileName)) {
        // Auto-append (copy) or number
        int num = 2;
        QString baseName = m_newProfileName;
        while (nameConflicts(m_newProfileName)) {
            m_newProfileName = baseName + QString(" (copy %1)").arg(num);
            num++;
        }
        ui->profileName->setText(m_newProfileName);
        updateStatus(tr("Profile name exists. Using: %1").arg(m_newProfileName));
        return true;
    }
    
    updateStatus(tr("Ready to import."));
    return true;
}

void ImportProfile::updateStatus(const QString &message)
{
    ui->statusLabel->setText(message);
}

void ImportProfile::on_importButton_clicked()
{
    accept();
}

void ImportProfile::on_cancelButton_clicked()
{
    reject();
}
