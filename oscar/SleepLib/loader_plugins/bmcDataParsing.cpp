#include "bmcDataParsing.h"
#include <QDataStream>
#include <QFile>
#include <QDir>
#include <QBuffer>

#include <QDebug>
#include <algorithm>


#ifdef DEBUG_BMC
#include <qtextstream.h>
QTextStream qout2(stdout);
#endif



QDateTime BmcEncodedDate::DecodeDate(quint16 encodedDate)
{
    int year = (encodedDate >> 9);
    year += 2000;
    int month = (encodedDate >> 5) & 0x0f;
    int day = encodedDate & 0x1f;
    return QDateTime(QDate(year, month, day), QTime(12, 0, 0), Qt::LocalTime);
}


BmcUsrSession::BmcUsrSession()
{

}

BmcUsrSession::BmcUsrSession(QDataStream* strm, bool InProgressSession) : BmcUsrSession()
{
    if (InProgressSession)
        this->ReadInProgressSession(strm);
    else
        this->ReadHistoricSession(strm);
}


quint32 BmcUsrSession::GetNextHistoricSessionOffset(QDataStream* strm)
{
    const quint64 current = strm->device()->pos();

    strm->skipRawData(1);          // skip 0xE1/0xE2
    quint32 next;  *strm >> next;

    // If next is invalid (0, 0xFFFFFFFF, or before the current position),
    // use end-of-file as a sentinel so the caller reads to EOF.
    const quint64 end   = strm->device()->size();
    if (next == 0            ||
        next == 0xFFFFFFFF   ||
        next <= current      ||
        next >  end)
        next = static_cast<quint32>(end);

    strm->device()->seek(current);
    return next;
}


void BmcUsrSession::ReadInProgressSession(QDataStream* strm)
{
    quint16 tmp16;
    strm->device()->seek(0x431);
    *strm >> tmp16;
    this->StartTimestamp = BmcEncodedDate::DecodeDate(tmp16);
    this->EndTimestamp = QDateTime(this->StartTimestamp.date().addDays(1),
                                   this->StartTimestamp.time(), this->StartTimestamp.timeSpec());
    strm->device()->seek(0x441);

    while (true)
    {
        quint8 msgType;
        *strm >> msgType;

        if (msgType == 0xff) break;

        quint8 datalen = 3;

        if (msgType != 0x02)
        {
            *strm >> datalen;
        }

        //char msgBytes[datalen];
        //strm->readRawData(msgBytes, datalen);

        std::vector<char> msgBytes(datalen);
        strm->readRawData(msgBytes.data(), datalen);

        if (msgType == 0x07 || msgType == 0x08 || msgType == 0x09)
        {
            BmcRespiratoryEvent evt;
            switch (msgType){
            case 0x07: evt.EventType = BmcRespiratoryEventType::CSA; break;
            case 0x08: evt.EventType = BmcRespiratoryEventType::OSA; break;
            case 0x09: evt.EventType = BmcRespiratoryEventType::HYP; break;
            }

            evt.StartTime = this->StartTimestamp.addSecs(60 * 60 * msgBytes[0]).addSecs(60 * msgBytes[1]);
            evt.DurationSeconds = msgBytes[2];
            evt.EndTime = evt.StartTime.addSecs(evt.DurationSeconds);

            this->RespiratoryEvents.append(evt);

        }
    }
}

