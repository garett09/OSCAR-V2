/* SleepLib Device Class Implementation
 *
 * Copyright (c) 2019-2026 The OSCAR Team
 * Copyright (c) 2011-2018 Mark Watkins
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#define TEST_MACROS_ENABLEDoff
#include <test_macros.h>

#include <QApplication>
#include <QDir>
#include <QDebug>
#include <QString>
#include <QObject>
#include <QThreadPool>
#include <QFile>
#include <QDataStream>


#include <QDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include "mainwindow.h"

#include "progressdialog.h"

#include <time.h>

#include "machine.h"
#include "profiles.h"
#include "schema.h"
#include "session.h"
#include "mainwindow.h"
#include "../database/machine_repository.h"
#include "../database/profile_repository.h"
#include "../database/session_repository.h"
#include "../database/session_summaries_repository.h"
#include "../database/daily_summary_repository.h"
#include "../database/database_manager.h"
#include "../database/device_time_correction_repository.h"

extern MainWindow * mainwin;

//void SaveThread::run()
//{
//    bool running = true;
//    while (running) {
//        Session //sess = machine->popSaveList();
//        if (sess) {
//            if (machine->m_donetasks % 20 == 0) {
//                int i = (float(machine->m_donetasks) / float(machine->m_totaltasks) * 100.0);
//                emit UpdateProgress(i);
//            }
//            sess->UpdateSummaries();
//            //machine->saveMutex.lock();
//            sess->Store(path);
//            //machine->saveMutex.unlock();
//
//            sess->TrashEvents();
//        } else {
//            if (!machine->m_save_threads_running) {
//                break; // done
//            } else {
//                yieldCurrentThread(); // go do something else for a while
//            }
//        }
//    }
//
//    machine->savelistSem->release(1);
//}

void SaveTask::run()
{
    sess->UpdateSummaries();
    mach->saveMutex.lock();
    sess->Store(mach->getDataPath());
    mach->saveMutex.unlock();
    sess->TrashEvents();
}

//////////////////////////////////////////////////////////////////////////////////////////
// Device Base-Class implmementation
//////////////////////////////////////////////////////////////////////////////////////////
Machine::Machine(Profile *_profile, MachineID id) : profile(_profile)
{
    day.clear();
    highest_sessionid = 0;
    m_suppressUntestedWarning = false;
    m_database_id = 0;  // Initialize database ID to 0 (not in database)

    // TODO: Have the device write m_suppressUntestedWarning and m_previousUnexpected
    // to XML (along with the current OSCAR version number) so that they persist across
    // application launches (but reset with each new OSCAR version).

    if (!id) {
        srand(time(nullptr));
        MachineID temp;

        bool found;
        // Keep trying until we get a unique DeviceID for this profile
        do {
            temp = rand();

            found = false;
            for (int i=0;i<profile->m_machlist.size(); ++i) {
                if (profile->m_machlist.at(i)->id() == temp) found = true;
            }
        } while (found);

        m_id = temp;

    } else { m_id = id; }
    m_loader = nullptr;

   // qDebug() << "Create device: " << hex << m_id; //%lx",m_id);
    m_type = MT_UNKNOWN;
    firstsession = true;
}
Machine::~Machine()
{
    //qDebug() << "Destroy device" << info.loadername << hex << m_id;
}
Session *Machine::SessionExists(SessionID session)
{
    if (sessionlist.find(session) != sessionlist.end()) {
        return sessionlist[session];
    } else {
        return nullptr;
    }
}

// Find date this session belongs in
QDate Machine::pickDate(qint64 first)
{
    QTime split_time = profile->session->daySplitTime();
    int combine_sessions = profile->session->combineCloseSessions();

    QDateTime d2 = QDateTime::fromSecsSinceEpoch(first / 1000);

    QDate date = d2.date();

    int closest_session = 0;

    qint64 splitEpoch = QDateTime(d2.date(), split_time, Qt::LocalTime).toMSecsSinceEpoch();
    if (first < splitEpoch) {
        date = date.addDays(-1);
    } else if (combine_sessions > 0) {
        QMap<QDate, Day *>::iterator dit = day.find(date.addDays(-1)); // Check Day Before

        if (dit != day.end()) {
            QDateTime lt = QDateTime::fromSecsSinceEpoch(dit.value()->last() / 1000L);
            closest_session = lt.secsTo(d2) / 60;

            if (closest_session < combine_sessions) {
                date = date.addDays(-1);
            }
        }
    }

    return date;
}

// allowOldSessions defaults to false and is only set to true when loading
// summary data on profile load. This true setting prevents old sessions from
// becoming lost if user preference indicates to not import sessions prior to a
// given date.
bool Machine::AddSession(Session *s, bool allowOldSessions)
{
    if (s == nullptr) {
        qCritical() << "AddSession() called with a null object";
        return false;
    }
    if (profile == nullptr) {
        qCritical() << "AddSession() called without a valid profile";
        return false;
    }
    if (sessionlist.contains(s->session())) {
        qCritical() << "Machine::AddSession called with duplicate session" << s->session()
                    << "["+QDateTime::fromSecsSinceEpoch(s->session()).toString("MMM dd, yyyy hh:mm:ss")+"]"
                    << "for machine" << serial();
        return false;
    }

    if (s->first() == 0) {
        qWarning() << "Machine::AddSession called with session" << s->session()
                   << "["+QDateTime::fromSecsSinceEpoch(s->session()).toString("MMM dd, yyyy hh:mm:ss")+"]"
                   << "with first=0";
        return false;
    }

    // allowOldSessions is true when loading summaries (already imported sessions)
    // We don't want to throw away data already in the database in circumstances
    // where user wants to ignore old sessions on import.
    if (profile->session->ignoreOlderSessions() && !allowOldSessions) {
        qint64 ignorebefore = profile->session->ignoreOlderSessionsDate().toMSecsSinceEpoch();
        if (s->last() < ignorebefore) {
            qDebug() << s->session() << "Ignoring old session";
            skipped_sessions++;
            return false;
        }
    }

    updateChannels(s);

    if (s->session() > highest_sessionid) {
        highest_sessionid = s->session();
    }

    QTime split_time;
    int combine_sessions;
    bool locksessions = profile->session->lockSummarySessions();

    if (locksessions) {
        split_time = s->summaryOnly() ? QTime(12,0,0) : profile->session->daySplitTime();
        combine_sessions = s->summaryOnly() ? 0 : profile->session->combineCloseSessions();
    } else {
        split_time = profile->session->daySplitTime();
        combine_sessions = profile->session->combineCloseSessions();
    }

    int ignore_sessions = profile->session->ignoreShortSessions();

    qint64 session_length = s->last() - s->first();
    session_length /= 60000L;

    sessionlist[s->session()] = s; // To make sure it get's saved later even if it's not wanted.

    //int drift=profile->cpap->clockDrift();

    qint64 rawFirst = s->first();
    qint64 corrMs   = correctionMs(QDateTime::fromMSecsSinceEpoch(rawFirst).date());
    QDateTime d2    = QDateTime::fromMSecsSinceEpoch(rawFirst + corrMs);

    QDate date = d2.date();

    QMap<QDate, Day *>::iterator dit, nextday;

    bool combine_next_day = false;
    int closest_session = 0;


    // Multithreaded import screws this up. :(

    qint64 splitEpoch = QDateTime(d2.date(), split_time, Qt::LocalTime).toMSecsSinceEpoch();
    if (rawFirst + corrMs < splitEpoch) {
        date = date.addDays(-1);
    } else if (combine_sessions > 0) {
        dit = day.find(date.addDays(-1)); // Check Day Before

        if (dit != day.end()) {
            QDateTime lt = QDateTime::fromSecsSinceEpoch(dit.value()->last() / 1000);
            closest_session = lt.secsTo(d2) / 60;

            if (closest_session < combine_sessions) {
                date = date.addDays(-1);
            } else {
                if ((split_time < d2.time()) && (split_time.secsTo(d2.time()) < 2)) {
                    if (s->machine()->loaderName() == STR_MACH_ResMed) {
                        date = date.addDays(-1);
                    }
                }
            }
        } else {
            nextday = day.find(date.addDays(1)); // Check Day Afterwards

            if (nextday != day.end()) {
                QDateTime lt = QDateTime::fromSecsSinceEpoch(nextday.value()->first() / 1000);
                closest_session = d2.secsTo(lt) / 60;

                if (closest_session < combine_sessions) {
                    // add todays here. pull all tomorrows records to this date.
                    combine_next_day = true;
                }
            }
        }
    }

    if (session_length < ignore_sessions) {
        // keep the session to save importing it again, but don't add it to the day record this time
        // qDebug() << s->session() << "Ignoring short session <" << ignore_sessions << "["+QDateTime::fromMSecsSinceEpoch(s->first()).toString("MMM dd, yyyy hh:mm:ss")+"]";
        return true;
    }

    if ( ! firstsession) {
        if (firstday > date) { firstday = date; }

        if (lastday < date) { lastday = date; }
    } else {
        firstday = lastday = date;
        firstsession = false;
    }


    Day *dd = nullptr;
    dit = day.find(date);

    if (dit == day.end()) {
        dit = day.insert(date, profile->addDay(date));
    }
    dd = dit.value();
    dd->addSession(s);

    if (combine_next_day) {
        for (QList<Session *>::iterator i = nextday.value()->begin(); i != nextday.value()->end(); i++) {
            // i may need to do something here
            if (locksessions && (*i)->summaryOnly()) continue; // can't move summary only sessions..
            unlinkSession(*i);
            // Add it back

            sessionlist[(*i)->session()] = *i;

            dd->addSession(*i);
        }

//        QMap<QDate, QList<Day *> >::iterator nd = profile->daylist.find(date.addDays(1));
//        if (nd != profile->daylist.end()) {
//            profile->unlinkDay(nd.key(), nd.value());
//        }

//        QList<Day *>::iterator iend = nd.value().end();
//        for (QList<Day *>::iterator i = nd.value()->begin(); i != iend; ++i) {
//            if (*i == nextday.value()) {
//                nd.value().erase(i);
//            }
//        }

//        day.erase(nextday);
    }

    return true;
}

bool Machine::unlinkDay(Day * d)
{
    return day.remove(day.key(d)) > 0;
}

QString Machine::getPixmapPath()
{
    if (!loader())
        return "";
    return loader()->getPixmapPath(info.series);
}

QPixmap & Machine::getPixmap()
{
    static QPixmap pm;
    if (!loader())
        return pm;
    return loader()->getPixmap(info.series);
}

bool Machine::unlinkSession(Session * sess)
{
    MachineType mt = sess->type();

    // Remove the object from the device object's session list
    bool b=sessionlist.remove(sess->session());

    QList<QDate> dates;

    QList<Day *> days;
    QMap<QDate, Day *>::iterator it;

    Day * d;

    // Doing this in case of accidental double linkages
    for (it = day.begin(); it != day.end(); ++it) {
        d = it.value();
        if (it.value()->sessions.contains(sess)) {
            days.push_back(d);
            dates.push_back(it.key());
        }
    }

    for (int i=0; i < days.size(); ++i) {
        d = days.at(i);
        if (d->sessions.removeAll(sess)) {
            b=true;
            if (!d->searchMachine(mt)) {
                d->machines.remove(mt);
                day.remove(dates[i]);
            }

            if (d->size() == 0) {
                profile->unlinkDay(d);
            }
        }
    }

    return b;
}

// This functions purpose is murder and mayhem... It deletes all of a devices data.
bool Machine::Purge(int secret)
{
    // Boring api key to stop this function getting called by accident :)
    if (secret != 3478216) { return false; }

    QString path = getDataPath();

    QDir dir(path);

    if (!dir.exists()) { // It doesn't exist anyway.
        return true;
    }

    if (!dir.isReadable()) {
        return false;
    }

    qDebug() << "Purging" << info.loadername << info.serial << dir.absoluteFilePath(path);

    // Remove any imported file list
    QFile impfile(getDataPath()+"/imported_files.csv");
    impfile.remove();

    QFile rxcache(profile->Get("{" + STR_GEN_DataFolder + "}/RXChanges.cache" ));
    rxcache.remove();

    QFile sumfile(getDataPath()+"/Summaries.xml.gz");
    sumfile.remove();

    QFile sessinfofile(getDataPath()+"/Sessions.info");
    sessinfofile.remove();

    // Create a copy of the list so the hash can be manipulated
    QList<Session *> sessions = sessionlist.values();
    QList<Day *> days = day.values();

    // Clean up any loaded sessions from memory first..
    //bool success = true;
    for (int i=0; i < sessions.size(); ++i) {
        Session * sess = sessions[i];
        if (!sess->Destroy()) {
            qDebug() << "Could not destroy "+ info.loadername +" ("+info.serial+") session" << sess->session();
      //      success = false;
        } else {
//            sessionlist.remove(sess->session());
        }

        delete sess;
    }
    // Make sure there aren't any dangling references to this device
    for (auto & d : days) {
        d->removeMachine(this);
    }

    // Remove EVERYTHING under Events folder..
    QString eventspath = getEventsPath();
    QDir evdir(eventspath);
    evdir.removeRecursively();

    QString summariespath = getSummariesPath();
    QDir sumdir(summariespath);
    sumdir.removeRecursively();


    // Clean up any straggling files (like from short sessions not being loaded...)
    dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    dir.setSorting(QDir::Name);

    const QFileInfoList & list = dir.entryInfoList();
    int could_not_kill = 0;

    for (const auto & fi : list) {
        QString fullpath = fi.canonicalFilePath();

        QString ext_s = fullpath.section('.', -1);
        bool ok;
        ext_s.toInt(&ok, 10);

        if (ok) {
            qDebug() << "Deleting " << fullpath;
            if (!dir.remove(fullpath)) {
                qDebug() << "Could not purge file" << fullpath;
                //success=false;
                could_not_kill++;
            }
        } else {
            qDebug() << "Didn't bother deleting cruft file" << fullpath;
            // cruft file..
        }
    }

    if (could_not_kill > 0) {
        qWarning() << "Could not purge path" << could_not_kill << "files in " << path;
        return false;
    }

    return true;
}

void Machine::setLoaderName(QString value)
{
    info.loadername = value;
    m_loader = GetLoader(value);
}

void Machine::rebuildCorrections(const QList<TimeCorrectionRow>& rows)
{
    m_correctionRows = rows;
    m_correctionCache.clear();
}

void Machine::reloadCorrectionsFromDb(Machine* mach)
{
    DeviceTimeCorrectionRepository repo;
    QList<DeviceTimeCorrectionData> dbRows = repo.findActive(mach->getDatabaseId());
    QList<TimeCorrectionRow> rows;
    rows.reserve(dbRows.size());
    for (const auto& d : dbRows) {
        TimeCorrectionRow r;
        r.dateFrom = QDate::fromString(d.dateFrom, Qt::ISODate);
        r.dateTo   = d.dateTo.isEmpty() ? QDate() : QDate::fromString(d.dateTo, Qt::ISODate);
        r.type     = d.type;
        r.offsetMs = d.offsetMs;
        r.c0Ms     = d.c0Ms;
        // Backward compat: drift rows were previously stored with c1 = slope + 1.0.
        // Detect old format (c1 >= 1.0 on a drift row) and strip the sentinel.
        r.c1 = (d.type == "drift" && d.c1 >= 1.0) ? d.c1 - 1.0 : d.c1;
        rows.append(r);
    }
    mach->rebuildCorrections(rows);
}

qint64 Machine::correctionMs(QDate night) const
{
    auto it = m_correctionCache.find(night);
    if (it != m_correctionCache.end()) return it.value();

    qint64 total = 0;
    qint64 t_noon = QDateTime(night, QTime(12,0,0), Qt::UTC).toMSecsSinceEpoch();

    for (const auto& row : m_correctionRows) {
        QDate to = row.dateTo.isNull() ? QDate(9999,12,31) : row.dateTo;
        if (night < row.dateFrom || night > to) continue;

        if (row.type != "drift") {
            total += row.offsetMs;
        } else {
            // Drift row: positive = CPAP ahead of reference; subtract to align.
            total -= row.c0Ms + qint64(row.c1 * double(t_noon));
        }
    }

    m_correctionCache[night] = total;
    return total;
}

/*! \brief Copy the identity fields of \a info into an existing machines record.
    \param data      The record as currently stored; updated in place.
    \param info      The identity the loader (or the database) has just supplied.
    \param machineId OSCAR's internal machine ID, or 0 when it is not yet known.
    \return true when at least one column changed and \a data must be written back.

    A field is only overwritten when the incoming value is non-empty, so a loader that
    supplies nothing for a column (an oximeter with no serial, a device whose series the
    loader does not know) leaves the stored value alone.  Anything else that differs wins:
    a model name corrected by a loader update replaces the stored one whatever it said,
    which the earlier "only replace an empty or 'Unknown' model" rule could not do for
    placeholders such as ResMed's "Resmed ???".

    This is the single place that decides what gets written back, shared by
    Machine::setInfo() and Machine::SaveToDatabase() so the two cannot diverge. */
