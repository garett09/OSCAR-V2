/// @file bmcG3xDataParsing.cpp
/// @brief Parser for BMC Luna G3X SD-card data files (.idx, .evt, .00x).
///
/// The G3X device writes three file types per recording:
///   - `.idx`  — index; one 0x800-byte record per day, carrying byte offsets into
///               the waveform and event streams plus daily summary statistics.
///   - `.evt`  — event stream; fixed-size 0x20-byte records containing respiratory
///               events (apneas, hypopneas) and therapy pressure snapshots.
///   - `.000`–`.062` — waveform stream; a circular series of 64 MiB files, each a
///               stream of 0x800-byte packets at one-second cadence.
///
/// All multi-byte integers are little-endian unless otherwise stated.
///
/// See Notes/G3X/BMC_G3X_EVT_FORMAT.md and BMC_G3X_00X_FORMAT.md for detailed
/// offset maps and confidence levels for each field.
///
/// Copyright (c) 2026 The OSCAR Team

#include "SleepLib/loader_plugins/bmcG3xDataParsing.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QIODevice>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QDebug>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

// ============================================================
// Anonymous namespace — all helpers are file-local.
// ============================================================
namespace {

// ------------------------------------------------------------
// File / stream structural constants
// ------------------------------------------------------------

/// Size of one IDX index record; records begin at kG3xIdxRecordOffset.
constexpr int kG3xIdxRecordOffset = 0x800;
constexpr int kG3xIdxRecordSize   = 0x800;

/// Size of one EVT event record (32 bytes).
constexpr int kG3xEvtRecordSize = 0x20;

/// Size of one waveform packet (2048 bytes). Each file is exactly 64 MiB.
constexpr quint32 kG3xWaveformPacketSize = 0x800;
constexpr qint64  kG3xWaveformFileSpan   = 64 * 1024 * 1024; // 64 MiB per .00x file

/// Size of the legacy 0x100-byte buffer used to build a BmcWaveformPacket
/// via the inherited constructor.
constexpr int kLegacyWaveformPacketSize = 0x100;

// ------------------------------------------------------------
// Waveform packet — high-resolution sample region layout
// ------------------------------------------------------------
// Flow region: 100 int16 LE samples (200 bytes) at 0x56E.
// Pressure wave region: 50 uint16 LE samples (100 bytes) at 0x380.
//   0x24A (100 samples) was tried but values were large/noisy; 0x380 confirmed as mask pressure source.
// Output is resampled to kBmcExtendedWaveformSamples (50) per packet.

/// Flow waveform region: 100 × int16 LE at 0x56E.
/// 0x182 was tried but has a shifted zero baseline; 0x56E gives the correct baseline.
constexpr int    kG3xFlowRegionOffset      = 0x56E;
constexpr int    kG3xFlowRegionSampleCount = 100;
constexpr qint16 kG3xFlowRawClamp          = 2000;

/// Pressure wave / mask pressure region: 50 × uint16 LE at 0x380.
/// Confirmed as the G3X mask pressure source — output looks correct in OSCAR.
/// 0x24A (100 samples) was tried but produced large/noisy values and is not used.
constexpr int    kG3xPressureWaveRegionOffset      = 0x380;
constexpr int    kG3xPressureWaveRegionSampleCount = 50;
constexpr qint16 kG3xPressureWaveRawClamp          = 4000;

/// Mask pressure region at 0x24A — tried but produced large/noisy values; not used.
/// Kept here for reference in case future investigation resumes.
constexpr int    kG3xMaskPressureRegionOffset      = 0x24A;
constexpr int    kG3xMaskPressureRegionSampleCount = 100;
constexpr qint16 kG3xMaskPressureRawClamp          = 4000;

/// Number of output waveform samples per packet (defined in bmcDataParsing.h).
constexpr int kG3xWaveformOutputSampleCount = kBmcExtendedWaveformSamples;

// ------------------------------------------------------------
// Waveform packet — per-packet scalar field offsets (all uint16 LE)
// ------------------------------------------------------------

/// Instantaneous pressure seed (hundredths cmH2O). Used only if no better
/// pressure source is available from the EVT stream.
constexpr int kG3xOffsetPressureSeed = 0x00A;

/// Packet offsets 0x074 and 0x07E: originally labelled Ti/Te (centiseconds) but
/// their sum is near-constant (~565 cs) regardless of respiratory rate and shows
/// no correlation with RR, tidal volume, or pressure.  Not confirmed Ti/Te.
/// Read but currently discarded; kept for future investigation.
constexpr int kG3xOffsetInspirationTime = 0x074;
constexpr int kG3xOffsetExpirationTime  = 0x07E;

/// Leak rate (offset 0x52A). Scale: raw × G3xLeakScaleTenthsPerRawUnit() → tenths of L/min.
/// Confirmed for firmware G3-2.11.x (Luna G3X; internal build G3-2.SC.72.01).  The scale
/// is platform-dependent — see G3xLeakScaleTenthsPerRawUnit(), which returns a smaller
/// factor for E5 firmware.  Reports unintentional (mask-fit) leak; ~0 for intentional vent.
constexpr int kG3xOffsetLeak = 0x52A;

/// Alternate leak rate (offset 0x568). Present in all known firmware versions.
/// In firmware G3-2.11.x (JCCPAP) this field exists but differs from 0x52A (r≈0.05);
/// its baseline (~16 L/min) appears to reflect intentional vent flow.
/// In firmware G3-2.12.x+ (e.g. Kavolodin G3 A20), 0x52A is always zero and 0x568 is
/// the only available continuous leak signal (~15–17 L/min baseline at 0.16 scale).
/// Selected automatically at runtime when 0x52A is found to be essentially unpopulated.
constexpr int kG3xOffsetAlternateLeak = 0x568;

/// Tidal volume (offset 0x52C). Scale factor not yet determined; stored raw.
constexpr int kG3xOffsetTidalVolume = 0x52C;

/// Minute ventilation (offset 0x52E). Scale factor not yet determined; stored raw.
constexpr int kG3xOffsetMinuteVentilation = 0x52E;

/// SpO2 oxygen saturation percent (offset 0x08A). Single byte, 0 when not available.
constexpr int kG3xOffsetSpO2 = 0x08A;

/// Pulse rate in beats/min (offset 0x08C). Single byte, 0 when not available.
constexpr int kG3xOffsetPulseRate = 0x08C;

/// Respiratory rate in breaths/min (offset 0x530).
constexpr int kG3xOffsetRespiratoryRate = 0x530;

/// EPAP pressure trend in hundredths cmH2O (offset 0x76C).
/// Slowly-varying field matching the BMC "Pressure Trend" display.
constexpr int kG3xOffsetPressureTrendEPAP = 0x76C;

/// IPAP pressure trend in hundredths cmH2O (offset 0x76E).
/// Paired with 0x76C; identical values in CPAP mode, may differ in BiPAP mode.
constexpr int kG3xOffsetPressureTrendIPAP = 0x76E;


// ------------------------------------------------------------
// Pressure channel source selection
// ------------------------------------------------------------

/// @brief When true, the Pressure / IPAP / EPAP channels are sourced from the
///        waveform-packet pressure trend (0x76C → EPAP, 0x76E → IPAP) instead
///        of the EVT 0x42 snapshot stream.
///
/// Set false to revert to EVT-based pressure (step-wise updates from the
/// therapy-pressure snapshot records).
constexpr bool kG3xUsePressureTrendForPressureChannel = true;

// ------------------------------------------------------------
// EVT stream — message type codes
// ------------------------------------------------------------

/// Respiratory events — value2 is duration in milliseconds (confirmed 2026-03-25).
constexpr int kG3xEvtTypeUH   = 0x01; ///< Unclassified hypopnea (confirmed 2026-03-25)
constexpr int kG3xEvtTypeRERA = 0x0A; ///< Respiratory Effort Related Arousal (RERA); confirmed 2026-03-23
constexpr int kG3xEvtTypeUA   = 0x02; ///< Unclassified apnea
constexpr int kG3xEvtTypeOSA  = 0x03; ///< Obstructive sleep apnea
constexpr int kG3xEvtTypeCSA  = 0x04; ///< Central sleep apnea
constexpr int kG3xEvtTypeOH   = 0x07; ///< Obstructive hypopnea
constexpr int kG3xEvtTypeCH   = 0x08; ///< Central hypopnea
constexpr int kG3xEvtTypePBMarker = 0x09; ///< Periodic breathing episode marker (confirmed 2026-03-30).
                                           ///< Like other respiratory events, timestamp marks the START.
                                           ///< Duration is uint32 at offset 0x1C (low 16 | high 16 at 0x1E),
                                           ///< in milliseconds (confirmed 2026-03-30 via Lijunjun data).

/// Session boundary markers — timestamp only, value fields unused.
constexpr int kG3xEvtTypeSessionStart = 0x40; ///< Session start (machine begins therapy recording).
constexpr int kG3xEvtTypeSessionEnd   = 0x41; ///< Session end   (machine stops therapy recording).

/// Therapy pressure snapshot.
/// value2 (0x1C) = EPAP in hundredths cmH2O.
/// unk1e (0x1E) = IPAP in hundredths cmH2O (confirmed 2026-03-25: equals EPAP + PS×100;
/// for pure CPAP/APAP with no pressure support IPAP = EPAP so both fields are equal).
/// This is the primary pressure source for the waveform packet loop.
constexpr int kG3xEvtTypePressure = 0x42;

/// Flow limitation point events — present only when the machine detects FL.
/// Confirmed by PAP-Link alignment on two independent sessions (2026-03-23).
constexpr int kG3xEvtTypeFlowLimitMild     = 0x0E; ///< Mild flow limitation (grade 1)
constexpr int kG3xEvtTypeFlowLimitModerate = 0x0F; ///< Moderate flow limitation (grade 2)
constexpr int kG3xEvtTypeFlowLimitSevere   = 0x10; ///< Severe flow limitation (grade 3)

/// Per-breath event types: 0x0C marks the start of each inspiration,
/// 0x0D marks the start of each expiration.  One pair fires per breath cycle.
/// Used for AASM-based computed periodic breathing (PB) detection.
constexpr int kG3xEvtTypeBreathInspiration = 0x0C;
constexpr int kG3xEvtTypeBreathExpiration  = 0x0D;

// AASM-based PB detection thresholds, applied to device-classified CSA events.
//
// AASM definition: ≥3 central apneas lasting >3 s, separated by ≤20 s of normal
// breathing.  Detection uses CSA events from rawRespEvents, which carry device-measured
// durations, rather than inferring timing from 0x0C breath gaps.

/// Minimum device-reported apnea duration to qualify for PB scoring (seconds).
/// 3 s matches the AASM PB definition; in practice the G3X firmware never classifies
/// an event as an apnea unless it is ≥10 s, so this threshold has no practical effect.
constexpr int kG3xPbMinApneaDurationSec = 3;
/// Maximum normal-breathing interval between consecutive apneas for them to belong to
/// the same PB cluster (AASM: ≤20 s of normal breathing between apneas).
constexpr int kG3xPbMaxInterApneaNormalBreathSec = 20;
/// Minimum number of qualifying apneas in a cluster to score as a PB episode (AASM: ≥3).
constexpr int kG3xPbMinApneasPerEpisode = 3;

/// Clamping limits for respiratory event duration.
constexpr int kG3xRespEventMinDurationSec = 10;
constexpr int kG3xRespEventMaxDurationSec = 180;


// ============================================================
// Internal data structures
// ============================================================

/// @brief Accumulated therapy pressure carried forward from EVT 0x42 records.
///
/// Pressure is the only field populated from the EVT stream; all other vitals
/// (leak, tidal volume, minute ventilation, respiratory rate, pressure trend,
/// I:E ratio) are read directly from each waveform packet.
struct G3xSampleValues
{
    bool hasEPAP = false;
    bool hasIPAP = false;
    int  epapHundredths = 0;
    int  ipapHundredths = 0;
};

/// @brief A time-tagged pressure update from the EVT stream.
struct G3xTimedSampleUpdate
{
    qint64         TimestampSec = 0;
    G3xSampleValues Values;
};

/// @brief A raw respiratory event read from the EVT stream, prior to OSCAR mapping.
struct G3xRawRespEvent
{
    int      MessageType = 0;
    int      Value2Millis = 0; ///< Duration in milliseconds (value2 field; confirmed 2026-03-25)
    QDateTime Timestamp;
};

#ifdef BMCDEBUG
/// @brief One row written to the optional per-day diagnostics CSV.
struct G3xDiagRow
{
    qint64  TimestampSec  = 0;
    quint64 VirtualOffset = 0;

    // Raw header / vitals fields read from the waveform packet.
    int RawPressureSeed       = 0; ///< 0x00A — instantaneous pressure, hundredths cmH2O
    int RawUnknown074         = 0; ///< 0x074 — unknown; not confirmed Ti
    int RawUnknown07E         = 0; ///< 0x07E — unknown; not confirmed Te
    int RawLeak               = 0; ///< 0x52A (fw SC.72) or 0x568 (fw SC.74+); selected by Phase 3.5 probe
    int RawTidalVolume        = 0; ///< 0x52C (scale TBD)
    int RawMinuteVentilation  = 0; ///< 0x52E (scale TBD)
    int RawRespiratoryRate    = 0; ///< 0x530 — breaths/min
    int RawPressureTrend      = 0; ///< 0x76C — hundredths cmH2O

    // Decoded current-state pressure (from EVT 0x42 accumulation).
    int CurrentIPAPHundredths = -1;
    int CurrentEPAPHundredths = -1;

    // Output fields written to the legacy BmcWaveformPacket.
    int OutputRawIPAPHalfCm = 0;
    int OutputRawEPAPHalfCm = 0;

