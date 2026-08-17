/* SleepLib Session Implementation
 * This stuff contains the base calculation smarts
 *
 * Copyright (c) 2019-2026 The OSCAR Team
 * Copyright (c) 2011-2018 Mark Watkins
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#define TEST_MACROS_ENABLEDoff
#include "test_macros.h"

//#define DBDEBUG                       // for maximum database diagnostics

#include <cmath>
#include <QDebug>
#include <QMessageBox>
#include <QMetaType>
#include <algorithm>
#include <limits>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

#include "session.h"
#include "speedcheck.h"
#include "SleepLib/machine_common.h"
#include "SleepLib/calcs.h"
#include "SleepLib/profiles.h"
#include "SleepLib/performance_timer.h"

// Database repositories
#include "../database/session_repository.h"
#include "../database/session_settings_repository.h"
#include "../database/session_channels_repository.h"
#include "../database/session_channel_values_repository.h"
#include "../database/session_slices_repository.h"
#include "../database/session_summaries_repository.h"
#include "../database/event_list_repository.h"
#include "../database/event_data_repository.h"
#include "../database/respiratory_events_repository.h"

using namespace std;
#define FIX_FOR_SINGLE_EVENT            // fixes ibreeze "No valuesummary for channel" error during import.

// This is the uber important database version for OSCAR's internal storage
// Increment this after stuffing with Session's save & load code.
const quint16 summary_version = 18;
const quint16 events_version = 10;

Session::Session(Machine *m, SessionID session)
{
    s_lonesession = false;

    if (!session) {
        session = m->CreateSessionID();
    }

    s_machine = m;
    s_machtype = m->type();
    s_session = session;
    s_changed = false;
    s_events_loaded = false;
    s_summary_loaded = false;
    _first_session = true;
    s_enabled = true;

    s_first = s_last = 0;
    s_evchecksum_checked = false;

    s_noSettings = s_summaryOnly = false;
    
    m_sessionrow_id = 0;  // Initialize database ID to 0 (not in database)

    destroyed = false;
}

Session::~Session()
{
    TrashEvents();
    destroyed = true;
}

void Session::TrashEvents()
// Trash this sessions Events and release memory.
{
    QHash<ChannelID, QVector<EventList *> >::iterator i;
    QHash<ChannelID, QVector<EventList *> >::iterator i_end=eventlist.end();

    if (s_changed) {
        // Save first..
    }

    for (i = eventlist.begin(); i != i_end; ++i) {
        for (EventList * ev : i.value()) {
            delete ev;  // destructor handles cleanup; no need for clear()+squeeze()
        }
    }

    s_events_loaded = false;
    eventlist.clear();
    eventlist.squeeze();
}

bool Session::enabled(bool realValues) const
{
    if (p_profile->cpap->clinicalMode() && !realValues) return true;
    return s_enabled;
}

void Session::setEnabled(bool b)
{
    if (b == s_enabled) return;  // no change — skip DB write and cache invalidation
    s_enabled = b;

    if (m_sessionrow_id > 0) {
        SessionRepository repo;
        repo.updateEnabled(m_sessionrow_id, b);
    }

    // Invalidate the hours cache in the day record.
    Day *day = p_profile->findSessionDay(this);
    if (day) {
        day->invalidate();
    }
}

QString Session::eventFile() const
{
    return s_machine->getEventsPath()+toHexid(s_session)+".001";
}

//const int max_pack_size=128;
bool Session::OpenEvents(bool debug)
{
    if (s_events_loaded) {
        return true;
    }

    s_events_loaded = eventlist.size() > 0;

    if (s_events_loaded) {
        return true;
    }


    QString filename = eventFile();
#ifdef DEBUG_EVENTS
    if (debug) qDebug() << "Loading" << s_machine->loaderName().toLocal8Bit().data() << "Events:" << filename.toLocal8Bit().data();
#endif
    bool b = LoadEvents(filename,debug);

    if ( ! b) {
        if (debug) qWarning() << "Error Loading Events" << filename;
        return false;
    }

    return s_events_loaded = true;
}

bool Session::Destroy()
{
    QString base;
    base=toHexid(s_session);

    // Remove session from database — all child rows (channels, slices, events, etc.)
    // are deleted automatically via ON DELETE CASCADE foreign key constraints.
    if (m_sessionrow_id > 0) {
        SessionRepository repo;
        if (!repo.remove(m_sessionrow_id)) {
            qWarning() << "Session::Destroy() - Failed to remove session" << m_sessionrow_id << "from database";
        }
    }

    return s_machine->unlinkSession(this);
}

bool Session::Store(QString path)
// Storing Session Data in our format
// {DataDir}/{MachineID}/{SessionID}.{ext}
{
    QDir dir(path);

    if ( ! dir.exists(path)) {
        dir.mkpath(path);
    }

    //qDebug() << "Storing Session: " << base;
    bool a = true;

    // ===== SUMMARY FILE STORAGE DISABLED - NOW IN DATABASE =====
    // .000 summary files are no longer saved - data is in database
    // Uncomment this line to re-enable summary file storage:
    //a = StoreSummary();
    // ===== END SUMMARY FILE STORAGE =====

    //qDebug() << " Summary done";
    
    // Database-only storage
    // IMPORTANT: Must call StoreToDatabase() FIRST to get m_database_id
    // Then StoreEvents() can use that ID to store event data
    if (s_machine->getDatabaseId() > 0) {
        a = StoreToDatabase();  // Save session metadata first - sets m_database_id
        
        // Now store events to database (m_database_id is now set)
        if (eventlist.size() > 0) {
            StoreEvents();  // This will now work because m_database_id is set
        }
    } else {
        qWarning() << "Session::Store() - Machine not in database yet, skipping database storage";
    }

    //qDebug() << " Events done";
    s_changed = false;
    s_events_loaded = true;

    return a;
}

//QDataStream & operator<<(QDataStream & out, const Session & session)
//{
//    session.StoreSummaryData(out);
//    return out;
//}
//
//void Session::StoreSummaryData(QDataStream & out) const
//{
//    out << summary_version;
//    out << (quint32)s_session;
//    out << s_first;  // Session Start Time
//    out << s_last;   // Duration of sesion in seconds.
//
//    out << settings;
//    out << m_cnt;
//    out << m_sum;
//    out << m_avg;
//    out << m_wavg;
//
//    out << m_min;
//    out << m_max;
//
//    out << m_physmin;
//    out << m_physmax;
//
//    out << m_cph;
//    out << m_sph;
//
//    out << m_firstchan;
//    out << m_lastchan;
//
//    out << m_valuesummary;
//    out << m_timesummary;
//
//    out << m_gain;
//
//    out << m_availableChannels;
//
//    out << m_timeAboveTheshold;
//    out << m_upperThreshold;
//    out << m_timeBelowTheshold;
//    out << m_lowerThreshold;
//
//    out << s_summaryOnly;
//}
//
//QDataStream & operator>>(QDataStream & in, Session & session)
//{
//    session.LoadSummaryData(in);
//    return in;
//}
//
//
//void Session::LoadSummaryData(QDataStream & in)
//{
//    quint16 version;
//    in >> version;
//
//    quint32 t32;
//    in >> t32;      // Sessionid;
//    s_session = t32;
//
//    in >> s_first;  // Start time
//    in >> s_last;   // Duration
//
//    in >> settings;
//    in >> m_cnt;
//    in >> m_sum;
//    in >> m_avg;
//    in >> m_wavg;
//
//    in >> m_min;
//    in >> m_max;
//
//    in >> m_physmin;
//    in >> m_physmax;
//
//    in >> m_cph;
//    in >> m_sph;
//    in >> m_firstchan;
//    in >> m_lastchan;
//
//    in >> m_valuesummary;
//    in >> m_timesummary;
//
//    in >> m_gain;
//
//    in >> m_availableChannels;
//    in >> m_timeAboveTheshold;
//    in >> m_upperThreshold;
//    in >> m_timeBelowTheshold;
//    in >> m_lowerThreshold;
//
//    in >> s_summaryOnly;
//
//    s_enabled = 1;
//}

QDataStream & operator>>(QDataStream & in, SessionSlice & slice)
{
    in >> slice.start;
    quint32 length;
    in >> length;
    slice.end = slice.start + length;

    quint16 i;
    in >> i;
    slice.status = (SliceStatus)i;
    return in;
}
QDataStream & operator<<(QDataStream & out, const SessionSlice & slice)
{
    out << slice.start;
    quint32 length = slice.end - slice.start;
    out << length;
    out << (quint16)slice.status;
    return out;
}

bool Session::StoreSummary()
{
    if (s_first == 0) {
        qWarning() << "Session::StoreSummary discarding session" << s_session
                 << "["+QDateTime::fromSecsSinceEpoch(s_session).toString("MMM dd, yyyy hh:mm:ss")+"]"
                 << "from machine" << machine()->serial() << "with first=0";
        return false;
    }

    QString filename = s_machine->getSummariesPath() + toHexid(s_session) + ".000" ;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly)) {
        QDir dir;
        dir.mkpath(s_machine->getSummariesPath());

        if (!file.open(QIODevice::WriteOnly)) {
//            qWarning() << "Summary open for writing failed" << "error code" << file.error() << file.errorString();
            qWarning() << "Session::StoreSummary could not open summary" << filename << "for writing, error code" << file.error() << file.errorString();
            return false;
        }
    }

    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_4_6);
    out.setByteOrder(QDataStream::LittleEndian);

    out << (quint32)magic;
    out << (quint16)summary_version;
    out << (quint16)filetype_summary;
    out << (quint32)s_machine->id();

    out << (quint32)s_session;
    out << s_first;  // Session Start Time
    out << s_last;   // Duration of sesion in seconds.
    //out << (quint16)settings.size();

    out << settings;
    out << m_cnt;

    out << m_sum;
    out << m_avg;
    out << m_wavg;

    out << m_min;
    out << m_max;
    out << m_physmin;
    out << m_physmax;
    out << m_cph;
    out << m_sph;
    out << m_firstchan;
    out << m_lastchan;

    // <- 8
    out << m_valuesummary;
    out << m_timesummary;
    // 8 ->

    // <- 9
    out << m_gain;
    // 9 ->

    // <- 15
    out << m_availableChannels;
    out << m_timeAboveTheshold;
    out << m_upperThreshold;
    out << m_timeBelowTheshold;
    out << m_lowerThreshold;

    out << s_summaryOnly;
    // 13 ->

    out << s_noSettings; // 18

    out << m_slices;

    file.close();
    return true;
}

SpeedCheck scLoad(50); // Keep outside function so number of messages is limited
bool Session::LoadSummary(bool debug)
{
    Q_UNUSED(debug)
//    static int sumcnt = 0;

    scLoad.restart();
    if (s_summary_loaded) return true;
    
    // Try loading from database first (Phase 1 - Database Integration)
    // Skip database loading if machine doesn't have a database ID yet (during initial scan/rebuild)
    if (s_machine->getDatabaseId() > 0) {
        if (LoadFromDatabase()) {
#ifdef DBDEBUG
            qDebug() << "Session::LoadSummary() - Loaded summary session" << s_session << "from database";
#endif
            return true;
        }
        qWarning() << "Session::LoadSummary() - Database load failed for session" << s_session;
        return false;  // Database-only mode - no file fallback
    }
    
    // Database not available - session not in database
    qDebug() << "Session::LoadSummary() - Session not in database";
    return false;
    
    /* ===== FILE-BASED LOADING (DISABLED - DATABASE ONLY) =====
    // This code is kept for reference but disabled for database-only operation
    // Uncomment to re-enable file loading fallback
    
    QString filename = s_machine->getSummariesPath() + toHexid(s_session) + ".000";

    scLoad.setMsg("Session::LoadSummary processing " + filename);

    if (filename.isEmpty()) {
        if (debug) qDebug() << "Empty summary filename";
        return false;
    }

    QFile file(filename);

    if (!file.open(QIODevice::ReadOnly)) {
        if (debug) qWarning() << "Could not open summary file" << filename << "for reading, error code" << file.error() << file.errorString();
        return false;
    }

//    qDebug() << "Loading" << s_machine->loaderName() << "Summary" << filename << sumcnt++;

    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_4_6);
    in.setByteOrder(QDataStream::LittleEndian);

    quint32 t32;
    quint16 t16;

    //QHash<ChannelID,MCDataType> mctype;
    //QVector<ChannelID> mcorder;
    in >> t32;

    if (t32 != magic) {
        if (debug) qDebug() << "Wrong magic number in " << filename;
        return false;
    }

    quint16 version;
    in >> version;      // DB Version

    if (version < 6) {
        //throw OldDBVersion();
        qWarning() << "Old dbversion " << version <<
                   "summary file.. Sorry, you need to purge and reimport";
        return false;
    }

    in >> t16;      // File Type

    if (t16 != filetype_summary) {
        qDebug() << "Wrong file type"; //wrong file type
        return false;
    }


    quint32 ts32;
    in >> ts32;      // MachineID (dont need this result)

    bool upgrade = false;
    if ( ts32 != s_machine->id()) {
        upgrade = true;
        qWarning() << "Machine ID does not match in" << filename <<
                   " I will try to load anyway in case you know what your doing.";
    }

    in >> t32;      // Sessionid;
    s_session = t32;

    in >> s_first;  // Start time
    in >> s_last;   // Duration // (16bit==Limited to 18 hours)

    QHash<ChannelID, EventDataType> cruft;

    if (version < 7) {
        // This code is deprecated.. just here incase anyone tries anything crazy...
        QHash<QString, QVariant> v1;
        in >> v1;
        settings.clear();
        ChannelID code;

        for (QHash<QString, QVariant>::iterator i = v1.begin(); i != v1.end(); i++) {
            code = schema::channel[i.key()].id();
            settings[code] = i.value();
        }

        QHash<QString, int> zcnt;
        in >> zcnt;
        for (QHash<QString, int>::iterator i = zcnt.begin(); i != zcnt.end(); i++) {
            code = schema::channel[i.key()].id();
            m_cnt[code] = i.value();
        }

        QHash<QString, double> zsum;
        in >> zsum;

        for (QHash<QString, double>::iterator i = zsum.begin(); i != zsum.end(); i++) {
            code = schema::channel[i.key()].id();
            m_sum[code] = i.value();
        }

        QHash<QString, EventDataType> ztmp;
        in >> ztmp; // avg

        for (QHash<QString, EventDataType>::iterator i = ztmp.begin(); i != ztmp.end(); i++) {
            code = schema::channel[i.key()].id();
            m_avg[code] = i.value();
        }

        ztmp.clear();
        in >> ztmp; // wavg

        for (QHash<QString, EventDataType>::iterator i = ztmp.begin(); i != ztmp.end(); i++) {
            code = schema::channel[i.key()].id();
            m_wavg[code] = i.value();
        }

        ztmp.clear();
        in >> ztmp; // 90p
        ztmp.clear();
        in >> ztmp; // min

        for (QHash<QString, EventDataType>::iterator i = ztmp.begin(); i != ztmp.end(); i++) {
            code = schema::channel[i.key()].id();
            m_min[code] = i.value();
        }

        ztmp.clear();
        in >> ztmp; // max

        for (QHash<QString, EventDataType>::iterator i = ztmp.begin(); i != ztmp.end(); i++) {
            code = schema::channel[i.key()].id();
            m_max[code] = i.value();
        }

        ztmp.clear();
        in >> ztmp; // cph

        for (QHash<QString, EventDataType>::iterator i = ztmp.begin(); i != ztmp.end(); i++) {
            code = schema::channel[i.key()].id();
            m_cph[code] = i.value();
        }

        ztmp.clear();
        in >> ztmp; // sph

        for (QHash<QString, EventDataType>::iterator i = ztmp.begin(); i != ztmp.end(); i++) {
            code = schema::channel[i.key()].id();
            m_sph[code] = i.value();
        }

        QHash<QString, quint64> ztim;
        in >> ztim; //firstchan

        for (QHash<QString, quint64>::iterator i = ztim.begin(); i != ztim.end(); i++) {
            code = schema::channel[i.key()].id();
            m_firstchan[code] = i.value();
        }

        ztim.clear();
        in >> ztim; // lastchan

        for (QHash<QString, quint64>::iterator i = ztim.begin(); i != ztim.end(); i++) {
            code = schema::channel[i.key()].id();
            m_lastchan[code] = i.value();
        }

        //SetChanged(true);
    } else {
        // version > 7

        in >> settings;
        if (version < 13) {
            QHash<ChannelID, int> cnt2;
            in >> cnt2;

            QHash<ChannelID, int>::iterator it;

            for (it = cnt2.begin(); it != cnt2.end(); ++it) {
                m_cnt[it.key()] = it.value();
            }
        } else {
            in >> m_cnt;
        }
        in >> m_sum;
        in >> m_avg;
        in >> m_wavg;

        if (version < 11) {
            cruft.clear();
            in >> cruft; // 90%

            if (version >= 10) {
                cruft.clear();
                in >> cruft;// med
                cruft.clear();
                in >> cruft; //p95
            }
        }

        in >> m_min;
        in >> m_max;

        // Added 24/10/2013 by MW to support physical graph min/max values
        if (version >= 12) {
            in >> m_physmin;
            in >> m_physmax;
        }

        in >> m_cph;
        in >> m_sph;
        in >> m_firstchan;
        in >> m_lastchan;

        if (version >= 8) {
            in >> m_valuesummary;
            in >> m_timesummary;

            if (version >= 9) {
                in >> m_gain;
            }
        }

        // screwed up with version 14
        if (version >= 15) {
            in >> m_availableChannels;
            in >> m_timeAboveTheshold;
            in >> m_upperThreshold;
            in >> m_timeBelowTheshold;
            in >> m_lowerThreshold;
        } // else this is ugly.. forced device database upgrade will solve it though.

        if (version == 13) {
            QHash<ChannelID, QVariant>::iterator it = settings.find(CPAP_SummaryOnly);
            if (it != settings.end()) {
                s_summaryOnly = (*it).toBool();
            } else s_summaryOnly = false;
        } else if (version > 13) {
            in >> s_summaryOnly;
        }
        if (version >= 18) {
            in >> s_noSettings;
//            qDebug() << "Session::LoadSummary" << s_session << "["
//                     << QDateTime::fromSecsSinceEpoch(s_session).toString("MM/dd/yyyy hh:mm:ss")
//                     << "] s_noSettings" << s_noSettings << "size" << settings.size();
        } else {
            s_noSettings = (settings.size() == 0);
        }

        if (version == 16) {
            QList<SessionSlice> slices;
            in >> slices;
            m_slices.clear();
            for (int i=0;i<slices.size(); ++i) {
                m_slices.append(slices[i]);
            }
        } else if (version >= 17) {
            in >> m_slices;
        }
    }

    // not really a good idea to do this... should flag and do a reindex
    if (upgrade || (version < summary_version)) {

        qDebug() << "Upgrading Summary file to version" << summary_version;
        if (!s_summaryOnly) {
            OpenEvents();
            UpdateSummaries();
            TrashEvents();
        } else {
            // summary only upgrades go here.
        }
        StoreSummary();
    }

    scLoad.check();

    s_summary_loaded = true;
    return true;
    ===== END FILE-BASED LOADING (DISABLED) ===== */
}

