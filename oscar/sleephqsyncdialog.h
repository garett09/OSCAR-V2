#ifndef SLEEPHQSYNCDIALOG_H
#define SLEEPHQSYNCDIALOG_H

#include <QDialog>
#include <QProcess>
#include <QTextEdit>
#include <QProgressBar>
#include <QPushButton>

class SleepHQSyncDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SleepHQSyncDialog(QWidget *parent = nullptr);
    ~SleepHQSyncDialog();

    void startSync(const QString &scriptPath, const QString &cpapDir, const QString &ringDir);

signals:
    void syncFinished(bool success);

private slots:
    void readProcessOutput();
    void processFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void processError(QProcess::ProcessError error);
    void cancelPressed();

private:
    QProcess *m_process;
    QTextEdit *m_logView;
    QProgressBar *m_progressBar;
    QPushButton *m_cancelButton;
    bool m_success;
};

#endif // SLEEPHQSYNCDIALOG_H
