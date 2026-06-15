#include "applewatchsyncdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QScrollBar>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>

AppleWatchSyncDialog::AppleWatchSyncDialog(QWidget *parent)
    : QDialog(parent), m_success(false)
{
    setWindowTitle(tr("Apple Watch Sleep Synchronizer"));
    resize(700, 500);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    mainLayout->setSpacing(12);

    QLabel *infoLabel = new QLabel(tr("Configure Apple Watch Sleep stage import:"), this);
    QFont titleFont = infoLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(12);
    infoLabel->setFont(titleFont);
    mainLayout->addWidget(infoLabel);

    // Grid Layout for configurations
    QGridLayout *formLayout = new QGridLayout();
    formLayout->setSpacing(8);

    // Script Path
    formLayout->addWidget(new QLabel(tr("Python Script:"), this), 0, 0);
    m_scriptEdit = new QLineEdit(this);
    formLayout->addWidget(m_scriptEdit, 0, 1);
    QPushButton *scriptBrowseBtn = new QPushButton(tr("Browse..."), this);
    connect(scriptBrowseBtn, &QPushButton::clicked, this, &AppleWatchSyncDialog::browseScript);
    formLayout->addWidget(scriptBrowseBtn, 0, 2);

    // Input Dir (iCloud)
    formLayout->addWidget(new QLabel(tr("Input Folder (iCloud):"), this), 1, 0);
    m_inputEdit = new QLineEdit(this);
    formLayout->addWidget(m_inputEdit, 1, 1);
    QPushButton *inputBrowseBtn = new QPushButton(tr("Browse..."), this);
    connect(inputBrowseBtn, &QPushButton::clicked, this, &AppleWatchSyncDialog::browseInput);
    formLayout->addWidget(inputBrowseBtn, 1, 2);

    // Output Dir (CSV Target)
    formLayout->addWidget(new QLabel(tr("Output Folder (CSVs):"), this), 2, 0);
    m_outputEdit = new QLineEdit(this);
    formLayout->addWidget(m_outputEdit, 2, 1);
    QPushButton *outputBrowseBtn = new QPushButton(tr("Browse..."), this);
    connect(outputBrowseBtn, &QPushButton::clicked, this, &AppleWatchSyncDialog::browseOutput);
    formLayout->addWidget(outputBrowseBtn, 2, 2);

    // Watch Source Name
    formLayout->addWidget(new QLabel(tr("Watch Source Name:"), this), 3, 0);
    m_sourceEdit = new QLineEdit(this);
    m_sourceEdit->setPlaceholderText(tr("Optional, e.g. Adrian Garett's Apple Watch"));
    formLayout->addWidget(m_sourceEdit, 3, 1, 1, 2);

    mainLayout->addLayout(formLayout);

    // Log View
    m_logView = new QTextEdit(this);
    m_logView->setReadOnly(true);
    m_logView->setFont(QFont("Menlo", 10)); // Monospace log display
    m_logView->setStyleSheet(
        "QTextEdit {"
        "   background-color: #1e1e1e;"
        "   color: #e0e0e0;"
        "   border: 1px solid #333333;"
        "   border-radius: 6px;"
        "   padding: 8px;"
        "}"
    );
    mainLayout->addWidget(m_logView);

    // Progress Bar
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setStyleSheet(
        "QProgressBar {"
        "   border: 1px solid #c0c0c0;"
        "   border-radius: 6px;"
        "   background-color: #e9ecef;"
        "   text-align: center;"
        "   height: 20px;"
        "}"
        "QProgressBar::chunk {"
        "   background-color: qlineargradient(spread:pad, x1:0, y1:0, x2:1, y2:0, stop:0 #a180f4, stop:1 #4facfe);"
        "   border-radius: 5px;"
        "}"
    );
    mainLayout->addWidget(m_progressBar);

    // Action Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    m_syncButton = new QPushButton(tr("Run Sync"), this);
    m_syncButton->setStyleSheet(
        "QPushButton {"
        "   padding: 6px 16px;"
        "   border: 1px solid #a180f4;"
        "   border-radius: 4px;"
        "   background-color: #a180f4;"
        "   color: #ffffff;"
        "   font-weight: bold;"
        "   min-width: 90px;"
        "}"
        "QPushButton:hover {"
        "   background-color: #8c6ae6;"
        "}"
        "QPushButton:pressed {"
        "   background-color: #7956d4;"
        "}"
    );
    connect(m_syncButton, &QPushButton::clicked, this, &AppleWatchSyncDialog::startSyncPressed);
    buttonLayout->addWidget(m_syncButton);

    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setStyleSheet(
        "QPushButton {"
        "   padding: 6px 16px;"
        "   border: 1px solid #ccc;"
        "   border-radius: 4px;"
        "   background-color: #ffffff;"
        "   color: #333333;"
        "   min-width: 80px;"
        "}"
        "QPushButton:hover {"
        "   background-color: #f0f0f0;"
        "}"
        "QPushButton:pressed {"
        "   background-color: #e5e5e5;"
        "}"
    );
    connect(m_cancelButton, &QPushButton::clicked, this, &AppleWatchSyncDialog::cancelPressed);
    buttonLayout->addWidget(m_cancelButton);

    mainLayout->addLayout(buttonLayout);

    m_process = new QProcess(this);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &AppleWatchSyncDialog::readProcessOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &AppleWatchSyncDialog::readProcessOutput);
    connect(m_process, &QProcess::errorOccurred, this, &AppleWatchSyncDialog::processError);
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &AppleWatchSyncDialog::processFinished);

    // Load saved settings
    QSettings settings;
    m_scriptEdit->setText(settings.value("AppleWatch/ScriptPath", QDir::homePath() + "/ezscript/Apple2Dreem.py").toString());
    m_inputEdit->setText(settings.value("AppleWatch/InputDir", QDir::homePath() + "/Library/Mobile Documents/com~apple~CloudDocs/SLEEP-STAGE").toString());
    m_outputEdit->setText(settings.value("AppleWatch/OutputDir", QDir::homePath() + "/APPLEWATCH-SLEEP").toString());
    m_sourceEdit->setText(settings.value("AppleWatch/SourceName", "Adrian Garett's Apple Watch").toString());
}