static bool applyIdentityTo(MachineData & data, const MachineInfo & info, MachineID machineId)
{
    bool changed = false;

    auto setIfChanged = [&changed](QString & stored, const QString & incoming) {
        if (incoming.isEmpty() || stored == incoming) return;
        stored = incoming;
        changed = true;
    };

    setIfChanged(data.brand,        info.brand);
    setIfChanged(data.model,        info.model);
    setIfChanged(data.modelNumber,  info.modelnumber);
    setIfChanged(data.serialNumber, info.serial);
    setIfChanged(data.series,       info.series);

    // A machine ID of 0 means "not assigned yet", and a data version of 0 means the loader
    // does not declare one; neither should overwrite a good stored value.
    if (machineId != 0 && data.machineId != machineId) {
        data.machineId = machineId;
        changed = true;
    }
    if (info.version != 0 && data.dataVersion != info.version) {
        data.dataVersion = info.version;
        changed = true;
    }

    return changed;
}

/*! \brief True when \a a and \a b differ in any field applyIdentityTo() would write. */
static bool identityDiffers(const MachineInfo & a, const MachineInfo & b)
{
    return a.brand   != b.brand   || a.model  != b.model   || a.modelnumber != b.modelnumber
        || a.serial  != b.serial  || a.series != b.series  || a.version     != b.version;
}