const quint16 compress_method = 1;

bool Session::StoreEvents()
{
    // ===== NEW: Database-Only Storage =====
    // Try storing to database if machine is in database
    if (s_machine->getDatabaseId() > 0 && m_sessionrow_id > 0) {
        bool dbSuccess = StoreEventsToDatabase();
        if (dbSuccess) {
#ifdef DBDEBUG
            qDebug() << "Session::StoreEvents() - Successfully stored to database";
#endif
            return true;
        } else {
            qWarning() << "Session::StoreEvents() - Database storage failed";
            // Continue to return false since we're database-only now
            return false;
        }
    }
    
    qWarning() << "Session::StoreEvents() - Machine or session not in database yet, skipping";
    return false;
    
    /* ===== OLD FILE-BASED STORAGE (COMMENTED OUT FOR RECOVERY) =====
    QString path = s_machine->getEventsPath();
    QDir dir;
    dir.mkpath(path);
    QString filename = path+ toHexid(s_session) +".001";

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Could not open events file" << filename << "for writing, error code" << file.error() << file.errorString();
        return false;
    }

    QByteArray headerbytes;
    QDataStream header(&headerbytes, QIODevice::WriteOnly);
    header.setVersion(QDataStream::Qt_4_6);
    header.setByteOrder(QDataStream::LittleEndian);

    header << (quint32)magic;      // New Magic Number
    header << (quint16)events_version; // File Version
    header << (quint16)filetype_data;  // File type 1 == Event
    header << (quint32)s_machine->id();// Device Type
    header << (quint32)s_session;      // This session's ID
    header << s_first;
    header << s_last;

    quint16 compress = 0;

    if (p_profile->session->compressSessionData()) {
        compress = compress_method;
    }

    header << (quint16)compress;

    header << (quint16)s_machine->type();// Device Type

    QByteArray databytes;
    QDataStream out(&databytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_4_6);
    out.setByteOrder(QDataStream::LittleEndian);

    out << (qint16)eventlist.size(); // Number of event categories

    QHash<ChannelID, QVector<EventList *> >::iterator i;
    QHash<ChannelID, QVector<EventList *> >::iterator i_end=eventlist.end();

    qint16 ev_size;

    for (i = eventlist.begin(); i != i_end; i++) {
        ev_size=i.value().size();

        out << i.key(); // ChannelID
        out << (qint16)ev_size;


        for (int j = 0; j < ev_size; j++) {
            EventList &e = *i.value()[j];
            out << e.first();
            out << e.last();
            out << (qint32)e.count();
            out << (qint8)e.type();
            out << e.rate();
            out << e.gain();
            out << e.offset();
            out << e.Min();
            out << e.Max();
            out << e.dimension();
            out << e.hasSecondField();

            if (e.hasSecondField()) {
                out << e.min2();
                out << e.max2();
            }
        }
    }
    for (i = eventlist.begin(); i != i_end; i++) {
        ev_size=i.value().size();

        for (int j = 0; j < ev_size; j++) {
            EventList &e = *i.value()[j];
            // ****** This is assuming little endian ******

            // Store the raw event list data in EventStoreType (16bit short)
            EventStoreType *ptr = e.m_data.data();
            out.writeRawData((char *)ptr, e.count() << 1);

            // *** Don't delete these comments ***
            //            for (quint32 c=0;c<e.count();c++) {
            //                out << *ptr++;//e.raw(c);
            //            }

            // Store the second field, only if there
            if (e.hasSecondField()) {
                ptr = e.m_data2.data();
                out.writeRawData((char *)ptr, e.count() << 1);
                // *** Don't delete these comments ***
                //                for (quint32 c=0;c<e.count();c++) {
                //                    out << *ptr++; //e.raw2(c);
                //                }
            }

            // Store the time delta fields for non-waveform EventLists
            if (e.type() != EVL_Waveform) {
                quint32 *tptr = e.m_time.data();
                out.writeRawData((char *)tptr, e.count() << 2);
                // *** Don't delete these comments ***
                //                for (quint32 c=0;c<e.count();c++) {
                //                    out << *tptr++; //e.getTime()[c];
                //                }
            }
        }
    }

    qint32 datasize = databytes.size();

    // Checksum the _uncompressed_ data
    quint16 chk = 0;

    if (compress) {
        // This checksum is hideously slow.. only using during compression, and not sure I should at all :-/
        #if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            chk = qChecksum(databytes.data(), databytes.size());
        #else
            chk = qChecksum(QByteArrayView(databytes));
        #endif
    }

    header << datasize;
    header << chk;

    QByteArray data;

    if (compress > 0) {
        data = qCompress(databytes);
    } else {
        data = databytes;
    }

    file.write(headerbytes);
    file.write(data);
    file.close();
    return true;
    ===== END OLD FILE-BASED STORAGE (COMMENTED OUT) ===== */
}

bool Session::LoadEvents(QString filename, bool debug)
{
    Q_UNUSED(filename)
    Q_UNUSED(debug)

    // Skip event loading for journal machines - they only have settings
    if (s_machine->type() == MT_JOURNAL) {
        return true;  // No events to load for journals
    }

    // ===== NEW: Try Database First =====
    // Try loading from database if machine and session are in database
    if (s_machine->getDatabaseId() > 0 && m_sessionrow_id > 0) {
        if (LoadEventsFromDatabase()) {
#ifdef DBDEBUG
            qDebug() << "Session::LoadEvents() - Successfully loaded from database";
#endif
            return true;
        } else {
            // Not a warning - many sessions legitimately don't have events stored
            // (summary-only sessions, or sessions where events haven't been imported yet)
#ifdef DBDEBUG
            qDebug() << "Session::LoadEvents() - No events found in database for session" << s_session
                     << "(db_id=" << m_sessionrow_id << ")";
#endif
            return false;  // Database-only mode - no file fallback
        }
    }
    
    // Database not available - session not in database
#ifdef DBDEBUG
    qDebug() << "Session::LoadEvents() - Session not in database (machine_id="
             << s_machine->getDatabaseId() << ", session_id=" << m_sessionrow_id << ")";
#endif
    return false;
    
    /* ===== FILE-BASED LOADING (DISABLED - DATABASE ONLY) =====
    // This code is kept for reference but disabled for database-only operation
    // Uncomment to re-enable file loading fallback
    
    qDebug() << "Session::LoadEvents() - Attempting to load from .001 file";
    
    // ===== FILE-BASED LOADING (FALLBACK FOR EXISTING DATA) =====
    quint32 magicnum, machid, sessid;
    quint16 version, type, crc16, machtype, compmethod;
    quint8 t8;
    qint32 datasize;

    if (filename.isEmpty()) {
        qDebug() << "Session::LoadEvents() Filename is empty";
        return false;
    }

    QFile file(filename);

    if ( ! file.open(QIODevice::ReadOnly)) {
//        qDebug() << "No Event/Waveform data available for" << s_session;
        if (debug) qWarning() << "No Event/Waveform data available for" << s_session << "filename" << filename << "error code" << file.error() << file.errorString();
        return false;
    }

    QByteArray headerbytes = file.read(42);

    QDataStream header(headerbytes);
    header.setVersion(QDataStream::Qt_4_6);
    header.setByteOrder(QDataStream::LittleEndian);

    header >> magicnum;         // Magic Number (quint32)
    header >> version;          // Version (quint16)
    header >> type;             // File type (quint16)
    header >> machid;           // Device ID (quint32)
    header >> sessid;           //(quint32)
    header >> s_first;          //(qint64)
    header >> s_last;           //(qint64)

#ifdef DEBUG_EVENTS
    qDebug() << "Session ID" << sessid << "Start Time" << QDateTime::fromMSecsSinceEpoch(s_first);
#endif

    if (type != filetype_data) {
        qDebug() << "Wrong File Type in " << filename;
        return false;
    }

    if (magicnum != magic) {
        qWarning() << "Wrong Magic number in " << filename;
        return false;
    }

    if (version < 6) {  // prior to version 6 is too old to deal with
        qDebug() << "Old File Version, can't open file";
        return false;
    }

    if (version < 10) {
        file.seek(32);
    } else {
        header >> compmethod;   // Compression Method (quint16)
        header >> machtype;     // Device Type (quint16)
        header >> datasize;     // Size of Uncompressed Data (quint32)
        header >> crc16;        // CRC16 of Uncompressed Data (quint16)
    }

    QByteArray databytes, temp = file.readAll();
    file.close();

    if (version >= 10) {
        if (compmethod > 0) {
            databytes = qUncompress(temp);

            if (!s_evchecksum_checked) {
                if (databytes.size() != datasize) {
                    qDebug() << "File" << filename << "has returned wrong datasize";
                    return false;
                }

                quint16 crc = 0;
                #if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
                    crc = qChecksum(databytes.data(), databytes.size());
                #else
                    crc = qChecksum(QByteArrayView(databytes));
                #endif

                if (crc != crc16) {
                    qDebug() << "CRC Doesn't match in" << filename;
                    return false;
                }

                s_evchecksum_checked = true;
            }
        } else {
            databytes = temp;
        }
    } else { databytes = temp; }

    QDataStream in(databytes);
    in.setVersion(QDataStream::Qt_4_6);
    in.setByteOrder(QDataStream::LittleEndian);

    qint16 mcsize;
    in >> mcsize;   // number of Device Code lists
#ifdef DEBUG_EVENTS
    qDebug() << "Number of Channels" << mcsize;
#endif

    ChannelID code;
    qint64 ts1, ts2;
    qint32 evcount;
    EventListType elt;
    EventDataType rate, gain, offset, mn, mx;
    qint16 size2;
    QVector<ChannelID> mcorder;
    QVector<qint16> sizevec;
    QString dim;

    for (int i = 0; i < mcsize; i++) {
        if (version < 8) {
            QString txt;
            in >> txt;
            code = schema::channel[txt].id();
        } else {
            in >> code;
        }

        mcorder.push_back(code);
        in >> size2;
        sizevec.push_back(size2);
#ifdef DEBUG_EVENTS
        qDebug() << "For Channel (hex)" << QString::number(code, 16) << "there are" << size2 << "EventLists";
#endif

        for (int j = 0; j < size2; j++) {
            in >> ts1;
            in >> ts2;
#ifdef DEBUG_EVENTS
            qDebug() << "Start:" << QDateTime::fromMSecsSinceEpoch(ts1).toString() <<
                        "Finish:" << QDateTime::fromMSecsSinceEpoch(ts2).toString();
#endif
            in >> evcount;
            in >> t8;
            elt = (EventListType)t8;
            in >> rate;
            in >> gain;
            in >> offset;
            in >> mn;
            in >> mx;
            in >> dim;
            bool second_field = false;

            if (version >= 7) { // version 7 added this field
                in >> second_field;
            }

            EventList *elist = AddEventList(code, elt, gain, offset, mn, mx, rate, second_field);
            elist->setDimension(dim);

            //eventlist[code].push_back(elist);
            elist->m_count = evcount;
            elist->m_first = ts1;
            elist->m_last = ts2;

            if (second_field) {
                EventDataType min, max;
                in >> min;
                in >> max;
                elist->setMin2(min);
                elist->setMax2(max);
            }
        }
    }

    //EventStoreType t;     // qint16
    //quint32 x;

    for (int i = 0; i < mcsize; i++) {
        code = mcorder[i];
        size2 = sizevec[i];

        for (int j = 0; j < size2; j++) {
            EventList &evec = *eventlist[code][j];
            evec.m_data.resize(evec.m_count);
            EventStoreType *ptr = evec.m_data.data();

            // ****** This is assuming little endian ******

            in.readRawData((char *)ptr, evec.m_count << 1);

            // *** Don't delete these comments ***
            // *** They explain what the above ReadRawData is doing!
            //            for (quint32 c=0;c<evec.m_count;c++) {
            //                in >> t;
            //                *ptr++=t;
            //            }
            if (evec.hasSecondField()) {
                evec.m_data2.resize(evec.m_count);
                ptr = evec.m_data2.data();

                in.readRawData((char *)ptr, evec.m_count << 1);
                // *** Don't delete these comments ***
                // *** They explain what the above ReadRawData is doing!
                //                for (quint32 c=0;c<evec.m_count;c++) {
                //                    in >> t;
                //                    *ptr++=t;
                //                }
            }

            if (evec.type() != EVL_Waveform) {
                evec.m_time.resize(evec.m_count);
                quint32 *tptr = evec.m_time.data();

                in.readRawData((char *)tptr, evec.m_count << 2);
                // *** Don't delete these comments ***
                //                for (quint32 c=0;c<evec.m_count;c++) {
                //                    in >> x;
                //                    *tptr++=x;
                //                }
            }
        }
    }

    if (version < events_version) {
        qDebug() << "Upgrading Events file" << filename << "to version" << events_version;
        UpdateSummaries();
        StoreEvents();
    }

    return true;
    ===== END FILE-BASED LOADING (DISABLED) ===== */
}