AppleWatchSyncDialog::~AppleWatchSyncDialog()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

void AppleWatchSyncDialog::browseScript()
{
    QString file = QFileDialog::getOpenFileName(this, tr("Select Python Script"), m_scriptEdit->text(), tr("Python Files (*.py)"));
    if (!file.isEmpty()) {
        m_scriptEdit->setText(file);
    }
}

void AppleWatchSyncDialog::browseInput()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr("Select Input Folder"), m_inputEdit->text());
    if (!dir.isEmpty()) {
        m_inputEdit->setText(dir);
    }
}

void AppleWatchSyncDialog::browseOutput()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr("Select Output Folder"), m_outputEdit->text());
    if (!dir.isEmpty()) {
        m_outputEdit->setText(dir);
    }
}

void AppleWatchSyncDialog::startSyncPressed()
{
    QString scriptPath = m_scriptEdit->text().trimmed();
    QString inputDir = m_inputEdit->text().trimmed();
    QString outputDir = m_outputEdit->text().trimmed();
    QString sourceName = m_sourceEdit->text().trimmed();

    if (scriptPath.isEmpty() || !QFile::exists(scriptPath)) {
        QMessageBox::critical(this, tr("Error"), tr("Python script not found at:\n%1").arg(scriptPath));
        return;
    }

    if (inputDir.isEmpty() || !QDir(inputDir).exists()) {
        QMessageBox::critical(this, tr("Error"), tr("Input folder does not exist:\n%1").arg(inputDir));
        return;
    }

    if (outputDir.isEmpty()) {
        QMessageBox::critical(this, tr("Error"), tr("Please specify a valid output folder."));
        return;
    }

    // Ensure output directory exists or create it
    QDir().mkpath(outputDir);

    m_scriptEdit->setEnabled(false);
    m_inputEdit->setEnabled(false);
    m_outputEdit->setEnabled(false);
    m_sourceEdit->setEnabled(false);
    m_syncButton->setEnabled(false);

    m_progressBar->setRange(0, 0); // Indeterminate
    m_logView->clear();
    m_logView->append(tr("--- Starting Apple Watch Sleep Sync ---"));
    m_logView->append(tr("Script: %1").arg(scriptPath));
    m_logView->append(tr("Input Folder: %1").arg(inputDir));
    m_logView->append(tr("Output Folder: %1").arg(outputDir));
    if (!sourceName.isEmpty()) {
        m_logView->append(tr("Source Filter: %1").arg(sourceName));
    }
    m_logView->append("");

    QStringList arguments;
    arguments << scriptPath;
    arguments << "-i" << inputDir;
    arguments << "-o" << outputDir;
    arguments << "-y" << "json";
    if (!sourceName.isEmpty()) {
        arguments << "-u" << sourceName;
    }

    m_process->start("/usr/bin/env", QStringList() << "python3" << arguments);
}

