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

/*! \brief Has this hardware model code been validated well enough not to warn?

    Model codes are hardware family identifiers, not product names: the Rêve Auto
    reports "1279R" and the S.Box AUTO has been seen as both "1200R" and "1263R".

    - 1279R — Rêve Auto, reconciled against a manufacturer report over 31
      sessions: event taxonomy, indices, pressures and settings.
    - 1200R — S.Box AUTO, reconciled against a manufacturer report over 6 days:
      event counts per type, operating time and the full settings history.
    - 1263R — S.Box AUTO. No manufacturer report exists for it, so it rides on
      1200R's validation: same product name, same firmware version and the same
      card layout. Its decoded pressure settings were separately checked against
      the pressure the device actually delivered.

    Anything else still warns, which is the point — the loader accepts any card
    matching the directory pattern, so an unrecognised model code means a device
    nobody has checked.

    1265R — the Néa Auto — is recognised by name and image but deliberately left
    out of this list for now. Its container is the Rêve's, every session on the
    one card seen parses, and its decoded mode, pressures, ramp, comfort level
    and mask leak have each been checked day by day against the vendor
    software's own settings table. What is missing is a manufacturer report of
    the kind the other two entries rest on, and a comparison that has been run
    to completion. See Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md, "The Néa
    Auto". */
inline bool sefamModelIsValidated(const QString &modelCode)
{
    return modelCode == QLatin1String("1279R")
        || modelCode == QLatin1String("1200R")
        || modelCode == QLatin1String("1263R");
}

/*! \name SEFAM accessory settings
    The settings decoded from the card's therapy-settings record. Each was pinned
    by a controlled single-setting change written through the vendor software, or
    by a manufacturer report; see Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md. The
    heated tube level is the one the analyzer prints that is still unconfirmed.
    @{ */
extern ChannelID SEFAM_HumidLevel;    //!< Humidifier level, 0 = off. Not on the S.Box.
extern ChannelID SEFAM_MaskLeakSet;   //!< Theoretical mask leak at 12 cmH2O, l/min.
extern ChannelID SEFAM_ComfortLevel;  //!< Comfort Control Plus level.
extern ChannelID SEFAM_Circuit;       //!< Patient circuit diameter in mm.
/*! @} */

/*! \class SefamLoader
    \brief Imports SEFAM CPAP SD cards.

    Any card matching <digits><letter>/<digits>/DATA_nnn/ is accepted. Sample
    rates and channel names come from each session's .INI, and header length is
    detected rather than assumed, so model codes outside sefamModelIsValidated()
    import on a best-effort basis and raise deviceIsUntested(). */
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