void BmcUsrSession::ReadHistoricSession(QDataStream* strm)
{
    quint32 tmp32;
    quint16 tmp16;
    quint8 b;

    *strm >> b;
    if (b != 0xE1)
        throw std::invalid_argument("BmcUsrSession: Session header error");

    strm->device()->seek(0x07);

    *strm >> tmp16;
    this->StartTimestamp = BmcEncodedDate::DecodeDate(tmp16);
    this->EndTimestamp = QDateTime(this->StartTimestamp.date().addDays(1),
                                   this->StartTimestamp.time(), this->StartTimestamp.timeSpec());

    strm->device()->seek(0x0f);
    *strm >> tmp16;
    this->DurationMinutes = tmp16;


    strm->device()->seek(0x45);
    while (true)
    {
        *strm >> b;
        *strm >> tmp32;
        if (b == 0xff)
            break;

        MessageItem32 msg(b, tmp32);
        this->MessagesOffset45.append(msg);
    }

    tmp32 = strm->device()->pos();

    while (strm->device()->pos() < strm->device()->size())
    {
        quint8 msgType;
        quint16 count;
        *strm >> msgType;
        *strm >> count;
        *strm >> tmp16; //Discard next byte

        switch (msgType)
        {
        case 0x86:
        case 0x82:
        {
            for (int i = 0; i < count; i++)
            {
                *strm >> tmp32;
                MessageItem32 msg(msgType, tmp32);
                this->DataMessages32.append(msg);
            }
            break;
        }

        case 0x83:
        case 0x84:
        case 0x87:
        {
            for (int i = 0; i < count; i++)
            {
                quint8 b1, b2, b3;
                *strm >> b1;
                *strm >> b2;
                *strm >> b3;
                MessageItem24 msg(msgType, b1, b2, b3);
                this->DataMessages24.append(msg);
            }
            break;
        }

        default:
        {
            for (int i = 0; i < count; i++)
            {
                *strm >> tmp16;
                MessageItem16 msg(msgType, tmp16);
                this->DataMessages16.append(msg);
            }
            break;
        }
        }
    }

    for (auto &msg : this->DataMessages24)
    {
        BmcRespiratoryEvent evt;
        switch (msg.MessageType){
        case 0x83: evt.EventType = BmcRespiratoryEventType::OSA; break;
        case 0x84: evt.EventType = BmcRespiratoryEventType::HYP; break;
        case 0x87: evt.EventType = BmcRespiratoryEventType::CSA; break;
        }

        evt.StartTime = this->StartTimestamp.addSecs(60 * 60 * msg.Data1).addSecs(60 * msg.Data2);
        evt.DurationSeconds = msg.Data3;
        evt.EndTime = evt.StartTime.addSecs(evt.DurationSeconds);

        this->RespiratoryEvents.append(evt);
    }

}


BmcIdxEntry::BmcIdxEntry()
{

}

BmcIdxEntry::BmcIdxEntry(QDataStream* strm) : BmcIdxEntry()
{
    quint16 header;
    *strm >> header; //0x000

    if (header != 0xAAAA)
        throw std::invalid_argument("IDX file header error");

    quint16 idx;
    *strm >> idx; //0x002

    quint8 year, month, day;
    *strm >> year >> month >> day;   //0x004,5,6

    Timestamp = QDateTime(QDate(year + 2000, month, day), QTime(0,0,0));

    strm->skipRawData(6); //0x007

    *strm >> this->StartOffsetPacket; //0x00d
    *strm >> this->StartFileIndex; //0x00f
    *strm >> this->NextOffsetPacket; //0x011
    *strm >> this->NextFileIndex; //0x013

    if (StartFileIndex > 999){
        throw std::invalid_argument("Invalid nnn waveform file index");
    }

    this->HasValidNext = this->NextFileIndex != 0xff;

    header++;

}

size_t BmcIdxEntry::StartOffsetByte()
{
    return this->StartOffsetPacket * 0x100;
}

QString BmcIdxEntry::StartFileExtension()
{
    return QString(".%1").arg(this->StartFileIndex, 3, 10, QLatin1Char('0'));
}

size_t BmcIdxEntry::NextOffsetByte()
{
    return this->NextOffsetPacket * 0x100;
}

QString BmcIdxEntry::NextFileExtension()
{
    return QString(".%1").arg(this->NextFileIndex, 3, 10, QLatin1Char('0'));
}



BmcMachineSettings::BmcMachineSettings()
{

}

