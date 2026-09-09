#ifndef BMCLOADER_H
#define BMCLOADER_H

#include <QVector>
#include <QFile>
#include "SleepLib/machine.h" // Base class: MachineLoader
#include "SleepLib/machine_loader.h"
#include "SleepLib/profiles.h"
#include "SleepLib/loader_plugins/bmcDataParsing.h"



class EventList;

const int bmc_version = 1;
const QString bmc_class_name = "BMC";

class BmcLoader;
class BmcDataLink;
class BmcDataParser;

class BmcLoaderTask: public ImportTask
{
public:
    BmcLoaderTask(BmcLoader* machineLoader, Machine* machine, BmcDataParser* bmcData, BmcDataLink *dataLink, int totalLinkCount, int currentLinkIdx):
        bmcLoader(machineLoader),
        mach(machine),
        bmc(bmcData),
        bmcLink(dataLink),
        totalLinksToImport(totalLinkCount),
        currentLinkIndex(currentLinkIdx)
    {}

    BmcLoader* bmcLoader;
    Machine* mach;
    BmcDataParser* bmc;
    BmcDataLink* bmcLink;
    int totalLinksToImport;
    int currentLinkIndex;

    virtual void run();
};



class BmcLoader : public CPAPLoader
{    
public:
    BmcLoader();

    void setSessionMachineSettings(BmcDateSession*, Session*);
    void setSessionRespiratoryEvents(BmcSession*, Session*);
    void setSessionWaveforms(BmcSession*, Session*);
        

    virtual bool Detect(const QString & path);

    virtual MachineInfo PeekInfo(const QString & path);

    virtual void initChannels();

    virtual int Open(const QString &);

    static void Register();

    virtual const QString &loaderName() { return bmc_class_name; }

    virtual int Version() { return bmc_version; }

    virtual MachineInfo newInfo() {
            return MachineInfo(MT_CPAP, 0, bmc_class_name, QObject::tr("BMC"), QString(),
            QString(), QString(), QObject::tr("BMC"), QDateTime::currentDateTime(), bmc_version);
        }

    

    

    virtual QString PresReliefLabel();
    virtual ChannelID PresReliefMode();
    virtual ChannelID PresReliefLevel();
    virtual ChannelID CPAPModeChannel();

    virtual double FlowWaveformGain() const { return 0.1; }
    virtual double PressureWaveformGain() const { return 0.1; }
    /// Gain applied to Raw.IPAP/Raw.EPAP when writing CPAP_Pressure/CPAP_IPAP/CPAP_EPAP
    /// event lists.  Legacy BMC stores half-cmH2O units (gain 0.5); G3X stores hundredths
    /// of cmH2O (gain 0.01) for full 0.01 cmH2O resolution.
    virtual double PressureChannelGain() const { return 0.5; }
    virtual double FlowAbnormalityWaveformGain() const { return 1.0; }
    virtual double WaveformSampleIntervalMs() const { return 1000 / 25.0; }
    virtual int WaveformSamplesPerPacket() const { return 25; }
    virtual qint64 WaveformPacketDurationMs() const { return 1000; }
    /// Legacy BMC: the PressureWave array (0x08) is mapped to CPAP_MaskPressure.
    /// No separate pressure-wave chart is needed.
    virtual bool ExportPressureWaveform() const { return false; }
    virtual bool ExportFlowAbnormalityWaveform() const { return true; }
    virtual bool ExportLeakRate() const { return true; }
    /// Returns true if CPAP_Ti, CPAP_Te, CPAP_IE, and BMC_IE_Ratio channels
    /// should be populated.  Override to false when the source fields are not
    /// confirmed to carry Ti/Te data (e.g. G3X offsets 0x074/0x07E).
    virtual bool ExportTimingChannels() const { return true; }

    /// Returns true if CPAP_PB (periodic breathing / Cheyne-Stokes) events
    /// should be emitted to OSCAR.  Legacy BMC: enabled (source confirmed).
    /// G3X: suppressed until the 0x44 EVT flag is validated; the signal is
    /// suspected nonsense in SC.74+ firmware and unverified in SC.72.
    virtual bool ExportPeriodicBreathing() const { return true; }

    /// Returns true if the BMC_RESLEX_PATIENT ("Reslex Availability") and
    /// BMC_RESLEX_MODE ("Reslex Mode") settings should be published.  Both are
    /// legacy-BMC concepts: availability is parsed from IDX byte 0x151 bit 7, and
    /// mode is a constant because legacy Reslex only ever operates full time.
    /// Override to false on families where neither has a known source, so the
    /// Daily Device Settings panel does not present a default as though it were a
    /// reading.  See GitLab #274.
    virtual bool ExportReslexDetails() const { return true; }

    /// Returns the session start timestamp to use for really_set_first().
    /// Base implementation uses the session's StartTimestamp (no adjustment).
    /// Subclasses may override to skip startup noise before therapy stabilizes.
    virtual qint64 findStableStartMs(BmcSession* bmcSession) const {
        return bmcSession->StartTimestamp.toMSecsSinceEpoch();
    }

    /// Returns the mask-off timestamp in ms since epoch, i.e. the point at which
    /// the patient removed the mask.  Used to populate SessionSlice(MaskOn) so that
    /// session->hours() reports mask-on time rather than total session length.
    ///
    /// Base implementation returns EndTimestamp (no mask-off detection; MaskOn
    /// covers the whole session).  Subclasses may override with device-specific logic.
    virtual qint64 findStableEndMs(BmcSession* bmcSession) const {
        return bmcSession->EndTimestamp.toMSecsSinceEpoch();
    }

    int sessionsLoaded;
};

#endif // BMCLOADER_H