void Session::destroyEvent(ChannelID code)
{
    QHash<ChannelID, QVector<EventList *> >::iterator it = eventlist.find(code);

    if (it != eventlist.end()) {
        for (int i = 0; i < it.value().size(); i++) {
            delete it.value()[i];
        }

        eventlist.erase(it);
    }

    // Use remove() instead of erase(find()) to avoid GCC 7.1+ ABI warnings
    m_gain.remove(code);
    m_firstchan.remove(code);
    m_lastchan.remove(code);
    m_sph.remove(code);
    m_cph.remove(code);
    m_min.remove(code);
    m_max.remove(code);
    m_avg.remove(code);
    m_wavg.remove(code);
    m_sum.remove(code);
    m_cnt.remove(code);
    m_valuesummary.remove(code);
    m_timesummary.remove(code);
    // does not trash settings..
}

// TODO: The below assumes values are held for their duration. This does not properly handle
// CPAP_PressureSet/EPAPSet/IPAPSet or other interpolated channels. The proper "value" held
// for any given duration is the average of the starting and ending values, for the duration
// between them.
void Session::updateCountSummary(ChannelID code)
{
    QHash<ChannelID, QVector<EventList *> >::iterator ev = eventlist.find(code);

    if (ev == eventlist.end()) {
        qDebug() << "No events for channel (hex)" << QString::number(code, 16);
        return;
    }

    QHash<ChannelID, QHash<EventStoreType, EventStoreType> >::iterator vs = m_valuesummary.find(code);

    if (vs != m_valuesummary.end()) { // already calculated?
        return;
    }

    QHash<EventStoreType, EventStoreType> valsum;
    QHash<EventStoreType, quint32> timesum;

    //QHash<EventStoreType, EventStoreType>::iterator it;
    //QHash<EventStoreType, EventStoreType>::iterator valsum_end;

    EventDataType raw, lastraw = 0;
    qint64 start, time, lasttime = 0;
    qint32 len, cnt;
    quint32 *tptr;
    EventStoreType *dptr, * eptr;

    int ev_size=ev.value().size();
    for (int i = 0; i < ev_size; i++) {
        EventList &e = *(ev.value()[i]);
        start = e.first();
        cnt = e.count();
        dptr = e.rawData();
        eptr = dptr + cnt;

        EventDataType rate = 0;

        m_gain[code] = e.gain();

        if (e.type() == EVL_Event) {
            tptr = e.rawTime();
            // The last event event raw time is never updated with the len to the next.
            // this is true for multiple samples or for a single sample.
            // a single sample is the first as well as the last sample.
            // so a single sample should work.
            // the iBreeze loader had snore events with a single snore sample for a session.
            // which triggered this investigation.

            // CRASH DEBUG DESCRIPTION.
            // Why this occurs only for QT6 and not QT5 is not known at this time
            // This crash occurs when an empty event list is created for an event.
            // and another event list is created for that event then the empty event list
            // will trigger the crash.
            // every time an event is added to an event list a counter is incremented.
            // if the count is zero then tptr is a null pointer and triggers the crash.
            // when the count is zero then just continue to process the next event list.
            if (cnt==0) {
                continue;
            }
            lastraw = *dptr++;
            lasttime = start + *tptr++;

            // After priming, dptr >= eptr iff cnt==1. Capture this per-EventList
            // so the fallback fires even when an earlier list already populated valsum.
            bool singleEvent = (dptr >= eptr);

            // Event version
            for (; dptr < eptr; dptr++) {
                time = start + *tptr++;
                raw = *dptr;

                valsum[raw]++;

                // elapsed time in seconds since last event occurred
                len = (time - lasttime) / 1000L;

                timesum[lastraw] += len;

                lastraw = raw;
                lasttime = time;
            }

            // cnt==1: loop never ran, so this EventList contributed nothing to valsum
            // or timesum. Give the sole constant value a minimal weight (true duration
            // is zero for a 1-event list) so calculatePercentiles() returns it.
            if (singleEvent) {
                valsum[lastraw]++;
                timesum[lastraw] += 1;
            }
        } else {
            // Waveform version: count this EventList's samples into a local map,
            // then accumulate the time weight using ONLY this EventList's counts.
            // Using the global valsum here would double-count earlier EventLists'
            // entries when multiple EventLists exist for the same channel.
            QHash<EventStoreType, EventStoreType> localValsum;
            for (; dptr < eptr; dptr++) {
                raw = *dptr;
                localValsum[raw]++;
                valsum[raw]++;
            }

            // Time weight is (count * rate_ms / 1000) = seconds, matching EVL_Event path.
            // rate is in ms; dividing by 1000 converts to seconds so timeAboveThreshold
            // fast path (which divides by 60 to get minutes) gives correct results.
            rate = e.rate();
            for (auto it = localValsum.cbegin(); it != localValsum.cend(); ++it) {
                timesum[it.key()] += (quint32)(EventDataType(it.value()) * rate / 1000.0 + 0.5);
            }
        }
    }

    if ( valsum.size() == 0) {  // no value summary for this channel
        using namespace schema;
        Channel *ch_p = channel.channels[code];
        if (  ! ch_p->isNull() ) {                      // the channel was found in the channel list
#ifdef DBDEBUG
            if ( ((ch_p->type() & (FLAG|SPAN|MINOR_FLAG)) == 0) ) {  // the channel is not a flag or span type
                qDebug() << "No valuesummary for channel " << ch_p->label() <<  " " << QDateTime::fromMSecsSinceEpoch( realFirst()).toString() ;    // so tell about missing summary
            }
#endif
        } else {
            // This channel wasn't added to the channel list, so we can't check its type
            qDebug() << "No valuesummary for channel (hex)" << QString::number(code, 16);
        }
        return;
    }

    m_valuesummary[code] = valsum;
    m_timesummary[code] = timesum;
}

void Session::UpdateSummaries()
{
    ChannelID id;

    // Generate that AHI per hour graph in daily view.
    calcAHIGraph(this);

    // Calculates RespRate and related waveforms (Tv, MV, Te, Ti) if missing
    calcRespRate(this);

    // Generate unintentional leaks if not present
    calcLeaks(this);

    // Flag the Large Leaks if unintentional leaks is available, and no LargeLeaks weren't flagged by the device already.
    flagLargeLeaks(this);

    calcSPO2Drop(this);
    calcPulseChange(this);

    QHash<ChannelID, QVector<EventList *> >::iterator c = eventlist.begin();
    QHash<ChannelID, QVector<EventList *> >::iterator ev_end = eventlist.end();

    m_availableChannels.clear();

    for (; c != ev_end; c++) {
        id = c.key();
        m_availableChannels.push_back(id);

        schema::ChanType ctype = schema::channel[id].type();
        if (ctype != schema::SETTING) {
            //sum(id); // avg calculates this and cnt.
            if (c.value().size() > 0) {
                EventList *el = c.value()[0];
                EventDataType gain = el->gain();
                m_gain[id] = gain;
            }

            if (!((id == CPAP_FlowRate) || (id == CPAP_MaskPressureHi) || (id == CPAP_RespEvent)
                    || (id == CPAP_MaskPressure))) {
                updateCountSummary(id);
            }

            Min(id);
            Max(id);
            count(id);
            last(id);
            first(id);

            if (((id == CPAP_FlowRate)
                 || (id == CPAP_MaskPressureHi)
                 || (id == CPAP_RespEvent)
                 || (id == CPAP_MaskPressure))) {
                continue;
            }

            cph(id);
            sph(id);
            avg(id);
            wavg(id);
        }
    }
    timeAboveThreshold(CPAP_Leak, p_profile->cpap->leakRedline());

    s_machine->updateChannels(this);
}

EventDataType Session::SearchValue(ChannelID code, qint64 time, bool square)
{
    time -= correctionMs();
    qint64 t1, t2, start;
    QHash<ChannelID, QVector<EventList *> >::iterator it;
    it = eventlist.find(code);
    quint32 *tptr;
    int cnt;

    EventDataType a,b,c,d,e;
    if (it != eventlist.end()) {
        int el_size=it.value().size();
        for (int i = 0; i < el_size; i++)  {
            EventList *el = it.value()[i];
            if ((time >= el->first()) && (time < el->last())) {
                cnt = el->count();

                if (el->type() == EVL_Waveform) {
                    qint64 tt = time - el->first();

                    double i = tt / el->rate();
                    if (i> cnt) {
                        qWarning() << "Session" << session() << "time bounds are broken.. There is a fault in the" << machine()->loaderName().toLocal8Bit().data() << "loader";
                        return 0;
                    }

                    int i1 = int(floor(i));
                    int i2 = int(ceil(i));

                    a = el->data(i1);

                    // Don't interpolate if next data point is past end or on exact data point
                    if (i2 >= cnt || i1 == i2) { return a; }

                    qint64 t1 = i1 * el->rate();
                    qint64 t2 = i2 * el->rate();

                    c = EventDataType(t2 - t1);
                    // Don't interpolate if t2-t1==0 (should be caught by i1==i2 above)
                    if (c == 0) return a;
                    d = EventDataType(t2 - tt);

                    e = d/c;
                    b = el->data(i2);

                    return b + ((a-b) * e);

                } else {
                    start = el->first();
                    tptr = el->rawTime();
                    // TODO: square plots need fixing
                    if (square) {
                        for (int j = 0; j < cnt-1; ++j) {
                            tptr++;
                            t2 = start + *tptr;
                            if (t2 > time) {
                                return el->data(j);
                            }
                        }
                    } else {
                        for (int j = 0; j < cnt-1; ++j) {
                            tptr++;
                            t2 = start + *tptr;
                            if (t2 > time) {
                                tptr--;
                                t1 = start + *tptr;
                                c = EventDataType(t2 - t1);
                                d = EventDataType(t2 - time);
                                e = d/c;
                                a = el->data(j);
                                b = el->data(j+1);
                                if (a == b) {
                                    return a;
                                } else {
                                    return b + ((a-b) * e);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return 0;
}

QString Session::dimension(ChannelID id)
{
    // Cheat for now
    return schema::channel[id].units();
}

EventDataType Session::Min(ChannelID id)
{
    QHash<ChannelID, EventDataType>::iterator i = m_min.find(id);

    if (i != m_min.end()) {
        return i.value();
    }

    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
        m_min[id] = 0;
        return 0;
    }

    QVector<EventList *> &evec = j.value();

    bool first = true;
    EventDataType min = 0, t1;

    int evec_size = evec.size();

    for (int i = 0; i < evec_size; ++i) {
        if (evec[i]->count() != 0) {
            t1 = evec[i]->Min();

            if ((t1 == 0) && (t1 == evec[i]->Max())) { continue; }

            if (first) {
                min = t1;
                first = false;
            } else {
                if (min > t1) { min = t1; }
            }
        }
    }

    m_min[id] = min;
    return min;
}

EventDataType Session::Max(ChannelID id)
{
    QHash<ChannelID, EventDataType>::iterator i = m_max.find(id);

    if (i != m_max.end()) {
        return i.value();
    }

    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
        m_max[id] = 0;
        return 0;
    }

    QVector<EventList *> &evec = j.value();

    bool first = true;
    EventDataType max = 0, t1;

    int evec_size=evec.size();

    for (int i = 0; i < evec_size; ++i) {
        if (evec.at(i)->count() != 0) {
            t1 = evec.at(i)->Max();

            if (t1 == 0 && t1 == evec.at(i)->Min()) { continue; }

            if (first) {
                max = t1;
                first = false;
            } else {
                if (max < t1) { max = t1; }
            }
        }
    }

    m_max[id] = max;
    return max;
}

////
EventDataType Session::physMin(ChannelID id)
{
    QHash<ChannelID, EventDataType>::iterator i = m_physmin.find(id);

    if (i != m_physmin.end()) {
        return i.value();
    }

    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
        m_physmin[id] = 0;
        return 0;
    }

    EventDataType min = floor(Min(id));
    m_physmin[id] = min;
    return min;
}

EventDataType Session::physMax(ChannelID id)
{
    QHash<ChannelID, EventDataType>::iterator i = m_physmax.find(id);

    if (i != m_physmax.end()) {
        return i.value();
    }

    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
        m_physmax[id] = 0;
        return 0;
    }

    EventDataType max = ceil(Max(id) + 0.5);
    m_physmax[id] = max;
    return max;
}


qint64 Session::first(ChannelID id)
{
    QHash<ChannelID, quint64>::iterator i = m_firstchan.find(id);

    if (i != m_firstchan.end()) {
        return i.value() + correctionMs();
    }

    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
        return 0;
    }

    QVector<EventList *> &evec = j.value();

    bool first = true;
    qint64 min = 0, t1;

    int evec_size=evec.size();
    for (int i = 0; i < evec_size; ++i) {
        t1 = evec[i]->first();

        if (first) {
            min = t1;
            first = false;
        } else {
            if (min > t1) { min = t1; }
        }
    }

    m_firstchan[id] = min;
    return min + correctionMs();
}
qint64 Session::last(ChannelID id)
{
    QHash<ChannelID, quint64>::iterator i = m_lastchan.find(id);

    if (i != m_lastchan.end()) {
        return i.value() + correctionMs();
    }

    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
        return 0;
    }

    QVector<EventList *> &evec = j.value();

    bool first = true;
    qint64 max = 0, t1;

    int evec_size=evec.size();

    for (int i = 0; i < evec_size; ++i) {
        t1 = evec[i]->last();

        if (first) {
            max = t1;
            first = false;
        } else {
            if (max < t1) { max = t1; }
        }
    }

    m_lastchan[id] = max;
    return max + correctionMs();
}
bool Session::channelDataExists(ChannelID id)
{
    if (s_events_loaded) {
        QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

        if (j == eventlist.end()) { // eventlist not loaded.
            return false;
        }

        return true;
    } else {
        qDebug() << "Calling channelDataExists without open eventdata! id=" << id;
    }

    return false;
}
bool Session::channelExists(ChannelID id)
{
    if ( ! enabled()) {
        return false;
    }

    if (s_events_loaded) {
        QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

        if (j == eventlist.end()) { // eventlist not loaded.
            return false;
        }
    } else {
        QHash<ChannelID, EventDataType>::iterator q = m_cnt.find(id);

        if (q == m_cnt.end()) {
            return false;
        }

        if (q.value() == 0) {
            return false;
        }
    }

    return true;
}

EventDataType Session::countInsideSpan(ChannelID span, ChannelID code)
{
    // TODO: Cache me!

    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(span);

    if (j == eventlist.end()) {
        return 0;
    }
    QVector<EventList *> &evec = j.value();

    qint64 t1,t2;

    int evec_size=evec.size();

    QList<qint64> start;
    QList<qint64> end;

    // Simplify the span flags to start and end times list
    for (int el = 0; el < evec_size; ++el) {
        EventList &ev = *evec[el];

        for (quint32 i=0; i < ev.count(); ++i) {
            end.push_back(t2=ev.time(i));
            start.push_back(t2 - (qint64(ev.data(i)) * 1000L));
        }
    }

    j = eventlist.find(code);

    if (j == eventlist.end()) {
        return 0;
    }
    QVector<EventList *> &evec2 = j.value();
    evec_size=evec2.size();
    int count = 0;

    int spans = start.size();

    for (int el = 0; el < evec_size; ++el) {
        EventList &ev = *evec2[el];

        for (quint32 i=0; i < ev.count(); ++i) {
            t1 = ev.time(i);
            for (int z=0; z < spans; ++z) {
                if ((t1 >= start.at(z)) && (t1 <= end.at(z))) {
                    count++;
                    break;
                }
            }
        }
    }
    return count;
}

EventDataType Session::rangeCount(ChannelID id, qint64 first, qint64 last)
{
    int total = 0, cnt;

    if (const QVector<ChannelID> * group = ahiChannelGroup(id)) {
        for (int i = 0; i < group->size(); i++)
            total += rangeCount(group->at(i), first, last);
        return (EventDataType)total;
    }

    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
        return 0;
    }

    QVector<EventList *> &evec = j.value();

    qint64 t, start;

    int evec_size=evec.size();

    for (int i = 0; i < evec_size; ++i) {
        EventList &ev = *evec[i];

        if ((ev.last() < first) || (ev.first() > last)) {
            continue;
        }

        if (ev.type() == EVL_Waveform) {
            qint64 et = last;

            if (et > ev.last()) {
                et = ev.last();
            }

            qint64 st = first;

            if (st < ev.first()) {
                st = ev.first();
            }

            t = (et - st) / ev.rate();
            total += t;
        } else {
            cnt = ev.count();
            start = ev.first();
            quint32 *tptr = ev.rawTime();
            quint32 *eptr = tptr + cnt;

            for (; tptr < eptr; tptr++) {
                t = start + *tptr;

                if (t >= first) {
                    if (t <= last) {
                        total++;
                    } else { break; }
                }
            }
        }
    }

    return (EventDataType)total;
}

double Session::rangeSum(ChannelID id, qint64 first, qint64 last)
{
    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
        return 0;
    }

    QVector<EventList *> &evec = j.value();
    double sum = 0, gain;

    qint64 t, start;
    EventStoreType *dptr, * eptr;
    quint32 *tptr;
    int cnt, idx = 0;

    qint64 rate;

    int evec_size=evec.size();

    for (int i = 0; i < evec_size; i++) {
        EventList &ev = *evec[i];

        if ((ev.last() < first) || (ev.first() > last)) {
            continue;
        }

        start = ev.first();
        dptr = ev.rawData();
        cnt = ev.count();
        eptr = dptr + cnt;
        gain = ev.gain();
        rate = ev.rate();

        if (ev.type() == EVL_Waveform) {
            if (first > ev.first()) {
                // Skip the samples before first
                idx = (first - ev.first()) / rate;
            }

            dptr += idx; //???? foggy.

            t = start;

            for (; dptr < eptr; dptr++) { //int j=idx;j<cnt;j++) {
                if (t <= last) {
                    sum += EventDataType(*dptr) * gain;
                } else { break; }

                t += rate;
            }
        } else {
            tptr = ev.rawTime();

            for (; dptr < eptr; dptr++) {
                t = start + *tptr++;

                if (t >= first) {
                    if (t <= last) {
                        sum += EventDataType(*dptr) * gain;
                    } else { break; }
                }
            }
        }
    }

    return sum;
}
EventDataType Session::rangeMin(ChannelID id, qint64 first, qint64 last)
{
    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
        return 0;
    }

    QVector<EventList *> &evec = j.value();
    EventDataType gain, v, min = std::numeric_limits<EventDataType>::max();

    qint64 t, start, rate;
    EventStoreType *dptr, * eptr;
    quint32 *tptr;
    int cnt, idx;

    int evec_size=evec.size();

    for (int i = 0; i < evec_size; ++i) {
        EventList &ev = *evec[i];

        if ((ev.last() < first) || (ev.first() > last)) {
            continue;
        }

        dptr = ev.rawData();
        start = ev.first();
        cnt = ev.count();
        eptr = dptr + cnt;
        gain = ev.gain();

        if (ev.type() == EVL_Waveform) {
            rate = ev.rate();
            t = start;
            idx = 0;

            if (first > ev.first()) {
                // Skip the samples before first
                idx = (first - ev.first()) / rate;
            }

            dptr += idx;

            for (; dptr < eptr; dptr++) { //int j=idx;j<cnt;j++) {
                if (t <= last) {
                    v = EventDataType(*dptr) * gain;

                    if (v < min) {
                        min = v;
                    }
                } else { break; }

                t += rate;
            }
        } else {
            tptr = ev.rawTime();

            for (; dptr < eptr; dptr++) { //int j=0;j<cnt;j++) {
                t = start + *tptr++;

                if (t >= first) {
                    if (t <= last) {
                        v = EventDataType(*dptr) * gain;

                        if (v < min) {
                            min = v;
                        }
                    } else { break; }
                }
            }
        }
    }

    return min;
}

