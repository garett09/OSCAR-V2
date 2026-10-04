/* OSCAR Logger module implementation
 *
 * Copyright (c) 2019-2026 The OSCAR Team
 * Copyright (c) 2011-2018 Mark Watkins 
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "logger.h"
#include "SleepLib/preferences.h"
#include "version.h"
#include <QDir>
#include <QSemaphore>

using namespace std;

LogThread * logger = nullptr;

static QThreadPool * otherThreadPool = nullptr;
static QSemaphore s_LoggerStarted;

void MyOutputHandler(QtMsgType type, const QMessageLogContext &context, const QString &msgtxt)
{
    Q_UNUSED(context)

    if (!logger) {
        fprintf(stderr, "Pre/Post: %s\n", msgtxt.toLocal8Bit().constData());
        return;
    }

    QString msg, typestr, contextstr;
#ifdef VERBOSE_LOGGING
    contextstr = QString(context.file) + " " + context.function + ":" + QString::number(context.line) + " ";
#endif

    switch (type) {
    case QtWarningMsg:
        typestr = QString("Warning:  ") + contextstr;
        break;

    case QtFatalMsg:
        typestr = QString("Fatal:    ") + contextstr;
        break;

    case QtCriticalMsg:
        typestr = QString("Critical: ") + contextstr;
        break;

    default:
        typestr = QString("Debug:    ") + contextstr;
        break;
    }

    msg = typestr + msgtxt; //+QString(" (%1:%2, %3)").arg(context.file).arg(context.line).arg(context.function);


    if (logger && logger->isRunning()) {
        logger->append(msg);
    } else {
        fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
    }

    if (type == QtFatalMsg) {
        abort();
    }

}

void initializeLogger()
{
    logger = new LogThread();
    otherThreadPool = new QThreadPool();
    bool b = otherThreadPool->tryStart(logger);
    if (b) {
        s_LoggerStarted.acquire();  // wait until the thread begins running
    }
#ifndef HARDLOG
    qInstallMessageHandler(MyOutputHandler);  // NOTE: comment this line out when debugging a crash, otherwise the deferred output will mislead you.
#endif
    if (b) {
        qDebug() << "Started logging thread";
    } else {
        qWarning() << "Logging thread did not start correctly";
    }
}

void LogThread::connectionReady()
{
    strlock.lock();
    connected = true;
    logTrigger.wakeAll();
    strlock.unlock();
    qDebug() << "Logging UI initialized";
}

bool LogThread::logToFile()
{
    if (m_logStream) {
        qWarning().noquote() << "Already logging to" << m_logFile->fileName();
        return true;
    }

    QString debugLog = GetLogDir() + "/debug.txt";
    rotateLogs(debugLog, 4);  // keep a limited set of previous logs
    
    strlock.lock();
    m_logFile = new QFile(debugLog);
    Q_ASSERT(m_logFile);
    if (m_logFile->open(QFile::ReadWrite | QFile::Text)) {
        m_logStream = new QTextStream(m_logFile);
        for (const QString& early : m_preFileBuffer) {
            *m_logStream << early << Qt::endl;
        }
        m_logFile->flush();
        m_preFileBuffer.clear();
    }
    logTrigger.wakeAll();
    strlock.unlock();

    if (m_logStream) {
        qDebug().noquote() << "Logging to" << debugLog;
    } else {
        qWarning().noquote() << "Could not open" << debugLog << "error code" << m_logFile->error() << m_logFile->errorString();
        return false;
    }
    return true;
}

LogThread::~LogThread()
{
    QMutexLocker lock(&strlock);
    
    Q_ASSERT(running.load(memory_order_relaxed) == false);
    if (m_logStream) {
        delete m_logStream;
        m_logStream = nullptr;
        Q_ASSERT(m_logFile);
        delete m_logFile;
        m_logFile = nullptr;
    }
}

QString LogThread::logFileName()
{
    if (!m_logFile) {
        return "";
    }
    return m_logFile->fileName();
}

void shutdownLogger()
{
    if (logger) {
        logger->quit();
        // The thread is automatically destroyed when its run() method exits.
        otherThreadPool->waitForDone(-1);  // wait until that happens
        logger = nullptr;
        delete otherThreadPool;
        otherThreadPool = nullptr;
    }
}


void LogThread::append(QString msg)
{
    QString tmp = QString("%1: %2").arg(logtime.elapsed(), 5, 10, QChar('0')).arg(msg);
    appendClean(tmp);
}

void LogThread::appendClean(QString msg)
{
    fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
    strlock.lock();
    // Write directly to file under the lock so the file is always current.
    // On crash the OS flushes the kernel buffer, so no messages are lost.
    if (m_logStream) {
        *m_logStream << msg << Qt::endl;  // Qt::endl flushes the stream buffer
        m_logFile->flush();               // fflush() to the kernel buffer
    } else {
        m_preFileBuffer.append(msg);
    }
    buffer.append(msg);     // retained for UI display only
    logTrigger.wakeAll();
    strlock.unlock();
}


void LogThread::quit() {
    qDebug() << "Shutting down logging thread";
    qInstallMessageHandler(0);  // Remove our logger.
    
    strlock.lock();
    running.store(false, memory_order_relaxed);  // Force the thread to exit after its next iteration.
    logTrigger.wakeAll();  // Trigger the final flush.
    strlock.unlock();      // Release the lock so that the thread can complete.
}


void LogThread::run()
{
    QMutexLocker lock(&strlock);

    running.store(true, memory_order_relaxed);
    // Release only after running is set, and while strlock is still held, so
    // that a quit() arriving first cannot be undone by the line above.
    s_LoggerStarted.release();
    do {
        logTrigger.wait(&strlock);  // releases strlock while it waits
        while (connected && !buffer.isEmpty()) {
            QString msg = buffer.takeFirst();
            emit outputLog(msg);   // file write already done in appendClean()
        }
    } while (running.load(memory_order_relaxed));

    // strlock will be released when lock goes out of scope
}


QString GetLogDir()
{
    static const QString LOG_DIR_NAME = "logs";

    Q_ASSERT(!GetAppData().isEmpty());  // If GetLogDir gets called before GetAppData() is valid, this would point at root.
    QDir oscarData(GetAppData());
    Q_ASSERT(oscarData.exists());
    if (!oscarData.exists(LOG_DIR_NAME)) {
        oscarData.mkdir(LOG_DIR_NAME);
    }
    QDir logDir(oscarData.canonicalPath() + "/" + LOG_DIR_NAME);
    if (!logDir.exists()) {
        qWarning() << "Unable to create" << logDir.absolutePath() << "reverting to" << oscarData.canonicalPath();
        logDir = oscarData;
    }
    Q_ASSERT(logDir.exists());

    return logDir.canonicalPath();
}

void rotateLogs(const QString & filePath, int maxPrevious)
{
    if (maxPrevious < 0) {
        if (getVersion().IsReleaseVersion()) {
            maxPrevious = 1;
        } else {
            // keep more in testing builds
            maxPrevious = 4;
        }
    }

    // Build the list of rotated logs for this filePath.
    QFileInfo info(filePath);
    QString path = QDir(info.absolutePath()).canonicalPath();
    QString base = info.baseName();
    QString ext = info.completeSuffix();
    if (!ext.isEmpty()) {
        ext = "." + ext;
    }
    if (path.isEmpty()) {
        qWarning() << "Skipping log rotation, directory does not exist:" << info.absoluteFilePath();
        return;
    }

    QStringList logs;
    logs.append(filePath);
    for (int i = 0; i < maxPrevious; i++) {
        logs.append(QString("%1/%2.%3%4").arg(path).arg(base).arg(i).arg(ext));
    }

    // Remove the expired log.
    QFileInfo expired(logs[maxPrevious]);
    if (expired.exists()) {
        if (expired.isDir()) {
            QDir dir(expired.canonicalFilePath());
            //qDebug() << "Removing expired log directory" << dir.canonicalPath();
            if (!dir.removeRecursively()) {
                qWarning() << "Unable to delete expired log directory" << dir.canonicalPath();
            }
        } else {
            QFile file(expired.canonicalFilePath());
            //qDebug() << "Removing expired log file" << file.fileName();
            if (!file.remove()) {
                qWarning() << "Unable to delete expired log file" << file.fileName();
            }
        }
    }

    // Rotate the remaining logs.
    for (int i = maxPrevious; i > 0; i--) {
        QFileInfo from(logs[i-1]);
        QFileInfo to(logs[i]);
        if (from.exists()) {
            if (to.exists()) {
                qWarning() << "Unable to rotate log:" << to.absoluteFilePath() << "exists";
                continue;
            }
            //qDebug() << "Renaming" << from.absoluteFilePath() << "to" << to.absoluteFilePath();
            if (!QFile::rename(from.absoluteFilePath(), to.absoluteFilePath())) {
                qWarning() << "Unable to rename" << from.absoluteFilePath() << "to" << to.absoluteFilePath();
            }
        }
    }
}