void Machine::setInfo(MachineInfo inf)
{
    const MachineInfo oldInfo = info;

    MachineInfo merged = inf;
    if (info.purgeDate.isValid()) merged.purgeDate = info.purgeDate;
    info = merged;
    m_loader = GetLoader(inf.loadername);

    // Some loaders (ResMed, BMC, BMCG3X, ...) look up an already-loaded Machine and call
    // setInfo() on it directly, bypassing Profile::CreateMachine(); others reuse an existing
    // Machine object through CreateMachine()'s dedup branches. Either way, nothing else writes
    // an identity change back to the database: ImportContext::CreateMachineFromInfo() and
    // Profile::storeMachinesToDatabase() both skip machines that already have a database id.
    // Without this, a stale DB data_version makes Profile::DataFormatError()'s "needs upgrade"
    // prompt reappear on every subsequent launch even after the user has upgraded, and a model
    // name the loader has learned since the record was written is discarded at shutdown.
    //
    // The in-memory comparison comes first so that the common case — re-importing a device
    // whose identity has not changed — costs nothing instead of a SELECT per machine.
    if (m_database_id > 0 && identityDiffers(oldInfo, info)) {
        MachineRepository repo;
        MachineData data = repo.findById(m_database_id);
        if (data.id > 0) {
            const int storedVersion = data.dataVersion;
            if (applyIdentityTo(data, info, m_id)) {
                qDebug() << "Machine::setInfo(): Persisting identity update for" << info.loadername
                         << info.serial << "model" << data.model
                         << "data version" << storedVersion << "->" << data.dataVersion;
                repo.update(data);
            }
        }
    }
}