EventDataType Session::rangeMax(ChannelID id, qint64 first, qint64 last)
{
    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
        return 0;
    }

    QVector<EventList *> &evec = j.value();
    EventDataType gain, v, max = std::numeric_limits<EventDataType>::min();

    qint64 t, start, rate;
    EventStoreType *dptr, * eptr;
    quint32 *tptr;
    int cnt, idx;

    int evec_size=evec.size();

    for (int i = 0; i < evec_size; i++) {
        EventList &ev = *evec[i];

        if ((ev.last() < first) || (ev.first() > last)) {
            continue;
        }

        start = ev.first();
        dptr = ev.rawData();
        cnt = ev.count();
        eptr = dptr + cnt;
        gain = ev.gain();

        if (ev.type() == EVL_Waveform) {
            rate = ev.rate();
            t = start;
            idx = 0;

            if (first > ev.first()) {
                // Skip the samples before first
                idx = (first - ev.first()) / rate;
            }

            dptr += idx;

            for (; dptr < eptr; dptr++) { //int j=idx;j<cnt;j++) {
                if (t <= last) {
                    v = EventDataType(*dptr) * gain;

                    if (v > max) { max = v; }
                } else { break; }

                t += rate;
            }
        } else {
            tptr = ev.rawTime();

            for (; dptr < eptr; dptr++) {
                t = start + *tptr++;

                if (t >= first) {
                    if (t <= last) {
                        v = EventDataType(*dptr) * gain;

                        if (v > max) { max = v; }
                    } else { break; }
                }
            }
        }
    }

    return max;
}

EventDataType Session::count(ChannelID id)
{
    int sum = 0;

    if (const QVector<ChannelID> * group = ahiChannelGroup(id)) {
        for (int i = 0; i < group->size(); i++)
            sum += count(group->at(i));
        return sum;
    }

    QHash<ChannelID, EventDataType>::iterator i = m_cnt.find(id);

    if (i != m_cnt.end()) {
//        if (i.value() != static_cast<int>(i.value()))
//            qDebug() << "Session::count() says i != m_cnt.end, returning" << qSetRealNumberPrecision(9) <<  i.value();
        return i.value();
    }

    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
//        m_cnt[id] = 0;
//        qDebug() << "Session::count() says j != eventlist.end, returning 0";
        return 0;
    }

    QVector<EventList *> &evec = j.value();

    int evec_size=evec.size();
    if (evec_size == 0)
        return 0;

    for (int i = 0; i < evec_size; ++i) {
        sum += evec.at(i)->count();
    }

    m_cnt[id] = sum;
//    if (sum != static_cast<int>(sum))
//        qDebug() << "Session::count() for channel" << id << "returning and setting m_cnt to" << qSetRealNumberPrecision(9) << sum;
    return sum;
}

double Session::sum(ChannelID id)
{
    QHash<ChannelID, double>::iterator i = m_sum.find(id);

    if (i != m_sum.end()) {
        return i.value();
    }

    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
        m_sum[id] = 0;
        return 0;
    }

    QVector<EventList *> &evec = j.value();

    double gain, sum = 0;
    EventStoreType *dptr, * eptr;
    int cnt;

    int evec_size=evec.size();

    for (int i = 0; i < evec_size; ++i) {
        EventList &ev = *(evec[i]);
        gain = ev.gain();
        cnt = ev.count();
        dptr = ev.rawData();
        eptr = dptr + cnt;

        for (; dptr < eptr; dptr++) {
            sum += double(*dptr) * gain;
        }
    }

    m_sum[id] = sum;
    return sum;
}

EventDataType Session::avg(ChannelID id)
{
    QHash<ChannelID, EventDataType>::iterator i = m_avg.find(id);

    if (i != m_avg.end()) {
        return i.value();
    }

    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);

    if (j == eventlist.end()) {
        m_avg[id] = 0;
        return 0;
    }

    QVector<EventList *> &evec = j.value();

    double val = 0, gain;
    int cnt = 0;
    EventStoreType *dptr, * eptr;
    int evec_size=evec.size();

    for (int i = 0; i < evec_size; ++i) {
        EventList &ev = *(evec[i]);
        dptr = ev.rawData();
        gain = ev.gain();
        // The per-list bound and the final divisor are different quantities:
        // val accumulates over every list, so cnt must be the total. Assigning
        // ev.count() here instead of adding it divided the sum of all lists by
        // the length of the last one, inflating the average of any channel
        // split across multiple EventLists by roughly (total / last).
        const int listCount = ev.count();
        cnt += listCount;
        eptr = dptr + listCount;

        for (; dptr < eptr; dptr++) {
            val += double(*dptr) * gain;
        }
    }

    if (cnt > 0) { // Shouldn't really happen.. Should aways contain data
        val /= double(cnt);
    }

    m_avg[id] = val;
    return val;
}
EventDataType Session::cph(ChannelID id) // count per hour
{
    QHash<ChannelID, EventDataType>::iterator i = m_cph.find(id);

    if (i != m_cph.end()) {
        return i.value();
    }

    EventDataType val = count(id);
    val /= hours();

    m_cph[id] = val;
    return val;
}
EventDataType Session::sph(ChannelID id) // sum per hour, assuming id is a time field in seconds
{
    QHash<ChannelID, EventDataType>::iterator i = m_sph.find(id);

    if (i != m_sph.end()) {
        return i.value();
    }

    EventDataType val = sum(id) / 3600.0;
    val = 100.0 / hours() * val;
    m_sph[id] = val;
    return val;
}

EventDataType Session::timeAboveThreshold(ChannelID id, EventDataType threshold)
{
    // Check cache first
    QHash<ChannelID, EventDataType>::iterator th = m_upperThreshold.find(id);
    if (th != m_upperThreshold.end()) {
        if (fabs(th.value()-threshold) < 0.00000001) { // close enough
            th = m_timeAboveTheshold.find(id);
            if (th != m_timeAboveTheshold.end()) {
                return th.value();
            }
        }
    }
    
    // PHASE 1 OPTIMIZATION: Try using m_timesummary if available
    // This is ~100x faster than loading events from disk.
    //
    // The weights in m_timesummary are NOT in a single unit: updateCountSummary()
    // accumulates seconds for EVL_Event lists but milliseconds for EVL_Waveform
    // lists. Every other consumer (wavg, percentiles) uses them as ratios, so the
    // difference cancels out and was never noticed. Reading them here as absolute
    // seconds reported waveform-backed channels as 1000x their true duration --
    // Prisma records leak as a 1 Hz waveform, which put "Time over leak redline"
    // at figures like 20764% instead of 20.764%.
    //
    // So take the fraction of the weight that sits above the threshold, which is
    // unit-agnostic, and scale it by the channel's actual recorded span. Both
    // endpoints are cached alongside the summaries and persisted to the database,
    // so this still avoids touching the event data on disk.
    auto ts = m_timesummary.find(id);
    if (ts != m_timesummary.end() && m_gain.contains(id)) {
        double gain = m_gain[id];
        double above = 0, total = 0;

        // Iterate through time summary hash
        for (auto it = ts.value().begin(); it != ts.value().end(); ++it) {
            EventDataType value = EventDataType(it.key()) * gain;
            double weight = it.value();

            total += weight;
            if (value >= threshold) {
                above += weight;
            }
        }

        auto fc = m_firstchan.find(id);
        auto lc = m_lastchan.find(id);

        if ((total > 0) && (fc != m_firstchan.end()) && (lc != m_lastchan.end())
                && (lc.value() > fc.value())) {
            // Span is in milliseconds; the result is in decimal minutes.
            EventDataType time = (above / total) * (double(lc.value() - fc.value()) / 60000.0);

            // Cache the result
            m_timeAboveTheshold[id] = time;
            m_upperThreshold[id] = threshold;

            return time;
        }
        // No usable span for this channel, so fall through to the exact
        // event-based calculation rather than return a mis-scaled figure.
    }

    // FALLBACK: Load events and calculate (original method)
    bool loaded = s_events_loaded;

    OpenEvents();
    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);
    if (j == eventlist.end()) {
        if (!loaded) {
            TrashEvents();
        }
        return 0.0f;
    }

    QVector<EventList *> &evec = j.value();
    int evec_size=evec.size();

    qint64 ti, started=0, total=0;
    EventDataType data;
    int elsize;
    for (int i = 0; i < evec_size; ++i) {
        EventList &ev = *(evec[i]);
        elsize = ev.count();

        for (int j=0; j < elsize; ++j) {
            ti=ev.time(j);
            data=ev.data(j);

            if (started == 0) {
                if (data >= threshold) {
                    started=ti;
                }
            } else {
                if (data < threshold) {
                    total += ti-started;
                    started = 0;
                }
            }
        }
    }
    if (started) {
        total += ti-started;
    }
    EventDataType time = double(total) / 60000.0;

    m_timeAboveTheshold[id] = time;
    m_upperThreshold[id] = threshold;
    if (!loaded) this->TrashEvents(); // otherwise leave it open
    return time;
}

EventDataType Session::timeBelowThreshold(ChannelID id, EventDataType threshold)
{
    QHash<ChannelID, EventDataType>::iterator th = m_lowerThreshold.find(id);
    if (th != m_lowerThreshold.end()) {
        if (fabs(th.value()-threshold) < 0.00000001) { // close enough
            th = m_timeBelowTheshold.find(id);
            if (th != m_timeBelowTheshold.end()) {
                return th.value();
            }
        }
    }
    bool loaded = s_events_loaded;

    QHash<ChannelID, QVector<EventList *> >::iterator j = eventlist.find(id);
    if (j == eventlist.end()) {
        return 0.0f;
    }

    QVector<EventList *> &evec = j.value();
    int evec_size=evec.size();

    qint64 ti, started=0, total=0;
    EventDataType data;
    int elsize;
    for (int i = 0; i < evec_size; ++i) {
        EventList &ev = *(evec[i]);
        elsize = ev.count();

        for (int j=0; j < elsize; ++j) {
            ti=ev.time(j);
            data=ev.data(j);

            if (started == 0) {
                if (data <= threshold) {
                    started=ti;
                }
            } else {
                if (data > threshold) {
                    total += ti-started;
                    started = 0;
                }
            }
        }
    }

    if (started) {
        total += ti-started;
    }

    EventDataType time = double(total) / 60000.0;

    m_timeBelowTheshold[id] = time;
    m_lowerThreshold[id] = threshold;
    if (!loaded) this->TrashEvents(); // otherwise leave it open

    return time;
}


bool sortfunction(EventStoreType i, EventStoreType j) { return (i < j); }

/*!
 * \brief Calculates a single percentile value with time-weighted linear interpolation
 * \param id Channel ID to calculate percentile for
 * \param percent Percentile to calculate (0.0 to 1.0, e.g., 0.50 for median, 0.95 for 95th)
 * \return Time-weighted interpolated percentile value
 * 
 * This function uses TIME-WEIGHTED percentile calculation to match Day::percentile().
 * Values are weighted by how long they were held (from m_timesummary), not by sample count.
 * This ensures session and day percentiles match for the same data.
 * 
 * For calculating multiple percentiles at once, use calculatePercentiles() instead,
 * which is more efficient (~3x faster for multiple percentiles).
 */
