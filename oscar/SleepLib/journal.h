/* SleepLib Journal Implementation
 *
 * Copyright (c) 2019-2026 The OSCAR Team
 * Copyright (C) 2011-2018 Mark Watkins
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */


#ifndef JOURNAL_H
#define JOURNAL_H

class Session;
#include <QString>
#include <QDate>
#include <QDomDocument>
#include "daily.h"

#include "SleepLib/profiles.h"

Session *GetOrCreateJournalSession(QDate date);

class Journal {
public:
    enum { JRNL_Zombie = 1 , JRNL_Weight = 2 , JRNL_Notes = 4 , JRNL_Bookmarks = 8 };
    static bool BackupJournal(QString filename);
    static bool RestoreJournal(QString filename);
    
    /**
     * @brief Migrates all journal .000 files to database for a profile
     * @param profile The profile to migrate
     * @return true if successful, false otherwise
     * @note Journal machine must already be in database (saved by Profile::LoadMachineData())
     */
    static bool MigrateToDatabase(Profile* profile);
    
    /**
     * @brief Checks if journal data needs to be migrated from .000 files to database
     * @param profile The profile to check
     * @return true if migration is needed (has .000 files but no database sessions)
     */
    static bool NeedsMigration(Profile* profile);
    
private:
    constexpr static double zeroD = 0.0001;
    static void getJournal(Daily*& daily,QDate& date,Session* & journal);
    static bool RestoreDay (QDomElement& dayElement,QDate& date,QString& filename) ;
};

#endif // JOURNAL_H