QString toHexid(quint32 id) { return QString("%1").arg(id,8,16,QLatin1Char('0')); };
QString Machine::hexid() { return toHexid((qint32)m_id);};

const QString Machine::getDataPath()
{
    // TODO: Rework the underlying database so that file storage doesn't rely on consistent presence or absence of the serial number.
    m_dataPath = p_pref->Get("{home}/Profiles/")+profile->user->userName()+"/"+info.loadername + "_"
                 + (info.serial.isEmpty() ? hexid() : info.serial) + "/";
    return m_dataPath;
}
const QString Machine::getSummariesPath()
{
    return getDataPath() + "Summaries/";
}
const QString Machine::getEventsPath()
{
    return getDataPath() + "Events/";
}
const QString Machine::getBackupPath()
{
    qDebug() << "Backup Path is " + getDataPath() + "Backup/";
    return getDataPath() + "Backup/";
}

// dirSize lazily pinched from https://stackoverflow.com/questions/47854288/can-not-get-directory-size-in-qt-c, thank's "Mike"
qint64 dirSize(QString dirPath) {
    qint64 size = 0;

    QDir dir(dirPath);
    QDir::Filters fileFilters = QDir::Files | QDir::System | QDir::Hidden;
    for(QString filePath : dir.entryList(fileFilters)) {
        QFileInfo fi(dir, filePath);
        size += fi.size();
    }

    QDir::Filters dirFilters = QDir::Dirs | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden;
    for(QString childDirPath : dir.entryList(dirFilters))
        size += dirSize(dirPath + QDir::separator() + childDirPath);

    return size;
}

