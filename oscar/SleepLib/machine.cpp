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
#include <QDomDocument>
#include <QDomElement>


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

void LoadTask::run()
{
    sess->LoadSummary();
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
//    saveSessionInfo();
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

const quint16 sessinfo_version = 2;

bool Machine::saveSessionInfo()
{
    if (info.type == MT_JOURNAL) return false;
    if (sessionlist.size() == 0) return false;

    qDebug() << "Machine::saveSessionInfo(): Saving" << info.brand << "session info" << info.loadername;
    QString filename = getDataPath() + "Sessions.info";
    QFile file(filename);
    if (!file.open(QFile::WriteOnly)) {
        qWarning() << "Machine::saveSessionInfo(): Couldn't open" << filename << "for writing, error code" << file.error() << file.errorString();
        return false;
    }

    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setVersion(QDataStream::Qt_5_0);

    out << magic;
    out << filetype_sessenabled;
    out << sessinfo_version;

    QHash<SessionID, Session *>::iterator s;

    out << (int)sessionlist.size();
    for (s = sessionlist.begin(); s != sessionlist.end(); ++s) {
        Session * sess = s.value();
        if (sess->realFirst() != 0) {
            out << (quint32) sess->session();
            out << (quint8)(sess->enabled(true));
        } else {
            qWarning() << "Machine::SaveSessionInfo(): discarding session" << sess->session()
                       << "["+QDateTime::fromSecsSinceEpoch(sess->session()).toString("MMM dd, yyyy hh:mm:ss")+"]"
                       << "from machine" << serial() << "with first=0";
        }

        //out << sess->m_availableChannels;
    }
    qDebug() << "Machine::saveSessionInfo(): Done Saving" << info.brand << "session info";

    return true;
}

bool Machine::loadSessionInfo()
{
    // SessionInfo basically just contains a list of all sessions and their enabled status,
    // so the enabling/reenabling doesn't require a summary rewrite every time.

    if (info.type == MT_JOURNAL)
        return true;

    QHash<SessionID, Session *>::iterator s;
    QFile file(getDataPath() + "Sessions.info");

    if ( ! file.open(QFile::ReadOnly)) {
        // No session.info file present, so let's create one...

        // But first check for legacy SESSION_ENABLED field in session settings
        for (s = sessionlist.begin(); s!= sessionlist.end(); ++s) {
            Session * sess = s.value();
            QHash<ChannelID, QVariant>::iterator it = sess->settings.find(SESSION_ENABLED);

            quint8 b = true;
            b &= 0x1;
            if (it != sess->settings.end()) {
                b = it.value().toBool();
            } else {
            }
            sess->setEnabled(b);        // Extract from session settings and save..
        }

        // Now write the file
        saveSessionInfo();
        return true;
    }

    QDataStream in(&file);
    in.setByteOrder(QDataStream::LittleEndian);
    in.setVersion(QDataStream::Qt_5_0);

    quint32 mag32;
    in >> mag32;

    quint16 ft16, version;
    in >> ft16;
    in >> version;

    // Legacy crud
    if (version == 1) {
        // was available channels
        QHash<ChannelID, bool> crap;
        in >> crap;
    }


    // Read in size record, followed by size * [SessionID, bool] pairs containing the enable status.
    int size;
    in >> size;

    quint32 sid;
    quint8 b;

    for (int i=0; i< size; ++i) {
        in >> sid;
        in >> b;
        b &= 0x1;

        s = sessionlist.find(sid);

        if (s != sessionlist.end()) {
            Session * sess = s.value();

            sess->setEnabled(b);
        }
    }
    return true;
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

void Machine::setInfo(MachineInfo inf)
{
    int oldVersion = info.version;

    MachineInfo merged = inf;
    if (info.purgeDate.isValid()) merged.purgeDate = info.purgeDate;
    info = merged;
    m_loader = GetLoader(inf.loadername);

    // Some loaders (ResMed, BMC, BMCG3X, ...) look up an already-loaded Machine and call
    // setInfo() on it directly, bypassing Profile::CreateMachine(); others reuse an existing
    // Machine object through CreateMachine()'s dedup branches. Either way, nothing else writes
    // a version bump back to the database: ImportContext::CreateMachineFromInfo() and
    // Profile::storeMachinesToDatabase() both skip machines that already have a database id.
    // Without this, a stale DB data_version makes Profile::DataFormatError()'s "needs upgrade"
    // prompt reappear on every subsequent launch even after the user has upgraded.
    if (m_database_id > 0 && info.version != oldVersion) {
        MachineRepository repo;
        MachineData data = repo.findById(m_database_id);
        if (data.id > 0 && data.dataVersion != info.version) {
            data.dataVersion = info.version;
            repo.update(data);
            qDebug() << "Machine::setInfo(): Persisted version bump for" << info.loadername << info.serial
                     << "from" << oldVersion << "to" << info.version;
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

    // Try loading from database first.  This must happen before the on-disk
    // directory check because restored profiles have all data in the database
    // and the machine subdirectory (e.g. ResMed_12345678/) may not exist yet.
    if (m_database_id > 0 && LoadSessionsFromDatabase(progress)) {
        qDebug() << "Machine::Load: Loaded" << sessionlist.size() << "sessions from database for machine" << info.serial;

        // Remove the legacy Sessions.info file if present — enabled state is
        // now stored in the DB and this file is no longer read or written.
        QFile sessionInfoFile(getDataPath() + "Sessions.info");
        if (sessionInfoFile.exists()) {
            sessionInfoFile.remove();
            qDebug() << "Machine::Load: Removed legacy Sessions.info for machine" << info.serial;
        }

        return true;
    }

    // Fall back to file-based loading (legacy .000/.001 files).
    // The on-disk machine directory must exist for this path.
    QString path = getDataPath();
    QDir dir(path);
    qDebug() << "Machine::Load: Loading" << info.loadername.toLocal8Bit().data() << "record:" << path.toLocal8Bit().data();

    if (!dir.exists() || !dir.isReadable()) {
        return false;
    }

    if (loader()) {
        mainwin->connect(loader(), SIGNAL(updateMessage(QString)), progress, SLOT(setMessage(QString)));
        mainwin->connect(loader(), SIGNAL(setProgressMax(int)),   progress, SLOT(setProgressMax(int)));
        mainwin->connect(loader(), SIGNAL(setProgressValue(int)), progress, SLOT(setProgressValue(int)));
    }

    if ( ! LoadSummary(progress)) {
        qDebug() << "Machine::Load: Recreating the Summary index XML file";
        // No XML index file, so assume upgrading, or it simply just got screwed up or deleted...
        progress->setMessage(QObject::tr("Scanning Files"));
        progress->setProgressValue(0);
        QApplication::processEvents();

        QElapsedTimer time;
        time.start();
        dir.setFilter(QDir::Files | QDir::Hidden | QDir::NoSymLinks);

        ///////////////////////////////////////////////////////////////////////
        // Read summary files from Summaries/ subdirectory
        ///////////////////////////////////////////////////////////////////////
        QString summarypath = getSummariesPath();
        dir.setPath(summarypath);
        QStringList filters;
        filters << "*.000";
        dir.setNameFilters(filters);
        QStringList filelist = dir.entryList();
        int size = filelist.size();

        progress->setMessage(QObject::tr("Reading summary files"));
        qDebug() << "Machine::Load: Reading summary files (.000)";
        progress->setProgressValue(0);
        QApplication::processEvents();

        QString sesstr;
        SessionID sessid;
        bool ok;

        for (int i=0; i < size; i++) {

            if ((i % 20) == 0) { // This is slow.. :-/
                progress->setProgressValue(i);
                QApplication::processEvents();
            }

            QString filename = filelist.at(i);
            sesstr = filename.section(".", 0, -2);
            sessid = sesstr.toLong(&ok, 16);

            if (!ok) { continue; }

            Session *sess = new Session(this, sessid);

            // Forced to load it, because know nothing about this session..
            if (sess->LoadSummary()) {
                AddSession(sess, true);
            } else {
                qWarning() << "Machine::Load: Error loading summary file" << filename;
                delete sess;
            }
        }

        SaveSummaryCache();
        qDebug() << "Machine::Load: Loaded" << info.model.toLocal8Bit().data() << "data in" << time.elapsed() << "ms";
        progress->setProgressValue(size);
    }
    progress->setMessage(QObject::tr("Loading Session Info"));
    qDebug() << "Machine::Load: Loading Session Info";
    QApplication::processEvents();

    loadSessionInfo();

    if (loader()) {
        mainwin->disconnect(loader(), SIGNAL(updateMessage(QString)), progress, SLOT(setMessage(QString)));
        mainwin->disconnect(loader(), SIGNAL(setProgressMax(int)),   progress, SLOT(setProgressMax(int)));
        mainwin->disconnect(loader(), SIGNAL(setProgressValue(int)), progress, SLOT(setProgressValue(int)));
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

const QString summaryFileName = "Summaries.xml";
const int summaryxml_version=1;

bool Machine::LoadSummary(ProgressDialog * progress)
{
    QElapsedTimer time;
    time.start();
    QString filename = getDataPath() + summaryFileName + ".gz";

    QDomDocument doc;
    QFile file(filename);
    qDebug() << "Loading" << filename.toLocal8Bit().data();
    progress->setMessage(QObject::tr("Loading Summaries.xml.gz"));
    QApplication::processEvents();

    if (!file.open(QIODevice::ReadOnly)) {
//        qWarning() << "Could not open" << filename;
        qWarning() << "Could not open" << filename << "for reading, error code" << file.error() << file.errorString();
        return false;
    }

    QByteArray data = file.readAll();
    QByteArray uncompressed = gUncompress(data);

#if QT_VERSION < QT_VERSION_CHECK(6,8,0)

    QString errorMsg;
    int errorLine;
    int errorColumn;
    if (!doc.setContent(uncompressed, false, &errorMsg, &errorLine, &errorColumn)) {
        qWarning() << "Invalid XML Content in" << filename
                   << "at line" << errorLine << ", column" << errorColumn
                   << ":" << errorMsg;
        file.close();
        return false;
    }
#else
    auto result = doc.setContent(uncompressed) ;
    if (!result) {
        qWarning() << "Invalid XML Content in" << filename
                   << "at line:" << result.errorLine << ", column: " <<  result.errorColumn
                   << ":" << result.errorMessage;
        file.close();
        return false;
    }
#endif

#if 0
///////////////////////////////////////////////////////////////////////////////////////////////
QByteArray uncompressed = gUncompress(data);
    QString errorMsg;
    int errorLine;
    int errorColumn;

    QDomDocument::ParseOptions options;
    options |= QDomDocument::ParseError; // Enable error reporting

    if (!doc.setContent(uncompressed, options, &errorMsg, &errorLine, &errorColumn)) {
        qWarning() << "Invalid XML Content in" << filename
                   << "at line" << errorLine << ", column" << errorColumn
                   << ":" << errorMsg;
        file.close();
        return false;
    }

////////////////////////////////////////////////////////////////////////////////////////////
#endif


    file.close();

    QDomElement root = doc.documentElement();

    if (root.tagName().compare("sessions", Qt::CaseInsensitive) != 0) {
        qDebug() << "Summaries cache messed up, recreating...";
        return false;
    }
    bool ok;
    int version = root.attribute("version", "").toInt(&ok);
    if (!ok || (version != summaryxml_version)) {
        qDebug() << "Summaries cache outdated, recreating...";
        return false;
    }
    QDomNode node;

    bool s_ok;

    QDomNodeList sessionlist = root.childNodes();

    int size = sessionlist.size();

    QMap<qint64, Session *>  sess_order;

    progress->setProgressMax(size);
    for (int s=0; s < size; ++s) {
        if ((s % 20) == 0) {
            progress->setProgressValue(s);
            QApplication::processEvents();
        }
        node = sessionlist.at(s);
        QDomElement e = node.toElement();
        SessionID sessid = e.attribute("id", "0").toLong(&s_ok);
        qint64 first =  e.attribute("first", "0").toLongLong();
        qint64 last =  e.attribute("last", "0").toLongLong();
        bool enabled = e.attribute("enabled", "1").toInt() == 1;
        bool events = e.attribute("events", "1").toInt() == 1;
        if (s_ok) {
            Session * sess = new Session(this, sessid);
            sess->really_set_first(first);
            sess->really_set_last(last);
            sess->setEnabled(enabled);
            sess->setSummaryOnly(!events);

            if (e.hasChildNodes()) {
                QList<ChannelID> available_channels;
                QList<ChannelID> available_settings;

                QDomElement chans = e.firstChildElement("channels");
                if (chans.isElement()) {
                    QDomNode node = chans.firstChild();
                    QString txt = node.nodeValue();
                    QStringList channels = txt.split(",");
                    for (int i=0; i<channels.size(); ++i) {
                        bool ok;
                        ChannelID code = channels.at(i).toInt(&ok, 16);
                        available_channels.append(code);
                    }
                }
                sess->m_availableChannels = available_channels;

                QDomElement sete = e.firstChildElement("settings");
                if (sete.isElement()) {
                    QString sets = sete.firstChild().nodeValue();
                    QStringList settings = sets.split(",");
                    for (int i=0; i<settings.size(); ++i) {
                        bool ok;
                        ChannelID code = settings.at(i).toInt(&ok, 16);
                        available_settings.append(code);
                    }
                }
                sess->m_availableSettings = available_settings;
            }

            sess_order[first] = sess;
        }
    }
    QMap<qint64, Session *>::iterator it_end = sess_order.end();
    QMap<qint64, Session *>::iterator it;
    bool loadSummaries = profile->session->preloadSummaries();
    qDebug() << "PreloadSummaries is" << (loadSummaries ? "true" : "false");
    qDebug() << "Queue task loader is" << (loader() ? "" : "not ") << "available";
//  sleep(1);

//  progress->setMessage(QObject::tr("Queueing Open Tasks"));
//  QApplication::processEvents();
//  int cnt = 0;

//  progress->setMaximum(sess_order.size());

    for (it = sess_order.begin(); it != it_end; ++it /*, ++cnt*/ ) {
/****************************************************************
//      if ((cnt % 100) == 0) {
//          progress->setValue(cnt);
//          //QApplication::processEvents();
//      }
*****************************************************************/
        Session * sess = it.value();
        if ( ! AddSession(sess, true)) {
            delete sess;
        } else {
            if (loadSummaries) {
                if (loader()) {
                    loader()->queTask(new LoadTask(sess,this));
                } else {
                    // no progress bar
                    queTask(new LoadTask(sess,this));
                }
            }
        }
    }
    progress->setMessage(QObject::tr("Loading Summary Data"));
    qDebug() << "Loading Summary Data";
    QApplication::processEvents();

    if (loader()) {
        qDebug() << "Machine::LoadSummary calling runTasks in loader";
        loader()->runTasks();
    } else {
        qDebug() << "Machine::LoadSummary calling runTasks";
        runTasks();
    }
    progress->setProgressValue(sess_order.size());
    QApplication::processEvents();

    // IMPORTANT: Ensure all sessions loaded from files know their machine's database ID
    // This is required so sessions can be saved to database correctly on profile reopen
    if (m_database_id > 0) {
        qDebug() << "Machine::LoadSummary(): Setting machine_id=" << m_database_id 
                 << "on" << this->sessionlist.size() << "sessions loaded from files";
        QHash<SessionID, Session *>::iterator sess_it;
        for (sess_it = this->sessionlist.begin(); sess_it != this->sessionlist.end(); ++sess_it) {
            Session* sess = sess_it.value();
            if (sess && sess->first() != 0) {
                sess->setSessionRowId(m_database_id);
            }
        }
    }

    qDebug() << "Loaded" << info.model.toLocal8Bit().data() << "data in" << time.elapsed() << "ms";

    return true;
}

bool Machine::SaveSummaryCache()
{
    qDebug() << "Machine::SaveSummaryCache: Saving" << info.brand << info.model <<  "Summaries";
    QString filename = getDataPath() + summaryFileName;

    QDomDocument doc("OSCAR_SessionIndex");

    QDomElement root = doc.createElement("sessions");
    root.setAttribute("version", summaryxml_version);
    root.setAttribute("profile", profile->user->userName());
    root.setAttribute("count", sessionlist.size());
    root.setAttribute("loader", info.loadername);
    root.setAttribute("serial", info.serial);

    doc.appendChild(root);

// No longer used as data is in database
//    if (!QDir().exists(getSummariesPath()))
//        QDir().mkpath(getSummariesPath());

    QHash<SessionID, Session *>::iterator s;
    QHash<SessionID, Session *>::iterator sess_end = sessionlist.end();

    for (s = sessionlist.begin(); s != sess_end; ++s) {
        QDomElement el = doc.createElement("session");
        Session * sess = s.value();
        el.setAttribute("id", (quint32)sess->session());
        el.setAttribute("first", sess->realFirst());
        el.setAttribute("last", sess->realLast());
        el.setAttribute("enabled", sess->enabled(true) ? "1" : "0");
        el.setAttribute("events", sess->summaryOnly() ? "0" : "1");

        QHash<ChannelID, QVector<EventList *> >::iterator ev;
        QHash<ChannelID, QVector<EventList *> >::iterator ev_end = sess->eventlist.end();
        QStringList chanlist;
        for (ev = sess->eventlist.begin(); ev != ev_end; ++ev) {
            chanlist.append(QString::number(ev.key(), 16));
        }
        if (chanlist.size() == 0) {
            for (int i=0; i<sess->m_availableChannels.size(); i++) {
                ChannelID code = sess->m_availableChannels.at(i);
                chanlist.append(QString::number(code, 16));
            }
        }

        QDomElement chans = doc.createElement("channels");
        chans.appendChild(doc.createTextNode(chanlist.join(",")));
        el.appendChild(chans);

        chanlist.clear();
        QHash<ChannelID, QVariant>::iterator si;
        QHash<ChannelID, QVariant>::iterator set_end = sess->settings.end();
        for (si = sess->settings.begin(); si != set_end; ++si) {
            chanlist.append(QString::number(si.key(), 16));
        }
        QDomElement settings = doc.createElement("settings");
        settings.appendChild(doc.createTextNode(chanlist.join(",")));
        el.appendChild(settings);

        root.appendChild(el);
        if (sess->IsChanged())
            sess->StoreSummary();
    }

    QString xmltext;
    QTextStream ts(&xmltext);
    doc.save(ts, 1);

    QByteArray data = gCompress(xmltext.toUtf8());

    QFile file(filename + ".gz");

    if (!file.open(QFile::WriteOnly)) {
        qWarning() << "Machine::SaveSummaryCache: Couldn't open summary cache" << filename << "for writing, error code" << file.error() << file.errorString();
    }
    file.write(data);

    return true;
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

    if (info.serial.isEmpty() && info.model.isEmpty()) {
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
        bool needsUpdate = (existing.machineId    != m_id)
                        || (existing.series        != info.series)
                        || (existing.model         != info.model)
                        || (existing.modelNumber   != info.modelnumber)
                        || (existing.serialNumber  != info.serial);
        if (needsUpdate) {
            MachineData updateData = existing;
            updateData.machineId    = m_id;
            updateData.serialNumber = info.serial;
            updateData.series       = info.series;
            updateData.model        = info.model;
            updateData.modelNumber  = info.modelnumber;
            qDebug() << "Machine::SaveToDatabase(): Updating machine record for" << info.serial
                     << "series:" << existing.series << "->" << info.series;
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
