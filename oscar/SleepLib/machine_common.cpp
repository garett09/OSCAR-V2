/* SleepLib Common Device Stuff
 *
 * Copyright (c) 2019-2026 The OSCAR Team
 * Copyright (c) 2011-2018 Mark Watkins 
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */


#include "machine_common.h"

ChannelID AllAhiChannels  = 0xffff;
ChannelID AllOahiChannels = 0xfffe;
ChannelID AllCahiChannels = 0xfffd;

QVector<ChannelID> ahiChannels;
QVector<ChannelID> oahiChannels;
QVector<ChannelID> cahiChannels;

const QVector<ChannelID> * ahiChannelGroup(ChannelID id)
{
    if (id == AllAhiChannels)  { return &ahiChannels; }
    if (id == AllOahiChannels) { return &oahiChannels; }
    if (id == AllCahiChannels) { return &cahiChannels; }
    return nullptr;
}

ChannelID NoChannel, SESSION_ENABLED, CPAP_SummaryOnly;
ChannelID CPAP_IPAP, CPAP_IPAPLo, CPAP_IPAPHi, CPAP_EPAP, CPAP_EPAPLo, CPAP_EPAPHi, CPAP_Pressure,
          CPAP_PS, CPAP_Mode, CPAP_AHI,
          CPAP_PressureMin, CPAP_PressureMax, CPAP_Ramp, CPAP_RampTime, CPAP_RampPressure, CPAP_Obstructive,
          CPAP_Hypopnea, CPAP_ObstructiveHypopnea, CPAP_CentralHypopnea, CPAP_AllApnea,
          CPAP_ClearAirway, CPAP_Apnea, CPAP_PB, CPAP_CSR, CPAP_LeakFlag, CPAP_ExP, CPAP_NRI, CPAP_VSnore,
          CPAP_VSnore2,
          CPAP_RERA, CPAP_PressurePulse, CPAP_FlowLimit, CPAP_SensAwake, CPAP_FlowRate, CPAP_MaskPressure,
          CPAP_MaskPressureHi,
          CPAP_RespEvent, CPAP_Snore, CPAP_MinuteVent, CPAP_RespRate, CPAP_TidalVolume, CPAP_PTB, CPAP_Leak,
          CPAP_LeakMedian, CPAP_LeakTotal, CPAP_MaxLeak, CPAP_FLG, CPAP_IE, CPAP_Te, CPAP_Ti, CPAP_TgMV,
          CPAP_UserFlag1, CPAP_UserFlag2, CPAP_UserFlag3, /*CPAP_BrokenSummary, CPAP_BrokenWaveform,*/ CPAP_RDI,
          CPAP_PresReliefMode, CPAP_PresReliefLevel, CPAP_PSMin, CPAP_PSMax, CPAP_Test1,
          CPAP_Test2, CPAP_HumidSetting,
          CPAP_PressureSet, CPAP_IPAPSet, CPAP_EPAPSet, CPAP_EEPAP, CPAP_EEPAPLo, CPAP_EEPAPHi
          #if defined(STEADY_BREATHING)
          , CPAP_SteadyBreathing,CPAP_SteadyBreathingFlag
          #endif
          ;



ChannelID RMS9_E01, RMS9_E02, RMS9_SetPressure, RMS9_MaskOnTime;
ChannelID INTELLIPAP_Unknown1, INTELLIPAP_Unknown2, INTP_SnoreFlag;

ChannelID CPAP_LargeLeak,
          PRS1_BND, PRS1_FlexMode, PRS1_FlexLevel, PRS1_HumidStatus, PRS1_HumidLevel, PRS1_HumidTargetTime, PRS1_MaskResistLock,
          PRS1_MaskResistSet, PRS1_HoseDiam, PRS1_AutoOn, PRS1_AutoOff, PRS1_MaskAlert, PRS1_ShowAHI;

//ChannelID SS_SenseAwakeLevel, SS_EPR, SS_EPRLevel, SS_Ramp;

ChannelID OXI_Pulse, OXI_SPO2, OXI_Perf, OXI_PulseChange, OXI_SPO2Drop, OXI_Plethy;

ChannelID Journal_Notes, Journal_Weight, Journal_BMI, Journal_ZombieMeter, LastUpdated,
        Bookmark_Start, Bookmark_End, Bookmark_Notes;


ChannelID SLEEP_Stage, ZEO_ZQ, SLEEP_TimeToSleep, SLEEP_TimeInWake, SLEEP_TimeInREM,
          SLEEP_TimeInLight, SLEEP_TimeInDeep, SLEEP_Awakenings, SLEEP_MorningFeel;

ChannelID AW_RespRate, AW_HRV, AW_BreathingDisturbances, AW_WristTemp;

ChannelID POS_Orientation, POS_Inclination, POS_Movement;

ChannelID BMC_PressureWave, BMC_FlowAbnormality, BMC_IE_Ratio;