BmcMachineSettings::BmcMachineSettings(QDataStream* strm) : BmcMachineSettings()
{
    if (strm->device()->size() < 0x200)
        throw std::invalid_argument("BmcMachineSettings: IDX packet is too short");

    quint16 header;
    *strm >> header; //0x000

    if (header != 0xAAAA)
        throw std::invalid_argument("BmcMachineSettings: IDX file header error");

    quint16 idx;
    *strm >> idx; //0x002

    quint8 year, month, day;
    *strm >> year >> month >> day;   //0x004,5,6

    Timestamp = QDate(year + 2000, month, day);

    strm->device()->seek(0x140);

    quint8 b;

    *strm >> b; //0x140
    this->APAP_IntialP = this->CPAP_InitialP = this->S_InitialEPAP = this->AutoS_InitialEPAP = (float)b / 2.0f;

    *strm >> b; //0x141
    this->CPAP_TreatP = this->APAP_MinAPAP = this->S_EPAP = this->AutoS_MinEPAP = (float)b / 2.0f;

    *strm >> this->RampTimeMinutes;  //142

    strm->skipRawData(1); //143

    *strm >> b; //0x144
    this->CPAP_ManualP = (float)b / 2.0f;

    *strm >> b; //145
    this->S_BackupRR = (b & 0x80) != 0;

    *strm >> this->HumidifierLevel; //146

    *strm >> b; //147
    this->LeakAlert = (b & 0x40) != 0;
    this->AutoOff = (b & 0x02) != 0;
    this->AutoOn = (b & 0x01) != 0;

    *strm >> b; //148
    this->Reslex= b & 0x03;
    float f = (float)(b >> 2) / 2.0f;
    this->S_IPAP = this->S_EPAP + f;
    this->AutoS_MinIPAP = this->AutoS_MinEPAP + f;

    *strm >> b; //149
    this->AutoS_ISENS = this->S_ISENS = 1 + (b & 0x07);
    this->AutoS_ESENS = this->S_ESENS = 1 + ((b >> 3) & 0x07);

    *strm >> b; //14a
    *strm >> b; //14b

    *strm >> b; //0x14c
    this->APAP_MaxAPAP = this->AutoS_MaxIPAP = (float)b / 2.0f;

    *strm >> b; //0x14d
    this->Mode = (BmcMode)(b >> 4);
    this->APAP_Sensitivity = b & 0x0f;

    *strm >> b; //14e

    *strm >> b; //14f
    this->AutoS_RiseTime = this->S_RiseTime = 1 + (b >> 6);

    *strm >> b; //150

    *strm >> b; //151
    this->ReslexPatient = (b & 0x80) != 0;

    *strm >> b; //152
    this->S_TiMin = (float)b / 10.0f;

    *strm >> b; //153
    this->S_TiMax = (float)b / 10.0f;

    strm->skipRawData(0x0c); //154 - 160

    *strm >> b; //160
    this->MaskType = (BmcMaskType)b;

    *strm >> b; //161

    *strm >> b; //162
    this->AirTubeType = (BmcAirTubeType)b;

    *strm >> b; //163

    *strm >> b; //164
    this->HeatedTubeLevel = b;

    *strm >> b; //165
    this->APAP_SmartA = (b & 0x02) != 0;
    this->CPAP_SmartC = (b & 0x01) != 0;
    this->AutoS_SmartB = (b & 0x04) != 0;

}