    // Waveform region min/max for sanity checking.
    int FlowMin          = 0;
    int FlowMax          = 0;
    int PressureWaveMin  = 0;
    int PressureWaveMax  = 0;
};
#endif // BMCDEBUG

// ============================================================
// Configuration helpers
// ============================================================

#ifdef BMCDEBUG
/// @brief Returns true when G3X diagnostics CSV output is requested via the
///        OSCAR_BMC_G3X_DIAG environment variable.
bool IsG3xDiagnosticsEnabled()
{
    static int enabled = -1;
    if (enabled >= 0) {
        return enabled != 0;
    }

    bool ok = false;
    const int val = qEnvironmentVariableIntValue("OSCAR_BMC_G3X_DIAG", &ok);
    if (ok) {
        enabled = (val != 0) ? 1 : 0;
    } else {
        enabled = qEnvironmentVariableIsSet("OSCAR_BMC_G3X_DIAG") ? 1 : 0;
    }
    return enabled != 0;
}
#endif // BMCDEBUG

/// @brief Returns true when the pressure value is within the physiologically
///        plausible CPAP range (4.00–35.00 cmH2O).
bool IsReasonablePressureHundredths(int pressureHundredths)
{
    return pressureHundredths >= 400 && pressureHundredths <= 3500;
}

/// @brief Default leak scale, in tenths of L/min per raw unit (raw × 0.16 = L/min).
///
/// Calibrated against PAP-Link on a Luna G3X (config `110A40113`, firmware G3-2.SC.72.01)
/// and applied to the whole G3 platform.  OSCAR's CPAP_Leak EventList stores tenths of
/// L/min and applies a display gain of 0.1, hence 1.6 rather than 0.16.
constexpr double kG3xLeakScaleDefaultTenths = 1.6;

/// @brief Leak scale for the E5 platform, in tenths of L/min per raw unit
///        (raw × 0.10 = L/min).
///
/// The G3-derived 0.16 overstates leak on the E5 by roughly 60%.  Derived from a PAP-Link
/// readout of an E5 B25A Plus reference card for one night, using the two measures that do
/// not depend on how the two programs define percentiles or handle mask-off periods:
///
///   - Mean leak: PAP-Link 0.70 L/min against a raw mean of 6.881 units → 0.1017.
///   - A visually stable stretch of graph read ~2 L/min in PAP-Link where OSCAR (at 0.16)
///     showed ~3.5, i.e. ~21.9 raw units → ~0.091.
///
/// Both land on ~0.10.  The same card's 95th percentile implies 0.12 and its maximum
/// implies 0.051; those two are not usable for calibration because PAP-Link's tail
/// statistics are evidently not computed the same way OSCAR's are — its reported maximum
/// (11.2 L/min) is far below the raw peak under *any* single linear scale, which points to
/// smoothing or mask-off exclusion on PAP-Link's side rather than to a different scale.
/// Note that a moving average cannot explain the difference in the mean, so the gap is a
/// scale difference and not smoothing alone.
///
/// **Confidence: medium** — one card, one night, one PAP-Link readout.  A second E5 sample
/// would settle whether 0.10 is exact or merely close.
constexpr double kG3xLeakScaleE5Tenths = 1.0;

/// @brief Returns the leak scale factor for @p firmwareVersion: raw → tenths of L/min.
///
/// Applies to both leak field offsets (0x52A and 0x568).  Override for any device via the
/// OSCAR_BMC_G3X_LEAK_SCALE environment variable (env value is in L/min per raw unit; the
/// code multiplies by 10 internally).
double G3xLeakScaleTenthsPerRawUnit(const QString& firmwareVersion)
{
    bool ok = false;
    const QByteArray envValue = qgetenv("OSCAR_BMC_G3X_LEAK_SCALE");
    const double envScale = envValue.toDouble(&ok);
    if (ok && envScale > 0.0 && envScale < 10.0) {
        return envScale * 10.0;
    }
    // Platform is the token before the first '-' in the version string ("E5-1.02.05.02").
    if (firmwareVersion.startsWith(QLatin1String("E5"), Qt::CaseInsensitive)) {
        return kG3xLeakScaleE5Tenths;
    }
    return kG3xLeakScaleDefaultTenths;
}

/// @brief Converts a raw leak field value to tenths of L/min using @p scaleTenthsPerRaw.
///
/// Clamps the result to [0, 5000] (0–500 L/min), which is well above any
/// clinically observed leak value.
int ConvertG3xLeakRawToTenths(int rawLeak, double scaleTenthsPerRaw)
{
    const double scaledTenths = static_cast<double>(qMax(0, rawLeak)) * scaleTenthsPerRaw;
    return qBound(0, qRound(scaledTenths), 5000);
}

// ============================================================
// Diagnostics CSV helpers
// ============================================================

#ifdef BMCDEBUG
/// @brief Builds the output path for the per-day diagnostics CSV file.
QString BuildG3xDiagnosticsPath(const QString& serial, const QDate& day)
{
    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (baseDir.isEmpty()) {
        baseDir = QDir::tempPath();
    }

    QDir dir(baseDir);
    if (!dir.exists("g3x-diagnostics")) {
        dir.mkpath("g3x-diagnostics");
    }

    const QString safeSerial = serial.isEmpty() ? QString("unknown") : serial;
    const QString filename = QString("%1_%2_%3.csv")
                                 .arg(safeSerial)
                                 .arg(day.toString("yyyyMMdd"))
                                 .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    return dir.absoluteFilePath(QString("g3x-diagnostics/%1").arg(filename));
}

/// @brief Computes the min and max of an array of int16 values.
void ComputePacketMinMax(const qint16* values, int count, int* minOut, int* maxOut)
{
    if (!values || count <= 0 || !minOut || !maxOut) {
        return;
    }
    int mn = values[0];
    int mx = values[0];
    for (int i = 1; i < count; ++i) {
        mn = std::min<int>(mn, values[i]);
        mx = std::max<int>(mx, values[i]);
    }
    *minOut = mn;
    *maxOut = mx;
}

/// @brief Writes the per-day diagnostics CSV.
///
/// Each row corresponds to one accepted waveform packet.  Fields reflect the
/// raw values read from the packet and the decoded state at that point in time.
bool WriteG3xDiagnosticsCsv(const QString& outputPath,
                             const QVector<G3xDiagRow>& rows,
                             const QDate& day,
                             quint32 waveStartOffset,
                             quint32 waveEndOffset,
                             quint32 eventStartOffset,
                             quint32 eventEndOffset)
{
    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }

    QTextStream out(&file);
    out << "# day=" << day.toString(Qt::ISODate)
        << ", wave_start=" << waveStartOffset
        << ", wave_end="   << waveEndOffset
        << ", event_start=" << eventStartOffset
        << ", event_end="   << eventEndOffset
        << ", rows=" << rows.size()
        << "\n";

    out << "timestamp_iso,timestamp_sec,virtual_offset"
           ",raw_pressure_seed_0x0A"
           ",raw_unknown_0x74,raw_unknown_0x7E"
           ",raw_leak_0x52A,raw_tv_0x52C,raw_mv_0x52E,raw_rr_0x530"
           ",raw_pressure_trend_0x76C"
           ",current_ipap_hundredths,current_epap_hundredths"
           ",output_raw_ipap_halfcm,output_raw_epap_halfcm"
           ",flow_min,flow_max,pressurewave_min,pressurewave_max\n";

    for (const G3xDiagRow& row : rows) {
        out << QDateTime::fromSecsSinceEpoch(row.TimestampSec).toString(Qt::ISODate) << ","
            << row.TimestampSec                  << ","
            << row.VirtualOffset                 << ","
            << row.RawPressureSeed               << ","
            << row.RawUnknown074                 << ","
            << row.RawUnknown07E                 << ","
            << row.RawLeak                       << ","
            << row.RawTidalVolume                << ","
            << row.RawMinuteVentilation          << ","
            << row.RawRespiratoryRate            << ","
            << row.RawPressureTrend              << ","
            << row.CurrentIPAPHundredths         << ","
            << row.CurrentEPAPHundredths         << ","
            << row.OutputRawIPAPHalfCm           << ","
            << row.OutputRawEPAPHalfCm           << ","
            << row.FlowMin                       << ","
            << row.FlowMax                       << ","
            << row.PressureWaveMin               << ","
            << row.PressureWaveMax
            << "\n";
    }

    file.close();
    return true;
}
#endif // BMCDEBUG

// ============================================================
// Low-level binary decode helpers
// ============================================================

/// @brief Decodes a 6-byte G3X timestamp (year-1900, month, day, hour, min, sec)
///        at @p baseOffset within @p p.  Returns false if the date/time is invalid.
bool DecodeG3xTimestamp(const char* p, int baseOffset, QDateTime* out)
{
    const int year   = 1900 + static_cast<unsigned char>(p[baseOffset + 0]);
    const int month  = static_cast<unsigned char>(p[baseOffset + 1]);
    const int day    = static_cast<unsigned char>(p[baseOffset + 2]);
    const int hour   = static_cast<unsigned char>(p[baseOffset + 3]);
    const int minute = static_cast<unsigned char>(p[baseOffset + 4]);
    const int second = static_cast<unsigned char>(p[baseOffset + 5]);

    const QDate date(year, month, day);
    const QTime time(hour, minute, second);
    if (!date.isValid() || !time.isValid()) {
        return false;
    }

    *out = QDateTime(date, time);
    return true;
}