qint64 Machine::diskSpaceSummaries()
{
    return dirSize(getSummariesPath());
}
qint64 Machine::diskSpaceEvents()
{
    return dirSize(getEventsPath());
}
qint64 Machine::diskSpaceBackups()
{
    return dirSize(getBackupPath());
}

bool Machine::Load(ProgressDialog *progress)
{
#ifndef UNITTEST_MODE
    // Add loader pixmap to progress
    // Note: not used during test due to non-QGuiApplication quirks
    QPixmap image = getPixmap();
    if (!image.isNull()) {
        image = image.scaled(64,64);
        progress->setPixmap(image);
    }
#endif
    progress->setMessage(QObject::tr("Loading %1 data for %2...").arg(info.brand).arg(profile->user->userName()));

    // Sessions are read only from the database. The device's on-disk folder
    // (e.g. ResMed_12345678/) is not needed and may not exist for restored profiles.
    if (m_database_id == 0) {
        qWarning() << "Machine::Load: device" << info.serial << "is not in the database";
        return false;
    }

    // No sessions is not an error: a device keeps its record after all of its
    // sessions are purged or deleted.
    if (LoadSessionsFromDatabase(progress)) {
        qDebug() << "Machine::Load: Loaded" << sessionlist.size() << "sessions from database for machine" << info.serial;
    }

    // Remove the legacy Sessions.info file if present — enabled state is
    // now stored in the DB and this file is no longer read or written.
    QFile sessionInfoFile(getDataPath() + "Sessions.info");
    if (sessionInfoFile.exists()) {
        sessionInfoFile.remove();
        qDebug() << "Machine::Load: Removed legacy Sessions.info for machine" << info.serial;
    }

    return true;
}

bool Machine::SaveSession(Session *sess)
{
    QString path = getDataPath();

    if (sess->IsChanged() && sess->first() != 0) {
        sess->Store(path);
    }

    return true;
}

//Session *Machine::popSaveList()
//{
//    Session *sess = nullptr;
//    listMutex.lock();
//
//    if (!m_savelist.isEmpty()) {
//        sess = m_savelist.at(0);
//        m_savelist.pop_front();
//        m_donetasks++;
//    }
//
//    listMutex.unlock();
//    return sess;
//}

void Machine::queTask(ImportTask * task)
{
    if (AppSetting->multithreading()) {
        qDebug() << "Machine::queTask() Using multithreading";
        m_tasklist.push_back(task);
        return;
    }

    // Not multithreading, run it right now...
    task->run();
    delete task;  // the threadpool path auto-deletes, this one doesn't
    return;
}

void Machine::runTasks()
{
    if (m_tasklist.isEmpty()) {
        qDebug() << "Machine::runTasks(): No tasks in m_tasklist";
        return;
    }
    qDebug() << "Machine::runTasks(): m_tasklist size is" << m_tasklist.size();
    int taskNum = 0;

    QThreadPool * threadpool = QThreadPool::globalInstance();
/***********************************************************
//  int m_totaltasks=m_tasklist.size();
//  int m_currenttask=0;
//  if (loader())
//      emit loader()->setProgressMax(m_totaltasks);
***********************************************************/
    while ( ! m_tasklist.isEmpty()) {
        if (threadpool->tryStart(m_tasklist.at(0))) {
            m_tasklist.pop_front();
            qDebug() << "Machine:runTasks running task" << ++taskNum;
/************************************************************
//          if (loader()) {
//              emit loader()->setProgressValue(++m_currenttask);
//              QApplication::processEvents();
//          }
***************************************************************/
        }
    }
    int activeCount = threadpool->activeThreadCount();
    qDebug() << "Machine::runTasks:" << activeCount << "threads running";
    threadpool->waitForDone(-1);
}