BmcWaveformPacket::BmcWaveformPacket(char* buffer)
{
    BmcWaveformPacketStruct* packetStruct = (BmcWaveformPacketStruct*)buffer;

    std::fill_n(this->Flow, kBmcExtendedWaveformSamples, 0.0f);
    std::fill_n(this->PressureWave, kBmcExtendedWaveformSamples, 0);
    std::fill_n(this->FlowAbnormality, kBmcExtendedWaveformSamples, 0);
    std::fill_n(this->Raw.Flow, kBmcExtendedWaveformSamples, 0);
    std::fill_n(this->Raw.PressureWave, kBmcExtendedWaveformSamples, 0);
    std::fill_n(this->Raw.FlowAbnormality, kBmcExtendedWaveformSamples, 0);

    this->EPAP = packetStruct->EPAP / 2.0f;
    this->IPAP = packetStruct->IPAP / 2.0f;

    for (int i = 0; i < kBmcLegacyWaveformSamples; i++)
    {
        this->Flow[i] = packetStruct->Flow[i] / 10.0f;
        this->PressureWave[i] = packetStruct->PressureWave[i];
        this->FlowAbnormality[i] = packetStruct->FlowAbnormality[i];
    }

    this->Leak = packetStruct->Leak / 10.f;
    this->TidalVolume = packetStruct->TidalVolume;
    this->MinuteVentilation = packetStruct->MinuteVentilation / 10.0f;
    this->RespiratoryRate = packetStruct->RespiratoryRate;
    this->IERatio = packetStruct->IERatio <= 100 ? 100 - IERatioLookup[packetStruct->IERatio] : 0;
    this->Timestamp = QDateTime(QDate(packetStruct->Year, packetStruct->Month, packetStruct->Day), QTime(packetStruct->Hour, packetStruct->Minute, packetStruct->Second));


    this->Raw.EPAP = packetStruct->EPAP;
    this->Raw.IPAP = packetStruct->IPAP;

    for (int i = 0; i < kBmcLegacyWaveformSamples; i++)
    {
        this->Raw.Flow[i] = packetStruct->Flow[i];
        this->Raw.PressureWave[i] = packetStruct->PressureWave[i];
        this->Raw.FlowAbnormality[i] = packetStruct->FlowAbnormality[i];
    }

    this->Raw.Leak = packetStruct->Leak;
    this->Raw.TidalVolume = packetStruct->TidalVolume;
    // SpO2 (0xCC) and pulse rate (0xCE) are populated by an optional oximeter accessory.
    // Both will be 0 when the accessory is absent or not in use; they are expected to be
    // present or absent together.
    this->Raw.SpO2Pct   = packetStruct->SpO2Pct;
    this->Raw.PulseRate = packetStruct->PulseRate;
    this->Raw.MinuteVentilation = packetStruct->MinuteVentilation;
    this->Raw.RespiratoryRate = packetStruct->RespiratoryRate;
    //this->Raw.IERatioMapped = ((qint16)(packetStruct->IERatio <= 100 ? 100 - IERatioLookup[packetStruct->IERatio] : 0) * 10);
    this->Raw.IERatioMapped = static_cast<qint16>(
                (packetStruct->IERatio <= 100)
                              ? 100 - IERatioLookup[packetStruct->IERatio]
                                  : 0);
    this->Raw.Timestamp = QDateTime(QDate(packetStruct->Year, packetStruct->Month, packetStruct->Day), QTime(packetStruct->Hour, packetStruct->Minute, packetStruct->Second));

}


BmcDateSession::~BmcDateSession()
{
    qDeleteAll(this->Sessions.begin(), this->Sessions.end());
}


BmcData::BmcData() { }

BmcData::BmcData(const QString& path) : BmcData()
{
    QString tmpPath(path);
    if (!tmpPath.endsWith(QDir::separator()))
        tmpPath.append(QDir::separator());

    this->dirPath = tmpPath;
    this->usrFilePath = GetUsrFilePath(tmpPath);
}



bool BmcData::DirectoryHasBmcData(const QString& path)
{
    QString tmpPath(path);
    QDir dir(tmpPath);
    if (!dir.exists(tmpPath))
        throw std::invalid_argument("Path does not exist");

    if (!tmpPath.endsWith(QDir::separator()))
        tmpPath.append(QDir::separator());

    QString usrFile = GetUsrFilePath(tmpPath);
    if (usrFile == nullptr || !QFile::exists(usrFile)){
        return false;
    }

    if (!QFile::exists(ChangeFileExtension(usrFile, ".idx"))){
        return false;
    }

    if (!QFile::exists(ChangeFileExtension(usrFile, ".000"))){
        return false;
    }

    return true;

}


int BmcData::ReadDataCount()
{
    QFile file(this->usrFilePath);
    file.open(QIODevice::ReadOnly);

    file.seek(0x102338);  //Packets start at offset 0x800

    char countBuf[2];
    file.read(countBuf,2);

    file.close();

    uint16_t* count = (uint16_t*)countBuf;
    return *count;
}

void BmcData::ReadData()
{
    this->ReadIdxFile();
    this->ReadAllSessions();
    this->BuildWaveformCrumbs();
    this->FindValidSessions();
}


BmcMachineInfo BmcData::ReadMachineInfo()
{
   BmcMachineInfo info;

   QFile usrFile(this->usrFilePath);
   usrFile.open(QIODevice::ReadOnly);

   char buf[32];

   //SerialNumber
   usrFile.seek(0x2d);
   usrFile.read(buf, 32);
   info.SerialNumber = QString(buf);
   //info.SerialNumber.truncate(info.SerialNumber.indexOf(QChar::Null));
   info.SerialNumber = info.SerialNumber.trimmed();

   //Model Number
   usrFile.seek(0x2296);
   usrFile.read(buf, 32);
   info.Model = QString(buf);
   //info.Model.truncate(info.Model.indexOf(QChar::Null));
   info.Model = info.Model.trimmed();

   usrFile.close();

   return info;
}