EventDataType Session::percentile(ChannelID id, EventDataType percent)
{
    if (percent > 1.0) {
        qWarning() << "Session::percentile() called with > 1.0";
        return 0;
    }

    // Ensure m_valuesummary and m_timesummary are populated
    updateCountSummary(id);

    auto ei = m_valuesummary.find(id);
    if (ei == m_valuesummary.end()) {
        return 0;
    }

    auto tei = m_timesummary.find(id);
    bool timeweight = (tei != m_timesummary.end());
    
    if (!timeweight) {
        // Fallback to value counts if no time summary available
        // This shouldn't normally happen for waveform data
        qWarning() << "Session::percentile() - no time summary for channel" << QString::number(id, 16);
        return 0;
    }

    EventDataType gain = m_gain.value(id, 1.0);

    // Build weight map from time summary (time in seconds)
    QHash<EventStoreType, qint64> wmap;
    qint64 SN = 0;  // Total time (in seconds)

    for (auto it = tei.value().begin(), teival_end = tei.value().end(); it != teival_end; ++it) {
        qint64 weight = it.value();  // Time in seconds
        SN += weight;
        wmap[it.key()] += weight;
    }

    if (SN == 0) {
        return 0;
    }

    // Build sorted list of value/counts
    QVector<ValueCount> valcnt;
    valcnt.resize(wmap.size());

    auto wmap_end = wmap.end();
    int ii = 0;
    for (auto it = wmap.begin(); it != wmap_end; ++it) {
        valcnt[ii++] = ValueCount(EventDataType(it.key()) * gain, it.value(), 0);
    }

    // Sort by weight, then value
    std::sort(valcnt.begin(), valcnt.end());

    double p = 100.0 * percent;
    double nth = double(SN) * percent;  // Target time position
    double nthi = floor(nth);

    qint64 sum1 = 0, sum2 = 0;
    qint64 w1, w2 = 0;
    double v1 = 0, v2;

    int N = valcnt.size();
    int k = 0;

    // Find the values that bracket the target percentile
    for (k = 0; k < N; k++) {
        v1 = valcnt.at(k).value;
        w1 = valcnt.at(k).count;
        sum1 += w1;

        if (sum1 > nthi) {
            return v1;
        }

        if (sum1 == nthi) {
            break;  // boundary condition
        }
    }

    if (k >= N) {
        return v1;
    }

    if (valcnt.size() == 1) {
        return valcnt[0].value;
    }

    v2 = valcnt[k + 1].value;
    w2 = valcnt[k + 1].count;
    sum2 = sum1 + w2;

    // Value lies between v1 and v2 - calculate linear interpolation
    double px = 100.0 / double(SN);  // Percentile represented by one full value

    // Calculate percentile ranks
    double p1 = px * (double(sum1) - (double(w1) / 2.0));
    double p2 = px * (double(sum2) - (double(w2) / 2.0));

    // Calculate linear interpolation
    double v = v1 + ((p - p1) / (p2 - p1)) * (v2 - v1);

    return v;
}

/*!
 * \brief Calculates multiple percentiles in a single pass with time-weighted linear interpolation
 * \param id Channel ID to calculate percentiles for
 * \return PercentilesResult containing median (50th), p90, p95, and p995
 * 
 * This function uses TIME-WEIGHTED percentile calculation to match Day::percentile().
 * Values are weighted by how long they were held (from m_timesummary), not by sample count.
 * This ensures session and day percentiles match for the same data.
 * 
 * This is more efficient than calling percentile() multiple times since we only build
 * the weight map once and reuse it for all percentile calculations.
 */
Session::PercentilesResult Session::calculatePercentiles(ChannelID id)
{
    PercentilesResult result;
    
    // Ensure m_valuesummary and m_timesummary are populated
    updateCountSummary(id);

    auto ei = m_valuesummary.find(id);
    if (ei == m_valuesummary.end()) {
#ifdef DBDEBUG
        qWarning() << "Session::calculatePercentiles() - no value summary for channel" << id << QString::number(id, 16);
#endif
        return result; // valid = false
    }

    auto tei = m_timesummary.find(id);
    bool timeweight = (tei != m_timesummary.end());
    
    if (!timeweight) {
        // Fallback: no time summary available
#ifdef DBDEBUG
        qWarning() << "Session::calculatePercentiles() - no time summary for channel" << id << QString::number(id, 16);
#endif
        return result; // valid = false
    }

    EventDataType gain = m_gain.value(id, 1.0);

    // Build weight map from time summary (time in seconds)
    QHash<EventStoreType, qint64> wmap;
    qint64 SN = 0;  // Total time (in seconds)

    for (auto it = tei.value().begin(), teival_end = tei.value().end(); it != teival_end; ++it) {
        qint64 weight = it.value();  // Time in seconds
        SN += weight;
        wmap[it.key()] += weight;
    }

    if (SN == 0) {
        return result; // valid = false
        qDebug() << "Session::calculatePercentiles has zero time weight for channel" << id << QString::number(id, 16) << "percentile calcs skipped";
    }

    // Build sorted list of value/counts
    QVector<ValueCount> valcnt;
    valcnt.resize(wmap.size());

    auto wmap_end = wmap.end();
    int ii = 0;
    for (auto it = wmap.begin(); it != wmap_end; ++it) {
        valcnt[ii++] = ValueCount(EventDataType(it.key()) * gain, it.value(), 0);
    }

    // Sort by weight, then value
    std::sort(valcnt.begin(), valcnt.end());

    // Helper lambda to calculate time-weighted percentile
    auto calcTimeWeightedPercentile = [&](double percent) -> EventDataType {
        double p = 100.0 * percent;
        double nth = double(SN) * percent;  // Target time position
        double nthi = floor(nth);

        qint64 sum1 = 0, sum2 = 0;
        qint64 w1, w2 = 0;
        double v1 = 0, v2;

        int N = valcnt.size();
        int k = 0;

        // Find the values that bracket the target percentile
        for (k = 0; k < N; k++) {
            v1 = valcnt.at(k).value;
            w1 = valcnt.at(k).count;
            sum1 += w1;

            if (sum1 > nthi) {
                return v1;
            }

            if (sum1 == nthi) {
                break;  // boundary condition
            }
        }

        if (k >= N) {
            return v1;
        }

        if (valcnt.size() == 1) {
            return valcnt[0].value;
        }

        v2 = valcnt[k + 1].value;
        w2 = valcnt[k + 1].count;
        sum2 = sum1 + w2;

        // Value lies between v1 and v2 - calculate linear interpolation
        double px = 100.0 / double(SN);  // Percentile represented by one full value

        // Calculate percentile ranks
        double p1 = px * (double(sum1) - (double(w1) / 2.0));
        double p2 = px * (double(sum2) - (double(w2) / 2.0));

        // Calculate linear interpolation
        double v = v1 + ((p - p1) / (p2 - p1)) * (v2 - v1);

        return v;
    };
    
    // Calculate each percentile with time weighting
    result.median = calcTimeWeightedPercentile(0.50);
    result.p90 = calcTimeWeightedPercentile(0.90);
    result.p95 = calcTimeWeightedPercentile(0.95);
    result.p995 = calcTimeWeightedPercentile(0.995);
    
    result.valid = true;
    
    return result;
}

EventDataType Session::wavg(ChannelID id)
{
    QHash<EventStoreType, quint32> vtime;
    QHash<ChannelID, EventDataType>::iterator i = m_wavg.find(id);

    if (i != m_wavg.end()) {
        return i.value();
    }

    updateCountSummary(id);

    QHash<ChannelID, QHash<EventStoreType, quint32> >::iterator j2 = m_timesummary.find(id);

    if (j2 == m_timesummary.end()) {
        return 0;
    }

    QHash<EventStoreType, quint32> &timesum = j2.value();

    if (!m_gain.contains(id)) {
        return 0;
    }

    double s0 = 0, s1 = 0, s2;

    EventDataType val, gain = m_gain[id];

    QHash<EventStoreType, quint32>::iterator vi = timesum.begin();
    QHash<EventStoreType, quint32>::iterator ts_end = timesum.end();

    for (; vi != ts_end; vi++) {
        val = vi.key() * gain;
        s2 = vi.value();
        s0 += s2;
        s1 += val * s2;
    }

    if (s0 > 0) {
        val = s1 / s0;
    } else { val = 0; }

    m_wavg[id] = val;
    return val;
}

EventDataType Session::calcMiddle(ChannelID code)
{
    int c = p_profile->general->prefCalcMiddle();

    if (c == 0) {
        return percentile(code, 0.5); // Median
    } else if (c == 1 ) {
        return wavg(code); // Weighted Average
    } else {
        return avg(code); // Average
    }
}

EventDataType Session::calcMax(ChannelID code)
{
    return p_profile->general->prefCalcMax() ? percentile(code, 0.995f) : Max(code);
}

EventDataType Session::calcPercentile(ChannelID code)
{
    double p = p_profile->general->prefCalcPercentile() / 100.0;
    return percentile(code, p);
}


EventList *Session::AddEventList(ChannelID code, EventListType et, EventDataType gain,
                                 EventDataType offset, EventDataType min, EventDataType max, EventDataType rate, bool second_field)
{
    schema::Channel *channel = &schema::channel[code];

    if (!channel) {
        qWarning() << "Channel" << code << "does not exist!";
        //return nullptr;
    }

    EventList *el = new EventList(et, gain, offset, min, max, rate, second_field);

    eventlist[code].push_back(el);
    //s_machine->registerChannel(chan);
    return el;
}
void Session::offsetSession(qint64 offset)
{
    //qDebug() << "Session starts" << QDateTime::fromSecsSinceEpoch(s_first/1000).toString("yyyy-MM-dd HH:mm:ss");
    s_first += offset;
    s_last += offset;
    QHash<ChannelID, quint64>::iterator it;

    QHash<ChannelID, quint64>::iterator end;

    it = m_firstchan.begin();
    end = m_firstchan.end();
    for (; it != end; it++) {
        if (it.value() > 0) {
            it.value() += offset;
        }
    }

    it = m_lastchan.begin();
    end = m_lastchan.end();
    for (; it != end; it++) {
        if (it.value() > 0) {
            it.value() += offset;
        }
    }

    QHash<ChannelID, QVector<EventList *> >::iterator i;
    QHash<ChannelID, QVector<EventList *> >::iterator el_end=eventlist.end();

    int el_s;

    for (i = eventlist.begin(); i != el_end; i++) {
        el_s=i.value().size();
        for (int j = 0; j < el_s; j++) {
            EventList *e = i.value()[j];

            e->setFirst(e->first() + offset);
            e->setLast(e->last() + offset);
        }
    }

    qDebug() << "Session now starts" << QDateTime::fromSecsSinceEpoch(s_first /
             1000).toString("yyyy-MM-dd HH:mm:ss");

}

bool Session::StoreToDatabase()
{
    PERF_TIMER_SCOPE("Session::StoreToDatabase");
#ifdef DBDEBUG
    qDebug() << "Session:StoreToDatabase() entered";
#endif
    if (s_first == 0) {
        qWarning() << "Session::StoreToDatabase(): Skipping session" << s_session << "with first=0";
        return false;
    }
    
    // Track that we're storing a session
    PerformanceTimer::instance().increment("SessionsImported");

    // Get machine's database ID
    PERF_TIMER_START("Session::StoreDB::CreateRepos");
    SessionRepository sessionRepo;
    SessionSettingsRepository settingsRepo;
    SessionChannelsRepository channelsRepo;
    SessionSlicesRepository slicesRepo;
    SessionSummariesRepository summariesRepo;
    PERF_TIMER_STOP("Session::StoreDB::CreateRepos");
    
    qint64 machineDbId = s_machine->getDatabaseId();
    if (machineDbId == 0) {
        qWarning() << "Session::StoreToDatabase(): Machine not in database yet, data not saved!";
        return false;
    }
    
    // Get profile_id from machine (Schema v12 requirement)
    qint64 profileId = s_machine->getProfileId();
    if (profileId == 0) {
        qWarning() << "Session::StoreToDatabase(): Machine has no profile_id";
        return false;
    }
    
    // 1. Create or update session record
    PERF_TIMER_START("Session::StoreDB::SessionRecord");
    SessionData sessionData;
    sessionData.machineId = machineDbId;
    sessionData.sessionId = s_session;
    sessionData.startTime = s_first;
    sessionData.endTime = s_last;
    sessionData.duration = s_last - s_first;
    sessionData.enabled = s_enabled;
    sessionData.summaryOnly = s_summaryOnly;
    sessionData.noSettings = s_noSettings;
    sessionData.summaryFile = "";  // No longer using .000 summary files - data is in database
    sessionData.eventsFile = "";  // No longer using .001 event files - data is in database
    
    if (m_sessionrow_id == 0) {
        // Create new session
        m_sessionrow_id = sessionRepo.create(sessionData);
        if (m_sessionrow_id < 0) {
            // Row already exists (e.g. prior import) — look it up and update instead
            SessionData existing = sessionRepo.findByMachineAndSessionId(machineDbId, s_session);
            if (existing.id > 0) {
                m_sessionrow_id = existing.id;
                sessionData.id = m_sessionrow_id;
                if (!sessionRepo.update(sessionData)) {
                    qWarning() << "Session::StoreToDatabase(): Failed to update session" << s_session;
                    PERF_TIMER_STOP("Session::StoreDB::SessionRecord");
                    return false;
                }
            } else {
                qWarning() << "Session::StoreToDatabase(): Failed to create session" << s_session;
                PERF_TIMER_STOP("Session::StoreDB::SessionRecord");
                return false;
            }
        }
    } else {
        // Update existing session
        sessionData.id = m_sessionrow_id;
        if (!sessionRepo.update(sessionData)) {
            qWarning() << "Session::StoreToDatabase(): Failed to update session" << s_session;
            PERF_TIMER_STOP("Session::StoreDB::SessionRecord");
            return false;
        }
    }
    PERF_TIMER_STOP("Session::StoreDB::SessionRecord");
    
    // 2. Save settings
    PERF_TIMER_START("Session::StoreDB::Settings");
    if (!settings.isEmpty()) {
        // Delete existing settings records for this session to prevent duplicates
        settingsRepo.removeBySession(m_sessionrow_id);
        
        QList<SessionSettingData> settingsList;
        for (auto it = settings.begin(); it != settings.end(); ++it) {
            SessionSettingData setting;
            setting.sessionId = m_sessionrow_id;
            setting.profileId = profileId;
            setting.channelId = it.key();
            
            // Handle special JSON serialization for bookmark fields (QVariantList, QStringList)
            if (it.key() == Bookmark_Start || it.key() == Bookmark_End) {
                // Serialize QVariantList to JSON
                QVariantList list = it.value().toList();
                QJsonArray array;
                for (const QVariant& v : list) {
                    array.append(QJsonValue::fromVariant(v));
                }
                setting.value = 0;  // Not used for JSON types
                setting.dataType = "json";
                setting.jsonValue = QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
            } else if (it.key() == Bookmark_Notes) {
                // Serialize QStringList to JSON
                QStringList list = it.value().toStringList();
                QJsonArray array;
                for (const QString& s : list) {
                    array.append(s);
                }
                setting.value = 0;  // Not used for JSON types
                setting.dataType = "json";
                setting.jsonValue = QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
            } else if (it.key() == Journal_Notes) {
                // Journal notes are stored as HTML text; skip if empty (no note entered).
                QString noteText = it.value().toString();
                if (noteText.isEmpty()) continue;
                setting.value = 0;
                setting.dataType = "text";
                setting.jsonValue = noteText;
            } else {
                // Standard numeric value (including Journal_Weight, Journal_ZombieMeter)
                setting.value = it.value().toDouble();
                setting.dataType = "numeric"; // Explicitly mark as numeric
                setting.jsonValue = QString();
            }
            
            settingsList.append(setting);
        }

        if (!settingsRepo.saveBatch(m_sessionrow_id, settingsList)) {
            qWarning() << "Session::StoreToDatabase(): Failed to save settings";
        }
    }
    PERF_TIMER_STOP("Session::StoreDB::Settings");
    
    // 3. Save channel statistics and value/time summaries
    PERF_TIMER_START("Session::StoreDB::Channels");
    if (!m_availableChannels.isEmpty()) {
        // Delete existing channel records for this session to prevent duplicates
        // This is necessary because INSERT OR REPLACE creates new autoincrement IDs,
        // leading to duplicate rows if StoreToDatabase() is called multiple times
        channelsRepo.removeBySession(m_sessionrow_id);
        
        QList<SessionChannelData> channelsList;
        SessionChannelValuesRepository valuesRepo;
        
        for (ChannelID id : m_availableChannels) {
            SessionChannelData channel;
            channel.channelId = id;
            channel.count = m_cnt.value(id, 0);
            channel.sum = m_sum.value(id, 0);
            channel.avg = m_avg.value(id, 0);
            channel.wavg = m_wavg.value(id, 0);
            channel.min = m_min.value(id, 0);
            channel.max = m_max.value(id, 0);
            
            // Calculate percentiles only for DATA channels (continuous readings like pressure, leak)
            // Skip some WAVEFORMS (percentiles not meaningful on some waveforms),
            // FLAGS, SPANs, and EVENTs (discrete events, percentiles don't make sense)
            schema::ChanType chanType = schema::channel[id].type();
            bool needsPercentiles = (chanType == schema::DATA || chanType == schema::WAVEFORM);
            if (chanType == schema::WAVEFORM) {
                // Skip calculate for specific waveform types
                if (   id == CPAP_FlowRate
                    || id == CPAP_RespEvent
                    || id == CPAP_MaskPressure
                    || id == CPAP_MaskPressureHi) {
                    needsPercentiles = false;
                }
            }
#ifdef DBDEBUG
            if (needsPercentiles && (channel.count > 10000))
                qDebug() << "Channel" << QString::number(id, 16) << "has lots of events -" << channel.count;
#endif

            // Check if eventlist actually contains data for this channel
            bool hasEventData = needsPercentiles &&
                                (eventlist.find(id) != eventlist.end()) &&
                                !eventlist[id].isEmpty() &&
                                eventlist[id][0]->count() > 0;

            // Fallback: value/time summaries loaded from the DB are sufficient
            // for percentile calculation even when events aren't in memory.
            // Without this, StoreToDatabase() called on a DB-loaded session
            // (events not loaded) overwrites correct p95/median with 0.
            bool hasSummaryData = needsPercentiles && !hasEventData &&
                                  m_valuesummary.contains(id) &&
                                  m_timesummary.contains(id);

            PERF_TIMER_START("Session::StoreDB::Channels::Calc");
            if (hasEventData || hasSummaryData) {
                // Use optimized multi-percentile calculator (~3x faster than calling percentile() 3 times)
                PercentilesResult percentiles = calculatePercentiles(id);
                if (percentiles.valid) {
                    channel.median = percentiles.median;
                    channel.p90 = percentiles.p90;
                    channel.p95 = percentiles.p95;
                } else {
                    channel.median = 0;
                    channel.p90 = 0;
                    channel.p95 = 0;
                }
            } else {
                // Skip percentiles for flag/event channels or when events not loaded
                channel.median = 0;
                channel.p90 = 0;
                channel.p95 = 0;
            }
            PERF_TIMER_STOP("Session::StoreDB::Channels::Calc");

            channel.cph = m_cph.value(id, 0);
            channel.sph = m_sph.value(id, 0);
            channel.gain = m_gain.value(id, 1.0);
            channel.firstTime = m_firstchan.value(id, 0);
            channel.lastTime = m_lastchan.value(id, 0);
            
            // Physical min/max — use accessor to trigger lazy floor/ceil computation
            // for loaders that don't call setPhysMin/setPhysMax explicitly.
            channel.physMin = physMin(id);
            channel.physMax = physMax(id);
#ifdef DBDEBUG
            if (id == 4355) {
                qDebug() << "Session::StoreToDatabase channel" << id << "P90" << channel.p90;
            }
#endif
            channelsList.append(channel);
        }

        // Use saveBatchWithIds to get channelId -> dbRowId mapping in one pass
        // This eliminates the N+1 findByChannel() calls that were needed before
        QHash<int, qint64> channelIdMap;
        if (!channelsRepo.saveBatchWithIds(m_sessionrow_id, profileId, channelsList, channelIdMap)) {
            qWarning() << "Session::StoreToDatabase(): Failed to save channels";
        }
        PERF_TIMER_STOP("Session::StoreDB::Channels");
        
        // 3b. Save value/time summaries using the ID map (no extra queries needed)
        PERF_TIMER_START("Session::StoreDB::ValueSummaries");
        for (const SessionChannelData& channelData : channelsList) {
            ChannelID id = channelData.channelId;
            
            auto valueSummaryIt = m_valuesummary.find(id);
            auto timeSummaryIt = m_timesummary.find(id);
            
            if (valueSummaryIt != m_valuesummary.end() && timeSummaryIt != m_timesummary.end()) {
                // Use the ID map directly instead of querying the database
                qint64 dbRowId = channelIdMap.value(id, 0);
                if (dbRowId > 0) {
                    if (!valuesRepo.saveChannelSummaries(dbRowId, 
                                                         valueSummaryIt.value(), 
                                                         timeSummaryIt.value())) {
                        qWarning() << "Session::StoreToDatabase(): Failed to save value/time summaries for channel" << id;
                    }
                }
            }
        }
        PERF_TIMER_STOP("Session::StoreDB::ValueSummaries");
    }
    
    // 4. Save slices
    PERF_TIMER_START("Session::StoreDB::Slices");
    if (!m_slices.isEmpty()) {
        // Delete existing slices records for this session to prevent duplicates
        slicesRepo.removeBySession(m_sessionrow_id);
        
        QList<SessionSliceData> slicesList;
        for (const SessionSlice& slice : m_slices) {
            SessionSliceData data;
            data.sessionId = m_sessionrow_id;
            data.startTime = slice.start;
            data.endTime = slice.end;
            data.status = slice.status;
            slicesList.append(data);
        }
        
        if (!slicesRepo.saveBatch(slicesList)) {
            qWarning() << "Session::StoreToDatabase(): Failed to save slices";
        }
    }
    PERF_TIMER_STOP("Session::StoreDB::Slices");
    
    // 5. Save summary statistics to session_summaries table
    PERF_TIMER_START("Session::StoreDB::Summaries");
    // Now that m_database_id is set, we can store the calculated summary data
    // already done?
    StoreSummaryToDatabase();
    PERF_TIMER_STOP("Session::StoreDB::Summaries");

#ifdef DBDEBUG
    qDebug() << "Session::StoreToDatabase(): Saved session" << s_session << "to database with ID" << m_sessionrow_id;
#endif
    // Clear dirty flag directly; don't use SetChanged() which has a side-effect on s_events_loaded.
    s_changed = false;
    return true;
}