bool Machine::hasModifiedSessions()
{
    QHash<SessionID, Session *>::iterator s;

    for (s = sessionlist.begin(); s != sessionlist.end(); s++) {
        if (s.value()->IsChanged()) {
            return true;
        }
    }
    return false;
}

bool Machine::Save()
{
    //int size;
    // int cnt = 0;

    qDebug() << "Machine::Save entered";

    QString path = getDataPath();
    QDir dir(path);

    if (!dir.exists()) {
        dir.mkdir(path);
    }

    // IMPORTANT: Save machine to database FIRST so it has a database ID
    // This must happen before sessions are saved
    if (m_database_id == 0) {
        qDebug() << "Machine::Save(): Machine not in database, calling SaveToDatabase()";
        if (!SaveToDatabase()) {
            qWarning() << "Machine::Save(): Failed to save machine to database, sessions will not be saved";
        } else {
            qDebug() << "Machine::Save(): Machine saved successfully with database ID" << m_database_id;
        }
    }

    QHash<SessionID, Session *>::iterator s;

    // Settle this device's reported-channel set before any SaveTask runs: the tasks
    // write session_summaries in parallel and each row's NULL columns depend on it
    // (Session::StoreSummaryToDatabase). The pre-pass also detects a device reporting
    // a channel for the first time after rows already exist — a G3X's first central
    // hypopnea weeks in, a ResMed's first RERA — and rebuilds its earlier rows, which
    // were stored NULL for that channel (GitLab #261).
    const QSet<ChannelID> before = m_reportedChannels;
    bool hasStoredRows = false;
    for (s = sessionlist.begin(); s != sessionlist.end(); s++) {
        if ((*s)->IsChanged()) noteReportedChannels(*s);
        if ((*s)->sessionRowId() != 0) hasStoredRows = true;
    }
    if (hasStoredRows && m_database_id > 0 && m_reportedChannels != before) {
        qDebug() << "Machine::Save(): device reports new channels; rebuilding stored summaries for machine" << m_database_id;
        QSqlDatabase db = DatabaseManager::instance().database();
        if (SessionSummariesRepository::rebuildFromChannels(db, m_database_id)) {
            // Daily rows: recompute this device's days. calculateAndStoreFromDay() is an
            // INSERT OR REPLACE, so nothing needs invalidating first. The post-import
            // pass may cover only the imported days (ImportContext::Commit), so the
            // older days are not left to it.
            DailySummaryRepository dailyRepo;
            const qint64 profileId = getProfileId();
            for (auto it = day.begin(); it != day.end(); ++it) {
                Day * d = it.value();
                if (d && d->hasEnabledSessions()) dailyRepo.calculateAndStoreFromDay(d, profileId);
            }
        }
    }

//  m_savelist.clear();

    // Store any event summaries to files (existing behavior)
    for (s = sessionlist.begin(); s != sessionlist.end(); s++) {
        // cnt++;

        if ((*s)->IsChanged()) {
            queTask(new SaveTask(*s, this));
        }
    }

    qDebug() << "Machine::Save(): calling runTasks()";
    runTasks();

    // NOW save all sessions to database in a SINGLE TRANSACTION (machine now has a database ID)
    // Check again after SaveToDatabase() call above
    if (m_database_id > 0) {
        qDebug() << "Machine::Save(): Machine database ID is" << m_database_id 
                 << "- saving" << sessionlist.size() << "sessions to database";
        
        // Check if we're already in a transaction (e.g., during profile import)
        // If so, don't create a nested transaction - just save the sessions
        DatabaseManager& dbMgr = DatabaseManager::instance();
        bool alreadyInTransaction = dbMgr.inTransaction();
        bool ownTransaction = false;
        
        if (!alreadyInTransaction) {
            // Begin our own transaction for batch session save
            SessionRepository sessionRepo;
            if (!sessionRepo.beginTransaction()) {
                qWarning() << "Machine::Save(): Failed to begin transaction for batch session save";
            } else {
                ownTransaction = true;
                qDebug() << "Machine::Save(): Started transaction for batch session save";
            }
        } else {
//            qDebug() << "Machine::Save(): Already in transaction - skipping nested transaction";
        }
        
        int savedCount = 0;
        int skippedCount = 0;
        
        // Save all sessions (within existing or new transaction)
        for (s = sessionlist.begin(); s != sessionlist.end(); s++) {
            Session *sess = s.value();
            // Save new sessions (never persisted) or sessions explicitly marked changed.
            // Skipping unchanged DB-loaded sessions prevents overwriting correct
            // percentile values with 0 when events are not in memory.
            if (sess->first() != 0
                    && (sess->sessionRowId() == 0 || sess->IsChanged())) {
                if (sess->StoreToDatabase()) {
                    savedCount++;
                } else {
                    skippedCount++;
                    // Don't fail the entire batch for one session failure
                    qWarning() << "Machine::Save(): Failed to save session" << sess->session() << "- continuing";
                }
            } else {
                skippedCount++;
            }
        }
        
        // Commit our transaction if we started one
        if (ownTransaction) {
            SessionRepository sessionRepo;
            if (sessionRepo.commitTransaction()) {
                qDebug() << "Machine::Save(): Transaction committed -" << savedCount << "sessions saved to database," << skippedCount << "skipped/failed";
            } else {
                qWarning() << "Machine::Save(): Failed to commit transaction - rolling back";
                sessionRepo.rollbackTransaction();
            }
        } else {
            qDebug() << "Machine::Save(): Saved" << savedCount << "sessions to database," << skippedCount << "skipped/failed (parent transaction will commit)";
        }
    } else {
        qWarning() << "Machine::Save(): Machine database ID is 0, cannot save" << sessionlist.size() << "sessions to database";
    }

    return true;
}

