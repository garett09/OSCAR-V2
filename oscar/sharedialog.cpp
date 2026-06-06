/* Share Dialog Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "sharedialog.h"
#include "ui_sharedialog.h"
#include "translation.h"
#include "network/dropbox_uploader.h"
#include "network/googledrive_uploader.h"

#include <QApplication>
#include <QCalendarWidget>
#include <QClipboard>
#include <QCheckBox>
#include <QDateEdit>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QTextCharFormat>
#include <QUrl>
#include <QVBoxLayout>

#include "SleepLib/profiles.h"
#include "database/database_manager.h"
#include "database/app_preferences_repository.h"
#include "database/profile_repository.h"
#include "database/backup/profile_backup.h"
#include "mainwindow.h"

extern MainWindow *mainwin;


ShareDialog::ShareDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::ShareDialog)
{
    ui->setupUi(this);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    m_dropboxUploader     = new DropboxUploader(this);
    m_googleDriveUploader = new GoogleDriveUploader(this);

    setupCalendarFormatting();
    populateProfiles();
    populateDestinations();

    // Initialize date edits to today as a fallback.
    QDate today = QDate::currentDate();
    ui->fromDate->setDate(today);
    ui->toDate->setDate(today);

    // Apply default range — "Last Week" is suitable for sharing.
    applyDateRange(ui->rangeCombo->currentText());

    connect(ui->outputDirEdit, &QLineEdit::textChanged,
            this, [this](const QString&){ updateFilenamePreview(); });
    connect(ui->filenameEdit, &QLineEdit::textChanged,
            this, [this](const QString&){ updateShareButtonState(); });
    connect(ui->fromDate, &QDateEdit::dateChanged,
            this, [this](const QDate&){ updateFilenamePreview(); });
    connect(ui->toDate, &QDateEdit::dateChanged,
            this, [this](const QDate&){ updateFilenamePreview(); });

    connect(ui->closeButton, &QPushButton::clicked, this, [this]() {
        if (m_operationActive) {
            m_cancelRequested = true;
            if (m_backup) m_backup->requestCancel();
            if (m_uploadInProgress) {
                if (m_dropboxUploader)     m_dropboxUploader->abort();
                if (m_googleDriveUploader) m_googleDriveUploader->abort();
            }
            ui->closeButton->setEnabled(false);
            ui->closeButton->setText(tr("Cancelling..."));
        } else {
            reject();
        }
    });
    connect(ui->copyLinkButton,  &QPushButton::clicked,  this, &ShareDialog::onCopyLinkClicked);
    connect(ui->openFolderButton,&QPushButton::clicked,  this, &ShareDialog::onOpenFolderClicked);
    connect(ui->dropboxAuthButton, &QPushButton::clicked,
            this, &ShareDialog::onDropboxAuthButtonClicked);
    connect(ui->googleDriveAuthButton, &QPushButton::clicked,
            this, &ShareDialog::onGoogleDriveAuthButtonClicked);

    connect(m_dropboxUploader, &DropboxUploader::authComplete,
            this,              &ShareDialog::onDropboxAuthComplete);
    connect(m_googleDriveUploader, &GoogleDriveUploader::authComplete,
            this,                  &ShareDialog::onGoogleDriveAuthComplete);

    restoreSettings();
}

ShareDialog::~ShareDialog()
{
    if (m_uploadInProgress) {
        if (m_dropboxUploader)     m_dropboxUploader->abort();
        if (m_googleDriveUploader) m_googleDriveUploader->abort();
    }
    cleanupTempFile();
    delete ui;
}

// ---------------------------------------------------------------------------
//  Initialization
// ---------------------------------------------------------------------------

void ShareDialog::populateProfiles()
{
    ui->profileCombo->clear();
    m_profileIds.clear();

    ProfileRepository repo;
    QList<ProfileData> profiles = repo.findActive();

    // Priority: (1) currently open profile, (2) highlighted in profile selector, (3) none.
    QString targetUsername;
    if (p_profile && p_profile->user) {
        targetUsername = p_profile->user->userName();
    } else if (mainwin) {
        targetUsername = mainwin->selectedProfileName();
    }

    int preSelectIndex = -1;
    for (int i = 0; i < profiles.size(); ++i) {
        ui->profileCombo->addItem(profiles[i].username);
        m_profileIds.append(profiles[i].id);
        if (!targetUsername.isEmpty() && profiles[i].username == targetUsername) {
            preSelectIndex = i;
        }
    }

    ui->profileCombo->setCurrentIndex(preSelectIndex);
}

void ShareDialog::populateDestinations()
{
    // Block signals while adding items so the currentIndexChanged slot doesn't
    // fire (and call saveSettings()) before restoreSettings() has run.
    ui->destinationCombo->blockSignals(true);

    // Order matters — index must match the stacked widget page order.
    // Page 0 = File, Page 1 = Dropbox, Page 2 = Google Drive, Page 3 = OneDrive.
    ui->destinationCombo->addItem(tr("File (save to disk)"),
        static_cast<int>(ShareDestination::File));
    ui->destinationCombo->addItem(tr("Dropbox"),
        static_cast<int>(ShareDestination::Dropbox));
    ui->destinationCombo->addItem(tr("Google Drive"),
        static_cast<int>(ShareDestination::GoogleDrive));
    // ui->destinationCombo->addItem(tr("OneDrive (coming soon)"),
    //     static_cast<int>(ShareDestination::OneDrive));
    // ui->destinationCombo->addItem(tr("0x0.st (temporary anonymous hosting)"),
    //     static_cast<int>(ShareDestination::ZeroX0));
    //  ^ Commented out: 0x0.st is currently not accepting new uploads.

    ui->destinationCombo->blockSignals(false);

    updateDestinationUi();
}

// ---------------------------------------------------------------------------
//  Date range
// ---------------------------------------------------------------------------

void ShareDialog::applyDateRange(const QString& rangeText)
{
    bool isCustom = (rangeText == tr("Custom"));
    ui->fromDate->setEnabled(isCustom);
    ui->toDate->setEnabled(isCustom);

    if (!isCustom) {
        const QDate last = getLastDataDate();

        if (rangeText == tr("Most Recent Day")) {
            ui->fromDate->setDate(last);
            ui->toDate->setDate(last);
        } else if (rangeText == tr("Last Week")) {
            ui->fromDate->setDate(last.addDays(-6));
            ui->toDate->setDate(last);
        } else if (rangeText == tr("Last Fortnight")) {
            ui->fromDate->setDate(last.addDays(-13));
            ui->toDate->setDate(last);
        } else if (rangeText == tr("Last Month")) {
            ui->fromDate->setDate(last.addMonths(-1).addDays(1));
            ui->toDate->setDate(last);
        } else if (rangeText == tr("Last 6 Months")) {
            ui->fromDate->setDate(last.addMonths(-6).addDays(1));
            ui->toDate->setDate(last);
        } else if (rangeText == tr("Last Year")) {
            ui->fromDate->setDate(last.addYears(-1).addDays(1));
            ui->toDate->setDate(last);
        }
    }

    updateFilenamePreview();
}

// ---------------------------------------------------------------------------
//  Filename preview
// ---------------------------------------------------------------------------

QString ShareDialog::buildShareFilename() const
{
    const int idx = ui->profileCombo->currentIndex();
    if (idx < 0 || idx >= m_profileIds.size()) return QString();

    // Privacy always on — use "p<id>" as the name portion.
    const QString namePart = QString("p%1").arg(m_profileIds[idx]);

    // Simplify always on — use start date + day count.
    const QDate start = ui->fromDate->date();
    const QDate end   = ui->toDate->date();
    const int count   = start.daysTo(end) + 1;

    return QString("share_%1_%2_%3.oscar")
               .arg(namePart,
                    start.toString(QStringLiteral("yyyyMMdd")),
                    QString::number(count));
}

void ShareDialog::updateFilenamePreview()
{
    // Only relevant for File destination.
    if (currentDestination() != ShareDestination::File) {
        updateShareButtonState();
        return;
    }

    ui->shareButton->setEnabled(false);

    const QString dirText = ui->outputDirEdit->text().trimmed();
    if (dirText.isEmpty() || !QDir(dirText).exists()) {
        ui->filenameEdit->clear();
        ui->filenameEdit->setPlaceholderText(
            dirText.isEmpty() ? tr("(select a directory first)")
                              : tr("(directory does not exist)"));
        ui->filenameEdit->setEnabled(false);
        return;
    }

    const QString filename = buildShareFilename();
    if (filename.isEmpty()) {
        ui->filenameEdit->clear();
        ui->filenameEdit->setPlaceholderText(tr("(no profile selected)"));
        ui->filenameEdit->setEnabled(false);
        return;
    }

    ui->filenameEdit->setText(filename);
    ui->filenameEdit->setEnabled(true);
    updateShareButtonState();
}

// ---------------------------------------------------------------------------
//  Destination UI
// ---------------------------------------------------------------------------

void ShareDialog::updateDestinationUi()
{
    ShareDestination dest = currentDestination();

    // Switch the stacked widget to the matching page.
    switch (dest) {
    case ShareDestination::File:
        ui->destinationStack->setCurrentIndex(0);
        break;
    case ShareDestination::Dropbox:
        ui->destinationStack->setCurrentIndex(1);
        // Update auth button text.
        if (m_dropboxUploader->isAuthenticated()) {
            ui->dropboxAuthButton->setText(tr("Sign Out"));
            ui->dropboxStatusLabel->setText(tr("Signed in to Dropbox."));
        } else {
            ui->dropboxAuthButton->setText(tr("Sign In..."));
            ui->dropboxStatusLabel->setText(
                tr("Sign in to Dropbox to upload and create a share link."));
        }
        break;
    case ShareDestination::GoogleDrive:
        ui->destinationStack->setCurrentIndex(2);
        if (m_googleDriveUploader->isAuthenticated()) {
            ui->googleDriveAuthButton->setText(tr("Sign Out"));
            ui->googleDriveStatusLabel->setText(tr("Signed in to Google Drive."));
        } else {
            ui->googleDriveAuthButton->setText(tr("Sign In..."));
            ui->googleDriveStatusLabel->setText(
                tr("Sign in to Google Drive to upload and create a share link."));
        }
        break;
    }

    // URL row only relevant for cloud destinations, and only shown after upload.
    // (Visibility is managed by onUploadFinished / setUiLocked.)

    updateShareButtonState();
}

void ShareDialog::updateShareButtonState()
{
    ShareDestination dest = currentDestination();
    switch (dest) {
    case ShareDestination::File:
        ui->shareButton->setText(tr("Create File"));
        ui->shareButton->setEnabled(
            !ui->outputDirEdit->text().isEmpty() &&
            !ui->filenameEdit->text().trimmed().isEmpty());
        break;
    case ShareDestination::Dropbox:
        ui->shareButton->setText(tr("Share"));
        ui->shareButton->setEnabled(m_dropboxUploader->isAuthenticated());
        break;
    case ShareDestination::GoogleDrive:
        ui->shareButton->setText(tr("Share"));
        ui->shareButton->setEnabled(m_googleDriveUploader->isAuthenticated());
        break;
    }
}

// ---------------------------------------------------------------------------
//  Calendar formatting
// ---------------------------------------------------------------------------

void ShareDialog::setupCalendarFormatting()
{
    QLocale locale = QLocale::system();
    QString fmt = locale.dateFormat(QLocale::ShortFormat);
    if (!fmt.toLower().contains("yyyy")) {
        fmt.replace("yy", "yyyy");
    }
    ui->fromDate->setDisplayFormat(fmt);
    ui->toDate->setDisplayFormat(fmt);

    for (QDateEdit* de : { ui->fromDate, ui->toDate }) {
        QCalendarWidget* cal = de->calendarWidget();
        QTextCharFormat format = cal->weekdayTextFormat(Qt::Saturday);
        format.setForeground(QBrush(Qt::black, Qt::SolidPattern));
        cal->setWeekdayTextFormat(Qt::Saturday, format);
        cal->setWeekdayTextFormat(Qt::Sunday, format);
    }
}

// ---------------------------------------------------------------------------
//  Settings persistence
// ---------------------------------------------------------------------------

void ShareDialog::saveSettings()
{
    AppPreferencesRepository repo;
    repo.save("ShareDialog", "lastOutputDir",   ui->outputDirEdit->text());
    repo.save("ShareDialog", "lastDestination", ui->destinationCombo->currentData().toInt());
}

void ShareDialog::restoreSettings()
{
    AppPreferencesRepository repo;
    const auto rows = repo.loadByCategory("ShareDialog");
    QString lastDir;
    int lastDest = static_cast<int>(ShareDestination::File);
    for (const AppPrefData& row : rows) {
        if      (row.key == "lastOutputDir")   lastDir  = row.value;
        else if (row.key == "lastDestination") lastDest = row.value.toInt();
    }

    if (!lastDir.isEmpty() && QDir(lastDir).exists()) {
        ui->outputDirEdit->setText(lastDir);
    }

    for (int i = 0; i < ui->destinationCombo->count(); ++i) {
        if (ui->destinationCombo->itemData(i).toInt() == lastDest) {
            // Block signals so updateDestinationUi isn't called before
            // populateDestinations() finishes setting everything up.
            ui->destinationCombo->blockSignals(true);
            ui->destinationCombo->setCurrentIndex(i);
            ui->destinationCombo->blockSignals(false);
            break;
        }
    }

    updateDestinationUi();
    updateFilenamePreview();
}

// ---------------------------------------------------------------------------
//  Sharing warning
// ---------------------------------------------------------------------------

bool ShareDialog::showSharingWarning()
{
    if (m_warningAcknowledged) return true;

    QDialog warn(this);
    warn.setWindowTitle(tr("Sharing Medical Data"));
    warn.setWindowFlags(warn.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    QVBoxLayout* layout = new QVBoxLayout(&warn);

    QLabel* title = new QLabel(tr("<b>Important: You are sharing medical data</b>"), &warn);
    layout->addWidget(title);

    QLabel* body = new QLabel(
        tr("You are about to share a file containing your sleep therapy data.\n\n"
           "\u2022 The file contains session data, events, and machine settings\n"
           "  for the selected date range\n"
           "\u2022 Personal information (name, DOB, contact details) will be removed\n\n"
           "Make sure you trust the recipient before sharing this data."),
        &warn);
    body->setWordWrap(true);
    layout->addWidget(body);

    QCheckBox* check = new QCheckBox(
        tr("I understand this data will be shared with another person"), &warn);
    layout->addWidget(check);

    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &warn);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Continue"));
    layout->addWidget(buttons);

    connect(check, &QCheckBox::toggled,
            buttons->button(QDialogButtonBox::Ok), &QPushButton::setEnabled);
    connect(buttons, &QDialogButtonBox::accepted, &warn, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &warn, &QDialog::reject);

    if (warn.exec() != QDialog::Accepted) return false;

    m_warningAcknowledged = true;
    return true;
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

ShareDestination ShareDialog::currentDestination() const
{
    return static_cast<ShareDestination>(ui->destinationCombo->currentData().toInt());
}

void ShareDialog::setUiLocked(bool locked)
{
    m_operationActive = locked;
    ui->profileCombo->setEnabled(!locked);
    ui->rangeCombo->setEnabled(!locked);
    ui->destinationCombo->setEnabled(!locked);
    ui->shareButton->setEnabled(!locked);
    ui->browseButton->setEnabled(!locked);
    ui->dropboxAuthButton->setEnabled(!locked);
    ui->googleDriveAuthButton->setEnabled(!locked);

    // Button always says Cancel while the dialog is in an active or ready state;
    // the completion handlers switch it to Close after a successful share.
    ui->closeButton->setText(tr("Cancel"));
    ui->closeButton->setEnabled(true);

    // filenameEdit is only editable for File destination when not locked.
    bool fileEditable = !locked &&
                        currentDestination() == ShareDestination::File &&
                        !ui->outputDirEdit->text().isEmpty();
    ui->filenameEdit->setEnabled(fileEditable);
}

void ShareDialog::cleanupTempFile()
{
    if (!m_tempFilePath.isEmpty()) {
        QFile::remove(m_tempFilePath);
        m_tempFilePath.clear();
    }
}

QDate ShareDialog::getLastDataDate() const
{
    QDate last = QDate::currentDate();
    const int idx = ui->profileCombo->currentIndex();
    if (idx >= 0 && idx < m_profileIds.size()) {
        qint64 pid = m_profileIds[idx];
        QSqlQuery q(DatabaseManager::instance().database());
        q.prepare(QStringLiteral(
            "SELECT MAX(DATE(start_time/1000 - 43200, 'unixepoch', 'localtime')) "
            "FROM sessions "
            "WHERE machine_id IN (SELECT id FROM machines WHERE profile_id = :pid)"));
        q.bindValue(QStringLiteral(":pid"), pid);
        if (q.exec() && q.next() && !q.value(0).isNull()) {
            QDate d = QDate::fromString(q.value(0).toString(), Qt::ISODate);
            if (d.isValid()) last = d;
        }
    }
    return last;
}

// ---------------------------------------------------------------------------
//  Cloud upload
// ---------------------------------------------------------------------------

void ShareDialog::startCloudUpload(const QString& filePath)
{
    m_uploadInProgress = true;
    ShareDestination dest = currentDestination();

    switch (dest) {
    case ShareDestination::Dropbox:
        m_dropboxUploader->setFilePath(filePath);
        connect(m_dropboxUploader, &DropboxUploader::uploadProgress,
                this, &ShareDialog::onUploadProgress, Qt::UniqueConnection);
        connect(m_dropboxUploader, &DropboxUploader::uploadFinished,
                this, &ShareDialog::onUploadFinished, Qt::UniqueConnection);
        connect(m_dropboxUploader, &DropboxUploader::uploadFailed,
                this, &ShareDialog::onUploadFailed, Qt::UniqueConnection);
        m_dropboxUploader->startUpload();
        break;
    case ShareDestination::GoogleDrive:
        m_googleDriveUploader->setFilePath(filePath);
        connect(m_googleDriveUploader, &GoogleDriveUploader::uploadProgress,
                this, &ShareDialog::onUploadProgress, Qt::UniqueConnection);
        connect(m_googleDriveUploader, &GoogleDriveUploader::uploadFinished,
                this, &ShareDialog::onUploadFinished, Qt::UniqueConnection);
        connect(m_googleDriveUploader, &GoogleDriveUploader::uploadFailed,
                this, &ShareDialog::onUploadFailed, Qt::UniqueConnection);
        m_googleDriveUploader->startUpload();
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
//  Slots
// ---------------------------------------------------------------------------

void ShareDialog::on_profileCombo_currentIndexChanged(int /*index*/)
{
    applyDateRange(ui->rangeCombo->currentText());
}

