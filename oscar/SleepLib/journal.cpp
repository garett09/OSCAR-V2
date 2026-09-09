/* SleepLib Journal Implementation
 *
 * Copyright (c) 2019-2026 The OSCAR Team
 * Copyright (c) 2011-2018 Mark Watkins
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#define TEST_MACROS_ENABLEDoff
#include <test_macros.h>

#include <QMessageBox>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QTextStream>
#include <QXmlStreamWriter>
#include <QDir>
#include "journal.h"
#include "daily.h"
#include "SleepLib/common.h"
#include "SleepLib/machine_common.h"
#include "SleepLib/session.h"
#include "SleepLib/profiles.h"
#include "mainwindow.h"
#include "../database/session_repository.h"
#include "../database/session_settings_repository.h"

extern MainWindow * mainwin;
const QString OLD_ZOMBIE = QString("zombie");
const QString ZOMBIE = QString("Feelings");
const QString WEIGHT = QString("weight");

Session *GetOrCreateJournalSession(QDate date)
{
    Day *day = p_profile->GetDay(date, MT_JOURNAL);
    if (day) {
        if (Session *session = day->firstSession(MT_JOURNAL)) {
            return session;
        }
    }

    Machine *machine = p_profile->GetMachine(MT_JOURNAL);
    if (!machine) {
        machine = new Machine(p_profile, 0);
        MachineInfo info;
        info.loadername = "Journal";
        info.serial = machine->hexid();
        info.brand = "Journal";
        info.type = MT_JOURNAL;
        machine->setInfo(info);
        machine->setType(MT_JOURNAL);
        p_profile->AddMachine(machine);
        if (!machine->SaveToDatabase()) {
            qWarning() << "GetOrCreateJournalSession: could not save journal machine";
        }
    }

    qint64 startMs;
    qint64 endMs;
    Day *currentDay = p_profile->GetDay(date);
    if (currentDay && currentDay->first() > 0) {
        startMs = currentDay->first();
        endMs = currentDay->last();
    } else {
        startMs = qint64(QDateTime(date, QTime(20, 0)).toSecsSinceEpoch()) * 1000L;
        // AddSession keeps sessions shorter than the ignore threshold (up to 90 min) out
        // of the Day, which would hide the journal data. Span the fallback past the threshold.
        const qint64 minSpanMs =
            (qint64(p_profile->session->ignoreShortSessions()) + 1) * 60000L;
        endMs = startMs + qMax<qint64>(3600000L, minSpanMs);
    }
    // A same-ID session can sit in the sessionlist without being found through the Day
    // lookup above (legacy short sessions, day-split edges); reuse it rather than collide.
    if (Session *existing = machine->SessionExists(startMs / 1000L)) {
        return existing;
    }

    Session *session = new Session(machine, 0);
    session->SetSessionID(startMs / 1000L);
    session->set_first(startMs);
    session->set_last(endMs);
    if (!machine->AddSession(session, true)) {
        delete session;
        return nullptr;
    }
    return session;
}

bool Journal::BackupJournal(QString filename)
{
    QString outBuf;
    QXmlStreamWriter stream(&outBuf);
    stream.setAutoFormatting(true);
    stream.setAutoFormattingIndent(2);

    stream.writeStartDocument();
//    stream.writeProcessingInstruction("xml", "version=\"1.0\" encoding=\"UTF-8\"");
    stream.writeStartElement("OSCAR");
    stream.writeAttribute("created", QDateTime::currentDateTime().toString(Qt::ISODate));

    stream.writeStartElement("Journal");
    stream.writeAttribute("username", p_profile->user->userName());
    stream.writeAttribute("height_cm", QString::number(p_profile->user->height()));
    if (p_profile->general->unitSystem()!=US_Undefined) {
        stream.writeAttribute("systemUnits", ( p_profile->general->unitSystem()==US_Metric?"Metric":"Englsh") );
    }

    QDate first = p_profile->FirstDay(MT_JOURNAL);
    QDate last = p_profile->LastDay(MT_JOURNAL);
    DEBUGFC Q(first) Q(last);


    QDate date = first.addDays(-1);
    int days_saved = 0 ;
    do {
        date = date.addDays(1);

        Day * journal = p_profile->GetDay(date, MT_JOURNAL);
        if (!journal) continue;

        Session * sess = journal->firstSession(MT_JOURNAL);
        if (!sess) continue;

        if (   !journal->settingExists(Journal_Notes)
            && !journal->settingExists(Journal_Weight)
            && !journal->settingExists(Journal_ZombieMeter)
            && !journal->settingExists(LastUpdated)
            && !journal->settingExists(Bookmark_Start)) {
            continue;
        }
        QString weight;
        QString zombie;
        QString notes;
        QString lastupdated;
        QVariantList start;
        int havedata=0;


        if (journal->settingExists(Journal_Weight)) {
            weight = sess->settings[Journal_Weight].toString();
            havedata |= JRNL_Weight ;
        }

        if (journal->settingExists(Journal_ZombieMeter)) {
            zombie = sess->settings[Journal_ZombieMeter].toString();
            havedata |= JRNL_Zombie ;
        }

        if (journal->settingExists(Journal_Notes)) {
            notes = sess->settings[Journal_Notes].toString();
            //notes = Daily::convertHtmlToPlainText(notes).trimmed();
            havedata |= JRNL_Notes ;
        }

        if (journal->settingExists(Bookmark_Start)) {
            start=sess->settings[Bookmark_Start].toList();
            if (start.size()>0) havedata |= JRNL_Bookmarks ;
        }

        if (journal->settingExists(LastUpdated)) {
            QDateTime dt = sess->settings[LastUpdated].toDateTime();
#if QT_VERSION < QT_VERSION_CHECK(5,8,0)
            qint64 dtx = dt.toMSecsSinceEpoch()/1000L;
#else
            qint64 dtx = dt.toSecsSinceEpoch();
#endif
            lastupdated = QString::number(dtx);
        }

        if (!havedata) {
            // No data to archive.
            continue ;
        }
        //QString dateStr = ((++count & 1)==0) ? date.toString(/*Qt::ISODate*/) : date.toString(Qt::ISODate) ;
        QString dateStr =date.toString(Qt::ISODate) ;
        stream.writeStartElement("day");
            stream.writeAttribute("date", dateStr);
                if(!weight.isEmpty()) stream.writeAttribute(WEIGHT, weight);
                if(!zombie.isEmpty()) stream.writeAttribute(ZOMBIE, zombie);
                stream.writeAttribute("lastupdated", lastupdated);

            if (!notes.isEmpty() ) {
                stream.writeStartElement("note");
                    stream.writeTextElement("text", notes);
                stream.writeEndElement(); // notes
            }

            if (start.size()>0) {
                QVariantList end=sess->settings[Bookmark_End].toList();
                QStringList notes=sess->settings[Bookmark_Notes].toStringList();
                stream.writeStartElement("bookmarks");
                int size = start.size();
                for (int i=0; i< size; i++) {
                    stream.writeStartElement("bookmark");
                    stream.writeAttribute("notes",notes.at(i));
                    stream.writeAttribute("start",start.at(i).toString());
                    stream.writeAttribute("end",end.at(i).toString());
                    stream.writeEndElement(); // bookmark
                }
                stream.writeEndElement(); // bookmarks
            }
        days_saved++;
        stream.writeEndElement(); // day
    } while (date <= last);
    // //stream.writeAttribute("DaysSaved", QString::number(days_saved));

    stream.writeEndElement(); // Journal
    stream.writeEndElement(); // OSCAR
    stream.writeEndDocument();

    QFile file(filename);

    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Couldn't open journal file" << filename << "error code" << file.error() << file.errorString();
        return false;
    }

    QTextStream ts(&file);
    #if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        ts.setCodec("UTF-8");
    #else
        // Qt6 version utf-8 is the default
        // or if not utf-8
        // ts.setEncoding(QStringConverter::Utf8);
    #endif
    ts.setGenerateByteOrderMark(true);
    ts << outBuf;
    file.close();
    QMessageBox::information(nullptr, STR_MessageBox_Information,
        QString(QObject::tr("%1 days Journal Data was saved in file %2")).arg(days_saved).arg(filename) ,
        QMessageBox::Ok);

    return true;
}

