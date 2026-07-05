/* Compliance Report Exporter Source
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef COMPLIANCEREPORT_INTERNAL_H
#define COMPLIANCEREPORT_INTERNAL_H

#include <QColor>
#include <QDate>
#include <QList>
#include <QMap>
#include <QString>
#include <QVector>
#include "SleepLib/profiles.h"
#include "SleepLib/day.h"
#include "SleepLib/common.h"

// Single-pass collector for the dropout-filtered oximetry statistics. The four
// quantities the report needs — min, max, time-weighted mean, and seconds spent
// below a threshold — all derive from one walk over the same raw event samples,
// so computing them together avoids re-opening and re-walking the same sessions
// once per statistic (the oximetry page alone used to make six full passes over
// the same SpO2/Pulse data). Every value at or below floorVal is treated as
// sensor-off dropout and excluded from min/max/wavg.
//
//  - min / max:      lowest / highest sample above floorVal. `found` is false
//                    when no non-dropout sample exists (min/max then read 0.0,
//                    matching the old helpers).
//  - wavg:           each sample weighted by the interval to the next sample,
//                    mirroring Profile::calcWavg for evenly-sampled oximetry; the
//                    trailing sample of each EventList carries no interval and is
//                    omitted, as before.
//  - timeBelowSecs:  gap-integrated time the signal sits in (floorVal, threshold],
//                    mirroring Session::timeBelowThreshold but discarding the
//                    probe-loss 0% spans that would otherwise inflate T90.
struct FilteredStats {
    bool   found = false;
    double min = 0.0;
    double max = 0.0;
    double wavg = 0.0;
    double timeBelowSecs = 0.0;
};

struct SettingsChangeBlock {
    QDate start;
    QDate end;
    int days = 0;
    double hours = 0.0;
    double ahiEvents = 0.0;
    QString deviceStr;
    QString relief;
    QString mode;
    QString pressure;
    double minSpo2 = -1.0;  // < 0 means no oximetry data for this block
    double p95Leak = 0.0;
    double deltaAhiVsBase = 0.0;
    // AirView V2 therapy-history extras
    double avgUseHrs = 0.0;      // hours / days (avg use per night in the block)
    int    compliantNights = 0;  // nights >= compliance threshold within the block
    double cai = 0.0;            // central apnea index for the block (centrals / hours)
    double centralEvents = 0.0;  // accumulator for cai
    QString pressureShort;       // compact set pressure ("9.0–12.0" auto / "10.6" fixed)
};

struct StatsRow {
    QString label;
    QString value;
    QString color;      // empty = default (label grey #64748b, value dark #0f172a)
    bool bold = false;  // true = label is also dark #0f172a and bold
    bool boldValue = false; // true = value is bold
};

// Per-night oximetry summary used to pick the single "worst" night for the
// Lowest-Oxygen Night trace. Pure data — no Qt rendering.
struct NightO2Stat {
    QDate   date;
    double  nadir;     // lowest valid SpO2 that night (artifact-floored)
    double  t90Secs;   // seconds below 90% that night
    bool    valid;     // true only if the night had usable oximetry data
};

// SpO2 time-in-zone percentages for the Overnight Oxygen Summary, derived from
// cumulative "time above threshold" seconds. totalSec = total recording seconds;
// secAbove95/secAbove90 = cumulative seconds with SpO2 above each threshold.
struct Spo2ZonePercents { double pctGe95; double pct90to95; double pctLt90; };

struct SleepStageStats {            // [GATED] populated later; default = absent
    bool present = false;
    double deepPct = 0, lightPct = 0, remPct = 0, awakePct = 0;
    double deepMin = 0, lightMin = 0, remMin = 0, awakeMin = 0, tstMin = 0;
    QString avgBedtime, avgWake;
    double sleepLatencyMin = 0, remOfTstPct = 0;
    int sleepStageNights = 0;
};

// All numeric/string statistics the compliance report needs, gathered once by
// ComplianceReport::gatherReportData() so the Classic renderer and the future
// AirView V2 renderer share byte-identical numbers. Fields tagged [DERIVED-later]
// / [GATED-later] / sleep are declared now but populated in a later task; they
// stay at their defaults here.
struct ReportData {
    QDate start, end;
    int totalDays = 0, presentDays = 0, daysUsed = 0, daysNotUsed = 0, daysNoData = 0;
    QMap<QDate,Day*> daysMap, oxiDaysMap; QList<Day*> daysList, allDays;
    int compliantDays = 0; double pctCompliant = 0, pctUsed = 0, complianceThreshold = 0;
    double totalHours = 0, avgHoursUsed = 0, avgHoursTotal = 0, medianHoursUsed = 0;
    double q1HoursUsed = 0, q3HoursUsed = 0;                       // [DERIVED-later]
    double AHI = 0, AI = 0, HI = 0, CentralAI = 0, ObstructiveAI = 0, UnknownAI = 0, ReraIndex = 0, RDI = 0;
    double avgCsrPctPerNight = 0, totalCsrMinutes = 0; QString timeInCsrStr, timeInApneaStr;
    double untreatedAHI = 0, baselinePctChange = 0; bool hasBaseline = false;   // [GATED-later]
    ChannelID leakChan = NoChannel; bool isTotalLeak = false;
    double medianLeak = 0, p95Leak = 0, maxLeak = 0, leakRedline = 0, pctOverRedline = 0; QString timeAboveRedlineStr;
    bool hasSpO2 = false, hasPulse = false; int usedDaysOxi = 0, usedDaysPulse = 0, usedDaysOxiAny = 0;
    int oxiNotUsedDays = 0; double oxiRecordingHrsAvg = 0;          // [DERIVED-later]
    double minSpO2 = 0, avgSpO2Period = 0, t90Secs = 0, t88Secs = 0, spo2RecordingSecs = 0; bool minSpO2Set = false;
    double odi = 0, dropPct = 0; Spo2ZonePercents spo2Zones {0,0,0};
    double pctTimeInDesat = 0;                                      // [DERIVED-later]
    double minPulsePeriod = 0, maxPulsePeriod = 0, avgPulsePeriod = 0; QString t90Str;
    double pulseChangeIndex = 0;                                    // [DERIVED-later]
    MachineType oxiSourceType = MT_CPAP;
    double p95FlowLimit = 0, flIndex = 0;
    QString brand, model, serial, modeStr, pressureSettings, relief, pressDisplay;
    QString humidLevel, rampTime, rampPres, maskDescription;
    Day* lastDay = nullptr; CPAPMode p1Mode = MODE_UNKNOWN; bool p1IsCpap = false; int age = -1; QDate dob; QString patientName;
    double avgPressure = 0, p95Pressure = 0, maxPressure = 0;
    QList<SettingsChangeBlock> settingsBlocks;
    SleepStageStats sleep;
};

// ---- Function declarations (definitions in compliancereport.cpp) ----

QColor colorForChannel(ChannelID chanId);
QString unitForChannel(ChannelID chanId);
QString getSettingStr(Day* day, ChannelID cid);
FilteredStats calcFilteredStats(ChannelID code, MachineType mt,
                                const QDate& start, const QDate& end,
                                double floorVal, double belowThreshold,
                                double maxFallPerSec = 0.0);
FilteredStats calcDayFilteredStats(Day* day, ChannelID code, MachineType mt,
                                   double floorVal, double belowThreshold,
                                   double maxFallPerSec = 0.0);
double calcDayAvgPercentile(ChannelID chan, EventDataType pct,
                             const QMap<QDate, Day*>& daysMap,
                             MachineType mt = MT_CPAP);
double calcDayAvgMax(ChannelID chan, const QMap<QDate, Day*>& daysMap,
                     MachineType mt = MT_CPAP);
Spo2ZonePercents spo2ZonePercents(double totalSec, double secAbove95, double secAbove90);
QString renderLowestOxygenNight(Day* day, double nadir, const QString& title);
QList<SettingsChangeBlock> getSettingsChanges(const QDate& start, const QDate& end,
                                              const QList<Day*>& daysList);

#endif // COMPLIANCEREPORT_INTERNAL_H