void ShareDialog::on_rangeCombo_currentTextChanged(const QString& text)
{
    applyDateRange(text);
}

void ShareDialog::on_destinationCombo_currentIndexChanged(int /*index*/)
{
    // Hide URL row and folder button when destination changes.
    ui->urlEdit->setVisible(false);
    ui->copyLinkButton->setVisible(false);
    ui->openFolderButton->setVisible(false);
    ui->shareButton->setVisible(true);
    ui->statusLabel->clear();

    saveSettings();
    updateDestinationUi();
    updateFilenamePreview();
}

void ShareDialog::on_browseButton_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("Select Output Directory"),
        ui->outputDirEdit->text().isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            : ui->outputDirEdit->text(),
        QFileDialog::ShowDirsOnly | nativeDialogOption());

    if (!dir.isEmpty()) {
        ui->outputDirEdit->setText(dir);
        saveSettings();
        updateFilenamePreview();
    }
}

void ShareDialog::on_shareButton_clicked()
{
    const int idx = ui->profileCombo->currentIndex();
    if (idx < 0 || idx >= m_profileIds.size()) {
        QMessageBox::warning(this, tr("Share Profile"), tr("No profile selected."));
        return;
    }

    if (!showSharingWarning()) return;

    m_cancelRequested = false;
    setUiLocked(true);
    ui->progressBar->setVisible(true);
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);
    ui->statusLabel->setText(tr("Creating file..."));

    const qint64 profileId = m_profileIds[idx];
    m_backup = new ProfileBackup(profileId, this);
    ProfileBackup* backup = m_backup;

    ShareDestination dest = currentDestination();

    if (dest == ShareDestination::File) {
        // Permanent file in the user-chosen directory.
        if (ui->outputDirEdit->text().isEmpty()) {
            QMessageBox::warning(this, tr("Share Profile"),
                tr("Please select an output directory."));
            setUiLocked(false);
            ui->progressBar->setVisible(false);
            return;
        }
        backup->setOutputPath(ui->outputDirEdit->text());
        // Strip any path components from the user-entered filename to prevent
        // traversal (e.g. "../evil") writing outside the selected directory.
        const QString filename = QFileInfo(ui->filenameEdit->text().trimmed()).fileName();
        if (!filename.isEmpty()) {
            backup->setFilename(filename);
        }
    } else {
        // Temporary file for cloud upload — use the same meaningful filename
        // as the File destination so the name in Dropbox etc. is readable.
        const QString filename = buildShareFilename();
        m_tempFilePath = QDir::tempPath() + "/" + filename;
        // Remove any leftover file at this path before writing.
        QFile::remove(m_tempFilePath);

        backup->setOutputPath(QDir::tempPath());
        backup->setFilename(filename);
    }

    backup->setPackageType(QStringLiteral("share"));
    backup->setPrivacyMode(true);
    backup->setIncludeSDData(false);
    backup->setDateRange(ui->fromDate->date(), ui->toDate->date());

    connect(backup, &ProfileBackup::progressChanged,
            this,   &ShareDialog::onProgressChanged);
    connect(backup, &ProfileBackup::backupCompleted,
            this,   &ShareDialog::onBackupCompleted);
    connect(backup, &ProfileBackup::backupFailed,
            this,   &ShareDialog::onBackupFailed);

    backup->createBackup();
}

