/* Apex Medical XT Auto Loader Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef APEX_LOADER_H
#define APEX_LOADER_H

#include "SleepLib/machine.h"
#include "SleepLib/machine_loader.h"
#include "SleepLib/loader_plugins/apexDataParsing.h"

#ifdef UNITTEST_MODE
class ApexTests;
#endif

//! Bump when a format change requires reimporting existing Apex data.
const int apex_data_version = 1;

const QString apex_class_name = "ApexLoader";

/*! \class ApexLoader
    \brief Imports Apex Medical XT Auto APAPDATA cards. */
class ApexLoader : public CPAPLoader
{
    Q_OBJECT

#ifdef UNITTEST_MODE
    friend class ApexTests;
#endif
  public:
    ApexLoader();
    virtual ~ApexLoader();

    static void Register();

    virtual bool Detect(const QString &path) override;
    virtual int Open(const QString &path) override;
    virtual int Version() override { return apex_data_version; }
    virtual const QString &loaderName() override { return apex_class_name; }
    virtual MachineInfo PeekInfo(const QString &path) override;

    virtual MachineInfo newInfo() override;

    //! Copy the raw card files to the machine's canonical backup directory.
    bool backupData(Machine *mach, const QString &path);

  protected:
    bool rebuild_from_backups = false;
    bool create_backups = true;

    /*! \brief Locate the directory containing 00000000.APF.

        The optional 00000000.APE minute-detail file is not required to be
        present, and is not looked for here.
        Accepts the card root, APAPDATA directory, or 00000000 directory. */
    QString findDataDir(const QString &path);

    //! Validate the Apex signature in an already resolved data directory.
    static bool validateDataDir(const QString &dataPath, QByteArray *apfData = nullptr);

    //! Back up files from an already resolved data directory.
    bool backupDataDir(Machine *mach, const QString &dataPath);

    //! Map the device's APAP/fixed-pressure settings to OSCAR channels.
    static void importSettings(Session *session, const ApexParsing::ApfRecord &rec);

    //! Import the session-level channels available without .APE detail.
    static void importSessionAverages(Session *session,
                                      const ApexParsing::ApfRecord &rec);

    //! Import the session-average total-leak trace.
    static void importLeakChannel(Session *session,
                                  const ApexParsing::ApfRecord &rec,
                                  qint64 startMs, qint64 endMs);

    //! Import one-minute pressure samples and respiratory-event counts.
    static void importMinuteDetail(
        Session *session, const ApexParsing::ApfRecord &rec,
        const QVector<ApexParsing::ApeMinuteRecord> &minutes, qint64 startMs);
};

#endif // APEX_LOADER_H