void Machine::updateChannels(Session * sess)
{
    int size = sess->m_availableChannels.size();
    for (int i=0; i < size; ++i) {
        ChannelID code = sess->m_availableChannels.at(i);
        m_availableChannels[code] = true;
    }

    size = sess->m_availableSettings.size();
    for (int i=0; i < size; ++i) {
        ChannelID code = sess->m_availableSettings.at(i);
        m_availableSettings[code] = true;
    }
}

void Machine::noteReportedChannels(Session * sess)
{
    // A session loaded from the database carries its counts in m_cnt ...
    for (auto it = sess->m_cnt.cbegin(); it != sess->m_cnt.cend(); ++it) {
        if (it.value() > 0) m_reportedChannels.insert(it.key());
    }
    // ... while one built by a loader still has its events in the EventLists.
    for (auto it = sess->eventlist.cbegin(); it != sess->eventlist.cend(); ++it) {
        if (m_reportedChannels.contains(it.key())) continue;
        for (EventList * el : it.value()) {
            if (el && el->count() > 0) {
                m_reportedChannels.insert(it.key());
                break;
            }
        }
    }
}

QList<ChannelID> Machine::availableChannels(quint32 chantype)
{
    QList<ChannelID> list;

    QHash<ChannelID, bool>::iterator end = m_availableChannels.end();
    QHash<ChannelID, bool>::iterator it;
    for (it = m_availableChannels.begin(); it != end; ++it) {
        ChannelID code = it.key();
        const schema::Channel & chan = schema::channel[code];
        if (chan.type() & chantype) {
            list.push_back(code);
        }
    }
    return list;
}

bool Machine::LoadSessionsFromDatabase(ProgressDialog *progress)
{
    if (m_database_id == 0) {
        qDebug() << "Machine::LoadSessionsFromDatabase(): Machine not in database";
        return false;
    }

    SessionRepository repo;
    QList<SessionData> sessions = repo.findByMachine(m_database_id);
    
    if (sessions.isEmpty()) {
        qDebug() << "Machine::LoadSessionsFromDatabase(): No sessions found in database for machine" << m_database_id;
        return false;
    }
    
    qDebug() << "Machine::LoadSessionsFromDatabase(): Loading" << sessions.size() << "sessions from database";
    
    progress->setProgressMax(sessions.size());
    progress->setMessage(QObject::tr("Loading %1 sessions from database...").arg(sessions.size()));
    
    int loaded = 0;
    for (const SessionData& data : sessions) {
        if ((loaded % 20) == 0) {
            progress->setProgressValue(loaded);
            QApplication::processEvents();
        }
        
        // Create session object
        Session* sess = new Session(this, data.sessionId);
        
        // Load from database
        if (sess->LoadFromDatabase()) {
            // m_cnt is populated now and the event lists are empty, so this is the
            // point to learn which channels the device has actually scored.
            noteReportedChannels(sess);
            // Add to machine (this creates Day objects)
            if (AddSession(sess, true)) {
                loaded++;
            } else {
                delete sess;
            }
        } else {
            qWarning() << "Failed to load session" << data.sessionId << "from database";
            delete sess;
        }
    }
    
    progress->setProgressValue(loaded);
    qDebug() << "Machine::LoadSessionsFromDatabase(): Loaded" << loaded << "sessions successfully";
    
    return loaded > 0;
}

/*!
 * \brief Returns the profile ID this machine belongs to (Schema v12)
 * \return Profile ID from the profile object, or 0 if no profile
 */
qint64 Machine::getProfileId() const
{
    if (profile) {
        return profile->getDatabaseId();
    }
    return 0;
}