/// @brief Writes a little-endian int16 into a byte buffer.
void SetInt16LE(char* p, int offset, qint16 value)
{
    p[offset]     = static_cast<char>(value & 0xFF);
    p[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
}

/// @brief Writes a little-endian uint16 into a byte buffer.
void SetUInt16LE(char* p, int offset, quint16 value)
{
    p[offset]     = static_cast<char>(value & 0xFF);
    p[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
}

/// @brief Converts pressure in hundredths of cmH2O to the half-cmH2O raw units
///        used in the legacy BmcWaveformPacketStruct.
qint16 PressureHundredthsToRawHalfCm(int pressureHundredths)
{
    // half-cm units: 1 unit = 0.5 cmH2O = 50 hundredths.
    const int halfCm = qBound(0, (pressureHundredths + 25) / 50, 100);
    return static_cast<qint16>(halfCm);
}

/// @brief Reads the pressure seed from waveform packet offset 0x0A.
///        Returns -1 if the value is outside the physiologically plausible range.
int ReadPacketPressureSeedHundredths(const QByteArray& waveformPacket)
{
    if (waveformPacket.size() < 12) {
        return -1;
    }
    const unsigned char* b = reinterpret_cast<const unsigned char*>(waveformPacket.constData() + kG3xOffsetPressureSeed);
    const int pressureHundredths = static_cast<int>(b[0] | (b[1] << 8));
    return IsReasonablePressureHundredths(pressureHundredths) ? pressureHundredths : -1;
}

/// @brief Reads a little-endian uint16 from a raw byte pointer.
qint32 ReadUInt16LEPtr(const char* p, int offset)
{
    const unsigned char* b = reinterpret_cast<const unsigned char*>(p + offset);
    return static_cast<quint16>(b[0] | (b[1] << 8));
}

/// @brief Reads a little-endian int16 from a raw byte pointer.
qint16 ReadInt16LEPtr(const char* p, int offset)
{
    return static_cast<qint16>(ReadUInt16LEPtr(p, offset));
}

/// @brief Decodes the full timestamp of a 32-byte EVT record, including milliseconds.
///
/// The whole-second part is the standard 6-byte G3X timestamp at 0x14.  The
/// little-endian uint16 immediately after it, at 0x1A, carries the milliseconds
/// within that second (confirmed 2026-08-05):
///
///   - Across 246,260 records of one card the field never exceeded 999, and its
///     values were spread evenly over 0-999 (about 24,600 per 100 ms decile).
///   - Within a run of records sharing the same whole second it is non-decreasing
///     in file order for 99.7% of pairs; every exception is a transition between
///     two different record types, which are not strictly interleaved.
///   - The 0x42 pressure records are emitted on a 30 s timer.  Including this field
///     resolves that interval to 30031 ms with an interquartile range of 32 ms;
///     an unrelated value would have scattered it across a full second.
///   - Inspiration-to-expiration intervals (0x0C -> 0x0D) change from a comb at
///     0 s / 1 s into a smooth distribution with a median of 842 ms.
///
/// PAP-Link rounds this to the nearest second for display; OSCAR event lists store
/// milliseconds natively, so the value is carried through unrounded.
///
/// @param rec Pointer to the start of a 32-byte EVT record.
/// @param out Receives the decoded timestamp.
/// @return false if the whole-second part is not a valid date/time.
bool DecodeG3xEvtTimestamp(const char* rec, QDateTime* out)
{
    if (!DecodeG3xTimestamp(rec, 0x14, out)) {
        return false;
    }

    // Guard against a corrupt record: only a value the field is known to take is
    // applied, so a bad read leaves the whole-second timestamp intact.
    const qint32 millis = ReadUInt16LEPtr(rec, 0x1A);
    if (millis >= 0 && millis <= 999) {
        *out = out->addMSecs(millis);
    }
    return true;
}

// ============================================================
// Pressure accumulation helpers
// ============================================================

/// @brief Merges a pressure update from the EVT stream into the running
///        current-state struct.  Only pressure fields are carried from EVT;
///        all other vitals are read directly from each waveform packet.
void MergeSampleValues(const G3xSampleValues& update, G3xSampleValues* current)
{
    if (!current) {
        return;
    }
    if (update.hasIPAP) {
        current->hasIPAP      = true;
        current->ipapHundredths = update.ipapHundredths;
    }
    if (update.hasEPAP) {
        current->hasEPAP      = true;
        current->epapHundredths = update.epapHundredths;
    }
}

// ============================================================
// Packet construction helpers
// ============================================================

/// @brief Builds a BmcWaveformPacket carrying only the current pressure state.
///
/// All other fields (leak, tidal volume, minute ventilation, respiratory rate,
/// I:E ratio, pressure trend) are populated directly from the waveform packet
/// after this function returns.
///
/// The returned packet is constructed via the legacy 0x100-byte buffer so that
/// IPAP/EPAP are encoded in the format expected by downstream consumers.
BmcWaveformPacket BuildLegacyCompatiblePacket(const QDateTime& timestamp,
                                               const G3xSampleValues& values)
{
    char buffer[kLegacyWaveformPacketSize] = {0};
    SetUInt16LE(buffer, 0x00, 0xAAAD); // packet magic

    if (values.hasIPAP) {
        SetInt16LE(buffer, 0x04, PressureHundredthsToRawHalfCm(values.ipapHundredths));
    }
    if (values.hasEPAP) {
        SetInt16LE(buffer, 0x06, PressureHundredthsToRawHalfCm(values.epapHundredths));
    }

    // Embed timestamp so the packet is self-describing.
    SetUInt16LE(buffer, 0xF8, static_cast<quint16>(timestamp.date().year()));
    buffer[0xFA] = static_cast<char>(timestamp.date().month());
    buffer[0xFB] = static_cast<char>(timestamp.date().day());
    buffer[0xFC] = static_cast<char>(timestamp.time().hour());
    buffer[0xFD] = static_cast<char>(timestamp.time().minute());
    buffer[0xFE] = static_cast<char>(timestamp.time().second());

    return BmcWaveformPacket(buffer);
}

/// @brief Copies the flow and pressure-wave waveform regions from a raw G3X
///        waveform packet into a legacy BmcWaveformPacket.
///
/// Each source region contains 100 int16 LE samples.  BMC writes each value
/// as an identical pair (50 discrete values × 2 = 100 stored), so the
/// resampler naturally handles the redundancy.
///
/// Flow abnormality data (region at 0x510) is currently not displayed pending
/// further validation.  The FlowAbnormality arrays are zeroed.
void ApplyFlowWaveformFromG3xPacket(const QByteArray& waveformPacket,
                                     BmcWaveformPacket* legacyPacket)
{
    if (!legacyPacket) {
        return;
    }

    // Verify the packet is large enough to contain all waveform regions.
    const int requiredSize = qMax(
        kG3xFlowRegionOffset        + (kG3xFlowRegionSampleCount        * 2),
        qMax(
            kG3xPressureWaveRegionOffset + (kG3xPressureWaveRegionSampleCount * 2),
            kG3xMaskPressureRegionOffset + (kG3xMaskPressureRegionSampleCount * 2)));
    if (waveformPacket.size() < requiredSize) {
        return;
    }

    const char* packetData = waveformPacket.constData();

    // Flow abnormality is not yet validated; zero the output arrays explicitly.
    std::fill_n(legacyPacket->Raw.FlowAbnormality, kG3xWaveformOutputSampleCount, qint16(0));
    std::fill_n(legacyPacket->FlowAbnormality,     kG3xWaveformOutputSampleCount, quint16(0));

    for (int i = 0; i < kG3xWaveformOutputSampleCount; ++i) {
        // --- Flow ---
        // Map output index i into the 100-sample source region.
        const int srcFlow = (i * (kG3xFlowRegionSampleCount - 1)) / (kG3xWaveformOutputSampleCount - 1);
        const qint16 rawFlow = qBound<qint16>(
            -kG3xFlowRawClamp,
            ReadInt16LEPtr(packetData, kG3xFlowRegionOffset + (srcFlow * 2)),
            kG3xFlowRawClamp);
        legacyPacket->Raw.Flow[i] = rawFlow;
        legacyPacket->Flow[i]     = rawFlow / 10.0f;

        // --- Pressure wave (0x380) ---
        // Decoded as unsigned 16-bit; values in the 400–1600 range (pressure-like, slowly varying).
        const int srcPressure = (i * (kG3xPressureWaveRegionSampleCount - 1)) / (kG3xWaveformOutputSampleCount - 1);
        const qint16 rawPressureWave = static_cast<qint16>(qMin(
            static_cast<int>(kG3xPressureWaveRawClamp),
            ReadUInt16LEPtr(packetData, kG3xPressureWaveRegionOffset + (srcPressure * 2))));
        legacyPacket->Raw.PressureWave[i] = rawPressureWave;
        legacyPacket->PressureWave[i]     = rawPressureWave;

        // --- Mask pressure (0x24A) ---
        // Decoded as signed 16-bit, 100 source samples resampled to 50 output samples.
        // Clamped to [0, max]: negative values are suppressed (not plotted below zero line).
        const int srcMask = (i * (kG3xMaskPressureRegionSampleCount - 1)) / (kG3xWaveformOutputSampleCount - 1);
        const qint16 rawMaskPressure = qBound<qint16>(
            qint16(0),
            ReadInt16LEPtr(packetData, kG3xMaskPressureRegionOffset + (srcMask * 2)),
            kG3xMaskPressureRawClamp);
        legacyPacket->Raw.MaskPressure[i] = rawMaskPressure;
        legacyPacket->MaskPressure[i]     = rawMaskPressure;
    }
}

} // end anonymous namespace

// ============================================================
// BmcG3xData — public interface
// ============================================================

BmcG3xData::BmcG3xData() { }

BmcG3xData::BmcG3xData(const QString& path)
    : BmcG3xData()
{
    QString tmpPath(path);
    if (!tmpPath.endsWith(QDir::separator())) {
        tmpPath.append(QDir::separator());
    }
    dirPath = tmpPath;
    ResolveIdxFile();
}

bool BmcG3xData::DirectoryHasBmcG3xData(const QString& path)
{
    QDir dir(path);
    if (!dir.exists()) {
        return false;
    }

    const QFileInfoList idxCandidates = dir.entryInfoList(QStringList() << "*.idx", QDir::Files);
    for (const QFileInfo& idxInfo : idxCandidates) {
        QFile idxFile(idxInfo.absoluteFilePath());
        if (!idxFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray header = idxFile.read(32);
        idxFile.close();
        if (!IsG3xIdxHeader(header)) {
            continue;
        }
        const QString basePath = idxInfo.absolutePath() + QDir::separator() + idxInfo.completeBaseName();
        if (QFile::exists(basePath + ".000")) {
            return true;
        }
    }
    return false;
}

void BmcG3xData::ReadData()
{
    if (!ResolveIdxFile()) {
        throw std::invalid_argument("BmcG3xData: Missing G3X index file");
    }

    QFile idxFile(idxFilePath);
    if (!idxFile.open(QIODevice::ReadOnly)) {
        throw std::invalid_argument("BmcG3xData: Could not open G3X index file");
    }

    const QByteArray idxBytes = idxFile.readAll();
    idxFile.close();

    ParseMachineInfo(idxBytes);
    ParseIdxRecords(idxBytes);
}

BmcMachineInfo BmcG3xData::ReadMachineInfo()
{
    if (!machineInfo.SerialNumber.isEmpty() || !machineInfo.Model.isEmpty()) {
        return machineInfo;
    }
    if (!ResolveIdxFile()) {
        return machineInfo;
    }

    QFile idxFile(idxFilePath);
    if (!idxFile.open(QIODevice::ReadOnly)) {
        return machineInfo;
    }

    const QByteArray idxBytes = idxFile.readAll();
    idxFile.close();

    ParseMachineInfo(idxBytes);
    return machineInfo;
}

// ============================================================
// BmcG3xData::ReadDateSession — main import entry point
// ============================================================

/// @brief Reads all waveform packets and EVT records for one calendar day and
///        returns a fully populated BmcDateSession.
///
/// Processing order:
///   1. Parse the EVT stream for the day window:
///      - Respiratory events (0x02–0x09) → dateSession.RespiratoryEvents.
///      - Therapy pressure snapshots (0x42) → timedSampleUpdates (pressure only).
///   2. Seed the initial pressure state from the IDX daily summary if EVT
///      provides no pressure records.
///   3. Iterate waveform packets in file order:
///      a. Merge any EVT pressure updates whose timestamp precedes the packet.
///      b. Read all vitals directly from the packet (leak, TV, MV, RR,
///         pressure trend, inspiration/expiration times).
///      c. Build a BmcWaveformPacket and append to dateSession.Waveforms.
///   4. Split the waveform list into BmcSession objects on gaps ≥ 5 seconds.
///   5. Assign respiratory events to the session whose window contains them.
BmcDateSession BmcG3xData::ReadDateSession(QDate aDate)
{
    // ---- Locate the IDX entry for this date ----
    const G3xDayEntry* dayEntry = nullptr;
    for (const G3xDayEntry& entry : dayEntries) {
        if (entry.Date == aDate) {
            dayEntry = &entry;
            break;
        }
    }
    // Backward-compatible fallback: match on waveform timestamp date.
    if (!dayEntry) {
        for (const G3xDayEntry& entry : dayEntries) {
            if (entry.StartTimestamp.date() == aDate) {
                dayEntry = &entry;
                break;
            }
        }
    }
    if (!dayEntry) {
        throw std::invalid_argument("No G3X session with that date could be found");
    }

    // ---- Populate session metadata ----
    BmcDateSession dateSession;
    dateSession.StartTime        = dayEntry->StartTimestamp;
    dateSession.DurationMinutes  = std::max(1, static_cast<int>(
        dayEntry->StartTimestamp.secsTo(dayEntry->EndTimestamp) / 60));
    dateSession.MachineInfo      = ReadMachineInfo();
    std::memset(&dateSession.MacineSettings, 0, sizeof(BmcMachineSettings));
    dateSession.MacineSettings.Mode = BmcMode::CPAP;

    // ---- Phase 1: Parse EVT stream ----
    // Collect therapy pressure updates and respiratory events.

    QList<G3xTimedSampleUpdate> timedSampleUpdates;
    bool hasEvtPressureUpdates  = false;
    int  evtPressureUpdateCount = 0;

    QVector<G3xRawRespEvent>  rawRespEvents;
    QVector<QDateTime>        rawInspirationTimestamps;
    QVector<BmcFlowLimitEvent> rawFlEvents;
    int rawRespType01Count = 0;
    int rawRespType02Count = 0;
    int rawRespType03Count = 0;
    int rawRespType04Count = 0;
    int rawRespType07Count = 0;
    int rawRespType08Count = 0;
    int rawRespType09Count = 0;  // counts 0x09 PB marker records (for BMCDEBUG)
    int rawRespType0ACount = 0;
    QVector<BmcRespiratoryEvent> rawPbEvents;  // PB episodes from 0x09 records

    // Collected for EVT-only session boundary detection (waveLen == 0).
    QVector<QDateTime> evtSessionStarts;
    QVector<QDateTime> evtSessionEnds;

    const QString evtFilePath = fileBasePath + ".evt";
    QFile evtFile(evtFilePath);
    if (evtFile.open(QIODevice::ReadOnly)) {
        const qint64 safeStart = std::min<qint64>(dayEntry->EventStartOffset, evtFile.size());
        const qint64 safeEnd   = std::min<qint64>(dayEntry->EventEndOffset,   evtFile.size());
        if (safeEnd > safeStart) {
            // EventStartOffset in the IDX is not guaranteed to fall on a 32-byte record
            // boundary (confirmed for at least one device whose offset falls 16 bytes into
            // a record).  Round down to the nearest boundary; the AE AA magic check will
            // skip the partial record fragment at the start.
            const qint64 alignedStart = (safeStart / kG3xEvtRecordSize) * kG3xEvtRecordSize;
            evtFile.seek(alignedStart);
            const QByteArray evtBytes = evtFile.read(static_cast<qint64>(safeEnd - alignedStart));

            for (int offset = 0; offset + kG3xEvtRecordSize <= evtBytes.size(); offset += kG3xEvtRecordSize) {
                const char* rec = evtBytes.constData() + offset;

                // Each EVT record begins with magic bytes AE AA.
                if (static_cast<unsigned char>(rec[0]) != 0xAE ||
                    static_cast<unsigned char>(rec[1]) != 0xAA) {
                    continue;
                }

                // 0x1A is the millisecond part of the timestamp and is folded in by
                // DecodeG3xEvtTimestamp(); it is not a separate payload field.
                QDateTime evtTime;
                if (!DecodeG3xEvtTimestamp(rec, &evtTime)) {
                    continue;
                }

                const int messageType = static_cast<unsigned char>(rec[0x10]);
                const int value2      = ReadUInt16LEPtr(rec, 0x1C);

                switch (messageType) {
                case kG3xEvtTypePressure: {
                    // value2 (0x1C) = EPAP; unk1e (0x1E) = IPAP (both hundredths cmH2O).
                    // For CPAP/APAP with no pressure support IPAP = EPAP so both fields are equal.
                    const int epapHundredths = value2;
                    const int ipapHundredths = ReadUInt16LEPtr(rec, 0x1E);
                    if (IsReasonablePressureHundredths(epapHundredths)) {
                        G3xSampleValues sample;
                        sample.hasEPAP        = true;
                        sample.epapHundredths = epapHundredths;
                        // Use IPAP if valid; otherwise fall back to EPAP (CPAP mode).
                        sample.hasIPAP        = true;
                        sample.ipapHundredths = IsReasonablePressureHundredths(ipapHundredths)
                                                ? ipapHundredths
                                                : epapHundredths;
                        timedSampleUpdates.append(G3xTimedSampleUpdate{evtTime.toSecsSinceEpoch(), sample});
                        hasEvtPressureUpdates = true;
                        ++evtPressureUpdateCount;
                    }
                    break;
                }

                case kG3xEvtTypeUH:
                case kG3xEvtTypeUA:
                case kG3xEvtTypeOSA:
                case kG3xEvtTypeCSA:
                case kG3xEvtTypeOH:
                case kG3xEvtTypeCH:
                case kG3xEvtTypeRERA:
                    // Respiratory events: value2 = duration in milliseconds (confirmed 2026-03-25).
                    // Timestamp marks the START of the event (confirmed 2026-04-03 by PAP-Link position comparison).
                    switch (messageType) {
                    case kG3xEvtTypeUH:   ++rawRespType01Count; break;
                    case kG3xEvtTypeUA:   ++rawRespType02Count; break;
                    case kG3xEvtTypeOSA:  ++rawRespType03Count; break;
                    case kG3xEvtTypeCSA:  ++rawRespType04Count; break;
                    case kG3xEvtTypeOH:   ++rawRespType07Count; break;
                    case kG3xEvtTypeCH:   ++rawRespType08Count; break;
                    case kG3xEvtTypeRERA: ++rawRespType0ACount; break;
                    default: break;
                    }
                    rawRespEvents.append(G3xRawRespEvent{messageType, value2, evtTime});
                    break;

                case kG3xEvtTypePBMarker:
                {
                    // Periodic breathing episode start marker (confirmed 2026-03-30).
                    // Unlike other respiratory events, timestamp marks the START of the episode.
                    // Duration is a uint32 at offset 0x1C: low 16 bits (value2) | high 16 bits at 0x1E.
                    // Reading as uint16 only gives ~28s/23s; uint32 gives correct ~159s/154s
                    // matching PAP-Link (confirmed 2026-03-30 via Lijunjun data).
                    ++rawRespType09Count;
                    const quint32 durationMs = static_cast<quint32>(value2) |
                                               (static_cast<quint32>(ReadUInt16LEPtr(rec, 0x1E)) << 16);
                    BmcRespiratoryEvent pbEvt;
                    pbEvt.EventType       = BmcRespiratoryEventType::PB;
                    pbEvt.StartTime       = evtTime;
                    pbEvt.DurationSeconds = static_cast<int>(durationMs / 1000);
                    pbEvt.EndTime         = pbEvt.StartTime.addSecs(pbEvt.DurationSeconds);
                    rawPbEvents.append(pbEvt);
                    break;
                }

                case kG3xEvtTypeBreathInspiration:
                    // Per-breath inspiration marker; one record per breath cycle.
                    // Collected for AASM-based PB detection in Phase 2b.
                    rawInspirationTimestamps.append(evtTime);
                    break;

                case kG3xEvtTypeBreathExpiration:
                    // Per-breath expiration marker; not currently decoded.
                    break;

                case kG3xEvtTypeFlowLimitMild:
                case kG3xEvtTypeFlowLimitModerate:
                case kG3xEvtTypeFlowLimitSevere:
                {
                    // Flow limitation point events — confirmed by PAP-Link alignment.
                    // Collected here and assigned to sessions after the loop.
                    int grade = 0;
                    switch (messageType) {
                    case kG3xEvtTypeFlowLimitMild:     grade = 1; break;
                    case kG3xEvtTypeFlowLimitModerate: grade = 2; break;
                    case kG3xEvtTypeFlowLimitSevere:   grade = 3; break;
                    default: break;
                    }
                    BmcFlowLimitEvent flEvt;
                    flEvt.Timestamp  = evtTime;
                    flEvt.Grade      = grade;
                    flEvt.DurationMs = value2; // milliseconds, same convention as respiratory events
                    rawFlEvents.append(flEvt);
                    break;
                }

                case kG3xEvtTypeSessionStart:
                    // Collected for EVT-only session building when no waveform data exists.
                    evtSessionStarts.append(evtTime);
                    break;
                case kG3xEvtTypeSessionEnd:
                    evtSessionEnds.append(evtTime);
                    break;

                default:
                    // All other EVT message types are not currently decoded.
                    break;
                }
            }
        }
        evtFile.close();
    }

    // If no pressure records exist in the indexed day slice, perform a full-file
    // scan restricted to message 0x42 near the day window.  This handles cases
    // where EVT timestamps are shifted relative to the IDX date boundary.
    if (!hasEvtPressureUpdates) {
        QFile evtFallbackFile(evtFilePath);
        if (evtFallbackFile.open(QIODevice::ReadOnly)) {
            const QByteArray evtBytes = evtFallbackFile.readAll();
            evtFallbackFile.close();

            // Allow a ±12-hour window around the waveform timestamps, and also
            // accept records dated one day either side of the target date.
            const qint64 startWindowSec = dayEntry->StartTimestamp.toSecsSinceEpoch() - (12 * 60 * 60);
            const qint64 endWindowSec   = dayEntry->EndTimestamp.toSecsSinceEpoch()   + (12 * 60 * 60);
            const QDate  dateWindowStart = aDate.addDays(-1);
            const QDate  dateWindowEnd   = aDate.addDays(1);
            int fallbackPressureUpdates = 0;
            int scanned42 = 0;
            int inTimeWindow42 = 0;
            int inDateWindow42 = 0;

            for (int offset = 0; offset + kG3xEvtRecordSize <= evtBytes.size(); offset += kG3xEvtRecordSize) {
                const char* rec = evtBytes.constData() + offset;
                if (static_cast<unsigned char>(rec[0]) != 0xAE ||
                    static_cast<unsigned char>(rec[1]) != 0xAA) {
                    continue;
                }
                if (static_cast<unsigned char>(rec[0x10]) != kG3xEvtTypePressure) {
                    continue;
                }
                ++scanned42;

                QDateTime evtTime;
                if (!DecodeG3xEvtTimestamp(rec, &evtTime)) {
                    continue;
                }

                const qint64 evtSec      = evtTime.toSecsSinceEpoch();
                const bool   inTimeWindow = (evtSec >= startWindowSec && evtSec <= endWindowSec);
                const bool   inDateWindow = (evtTime.date() >= dateWindowStart && evtTime.date() <= dateWindowEnd);
                if (inTimeWindow) { ++inTimeWindow42; }
                if (inDateWindow) { ++inDateWindow42; }
                if (!inTimeWindow && !inDateWindow) {
                    continue;
                }

                const int epapHundredths = ReadUInt16LEPtr(rec, 0x1C);
                const int ipapHundredths = ReadUInt16LEPtr(rec, 0x1E);
                if (!IsReasonablePressureHundredths(epapHundredths)) {
                    continue;
                }

                G3xSampleValues sample;
                sample.hasEPAP        = true;
                sample.epapHundredths = epapHundredths;
                sample.hasIPAP        = true;
                sample.ipapHundredths = IsReasonablePressureHundredths(ipapHundredths)
                                        ? ipapHundredths
                                        : epapHundredths;
                timedSampleUpdates.append(G3xTimedSampleUpdate{evtSec, sample});
                hasEvtPressureUpdates = true;
                ++evtPressureUpdateCount;
                ++fallbackPressureUpdates;
            }

#ifdef BMCDEBUG
            qDebug() << "BmcG3xData: EVT pressure fallback scan day" << aDate.toString(Qt::ISODate)
                     << "scanned42"    << scanned42
                     << "timeWindow42" << inTimeWindow42
                     << "dateWindow42" << inDateWindow42
                     << "recovered"    << fallbackPressureUpdates;
#endif // BMCDEBUG
        }
    }

    // Sort pressure updates chronologically for the merge step below.
    std::sort(timedSampleUpdates.begin(), timedSampleUpdates.end(),
              [](const G3xTimedSampleUpdate& a, const G3xTimedSampleUpdate& b) {
                  return a.TimestampSec < b.TimestampSec;
              });

    // EVT records are not always in strict chronological order (pressure and
    // respiratory records can appear out of sequence by up to ~86 minutes).
    // Sort both raw event lists so OSCAR's EventList receives them in time
    // order; out-of-order insertion triggers costly reindex and display glitches.
    std::sort(rawRespEvents.begin(), rawRespEvents.end(),
              [](const G3xRawRespEvent& a, const G3xRawRespEvent& b) {
                  return a.Timestamp < b.Timestamp;
              });
    std::sort(rawFlEvents.begin(), rawFlEvents.end(),
              [](const BmcFlowLimitEvent& a, const BmcFlowLimitEvent& b) {
                  return a.Timestamp < b.Timestamp;
              });

    // ---- Phase 2: Map EVT respiratory events to OSCAR types ----

    if (!rawRespEvents.isEmpty()) {
        int mappedOsaCount  = 0;
        int mappedCsaCount  = 0;
        int mappedHypCount  = 0;
        int mappedUaCount   = 0;
        int mappedReraCount = 0;
        int ignoredCount    = 0;

        for (const G3xRawRespEvent& rawEvt : rawRespEvents) {
            BmcRespiratoryEventType mappedType  = BmcRespiratoryEventType::Unknown;
            bool                    hasMappedType = true;

            switch (rawEvt.MessageType) {
            case kG3xEvtTypeUH:  mappedType = BmcRespiratoryEventType::HYP;  break;
            case kG3xEvtTypeUA:  mappedType = BmcRespiratoryEventType::UA;   break;
            case kG3xEvtTypeOSA: mappedType = BmcRespiratoryEventType::OSA;  break;
            case kG3xEvtTypeCSA: mappedType = BmcRespiratoryEventType::CSA;  break;
            case kG3xEvtTypeOH:
            case kG3xEvtTypeCH:  mappedType = BmcRespiratoryEventType::HYP;  break;
            case kG3xEvtTypeRERA: mappedType = BmcRespiratoryEventType::RERA; break;
            default: hasMappedType = false; break;
            }

            if (!hasMappedType) {
                ++ignoredCount;
                continue;
            }

            BmcRespiratoryEvent evt;
            evt.EventType = mappedType;
            evt.StartTime = rawEvt.Timestamp;
            // value2 encodes duration in milliseconds (confirmed 2026-03-25).
            // Clamp to [10, 180] seconds to guard against corrupt records.
            const int durationSec = static_cast<int>(rawEvt.Value2Millis / 1000.0 + 0.5);
            evt.DurationSeconds = qBound(kG3xRespEventMinDurationSec, durationSec, kG3xRespEventMaxDurationSec);
            evt.EndTime = evt.StartTime.addSecs(evt.DurationSeconds);
            dateSession.RespiratoryEvents.append(evt);

            switch (mappedType) {
            case BmcRespiratoryEventType::OSA:  ++mappedOsaCount;  break;
            case BmcRespiratoryEventType::CSA:  ++mappedCsaCount;  break;
            case BmcRespiratoryEventType::HYP:  ++mappedHypCount;  break;
            case BmcRespiratoryEventType::UA:   ++mappedUaCount;   break;
            case BmcRespiratoryEventType::RERA: ++mappedReraCount; break;
            default: break;
            }
        }

#ifdef BMCDEBUG
        qDebug() << "BmcG3xData respiratory summary day" << aDate.toString(Qt::ISODate)
                 << "raw01(UH)"   << rawRespType01Count
                 << "raw02(UA)"   << rawRespType02Count
                 << "raw03(OSA)"  << rawRespType03Count
                 << "raw04(CSA)"  << rawRespType04Count
                 << "raw07(OH)"   << rawRespType07Count
                 << "raw08(CH)"   << rawRespType08Count
                 << "raw09(PB)"   << rawRespType09Count
                 << "raw0A(RERA)" << rawRespType0ACount
                 << "mappedUA"    << mappedUaCount
                 << "mappedOSA"   << mappedOsaCount
                 << "mappedCSA"   << mappedCsaCount
                 << "mappedHYP"   << mappedHypCount
                 << "mappedRERA"  << mappedReraCount
                 << "ignored"     << ignoredCount;

#endif // BMCDEBUG
    }

    // ---- Phase 2b: Collect PB episodes from EVT 0x09 records ----
    //
    // EVT type 0x09 is a periodic breathing episode marker (confirmed 2026-03-30 via
    // Lijunjun data).  Unlike other respiratory events, the timestamp marks the START
    // of the episode.  Duration is uint32 at offset 0x1C (low 16 bits = value2,
    // high 16 bits at 0x1E), in milliseconds.

    for (const BmcRespiratoryEvent& pbEvt : rawPbEvents) {
        dateSession.RespiratoryEvents.append(pbEvt);
    }
#ifdef BMCDEBUG
    qDebug() << "BmcG3xData PB day" << aDate.toString(Qt::ISODate)
             << "episodes" << rawPbEvents.size();
#endif // BMCDEBUG

    // ---- Phase 2c: Collect flow limitation events ----

    if (!rawFlEvents.isEmpty()) {
        dateSession.FlowLimitEvents = rawFlEvents;
#ifdef BMCDEBUG
        qDebug() << "BmcG3xData FL day" << aDate.toString(Qt::ISODate)
                 << "mild"     << std::count_if(rawFlEvents.begin(), rawFlEvents.end(), [](const BmcFlowLimitEvent& e){ return e.Grade == 1; })
                 << "moderate" << std::count_if(rawFlEvents.begin(), rawFlEvents.end(), [](const BmcFlowLimitEvent& e){ return e.Grade == 2; })
                 << "severe"   << std::count_if(rawFlEvents.begin(), rawFlEvents.end(), [](const BmcFlowLimitEvent& e){ return e.Grade == 3; });
#endif // BMCDEBUG
    }

    // ---- EVT-only path (no waveform data on card) ----
    if (dayEntry->WaveLength == 0) {
        // Build BmcPressureSnapshot list from the 0x42 updates already collected in Phase 1.
        // timedSampleUpdates is sorted chronologically (sorted after Phase 1).
        QVector<BmcPressureSnapshot> evtPressureSnapshots;
        for (const G3xTimedSampleUpdate& u : timedSampleUpdates) {
            const int epap = u.Values.hasEPAP ? u.Values.epapHundredths
                                              : (u.Values.hasIPAP ? u.Values.ipapHundredths : 0);
            const int ipap = u.Values.hasIPAP ? u.Values.ipapHundredths : epap;
            if (epap > 0 || ipap > 0) {
                BmcPressureSnapshot snap;
                snap.Timestamp       = QDateTime::fromSecsSinceEpoch(u.TimestampSec);
                snap.EpapHundredths  = epap;
                snap.IpapHundredths  = ipap;
                evtPressureSnapshots.append(snap);
            }
        }

        if (!evtSessionStarts.isEmpty()) {
            // Build one BmcSession per 0x40 marker.  Pair by index: the i-th start
            // pairs with the i-th end (BMC firmware emits them in matched pairs).
            // Fall back to dayEntry->EndTimestamp if the corresponding end is absent.
            for (int i = 0; i < evtSessionStarts.size(); ++i) {
                BmcSession* s      = new BmcSession();
                s->StartTimestamp  = evtSessionStarts.at(i);
                s->EndTimestamp    = (i < evtSessionEnds.size())
                                         ? evtSessionEnds.at(i)
                                         : dayEntry->EndTimestamp;

                for (const BmcPressureSnapshot& snap : evtPressureSnapshots) {
                    if (snap.Timestamp >= s->StartTimestamp &&
                        snap.Timestamp <= s->EndTimestamp) {
                        s->PressureSnapshots.append(snap);
                    }
                }
                dateSession.Sessions.append(s);
            }
        } else {
            // No EVT session markers: create one synthetic session from IT block duration.
            BmcSession* s     = new BmcSession();
            s->StartTimestamp = dayEntry->StartTimestamp;
            s->EndTimestamp   = dayEntry->EndTimestamp;

            // Add two synthetic pressure snapshots (start + end) at the EPAP setting
            // so the session passes the downstream skip check and OSCAR has a pressure anchor.
            if (IsReasonablePressureHundredths(dayEntry->ItPressureEPAPHundredths)) {
                BmcPressureSnapshot snapStart;
                snapStart.Timestamp      = s->StartTimestamp;
                snapStart.EpapHundredths = dayEntry->ItPressureEPAPHundredths;
                snapStart.IpapHundredths = dayEntry->ItPressureEPAPHundredths;
                BmcPressureSnapshot snapEnd;
                snapEnd.Timestamp      = s->EndTimestamp.addSecs(-1);
                snapEnd.EpapHundredths = dayEntry->ItPressureEPAPHundredths;
                snapEnd.IpapHundredths = dayEntry->ItPressureEPAPHundredths;
                s->PressureSnapshots.append(snapStart);
                s->PressureSnapshots.append(snapEnd);
            }
            dateSession.Sessions.append(s);
        }

        // Assign respiratory and FL events to the session whose window contains them.
        // Event timestamps carry milliseconds (EVT offset 0x1A) but session boundaries
        // are whole seconds, so the window runs to the end of EndTimestamp's second.
        // Without that an event in the closing second is dropped whenever its
        // millisecond part is non-zero.
        for (BmcSession* s : dateSession.Sessions) {
            const QDateTime windowEnd = s->EndTimestamp.addMSecs(999);
            for (const BmcRespiratoryEvent& evt : dateSession.RespiratoryEvents) {
                if (evt.StartTime >= s->StartTimestamp && evt.StartTime <= windowEnd) {
                    s->RespiratoryEvents.append(evt);
                }
            }
            for (const BmcFlowLimitEvent& evt : dateSession.FlowLimitEvents) {
                if (evt.Timestamp >= s->StartTimestamp && evt.Timestamp <= windowEnd) {
                    s->FlowLimitEvents.append(evt);
                }
            }
        }

        // Machine settings from IT block (mirrors Phase 5 logic in the normal path).
        const float epapCmH2O = IsReasonablePressureHundredths(dayEntry->ItPressureEPAPHundredths)
                                    ? dayEntry->ItPressureEPAPHundredths / 100.0f
                                    : 4.0f;
        dateSession.MacineSettings.CPAP_TreatP   = epapCmH2O;
        dateSession.MacineSettings.CPAP_InitialP  = epapCmH2O;
        dateSession.MacineSettings.CPAP_ManualP   = epapCmH2O;

        int minPressureHundredths = 0;
        if (IsReasonablePressureHundredths(dayEntry->TsPressureMinHundredths))
            minPressureHundredths = dayEntry->TsPressureMinHundredths;
        else if (IsReasonablePressureHundredths(dayEntry->ItPressureMinHundredths))
            minPressureHundredths = dayEntry->ItPressureMinHundredths;

        int maxPressureHundredths = 0;
        if (IsReasonablePressureHundredths(dayEntry->TsPressureMaxHundredths))
            maxPressureHundredths = dayEntry->TsPressureMaxHundredths;
        else if (IsReasonablePressureHundredths(dayEntry->ItPressureMaxHundredths))
            maxPressureHundredths = dayEntry->ItPressureMaxHundredths;

        if (minPressureHundredths > 0) {
            dateSession.MacineSettings.APAP_IntialP = minPressureHundredths / 100.0f;
            dateSession.MacineSettings.APAP_MinAPAP = minPressureHundredths / 100.0f;
        }
        if (maxPressureHundredths > 0)
            dateSession.MacineSettings.APAP_MaxAPAP = maxPressureHundredths / 100.0f;

        dateSession.MacineSettings.Mode =
            (minPressureHundredths > 0 && maxPressureHundredths > minPressureHundredths)
                ? BmcMode::AutoCPAP
                : BmcMode::CPAP;

        // Real settings supersede the inference above.  Prefer this day's own TS block from
        // the IDX — it records what was in force that night — and fall back to the .set
        // file (the device's *current* config) only when the record carried no TS block.
        if (!DecodeTsBlock(dayEntry->TsBlock, dateSession.MacineSettings)) {
            ApplySetFileSettings(dateSession.MacineSettings);
        }

        return dateSession;
    }
    // ---- End EVT-only path — waveform path continues below ----

    // ---- Phase 3: Seed initial pressure from IDX summary ----
    // Used when no EVT pressure records were found.

    int idxPressureSeedHundredths = 0;
    if (IsReasonablePressureHundredths(dayEntry->ItPressureEPAPHundredths)) {
        idxPressureSeedHundredths = dayEntry->ItPressureEPAPHundredths;
    } else if (IsReasonablePressureHundredths(dayEntry->ItPressureMinHundredths)) {
        idxPressureSeedHundredths = dayEntry->ItPressureMinHundredths;
    } else if (IsReasonablePressureHundredths(dayEntry->TsPressureMinHundredths)) {
        idxPressureSeedHundredths = dayEntry->TsPressureMinHundredths;
    }

    // Scan EVT updates to find the first valid pressure and prime currentValues.
    G3xSampleValues currentValues;
    bool seededPressureFromEvt = false;
    for (const G3xTimedSampleUpdate& update : timedSampleUpdates) {
        if (update.Values.hasIPAP) {
            currentValues.hasIPAP       = true;
            currentValues.ipapHundredths  = update.Values.ipapHundredths;
            seededPressureFromEvt = true;
        }
        if (update.Values.hasEPAP) {
            currentValues.hasEPAP       = true;
            currentValues.epapHundredths  = update.Values.epapHundredths;
            seededPressureFromEvt = true;
        }
        if (seededPressureFromEvt) {
            // Mirror IPAP↔EPAP if only one was present.
            if (!currentValues.hasIPAP && currentValues.hasEPAP) {
                currentValues.hasIPAP       = true;
                currentValues.ipapHundredths  = currentValues.epapHundredths;
            }
            if (!currentValues.hasEPAP && currentValues.hasIPAP) {
                currentValues.hasEPAP       = true;
                currentValues.epapHundredths  = currentValues.ipapHundredths;
            }
            break;
        }
    }

    if (!seededPressureFromEvt && idxPressureSeedHundredths > 0) {
        currentValues.hasIPAP       = true;
        currentValues.ipapHundredths  = idxPressureSeedHundredths;
        currentValues.hasEPAP       = true;
        currentValues.epapHundredths  = idxPressureSeedHundredths;
    } else if (!seededPressureFromEvt) {
        // Fall back to 4 cmH2O — the lowest typical CPAP setting.
        currentValues.hasIPAP       = true;
        currentValues.ipapHundredths  = 400;
        currentValues.hasEPAP       = true;
        currentValues.epapHundredths  = 400;
    }

    // ---- Phase 3.5: Select which leak offset to use ----
    //
    // Resolved once per device (see ResolveLeakFieldOffset) rather than per day: which
    // field the firmware populates cannot change from one night to the next.
    const int leakFieldOffset = ResolveLeakFieldOffset();
    // The raw→L/min scale is platform-dependent: the G3-calibrated 0.16 overstates leak
    // on the E5 by roughly 60%.  See G3xLeakScaleTenthsPerRawUnit().
    const double leakScaleTenthsPerRaw =
        G3xLeakScaleTenthsPerRawUnit(dateSession.MachineInfo.FirmwareVersion);

    // ---- Phase 4: Iterate waveform packets ----

#ifdef BMCDEBUG
    const bool diagnosticsEnabled = IsG3xDiagnosticsEnabled();
    QVector<G3xDiagRow> diagRows;
    if (diagnosticsEnabled) {
        const quint32 packetCountEstimate = std::max<quint32>(1, dayEntry->WaveLength / kG3xWaveformPacketSize);
        diagRows.reserve(static_cast<int>(packetCountEstimate));
    }
#endif // BMCDEBUG

    float  firstPressureCmH2O   = -1.0f;
    qint16 waveformRawIpapMin   = std::numeric_limits<qint16>::max();
    qint16 waveformRawIpapMax   = std::numeric_limits<qint16>::min();
    int    outOfOrderPacketCount = 0;
    qint64 lastTimestampKey      = std::numeric_limits<qint64>::min();
    int    timedUpdateIndex      = 0;

    const int startFileIndex = static_cast<int>(dayEntry->WaveStartOffset / kG3xWaveformFileSpan);
    const int endFileIndex   = static_cast<int>((dayEntry->WaveEndOffset - 1) / kG3xWaveformFileSpan);

    for (int fileIndex = startFileIndex; fileIndex <= endFileIndex; ++fileIndex) {
        const QString filepath = QString("%1.%2")
                                     .arg(fileBasePath)
                                     .arg(fileIndex, 3, 10, QLatin1Char('0'));

        QFile waveformFile(filepath);
        if (!waveformFile.open(QIODevice::ReadOnly)) {
            continue;
        }

        qint64 localStart = (fileIndex == startFileIndex)
                                ? (dayEntry->WaveStartOffset % kG3xWaveformFileSpan) : 0;
        qint64 localEnd   = (fileIndex == endFileIndex)
                                ? (dayEntry->WaveEndOffset % kG3xWaveformFileSpan)   : waveformFile.size();
        // If the day boundary falls exactly on a file boundary, treat the whole file.
        if (fileIndex == endFileIndex && localEnd == 0) {
            localEnd = waveformFile.size();
        }

        localStart = std::max<qint64>(0, std::min(localStart, waveformFile.size()));
        localEnd   = std::max<qint64>(0, std::min(localEnd,   waveformFile.size()));
        if (localEnd <= localStart) {
            waveformFile.close();
            continue;
        }

        qint64 cursor = (localStart / kG3xWaveformPacketSize) * kG3xWaveformPacketSize;
        while (cursor + kG3xWaveformPacketSize <= localEnd) {
            waveformFile.seek(cursor);
            const QByteArray waveformPacket = waveformFile.read(kG3xWaveformPacketSize);
            if (waveformPacket.size() < static_cast<int>(kG3xWaveformPacketSize)) {
                break;
            }

            // Validate packet magic (AD AA).
            if (static_cast<unsigned char>(waveformPacket.at(0)) != 0xAD ||
                static_cast<unsigned char>(waveformPacket.at(1)) != 0xAA) {
                cursor += kG3xWaveformPacketSize;
                continue;
            }

            // Validate and decode the packet timestamp (bytes 0x04–0x09).
            QDateTime packetTimestamp;
            if (!DecodeG3xTimestamp(waveformPacket.constData(), 0x04, &packetTimestamp)) {
                cursor += kG3xWaveformPacketSize;
                continue;
            }

            // Timestamp validation: skip exact duplicates and stale ring-buffer packets.
            // Allow backward jumps up to kMaxAllowedBackwardSecs to handle DST "fall back"
            // (clocks go back 1 hour). Larger backward jumps indicate stale ring-buffer
            // data from a previous recording cycle and are discarded.
            // Matches the tolerance used by the legacy BMC loader.
            constexpr qint64 kMaxAllowedBackwardSecs = 7200LL;
            const qint64 timestampKey = packetTimestamp.toSecsSinceEpoch();
            if (timestampKey == lastTimestampKey) {
                // Exact duplicate: skip silently.
                cursor += kG3xWaveformPacketSize;
                continue;
            }
            if (lastTimestampKey != std::numeric_limits<qint64>::min() &&
                timestampKey < lastTimestampKey - kMaxAllowedBackwardSecs) {
                // Large backward jump: stale data, skip.
                ++outOfOrderPacketCount;
                cursor += kG3xWaveformPacketSize;
                continue;
            }

            // Merge any EVT pressure updates that precede this packet's timestamp.
            while (timedUpdateIndex < timedSampleUpdates.size() &&
                   timedSampleUpdates.at(timedUpdateIndex).TimestampSec <= timestampKey) {
                MergeSampleValues(timedSampleUpdates.at(timedUpdateIndex).Values, &currentValues);
                ++timedUpdateIndex;
            }

            // ---- Read vitals directly from this waveform packet ----
            const char* packetData = waveformPacket.constData();

            // Leak: offset selected in Phase 3.5 (0x52A for fw SC.72, 0x568 for fw SC.74+).
            const int rawLeak     = ReadUInt16LEPtr(packetData, leakFieldOffset);
            const int leakTenths  = ConvertG3xLeakRawToTenths(rawLeak, leakScaleTenthsPerRaw);

            // Tidal volume (0x52C): raw stored directly; scale factor TBD.
            const int rawTidalVolume = ReadUInt16LEPtr(packetData, kG3xOffsetTidalVolume);

            // Minute ventilation (0x52E): raw stored directly; scale factor TBD.
            const int rawMinuteVentilation = ReadUInt16LEPtr(packetData, kG3xOffsetMinuteVentilation);

            // SpO2 (0x08A) and pulse rate (0x08C): single-byte values, 0 when unavailable.
            const int rawSpO2      = static_cast<unsigned char>(packetData[kG3xOffsetSpO2]);
            const int rawPulseRate = static_cast<unsigned char>(packetData[kG3xOffsetPulseRate]);

            // Respiratory rate (0x530): direct breaths/min value.
            const int rawRespiratoryRate = ReadUInt16LEPtr(packetData, kG3xOffsetRespiratoryRate);

            // Pressure trend (0x76C = EPAP, 0x76E = IPAP): hundredths cmH2O.
            // Identical in CPAP mode; may differ in BiPAP mode.
            const int rawPressureTrend     = ReadUInt16LEPtr(packetData, kG3xOffsetPressureTrendEPAP);
            const int rawPressureTrendIPAP = ReadUInt16LEPtr(packetData, kG3xOffsetPressureTrendIPAP);

            // Offsets 0x074 and 0x07E were originally labelled inspiration/expiration time
            // (centiseconds), but analysis shows their sum is near-constant (~565 cs)
            // regardless of respiratory rate and they carry no correlation with RR, TV,
            // MV, or pressure.  They are NOT confirmed Ti/Te.  The I:E computation is
            // suppressed until the correct offsets are identified.
            // The raw values are still captured in the diagnostics CSV (RawUnknown074/07E).
            const int packetIePermille = 0;

            // Fall back to waveform packet pressure seed only if EVT and IDX
            // have not already provided a pressure value.
            const int packetPressureSeedHundredths = ReadPacketPressureSeedHundredths(waveformPacket);
            if (!hasEvtPressureUpdates && idxPressureSeedHundredths <= 0 &&
                packetPressureSeedHundredths > 0) {
                currentValues.hasIPAP       = true;
                currentValues.ipapHundredths  = packetPressureSeedHundredths;
                currentValues.hasEPAP       = true;
                currentValues.epapHundredths  = packetPressureSeedHundredths;
            }

            // Track the first valid pressure for machine-settings initialisation.
            if (firstPressureCmH2O < 0.0f) {
                if (currentValues.hasIPAP) {
                    firstPressureCmH2O = currentValues.ipapHundredths / 100.0f;
                } else if (currentValues.hasEPAP) {
                    firstPressureCmH2O = currentValues.epapHundredths / 100.0f;
                } else if (packetPressureSeedHundredths > 0) {
                    firstPressureCmH2O = packetPressureSeedHundredths / 100.0f;
                }
            }

            // ---- Build output packet ----
            BmcWaveformPacket legacyPacket = BuildLegacyCompatiblePacket(packetTimestamp, currentValues);

            // Populate vitals read directly from the waveform packet.
            legacyPacket.Raw.Leak           = static_cast<quint16>(qBound(0, leakTenths, 65535));
            legacyPacket.Leak               = leakTenths / 10.0f;

            legacyPacket.Raw.TidalVolume    = static_cast<qint16>(qBound(0, rawTidalVolume, 32767));
            legacyPacket.TidalVolume        = rawTidalVolume;

            legacyPacket.Raw.MinuteVentilation = static_cast<qint16>(qBound(0, rawMinuteVentilation, 32767));
            legacyPacket.MinuteVentilation     = static_cast<float>(rawMinuteVentilation);

            legacyPacket.Raw.RespiratoryRate  = static_cast<quint16>(qBound(0, rawRespiratoryRate, 65535));
            legacyPacket.RespiratoryRate      = static_cast<quint16>(qBound(0, rawRespiratoryRate, 65535));

            legacyPacket.Raw.SpO2Pct   = static_cast<quint16>(rawSpO2);
            legacyPacket.Raw.PulseRate = static_cast<quint16>(rawPulseRate);

            if (packetIePermille > 0) {
                // Store I:E as per-mille (0–1000) for Ti/Te derivation downstream.
                legacyPacket.Raw.IERatioMapped = static_cast<qint16>(packetIePermille);
                legacyPacket.IERatio           = static_cast<float>(packetIePermille) / 10.0f;
            }

            // Pressure trend: convert hundredths cmH2O to cmH2O.
            legacyPacket.Raw.PressureTrend  = static_cast<quint16>(rawPressureTrend);

            // Override Pressure / IPAP / EPAP with waveform pressure trend values.
            // 0x76C → EPAP, 0x76E → IPAP (identical in CPAP mode).
            if (kG3xUsePressureTrendForPressureChannel && rawPressureTrend > 0) {
                // Store hundredths of cmH2O directly; PressureChannelGain() returns 0.01
                // in the G3X loader so the displayed value is raw / 100 = cmH2O.
                legacyPacket.Raw.EPAP = static_cast<qint16>(rawPressureTrend);
                legacyPacket.Raw.IPAP = static_cast<qint16>(rawPressureTrendIPAP);
                legacyPacket.EPAP     = rawPressureTrend     / 100.0f;
                legacyPacket.IPAP     = rawPressureTrendIPAP / 100.0f;
            }

            // Copy flow and pressure-wave waveform regions into the output packet.
            ApplyFlowWaveformFromG3xPacket(waveformPacket, &legacyPacket);

            dateSession.Waveforms.append(legacyPacket);
            waveformRawIpapMin = std::min<qint16>(waveformRawIpapMin, legacyPacket.Raw.IPAP);
            waveformRawIpapMax = std::max<qint16>(waveformRawIpapMax, legacyPacket.Raw.IPAP);

#ifdef BMCDEBUG
            // ---- Optionally record a diagnostics row ----
            if (diagnosticsEnabled) {
                G3xDiagRow row;
                row.TimestampSec  = timestampKey;
                row.VirtualOffset = (static_cast<quint64>(fileIndex) * static_cast<quint64>(kG3xWaveformFileSpan)) +
                                    static_cast<quint64>(cursor);

                row.RawPressureSeed            = packetPressureSeedHundredths > 0 ? packetPressureSeedHundredths : 0;
                row.RawUnknown074              = ReadUInt16LEPtr(packetData, kG3xOffsetInspirationTime);
                row.RawUnknown07E              = ReadUInt16LEPtr(packetData, kG3xOffsetExpirationTime);
                row.RawLeak                    = rawLeak;
                row.RawTidalVolume             = rawTidalVolume;
                row.RawMinuteVentilation       = rawMinuteVentilation;
                row.RawRespiratoryRate         = rawRespiratoryRate;
                row.RawPressureTrend           = rawPressureTrend;

                row.CurrentIPAPHundredths = currentValues.hasIPAP ? currentValues.ipapHundredths : -1;
                row.CurrentEPAPHundredths = currentValues.hasEPAP ? currentValues.epapHundredths : -1;

                row.OutputRawIPAPHalfCm = legacyPacket.Raw.IPAP;
                row.OutputRawEPAPHalfCm = legacyPacket.Raw.EPAP;

                ComputePacketMinMax(legacyPacket.Raw.Flow,
                                    kG3xWaveformOutputSampleCount,
                                    &row.FlowMin, &row.FlowMax);
                ComputePacketMinMax(legacyPacket.Raw.PressureWave,
                                    kG3xWaveformOutputSampleCount,
                                    &row.PressureWaveMin, &row.PressureWaveMax);

                diagRows.append(row);
            }
#endif // BMCDEBUG

            lastTimestampKey = timestampKey;
            cursor += kG3xWaveformPacketSize;
        }

        waveformFile.close();
    }

    // If no packets were found, emit a single synthetic packet so the session
    // is not empty.
    if (dateSession.Waveforms.isEmpty() && dayEntry->StartTimestamp.isValid()) {
        BmcWaveformPacket fallback = BuildLegacyCompatiblePacket(dayEntry->StartTimestamp, currentValues);
        // Override the half-cmH2O values written by BuildLegacyCompatiblePacket with
        // hundredths of cmH2O to match PressureChannelGain() = 0.01 in the G3X loader.
        fallback.Raw.IPAP = static_cast<qint16>(currentValues.ipapHundredths);
        fallback.Raw.EPAP = static_cast<qint16>(currentValues.epapHundredths);
        dateSession.Waveforms.append(fallback);
    }

    if (firstPressureCmH2O <= 0.0f) {
        firstPressureCmH2O = 4.0f;
    }

#ifdef BMCDEBUG
    // ---- Write diagnostics CSV if requested ----
    if (diagnosticsEnabled && !diagRows.isEmpty()) {
        const QString diagPath = BuildG3xDiagnosticsPath(dateSession.MachineInfo.SerialNumber, aDate);
        if (!WriteG3xDiagnosticsCsv(diagPath, diagRows, aDate,
                                     dayEntry->WaveStartOffset, dayEntry->WaveEndOffset,
                                     dayEntry->EventStartOffset, dayEntry->EventEndOffset)) {
            qWarning() << "BmcG3xData: Failed to write diagnostics CSV:" << diagPath;
        } else {
            qDebug() << "BmcG3xData: Wrote diagnostics CSV:" << diagPath;
        }
    }
#endif // BMCDEBUG

    // ---- Phase 5: Populate machine settings ----

    dateSession.MacineSettings.CPAP_TreatP  = firstPressureCmH2O;
    dateSession.MacineSettings.CPAP_InitialP = firstPressureCmH2O;
    dateSession.MacineSettings.CPAP_ManualP  = firstPressureCmH2O;

    int minPressureHundredths = 0;
    if (IsReasonablePressureHundredths(dayEntry->TsPressureMinHundredths)) {
        minPressureHundredths = dayEntry->TsPressureMinHundredths;
    } else if (IsReasonablePressureHundredths(dayEntry->ItPressureMinHundredths)) {
        minPressureHundredths = dayEntry->ItPressureMinHundredths;
    }

    int maxPressureHundredths = 0;
    if (IsReasonablePressureHundredths(dayEntry->TsPressureMaxHundredths)) {
        maxPressureHundredths = dayEntry->TsPressureMaxHundredths;
    } else if (IsReasonablePressureHundredths(dayEntry->ItPressureMaxHundredths)) {
        maxPressureHundredths = dayEntry->ItPressureMaxHundredths;
    }

    if (minPressureHundredths > 0) {
        dateSession.MacineSettings.APAP_IntialP  = minPressureHundredths / 100.0f;
        dateSession.MacineSettings.APAP_MinAPAP  = minPressureHundredths / 100.0f;
    }
    if (maxPressureHundredths > 0) {
        dateSession.MacineSettings.APAP_MaxAPAP  = maxPressureHundredths / 100.0f;
    }

    dateSession.MacineSettings.Mode =
        (minPressureHundredths > 0 && maxPressureHundredths > minPressureHundredths)
            ? BmcMode::AutoCPAP
            : BmcMode::CPAP;

    // Real settings supersede the inference above.  Prefer this day's own TS block from the
    // IDX — it records what was in force that night — and fall back to the .set file (the
    // device's *current* config) only when the record carried no TS block.
    if (!DecodeTsBlock(dayEntry->TsBlock, dateSession.MacineSettings)) {
        ApplySetFileSettings(dateSession.MacineSettings);
    }

    // ---- Summary debug output ----
    const auto idxToDouble = [](int x100) -> double {
        return (x100 >= 0) ? (static_cast<double>(x100) / 100.0) : -1.0;
    };
#ifdef BMCDEBUG
    qDebug() << "BmcG3xData idx summary day" << aDate.toString(Qt::ISODate)
             << "ahi"          << idxToDouble(dayEntry->ItAhiX100)
             << "ai"           << idxToDouble(dayEntry->ItAiX100)
             << "hi"           << idxToDouble(dayEntry->ItHiX100)
             << "oai"          << idxToDouble(dayEntry->ItOaiX100)
             << "cai"          << idxToDouble(dayEntry->ItCaiX100)
             << "rerai"        << idxToDouble(dayEntry->ItReraIndexX100)
             << "counts_total" << dayEntry->ItEventTotalCount
             << "counts_oa"    << dayEntry->ItEventObstructiveCount
             << "counts_ca"    << dayEntry->ItEventCentralCount
             << "counts_h"     << dayEntry->ItEventHypopneaCount
             << "counts_rera"  << dayEntry->ItEventReraCount
             << "counts_other" << dayEntry->ItEventOtherCount;

    if (!dateSession.Waveforms.isEmpty()) {
        qDebug() << "BmcG3xData waveform summary day" << aDate.toString(Qt::ISODate)
                 << "wave_packets"         << dateSession.Waveforms.size()
                 << "out_of_order_packets" << outOfOrderPacketCount
                 << "evt42_updates"        << evtPressureUpdateCount
                 << "leak_scale"           << leakScaleTenthsPerRaw
                 << "raw_ipap_min_halfcm"  << waveformRawIpapMin
                 << "raw_ipap_max_halfcm"  << waveformRawIpapMax
                 << "mode"                 << static_cast<int>(dateSession.MacineSettings.Mode)
                 << "ts_minmax_hundredths" << minPressureHundredths << maxPressureHundredths;
    }
#endif // BMCDEBUG

    // ---- Phase 6: Split into sessions on forward gaps ≥ 5 seconds ----
    // Only split on forward gaps (machine turned off for ≥ 5 s). Backward jumps
    // due to DST "fall back" are negative here and must not trigger a split.

    BmcSession* currentSession = new BmcSession();
    QDateTime   lastPacketTimestamp;

    for (const BmcWaveformPacket& packet : dateSession.Waveforms) {
        if (lastPacketTimestamp.isValid() &&
            lastPacketTimestamp.secsTo(packet.Timestamp) >= 5 &&
            !currentSession->Waveforms.isEmpty()) {
            currentSession->StartTimestamp = currentSession->Waveforms.first().Timestamp;
            currentSession->EndTimestamp   = currentSession->Waveforms.last().Timestamp.addSecs(1);
            dateSession.Sessions.append(currentSession);
            currentSession = new BmcSession();
        }
        currentSession->Waveforms.append(packet);
        lastPacketTimestamp = packet.Timestamp;
    }

    if (!currentSession->Waveforms.isEmpty()) {
        currentSession->StartTimestamp = currentSession->Waveforms.first().Timestamp;
        currentSession->EndTimestamp   = currentSession->Waveforms.last().Timestamp.addSecs(1);
        dateSession.Sessions.append(currentSession);
    } else {
        delete currentSession;
    }

    // Assign respiratory events to the session whose time window contains them.
    // Event timestamps carry milliseconds (EVT offset 0x1A) but session boundaries are
    // whole seconds taken from waveform packet times, so the window runs to the end of
    // EndTimestamp's second.  Without that an event in the closing second is dropped
    // whenever its millisecond part is non-zero.
    for (BmcSession* s : dateSession.Sessions) {
        const QDateTime windowEnd = s->EndTimestamp.addMSecs(999);
        for (const BmcRespiratoryEvent& evt : dateSession.RespiratoryEvents) {
            if (evt.StartTime >= s->StartTimestamp && evt.StartTime <= windowEnd) {
                s->RespiratoryEvents.append(evt);
            }
        }
        for (const BmcFlowLimitEvent& evt : dateSession.FlowLimitEvents) {
            if (evt.Timestamp >= s->StartTimestamp && evt.Timestamp <= windowEnd) {
                s->FlowLimitEvents.append(evt);
            }
        }
    }

    return dateSession;
}

// ============================================================
// BmcG3xData — private helpers
// ============================================================

const QList<BmcDataLink>& BmcG3xData::GetSessionLinks() const
{
    return sessionLinks;
}

/// @brief Finds and validates the `.idx` file for this recording directory.
///
/// Sets idxFilePath and fileBasePath on success.  Returns false if no valid
/// `.idx` with a matching `.000` companion file is found.
bool BmcG3xData::ResolveIdxFile()
{
    if (!idxFilePath.isEmpty() && QFile::exists(idxFilePath)) {
        return true;
    }

    QDir dir(dirPath);
    if (!dir.exists()) {
        return false;
    }

    const QFileInfoList idxCandidates = dir.entryInfoList(QStringList() << "*.idx", QDir::Files);
    for (const QFileInfo& idxInfo : idxCandidates) {
        QFile idxFile(idxInfo.absoluteFilePath());
        if (!idxFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray header = idxFile.read(32);
        idxFile.close();
        if (!IsG3xIdxHeader(header)) {
            continue;
        }
        const QString candidateBase = idxInfo.absolutePath() + QDir::separator() + idxInfo.completeBaseName();
        if (!QFile::exists(candidateBase + ".000")) {
            continue;
        }
        idxFilePath  = idxInfo.absoluteFilePath();
        fileBasePath = candidateBase;
        return true;
    }
    return false;
}

/// @brief Extracts machine serial number, model name, and firmware version from the IDX file header
///        and the companion .log file.
///
/// IDX header layout (confirmed from binary analysis, 2026-03-26):
///   0x0030 (16 bytes) — serial number (e.g. "A3125636308")
///   0x0048 (16 bytes) — part/config code (e.g. "110A40113"); NOT the product name
///   0x0100 (16 bytes) — product name (e.g. "G3 A20"); the human-readable model
///   0x0345 (20 bytes) — internal SC firmware build string (e.g. "G3-2.SC.72.01")
///
/// .log file (first 64 KB scanned for a "<platform>-<major>.<n>.<n>.<n>" version token):
///   User-facing firmware version, e.g. "G3-2.11.02.33", "G3-2.12.54.13", "E5-1.02.05.02".
///   This matches what PAP-Link and the device display report to the user.
///   The byte offset varies by device: ~0x0420 in small log files (G3 A20, SC.72/SC.74),
///   ~0x1420 in larger ring-buffer log files (G3 B20A, SC.75), 0x1620 on the E5 B25A Plus.
///   For the G3 platform, major version 11 = SC.72; major version 12 = SC.74 / SC.75.
void BmcG3xData::ParseMachineInfo(const QByteArray& idxBytes)
{
    machineInfo.SerialNumber = ReadAscii(idxBytes, 0x30, 16);

    // Product name at 0x100 ("G3 A20"); fall back to part-code at 0x48 if absent.
    machineInfo.Model = ReadAscii(idxBytes, 0x100, 16);
    if (machineInfo.Model.isEmpty()) {
        machineInfo.Model = ReadAscii(idxBytes, 0x48, 16);
    }

    // User-facing firmware version from the .log file.  This matches what PAP-Link and
    // the device display report to the user.
    //
    // The version token is a platform prefix ("G3-2." on Luna G3X, "E5-1." on E5 B25A Plus)
    // followed by dotted numeric fields, e.g. "G3-2.11.02.33", "G3-2.12.55.05",
    // "E5-1.02.05.02".  Earlier revisions of this function searched for the literal prefix
    // "G3-2.", so any non-G3 platform found nothing and silently fell through to the IDX
    // build string below.  Match the general platform pattern instead so new BMC families
    // report their real firmware version.
    //
    // The byte offset varies by device and log-file size: ~0x0420 in small log files
    // (G3 A20), ~0x1420 in larger ring-buffer log files (G3 B20A), and 0x1620 on the
    // E5 B25A Plus sample — close enough to the old 6 KB limit to be fragile, so scan
    // 64 KB.  Verified on the G3 A20 and E5 B25A Plus reference cards: the version token
    // is the only text in the entire log matching this pattern, so the wider scan cannot
    // pick up a false positive.
    const QString logFilePath = fileBasePath + ".log";
    QFile logFile(logFilePath);
    if (logFile.open(QIODevice::ReadOnly)) {
        const QByteArray logData = logFile.read(65536);
        logFile.close();
        // <letter><alphanumeric> '-' <digits> then at least two dot-separated numeric groups.
        static const QRegularExpression kLogVersionPattern(
            QStringLiteral("[A-Za-z][A-Za-z0-9]-[0-9]+(?:\\.[0-9]+){2,}"));
        const QRegularExpressionMatch match =
            kLogVersionPattern.match(QString::fromLatin1(logData));
        if (match.hasMatch()) {
            machineInfo.FirmwareVersion = match.captured(0);
        }
    }
    if (machineInfo.FirmwareVersion.isEmpty()) {
        // Fallback: internal SC build string from IDX 0x0345 (e.g. "G3-2.SC.72.01").
        machineInfo.FirmwareVersion = ReadAscii(idxBytes, 0x0345, 20);
    }

    if (machineInfo.SerialNumber.isEmpty()) {
        machineInfo.SerialNumber = QFileInfo(idxFilePath).completeBaseName();
    }
    if (machineInfo.Model.isEmpty()) {
        machineInfo.Model = QString("Luna G3X");
    }

#ifdef BMCDEBUG
    qDebug() << "BmcG3xData machine info: serial" << machineInfo.SerialNumber
             << "model" << machineInfo.Model
             << "firmware" << machineInfo.FirmwareVersion;
#endif // BMCDEBUG
}

/// @brief Parses the per-day index records from the IDX file.
///
/// Each record is 0x800 bytes, beginning at offset 0x800 in the file.
/// Magic 0xAAAA selects valid records.  Each record carries:
///   - Date (year-1900, month, day at offsets +0x08–+0x0A)
///   - Virtual byte ranges for the waveform, EVT, and log streams
///   - IT (nightly statistics) sub-block at record+0x80 (tag 'IT')
///   - TS (therapy settings) sub-block at record+0x280 (tag 'TS')
void BmcG3xData::ParseIdxRecords(const QByteArray& idxBytes)
{
    sessionLinks.clear();
    dayEntries.clear();

    const auto decodePressureField = [](quint16 rawValue) -> int {
        return (rawValue == 0xFFFF) ? 0 : static_cast<int>(rawValue);
    };
    const auto decodeOptionalU16 = [](quint16 rawValue) -> int {
        return (rawValue == 0xFFFF) ? -1 : static_cast<int>(rawValue);
    };

    int idxScanned = 0, idxNoMagic = 0, idxBadDate = 0, idxNoWave = 0, idxBadTs = 0, idxAccepted = 0;

    for (int offset = kG3xIdxRecordOffset; offset + 0x34 <= idxBytes.size(); offset += kG3xIdxRecordSize) {
        ++idxScanned;
        if (ReadUInt16LE(idxBytes, offset) != 0xAAAA) {
            ++idxNoMagic;
            continue;
        }

        const int year  = 1900 + static_cast<unsigned char>(idxBytes.at(offset + 0x08));
        const int month = static_cast<unsigned char>(idxBytes.at(offset + 0x09));
        const int day   = static_cast<unsigned char>(idxBytes.at(offset + 0x0A));
        if (!QDate::isValid(year, month, day)) {
            ++idxBadDate;
#ifdef BMCDEBUG
            qDebug() << "BmcG3xData IDX: skipping bad date" << year << month << day
                     << "at idx offset" << Qt::hex << offset;
#endif // BMCDEBUG
            continue;
        }

        const quint32 waveStart  = ReadUInt32LE(idxBytes, offset + 0x10);
        const quint32 waveEnd    = ReadUInt32LE(idxBytes, offset + 0x14);
        const quint32 waveLen    = ReadUInt32LE(idxBytes, offset + 0x18);
        const quint32 eventStart = ReadUInt32LE(idxBytes, offset + 0x1C);
        const quint32 eventEnd   = ReadUInt32LE(idxBytes, offset + 0x20);
        const quint32 eventLen   = ReadUInt32LE(idxBytes, offset + 0x24);
        const quint32 logStart   = ReadUInt32LE(idxBytes, offset + 0x28);
        const quint32 logEnd     = ReadUInt32LE(idxBytes, offset + 0x2C);
        const quint32 logLen     = ReadUInt32LE(idxBytes, offset + 0x30);

        G3xDayEntry dayEntry;
        dayEntry.Date             = QDate(year, month, day);
        dayEntry.WaveStartOffset  = waveStart;
        dayEntry.WaveEndOffset    = waveEnd;
        dayEntry.WaveLength       = waveLen;
        dayEntry.EventStartOffset = eventStart;
        dayEntry.EventEndOffset   = eventEnd;
        dayEntry.EventLength      = eventLen;
        dayEntry.LogStartOffset   = logStart;
        dayEntry.LogEndOffset     = logEnd;
        dayEntry.LogLength        = logLen;

        if (waveLen > 0 && waveEnd > waveStart) {
            // Normal path: derive session timestamps from waveform packets.
            const QDateTime startTs = ReadWaveformPacketTimestamp(waveStart);
            const quint32 endPacketOffset = (waveEnd >= kG3xWaveformPacketSize)
                                                ? (waveEnd - kG3xWaveformPacketSize)
                                                : waveStart;
            QDateTime endTs = ReadWaveformPacketTimestamp(endPacketOffset);

            if (!startTs.isValid()) {
                ++idxBadTs;
#ifdef BMCDEBUG
                qDebug() << "BmcG3xData IDX: skipping" << QDate(year, month, day).toString(Qt::ISODate)
                         << "invalid waveform timestamp at waveStart" << Qt::hex << waveStart;
#endif // BMCDEBUG
                continue;
            }
            if (!endTs.isValid() || endTs < startTs) {
                // Fall back to estimating end time from packet count.
                const quint32 packetCount = std::max<quint32>(1, waveLen / kG3xWaveformPacketSize);
                endTs = startTs.addSecs(static_cast<int>(packetCount));
            }
            dayEntry.StartTimestamp = startTs;
            dayEntry.EndTimestamp   = endTs.addSecs(1);
        } else {
            // No waveform data: accept the record if IT block or EVT stream has data.
            const int itOff = offset + 0x80;
            const bool hasItDuration =
                (itOff + 0x18 <= idxBytes.size() &&
                 idxBytes.at(itOff)     == 'I'   &&
                 idxBytes.at(itOff + 1) == 'T'   &&
                 ReadUInt32LE(idxBytes, itOff + 0x14) > 0);
            const bool hasEvtData = (eventLen > 0 && eventEnd > eventStart);
            if (!hasItDuration && !hasEvtData) {
                ++idxNoWave;
#ifdef BMCDEBUG
                qDebug() << "BmcG3xData IDX: skipping" << QDate(year, month, day).toString(Qt::ISODate)
                         << "no waveform, no IT duration, no EVT data"
                         << "evtLen" << eventLen;
#endif // BMCDEBUG
                continue;
            }
            // Use noon of the IDX calendar date as synthetic start.
            dayEntry.StartTimestamp = QDateTime(dayEntry.Date, QTime(12, 0, 0), Qt::LocalTime);
            const quint32 itDuration = hasItDuration
                                           ? ReadUInt32LE(idxBytes, itOff + 0x14)
                                           : 0;
            dayEntry.EndTimestamp = dayEntry.StartTimestamp.addSecs(
                itDuration > 0 ? static_cast<int>(itDuration) : 3600);
        }

        // Parse the IT (nightly statistics) sub-block if present.
        const int itOffset = offset + 0x80;
        if (itOffset + 0xD4 <= idxBytes.size() &&
            idxBytes.at(itOffset)     == 'I' &&
            idxBytes.at(itOffset + 1) == 'T') {
            dayEntry.ItDurationSeconds       = static_cast<int>(ReadUInt32LE(idxBytes, itOffset + 0x14));
            dayEntry.ItSessionCount          = static_cast<unsigned char>(idxBytes.at(itOffset + 0x24));
            dayEntry.ItPressureMinHundredths = decodePressureField(ReadUInt16LE(idxBytes, itOffset + 0x28));
            dayEntry.ItPressureMaxHundredths = decodePressureField(ReadUInt16LE(idxBytes, itOffset + 0x2A));
            dayEntry.ItPressureP95Hundredths = decodePressureField(ReadUInt16LE(idxBytes, itOffset + 0x2C));
            dayEntry.ItPressureEPAPHundredths= decodePressureField(ReadUInt16LE(idxBytes, itOffset + 0x30));
            dayEntry.ItAhiX100               = decodeOptionalU16(ReadUInt16LE(idxBytes, itOffset + 0xBC));
            dayEntry.ItAiX100                = decodeOptionalU16(ReadUInt16LE(idxBytes, itOffset + 0xBE));
            dayEntry.ItHiX100                = decodeOptionalU16(ReadUInt16LE(idxBytes, itOffset + 0xC0));
            dayEntry.ItOaiX100               = decodeOptionalU16(ReadUInt16LE(idxBytes, itOffset + 0xC2));
            dayEntry.ItCaiX100               = decodeOptionalU16(ReadUInt16LE(idxBytes, itOffset + 0xC4));
            dayEntry.ItReraIndexX100         = decodeOptionalU16(ReadUInt16LE(idxBytes, itOffset + 0xC6));
            dayEntry.ItEventTotalCount       = decodeOptionalU16(ReadUInt16LE(idxBytes, itOffset + 0xC8));
            dayEntry.ItEventObstructiveCount = decodeOptionalU16(ReadUInt16LE(idxBytes, itOffset + 0xCA));
            dayEntry.ItEventCentralCount     = decodeOptionalU16(ReadUInt16LE(idxBytes, itOffset + 0xCC));
            dayEntry.ItEventHypopneaCount    = decodeOptionalU16(ReadUInt16LE(idxBytes, itOffset + 0xCE));
            dayEntry.ItEventReraCount        = decodeOptionalU16(ReadUInt16LE(idxBytes, itOffset + 0xD0));
            dayEntry.ItEventOtherCount       = static_cast<qint16>(ReadUInt16LE(idxBytes, itOffset + 0xD2));
        }

        // Parse the TS (therapy settings) sub-block if present.
        const int tsOffset = offset + 0x280;
        if (tsOffset + 0x12 <= idxBytes.size() &&
            idxBytes.at(tsOffset)     == 'T' &&
            idxBytes.at(tsOffset + 1) == 'S') {
            dayEntry.TsPressureMinHundredths = decodePressureField(ReadUInt16LE(idxBytes, tsOffset + 0x0E));
            dayEntry.TsPressureMaxHundredths = decodePressureField(ReadUInt16LE(idxBytes, tsOffset + 0x10));
            // Keep the whole block: it is this night's complete settings record.
            dayEntry.TsBlock = idxBytes.mid(tsOffset, 0x100);
        }

        ++idxAccepted;
#ifdef BMCDEBUG
        qDebug() << "BmcG3xData IDX: accepted" << dayEntry.Date.toString(Qt::ISODate)
                 << "startTs" << dayEntry.StartTimestamp.toString(Qt::ISODate)
                 << "waveStart" << Qt::hex << waveStart << "waveEnd" << Qt::hex << waveEnd
                 << "evtStart"  << Qt::hex << eventStart << "evtEnd" << Qt::hex << eventEnd;
#endif // BMCDEBUG
        dayEntries.append(dayEntry);

        // Build the session link.  Use the IDX calendar date (not the waveform
        // timestamp date) to avoid duplicate imports when recordings cross midnight.
        BmcDataLink link;
        link.UsrSession.StartTimestamp  = QDateTime(dayEntry.Date, QTime(12, 0, 0), Qt::LocalTime);
        link.UsrSession.EndTimestamp    = QDateTime(dayEntry.Date.addDays(1), QTime(12, 0, 0), Qt::LocalTime).addSecs(-1);
        const qint64 durationMinutes    = std::max<qint64>(1,
            dayEntry.StartTimestamp.secsTo(dayEntry.EndTimestamp) / 60);
        link.UsrSession.DurationMinutes = static_cast<int>(durationMinutes);
        sessionLinks.append(link);
    }

#ifdef BMCDEBUG
    qDebug() << "BmcG3xData IDX summary: scanned" << idxScanned
             << "noMagic" << idxNoMagic
             << "badDate"  << idxBadDate
             << "noWave"   << idxNoWave
             << "badTs"    << idxBadTs
             << "accepted" << idxAccepted;
#endif // BMCDEBUG

    std::sort(sessionLinks.begin(), sessionLinks.end(), [](const BmcDataLink& a, const BmcDataLink& b) {
        return a.UsrSession.StartTimestamp < b.UsrSession.StartTimestamp;
    });
    std::sort(dayEntries.begin(), dayEntries.end(), [](const G3xDayEntry& a, const G3xDayEntry& b) {
        return a.StartTimestamp < b.StartTimestamp;
    });
}

/// @brief Reads the timestamp from the waveform packet at @p virtualByteOffset.
///
/// The virtual byte offset spans the entire .00x file series as if they were
/// one contiguous file.  Returns an invalid QDateTime on any error.
/// @brief Decodes a 256-byte BMC "TS" (therapy settings) block into @p settings.
///
/// The same block layout appears in two places and this decoder serves both:
///   - `IDX record + 0x280` — the settings actually in force on that night.  This is what
///     PAP-Link reports from, and what OSCAR prefers.
///   - a block in the `.set` file — the device's *current* configuration, used only as a
///     fallback for days whose IDX record carried no TS block.
///
/// Preferring the per-day copy matters as soon as a setting is ever changed: `.set` holds
/// only today's values, so relying on it relabels every historical night with them.  One
/// reference card has Min EPAP at 8.5 on its early nights and 7.5 now.
///
/// Field map (see Notes/loaders/G3X/BMC_G3X_SET_FORMAT.md).  Contributed by an external
/// reverse engineer and validated 15/15 against a PAP-Link readout, then re-checked here
/// against a second card in a different mode.  Pressures are u16 LE, hundredths of cmH2O.
///
///   0x08 mode (0 CPAP, 1 AutoCPAP, 2 S, 6 AutoS)
///   0x0A Treat P / EPAP        0x0E Min APAP / Min EPAP    0x10 Max APAP
///   0x12 IPAP                  0x16 Max IPAP               0x18 Initial P / Initial EPAP
///   0x1A rise time (ms)        0x1E Ti Min (x0.1s)         0x20 Ti Max (x0.1s)
///   0x2A pressure support      0x30 I Sens                 0x31 E Sens
///   0x32 pressure response     0x34 backup RR              0x36 Smart C/A/B
///   0x82 ramp minutes          0x86 Reslex                 0x87 humidifier
///   0x8A auto flags            0x8C auto on                0x8D auto off
///   0x8E mask type             0xB3 leak alert
///
/// The 0x8A flags byte carries the "Auto" states that have no numeric encoding:
/// bit0 ramp, bit2 humidifier, bit5 I Sens, bit6 E Sens.
///
/// Air tube type is deliberately **not** set: no offset for it is known in this layout, and
/// BmcMachineSettings::AirTubeType defaults to a value that is itself a real option
/// ("Normal 19mm"), so publishing it would present a guess as though it were a reading.
///
/// @param ts       The 256-byte block; must start with the ASCII tag "TS".
/// @param settings Populated in place; untouched if the block is rejected.
/// @return true if @p settings was updated.
bool BmcG3xData::DecodeTsBlock(const QByteArray& ts, BmcMachineSettings& settings)
{
    if (ts.size() < 0x100 || ts.at(0) != 'T' || ts.at(1) != 'S') {
        return false;
    }

    const auto u8 = [&ts](int o) {
        return static_cast<int>(static_cast<unsigned char>(ts.at(o)));
    };
    const auto u16 = [&ts](int o) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(ts.constData() + o);
        return static_cast<int>(p[0] | (p[1] << 8));
    };
    // Accept a pressure only if it is physiologically plausible; anything else means the
    // block is not what we think it is, and the inferred values are the safer choice.
    const auto pressureOk = [](int h) { return h >= 300 && h <= 3500; };

    const int mode  = u8(0x08);
    const int flags = u8(0x8A);
    const auto sens = [](int raw, bool isAuto) {
        return isAuto ? 0 : ((raw >= 1 && raw <= 7) ? raw : -1);
    };
    const auto presResp = [&u8]() {
        return (u8(0x32) >= 1 && u8(0x32) <= 3) ? u8(0x32) : -1;
    };
    const auto riseSecs = [&u16]() {
        return (u16(0x1A) <= 2000) ? u16(0x1A) / 1000.0f : -1.0f;
    };

    switch (mode) {
    case static_cast<int>(BmcMode::CPAP):
        if (!pressureOk(u16(0x0A))) return false;
        settings.Mode         = BmcMode::CPAP;
        settings.CPAP_TreatP  = u16(0x0A) / 100.0f;
        settings.CPAP_ManualP = u16(0x0A) / 100.0f;
        if (pressureOk(u16(0x18))) settings.CPAP_InitialP = u16(0x18) / 100.0f;
        settings.CPAP_SmartC  = u8(0x36) != 0;
        break;

    case static_cast<int>(BmcMode::AutoCPAP):
        if (!pressureOk(u16(0x0E)) || !pressureOk(u16(0x10)) || u16(0x10) < u16(0x0E)) return false;
        settings.Mode         = BmcMode::AutoCPAP;
        settings.APAP_MinAPAP = u16(0x0E) / 100.0f;
        settings.APAP_MaxAPAP = u16(0x10) / 100.0f;
        if (pressureOk(u16(0x18))) settings.APAP_IntialP = u16(0x18) / 100.0f;
        settings.APAP_SmartA  = u8(0x36) != 0;
        settings.PresResponse = presResp();
        break;

    case static_cast<int>(BmcMode::S):
        if (!pressureOk(u16(0x0A)) || !pressureOk(u16(0x12)) || u16(0x12) < u16(0x0A)) return false;
        settings.Mode       = BmcMode::S;
        settings.S_EPAP     = u16(0x0A) / 100.0f;
        settings.S_IPAP     = u16(0x12) / 100.0f;
        if (pressureOk(u16(0x18))) settings.S_InitialEPAP = u16(0x18) / 100.0f;
        settings.S_RiseTime = riseSecs();
        settings.S_ISENS    = sens(u8(0x30), (flags & 0x20) != 0);
        settings.S_ESENS    = sens(u8(0x31), (flags & 0x40) != 0);
        settings.S_TiMin    = u8(0x1E) / 10.0f;
        settings.S_TiMax    = u8(0x20) / 10.0f;
        settings.S_BackupRR = u8(0x34) != 0;
        break;

    case static_cast<int>(BmcMode::AutoS):
        if (!pressureOk(u16(0x0E)) || !pressureOk(u16(0x16)) || u16(0x16) < u16(0x0E)) return false;
        settings.Mode              = BmcMode::AutoS;
        settings.AutoS_MinEPAP     = u16(0x0E) / 100.0f;
        settings.AutoS_MaxIPAP     = u16(0x16) / 100.0f;
        if (pressureOk(u16(0x18))) settings.AutoS_InitialEPAP = u16(0x18) / 100.0f;
        settings.AutoS_PS          = u16(0x2A) / 100.0f;
        // AutoS holds PS fixed while EPAP floats, so minimum IPAP is exactly
        // min EPAP + PS.  Derived: no confirmed field carries it.
        settings.AutoS_MinIPAP     = settings.AutoS_MinEPAP + settings.AutoS_PS;
        settings.AutoS_RiseTime    = riseSecs();
        settings.AutoS_ISENS       = sens(u8(0x30), (flags & 0x20) != 0);
        settings.AutoS_ESENS       = sens(u8(0x31), (flags & 0x40) != 0);
        settings.AutoS_SmartB      = u8(0x36) != 0;
        settings.PresResponse      = presResp();
        break;

    default:
        // ST / T / Titration are not in the confirmed map; leave inferred settings alone.
        return false;
    }

    // ---- Comfort settings, common to every mode ----
    // Ramp: 0xFF is the sentinel bmc_loader.cpp tests for to display "Auto".
    settings.RampTimeMinutes = (flags & 0x01) ? 0xFF : static_cast<quint8>(u8(0x82));
    settings.Reslex          = static_cast<quint8>(u8(0x86));
    // Humidifier option 6 is registered as "Auto" on the BMC_HUMIDIFIER channel.
    settings.HumidifierLevel = (flags & 0x04) ? 6 : static_cast<quint8>(u8(0x87));
    settings.AutoOn          = u8(0x8C) != 0;
    settings.AutoOff         = u8(0x8D) != 0;
    settings.MaskType        = static_cast<BmcMaskType>(u8(0x8E) <= 3 ? u8(0x8E) : 3);
    settings.LeakAlert       = u8(0xB3) != 0;

    // Air tube type (0x8F).  The byte uses the same encoding as BmcAirTubeType and as the
    // legacy BMC IDX field, so it maps across directly:
    //
    //   0 = Unheated 19mm   1 = Unheated 15mm   2 = Heated 19mm   3 = Heated 15mm
    //
    // Values 1 and 2 are confirmed: a bilevel reference card carries 1 on every night and
    // PAP-Link reports "15mm normal" for all of them, and the heated label for 2 was checked
    // against a heated G3.  Value 1 also accounts for the original defect — this field was
    // not being read at all, so AirTubeType kept its zero default and a slim-hose card was
    // reported as the standard one.  Value 3 has not been seen on any card yet.
    //
    // Sizes are BMC's own: it calls the standard hose 19mm (inner diameter) where other
    // vendors call the same physical tube 22mm (its end connectors).  OSCAR reports what
    // the device reports — see GitLab #256.  A contributor's note taken from the device UI
    // lists 15mm as 0 and 19mm as 1, the opposite of what the data shows, so the two values
    // appear transposed there; PAP-Link agreeing with the card on all 19 nights settles it.
    //
    // Value 0 is not directly confirmed but is the only remaining unheated option, and a
    // G3 A20 reference card carries it on 12 of its 172 nights (2 on the other 160).
    const int rawTube = u8(0x8F);
    if (rawTube >= 0 && rawTube <= 3) {
        settings.AirTubeType      = static_cast<BmcAirTubeType>(rawTube);
        settings.AirTubeTypeKnown = true;
    }

    return true;
}

/// @brief Overlays device settings from the `.set` file onto @p settings.
///
/// Fallback only — ReadDateSession prefers this day's own TS block from the IDX, and uses
/// this when the IDX record carried no TS block.  The `.set` file holds the device's
/// *current* configuration, so on a device whose settings have been changed it is wrong for
/// historical nights; that is why it is second choice.
///
/// Layout: 256-byte blocks tagged by their first two ASCII bytes ("SH" header, "SS" system
/// settings, "TS" therapy settings, one per mode).  `SS` byte 0x1C holds the active mode.
/// The file carries a stale snapshot followed by the live per-mode table, so the **last**
/// `TS` block for the active mode is the current one — verified against PAP-Link, which
/// matched the last block and not the first.
///
/// @param settings Populated in place; untouched unless a block was decoded.
/// @return true if @p settings was updated.
bool BmcG3xData::ApplySetFileSettings(BmcMachineSettings& settings) const
{
    constexpr int kBlockSize          = 256;
    constexpr int kSsActiveModeOffset = 0x1C;
    constexpr int kTsModeOffset       = 0x08;

    QFile setFile(fileBasePath + ".set");
    if (!setFile.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray setBytes = setFile.readAll();
    setFile.close();
    if (setBytes.size() < 2 * kBlockSize) {
        return false;
    }

    // Active mode comes from the SS block; fall back to the last TS block's own mode byte
    // if no SS block is present.  Both agreed on every reference card.
    int activeMode = -1;
    for (int offset = 0; offset + kBlockSize <= setBytes.size(); offset += kBlockSize) {
        if (setBytes.at(offset) == 'S' && setBytes.at(offset + 1) == 'S') {
            activeMode = static_cast<unsigned char>(setBytes.at(offset + kSsActiveModeOffset));
            break;
        }
    }

    // Locate the last TS block for the active mode (or, if SS was missing, the last TS
    // block of any mode).
    int chosenBlock = -1;
    for (int offset = 0; offset + kBlockSize <= setBytes.size(); offset += kBlockSize) {
        if (setBytes.at(offset) != 'T' || setBytes.at(offset + 1) != 'S') {
            continue;
        }
        const int blockMode = static_cast<unsigned char>(setBytes.at(offset + kTsModeOffset));
        if (activeMode < 0) {
            chosenBlock = offset;
            activeMode  = blockMode;
        } else if (blockMode == activeMode) {
            chosenBlock = offset;
        }
    }
    if (chosenBlock < 0) {
        return false;
    }

    const bool decoded = DecodeTsBlock(setBytes.mid(chosenBlock, kBlockSize), settings);
    qDebug() << "BmcG3xData: .set fallback settings, active mode" << activeMode
             << (decoded ? "- decoded" : "- rejected, keeping inferred settings");
    return decoded;
}

/// @brief Decides which waveform offset the leak channel is read from, once per device.
///
/// Firmware SC.72 (user version G3-2.11.x) populates 0x52A with unintentional leak.
/// Firmware SC.74+ (user version G3-2.12.x+) leaves 0x52A at zero and uses 0x568.
/// Selection is by firmware version string where recognised, otherwise by sampling
/// packets: if fewer than 10% carry a non-zero 0x52A the field is treated as dead and
/// 0x568 is used instead.
///
/// The sampling is deliberately spread over up to @c kProbeDays days and takes far more
/// packets than the previous implementation, which probed only the first 200 packets of
/// the day currently being read.  That was both too small a sample and the wrong scope:
/// on the E5 B25A Plus reference card the non-zero fraction hovers near the 10% threshold
/// (11.5%–25% on most nights, but 3%–5% on four of nineteen), so the per-day probe
/// selected a *different* field on those four nights.  Because 0x568 carries a ~15.7 L/min
/// pressure-independent baseline while 0x52A sits near zero, the leak graph jumped between
/// two unrelated signals from one night to the next on the same device and mask.
///
/// @return kG3xOffsetLeak (0x52A) or kG3xOffsetAlternateLeak (0x568).
int BmcG3xData::ResolveLeakFieldOffset()
{
    if (leakFieldOffsetCache >= 0) {
        return leakFieldOffsetCache;
    }

    const QString fwVer = ReadMachineInfo().FirmwareVersion;
    if (fwVer.contains(QLatin1String("G3-2.11.")) || fwVer.contains(QLatin1String(".SC.72"))) {
        leakFieldOffsetCache = kG3xOffsetLeak;          // SC.72 / G3-2.11.x
        qDebug() << "BmcG3xData: leak field 0x52A selected by firmware version" << fwVer;
        return leakFieldOffsetCache;
    }
    if (fwVer.startsWith(QLatin1String("G3-2."))) {
        leakFieldOffsetCache = kG3xOffsetAlternateLeak; // G3-2.12.x+ / SC.74+
        qDebug() << "BmcG3xData: leak field 0x568 selected by firmware version" << fwVer;
        return leakFieldOffsetCache;
    }

    // Firmware not recognised (e.g. the E5 platform) — sample the data itself.
    constexpr int kProbeDays            = 5;
    constexpr int kProbePacketsPerDay   = 2000;
    int probeTotal      = 0;
    int probeNonZero52A = 0;
    int daysProbed      = 0;

    for (const G3xDayEntry& entry : dayEntries) {
        if (daysProbed >= kProbeDays) {
            break;
        }
        if (entry.WaveLength == 0) {
            continue;
        }
        const int fileIndex = static_cast<int>(entry.WaveStartOffset / kG3xWaveformFileSpan);
        QFile probeFile(QString("%1.%2").arg(fileBasePath).arg(fileIndex, 3, 10, QLatin1Char('0')));
        if (!probeFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const qint64 startRaw   = entry.WaveStartOffset % kG3xWaveformFileSpan;
        const qint64 startLocal = (startRaw / kG3xWaveformPacketSize) * kG3xWaveformPacketSize;
        probeFile.seek(std::max<qint64>(0, startLocal));

        int packetsThisDay = 0;
        while (packetsThisDay < kProbePacketsPerDay) {
            const QByteArray probePkt = probeFile.read(kG3xWaveformPacketSize);
            if (probePkt.size() < static_cast<int>(kG3xWaveformPacketSize)) {
                break;
            }
            if (static_cast<unsigned char>(probePkt[0]) != 0xAD ||
                static_cast<unsigned char>(probePkt[1]) != 0xAA) {
                continue;
            }
            if (ReadUInt16LEPtr(probePkt.constData(), kG3xOffsetLeak) > 0) {
                ++probeNonZero52A;
            }
            ++packetsThisDay;
            ++probeTotal;
        }
        probeFile.close();
        if (packetsThisDay > 0) {
            ++daysProbed;
        }
    }

    leakFieldOffsetCache = (probeTotal > 0 && probeNonZero52A < probeTotal / 10)
                               ? kG3xOffsetAlternateLeak
                               : kG3xOffsetLeak;

    qDebug() << "BmcG3xData: leak field"
             << (leakFieldOffsetCache == kG3xOffsetLeak ? "0x52A" : "0x568")
             << "selected by probe (firmware" << fwVer << "not recognised) -"
             << probeNonZero52A << "of" << probeTotal << "packets non-zero at 0x52A across"
             << daysProbed << "day(s)";

    return leakFieldOffsetCache;
}

QDateTime BmcG3xData::ReadWaveformPacketTimestamp(quint32 virtualByteOffset) const
{
    const QByteArray header = ReadVirtualWaveformBytes(virtualByteOffset, 10);
    if (header.size() < 10) {
        return QDateTime();
    }
    if (static_cast<unsigned char>(header.at(0)) != 0xAD ||
        static_cast<unsigned char>(header.at(1)) != 0xAA) {
        return QDateTime();
    }

    const int year   = 1900 + static_cast<unsigned char>(header.at(0x04));
    const int month  = static_cast<unsigned char>(header.at(0x05));
    const int day    = static_cast<unsigned char>(header.at(0x06));
    const int hour   = static_cast<unsigned char>(header.at(0x07));
    const int minute = static_cast<unsigned char>(header.at(0x08));
    const int second = static_cast<unsigned char>(header.at(0x09));

    const QDate date(year, month, day);
    const QTime time(hour, minute, second);
    if (!date.isValid() || !time.isValid()) {
        return QDateTime();
    }
    return QDateTime(date, time);
}

/// @brief Reads the timestamp of every valid waveform packet in a virtual byte range.
QList<QDateTime> BmcG3xData::ReadWaveformPacketTimestamps(quint32 startOffset, quint32 endOffset) const
{
    QList<QDateTime> timestamps;
    if (endOffset <= startOffset) {
        return timestamps;
    }

    const int startFileIndex = static_cast<int>(startOffset / kG3xWaveformFileSpan);
    const int endFileIndex   = static_cast<int>((endOffset - 1) / kG3xWaveformFileSpan);

    for (int fileIndex = startFileIndex; fileIndex <= endFileIndex; ++fileIndex) {
        const QString filepath = QString("%1.%2")
                                     .arg(fileBasePath)
                                     .arg(fileIndex, 3, 10, QLatin1Char('0'));
        QFile waveformFile(filepath);
        if (!waveformFile.open(QIODevice::ReadOnly)) {
            continue;
        }

        qint64 localStart = (fileIndex == startFileIndex) ? (startOffset % kG3xWaveformFileSpan) : 0;
        qint64 localEnd   = (fileIndex == endFileIndex)   ? (endOffset   % kG3xWaveformFileSpan) : waveformFile.size();
        if (fileIndex == endFileIndex && localEnd == 0) {
            localEnd = waveformFile.size();
        }
        localStart = std::max<qint64>(0, std::min(localStart, waveformFile.size()));
        localEnd   = std::max<qint64>(0, std::min(localEnd,   waveformFile.size()));
        if (localEnd <= localStart) {
            waveformFile.close();
            continue;
        }

        qint64 cursor = (localStart / kG3xWaveformPacketSize) * kG3xWaveformPacketSize;
        while (cursor + kG3xWaveformPacketSize <= localEnd) {
            waveformFile.seek(cursor);
            const QByteArray packetHeader = waveformFile.read(10);
            if (packetHeader.size() < 10) {
                break;
            }
            if (static_cast<unsigned char>(packetHeader.at(0)) == 0xAD &&
                static_cast<unsigned char>(packetHeader.at(1)) == 0xAA) {
                QDateTime timestamp;
                if (DecodeG3xTimestamp(packetHeader.constData(), 0x04, &timestamp)) {
                    timestamps.append(timestamp);
                }
            }
            cursor += kG3xWaveformPacketSize;
        }
        waveformFile.close();
    }
    return timestamps;
}

/// @brief Reads @p byteCount bytes from the virtual waveform address space
///        starting at @p virtualByteOffset, spanning .00x files as needed.
QByteArray BmcG3xData::ReadVirtualWaveformBytes(quint32 virtualByteOffset, int byteCount) const
{
    QByteArray output;
    output.reserve(byteCount);

    qint64 currentOffset = virtualByteOffset;
    int    remaining     = byteCount;

    while (remaining > 0) {
        const int    fileIndex  = static_cast<int>(currentOffset / kG3xWaveformFileSpan);
        const qint64 fileOffset = currentOffset % kG3xWaveformFileSpan;

        const QString filepath = QString("%1.%2")
                                     .arg(fileBasePath)
                                     .arg(fileIndex, 3, 10, QLatin1Char('0'));
        QFile waveformFile(filepath);
        if (!waveformFile.open(QIODevice::ReadOnly)) {
            break;
        }
        if (!waveformFile.seek(fileOffset)) {
            waveformFile.close();
            break;
        }

        const qint64 maxReadable = waveformFile.size() - fileOffset;
        const int    chunkSize   = static_cast<int>(std::min<qint64>(remaining, maxReadable));
        if (chunkSize <= 0) {
            waveformFile.close();
            break;
        }

        output.append(waveformFile.read(chunkSize));
        waveformFile.close();
        remaining      -= chunkSize;
        currentOffset  += chunkSize;
    }
    return output;
}

/// @brief Returns true when @p headerBytes begins with the G3X IDX file signature.
bool BmcG3xData::IsG3xIdxHeader(const QByteArray& headerBytes)
{
    return headerBytes.startsWith("BMC G/E/P INDEX");
}

/// @brief Reads a null-terminated ASCII string from @p bytes at @p offset.
QString BmcG3xData::ReadAscii(const QByteArray& bytes, int offset, int length)
{
    if (offset < 0 || length <= 0 || offset >= bytes.size()) {
        return QString();
    }
    const int safeLength = std::min(length, static_cast<int>(bytes.size() - offset));
    QByteArray str = bytes.mid(offset, safeLength);
    // BMC pads these fields with either NUL or 0xFF (erased-flash filler).  Truncate at the
    // first of either: 0xFF is not valid in any of the ASCII fields we read here, and
    // QString::fromLatin1 would otherwise render it as U+00FF ("ÿ") and append a run of
    // them to the value — e.g. the IDX build string appearing as "E5-1.SC.00.22.22ÿÿÿ".
    int endIndex = str.size();
    for (int i = 0; i < str.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(str.at(i));
        if (ch == 0x00 || ch == 0xFF) {
            endIndex = i;
            break;
        }
    }
    str.truncate(endIndex);
    return QString::fromLatin1(str).trimmed();
}

/// @brief Reads a little-endian uint16 from @p bytes at @p offset.
///        Returns 0 if the offset would read past the end of the array.
quint16 BmcG3xData::ReadUInt16LE(const QByteArray& bytes, int offset)
{
    if (offset < 0 || offset + 2 > bytes.size()) {
        return 0;
    }
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
    return static_cast<quint16>(p[0] | (p[1] << 8));
}

/// @brief Reads a little-endian uint32 from @p bytes at @p offset.
///        Returns 0 if the offset would read past the end of the array.
quint32 BmcG3xData::ReadUInt32LE(const QByteArray& bytes, int offset)
{
    if (offset < 0 || offset + 4 > bytes.size()) {
        return 0;
    }
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.constData() + offset);
    return static_cast<quint32>(p[0])       |
           (static_cast<quint32>(p[1]) << 8)  |
           (static_cast<quint32>(p[2]) << 16) |
           (static_cast<quint32>(p[3]) << 24);
}