bool Session::LoadFromDatabase()
{
    // Get machine's database ID
    qint64 machineDbId = s_machine->getDatabaseId();
    if (machineDbId == 0) {
        qWarning() << "Session::LoadFromDatabase(): Machine not in database yet";
        return false;
    }
    
    SessionRepository sessionRepo;
    SessionSettingsRepository settingsRepo;
    SessionChannelsRepository channelsRepo;
    SessionSlicesRepository slicesRepo;
    SessionSummariesRepository summariesRepo;
    
    // 1. Find and load session record
    SessionData sessionData = sessionRepo.findByMachineAndSessionId(machineDbId, s_session);
    if (sessionData.id == 0) {
        qWarning() << "Session::LoadFromDatabase(): Session" << s_session << "not found in database";
        return false;
    }

    // Store database ID for future updates
    m_sessionrow_id = sessionData.id;
    
    // Load basic session data
    s_first = sessionData.startTime;
    s_last = sessionData.endTime;
    s_enabled = sessionData.enabled;
    s_summaryOnly = sessionData.summaryOnly;
    s_noSettings = sessionData.noSettings;
    
#ifdef DBDEBUG
    qDebug() << "Session::LoadFromDatabase(): Loading session" << s_session
             << "from database ID" << m_sessionrow_id;
    if (s_summaryOnly)
        qDebug() << "Session::LoadFromDatabase(): Session" << s_session
                 << "is a summary only sesson";
#endif

    // 2. Load settings
    QList<SessionSettingData> settingsList = settingsRepo.findBySession(m_sessionrow_id);
    settings.clear();
    for (const SessionSettingData& setting : settingsList) {
        // Handle JSON deserialization for bookmark fields
        if (setting.dataType == "json" && !setting.jsonValue.isEmpty()) {
            QJsonDocument doc = QJsonDocument::fromJson(setting.jsonValue.toUtf8());
            if (doc.isArray()) {
                QJsonArray array = doc.array();
                
                // Deserialize based on channel type
                if (static_cast<ChannelID>(setting.channelId) == Bookmark_Start || 
                    static_cast<ChannelID>(setting.channelId) == Bookmark_End) {
                    // Convert JSON array to QVariantList
                    QVariantList list;
                    for (const QJsonValue& val : array) {
                        list.append(val.toVariant());
                    }
                    settings[setting.channelId] = list;
                } else if (static_cast<ChannelID>(setting.channelId) == Bookmark_Notes) {
                    // Convert JSON array to QStringList
                    QStringList list;
                    for (const QJsonValue& val : array) {
                        list.append(val.toString());
                    }
                    settings[setting.channelId] = list;
                } else {
                    // Generic JSON deserialization
                    settings[setting.channelId] = doc.toVariant();
                }
            } else {
                qWarning() << "Session::LoadFromDatabase(): JSON value for channel" 
                          << setting.channelId << "is not an array";
                settings[setting.channelId] = setting.value;
            }
        } else if (setting.dataType == "text") {
            // Handle text values (like Journal_Notes). jsonValue may be empty (empty note = "").
            if (setting.jsonValue.isEmpty())
                qWarning() << "LoadFromDatabase: session" << s_session
                           << "has text channel" << setting.channelId << "with empty json_value — stored as empty string";
            settings[setting.channelId] = setting.jsonValue;
        } else if (setting.dataType == "numeric" || setting.dataType.isEmpty()) {
            // Standard numeric value (including Journal_Weight, Journal_ZombieMeter)
            // Empty dataType is treated as numeric for backwards compatibility
            settings[setting.channelId] = setting.value;
        } else {
            // Fallback: treat as numeric value
            qWarning() << "Session::LoadFromDatabase(): Unknown dataType" << setting.dataType 
                      << "for channel" << setting.channelId << "- treating as numeric";
            settings[setting.channelId] = setting.value;
        }
    }
    
#ifdef DBDEBUG
    qDebug() << "Session::LoadFromDatabase(): Loaded" << settingsList.size() << "settings";
#endif

    // 3. Load channel statistics and value/time summaries
    QList<SessionChannelData> channelsList = channelsRepo.findBySession(m_sessionrow_id);
    
    // Clear existing channel data
    m_cnt.clear();
    m_sum.clear();
    m_avg.clear();
    m_wavg.clear();
    m_min.clear();
    m_max.clear();
    m_cph.clear();
    m_sph.clear();
    m_gain.clear();
    m_firstchan.clear();
    m_lastchan.clear();
    m_availableChannels.clear();
    m_valuesummary.clear();  // NEW - clear value summaries
    m_timesummary.clear();   // NEW - clear time summaries
    
    // Populate channel data from database
    for (const SessionChannelData& channel : channelsList) {
        ChannelID id = channel.channelId;
        m_availableChannels.push_back(id);
        m_cnt[id] = channel.count;
        if (channel.count != static_cast<int>(channel.count)) {   // channel.count is ok!
            qDebug() << "Session::LoadFromDatabase() channel" << id << "channel.count" << qSetRealNumberPrecision(9) <<channel.count;
        }
        m_sum[id] = channel.sum;
        m_avg[id] = channel.avg;
        m_wavg[id] = channel.wavg;
        m_min[id] = channel.min;
        m_max[id] = channel.max;
        m_physmin[id] = channel.physMin;
        m_physmax[id] = channel.physMax;
        m_cph[id] = channel.cph;
        m_sph[id] = channel.sph;
        m_gain[id] = channel.gain;
        m_firstchan[id] = channel.firstTime;
        m_lastchan[id] = channel.lastTime;
    }
    
#ifdef DBDEBUG
    qDebug() << "Session::LoadFromDatabase(): Loaded" << channelsList.size() << "channels";
#endif

    // 3b. Bulk-load value/time summaries for ALL channels in one query (eliminates N+1 pattern)
    SessionChannelValuesRepository valuesRepo;
    int valuesLoadedCount = 0;
    
    QHash<ChannelID, QHash<EventStoreType, EventStoreType>> allValueSummaries;
    QHash<ChannelID, QHash<EventStoreType, quint32>> allTimeSummaries;
    
    if (valuesRepo.loadAllChannelSummaries(m_sessionrow_id, allValueSummaries, allTimeSummaries)) {
        for (auto it = allValueSummaries.begin(); it != allValueSummaries.end(); ++it) {
            m_valuesummary[it.key()] = it.value();
            valuesLoadedCount++;
        }
        for (auto it = allTimeSummaries.begin(); it != allTimeSummaries.end(); ++it) {
            m_timesummary[it.key()] = it.value();
        }
    }
    
#ifdef DBDEBUG
    if (valuesLoadedCount > 0) {
        qDebug() << "Session::LoadFromDatabase(): Loaded value/time summaries for" << valuesLoadedCount << "channels";
    }
#endif

    // 4. Load slices
    QList<SessionSliceData> slicesList = slicesRepo.findBySession(m_sessionrow_id);
    m_slices.clear();
    
    for (const SessionSliceData& sliceData : slicesList) {
        SessionSlice slice;
        slice.start = sliceData.startTime;
        slice.end = sliceData.endTime;
        slice.status = (SliceStatus)sliceData.status;
        m_slices.append(slice);
    }
    
#ifdef DBDEBUG
    qDebug() << "Session::LoadFromDatabase(): Loaded" << slicesList.size() << "slices";
#endif

    // 5. Load summary data (optional - contains computed values)
    // GTS: Nothing optional about it. Summary data ALWAYS exists.
    SessionSummaryData summaryData = summariesRepo.findBySession(m_sessionrow_id);
#ifdef DBDEBUG
    if (s_summaryOnly) {
        // Summary data is available - we could use it to pre-populate
        // some calculated values, but for now we'll just log it
        qDebug() << "Session::LoadFromDatabase(): Found summary data - AHI:" 
                 << summaryData.ahi << "Hours:" << summaryData.hoursUsed;
    }
#endif

    // For summary-only sessions, restore m_cnt from session_summaries
    // This is necessary because summary-only sessions don't have event data
    // stored in session_channels, but the event counts are stored in session_summaries
    if (s_summaryOnly) {
        double sessionHours = summaryData.hoursUsed;
#ifdef DBDEBUG
        qDebug() << "Session::LoadFromDatabase(): Found summary only session, SummaryData.id" << summaryData.id;
#endif

        // Restore event counts from summary data.
        // Prefer cph * hours over the stored count: cph is preserved as REAL, while
        // the count was truncated to INTEGER in session_channels.
        auto restoreCount = [&](ChannelID id, int storedCount) {
            if (storedCount <= 0 && !(sessionHours > 0 && m_cph.contains(id))) { return; }

            if (m_cph.contains(id) && sessionHours > 0) {
                m_cnt[id] = qRound(static_cast<double>(m_cph[id] * sessionHours));
            } else if (storedCount > 0) {
                m_cnt[id] = qRound(static_cast<double>(storedCount));
            }
            if (!m_availableChannels.contains(id)) {
                m_availableChannels.push_back(id);
            }
        };

        restoreCount(CPAP_Obstructive, summaryData.obstructiveCount);
        restoreCount(CPAP_ClearAirway, summaryData.clearAirwayCount);
        restoreCount(CPAP_Hypopnea, summaryData.hypopneaCount);
        restoreCount(CPAP_ObstructiveHypopnea, summaryData.obstructiveHypopneaCount);
        restoreCount(CPAP_CentralHypopnea, summaryData.centralHypopneaCount);
        restoreCount(CPAP_Apnea, summaryData.unclassifiedCount);
        restoreCount(CPAP_AllApnea, summaryData.allApneaCount);
        restoreCount(CPAP_RERA, summaryData.reraCount);
    }

    // Mark summary as loaded since we have the cached statistics
    s_summary_loaded = true;
    
#ifdef DBDEBUG
    qDebug() << "Session::LoadFromDatabase(): Successfully loaded session" << s_session
             << "from database";
#endif
    return true;
}