void ShareDialog::onDropboxAuthButtonClicked()
{
    if (m_dropboxUploader->isAuthenticated()) {
        m_dropboxUploader->signOut();
        updateDestinationUi();
    } else {
        ui->dropboxStatusLabel->setText(tr("Opening browser for sign in..."));
        ui->dropboxAuthButton->setEnabled(false);
        m_dropboxUploader->authenticate();
    }
}

void ShareDialog::onDropboxAuthComplete(bool success)
{
    ui->dropboxAuthButton->setEnabled(true);
    if (success) {
        ui->dropboxStatusLabel->setText(tr("Signed in to Dropbox."));
    } else {
        ui->dropboxStatusLabel->setText(tr("Dropbox sign in failed."));
    }
    updateDestinationUi();
}

void ShareDialog::onProgressChanged(int percent, const QString& message)
{
    ui->progressBar->setValue(percent);
    ui->statusLabel->setText(message);
}

void ShareDialog::onBackupCompleted(const QString& path)
{
    m_backup = nullptr;
    ui->progressBar->setValue(100);
    m_lastFilePath = path;

    ShareDestination dest = currentDestination();
    if (dest == ShareDestination::File) {
        // Done — show success and open-folder button.
        QFileInfo fi(path);
        QString sizeStr;
        qint64 bytes = fi.size();
        if (bytes >= 1024 * 1024) {
            sizeStr = QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
        } else {
            sizeStr = QString::number(bytes / 1024.0, 'f', 1) + " KB";
        }
        ui->statusLabel->setText(
            tr("File created: %1  (%2)").arg(fi.fileName(), sizeStr));
        ui->openFolderButton->setVisible(true);
        ui->shareButton->setVisible(false);
        m_operationActive = false;
        ui->closeButton->setText(tr("Close"));
        ui->closeButton->setEnabled(true);
        // Leave other controls locked so the user sees the result before closing.
    } else {
        // Cloud destination — now upload the temp file.
        ui->progressBar->setRange(0, 0);  // Indeterminate during upload.
        ui->statusLabel->setText(tr("Uploading..."));
        startCloudUpload(path);
    }
}