void Journal::getJournal(Daily*& daily,QDate& date,Session* & journal) {
    if (journal) return;
    journal = daily->GetJournalSession(date,true);
    if (journal) return;
    journal = daily->CreateJournalSession(date);
}


bool Journal::RestoreDay (QDomElement& dayElement,QDate& date,QString& filename) {
    Daily* daily = mainwin->getDaily();
    if (!daily) return false;
    Session* journal = nullptr;
    bool changed = false;

    // handle zombie - feelings
    bool ok = false;
    int zombie = 0;
    zombie =  (dayElement.attribute(ZOMBIE)).toInt(&ok);
    if (!ok) { zombie=0; }
    if (zombie == 0 ) {
        zombie =  (dayElement.attribute(OLD_ZOMBIE)).toInt(&ok);
        if (!ok) { zombie=0; }
    }
    DEBUGFC O("\n\n") ;
    DEBUGFC O(date) O(filename); Q_UNUSED(filename);
    if (zombie>0) {
        int jvalue =  0 ;
        getJournal(daily,date,journal);
        if (journal == nullptr) { qWarning() << "RestoreDay: no journal session for" << date; return changed; }
        if (journal->settings.contains(Journal_ZombieMeter)) {
            jvalue = journal->settings[Journal_ZombieMeter].toInt();
        }
        if (jvalue == 0)  {
            DEBUGFC O(date) Q(zombie);
            daily->set_JournalZombie(date,zombie);
            if (date == daily->getDate()) daily->set_ZombieUI(zombie);
            changed = true;
        }
    }

    // handle Weight
    double weight =  (dayElement.attribute(WEIGHT)).toDouble(&ok);
    if (weight>zeroD) {
        getJournal(daily,date,journal);
        if (journal == nullptr) { qWarning() << "RestoreDay: no journal session for" << date; return changed; }
        double jvalue = 0.0 ;
        if (journal->settings.contains(Journal_Weight)) {
            jvalue = journal->settings[Journal_Weight].toDouble();
        }
        if (jvalue < zeroD )  {
            DEBUGFC O(date) Q(weight);
            daily->set_JournalWeightValue(date,weight);
            if (date == daily->getDate()) daily->set_WeightUI(weight);
            changed = true;
        }
    }

    // Handle Notes.
    QDomElement noteText = dayElement.elementsByTagName("note").at(0).toElement().elementsByTagName("text").at(0).toElement();
    if (!noteText.text().isEmpty() ) {
        getJournal(daily,date,journal);
        if (journal == nullptr) { qWarning() << "RestoreDay: no journal session for" << date; return changed; }
        // there are characters in notes. maybe just spaces. Ignore spaces.
        QString plainTextToAdd = Daily::convertHtmlToPlainText(noteText.text());

        // get existing note if any.
        QString currNoteHtml = journal->settings[Journal_Notes].toString();
        QString currNotePlainText = Daily::convertHtmlToPlainText(currNoteHtml);
        if (currNotePlainText.contains(plainTextToAdd) ) {
            // plainText to add is equal to curr or is it a subset of the current Note
            // use the current notes.. ignore text in backup.
        } else {
            // plainText is not equal to curr nor is it a subset of the current Note
            if (plainTextToAdd.contains(currNotePlainText) ) {
                //curr note is a subset of the new - so use new.
                currNoteHtml = noteText.text();
            } else {
                // ToAdd text and Current text are different.
                // use previous verson append with  current 
                currNoteHtml.prepend(noteText.text());
            }
            daily->set_JournalNotesHtml(date,currNoteHtml);
            if (date == daily->getDate()) daily->set_NotesUI(noteText.text());
            DEBUGFC O(date) Q((void*)journal) O( )Daily::convertHtmlToPlainText(currNoteHtml);
            changed = true;
        }
    }

    QDomNodeList bookmarks = dayElement.elementsByTagName("bookmarks").at(0).toElement().elementsByTagName("bookmark");
    if (bookmarks.size()>0) {
        DEBUGFC Q(bookmarks.size());
        getJournal(daily,date,journal);
        if (journal == nullptr) { qWarning() << "RestoreDay: no journal session for" << date; return changed; }
        // get list of bookmarks for journal. These will not be removed.
        QVariantList start;
        QVariantList end;
        QStringList notes;
        if (journal->settings.contains(Bookmark_Start)) {
            //DEBUGFC;
            start=journal->settings[Bookmark_Start].toList();
            end=journal->settings[Bookmark_End].toList();
            notes=journal->settings[Bookmark_Notes].toStringList();
        }
        // check if arcived bookmark is current list
        bool bmChanged = false;
        for (int idx=0 ; idx < bookmarks.size() ; idx++) {
            // get archived bookmark
            //DEBUGFC Q(idx) Q(bookmarks.size()) Q(start.size()) ;
            QDomElement bookmark = bookmarks.at(idx).toElement();
            qint64 archiveStart = bookmark.attribute("start").toLongLong();
            qint64 archiveEnd   = bookmark.attribute("end").toLongLong();
            QString archiveNote = bookmark.attribute("notes");
            // check if bookmark already exists.

            bool duplicate = false;
            bool bNoteChanged = false;
            for (int idy=0 ; idy < start.size() ; idy++) {
                //DEBUGFC Q(idy);
                qint64 bmStart = start.at(idy).toLongLong();
                qint64 bmEnd   = end.at(idy).toLongLong();
                if ( (bmStart == archiveStart) && (bmEnd == archiveEnd) ) {
                    duplicate = true;
                    //DEBUGFC Q(idx) Q(idy);
                    // have same bookmark - new check if notes need merging
                    QString aNote = archiveNote.simplified();
                    QString bmNote = notes.at(idy);
                    QString bNote = bmNote.simplified();
                    DEBUGFC  Q(aNote) Q(bNote) Q(bNote.contains(aNote)) Q(aNote.contains(bNote));
                    if (bNote.contains(aNote) ) {
                        // no action needed.
                        //DEBUGFC Q(idx) Q(idy) Q(bNote);;
                        break;
                    } else {
                        // updated existing bookmark label
                        //DEBUGFC Q(idx) Q(idy);
                        // if an append is needed.
                        if (aNote.contains(bNote)) {
                            // use archived note
                            //DEBUGFC Q(idx) Q(idy);
                        } else {
                            //DEBUGFC Q(idx) Q(idy);
                            // prepend  archive note to bmNote
                            archiveNote.append(" :: ").append(bmNote);
                        }
                        // use archiveNote.
                        notes[idy]= archiveNote;
                        bmChanged = true;
                        bNoteChanged = true;
                    }
                }
            }
            if (!duplicate) {
                DEBUGFC Q(date) Q(idx) Q(archiveNote);
                // here if need to add archive bookmark.
                // add archive bookmark to currrent list
                start.push_back(archiveStart);
                end.push_back(archiveEnd);
                notes.push_back(archiveNote);
                bmChanged = true;
            } else if (bNoteChanged) {
                DEBUGFC Q(date) Q(idx) Q(archiveNote);
            }
        }
       if (bmChanged) {
            //DEBUGFC Q(bmChanged);
            getJournal(daily,date,journal);
        if (journal == nullptr) { qWarning() << "RestoreDay: no journal session for" << date; return changed; }
            journal->settings[Bookmark_Start]=start;
            journal->settings[Bookmark_End]=end;
            journal->settings[Bookmark_Notes]=notes;
            if (date == daily->getDate()) daily->set_BookmarksUI(start,end,notes,0);
        } else {
            DEBUGFC O("bookmark ignored") ;
        }

    }

    if (!journal) {
        return true;
    }
    if (!journal->machine()) {
        return true;
    }
    if (changed) {
        journal->SetChanged(true);
        journal->settings[LastUpdated] = QDateTime::currentDateTime();
        journal->machine()->SaveSummaryCache();
        journal->SetChanged(false);
    }
    DEBUGFC O(date) O("\n\n");
    return true;
}