bool Machine::SaveToDatabase()
{
    if (m_database_id > 0) {
        qDebug() << "Machine::SaveToDatabase(): Machine already in database with ID" << m_database_id;
        return true;  // Already saved
    }

    // A CPAP with neither serial nor model means the loader failed to identify the card, so no
    // record is created. Oximetry, position, sleep-stage and journal importers may legitimately
    // supply neither (e.g. a Somnopose CSV without the optional model=/serial= headers): these
    // are the single-instance types Profile::CreateMachine() recognises, one machine per
    // loader, identified by (serial "", loader, profile).
    const bool singleInstance = (info.type == MT_OXIMETER) || (info.type == MT_POSITION)
                             || (info.type == MT_SLEEPSTAGE) || (info.type == MT_JOURNAL);
    if (info.loadername.isEmpty() || (!singleInstance && info.serial.isEmpty() && info.model.isEmpty())) {
        qWarning() << "Machine::SaveToDatabase(): Cannot save machine without serial or model";
        return false;
    }

    MachineRepository repo;
    ProfileRepository profileRepo;

    // Get the profile ID for this machine's profile
    QString profileName = profile->user->userName();
    qDebug() << "Machine::SaveToDatabase(): Looking up profile" << profileName 
             << "for machine" << info.serial << info.loadername;
    
    ProfileData profileData = profileRepo.findByUsername(profileName);
    if (profileData.id == 0) {
        qWarning() << "Machine::SaveToDatabase(): Profile" << profileName << "not in database yet";
        return false;
    }
    
    qDebug() << "Machine::SaveToDatabase(): Found profile" << profileName 
             << "with database ID" << profileData.id;
    
    // IMPORTANT: Set the profile's database ID so getProfileId() works
    if (profile->getDatabaseId() == 0) {
        profile->setDatabaseId(profileData.id);
        qDebug() << "Machine::SaveToDatabase(): Set profile database ID to" << profileData.id;
    }

    // Check if this machine already exists in THIS PROFILE's database.
    // Primary lookup: by serial+loader+profile (handles renamed machines and backup restore).
    MachineData existing = repo.findBySerialLoaderAndProfile(info.serial, info.loadername, profileData.id);

    // Fallback lookup: by (profile_id, machine_id).  Needed when a machine was migrated from
    // OSCAR 1.x with an empty serial — the DB record has serial_number="" while the loader
    // now supplies the real serial, so the primary lookup finds nothing even though the record
    // is there.  Only accept the fallback if the loader name matches to avoid stealing a record
    // that belongs to a different loader.
    if (existing.id == 0 && m_id != 0) {
        MachineData byId = repo.findByProfileAndMachineId(profileData.id, m_id);
        if (byId.id > 0 && byId.loaderName == info.loadername) {
            qDebug() << "Machine::SaveToDatabase(): Serial lookup missed; found by machine_id"
                     << m_id << "(DB serial was" << byId.serialNumber << ", loader provides" << info.serial << ")";
            existing = byId;
        }
    }

    if (existing.id > 0) {
        // Machine already exists in this profile, reuse the existing ID
        m_database_id = existing.id;

        // Update machine_id and any info fields that may have changed since the record
        // was first written (e.g. series corrected after a loader bug fix, machineId changed
        // after a rebuild-from-backup, or serial updated after a 1.x migration).
        MachineData updateData = existing;
        if (applyIdentityTo(updateData, info, m_id)) {
            qDebug() << "Machine::SaveToDatabase(): Updating machine record for" << info.serial
                     << "series:" << existing.series << "->" << updateData.series
                     << "model:" << existing.model << "->" << updateData.model;
            repo.update(updateData);
        }

        qDebug() << "Machine::SaveToDatabase(): Found existing machine"
                 << info.serial << "in profile" << profile->user->userName()
                 << "with database ID" << m_database_id;
        return true;
    }

    // No existing machine found, create a new one
    MachineData data;

    // Use the actual profile ID we just looked up
    data.profileId = profileData.id;
    data.machineId = m_id;  // OSCAR's internal machine ID
    data.machineType = static_cast<int>(info.type);
    data.brand = info.brand;
    data.model = info.model;
    data.modelNumber = info.modelnumber;
    data.serialNumber = info.serial;
    data.series = info.series;
    data.loaderName = info.loadername;
    data.lastImported = info.lastimported.toString(Qt::ISODate);

    // Store capabilities in properties field (JSON format)
    data.properties = QString("{\"capabilities\":%1}").arg(info.cap);

    qint64 newId = repo.create(data);
    if (newId < 0) {
        qWarning() << "Machine::SaveToDatabase(): Failed to save machine to database";
        return false;
    }

    m_database_id = newId;
    qDebug() << "Machine::SaveToDatabase(): Saved machine" << info.serial << "to database with ID" << m_database_id;

    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////
// CPAP implmementation
//////////////////////////////////////////////////////////////////////////////////////////
CPAP::CPAP(Profile * profile, MachineID id): Machine(profile, id)
{
    m_type = MT_CPAP;
}

CPAP::~CPAP()
{
}

//////////////////////////////////////////////////////////////////////////////////////////
// Oximeter Class implmementation
//////////////////////////////////////////////////////////////////////////////////////////
Oximeter::Oximeter(Profile * profile, MachineID id): Machine(profile, id)
{
    m_type = MT_OXIMETER;
}

Oximeter::~Oximeter()
{
}

//////////////////////////////////////////////////////////////////////////////////////////
// SleepStage Class implmementation
//////////////////////////////////////////////////////////////////////////////////////////
SleepStage::SleepStage(Profile * profile, MachineID id): Machine(profile, id)
{
    m_type = MT_SLEEPSTAGE;
}
SleepStage::~SleepStage()
{
}

//////////////////////////////////////////////////////////////////////////////////////////
// PositionSensor Class implmementation
//////////////////////////////////////////////////////////////////////////////////////////
PositionSensor::PositionSensor(Profile * profile, MachineID id): Machine(profile, id)
{
    m_type = MT_POSITION;
}
PositionSensor::~PositionSensor()
{
}
