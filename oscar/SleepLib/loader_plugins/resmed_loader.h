/* SleepLib RESMED Loader Header
 *
 * Copyright (c) 2019-2026 The OSCAR Team
 * Copyright (C) 2011-2018 Mark Watkins 
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef RESMED_LOADER_H
#define RESMED_LOADER_H

#include <QVector>
#include "SleepLib/machine.h" // Base class: MachineLoader
#include "SleepLib/machine_loader.h"
#include "SleepLib/profiles.h"
#include "SleepLib/loader_plugins/edfparser.h"
#include "SleepLib/loader_plugins/resmed_EDFinfo.h"

//********************************************************************************************
/// IMPORTANT!!!
//********************************************************************************************
// Please INCREMENT the following value when making changes to this loaders implementation.
//
const int resmed_data_version = 15;
//
//********************************************************************************************


class ResmedLoader;

class ResMedDay {
public:
    ResMedDay( QDate d) : date(d) {}

    QDate date;
    STRRecord str;
    QHash<QString, QString> files;  // key is filename, value is fullpath
};

typedef void (*ResDaySaveCallback)(ResmedLoader* loader, Session* session);

class ResDayTask:public ImportTask
{
public:
    ResDayTask(ResmedLoader * l, Machine * m, ResMedDay * d, ResDaySaveCallback s): reimporting(false), loader(l), mach(m), resday(d), save(s) {}
    virtual ~ResDayTask() {}
    virtual void run();

    bool reimporting;

protected:
    ResmedLoader * loader;
    Machine * mach;
    ResMedDay * resday;
    ResDaySaveCallback save;
};

/*! \class ResmedLoader
    \brief Importer for ResMed S9 Data
    */
class ResmedLoader : public CPAPLoader
{
    Q_OBJECT
    friend class ResmedImport;
    friend class ResmedImportStage2;
  public:
    ResmedLoader();
    virtual ~ResmedLoader();

    //! \brief Register the ResmedLoader with the list of other device loaders
    static void Register();

    //! \brief Detect if the given path contains a valid Folder structure
    virtual bool Detect(const QString & path);

    //! \brief Look up device model information of ResMed file structure stored at path
    virtual MachineInfo PeekInfo(const QString & path);

    virtual void checkSummaryDay( ResMedDay & resday, QDate date, Machine * mach );

    //! \brief Scans for ResMed SD folder structure signature, and loads any new data if found
    virtual int Open(const QString &);

    //! \brief Returns the version number of this ResMed loader
    virtual int Version() { return resmed_data_version; }

    //! \brief Returns the device class name of this loader. ("ResMed")
    virtual const QString &loaderName() { return resmed_class_name; }

    virtual MachineInfo newInfo() {
        return MachineInfo(MT_CPAP, 0, resmed_class_name, QObject::tr("ResMed"), QString(), 
        QString(), QString(), QObject::tr("S9"), QDateTime::currentDateTime(), resmed_data_version);
    }

    virtual void initChannels();

    //! \brief Converts EDFSignal data to time delta packed EventList, and adds to Session
    void ToTimeDelta(Session *sess, ResMedEDFInfo &edf, EDFSignal &es, ChannelID code, long recs,
                     qint64 duration, EventDataType min = 0, EventDataType max = 0, bool square = false);

    //! \brief Parse the EVE Event annotation data, and save to Session * sess
    //! This contains all Hypopnea, Obstructive Apnea, Central and Apnea codes
    bool LoadEVE(Session *sess, const QString & path);

    //! \brief Parse the CSL Event annotation data, and save to Session * sess
    //! This contains Cheyne Stokes Respiration flagging on the AirSense 10
    bool LoadCSL(Session *sess, const QString & path);

    //! \brief Parse the BRP High Resolution data, and save to Session * sess
    //! This contains Flow Rate, Mask Pressure, and Resp. Event  data
    bool LoadBRP(Session *sess, const QString & path);

    //! \brief Parse the SAD Pulse oximetry attachment data, and save to Session * sess
    //! This contains Pulse Rate and SpO2 Oxygen saturation data
    bool LoadSAD(Session *sess, const QString & path);

    //! \brief Parse the PRD low resolution data, and save to Session * sess
    //! This contains the Pressure, Leak, Respiratory Rate, Minute Ventilation, Tidal Volume, etc..
    bool LoadPLD(Session *sess, const QString & path);

    //! \brief Validate edf.startdate against sess->session() (which is the STR.edf
    //! mask-on time) and repair it if the EDF header ASCII datetime field is
    //! corrupt. Also recomputes edf.enddate from the still-valid record count and
    //! duration fields. Call immediately after edf.Parse().
    //! \return true if startdate was valid or was successfully repaired.
    bool repairEDFStartFromSession(ResMedEDFInfo &edf, Session *sess,
                                   const QString &path);


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Now for some CPAPLoader overrides
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////
    virtual QString PresReliefLabel() { return QObject::tr("EPR: "); }

    virtual ChannelID PresReliefMode() ;
    virtual ChannelID PresReliefLevel() ;
    virtual ChannelID CPAPModeChannel() ;

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //! \brief Number of sessions saved during the current import.
    //! Reset in Open() and incremented by SaveSession() under sessionMutex.
    int sessionCount;
    static void SaveSession(ResmedLoader* loader, Session* session);
    ResDaySaveCallback saveCallback;
    int OpenWithCallback(const QString & dirpath, ResDaySaveCallback s);

    void LogUnexpectedMessage(const QString & message);

protected:
//! \brief The STR.edf file is a unique edf file with many signals
    bool ProcessSTRfiles(Machine *, QMap<QDate, STRFile> &, QDate);

    //! \brief Scan for new files to import, group into sessions and add to task que
    int ScanFiles(Machine * mach, const QString & datalog_path, QDate firstImport);

//! \brief Write a backup copy to the backup path
    QString Backup(const QString & file, const QString & backup_path);

// The data members
//    QMap<SessionID, QStringList> sessfiles;
//    QMap<quint32, STRRecord> strsess;
//    QMap<QDate, QList<STRRecord *> > strdate;

    QMap<QDate, ResMedDay> resdayList;

#ifdef DEBUG_EFFICIENCY
    QHash<ChannelID, qint64> channel_efficiency;
    QHash<ChannelID, qint64> channel_time;
    // Cumulative profiling times in ms. Worker threads update them under timeMutex.
    qint64 timeInLoadBRP;       //!< Time spent in LoadBRP().
    qint64 timeInLoadPLD;       //!< Time spent in LoadPLD().
    qint64 timeInLoadEVE;       //!< Time spent in LoadEVE().
    qint64 timeInLoadCSL;       //!< Time spent in LoadCSL().
    qint64 timeInLoadSAD;       //!< Time spent in LoadSAD().
    qint64 timeInEDFOpen;       //!< Time spent opening EDF files.
    qint64 timeInEDFInfo;       //!< Time spent parsing EDF headers.
    qint64 timeInAddWaveform;   //!< Time spent in AddWaveform() for BRP data.
    qint64 timeInTimeDelta;     //!< Time spent computing time deltas.
    QMutex timeMutex;
#endif

    // TODO: This really belongs in a generic location that all loaders can use.
    // But that will require retooling the overall call structure so that there's
    // a top-level import job that's managing a specific import. Right now it's
    // essentially managed by the importCPAP method rather than an object instance
    // with state.
    QMutex m_importMutex;
    QSet<QString> m_unexpectedMessages;

};

#endif // RESMED_LOADER_H