bool Journal::RestoreJournal(QString filename)
{
    QFile file(filename);
    if (!file.open(QFile::ReadOnly)) {
        qWarning() << "Could not open" << filename.toLocal8Bit().data() << "for reading, error code" << file.error() << file.errorString();
        return false;
    }

    QDomDocument doc("machines.xml");

    if (!doc.setContent(&file)) {
        qWarning() << "Invalid XML Content in" << filename.toLocal8Bit().data();
        return false;
    }
    file.close();

    QDomElement root = doc.documentElement(); // Get the root element of the document
    if (root.isNull()) {
        return false;
    }
    QDomNodeList days = doc.elementsByTagName("day");
    if (days.isEmpty()) {
        return false;
    }

    QDate first = p_profile->FirstDay();
    QDate last = p_profile->LastDay();
    QDate current = QDate::currentDate();
    //int used=0;
    for (int idx=0 ; idx < days.size() ; idx++) {
        QDomElement dayElement = days.at(idx).toElement();

        QString dateStr = dayElement.attribute("date") ;
        QDate   date    = QDate::fromString(dateStr,Qt::ISODate) ;
        if (!date.isValid()) {
            QDate newdate   = QDate::fromString(dateStr) ;  // read original date format 
            date = newdate;
        }
        if (!date.isValid()) {
            continue;
        }
        if (date < first) {
            continue;
        }
        if (date > last) {
            continue;
        }
        if (date > current) {
            continue;
        }
        if ( RestoreDay(dayElement,date,filename) ) {
           //used++;
        }
    }
    double user_height_cm  = p_profile->user->height();
    if (user_height_cm<zeroD) {
        QDomElement journal = root.elementsByTagName("Journal").at(0).toElement();
        double height_cm = journal.attribute("height_cm").toDouble();
        DEBUGFC  Q(journal.tagName()) Q(height_cm);
        if (height_cm>=zeroD) p_profile->user->setHeight(height_cm);
    }
    return true;
}

