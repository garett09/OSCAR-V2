#include "sleephqsyncdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QDebug>
#include <QDir>
#include <QFileInfo>

SleepHQSyncDialog::SleepHQSyncDialog(QWidget *parent)
    : QDialog(parent), m_success(false)
{
    setWindowTitle(tr("SleepHQ Synchronizer"));
    resize(650, 450);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    mainLayout->setSpacing(12);

    QLabel *infoLabel = new QLabel(tr("Synchronizing CPAP and O2 Ring data with SleepHQ..."), this);
    QFont titleFont = infoLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(12);
    infoLabel->setFont(titleFont);
    mainLayout->addWidget(infoLabel);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 0); // Indeterminate until we parse file counts
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
        "   background-color: qlineargradient(spread:pad, x1:0, y1:0, x2:1, y2:0, stop:0 #4facfe, stop:1 #00f2fe);"
        "   border-radius: 5px;"
        "}"
    );
    mainLayout->addWidget(m_progressBar);

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

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setStyleSheet(
        "QPushButton {"
        "   padding: 6px 16px;"
        "   border: 1px solid #ccc;"
        "   border-radius: 4px;"
        "   background-color: #ffffff;"
        "   min-width: 80px;"
        "}"
        "QPushButton:hover {"
        "   background-color: #f0f0f0;"
        "}"
        "QPushButton:pressed {"
        "   background-color: #e5e5e5;"
        "}"
    );
    connect(m_cancelButton, &QPushButton::clicked, this, &SleepHQSyncDialog::cancelPressed);
    buttonLayout->addWidget(m_cancelButton);

    mainLayout->addLayout(buttonLayout);

    m_process = new QProcess(this);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &SleepHQSyncDialog::readProcessOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &SleepHQSyncDialog::readProcessOutput);
    
    // Connect error and finished signals using appropriate overloads or syntax
    connect(m_process, &QProcess::errorOccurred, this, &SleepHQSyncDialog::processError);
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &SleepHQSyncDialog::processFinished);
}

SleepHQSyncDialog::~SleepHQSyncDialog()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

void SleepHQSyncDialog::startSync(const QString &scriptPath, const QString &cpapDir, const QString &ringDir)
{
    m_logView->append(tr("--- Starting SleepHQ Downloader ---"));
    m_logView->append(tr("Script: %1").arg(scriptPath));
    m_logView->append(tr("CPAP Target: %1").arg(cpapDir));
    m_logView->append(tr("O2 Ring Target: %1").arg(ringDir));
    m_logView->append("");

    QStringList arguments;
    arguments << scriptPath;
    arguments << "--output-dir" << cpapDir;
    arguments << "--ring-dir" << ringDir;

    m_process->start("/usr/bin/env", QStringList() << "python3" << arguments);
}

void SleepHQSyncDialog::readProcessOutput()
{
    while (m_process->canReadLine()) {
        QByteArray lineBytes = m_process->readLine();
        QString line = QString::fromUtf8(lineBytes).trimmed();
        if (line.isEmpty()) continue;

        // Print to log view
        m_logView->append(line);
        m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());

        // Parse progress like [1/10]
        if (line.startsWith('[')) {
            int closeBracket = line.indexOf(']');
            if (closeBracket > 1) {
                QString progressPart = line.mid(1, closeBracket - 1);
                int slash = progressPart.indexOf('/');
                if (slash != -1) {
                    bool ok1 = false;
                    bool ok2 = false;
                    int current = progressPart.left(slash).toInt(&ok1);
                    int total = progressPart.mid(slash + 1).toInt(&ok2);
                    if (ok1 && ok2 && total > 0) {
                        m_progressBar->setMaximum(total);
                        m_progressBar->setValue(current);
                    }
                }
            }
        }
    }
}

void SleepHQSyncDialog::processFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    // Clean up any remaining output
    readProcessOutput();

    if (exitStatus == QProcess::NormalExit && (exitCode == 0 || exitCode == 2)) {
        // Exit code 2 is returned when some file downloads fail, but the execution itself finishes.
        // We'll treat it as success for the execution, and let the loader import whatever was downloaded.
        m_success = true;
        m_progressBar->setMaximum(100);
        m_progressBar->setValue(100);
        m_logView->append("");
        m_logView->append(tr("--- Downloader script finished successfully! ---"));
        m_logView->append(tr("Exit code: %1").arg(exitCode));
        m_logView->append(tr("Click 'Close' to import downloaded files."));

        m_cancelButton->setText(tr("Close"));
        disconnect(m_cancelButton, &QPushButton::clicked, this, &SleepHQSyncDialog::cancelPressed);
        connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::accept);

        emit syncFinished(true);
    } else {
        m_success = false;
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
        m_logView->append("");
        m_logView->append(tr("--- Downloader script failed. ---"));
        m_logView->append(tr("Exit code: %1, Exit status: %2").arg(exitCode).arg(exitStatus == QProcess::CrashExit ? tr("Crashed") : tr("Normal")));
        
        m_cancelButton->setText(tr("Close"));
        disconnect(m_cancelButton, &QPushButton::clicked, this, &SleepHQSyncDialog::cancelPressed);
        connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);

        emit syncFinished(false);
    }
}

void SleepHQSyncDialog::processError(QProcess::ProcessError error)
{
    m_logView->append("");
    m_logView->append(tr("Error: Failed to run python downloader script."));
    switch (error) {
        case QProcess::FailedToStart:
            m_logView->append(tr("Reason: Python executable or script not found. Make sure python3 is installed."));
            break;
        case QProcess::Crashed:
            m_logView->append(tr("Reason: The process crashed."));
            break;
        case QProcess::Timedout:
            m_logView->append(tr("Reason: Connection timeout."));
            break;
        default:
            m_logView->append(tr("Reason: Unknown process error."));
            break;
    }
    m_cancelButton->setText(tr("Close"));
    disconnect(m_cancelButton, &QPushButton::clicked, this, &SleepHQSyncDialog::cancelPressed);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    emit syncFinished(false);
}

void SleepHQSyncDialog::cancelPressed()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_logView->append(tr("\n--- Synchronization cancelled by user. ---"));
    }
    reject();
}