void ShareDialog::onBackupFailed(const QString& error)
{
    m_backup = nullptr;
    cleanupTempFile();
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);
    if (m_cancelRequested) {
        m_cancelRequested = false;
        ui->statusLabel->setText(tr("Share cancelled."));
    } else {
        ui->statusLabel->setText(tr("Failed to create file."));
        QMessageBox::critical(this, tr("Share Failed"),
            tr("Could not create the sharing file.\n\n%1").arg(error));
    }
    setUiLocked(false);
    ui->progressBar->setVisible(false);
}

void ShareDialog::onUploadProgress(qint64 bytesSent, qint64 bytesTotal)
{
    if (bytesTotal > 0) {
        ui->progressBar->setRange(0, 100);
        int percent = static_cast<int>(bytesSent * 100 / bytesTotal);
        ui->progressBar->setValue(percent);

        QString sent, total;
        if (bytesTotal >= 1024 * 1024) {
            sent  = QString::number(bytesSent / (1024.0 * 1024.0), 'f', 1) + " MB";
            total = QString::number(bytesTotal / (1024.0 * 1024.0), 'f', 1) + " MB";
        } else {
            sent  = QString::number(bytesSent / 1024.0, 'f', 0) + " KB";
            total = QString::number(bytesTotal / 1024.0, 'f', 0) + " KB";
        }
        ui->statusLabel->setText(tr("Uploading... (%1 / %2)").arg(sent, total));
    }
}

