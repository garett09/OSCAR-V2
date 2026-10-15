#ifndef YUWELL_LOADER_H
#define YUWELL_LOADER_H

#include "SleepLib/machine.h"
#include "SleepLib/machine_loader.h"

const int yuwell_data_version = 1;
const QString yuwell_class_name = "YuwellLoader";
const unsigned char YUWELL_CPAP = 0x00;
const unsigned char YUWELL_APAP = 0x01;

const unsigned char YUWELL_FORMATC_CPAP = 0x00;
const unsigned char YUWELL_FORMATC_S = 0x01;
const unsigned char YUWELL_FORMATC_T = 0x02;
const unsigned char YUWELL_FORMATC_ST = 0x03;
const unsigned char YUWELL_FORMATC_VGPS = 0x04;
const unsigned char YUWELL_FORMATC_AUTOS = 0x05;
const unsigned char YUWELL_FORMATC_APAP = 0x06;

struct FormatBSessionSummary {
  QDateTime start;
  QDateTime finish;
  unsigned char mode;
  unsigned char ramp;
  unsigned char initial_pressure;
  unsigned char pressure_setting;
  unsigned char maximum_pressure;
  unsigned char minimum_pressure;
  unsigned char humidity;
  unsigned char fps_level;
  unsigned char oai_count;
  unsigned char hi_count;
  unsigned char avg_leak_vol;
  unsigned char avg_pressure;
  short unsigned int offset;
  quint16 session_minutes;  // bytes 28-29 of the 30-byte session summary record, BE u16
};

class YuwellLoader;

class YuwellFormat
{
  public:
    YuwellFormat(YuwellLoader &loader, const QString &filePath) : m_loader(&loader), m_filePath(filePath) {};
    virtual ~YuwellFormat() = default;
    virtual QStringList GetModelSerials() = 0;
    virtual bool Detect() = 0;
    virtual int Open() = 0;
  protected:
    YuwellLoader *m_loader;
    QString m_filePath;
    QMap<SessionID, Session *> Sessions;
    bool calc_leaks = true;
    float lpm4, lpm20;  // Leak per minute at 4 and 20 cmH20
};


/*
 * These are placeholder class names until we can confirm that the formats are actually the BreathCare versions
 * If this is the case, then the renaming should be:
 *
 * YuwellFormatA -> YuwellBreathCareECO
 * YuwellFormatB -> YuwellBreathCareI
 * YuwellFormatC -> YuwellBreathCareII
 * YuwellFormatD -> YuwellBreathCareIII
 *
 * The list of known Yuwell machines and their BreathCare revisions:
 *
 * BreathCare ECO: YH-550
 * BreathCare I: YH-350, YH-360, YH-580, YH-720, YH-725, YH-730
 * BreathCare II: YH-450, YH-480, YH-820, YH-825, YH-830
 * BreathCare III: YH-680, YH-690
 * Unknown revision, own format (YuwellFormatE): YH-920
 * Unknown revision, own format (YuwellFormatF): YH-560
 *
 * So far we've only seen one model from each BreathCare revision, so its difficult to say for sure. If we see
 * another machine and it *just works* then we can be closer to saying that this is how the data format are organised.
 */


class YuwellFormatA : public YuwellFormat
{
  public:
    YuwellFormatA(YuwellLoader &loader, const QString &filePath) : YuwellFormat(loader, filePath) {};
    virtual QStringList GetModelSerials();
    virtual bool Detect();
    virtual int Open();
  private:
    int OpenMachine(Machine *mach, const QString & serial);
    bool OpenSession(Machine *mach, const QString & filename);
};

class YuwellFormatB : public YuwellFormat
{
  public:
    YuwellFormatB(YuwellLoader &loader, const QString &filePath) : YuwellFormat(loader, filePath) {};
    virtual QStringList GetModelSerials();
    virtual bool Detect();
    virtual int Open();
  private:
    int OpenMachine(Machine *mach, const QString & serial);
};

class YuwellFormatC : public YuwellFormat
{
  public:
    YuwellFormatC(YuwellLoader &loader, const QString &filePath) : YuwellFormat(loader, filePath) {};
    virtual QStringList GetModelSerials();
    virtual bool Detect();
    virtual int Open();
  private:
    int OpenMachine(Machine *mach, const QString & serial);
    bool OpenSession(Machine *mach, const QString & filename);
};

class YuwellFormatD : public YuwellFormat
{
  public:
    YuwellFormatD(YuwellLoader &loader, const QString &filePath) : YuwellFormat(loader, filePath) {};
    virtual QStringList GetModelSerials();
    virtual bool Detect();
    virtual int Open();
  private:
    int OpenMachine(Machine *mach, const QString & serial);
    bool OpenSession(Machine *mach, const QString & filename);
};

class YuwellFormatE : public YuwellFormat
{
  public:
    YuwellFormatE(YuwellLoader &loader, const QString &filePath) : YuwellFormat(loader, filePath) {};
    virtual QStringList GetModelSerials();
    virtual bool Detect();
    virtual int Open();
  private:
    QString FindDataDir(const QString & serial);
    int OpenMachine(Machine *mach, const QString & serial);
    bool OpenSession(Machine *mach, const QString & sessionDirPath);
};

class YuwellFormatF : public YuwellFormat
{
  public:
    YuwellFormatF(YuwellLoader &loader, const QString &filePath) : YuwellFormat(loader, filePath) {};
    virtual QStringList GetModelSerials();
    virtual bool Detect();
    virtual int Open();
  private:
    QString FindDataDir(const QString & serial);
    int OpenMachine(Machine *mach, const QString & serial);
    bool OpenSession(Machine *mach, const QString & filename);
};

// class YuwellLoader;
class YuwellLoader : public CPAPLoader
{
  public:
    YuwellLoader();
    virtual ~YuwellLoader();
    static void Register();
    virtual bool Detect(const QString & path);
    virtual bool backupData (Machine * mach, const QString & path);
    virtual int Version() { return yuwell_data_version; };
    virtual int Open(const QString &);
    virtual const QString &loaderName() { return yuwell_class_name; };
    virtual YuwellFormat* YuwellFactory(const QString & givenPath);
    virtual MachineInfo newInfo() {
        return MachineInfo(MT_CPAP, 0, yuwell_class_name, QObject::tr("Yuwell"),
                           QString(), QString(), QString(), QObject::tr("Yuwell"),
                           QDateTime::currentDateTime(), yuwell_data_version);
    }
    virtual void finish();

  protected:
    bool rebuild_from_backups = false;
    bool create_backups = true;

};

#endif // YUWELL_LOADER_H
