#ifndef BMCG3XDATAPARSING_H
#define BMCG3XDATAPARSING_H

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QString>

#include "SleepLib/loader_plugins/bmcDataParsing.h"

class BmcG3xData : public BmcDataParser
{
public:
    BmcG3xData();
    explicit BmcG3xData(const QString& path);

    static bool DirectoryHasBmcG3xData(const QString& path);

    void ReadData() override;
    BmcMachineInfo ReadMachineInfo() override;
    BmcDateSession ReadDateSession(QDate aDate) override;
    const QList<BmcDataLink>& GetSessionLinks() const override;

private:
    struct G3xDayEntry
    {
        QDate Date;
        QDateTime StartTimestamp;
        QDateTime EndTimestamp;
        quint32 WaveStartOffset = 0;
        quint32 WaveEndOffset = 0;
        quint32 WaveLength = 0;
        quint32 EventStartOffset = 0;
        quint32 EventEndOffset = 0;
        quint32 EventLength = 0;
        quint32 LogStartOffset = 0;
        quint32 LogEndOffset = 0;
        quint32 LogLength = 0;

        int ItDurationSeconds = 0;
        int ItSessionCount = 0;
        int ItPressureMinHundredths = 0;
        int ItPressureMaxHundredths = 0;
        int ItPressureP95Hundredths = 0;
        int ItPressureEPAPHundredths = 0;
        int ItAhiX100 = 0;
        int ItAiX100 = 0;
        int ItHiX100 = 0;
        int ItOaiX100 = 0;
        int ItCaiX100 = 0;
        int ItReraIndexX100 = 0;
        int ItEventTotalCount = 0;
        int ItEventObstructiveCount = 0;
        int ItEventCentralCount = 0;
        int ItEventHypopneaCount = 0;
        int ItEventReraCount = 0;
        int ItEventOtherCount = 0;

        int TsPressureMinHundredths = 0;
        int TsPressureMaxHundredths = 0;

        /// Raw 256-byte TS (therapy settings) block from this day's IDX record, captured
        /// at parse time.  This is the authoritative per-night settings record — it is
        /// what PAP-Link reports from, and unlike the `.set` file it reflects the settings
        /// actually in force on this date rather than the device's current configuration.
        /// Empty when the record carried no TS block.
        QByteArray TsBlock;
    };

    QString dirPath;
    QString idxFilePath;
    QString fileBasePath;

    QList<BmcDataLink> sessionLinks;
    QList<G3xDayEntry> dayEntries;
    BmcMachineInfo machineInfo;

    /// Waveform offset the leak channel is read from (0x52A or 0x568).  Which field a
    /// device populates is a property of its firmware, not of any individual night, so
    /// this is resolved once per device and reused for every day of the import.
    /// -1 means "not yet resolved".
    int leakFieldOffsetCache = -1;

    int ResolveLeakFieldOffset();

    static bool DecodeTsBlock(const QByteArray& ts, BmcMachineSettings& settings);
    bool ApplySetFileSettings(BmcMachineSettings& settings) const;

    bool ResolveIdxFile();
    void ParseMachineInfo(const QByteArray& idxBytes);
    void ParseIdxRecords(const QByteArray& idxBytes);

    QDateTime ReadWaveformPacketTimestamp(quint32 virtualByteOffset) const;
    QList<QDateTime> ReadWaveformPacketTimestamps(quint32 startOffset, quint32 endOffset) const;
    QByteArray ReadVirtualWaveformBytes(quint32 virtualByteOffset, int byteCount) const;

    static bool IsG3xIdxHeader(const QByteArray& headerBytes);
    static QString ReadAscii(const QByteArray& bytes, int offset, int length);
    static quint16 ReadUInt16LE(const QByteArray& bytes, int offset);
    static quint32 ReadUInt32LE(const QByteArray& bytes, int offset);
};

#endif // BMCG3XDATAPARSING_H