void ShareDialog::onUploadFinished(const QString& shareUrl)
{
    m_uploadInProgress = false;
    cleanupTempFile();

    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(100);

    QApplication::clipboard()->setText(shareUrl);
    ui->statusLabel->setText(tr("Upload complete. Link copied to clipboard."));

    ui->urlEdit->setText(shareUrl);
    ui->urlEdit->setVisible(true);
    ui->urlEdit->selectAll();
    ui->copyLinkButton->setVisible(true);
    ui->shareButton->setVisible(false);
    m_operationActive = false;
    ui->closeButton->setText(tr("Close"));
    ui->closeButton->setEnabled(true);
}

void ShareDialog::onUploadFailed(const QString& error)
{
    m_uploadInProgress = false;
    cleanupTempFile();

    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);
    if (m_cancelRequested) {
        m_cancelRequested = false;
        ui->statusLabel->setText(tr("Share cancelled."));
    } else {
        ui->statusLabel->setText(tr("Upload failed."));
        QMessageBox::warning(this, tr("Upload Failed"), error);
    }

    setUiLocked(false);
    updateShareButtonState();
}

void ShareDialog::onCopyLinkClicked()
{
    QApplication::clipboard()->setText(ui->urlEdit->text());
    ui->statusLabel->setText(tr("Link copied to clipboard."));
}

void ShareDialog::onOpenFolderClicked()
{
    if (!m_lastFilePath.isEmpty()) {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QFileInfo(m_lastFilePath).absolutePath()));
    }
}

void ShareDialog::onGoogleDriveAuthButtonClicked()
{
    if (m_googleDriveUploader->isAuthenticated()) {
        m_googleDriveUploader->signOut();
        updateDestinationUi();
    } else {
        ui->googleDriveStatusLabel->setText(tr("Opening browser for sign in..."));
        ui->googleDriveAuthButton->setEnabled(false);
        m_googleDriveUploader->authenticate();
    }
}

void ShareDialog::onGoogleDriveAuthComplete(bool success)
{
    ui->googleDriveAuthButton->setEnabled(true);
    if (success) {
        ui->googleDriveStatusLabel->setText(tr("Signed in to Google Drive."));
    } else {
        ui->googleDriveStatusLabel->setText(tr("Google Drive sign in failed."));
    }
    updateDestinationUi();
}