BmcDateSession BmcData::ReadDateSession(QDate aDate)
{
    BmcDataLink* foundLink = NULL;
    for (auto &link: this->SessionLinks)
    {
        if (link.UsrSession.StartTimestamp.date() == aDate)
        {
            foundLink = &link;
            break;
        }
    }

    if (foundLink == NULL)
        throw std::invalid_argument("No session with that date could be found");

    BmcDateSession dateSession;
    dateSession.StartTime = foundLink->UsrSession.StartTimestamp;
    dateSession.DurationMinutes = foundLink->UsrSession.DurationMinutes;
    dateSession.MachineInfo = ReadMachineInfo();
    dateSession.RespiratoryEvents = foundLink->UsrSession.RespiratoryEvents;
    dateSession.Waveforms = ReadWaveforms(*foundLink);

    BmcMachineSettings foundSettings;
    bool settingsFound = false;
    for (const auto &msettings : this->AllMachineSettings)
    {
        if (msettings.Timestamp > foundLink->UsrSession.StartTimestamp.date())
        {
            foundSettings = msettings;
            settingsFound = true;
            break;
        }
    }
    if (!settingsFound && !AllMachineSettings.isEmpty())
        foundSettings = AllMachineSettings.last();

    dateSession.MacineSettings = foundSettings;


    //Split the day sessions up by gaps in the waveform longer than 5 seconds
    QDateTime lastPacketTimestamp;
    BmcSession* session = new BmcSession();
    for (auto & packet : dateSession.Waveforms)
    {
        // Only split on forward gaps (machine turned off for ≥5 s). Backward jumps
        // due to DST "fall back" are negative here and must not trigger a split.
        if (lastPacketTimestamp.isValid() && lastPacketTimestamp.secsTo(packet.Timestamp) >= 5
            && !session->Waveforms.isEmpty())
        {
            session->StartTimestamp = session->Waveforms.first().Timestamp;
            session->EndTimestamp = lastPacketTimestamp;
            dateSession.Sessions.append(session);
            session = new BmcSession();
        }

        session->Waveforms.append(packet);
        lastPacketTimestamp = packet.Timestamp;
    }

    if (lastPacketTimestamp.isValid() && !session->Waveforms.isEmpty()) {
        session->StartTimestamp = session->Waveforms.first().Timestamp;
        session->EndTimestamp = lastPacketTimestamp;
        dateSession.Sessions.append(session);
    } else {
        delete session;
    }

    //Get all the respiratory events that fall in this session
    for (auto session : dateSession.Sessions)
    {
        for (auto respEvent : dateSession.RespiratoryEvents)
        {
            if (respEvent.StartTime >= session->StartTimestamp && respEvent.StartTime <= session->EndTimestamp)
            {
                session->RespiratoryEvents.append(respEvent);
            }
        }
    }

    // If no waveform data is available, create an EVT-only session from the USR summary.
    // This preserves respiratory event data for nights where the circular waveform buffer
    // has been overwritten.
    if (dateSession.Sessions.isEmpty() && foundLink->UsrSession.DurationMinutes > 0) {
        BmcSession* evtOnly = new BmcSession();
        if (!dateSession.RespiratoryEvents.isEmpty()) {
            evtOnly->StartTimestamp = dateSession.RespiratoryEvents.first().StartTime;
            evtOnly->EndTimestamp   = dateSession.RespiratoryEvents.last().EndTime;
            const QDateTime minEnd  = evtOnly->StartTimestamp.addSecs(
                static_cast<qint64>(foundLink->UsrSession.DurationMinutes) * 60);
            if (evtOnly->EndTimestamp < minEnd)
                evtOnly->EndTimestamp = minEnd;
            evtOnly->RespiratoryEvents = dateSession.RespiratoryEvents;
        } else {
            evtOnly->StartTimestamp = foundLink->UsrSession.StartTimestamp;
            evtOnly->EndTimestamp   = foundLink->UsrSession.StartTimestamp.addSecs(
                static_cast<qint64>(foundLink->UsrSession.DurationMinutes) * 60);
        }
        dateSession.Sessions.append(evtOnly);
    }

    return dateSession;
}

const QList<BmcDataLink>& BmcData::GetSessionLinks() const
{
    return this->SessionLinks;
}


