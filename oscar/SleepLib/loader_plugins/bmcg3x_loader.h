#ifndef BMCG3XLOADER_H
#define BMCG3XLOADER_H

#include <QDebug>
#include "SleepLib/loader_plugins/bmc_loader.h"

const int bmcg3x_version = 1;
const QString bmcg3x_class_name = "BMCG3X";

class BmcG3xLoader : public BmcLoader
{
public:
    BmcG3xLoader();

    virtual bool Detect(const QString& path) override;
    virtual MachineInfo PeekInfo(const QString& path) override;
    virtual int Open(const QString& path) override;
    virtual void initChannels() override;

    static void Register();

    virtual const QString& loaderName() override { return bmcg3x_class_name; }
    virtual int Version() override { return bmcg3x_version; }
    virtual MachineInfo newInfo() override {
        return MachineInfo(MT_CPAP, 0, bmcg3x_class_name, QObject::tr("BMC"), QString(),
                           QString(), QString(), QObject::tr("BMC"), QDateTime::currentDateTime(), bmcg3x_version);
    }

    // G3X pressure-wave bytes appear to be in finer units than legacy BMC packets.
    // Start with a conservative scale to put the plotted range near clinical values.
    virtual double PressureWaveformGain() const override { return 0.01; }
    virtual double PressureChannelGain() const override { return 0.01; }
    virtual double WaveformSampleIntervalMs() const override { return 20.0; }
    virtual int WaveformSamplesPerPacket() const override { return 50; }
    virtual qint64 WaveformPacketDurationMs() const override { return 1000; }
    virtual bool ExportPressureWaveform() const override { return false; }
    virtual bool ExportFlowAbnormalityWaveform() const override { return false; }

    /// Advances the session start past machine startup noise to the first
    /// therapeutically meaningful moment.
    ///
    /// Phase 1 — skip idle: the machine idles at minimum APAP pressure before
    ///   the patient puts on the mask; scan forward until PressureTrend rises
    ///   above the initial value.
    /// Phase 2 — skip ramp: once rising, scan until PressureTrend stops
    ///   increasing, i.e., the ramp peak has been reached.
    /// Phase 3 — flow fallback: for fixed-pressure (CPAP) sessions where pressure
    ///   never rises, scan forward for the first packet with sustained respiratory
    ///   flow, indicating the patient has put on the mask.
    virtual qint64 findStableStartMs(BmcSession* bmcSession) const override {
        const auto & waveforms = bmcSession->Waveforms;
        if (waveforms.size() < 2) {
            return bmcSession->StartTimestamp.toMSecsSinceEpoch();
        }

        // Phase 1: skip packets where pressure is at or below the initial idle value.
        const quint16 idlePt = waveforms[0].Raw.PressureTrend;
        int rampStart = 0;
        for (int i = 1; i < waveforms.size(); ++i) {
            if (waveforms[i].Raw.PressureTrend > idlePt) {
                rampStart = i;
                break;
            }
        }

        if (rampStart > 0) {
            // Phase 2: from rampStart, find first packet where pressure stops rising.
            for (int i = rampStart; i + 1 < waveforms.size(); ++i) {
                quint16 pt0 = waveforms[i].Raw.PressureTrend;
                quint16 pt1 = waveforms[i + 1].Raw.PressureTrend;
                if (pt0 >= pt1) {
                    return waveforms[i].Timestamp.toMSecsSinceEpoch();
                }
            }
            // Ramp never completed within this session.
            return bmcSession->StartTimestamp.toMSecsSinceEpoch();
        }

        // Phase 3: pressure never rose (fixed-pressure / CPAP mode, or very short session).
        // Scan forward for the first packet followed by a sustained run of respiratory flow.
        // Uses the same noise floor as findStableEndMs().
        static constexpr qint16 kMaskOnFlowNoise  = 25;
        static constexpr int    kMaskOnSustainSec = 10; ///< Consecutive active packets required.

        const int n = waveforms.size();
        for (int i = 0; i < n; ++i) {
            // Check whether this packet has any flow above the noise floor.
            bool active = false;
            const qint16* flow = waveforms[i].Raw.Flow;
            for (int s = 0; s < kBmcExtendedWaveformSamples; ++s) {
                if (flow[s] > kMaskOnFlowNoise || flow[s] < -kMaskOnFlowNoise) {
                    active = true;
                    break;
                }
            }
            if (!active) {
                continue;
            }
            // Count consecutive active packets from here.
            int streak = 0;
            for (int j = i; j < n && streak < kMaskOnSustainSec; ++j) {
                const qint16* f = waveforms[j].Raw.Flow;
                bool packetActive = false;
                for (int s = 0; s < kBmcExtendedWaveformSamples; ++s) {
                    if (f[s] > kMaskOnFlowNoise || f[s] < -kMaskOnFlowNoise) {
                        packetActive = true;
                        break;
                    }
                }
                if (packetActive) {
                    ++streak;
                } else {
                    break; // streak broken; outer loop will resume from i+1
                }
            }
            if (streak >= kMaskOnSustainSec) {
                return waveforms[i].Timestamp.toMSecsSinceEpoch();
            }
        }

        // Flow-based detection also failed — use full session start.
        return bmcSession->StartTimestamp.toMSecsSinceEpoch();
    }
    // Leak is now sourced from waveform packet offset 0x52A (raw × 0.16 → L/min).
    // The old EVT-based leak source (0x0C) was discarded; see bmcG3xDataParsing.cpp.
    virtual bool ExportLeakRate() const override { return true; }
    /// 0x074 and 0x07E are not confirmed Ti/Te: their sum is near-constant
    /// (~565 cs) regardless of RR and shows no correlation with respiratory
    /// parameters.  Suppress Ti, Te, IE channels until the correct offsets
    /// are identified.
    virtual bool ExportTimingChannels() const override { return false; }

