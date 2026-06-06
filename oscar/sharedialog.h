/* Share Dialog Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * Modal dialog for preparing and delivering a .oscar profile package
 * optimized for sharing with another user.  The user selects a destination
 * (File, Dropbox, Google Drive, OneDrive) in the dialog itself; cloud
 * destinations upload automatically and display a share link, while the
 * File destination saves to disk.  Privacy mode and simplified filenames
 * are always enabled.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef SHAREDIALOG_H
#define SHAREDIALOG_H

#include <QDate>
#include <QDialog>

class DropboxUploader;
class GoogleDriveUploader;
class ProfileBackup;

namespace Ui {
class ShareDialog;
}

/// Identifies the share destination selected by the user.
enum class ShareDestination {
    File,
    Dropbox,
    GoogleDrive,
    // ZeroX0,  // Commented out: 0x0.st currently unavailable
};

/*!
 * \class ShareDialog
 * \brief Modal dialog for creating and delivering a .oscar profile share package.
 *
 * Combines file creation and delivery into a single dialog.  The user picks
 * a destination (File or a cloud provider) and clicks Share.  For cloud
 * destinations a temporary file is created, uploaded, then deleted; the
 * resulting share URL is displayed inline.  For the File destination a
 * permanent file is written to the selected directory.
 *
 * Privacy mode (personal info blanked) and simplified filenames are always on.
 *
 * Typical usage:
 * \code
 * ShareDialog* dialog = new ShareDialog(this);
 * dialog->exec();
 * delete dialog;
 * \endcode
 */
class ShareDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ShareDialog(QWidget* parent = nullptr);
    ~ShareDialog() override;

private slots:
    /*! \brief Re-anchor the date range to the new profile's data. */
    void on_profileCombo_currentIndexChanged(int index);

    /*! \brief Adjust From/To date edits when the range preset changes. */
    void on_rangeCombo_currentTextChanged(const QString& text);

    /*! \brief Open a directory-picker and populate outputDirEdit. */
    void on_browseButton_clicked();

    /*! \brief Show sharing warning then start the create + deliver operation. */
    void on_shareButton_clicked();

    /*! \brief Switch provider page and update button state when destination changes. */
    void on_destinationCombo_currentIndexChanged(int index);

    /*! \brief Toggle Dropbox sign in / sign out. */
    void onDropboxAuthButtonClicked();

    /*! \brief Handle Dropbox auth result. */
    void onDropboxAuthComplete(bool success);

    /*! \brief Toggle Google Drive sign in / sign out. */
    void onGoogleDriveAuthButtonClicked();

    /*! \brief Handle Google Drive auth result. */
    void onGoogleDriveAuthComplete(bool success);

    /*! \brief Copy the share URL to the clipboard. */
    void onCopyLinkClicked();

    /*! \brief Open the containing folder in the system file browser. */
    void onOpenFolderClicked();

    /*! \brief Update the progress bar and status label during file creation. */
    void onProgressChanged(int percent, const QString& message);

    /*! \brief File creation complete: for File destination show success; for cloud start upload. */
    void onBackupCompleted(const QString& path);

    /*! \brief File creation failed: show error and re-enable the UI. */
    void onBackupFailed(const QString& error);

    /*! \brief Update the progress bar during cloud upload. */
    void onUploadProgress(qint64 bytesSent, qint64 bytesTotal);

    /*! \brief Upload complete: show share URL inline and clean up temp file. */
    void onUploadFinished(const QString& shareUrl);

    /*! \brief Upload failed: show error, clean up temp file, and re-enable the UI. */
    void onUploadFailed(const QString& error);

private:
    /*! \brief Populate profileCombo from the database, pre-selecting the active profile. */
    void populateProfiles();

    /*! \brief Populate destinationCombo with supported destinations. */
    void populateDestinations();

    /*!
     * \brief Set fromDate / toDate from a named range preset.
     * \param rangeText  One of the rangeCombo item strings.
     */
    void applyDateRange(const QString& rangeText);

    /*! \brief Build the share filename from the current profile and date range. */
    QString buildShareFilename() const;

    /*! \brief Rebuild filenameEdit from the current profile and date range. */
    void updateFilenamePreview();

    /*! \brief Switch the stacked widget page and update all destination-dependent controls. */
    void updateDestinationUi();

    /*! \brief Enable/disable the Share button based on the current destination and state. */
    void updateShareButtonState();

    /*! \brief Apply locale-aware date formatting and remove weekend red-highlight. */
    void setupCalendarFormatting();

    /*! \brief Persist settings (output dir, last destination) to QSettings. */
    void saveSettings();

    /*! \brief Restore settings from QSettings. */
    void restoreSettings();

    /*!
     * \brief Query the most recent session date for the selected profile.
     * \return Last date with session data, or today as a fallback.
     */
    QDate getLastDataDate() const;

    /*!
     * \brief Show the sharing-warning dialog.
     * \return true if the user acknowledged and clicked Continue.
     */
    bool showSharingWarning();

    /*! \brief The destination currently selected in destinationCombo. */
    ShareDestination currentDestination() const;

    /*! \brief Disable/enable all input controls while an operation is running. */
    void setUiLocked(bool locked);

    /*! \brief Begin uploading \a filePath to the selected cloud provider. */
    void startCloudUpload(const QString& filePath);

    /*! \brief Delete the temp file if one was created for a cloud upload. */
    void cleanupTempFile();

    Ui::ShareDialog*      ui;
    QList<qint64>         m_profileIds;                    ///< DB IDs parallel to profileCombo.
    DropboxUploader*      m_dropboxUploader     = nullptr;
    GoogleDriveUploader*  m_googleDriveUploader = nullptr;
    ProfileBackup*        m_backup              = nullptr; ///< Active backup object (non-owning; parent owns).
    QString               m_tempFilePath;                  ///< Temp .oscar file for cloud uploads.
    QString               m_lastFilePath;                  ///< Path of last file created (for open folder).
    bool                  m_warningAcknowledged = false;   ///< True once the sharing warning is accepted.
    bool                  m_uploadInProgress    = false;
    bool                  m_operationActive     = false;   ///< True while file creation or upload is running.
    bool                  m_cancelRequested     = false;   ///< True after user clicks Cancel.
};

#endif // SHAREDIALOG_H