void AppleWatchSyncDialog::readProcessOutput()
{
    while (m_process->canReadLine()) {
        QByteArray lineBytes = m_process->readLine();
        QString line = QString::fromUtf8(lineBytes).trimmed();
        if (line.isEmpty()) continue;

        m_logView->append(line);
        m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());
    }
}

void AppleWatchSyncDialog::processFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    readProcessOutput();

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        m_success = true;
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(100);
        m_logView->append("");
        m_logView->append(tr("--- Sync finished successfully! ---"));
        m_logView->append(tr("Click 'Close' to import the generated sleep stage files."));

        // Save paths in settings
        QSettings settings;
        settings.setValue("AppleWatch/ScriptPath", m_scriptEdit->text().trimmed());
        settings.setValue("AppleWatch/InputDir", m_inputEdit->text().trimmed());
        settings.setValue("AppleWatch/OutputDir", m_outputEdit->text().trimmed());
        settings.setValue("AppleWatch/SourceName", m_sourceEdit->text().trimmed());

        m_cancelButton->setText(tr("Close"));
        disconnect(m_cancelButton, &QPushButton::clicked, this, &AppleWatchSyncDialog::cancelPressed);
        connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::accept);
    } else {
        m_success = false;
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
        m_logView->append("");
        m_logView->append(tr("--- Sync script failed or returned non-zero code. ---"));
        m_logView->append(tr("Exit code: %1").arg(exitCode));

        // Re-enable form fields for correction
        m_scriptEdit->setEnabled(true);
        m_inputEdit->setEnabled(true);
        m_outputEdit->setEnabled(true);
        m_sourceEdit->setEnabled(true);
        m_syncButton->setEnabled(true);
    }
}

void AppleWatchSyncDialog::processError(QProcess::ProcessError error)
{
    m_logView->append("");
    m_logView->append(tr("Error: Failed to run python sync script."));
    switch (error) {
        case QProcess::FailedToStart:
            m_logView->append(tr("Reason: Python executable or script not found. Make sure python3 is installed and script path is correct."));
            break;
        case QProcess::Crashed:
            m_logView->append(tr("Reason: The process crashed."));
            break;
        case QProcess::Timedout:
            m_logView->append(tr("Reason: Operation timed out."));
            break;
        default:
            m_logView->append(tr("Reason: Unknown process error."));
            break;
    }
    
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);

    // Re-enable form fields
    m_scriptEdit->setEnabled(true);
    m_inputEdit->setEnabled(true);
    m_outputEdit->setEnabled(true);
    m_sourceEdit->setEnabled(true);
    m_syncButton->setEnabled(true);
}

void AppleWatchSyncDialog::cancelPressed()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_logView->append(tr("\n--- Synchronization cancelled by user. ---"));
    }
    reject();
}
