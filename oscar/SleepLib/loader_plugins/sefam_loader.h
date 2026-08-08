/* SEFAM Loader Header
 *
 * Imports SEFAM CPAP SD cards. The card layout is
 * <modelcode>/<serial>/DATA_nnn/, one directory per session.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef SEFAM_LOADER_H
#define SEFAM_LOADER_H

#include "SleepLib/machine.h"
#include "SleepLib/machine_loader.h"

//! Bump when a format change requires reimporting existing SEFAM data.
const int sefam_data_version = 1;

const QString sefam_class_name = "SefamLoader";

//! The one model validated end-to-end against a manufacturer report.
const QString sefam_validated_model = "1279R";

//! Humidifier level, the one accessory setting decoded from the card.
extern ChannelID SEFAM_HumidLevel;

/*! \class SefamLoader
    \brief Imports SEFAM CPAP SD cards.

    Any card matching <digits><letter>/<digits>/DATA_nnn/ is accepted. Sample
    rates and channel names come from each session's .INI, and header length is
    detected rather than assumed, so models other than the validated one import
    on a best-effort basis and raise deviceIsUntested(). */
class SefamLoader : public CPAPLoader
{
    Q_OBJECT
  public:
    SefamLoader();
    virtual ~SefamLoader();

    static void Register();

    virtual bool Detect(const QString &path) override;
    virtual int  Open(const QString &path) override;
    virtual void initChannels() override;
    virtual int  Version() override { return sefam_data_version; }
    virtual const QString &loaderName() override { return sefam_class_name; }
    virtual MachineInfo PeekInfo(const QString &path) override;

    virtual MachineInfo newInfo() override {
        return MachineInfo(MT_CPAP, 0, sefam_class_name, QObject::tr("Sefam"),
                           QString(), QString(), QString(), QObject::tr("Sefam"),
                           QDateTime::currentDateTime(), sefam_data_version);
    }

    //! Copy the card to the machine's backup folder, honouring the user preference.
    bool backupData(Machine *mach, const QString &path);

  protected:
    bool rebuild_from_backups = false;
    bool create_backups = true;

    /*! \brief Locate the <modelcode>/<serial> directory beneath a card root.
        \return Absolute path, or an empty string if the tree does not match. */
    QString findSerialDir(const QString &path);

    /*! \brief Add one channel to a session as one or more waveform EventLists.
        \param session OSCAR session to populate.
        \param values  Pre-scaled sample values; multiplied by \a gain for display.
        \param valid   Per-sample validity, same length as \a values.
        \param chan    Target OSCAR channel.
        \param gain    Multiplier OSCAR applies to each stored value.
        \param rateMs  Milliseconds per sample.
        \param startMs Session start, milliseconds since epoch.

        Invalid samples are excluded and each contiguous run of valid samples
        becomes its own EventList, because one EventList holds one contiguous
        span. Imported naively the 0xFF sentinel would read as 153 L/min leak or
        25.5 cmH2O and corrupt the session statistics.

        Values are pre-scaled rather than passed as raw bytes with an offset
        because OSCAR applies EventList offsets inconsistently: gLineChart
        renders (raw + offset) * gain, while EventList::data() and
        FlowParser::openFlow() apply gain alone and ignore the offset entirely.
        Baking any offset into the stored value keeps graphs and statistics
        in agreement. */
    void importWaveform(Session *session, const QVector<qint16> &values,
                        const QVector<bool> &valid, ChannelID chan,
                        EventDataType gain, int rateMs, qint64 startMs);
};

#endif  // SEFAM_LOADER_H