QString BmcData::ChangeFileExtension(const QString& path, QString newExtensionWithDot)
{
    QFileInfo finfo(path);
    QString newName = finfo.path() + QDir::separator() + finfo.completeBaseName() + newExtensionWithDot;
    return newName;
}

QString BmcData::GetUsrFilePath(const QString& path)
{
    QDir dir(path);
    QStringList nameFilters;
    nameFilters << "*.USR";

    QString usrFile;

    auto usrFiles = dir.entryList(nameFilters);
    if (usrFiles.length() == 1){
        usrFile = path + usrFiles[0];
        return usrFile;
    } else {
        return nullptr;
    }
}


int BmcData::DetectFileDataOffset(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() < 0x101)
        return 0;
    f.seek(0xFF);
    const QByteArray magic = f.read(2);
    f.close();
    if (magic.size() < 2)
        return 0;
    // A 255-byte legacy tail packet ends with Terminator=0xAA at byte 0xFF; the
    // following 256-byte data packet then starts with header 0xAAAA at bytes 0xFF–0x100.
    return ((quint8)magic[0] == 0xAA && (quint8)magic[1] == 0xAA) ? 0xFF : 0;
}


QDateTime BmcData::ReadWaveformPacketTimestamp(const QString& path, quint64 packetStartByte)
{
    quint64 byteOffset = packetStartByte + 0xf8;

    if (!QFile::exists(path))
        return QDateTime();

    QFile file(path);

    if ((quint64)file.size() < byteOffset)
        return QDateTime();

    file.open(QIODevice::ReadOnly);
    if (!file.isOpen()){
        throw std::invalid_argument("Waveform file could be opened");
    }

    file.seek(byteOffset); //Offset in file of packet + offset of timestamp

    QDataStream strmPacket(&file);
    strmPacket.setByteOrder(QDataStream::LittleEndian);

    quint16 year;
    quint8 month, day, hour, minute, second;

    strmPacket >> year;
    strmPacket >> month;
    strmPacket >> day;
    strmPacket >> hour;
    strmPacket >> minute;
    strmPacket >> second;

    file.close();

    return QDateTime(QDate(year, month, day), QTime(hour, minute, second));
}


void BmcData::ReadIdxFile()
{
    QString idxPath = ChangeFileExtension(this->usrFilePath, ".idx");
    QFile file(idxPath);

    file.open(QIODevice::ReadOnly);

    file.seek(0x800);  //Packets start at offset 0x800

    //For each packet, we :
    //  * Read the 512 byte packet into memory
    //  * Read each packet for idx entry and machine settings
    //This results in less random-access seeking on a slow SD card

    while (file.pos() < file.size())
    {
        auto arr = file.read(512);
        QBuffer buf(&arr);
        buf.open(QIODevice::ReadOnly);
        QDataStream strmPacket(&buf);
        strmPacket.setByteOrder(QDataStream::LittleEndian);

        BmcIdxEntry entry;
        try {
            entry = BmcIdxEntry(&strmPacket);
        } catch (const std::invalid_argument &e) {
            qDebug() << "ReadIdxFile: stopping at invalid entry:" << e.what();
            break;
        }
        this->AllIdxEntries.append(entry);

        //Go back to the start of the packet and read the machine settings in it
        buf.seek(0);
        BmcMachineSettings settings(&strmPacket);
        this->AllMachineSettings.append(settings);
    }

    file.close();
}

void BmcData::ReadAllSessions()
{
    QFile fileUSR(this->usrFilePath);
    fileUSR.open(QIODevice::ReadOnly);

    QDataStream strmUSR(&fileUSR);
    strmUSR.setByteOrder(QDataStream::LittleEndian);

    BmcUsrSession inProgressSession(&strmUSR, true);

    const quint64 sessionsStart = 0x102340;
    fileUSR.seek(sessionsStart);
    //For each session, we determine the length of the session data, copy
    // the data to memory and then parse it from memory to save on file seeking.

    while (fileUSR.pos() < fileUSR.size())
    {
        quint64 here      = fileUSR.pos();
        quint32 next      = BmcUsrSession::GetNextHistoricSessionOffset(&strmUSR);
        quint64 sliceEnd  = qMin<quint64>(next, fileUSR.size());

        if (sliceEnd <= here)         // extra safety: nothing to read
            break;

        const quint32 len = static_cast<quint32>(sliceEnd - here);
        QByteArray raw   = fileUSR.read(len);

        QBuffer  buf(&raw);  buf.open(QIODevice::ReadOnly);
        QDataStream strm(&buf);  strm.setByteOrder(QDataStream::LittleEndian);

        try {
            BmcUsrSession s(&strm, /*InProgress*/false);
            this->AllUsrSessions.append(s);
        } catch (...) {
            qDebug() << "Corrupt session record at offset" << here;
        }
    }

    this->AllUsrSessions.append(inProgressSession);

    fileUSR.close();

}

