#ifndef APPLEWATCHSYNCDIALOG_H
#define APPLEWATCHSYNCDIALOG_H

#include <QDialog>
#include <QProcess>
#include <QTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>

class AppleWatchSyncDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AppleWatchSyncDialog(QWidget *parent = nullptr);
    ~AppleWatchSyncDialog();

    QString outputDir() const { return m_outputEdit->text().trimmed(); }

private slots:
    void readProcessOutput();
    void processFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void processError(QProcess::ProcessError error);
    void cancelPressed();
    void startSyncPressed();
    void browseScript();
    void browseInput();
    void browseOutput();

private:
    QLineEdit *m_scriptEdit;
    QLineEdit *m_inputEdit;
    QLineEdit *m_outputEdit;
    QLineEdit *m_sourceEdit;
    
    QTextEdit *m_logView;
    QProgressBar *m_progressBar;
    QPushButton *m_syncButton;
    QPushButton *m_cancelButton;
    
    QProcess *m_process;
    bool m_success;
};

#endif // APPLEWATCHSYNCDIALOG_H