    /// Periodic breathing episodes are read directly from EVT type 0x09 records,
    /// which mark the START of each episode with value2 = duration in milliseconds
    /// (confirmed 2026-03-30 via Lijunjun data).
    virtual bool ExportPeriodicBreathing() const override { return true; }

    /// The G3X/E5 TS block has no offset for either Reslex Availability or Reslex
    /// Mode, and the G3X parser sets neither — so both fell through to the struct
    /// default and the hard-coded zero, displaying "Clinician" and "Full Time" on
    /// every session regardless of the device.  PAP-Link shows neither setting for
    /// these machines.  Reslex itself (TS 0x86) is confirmed and still published.
    virtual bool ExportReslexDetails() const override { return false; }

    /// Detects the mask-off point by finding the last waveform packet with
    /// meaningful respiratory flow, followed by a sustained period of near-zero
    /// flow at the end of the session.
    ///
    /// After mask removal, the patient's respiratory flow through the mask drops
    /// to the instrument noise floor (±2–18 raw units at offset 0x56E).  The
    /// machine may continue running for many minutes (e.g. until the user presses
    /// the stop button), so the leak profile is not a reliable indicator.
    ///
    /// Detection rule:
    ///   1. Scan backward to find the last packet where any flow sample exceeds
    ///      kG3xMaskOffFlowNoise (i.e. has real respiratory signal).
    ///   2. If the trailing inactive period (from that packet to the last packet)
    ///      is at least kG3xMaskOffSustainSec seconds, declare mask-off at that
    ///      packet's timestamp.
    ///   3. Otherwise fall back to the full session end (mask on the whole time).
    virtual qint64 findStableEndMs(BmcSession* bmcSession) const override {
        /// Raw flow threshold above the noise floor (noise = ±2–18, breathing >> 25).
        static constexpr qint16 kG3xMaskOffFlowNoise  = 25;
        /// Minimum seconds of no-flow at session end to declare mask removal.
        static constexpr int    kG3xMaskOffSustainSec = 300;

        const auto& waveforms = bmcSession->Waveforms;
        const int n = waveforms.size();
        if (n < 2) {
            return bmcSession->EndTimestamp.toMSecsSinceEpoch();
        }

        // Scan backward: find the last packet with any flow sample above the noise floor.
        int lastActivePacket = -1;
        for (int i = n - 1; i >= 0; --i) {
            const qint16* flow = waveforms[i].Raw.Flow;
            for (int s = 0; s < kBmcExtendedWaveformSamples; ++s) {
                if (flow[s] > kG3xMaskOffFlowNoise || flow[s] < -kG3xMaskOffFlowNoise) {
                    lastActivePacket = i;
                    break;
                }
            }
            if (lastActivePacket >= 0) break;
        }

        if (lastActivePacket < 0) {
            // Entire session has near-zero flow — no useful mask-on period.
            return bmcSession->EndTimestamp.toMSecsSinceEpoch();
        }

        const int inactiveSec = static_cast<int>(
            waveforms[lastActivePacket].Timestamp.secsTo(waveforms[n - 1].Timestamp));

        if (inactiveSec < kG3xMaskOffSustainSec) {
            // Inactive period too short — could be an apnea at session end.
            return bmcSession->EndTimestamp.toMSecsSinceEpoch();
        }

        qDebug() << "BmcG3xLoader: mask-off detected at"
                 << waveforms[lastActivePacket].Timestamp.toString(Qt::ISODate)
                 << "inactiveSec" << inactiveSec;
        return waveforms[lastActivePacket].Timestamp.toMSecsSinceEpoch();
    }
};

#endif // BMCG3XLOADER_H