void BmcData::BuildWaveformCrumbs()
{
    int i = 0;
    QString extension = QString(".%1").arg(i, 3, 10, QLatin1Char('0'));
    QString filepath = ChangeFileExtension(this->usrFilePath, extension);

    while (QFile::exists(filepath))
    {
        // Some files begin with a 255-byte legacy tail packet before the current
        // 256-byte data packets.  Detect and account for this 1-byte offset so
        // that crumb timestamps are read from correctly-aligned packet positions.
        const int dataOffset = DetectFileDataOffset(filepath);

        for (int j = 0; j < 16; j++)
            {
            quint64 byteOffset = static_cast<quint64>(dataOffset) + static_cast<quint64>(j) * 0x1000 * 0x100;

            QDateTime timestamp = ReadWaveformPacketTimestamp(filepath, byteOffset);

            if (!timestamp.isNull() && timestamp.isValid()){
                BmcWaveformCrumb crumb;
                crumb.Filepath = filepath;
                crumb.FileIndex = i;
                crumb.PacketOffset = static_cast<quint16>(byteOffset >> 8);
                crumb.ByteOffset = byteOffset;
                crumb.Timestamp = timestamp;
                this->WaveformCrumbs.append(crumb);
                qDebug() << "Crumb" << crumb.Timestamp.toString(Qt::ISODate) << crumb.FileIndex << crumb.ByteOffset;

            }
        }

        i++;
        extension = QString(".%1").arg(i, 3, 10, QLatin1Char('0'));
        filepath = ChangeFileExtension(this->usrFilePath, extension);
    };


    std::sort(this->WaveformCrumbs.begin(), this->WaveformCrumbs.end(), [](BmcWaveformCrumb a, BmcWaveformCrumb b)->bool{
        return a.Timestamp < b.Timestamp;
    });

}


QList<BmcWaveformPacket> BmcData::ReadWaveforms(BmcDataLink& link)
{
    QList<BmcWaveformPacket> waveforms;
    QDateTime lastPacketTimestamp(QDate(2000, 1, 1), QTime(0, 0, 0));

    if (!QFile::exists(link.WaveformCrumb.Filepath))
        return waveforms;

    char packetBuf[256];

    int currentFileIndex = link.WaveformCrumb.FileIndex;
    QFile* nnnFile = new QFile(link.WaveformCrumb.Filepath);
    nnnFile->open(QIODevice::ReadOnly);
    nnnFile->seek(link.WaveformCrumb.ByteOffset);

    bool complete = false;

    while (!complete)
    {
        while (nnnFile->pos() < nnnFile->size())
        {
            nnnFile->read(packetBuf, 0x100);
#ifdef DEBUG_BMC
            qout2 << currentFileIndex << "   ";
            qout2 << QString("%1").arg((double)(nnnFile->pos()) / nnnFile->size() * 100);
            qout2 << "\n";
            qout2.flush();
#endif
            BmcWaveformPacket packet(packetBuf);

            if (packet.Timestamp >= link.UsrSession.StartTimestamp &&
                packet.Timestamp <= link.UsrSession.EndTimestamp)
                waveforms.append(packet);

            // Allow backward timestamp jumps of up to 2 hours to handle DST "fall back"
            // (clocks go back 1 hour). Larger backward jumps indicate stale circular
            // buffer data from a previous recording cycle.
            const qint64 kMaxAllowedBackwardSecs = 7200LL;
            if (packet.Timestamp > link.UsrSession.EndTimestamp ||
                packet.Timestamp < lastPacketTimestamp.addSecs(-kMaxAllowedBackwardSecs)){
                complete = true;
                break;
            }

            lastPacketTimestamp = packet.Timestamp;
        }
        nnnFile->close();

        currentFileIndex++;
        QString nextExtension = QString(".%1").arg(currentFileIndex, 3, 10, QLatin1Char('0'));
        QString nextPath = ChangeFileExtension(this->usrFilePath, nextExtension);

        if (!QFile::exists(nextPath)){
            currentFileIndex = 0;
            nextExtension = QString(".%1").arg(currentFileIndex, 3, 10, QLatin1Char('0'));
            nextPath = ChangeFileExtension(this->usrFilePath, nextExtension);
            if (!QFile::exists(nextPath)){
                complete = true;
                break;
            }
        }

        delete(nnnFile);

        if (!complete)
        {
            nnnFile = new QFile(nextPath);
            nnnFile->open(QIODevice::ReadOnly);
            // Skip the 255-byte legacy tail packet if present so subsequent
            // 256-byte reads are correctly aligned to the current data region.
            const int nextDataOffset = DetectFileDataOffset(nextPath);
            if (nextDataOffset > 0)
                nnnFile->seek(nextDataOffset);
        }
    }

    return waveforms;

}