bool Session::StoreSummaryToDatabase()
{
#ifdef DBDEBUG
    qDebug() << "Session::StoreSummaryToDatabase() entered";
#endif

    if (m_sessionrow_id == 0) {
        qWarning() << "Session::StoreSummaryToDatabase() - session not in database";
        return false;
    }
    
    // Get profile_id from machine (Schema v12 requirement)
    qint64 profileId = s_machine->getProfileId();
    if (profileId == 0) {
        qWarning() << "Session::StoreSummaryToDatabase() - machine has no profile_id";
        return false;
    }
    
    SessionSummariesRepository sessionSummariesRepo;
    SessionSummaryData sessionSummaryData;
    
    sessionSummaryData.sessionId = m_sessionrow_id;
    sessionSummaryData.profileId = profileId;
    
    // Calculate hours used — full session span regardless of slices.
    // hours() returns MaskOn slice time when slices are present, which would make
    // hoursUsed == maskOnHours.  Use the raw s_last/s_first span instead.
    sessionSummaryData.hoursUsed = (s_last > s_first) ? (s_last - s_first) / 3600000.0 : 0.0;
    
    // Calculate mask-on hours if we have slices
    if (!m_slices.isEmpty()) {
        double maskOnTime = 0;
        for (const SessionSlice& slice : m_slices) {
            if (slice.status == MaskOn) {
                maskOnTime += (slice.end - slice.start) / 3600000.0;
            }
        }
        sessionSummaryData.maskOnHours = maskOnTime;
    } else {
        sessionSummaryData.maskOnHours = sessionSummaryData.hoursUsed;
    }
    
    // Get AHI and RDI from cached values
    // For summary-only sessions, calculate AHI from event counts since m_wavg[CPAP_AHI] won't be set
    if (m_wavg.contains(CPAP_AHI)) {
        sessionSummaryData.ahi = m_wavg[CPAP_AHI];
    } else {
        // Calculate AHI from event counts for summary-only sessions.
        // count() walks ahiChannels, so this stays correct as channels are added and
        // cannot drift from Day::calcAHI() the way the previous hand-rolled sum did
        // (it omitted CPAP_AllApnea and added CPAP_RERA, i.e. it computed RDI).
        if (sessionSummaryData.hoursUsed > 0) {
            sessionSummaryData.ahi = count(AllAhiChannels) / sessionSummaryData.hoursUsed;
        }
    }
    if (m_wavg.contains(CPAP_RDI)) {
        sessionSummaryData.rdi = m_wavg[CPAP_RDI];
    }

    // Obstructive and central shares of the AHI. Always count-derived: no device
    // reports them, so unlike ahi above there is no cached value to prefer. They sum
    // to the count-derived AHI exactly; against a device-reported m_wavg[CPAP_AHI]
    // they may differ slightly, the same way OSCAR's own AHI already can.
    if (sessionSummaryData.hoursUsed > 0) {
        sessionSummaryData.oahi = count(AllOahiChannels) / sessionSummaryData.hoursUsed;
        sessionSummaryData.cahi = count(AllCahiChannels) / sessionSummaryData.hoursUsed;
    }

    // Event counts from cached values
    if (m_cnt.contains(CPAP_Obstructive)) {
        sessionSummaryData.obstructiveCount = m_cnt[CPAP_Obstructive];
    }
    if (m_cnt.contains(CPAP_ClearAirway)) {
        sessionSummaryData.clearAirwayCount = m_cnt[CPAP_ClearAirway];
    }
    if (m_cnt.contains(CPAP_Hypopnea)) {
        sessionSummaryData.hypopneaCount = m_cnt[CPAP_Hypopnea];
    }
    if (m_cnt.contains(CPAP_ObstructiveHypopnea)) {
        sessionSummaryData.obstructiveHypopneaCount = m_cnt[CPAP_ObstructiveHypopnea];
    }
    if (m_cnt.contains(CPAP_CentralHypopnea)) {
        sessionSummaryData.centralHypopneaCount = m_cnt[CPAP_CentralHypopnea];
    }
    if (m_cnt.contains(CPAP_RERA)) {
        sessionSummaryData.reraCount = m_cnt[CPAP_RERA];
    }
    // Also handle CPAP_Apnea (unknown/unclassified apnea) - store in unclassifiedCount
    if (m_cnt.contains(CPAP_Apnea)) {
        sessionSummaryData.unclassifiedCount = m_cnt[CPAP_Apnea];
    }
    // CPAP_AllApnea is the undifferentiated apnea a few devices report. It counts
    // towards AHI, so leaving it unstored made SQL sums over these columns disagree
    // with the stored ahi for those devices (schema v18 added the column).
    if (m_cnt.contains(CPAP_AllApnea)) {
        sessionSummaryData.allApneaCount = m_cnt[CPAP_AllApnea];
    }

    // Pressure statistics from cached values
    if (m_wavg.contains(CPAP_Pressure)) {
        sessionSummaryData.pressureAvg = m_wavg[CPAP_Pressure];
        if (m_min.contains(CPAP_Pressure)) {
            sessionSummaryData.pressureMin = m_min[CPAP_Pressure];
        }
        if (m_max.contains(CPAP_Pressure)) {
            sessionSummaryData.pressureMax = m_max[CPAP_Pressure];
        }
        // For 95th percentile, we need to calculate it if events are loaded
        // Otherwise leave at 0
        if (s_events_loaded) {
            sessionSummaryData.pressure95th = percentile(CPAP_Pressure, 0.95);
        }
    }
    
    // Leak statistics from cached values
    if (m_wavg.contains(CPAP_LeakTotal)) {
        sessionSummaryData.leakTotalAvg = m_wavg[CPAP_LeakTotal];
        if (m_max.contains(CPAP_LeakTotal)) {
            sessionSummaryData.leakTotalMax = m_max[CPAP_LeakTotal];
        }
        // For 95th percentile
        if (s_events_loaded) {
            sessionSummaryData.leakTotal95th = percentile(CPAP_LeakTotal, 0.95);
        }
    }
    
    // Oximetry data from cached values if available
    if (m_wavg.contains(OXI_SPO2)) {
        sessionSummaryData.spo2Avg = m_wavg[OXI_SPO2];
        if (m_min.contains(OXI_SPO2)) {
            sessionSummaryData.spo2Min = m_min[OXI_SPO2];
        }
    }
    if (m_wavg.contains(OXI_Pulse)) {
        sessionSummaryData.pulseAvg = m_wavg[OXI_Pulse];
    }
    
    // Create or update the summary
    bool success = sessionSummariesRepo.createOrUpdate(sessionSummaryData);
    
    if (success) {
#ifdef DBDEBUG
        qDebug() << "Session::StoreSummaryToDatabase() - Saved or updated summary for session" << s_session;
//                 << "AHI:" << data.ahi << "hours:" << data.hoursUsed;
#endif
    } else {
        qWarning() << "Session::StoreSummaryToDatabase() - Failed to save summary for session" << s_session;
    }
    
    return success;
}

qint64 Session::correctionMs() const
{
    return s_machine->correctionMs(m_night);
}

qint64 Session::first()
{
    return s_first + correctionMs();
}

qint64 Session::last()
{
    return s_last + correctionMs();
}

// ===== NEW DATABASE STORAGE FOR EVENTS/WAVEFORMS =====

QList<RespiratoryEventData> Session::extractRespiratoryEvents()
{
    QList<RespiratoryEventData> events;
#ifdef DBDEBUG
    qDebug() << "=== Session::extractRespiratoryEvents() CALLED for session" << s_session << "===";
    qDebug() << "    Session starts" << QDateTime::fromSecsSinceEpoch(s_first/1000).toString("yyyy-MM-dd HH:mm:ss");
    qDebug() << "    eventlist.size():" << eventlist.size();
#endif

    // Get profile_id from machine (Schema v12 requirement)
    qint64 profileId = s_machine->getProfileId();
#ifdef DBDEBUG
    qDebug() << "    profileId:" << profileId;
#endif

    if (profileId == 0) {
        qWarning() << "Session::extractRespiratoryEvents() - machine has no profile_id";
        return events;  // Return empty list
    }
    
    // Primary respiratory event channel IDs
    // If channel is in this list, eventType = 1; otherwise eventType = 0
    QSet<ChannelID> primaryRespiratoryChannels = {
        CPAP_Obstructive,    // OA
        CPAP_Apnea,          // UA (unclassified)
        CPAP_Hypopnea,       // H
        CPAP_ObstructiveHypopnea, // OH
        CPAP_CentralHypopnea,     // CH
        CPAP_RERA,           // RERA
        CPAP_ClearAirway,    // CA
        CPAP_AllApnea        // All
    };
    
    // Examine all channels in eventlist
#ifdef DBDEBUG
    int channelsProcessed = 0;
    int flagChannels = 0;
#endif
//    qDebug() << "    Checking channel types:";
    for (auto channelIt = eventlist.begin(); channelIt != eventlist.end(); ++channelIt) {
        ChannelID channelId = channelIt.key();
#ifdef DBDEBUG
        channelsProcessed++;
#endif
        // Get channel type from schema
        schema::ChanType chanType = schema::channel[channelId].type();
        
        // Check if FLAG, MINOR_FLAG, or SPAN bits are set
        bool isFlagType = (chanType & schema::FLAG) || 
                          (chanType & schema::MINOR_FLAG) || 
                          (chanType & schema::SPAN);
        
        if (!isFlagType) continue;
        
#ifdef DBDEBUG
        flagChannels++;
        qDebug() << "    Processing FLAG channel:" << QString::number(channelId, 16)
                 << "EventLists:" << channelIt.value().size();
#endif
        // Determine eventType based on whether channel is in primary list
        int eventType = primaryRespiratoryChannels.contains(channelId) ? 1 : 0;
        
        // Process all EventLists for this channel
        for (EventList* eventList : channelIt.value()) {
            if (!eventList || eventList->type() != EVL_Event) continue;
            
            qint64 startBase = eventList->first();
            
            // Skip if startTime is zero
            if (startBase == 0) continue;
            
            quint32* timePtr = eventList->rawTime();
            EventStoreType* dataPtr = eventList->rawData();
            
            for (quint32 i = 0; i < eventList->count(); i++) {
                qint64 startTime = startBase + timePtr[i];
                
                // Skip if startTime is zero
                if (startTime == 0) continue;
#ifdef DBDEBUG
                qDebug() << "Session::extractRespiratoryEvents() profile" <<  profileId << "channel" << channelId
                         << "eventType" << eventType;
#endif
                RespiratoryEventData event;
                event.sessionId = m_sessionrow_id;
                event.profileId = profileId;  // Schema v12 requirement
                event.channelId = channelId;   // Schema v12 requirement
                event.eventType = eventType;
                event.startTime = startTime;
                event.duration = static_cast<int>(dataPtr[i]);
                event.endTime = event.startTime + (event.duration * 1000LL);
                event.desaturation = 0.0;  // TODO: Link to SpO2 data in future
                event.severity = 0;
                events.append(event);
            }
        }
    }
    
#ifdef DBDEBUG
    qDebug() << "=== extractRespiratoryEvents() COMPLETE ===";
    qDebug() << "    Channels processed:" << channelsProcessed;
    qDebug() << "    FLAG channels:" << flagChannels;
    qDebug() << "    Total events extracted:" << events.size();
#endif

    return events;
}

bool Session::StoreEventsToDatabase()
{
    PERF_TIMER_SCOPE("Session::StoreEventsToDatabase");
    
    if (m_sessionrow_id == 0) {
        qWarning() << "Session::StoreEventsToDatabase() - session not in database";
        return false;
    }
    
    if (eventlist.isEmpty()) {
        qDebug() << "Session::StoreEventsToDatabase() - no events to store";
        return true;
    }
    
    // Get profile_id from machine (Schema v12 requirement)
    qint64 profileId = s_machine->getProfileId();
    if (profileId == 0) {
        qWarning() << "Session::StoreEventsToDatabase() - machine has no profile_id";
        return false;
    }
    
    EventListRepository eventListRepo;
    EventDataRepository eventDataRepo;

    // Clear existing event lists for this session before re-inserting,
    // so re-imports (e.g. to add newly-computed channels) don't hit UNIQUE constraints.
    // If the delete fails, stale rows remain and subsequent INSERTs will hit the
    // UNIQUE(session_id, channel_id, eventlist_index) constraint.
    if (!eventListRepo.deleteBySession(m_sessionrow_id)) {
        qWarning() << "Session::StoreEventsToDatabase() - Failed to clear existing event_lists for session"
                   << s_session << "— subsequent INSERTs may fail on UNIQUE constraint";
    }

    int totalEventLists = 0;
    int totalSaved = 0;
    qint64 totalUncompressed = 0;
    qint64 totalCompressed = 0;
    
#ifdef DBDEBUG
    qDebug() << "Session::StoreEventsToDatabase() - Storing events for session" << s_session;
#endif

    // Iterate through all channels
    QHash<ChannelID, QVector<EventList *> >::iterator channelIt;
    for (channelIt = eventlist.begin(); channelIt != eventlist.end(); ++channelIt) {
        ChannelID channelId = channelIt.key();
        QVector<EventList *> &eventLists = channelIt.value();
        
        // Iterate through all EventLists for this channel
        for (int index = 0; index < eventLists.size(); ++index) {
            EventList* eventList = eventLists[index];
            
            if (!eventList || eventList->count() == 0) {
                continue;  // Skip empty EventLists
            }

            // Guard against corrupted source data: a count field that overflows to a huge
            // value produces a blob SQLite cannot bind (SQLITE_TOOBIG). No legitimate CPAP
            // night comes close to 100 MB for a single event list (even 250 Hz waveforms
            // for 8 hours are ~14 MB). Skip and log; don't count in totalEventLists so the
            // rest of the session still imports cleanly.
            {
                qint64 estimatedSize = (qint64)eventList->count() * (qint64)sizeof(EventStoreType);
                if (eventList->hasSecondField()) estimatedSize *= 2;
                if (eventList->type() != EVL_Waveform)
                    estimatedSize += (qint64)eventList->count() * (qint64)sizeof(quint32);
                if (estimatedSize > 100LL * 1024 * 1024) {
                    qWarning() << "Session::StoreEventsToDatabase() - Skipping oversized event list"
                               << "channel" << channelId << "index" << index
                               << "count" << eventList->count()
                               << "estimated" << estimatedSize << "bytes — corrupted source data";
                    continue;
                }
            }

            totalEventLists++;
            
            // 1. Create EventListData from EventList
            EventListData listData;
            listData.sessionId = m_sessionrow_id;
            listData.profileId = profileId;
            listData.channelId = channelId;
            listData.eventlistIndex = index;
            listData.eventType = (int)eventList->type();
            listData.firstTime = eventList->first();
            listData.lastTime = eventList->last();
            listData.count = eventList->count();
            listData.rate = eventList->rate();
            listData.gain = eventList->gain();
            listData.offset = eventList->offset();
            listData.minValue = eventList->Min();
            listData.maxValue = eventList->Max();
            listData.dimension = eventList->dimension();
            listData.hasSecondField = eventList->hasSecondField();
            
            if (listData.hasSecondField) {
                listData.min2Value = eventList->min2();
                listData.max2Value = eventList->max2();
            }
            
            // Calculate uncompressed data size
            listData.dataSize = eventList->count() * sizeof(EventStoreType);
            if (listData.hasSecondField) {
                listData.dataSize += eventList->count() * sizeof(EventStoreType);
            }
            if (eventList->type() != EVL_Waveform) {
                listData.dataSize += eventList->count() * sizeof(quint32);
            }
            
            totalUncompressed += listData.dataSize;
            
            // 2. Create EventList metadata record in database
            qint64 eventListId = eventListRepo.create(listData);
            if (eventListId < 0) {
                qWarning() << "Session::StoreEventsToDatabase() - Failed to create event_list for channel"
                          << channelId << "index" << index;
                continue;
            }
            
            // 3. Store binary data
            if (!eventDataRepo.storeEventListData(eventListId, eventList)) {
                qWarning() << "Session::StoreEventsToDatabase() - Failed to store event data for channel"
                          << channelId << "index" << index;
                // Delete the metadata record since data storage failed
                eventListRepo.deleteById(eventListId);
                continue;
            }
            
            totalSaved++;
            
            // Track compressed size from the data we already have
            // (storeEventListData already updates compressed_size in the DB)
            totalCompressed += listData.dataSize;  // Approximate; exact value is in DB
        }
    }
    
#ifdef DBDEBUG
    // Log overall compression statistics for all EventLists combined
    if (totalCompressed > 0 && totalUncompressed > 0) {
        double ratio = (100.0 * totalCompressed / totalUncompressed);
        qDebug() << "Session" << s_session << "stored" << totalSaved << "EventLists:"
                 << totalUncompressed << "bytes ->" << totalCompressed << "bytes"
                 << "(" << QString::number(ratio, 'f', 1) << "%)";
    }
#endif

    // NEW: Extract and store respiratory events to respiratory_events table
    PERF_TIMER_START("Session::StoreDB::RespiratoryEvents");
    QList<RespiratoryEventData> respiratoryEvents = extractRespiratoryEvents();
    if (!respiratoryEvents.isEmpty()) {
        RespiratoryEventsRepository respRepo;
        if (!respRepo.createBatch(respiratoryEvents)) {
            qWarning() << "Session::StoreEventsToDatabase() - Failed to store respiratory events";
        } else {
            qDebug() << "Session" << s_session << "stored" << respiratoryEvents.size() << "respiratory events";
        }
    }
    PERF_TIMER_STOP("Session::StoreDB::RespiratoryEvents");
    
    return (totalSaved == totalEventLists);
}