bool Journal::NeedsMigration(Profile* profile)
{
    qDebug() << "Journal::NeedsMigration() - Start";
    
    if (!profile) {
        qDebug() << "Journal::NeedsMigration() - NULL profile";
        return false;
    }
    
    // Check if journal machine exists
    Machine* journalMachine = profile->GetMachine(MT_JOURNAL);
    if (!journalMachine) {
        qDebug() << "Journal::NeedsMigration() - No journal machine";
        return false;  // No journal machine, no migration needed
    }
    
    // Check if machine is in database
    if (journalMachine->getDatabaseId() == 0) {
        qDebug() << "Journal::NeedsMigration() - Machine not in database yet";
        return false;  // Machine not in database yet, can't migrate
    }
    
    // IMPORTANT: Check for .000 files FIRST before accessing database
    // This prevents unnecessary database access if there's nothing to migrate
    QString summariesPath = journalMachine->getSummariesPath();
    QDir dir(summariesPath);
    
    if (!dir.exists()) {
        qDebug() << "Journal::NeedsMigration() - No summaries directory";
        return false;  // No summaries directory
    }
    
    QStringList filters;
    filters << "*.000";
    dir.setNameFilters(filters);
    QStringList files = dir.entryList(QDir::Files);
    
    if (files.isEmpty()) {
        qDebug() << "Journal::NeedsMigration() - No .000 files found";
        return false;  // No .000 files to migrate
    }
    
    qDebug() << "Journal::NeedsMigration() - Found" << files.size() << ".000 files, checking if already migrated";
    
    // Now check database to see if already migrated
    // IMPORTANT: Only do this if we found .000 files
    try {
        SessionRepository sessionRepo;
        QList<SessionData> sessions = sessionRepo.findByMachine(journalMachine->getDatabaseId());
        
        if (!sessions.isEmpty()) {
            qDebug() << "Journal::NeedsMigration() - Found" << sessions.size() << "sessions in database, already migrated";
            return false;  // Already have sessions in database, no migration needed
        }
    } catch (...) {
        qWarning() << "Journal::NeedsMigration() - Exception while checking database, will attempt migration anyway";
        // If we can't check database but have .000 files, try to migrate
    }
    
    qDebug() << "Journal::NeedsMigration() - Migration needed";
    return true;  // Have .000 files but no database sessions, migration needed
}