void BmcData::FindValidSessions()
{
    //Since .nnn waveform data is overwritten cyclicly, we need to check
    //whether the packet the IDX entry points to in .nnn matches the date
    //of the idx entry. If not, we found the point where the data overwrite
    //tail is.

    QListIterator<BmcIdxEntry> itr(this->AllIdxEntries);
    itr.toBack();

    while (itr.hasPrevious())
    {
        BmcIdxEntry entry = itr.previous();
        QString startPath = ChangeFileExtension(this->usrFilePath, entry.StartFileExtension());
        QString nextPath = ChangeFileExtension(this->usrFilePath, entry.NextFileExtension());

        entry.StartWaveformPacketTimestamp = ReadWaveformPacketTimestamp(startPath, static_cast<quint64>(entry.StartOffsetPacket) * 0x100);


        auto daysDifference = qAbs(entry.Timestamp.daysTo(entry.StartWaveformPacketTimestamp));

        if (daysDifference >= 3)
            continue;


        this->ValidIdxEntries.insert(0, entry);
    }

    //Next we iterate all the USR sessions and see if there is a valid IDX session

    for (auto &usrSession : this->AllUsrSessions)
    {
        BmcIdxEntry* foundIdxEntry = nullptr;

        for (auto &idxEntry : this->ValidIdxEntries)
        {
            const bool sameDay           = usrSession.StartTimestamp.date() == idxEntry.StartWaveformPacketTimestamp.date();
            const bool nextDay           = usrSession.StartTimestamp.date().addDays(1) == idxEntry.StartWaveformPacketTimestamp.date();

            if (  usrSession.StartTimestamp >= idxEntry.StartWaveformPacketTimestamp
                || sameDay
                || nextDay)
            {
                foundIdxEntry = &idxEntry;
            }
            else
                break;
        }

        // Pick the last crumb whose timestamp is strictly before the session start.
        // ReadWaveforms reads forward from the crumb and filters to packets within
        // the session window, so starting from just before the session is correct.
        // (Picking the first crumb >= StartTimestamp would skip the opening minutes.)
        BmcWaveformCrumb chosenCrumb;
        for (const auto &crumb : this->WaveformCrumbs) {
            if (crumb.Timestamp < usrSession.StartTimestamp)
                chosenCrumb = crumb;
            else
                break;
        }
        // If no crumb precedes the session's noon start (e.g. the user went to bed
        // after noon on the same calendar day), fall back to the first crumb within
        // this OSCAR day (noon → next noon).
        if (!chosenCrumb.Timestamp.isValid()) {
            const QDateTime oscarDayEnd(usrSession.StartTimestamp.date().addDays(1), QTime(12, 0, 0));
            for (const auto &crumb : this->WaveformCrumbs) {
                if (crumb.Timestamp >= usrSession.StartTimestamp &&
                    crumb.Timestamp < oscarDayEnd) {
                    chosenCrumb = crumb;
                    break;
                }
            }
        }
        // Sessions with no waveform crumb are still linked for EVT-only import.

        BmcDataLink link;
        link.UsrSession   = usrSession;
        link.WaveformCrumb= chosenCrumb;
        if (foundIdxEntry)
            link.IdxEntry = *foundIdxEntry;

        this->SessionLinks.append(link);
    }
}