bool Session::LoadEventsFromDatabase()
{
    if (m_sessionrow_id == 0) {
        qWarning() << "Session::LoadEventsFromDatabase() - session not in database";
        return false;
    }
    
    EventListRepository eventListRepo;
    EventDataRepository eventDataRepo;
    
    // Get all EventList metadata for this session
    QList<EventListData> eventListsData = eventListRepo.findBySession(m_sessionrow_id);
    
    if (eventListsData.isEmpty()) {
#ifdef DBDEBUG
        qDebug() << "Session::LoadEventsFromDatabase() - No events found for session" << s_session;
#endif
        return false;
    }
    
#ifdef DBDEBUG
    qDebug() << "Session::LoadEventsFromDatabase() - Loading" << eventListsData.size()
             << "EventLists for session" << s_session;
#endif

    int loaded = 0;
    
    // Create EventLists from database data
    for (const EventListData& listData : eventListsData) {
        // Create the EventList object
        EventListType type = (EventListType)listData.eventType;
        EventList* eventList = AddEventList(
            listData.channelId,
            type,
            listData.gain,
            listData.offset,
            listData.minValue,
            listData.maxValue,
            listData.rate,
            listData.hasSecondField
        );
        
        // Set metadata
        eventList->setFirst(listData.firstTime);
        eventList->setLast(listData.lastTime);
        eventList->setCount(listData.count);
        eventList->setDimension(listData.dimension);
        
        if (listData.hasSecondField) {
            eventList->setMin2(listData.min2Value);
            eventList->setMax2(listData.max2Value);
        }
        
        // Load binary data from database
        if (!eventDataRepo.loadEventListData(listData.id, eventList)) {
            qWarning() << "Session::LoadEventsFromDatabase() - Failed to load event data for channel"
                      << listData.channelId << "index" << listData.eventlistIndex;
            // Remove the partially loaded EventList
            eventlist[listData.channelId].removeLast();
            continue;
        }
        
        loaded++;
    }
    
#ifdef DBDEBUG
    qDebug() << "Session::LoadEventsFromDatabase() - Successfully loaded" << loaded
             << "EventLists for session" << s_session;
#endif

    return (loaded == eventListsData.size());
}

// ===== END NEW DATABASE STORAGE =====

// ===== IMPORT-SPECIFIC FILE LOADERS =====
// These methods are used during profile import to load data from .000 and .001 files
// They bypass database checks and load directly from files

bool Session::LoadSummaryFromFile(const QString& filename)
{
//    qDebug() << "Session::LoadSummaryFromFile() - Loading from" << filename;
    
    if (filename.isEmpty()) {
        qDebug() << "Empty summary filename";
        return false;
    }

    QFile file(filename);

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Could not open summary file" << filename << "for reading, error code" << file.error() << file.errorString();
        return false;
    }

    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_4_6);
    in.setByteOrder(QDataStream::LittleEndian);

    quint32 t32;
    quint16 t16;

    in >> t32;

    if (t32 != magic) {
        qWarning() << "Wrong magic number in " << filename;
        file.close();
        return false;
    }

    quint16 version;
    in >> version;      // DB Version

    if (version < 6) {
        qWarning() << "Old dbversion " << version <<
                   "summary file.. Sorry, you need to purge and reimport";
        file.close();
        return false;
    }

    in >> t16;      // File Type

    if (t16 != filetype_summary) {
        qDebug() << "Wrong file type"; //wrong file type
        file.close();
        return false;
    }

    quint32 ts32;
    in >> ts32;      // MachineID (dont need this result)

//    bool upgrade = false;
    if ( ts32 != s_machine->id()) {
//        upgrade = true;
        qWarning() << "Machine ID does not match in" << filename <<
                   " I will try to load anyway in case you know what your doing.";
    }

    in >> t32;      // Sessionid;
    s_session = t32;

    in >> s_first;  // Start time
    in >> s_last;   // Duration

    QHash<ChannelID, EventDataType> cruft;

    if (version < 7) {
        // This code is deprecated.. just here incase anyone tries anything crazy...
        QHash<QString, QVariant> v1;
        in >> v1;
        settings.clear();
        ChannelID code;

        for (QHash<QString, QVariant>::iterator i = v1.begin(); i != v1.end(); i++) {
            code = schema::channel[i.key()].id();
            settings[code] = i.value();
        }

        QHash<QString, int> zcnt;
        in >> zcnt;
        for (QHash<QString, int>::iterator i = zcnt.begin(); i != zcnt.end(); i++) {
            code = schema::channel[i.key()].id();
            m_cnt[code] = i.value();
        }

        QHash<QString, double> zsum;
        in >> zsum;

        for (QHash<QString, double>::iterator i = zsum.begin(); i != zsum.end(); i++) {
            code = schema::channel[i.key()].id();
            m_sum[code] = i.value();
        }

        QHash<QString, EventDataType> ztmp;
        in >> ztmp; // avg

        for (QHash<QString, EventDataType>::iterator i = ztmp.begin(); i != ztmp.end(); i++) {
            code = schema::channel[i.key()].id();
            m_avg[code] = i.value();
        }

        ztmp.clear();
        in >> ztmp; // wavg

        for (QHash<QString, EventDataType>::iterator i = ztmp.begin(); i != ztmp.end(); i++) {
            code = schema::channel[i.key()].id();
            m_wavg[code] = i.value();
        }

        ztmp.clear();
        in >> ztmp; // 90p
        ztmp.clear();
        in >> ztmp; // min

        for (QHash<QString, EventDataType>::iterator i = ztmp.begin(); i != ztmp.end(); i++) {
            code = schema::channel[i.key()].id();
            m_min[code] = i.value();
        }

        ztmp.clear();
        in >> ztmp; // max

        for (QHash<QString, EventDataType>::iterator i = ztmp.begin(); i != ztmp.end(); i++) {
            code = schema::channel[i.key()].id();
            m_max[code] = i.value();
        }

        ztmp.clear();
        in >> ztmp; // cph

        for (QHash<QString, EventDataType>::iterator i = ztmp.begin(); i != ztmp.end(); i++) {
            code = schema::channel[i.key()].id();
            m_cph[code] = i.value();
        }

        ztmp.clear();
        in >> ztmp; // sph

        for (QHash<QString, EventDataType>::iterator i = ztmp.begin(); i != ztmp.end(); i++) {
            code = schema::channel[i.key()].id();
            m_sph[code] = i.value();
        }

        QHash<QString, quint64> ztim;
        in >> ztim; //firstchan

        for (QHash<QString, quint64>::iterator i = ztim.begin(); i != ztim.end(); i++) {
            code = schema::channel[i.key()].id();
            m_firstchan[code] = i.value();
        }

        ztim.clear();
        in >> ztim; // lastchan

        for (QHash<QString, quint64>::iterator i = ztim.begin(); i != ztim.end(); i++) {
            code = schema::channel[i.key()].id();
            m_lastchan[code] = i.value();
        }
    } else {
        // version >= 7

        in >> settings;
        if (version < 13) {
            QHash<ChannelID, int> cnt2;
            in >> cnt2;

            QHash<ChannelID, int>::iterator it;

            for (it = cnt2.begin(); it != cnt2.end(); ++it) {
                m_cnt[it.key()] = it.value();
            }
        } else {
            in >> m_cnt;
        }
        in >> m_sum;
        in >> m_avg;
        in >> m_wavg;

        if (version < 11) {
            cruft.clear();
            in >> cruft; // 90%

            if (version >= 10) {
                cruft.clear();
                in >> cruft;// med
                cruft.clear();
                in >> cruft; //p95
            }
        }

        in >> m_min;
        in >> m_max;

        // Added 24/10/2013 by MW to support physical graph min/max values
        if (version >= 12) {
            in >> m_physmin;
            in >> m_physmax;
        }

        in >> m_cph;
        in >> m_sph;
        in >> m_firstchan;
        in >> m_lastchan;

        if (version >= 8) {
            in >> m_valuesummary;
            in >> m_timesummary;

            if (version >= 9) {
                in >> m_gain;
            }
        }

        // screwed up with version 14
        if (version >= 15) {
            in >> m_availableChannels;
            in >> m_timeAboveTheshold;
            in >> m_upperThreshold;
            in >> m_timeBelowTheshold;
            in >> m_lowerThreshold;
        }

        if (version == 13) {
            QHash<ChannelID, QVariant>::iterator it = settings.find(CPAP_SummaryOnly);
            if (it != settings.end()) {
                s_summaryOnly = (*it).toBool();
            } else s_summaryOnly = false;
        } else if (version > 13) {
            in >> s_summaryOnly;
        }
        
        if (version >= 18) {
            in >> s_noSettings;
        } else {
            s_noSettings = (settings.size() == 0);
        }

        if (version == 16) {
            QList<SessionSlice> slices;
            in >> slices;
            m_slices.clear();
            for (int i=0;i<slices.size(); ++i) {
                m_slices.append(slices[i]);
            }
        } else if (version >= 17) {
            in >> m_slices;
        }
    }

    // Rebuild m_availableChannels if it wasn't stored in the file (version < 15 or version 14 bug).
    // Without this, StoreToDatabase() skips session_channels entirely — losing m_cph (event rates).
    // The restore block in LoadFromDatabase() needs m_cph to recover fractional event counts
    // (e.g., summary-only sessions store count = rate * hours = 0.997, which truncates to 0 as int).
    if (m_availableChannels.isEmpty()) {
        for (auto it = m_cnt.begin(); it != m_cnt.end(); ++it) {
            if (!m_availableChannels.contains(it.key()))
                m_availableChannels.push_back(it.key());
        }
        for (auto it = m_cph.begin(); it != m_cph.end(); ++it) {
            if (!m_availableChannels.contains(it.key()))
                m_availableChannels.push_back(it.key());
        }
    }

    file.close();

    s_summary_loaded = true;
    s_enabled = 1;
    
//    qDebug() << "Session::LoadSummaryFromFile() - Successfully loaded session" << s_session
//             << "from" << filename;
    
    return true;
}

bool Session::LoadEventsFromFile(const QString& filename)
{
//    qDebug() << "Session::LoadEventsFromFile() - Loading from" << filename;
    
    // Skip event loading for journal machines - they only have settings
    if (s_machine->type() == MT_JOURNAL) {
        return true;  // No events to load for journals
    }
    
    quint32 magicnum, machid, sessid;
    quint16 version, type, crc16, machtype, compmethod;
    quint8 t8;
    qint32 datasize;

    if (filename.isEmpty()) {
        qDebug() << "Session::LoadEventsFromFile() Filename is empty";
        return false;
    }

    QFile file(filename);

    if ( ! file.open(QIODevice::ReadOnly)) {
        qDebug() << "No Event/Waveform data available for" << s_session << "filename" << filename;
        return false;  // Not an error - many sessions may not have .001 files
    }

    QByteArray headerbytes = file.read(42);

    QDataStream header(headerbytes);
    header.setVersion(QDataStream::Qt_4_6);
    header.setByteOrder(QDataStream::LittleEndian);

    header >> magicnum;         // Magic Number (quint32)
    header >> version;          // Version (quint16)
    header >> type;             // File type (quint16)
    header >> machid;           // Device ID (quint32)
    header >> sessid;           //(quint32)
    header >> s_first;          //(qint64)
    header >> s_last;           //(qint64)

    if (type != filetype_data) {
        qDebug() << "Wrong File Type in " << filename;
        file.close();
        return false;
    }

    if (magicnum != magic) {
        qWarning() << "Wrong Magic number in " << filename;
        file.close();
        return false;
    }

    if (version < 6) {  // prior to version 6 is too old to deal with
        qDebug() << "Old File Version, can't open file";
        file.close();
        return false;
    }

    if (version < 10) {
        file.seek(32);
    } else {
        header >> compmethod;   // Compression Method (quint16)
        header >> machtype;     // Device Type (quint16)
        header >> datasize;     // Size of Uncompressed Data (quint32)
        header >> crc16;        // CRC16 of Uncompressed Data (quint16)
    }

    QByteArray databytes, temp = file.readAll();
    file.close();

    if (version >= 10) {
        if (compmethod > 0) {
            databytes = qUncompress(temp);

            if (!s_evchecksum_checked) {
                if (databytes.size() != datasize) {
                    qDebug() << "File" << filename << "has returned wrong datasize";
                    return false;
                }

                quint16 crc = 0;
                #if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
                    crc = qChecksum(databytes.data(), databytes.size());
                #else
                    crc = qChecksum(QByteArrayView(databytes));
                #endif

                if (crc != crc16) {
                    qDebug() << "CRC Doesn't match in" << filename;
                    return false;
                }

                s_evchecksum_checked = true;
            }
        } else {
            databytes = temp;
        }
    } else { 
        databytes = temp; 
    }

    QDataStream in(databytes);
    in.setVersion(QDataStream::Qt_4_6);
    in.setByteOrder(QDataStream::LittleEndian);

    qint16 mcsize;
    in >> mcsize;   // number of Device Code lists

    ChannelID code;
    qint64 ts1, ts2;
    qint32 evcount;
    EventListType elt;
    EventDataType rate, gain, offset, mn, mx;
    qint16 size2;
    QVector<ChannelID> mcorder;
    QVector<qint16> sizevec;
    QString dim;

    for (int i = 0; i < mcsize; i++) {
        if (version < 8) {
            QString txt;
            in >> txt;
            code = schema::channel[txt].id();
        } else {
            in >> code;
        }

        mcorder.push_back(code);
        in >> size2;
        sizevec.push_back(size2);

        for (int j = 0; j < size2; j++) {
            in >> ts1;
            in >> ts2;
            in >> evcount;
            in >> t8;
            elt = (EventListType)t8;
            in >> rate;
            in >> gain;
            in >> offset;
            in >> mn;
            in >> mx;
            in >> dim;
            bool second_field = false;

            if (version >= 7) { // version 7 added this field
                in >> second_field;
            }

            EventList *elist = AddEventList(code, elt, gain, offset, mn, mx, rate, second_field);
            elist->setDimension(dim);

            elist->setCount(evcount);
            elist->setFirst(ts1);
            elist->setLast(ts2);

            if (second_field) {
                EventDataType min, max;
                in >> min;
                in >> max;
                elist->setMin2(min);
                elist->setMax2(max);
            }
        }
    }

    for (int i = 0; i < mcsize; i++) {
        code = mcorder[i];
        size2 = sizevec[i];

        for (int j = 0; j < size2; j++) {
            EventList &evec = *eventlist[code][j];
            evec.getData().resize(evec.count());
            EventStoreType *ptr = evec.getData().data();

            in.readRawData((char *)ptr, evec.count() << 1);

            if (evec.hasSecondField()) {
                evec.getData2().resize(evec.count());
                ptr = evec.getData2().data();

                in.readRawData((char *)ptr, evec.count() << 1);
            }

            if (evec.type() != EVL_Waveform) {
                evec.getTime().resize(evec.count());
                quint32 *tptr = evec.getTime().data();

                in.readRawData((char *)tptr, evec.count() << 2);
            }
        }
    }

    s_events_loaded = true;
    
//    qDebug() << "Session::LoadEventsFromFile() - Successfully loaded events for session" << s_session
//             << "from" << filename;
    
    return true;
}

// ===== END IMPORT-SPECIFIC FILE LOADERS =====
