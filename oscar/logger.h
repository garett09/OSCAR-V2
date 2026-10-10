/* Overview GUI Headers
 *
 * Copyright (c) 2023-2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef LOGGER_H
#define LOGGER_H

#include <atomic>

#include <QDebug>
#include <QRunnable>
#include <QThreadPool>
#include <QMutex>
#include <QWaitCondition>
#include <QTime>
#include <QElapsedTimer>

void initializeLogger();
void shutdownLogger();

QString GetLogDir();
void rotateLogs(const QString & filePath, int maxPrevious=-1);


void MyOutputHandler(QtMsgType type, const QMessageLogContext &context, const QString &msgtxt);

class LogThread:public QObject, public QRunnable
{
    Q_OBJECT
public:
    explicit LogThread() :
        QRunnable(),
        running(false),
        connected(false),
        m_logFile(nullptr),
        m_logStream(nullptr)
    {
        logtime.start();
    }

    virtual ~LogThread();

    void run();
    void append(QString msg);
    void appendClean(QString msg);
    bool isRunning()
    {
        return running.load(std::memory_order_relaxed);
    }
    void connectionReady();
    bool logToFile();
    QString logFileName();

    void quit();

    QStringList buffer;
    QMutex strlock;
signals:
    void outputLog(QString);
protected:
    //! \brief True while run() is looping; quit() clears it to stop the thread.
    //! Atomic because isRunning() reads it from other threads; writes happen under strlock.
    std::atomic<bool> running;
    QElapsedTimer logtime;
    bool connected;
    class QFile* m_logFile;
    class QTextStream* m_logStream;
    QStringList m_preFileBuffer;  // holds messages that arrived before logToFile() was called
    QWaitCondition logTrigger;
};

extern LogThread * logger;

#endif // LOGGER_H