bool Journal::MigrateToDatabase(Profile* profile)
{
    if (!profile) {
        qWarning() << "Journal::MigrateToDatabase() - NULL profile";
        return false;
    }
    
    qDebug() << "Journal::MigrateToDatabase() - Starting migration for" << profile->user->userName();
    
    // 1. Get journal machine
    Machine* journalMachine = profile->GetMachine(MT_JOURNAL);
    if (!journalMachine) {
        qWarning() << "Journal::MigrateToDatabase() - No journal machine found";
        return false;
    }
    
    // 2. Verify machine is in database
    qint64 machineDbId = journalMachine->getDatabaseId();
    if (machineDbId == 0) {
        qWarning() << "Journal::MigrateToDatabase() - Journal machine not in database";
        return false;
    }
    
    qDebug() << "Journal::MigrateToDatabase() - Journal machine database ID:" << machineDbId;
    
    // 3. Find all .000 files in Summaries directory
    QString summariesPath = journalMachine->getSummariesPath();
    QDir dir(summariesPath);
    
    if (!dir.exists()) {
        qWarning() << "Journal::MigrateToDatabase() - Summaries directory does not exist:" << summariesPath;
        return false;
    }
    
    QStringList filters;
    filters << "*.000";
    dir.setNameFilters(filters);
    QStringList files = dir.entryList(QDir::Files);
    
    qDebug() << "Journal::MigrateToDatabase() - Found" << files.size() << ".000 files to migrate";
    
    if (files.isEmpty()) {
        qDebug() << "Journal::MigrateToDatabase() - No .000 files to migrate";
        return true;  // No files to migrate is success
    }
    
    // 4. Migrate each file
    int migratedCount = 0;
    int errorCount = 0;
    
    for (const QString& filename : files) {
        // Parse date from filename (format: XXXXXXXX.000 where X is hex SessionID)
        QString baseName = filename.section(".", 0, -2);  // Remove .000 extension
        bool ok;
        SessionID sessionId = baseName.toLongLong(&ok, 16);
        
        if (!ok) {
            qWarning() << "Journal::MigrateToDatabase() - Invalid filename format:" << filename;
            errorCount++;
            continue;
        }
        
        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(sessionId);
        QDate date = dateTime.date();
        if (!date.isValid()) {
            qWarning() << "Journal::MigrateToDatabase() - Invalid date for session:" << sessionId;
            errorCount++;
            continue;
        }
        
        // Load the .000 file as a session
        QString fullPath = summariesPath + "/" + filename;
        
        // Create session and set it to load from file (not database)
        Session* sess = new Session(journalMachine, sessionId);
        sess->setSessionRowId(machineDbId);
        
        // IMPORTANT: Load from the .000 FILE on disk, not from database
        // Read the summary file format to extract settings
        QFile file(fullPath);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "Journal::MigrateToDatabase() - Failed to open file" << fullPath;
            delete sess;
            errorCount++;
            continue;
        }
        
        QDataStream in(&file);
        in.setByteOrder(QDataStream::LittleEndian);
        in.setVersion(QDataStream::Qt_4_6);
        
        // Read the .000 file header (same format as Session::LoadSummary)
        quint32 mag, machId, sessId;
        quint16 version, fileType;
        qint64 first, last;
        
        in >> mag;          // magic number
        in >> version;      // DB version
        in >> fileType;     // file type
        in >> machId;       // machine ID
        in >> sessId;       // session ID
        in >> first;        // start time
        in >> last;         // duration
        
        // Now read the settings hash
        QHash<ChannelID, QVariant> settings;
        in >> settings;
        
        file.close();
        
        // Set the session data using public methods
        sess->set_first(first);
        sess->set_last(last);
        sess->settings = settings;
        
        // Store to database with JSON serialization for bookmarks
        if (!sess->StoreToDatabase()) {
            qWarning() << "Journal::MigrateToDatabase() - Failed to store to database for" << date.toString();
            delete sess;
            errorCount++;
            continue;
        }
        
        qDebug() << "Journal::MigrateToDatabase() - Migrated" << date.toString() << "to database";
        migratedCount++;
        
        delete sess;
    }
    
    qDebug() << "Journal::MigrateToDatabase() - Migration complete:"
             << migratedCount << "sessions migrated,"
             << errorCount << "errors";
    
    // Show result to user
    if (migratedCount > 0) {
        QString message = QObject::tr("Journal Migration Complete:\n"
                                    "%1 journal entries migrated to database\n"
                                    "%2 errors").arg(migratedCount).arg(errorCount);
        
        if (errorCount > 0) {
            QMessageBox::warning(nullptr, STR_MessageBox_Warning, message, QMessageBox::Ok);
        } else {
            QMessageBox::information(nullptr, STR_MessageBox_Information, message, QMessageBox::Ok);
        }
    }
    
    return (errorCount == 0);
}
