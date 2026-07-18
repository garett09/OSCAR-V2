#include <QTimeZone>
#include <QString>
#include <QDir>
#include <QDebug>
#include <QMessageBox>
#include <QCoreApplication>

#include "SleepLib/loader_plugins/yuwell_loader.h"

/*
 * Take care of BreathCare ECO and BreathCare I/II/III type machines
 * The files either contain single sessions per BYS file OR a single file with all sessions
 *
 * At this stage I don't know which machines have SD-Cards, some may not
 * and if all machines provide full logs rather than just summaries.
 * This is left in the simple state that it is until I receive data from
 * other models. Other models include:
 *
 * YH350 (BreathCare I Auto-CPAP)
 * YH360 (BreathCare I Auto-CPAP)
 * YH450 (BreathCare II Auto-CPAP)
 * YH480 (BreathCare II Auto-CPAP)
 * YH550 (BreathCare ECO Auto-CPAP, A/B Models) (Supported)
 * YH580 (BreathCare I Auto-CPAP) (Supported)
 * YH680 (BreathCare III Auto-CPAP, A/B Models)
 * YH690 (BreathCare III Auto-CPAP, A/B Models)
 * YH720 (BreathCare I Bi-PAP)
 * YH725 (BreathCare I Bi-PAP)
 * YH730 (BreathCare I Bi-PAP)
 * YH820 (BreathCare II Bi-PAP)
 * YH825 (BreathCare II Bi-PAP)
 * YH830 (BreathCare II Bi-PAP)
 * So This will probably change around here to detect each model and if the file
 * format is different, the Open() will have to change as well
 *
 * Fingers crossed the other models with CD-Cards have the same file format.
 * NARRATOR: They weren't
 *
 * NOTES
 * ======
 *
 * So far we have 4 BYS formats, the YH-550, YH-580, YH-690, YH-680 and the YH-830
 *
 * The BYS file contains either a single session or multiple sessions
 * The SD-CARD structure for both machines are completely different
 *
 * Can identify the machine type by looking at the source of truth in the model/serial field
 *
 * The SD-CARDS have a shallow directory structure, only 1 directory deep
 * Don't need to recurse and entire directory tree looking for BYS files
 *
 * Can detect Yuwell data using SD-CARD files and directories and if the model starts with "YH"
 *
 * Can find specific machines by looking at the model/serial fields
 *
 * Can then import data depending on the format of the BYS files and directory locations
 *
 * Call them YuwellFormatA, YuwellFormatB, YuwellFormatC, YuwellFormatD and so on. Iterate over each looking for a match
 *
 * 1. Match the file structure
 * 2. Look for model/serial
 *
 * Model must start with "YH"
 *
 * Do not reject data based on model number (yet)
 */

ChannelID Yuwell_Ramp;
ChannelID Yuwell_Humidity;


bool YuwellFormatA::Detect() {
    /*
     * root - RunLog.bys (0Kb in size)
     *      \ MODEL-SERIAL
     *        \ 0100001.BYS <- Data is in here
     *
     * Models seen
     * -----------
     * YH-550
     */
    QDir cardDir(m_filePath);

    if (!cardDir.exists("RunLog.bys")) {
        return false;
    }

    QStringList model_serials = GetModelSerials();

    if (model_serials.size() == 0) {
        return false;
    }

    qDebug() << "Found Yuwell format A serial number: " << model_serials;
    return true;
}

QStringList YuwellFormatA::GetModelSerials() {
    /*
     * Find a model/serial number from the inside of a file
     * instead of a directory name. Much more source-of-truth
     */
    QDir cardDir(m_filePath);

    cardDir.setFilter(QDir::NoDotAndDotDot | QDir::Dirs | QDir::NoSymLinks);
    cardDir.setSorting(QDir::Name);

    QFileInfoList dlist = cardDir.entryInfoList();

    QStringList model_serials;

    for (int i = 0; i < dlist.size(); i++) {
        // Iterate until we find our first file with a model/serial number
        QFileInfo fi = dlist.at(i);
        QString filename = fi.fileName();

        if (filename.toUpper().startsWith("YH")) {
            // Its tempting to use the directory as the model/serial, but there's a source of truth in each file
            QDir machineDir (m_filePath + "/" + filename);
            machineDir.setFilter(QDir::NoDotAndDotDot | QDir::Files | QDir::NoSymLinks);
            machineDir.setSorting(QDir::Name);

            QStringList filters;
            filters << "*.BYS";
            machineDir.setNameFilters(filters);
            QFileInfoList flist = machineDir.entryInfoList();
            if (flist.size() >= 1) { // Found data
                QFileInfo ex = flist.at(0);
                QFile bysFile(m_filePath + "/" + filename + "/" + ex.fileName());

                if (bysFile.open(QFile::ReadOnly)) {
                    QByteArray header = bysFile.read(0xA0);

                    if (header.size() == 0xA0) {
                        QDataStream in(header);
                        in.setVersion(QDataStream::Qt_4_8);

                        QByteArray raw_model_serial(16, Qt::Uninitialized);

                        in.skipRawData(0x1E);
                        in.readRawData(raw_model_serial.data(), 16);

                        QString model_serial(raw_model_serial);
                        if (model_serial.startsWith("YH") && !model_serials.contains(model_serial)) {
                            model_serials << model_serial;
                        }
                    }
                    bysFile.close();
                }
            }
        }
    }
    return model_serials;
}

int YuwellFormatA::OpenMachine(Machine *mach, const QString & serial)
{
    emit m_loader->updateMessage(QObject::tr("Getting Ready..."));
    emit m_loader->setProgressValue(0);
    QCoreApplication::processEvents();

    QString serialPath = m_filePath + "/" + serial;
    QDir dir(serialPath);

    if (!dir.exists() || (!dir.isReadable())) {
        return -1;
    }

    m_loader->backupData(mach, m_filePath);

    calc_leaks = p_profile->cpap->calculateUnintentionalLeaks();
    lpm4 = p_profile->cpap->custom4cmH2OLeaks();
    lpm20 = p_profile->cpap->custom20cmH2OLeaks();

    dir.setFilter(QDir::NoDotAndDotDot | QDir::Files | QDir::NoSymLinks);
    dir.setSorting(QDir::Name);
    QFileInfoList flist = dir.entryInfoList();

    emit m_loader->updateMessage(QObject::tr("Reading data files..."));
    QCoreApplication::processEvents();

    Sessions.clear();

    QString filename, fpath;
    emit m_loader->setProgressMax(flist.size());

    // There's only one file type with an extension .BYS containing a header and line data
    for (int i = 0; i < flist.size(); i++) {
        emit m_loader->setProgressValue(i);
        QFileInfo fi = flist.at(i);
        filename = fi.fileName();
        fpath = serialPath + "/" + filename;

        OpenSession(mach, fpath);
    }

    int c = Sessions.size();
    qDebug() << "Yuwell Format A Loader found" << c << "sessions";

    emit m_loader->updateMessage(QObject::tr("Finishing up..."));
    QCoreApplication::processEvents();

    m_loader->finish();

    return c;
}

bool YuwellFormatA::OpenSession(Machine *mach, const QString & filename)
{
    QFile file(filename);

    if (!file.open(QFile::ReadOnly)) {
        qWarning() << "Yuwell Session Couldn't open " << filename;
        return false;
    }

    // Read header of summary file, 51 bytes ending in 0xF9
    QByteArray header;
    header = file.read(0x33);

    if (header.size() != 0x33) {
        qWarning() << "Yuwell Session Short file " << filename;
        file.close();
        return false;
    }

    char terminator = 0xf9;
    if (terminator != header.at(0x32)) {
        qWarning() << "Yuwell Session Header missing 0xf9 terminator" << filename;
    }

    QDataStream in(header);
    in.setVersion(QDataStream::Qt_4_8);
    in.setByteOrder(QDataStream::LittleEndian);

    unsigned char start_year, start_month, start_day, start_hour, start_minute, start_second;
    unsigned char finish_year, finish_month, finish_day, finish_hour, finish_minute, finish_second;

    in >> start_year;
    in >> start_month;
    in >> start_day;
    in >> start_hour;
    in >> start_minute;
    in >> start_second;

    in >> finish_year;
    in >> finish_month;
    in >> finish_day;
    in >> finish_hour;
    in >> finish_minute;
    in >> finish_second;

    QDateTime start = QDateTime(QDate((int)start_year + 2000, start_month, start_day), QTime(start_hour, start_minute, start_second), QTimeZone::systemTimeZone());
    QDateTime finish = QDateTime(QDate((int)finish_year + 2000, finish_month, finish_day), QTime(finish_hour, finish_minute, finish_second), QTimeZone::systemTimeZone());

    unsigned char mode, ramp_up_time, initial_pressure, minimum_pressure, maximum_pressure;
    unsigned char humidity_settings;
    unsigned char avg_leak_volume, avg_pressure;
    QByteArray raw_model_serial(16, Qt::Uninitialized);
    short int record_count;

    in >> mode;  // 0x00=CPAP, 0x01=APAP
    in >> ramp_up_time;  // Minutes
    in >> initial_pressure;
    in >> minimum_pressure;
    in >> maximum_pressure;
    in.skipRawData(1);
    in >> humidity_settings;
    in.skipRawData(7);
    in >> avg_leak_volume;
    in.skipRawData(1);
    in >> avg_pressure;
    in.skipRawData(1);
    in.readRawData(raw_model_serial.data(), 16);
    in >> record_count;

    QString model_serial(raw_model_serial);
    mach->setModel(model_serial);

    quint32 ts;
    ts = start.toSecsSinceEpoch();  // Create timestamp from session start
    if (!mach->SessionExists(ts)) {
        Session *sess = new Session(mach, ts);
        sess->really_set_first(qint64(ts) * 1000L);
        sess->really_set_last(qint64(finish.toSecsSinceEpoch()) * 1000L);

        if (mode == YUWELL_CPAP) {
            sess->settings[CPAP_Mode] = (int)MODE_CPAP;
            sess->settings[CPAP_Pressure] = initial_pressure / 10.0;
        } else {
            sess->settings[CPAP_Mode] = (int)MODE_APAP;
            sess->settings[CPAP_PressureMin] = minimum_pressure / 10.0;
            sess->settings[CPAP_PressureMax] = maximum_pressure / 10.0;
        }

        sess->settings[Yuwell_Humidity] = humidity_settings;
        sess->settings[Yuwell_Ramp] = ramp_up_time;

        EventList *LK = sess->AddEventList(CPAP_LeakTotal, EVL_Event, 1);
        EventList *PR = sess->AddEventList(CPAP_Pressure, EVL_Event, 0.1F);
        EventList *OA = sess->AddEventList(CPAP_Obstructive, EVL_Event);
        EventList *CA = sess->AddEventList(CPAP_ClearAirway, EVL_Event);
        EventList *H =  sess->AddEventList(CPAP_Hypopnea, EVL_Event);

        qint64 ti;
        ti = qint64(ts) * 1000L;

        for (int i = 0; i < record_count; i++) {
            QCoreApplication::processEvents();
            QByteArray record;

            record = file.read(0x0a);

            if (record.size() != 0x0a) {
                qWarning() << "Yuwell Session Record Short" << filename;
                file.close();
                return false;
            }

            QDataStream in(record);
            in.setVersion(QDataStream::Qt_4_8);
            in.setByteOrder(QDataStream::LittleEndian);
            
            unsigned char pressure, oai, hi, cai, leak_volume;
            in >> pressure;
            in.skipRawData(2);
            in >> oai;
            in >> hi;
            in >> cai;
            in.skipRawData(3);
            in >> leak_volume;

            PR->AddEvent(ti + (i * 60000), pressure); // Samples are every 60 seconds

            if (oai > 0) {
                OA->AddEvent(ti + (i * 60000), 0);
            }
            if (hi > 0) {
                H->AddEvent(ti + (i * 60000), 0);
            }
            if (cai > 0) {
                CA->AddEvent(ti + (i * 60000), 0);
            }
            if (leak_volume > 0) {
                LK->AddEvent(ti + (i * 60000), leak_volume);
            }
        }
        sess->SetChanged(true);
        Sessions[ts] = sess;
        sess->UpdateSummaries();
        mach->AddSession(sess);
    }

    return true;
}

int YuwellFormatA::Open() {
    // Start with m_filePath and do the import
    QStringList model_serials = GetModelSerials();

    Machine *m;

    int c = 0;
    for (int i = 0; i < model_serials.size(); i++) {
        MachineInfo info = m_loader->newInfo();
        info.serial = model_serials[i];
        m = p_profile->CreateMachine(info);
        try {
            if (m) {
                c += OpenMachine(m, model_serials[i]);
                m->Save();
            }
        } catch (OneTypePerDay& e) {
            Q_UNUSED(e)
            p_profile->DelMachine(m);
            QMessageBox::warning(nullptr, QObject::tr("Import Error"),
                                 QObject::tr("This device Record cannot be imported in this profile.")
                                     +"\n\n"+QObject::tr("The Day records overlap with already existing content."),
                                 QMessageBox::Ok);
            delete m;
        }
    }

    return c;
}

bool YuwellFormatB::Detect() {
    /*
     * root - YHSD-NEW.BYS (64Kb in size) <- Data is in here
     *      \ YHSD-OLD.BYS <- We don't care about this
     *
     * Models seen
     * -----------
     * YH-580
     */
    QStringList model_serials = GetModelSerials();
    if (model_serials.size() == 0) {
        return false;
    }

    // Resolve where the data file actually is: directly in m_filePath, or in a
    // model-serial subfolder. This lets detection work from the parent folder too,
    // like every other Yuwell model - not only when pointed inside the .BYS folder.
    QString dataFile = m_filePath + "/YHSD-NEW.BYS";
    if (!QFile::exists(dataFile)) {
        dataFile = m_filePath + "/" + model_serials.first() + "/YHSD-NEW.BYS";
    }
    QFileInfo fi = QFileInfo(dataFile);
    if (fi.size() != 0x10000) { // Needs to be 64Kb in size
        return false;
    }

    qDebug() << "Found Yuwell Format B serial: " << model_serials;

    return true;
}

QStringList YuwellFormatB::GetModelSerials() {
    /*
     * Find a model/serial number from the inside of a file
     * instead of a directory name. Much more source-of-truth
     */
    QStringList model_serials;

    // The .BYS files can be directly in m_filePath (SD root, or a model-serial
    // folder selected directly) or inside a model-serial subfolder when the user
    // selects the PARENT folder - the way every other Yuwell model is imported.
    // Try the direct path first; only if it isn't there do we scan subfolders, so
    // the direct case keeps its exact original behaviour.
    QStringList dirs;
    dirs << m_filePath;
    if (!QFile::exists(m_filePath + "/YHSD-NEW.BYS")) {
        QDir parent(m_filePath);
        parent.setFilter(QDir::NoDotAndDotDot | QDir::Dirs | QDir::NoSymLinks);
        parent.setSorting(QDir::Name);
        QFileInfoList subs = parent.entryInfoList();
        for (int i = 0; i < subs.size(); i++) {
            dirs << subs.at(i).absoluteFilePath();
        }
    }

    for (int di = 0; di < dirs.size(); di++) {
        QFile bysFile(dirs.at(di) + "/YHSD-NEW.BYS");
        if (!bysFile.open(QFile::ReadOnly)) {
            continue;
        }
        QByteArray header = bysFile.read(0xA0);
        bysFile.close();
        if (header.size() != 0xA0) {
            continue;
        }

        QDataStream in(header);
        in.setVersion(QDataStream::Qt_4_8);

        QByteArray raw_model_serial(16, Qt::Uninitialized);

        in.skipRawData(0x84);
        in.readRawData(raw_model_serial.data(), 16);

        QString model_serial(raw_model_serial);
        if (model_serial.startsWith("YH")) {
            if (!model_serials.contains(model_serial)) {
                model_serials << model_serial;
            }
        } else {
            // Some models (e.g. the YH-560) store this serial field obfuscated,
            // so it does not read as "YH...". Fall back to the containing folder
            // name, which is in the "Model-Serial" form (e.g. YH560A-221050031).
            QString dirName = QDir(dirs.at(di)).dirName();
            if (dirName.toUpper().startsWith("YH") && !model_serials.contains(dirName)) {
                model_serials << dirName;
            }
        }
    }

    return model_serials;
}

int YuwellFormatB::Open() {
    // Start with m_filePath and do the import
    QStringList model_serials = GetModelSerials();

    Machine *m;

    int c = 0;
    for (int i = 0; i < model_serials.size(); i++) {
        MachineInfo info = m_loader->newInfo();
        info.serial = model_serials[i];
        m = p_profile->CreateMachine(info);
        try {
            if (m) {
                c += OpenMachine(m, model_serials[i]);
                m->Save();
            }
        } catch (OneTypePerDay& e) {
            Q_UNUSED(e)
            p_profile->DelMachine(m);
            QMessageBox::warning(nullptr, QObject::tr("Import Error"),
                                 QObject::tr("This device Record cannot be imported in this profile.")
                                     +"\n\n"+QObject::tr("The Day records overlap with already existing content."),
                                 QMessageBox::Ok);
            delete m;
        }
    }

    return c;
}

int YuwellFormatB::OpenMachine(Machine *mach, const QString & serial) {
    emit m_loader->updateMessage(QObject::tr("Getting Ready..."));
    emit m_loader->setProgressValue(0);
    QCoreApplication::processEvents();

    // The .BYS may be directly in m_filePath (SD root / model-serial folder
    // selected directly) or inside a model-serial subfolder when the user selects
    // the parent folder. Resolve the actual data directory for this serial.
    QString dataDir = m_filePath;
    if (!QFile::exists(dataDir + "/YHSD-NEW.BYS")) {
        dataDir = m_filePath + "/" + serial;
    }
    QDir dir(dataDir);

    if (!dir.exists() || (!dir.isReadable())) {
        return -1;
    }

    m_loader->backupData(mach, dataDir);

    calc_leaks = p_profile->cpap->calculateUnintentionalLeaks();
    lpm4 = p_profile->cpap->custom4cmH2OLeaks();
    lpm20 = p_profile->cpap->custom20cmH2OLeaks();

    emit m_loader->updateMessage(QObject::tr("Reading data files..."));
    QCoreApplication::processEvents();

    Sessions.clear();

    QFile file(dataDir + "/YHSD-NEW.BYS");

    if (!file.exists()) {
        qWarning() << "Yuwell Session Cannot find " << file.fileName();
        return -1;
    }

    // We just read the file contents from here...
    if (!file.open(QFile::ReadOnly)) {
        qWarning() << "Yuwell Session Couldn't open " << file.fileName();
        return -1;
    }

    QByteArray cpap_data;
    cpap_data = file.read(0x10000);

    if (cpap_data.size() != 0x10000) {
        qWarning() << "Yuwell Session Short file " << file.fileName();
        file.close();
        return -1;
    }

    QDataStream in(cpap_data);
    in.setVersion(QDataStream::Qt_4_8);
    in.setByteOrder(QDataStream::LittleEndian); // Mostly, except for record offsets

    unsigned char mode, ramp, initial_pressure, pressure_setting, maximum_pressure, minimum_pressure, humidity, fps_level;
    unsigned char oai_count, hi_count, avg_leak_vol, avg_pressure, offset_high, offset_low;
    quint16 session_minutes;  // bytes 28-29 of the 30-byte session summary record, BE u16
    short int record_count;
    short unsigned int offset;
    unsigned char start_year, start_month, start_day, start_hour, start_minute, start_second;
    unsigned char finish_year, finish_month, finish_day, finish_hour, finish_minute, finish_second;

    QByteArray raw_model_serial(16, Qt::Uninitialized);

    in.skipRawData(4); // Magic number "AAAA", but as this may be different for some machines, we don't enfore a check (yet)

    in >> mode;
    in >> ramp;
    in >> initial_pressure;
    in >> pressure_setting;
    in >> maximum_pressure;
    in >> minimum_pressure;
    in >> humidity;
    in >> fps_level;

    in.skipRawData(19);

    in >> record_count;

    in.skipRawData(99);

    in.readRawData(raw_model_serial.data(), 16);

    QString model_serial(raw_model_serial);
    // Some models (e.g. the YH-560) store this serial field obfuscated, so it
    // does not read as "YH...". Such a device could not be detected AT ALL before
    // this patch (GetModelSerials() returned nothing and Detect() failed), so
    // there is no working behaviour to regress. When the serial is obfuscated we
    // fall back to the folder-derived serial and enable the recovery workarounds
    // below; devices whose serial reads cleanly keep the original code path
    // unchanged, byte for byte.
    bool serialObfuscated = !model_serial.startsWith("YH");
    if (serialObfuscated) {
        model_serial = serial;
    }
    mach->setModel(model_serial);

    in.skipRawData(2924); // Alarming amount of data to skip

    QList<FormatBSessionSummary> sessionSummaries;
    // On models with an obfuscated serial the header record_count is obfuscated
    // too (e.g. the YH-560 reads 273 but has ~365 valid records), so instead of
    // trusting it we walk the whole 30-byte session grid from 0xC00 to 0x7600 and
    // validate each record, skipping garbage (the ring buffer's partially-written
    // write-head and empty 0xFF slots). Models with a clean serial keep the
    // original behaviour: read exactly record_count records, unconditionally.
    int maxRecords = (0x7600 - 0x0C00) / 30;
    int loopCount = serialObfuscated ? maxRecords : record_count;
    for (int i = 0; i < loopCount; i++) {
        in >> start_year;
        in >> start_month;
        in >> start_day;
        in >> start_hour;
        in >> start_minute;
        in >> start_second;

        in >> finish_year;
        in >> finish_month;
        in >> finish_day;
        in >> finish_hour;
        in >> finish_minute;
        in >> finish_second;

        QDateTime start = QDateTime(QDate((int)start_year + 2000, start_month, start_day), QTime(start_hour, start_minute, start_second), QTimeZone::systemTimeZone());
        QDateTime finish = QDateTime(QDate((int)finish_year + 2000, finish_month, finish_day), QTime(finish_hour, finish_minute, finish_second), QTimeZone::systemTimeZone());

        in >> mode;
        in >> ramp;
        in >> initial_pressure;
        in >> pressure_setting;
        in >> maximum_pressure;
        in >> minimum_pressure;
        in >> humidity;
        in >> fps_level;
        in >> oai_count;
        in >> hi_count;
        in.skipRawData(2);
        in >> avg_pressure;   // byte 24 is average pressure (confirmed vs the
        in >> avg_leak_vol;   // official app); byte 25 is average leak. Was reversed.
        in >> offset_high;
        in >> offset_low;
        /*
         * The offset 0x7600 here seems fairly arbitrary but from examining the header of the file I
         * cannot find any data that could possibly be used to calculate this value. It's fairly odd
         * to have something like this in a data file at a fixed place. So if the imports fail in the
         * future, this is a likely culprit.
         * For reference, the header seems to extent to position 0x0C00 where it starts with session
         * summary information. Then there's nothing of a bunch of 0xFF's until 0x7600.
         */
        offset = ((offset_high << 0x08) + offset_low) + 0x7600;
        in.setByteOrder(QDataStream::BigEndian);
        in >> session_minutes;   // bytes 28-29: BE u16
        in.setByteOrder(QDataStream::LittleEndian);

        // The finish datetime (bytes 6-11) is frequently corrupt on obfuscated
        // models (e.g. the YH-560), which produced absurd session lengths. The
        // duration (bytes 28-29) is reliable, so reconstruct finish = start +
        // duration. Only for obfuscated models; clean models keep their own,
        // valid finish datetime.
        if (serialObfuscated) {
            finish = start.addSecs((qint64)session_minutes * 60);
        }

        FormatBSessionSummary sessionSummary = {
            start,
            finish,
            mode,
            ramp,
            initial_pressure,
            pressure_setting,
            maximum_pressure,
            minimum_pressure,
            humidity,
            fps_level,
            oai_count,
            hi_count,
            avg_leak_vol,
            avg_pressure,
            offset,
            session_minutes
        };
        if (!serialObfuscated) {
            // Clean models: original behaviour — trust record_count, append all.
            sessionSummaries.append(sessionSummary);
        } else if (start.isValid() && start <= QDateTime::currentDateTime()
                && mode <= 1
                && ramp <= 60 && humidity <= 8 && fps_level <= 8
                && maximum_pressure >= 20 && maximum_pressure <= 200
                && minimum_pressure >= 20 && minimum_pressure <= 200
                && minimum_pressure <= maximum_pressure
                && session_minutes >= 1 && session_minutes <= 1440) {
            // Obfuscated models: keep only plausible records; skip garbage/
            // write-head slots and empty (0xFF) padding. A session can't be future.
            sessionSummaries.append(sessionSummary);
        }
    }

    unsigned char leakage, pressure, spo2, oai, hi, pulse, cai;
    emit m_loader->setProgressMax(sessionSummaries.size());
    int counter = 0;
    for (FormatBSessionSummary summary : sessionSummaries) {
        emit m_loader->setProgressValue(counter++);
        QDataStream in(cpap_data.mid(summary.offset, summary.session_minutes * 7));
        quint32 ts;
        ts = summary.start.toSecsSinceEpoch();  // Create timestamp from session start
        if (!mach->SessionExists(ts)) {
            Session *sess = new Session(mach, ts);
            sess->really_set_first(qint64(ts) * 1000L);
            sess->really_set_last(qint64(summary.finish.toSecsSinceEpoch()) * 1000L);
            if (summary.mode == YUWELL_CPAP) {
                sess->settings[CPAP_Mode] = (int)MODE_CPAP;
                sess->settings[CPAP_Pressure] = summary.initial_pressure / 10.0;
            } else {
                sess->settings[CPAP_Mode] = (int)MODE_APAP;
                sess->settings[CPAP_PressureMin] = summary.minimum_pressure / 10.0;
                sess->settings[CPAP_PressureMax] = summary.maximum_pressure / 10.0;
            }
            sess->settings[Yuwell_Humidity] = summary.humidity;
            sess->settings[Yuwell_Ramp] = summary.ramp;

            EventList *LK = sess->AddEventList(CPAP_LeakTotal, EVL_Event, 1);
            EventList *PR = sess->AddEventList(CPAP_Pressure, EVL_Event, 0.1F);
            EventList *OA = sess->AddEventList(CPAP_Obstructive, EVL_Event);
            EventList *CA = sess->AddEventList(CPAP_ClearAirway, EVL_Event);
            EventList *H =  sess->AddEventList(CPAP_Hypopnea, EVL_Event);
            EventList *P =  sess->AddEventList(OXI_Pulse, EVL_Event);
            EventList *O =  sess->AddEventList(OXI_SPO2, EVL_Event);

            qint64 ti;
            ti = qint64(ts) * 1000L;

            bool logDecoded = false;
            if (!serialObfuscated) {
                // Original YH-580 per-minute layout: 7-byte records
                // [leak, pressure, spo2, oai, hi, pulse, cai], first byte 0xF9.
                for (int i = 0; i < summary.session_minutes; i++) {
                    QCoreApplication::processEvents();

                    in >> leakage;
                    in >> pressure;
                    in >> spo2;
                    in >> oai;
                    in >> hi;
                    in >> pulse;
                    in >> cai;

                    if (i == 0 && leakage != 0xF9) {
                        break; // Out of bounds offset, not a valid record
                    }
                    logDecoded = true;
                    PR->AddEvent(ti + (i * 60000), pressure); // Samples are every 60 seconds
                    if (pulse < 249) {
                        P->AddEvent(ti + (i * 60000), pulse);
                    }
                    O->AddEvent(ti + (i * 60000), spo2);

                    if (oai > 0) {
                        OA->AddEvent(ti + (i * 60000), 0);
                    }
                    if (hi > 0) {
                        H->AddEvent(ti + (i * 60000), 0);
                    }
                    if (cai > 0) {
                        CA->AddEvent(ti + (i * 60000), 0);
                    }
                    if (leakage > 0 && leakage < 249) { // First record always pulls high, ignore
                        LK->AddEvent(ti + (i * 60000), leakage);
                    }
                }
            } else {
                // YH-560: use the device's own per-session average pressure (byte 24)
                // and leak (byte 25) from the summary, held flat across the night.
                //
                // The YH-560 does store a per-minute log (reverse-engineered: 7 bytes/
                // min, [0]=0x7F & [3]=0xFF sync, [5]=leak, [6]=pressure x10; no SpO2/
                // pulse channel since this family has no oximeter), but it lives in a
                // small ring buffer. Verified on known-good pre-fault data: only ~2% of
                // sessions retain a full per-minute trace and ~80% none at all, so the
                // per-minute samples can't form meaningful pressure/leak aggregates
                // (they dilute the reported average toward ~1 cmH2O). The summary
                // averages, by contrast, are present and correct for every session
                // (byte 24 mean matches the device's reported average pressure), so we
                // use those and hold them flat from session start to finish.
                qint64 tend = qint64(summary.finish.toSecsSinceEpoch()) * 1000L;
                if (summary.avg_pressure >= 20 && summary.avg_pressure <= 200) {
                    PR->AddEvent(ti, summary.avg_pressure);    // gain 0.1 -> cmH2O
                    PR->AddEvent(tend, summary.avg_pressure);
                }
                // byte 25 is already the device's EXCESS (unintentional) leak x10 - the
                // number it shows on screen as "Leak". Put it straight into CPAP_Leak,
                // NOT CPAP_LeakTotal: OSCAR's Leak Rate statistic reads CPAP_Leak, which
                // for a non-ResMed machine it otherwise DERIVES from CPAP_LeakTotal by
                // subtracting the mask's intentional leak (~22 L/min at these pressures)
                // - that would zero out our already-excess value. calcs.cpp calcLeaks()
                // early-returns when a session already has a CPAP_Leak list, so writing
                // it directly here is respected, not recomputed.
                EventList *LKu = sess->AddEventList(CPAP_Leak, EVL_Event, 1);
                int leak_lpm = summary.avg_leak_vol / 10;      // byte 25 is stored x10
                if (leak_lpm <= 200) {
                    LKu->AddEvent(ti, leak_lpm);
                    LKu->AddEvent(tend, leak_lpm);
                }
            }
            // Events for the YH-560 come from the reliable summary counts (bytes
            // 20-21), not the per-minute flag bytes (which aren't the scored events):
            // synthesize OAI/HI events spread across the session so the AHI matches
            // what the device reports. Gated on serialObfuscated (logDecoded stays
            // false on that branch) so a clean model keeps its original behaviour.
            // (Best-effort; needs a compile + import test.)
            if (!logDecoded && serialObfuscated
                    && (summary.oai_count > 0 || summary.hi_count > 0)) {
                int total = summary.oai_count + summary.hi_count;
                qint64 span = qint64(summary.session_minutes) * 60000L;
                int placed = 0;
                for (int j = 0; j < summary.oai_count; j++, placed++) {
                    OA->AddEvent(ti + (span * placed / (total ? total : 1)), 0);
                }
                for (int j = 0; j < summary.hi_count; j++, placed++) {
                    H->AddEvent(ti + (span * placed / (total ? total : 1)), 0);
                }
            }
            sess->SetChanged(true);
            Sessions[ts] = sess;
            sess->UpdateSummaries();
            mach->AddSession(sess);
        }
    }

    emit m_loader->updateMessage(QObject::tr("Finishing up..."));
    QCoreApplication::processEvents();

    m_loader->finish();

    return sessionSummaries.size();
}

bool YuwellFormatC::Detect() {
    /*
     * root - \ MODEL-SERIAL
     *          \ 0100001.BYS <- Data is in here
     *
     * Models seen
     * -----------
     * YH-830
     */
    QDir cardDir(m_filePath);

    QStringList model_serials = GetModelSerials();

    if (model_serials.size() == 0) {
        return false;
    }

    qDebug() << "Found Yuwell format C serial number: " << model_serials;
    return true;
}

QStringList YuwellFormatC::GetModelSerials() {
    /*
     * Find a model/serial number from the inside of a file
     * instead of a directory name. Much more source-of-truth
     */
    QDir cardDir(m_filePath);

    cardDir.setFilter(QDir::NoDotAndDotDot | QDir::Dirs | QDir::NoSymLinks);
    cardDir.setSorting(QDir::Name);

    QFileInfoList dlist = cardDir.entryInfoList();

    QStringList model_serials;

    for (int i = 0; i < dlist.size(); i++) {
        // Iterate until we find our first file with a model/serial number
        QFileInfo fi = dlist.at(i);
        QString filename = fi.fileName();

        if (filename.toUpper().startsWith("YH")) {
            // Its tempting to use the directory as the model/serial, but there's a source of truth in each file
            QDir machineDir (m_filePath + "/" + filename);
            machineDir.setFilter(QDir::NoDotAndDotDot | QDir::Files | QDir::NoSymLinks);
            machineDir.setSorting(QDir::Name);

            QStringList filters;
            filters << "*.BYS";
            machineDir.setNameFilters(filters);
            QFileInfoList flist = machineDir.entryInfoList();
            if (flist.size() >= 1) { // Found data
                QFileInfo ex = flist.at(0);
                QFile bysFile(m_filePath + "/" + filename + "/" + ex.fileName());

                if (bysFile.open(QFile::ReadOnly)) {
                    QByteArray header = bysFile.read(0xA0);

                    if (header.size() == 0xA0) {
                        QDataStream in(header);
                        in.setVersion(QDataStream::Qt_4_8);

                        QByteArray raw_model_serial(16, Qt::Uninitialized);

                        in.skipRawData(0x27);
                        in.readRawData(raw_model_serial.data(), 16);

                        QString model_serial(raw_model_serial);
                        if (model_serial.startsWith("YH") && !model_serials.contains(model_serial)) {
                            model_serials << model_serial;
                        }
                    }
                    bysFile.close();
                }
            }
        }
    }
    return model_serials;
}

int YuwellFormatC::OpenMachine(Machine *mach, const QString & serial)
{
    emit m_loader->updateMessage(QObject::tr("Getting Ready..."));
    emit m_loader->setProgressValue(0);
    QCoreApplication::processEvents();

    QString serialPath = m_filePath + "/" + serial;
    QDir dir(serialPath);

    if (!dir.exists() || (!dir.isReadable())) {
        return -1;
    }

    m_loader->backupData(mach, m_filePath);

    calc_leaks = p_profile->cpap->calculateUnintentionalLeaks();
    lpm4 = p_profile->cpap->custom4cmH2OLeaks();
    lpm20 = p_profile->cpap->custom20cmH2OLeaks();

    dir.setFilter(QDir::NoDotAndDotDot | QDir::Files | QDir::NoSymLinks);
    dir.setSorting(QDir::Name);
    QFileInfoList flist = dir.entryInfoList();

    emit m_loader->updateMessage(QObject::tr("Reading data files..."));
    QCoreApplication::processEvents();

    Sessions.clear();

    QString filename, fpath;
    emit m_loader->setProgressMax(flist.size());

    // There's only one file type with an extension .BYS containing a header and line data
    for (int i = 0; i < flist.size(); i++) {
        emit m_loader->setProgressValue(i);
        QFileInfo fi = flist.at(i);
        filename = fi.fileName();
        fpath = serialPath + "/" + filename;

        OpenSession(mach, fpath);
    }

    int c = Sessions.size();
    qDebug() << "Yuwell Format C Loader found" << c << "sessions";

    emit m_loader->updateMessage(QObject::tr("Finishing up..."));
    QCoreApplication::processEvents();

    m_loader->finish();

    return c;
}

bool YuwellFormatC::OpenSession(Machine *mach, const QString & filename)
{
    qDebug() << "Opening session " << filename;
    QFile file(filename);

    if (!file.open(QFile::ReadOnly)) {
        qWarning() << "Yuwell Session Couldn't open " << filename;
        return false;
    }

    // Read header of summary file, 60 bytes
    QByteArray header;
    header = file.read(0x3C);

    if (header.size() != 0x3C) {
        qWarning() << "Yuwell Session Short file " << filename;
        file.close();
        return false;
    }

    QDataStream in(header);
    in.setVersion(QDataStream::Qt_4_8);
    in.setByteOrder(QDataStream::LittleEndian);

    short int start_year;
    unsigned char start_month, start_day, start_hour, start_minute, start_second;
    short int finish_year;
    unsigned char finish_month, finish_day, finish_hour, finish_minute, finish_second;

    in >> start_year;
    in >> start_month;
    in >> start_day;
    in >> start_hour;
    in >> start_minute;
    in >> start_second;

    in >> finish_year;
    in >> finish_month;
    in >> finish_day;
    in >> finish_hour;
    in >> finish_minute;
    in >> finish_second;

    QDateTime start = QDateTime(QDate(start_year, start_month, start_day), QTime(start_hour, start_minute, start_second), QTimeZone::systemTimeZone());
    QDateTime finish = QDateTime(QDate(finish_year, finish_month, finish_day), QTime(finish_hour, finish_minute, finish_second), QTimeZone::systemTimeZone());

    short int record_count;
    unsigned char humidity_settings;
    QByteArray raw_model_serial(16, Qt::Uninitialized);

    in >> record_count;
    in.skipRawData(5);
    in >> humidity_settings;
    in.skipRawData(17);
    in.readRawData(raw_model_serial.data(), 16);
    in.skipRawData(5);

    QString model_serial(raw_model_serial);
    mach->setModel(model_serial);

    unsigned char maximum_pressure;
    maximum_pressure = 150; // TODO: This doesn't seem to exist?
    // Missing stuff, especially the CPAP mode and ramp time
    // unsigned char mode, minimum_pressure, maximum_pressure;
    // unsigned char avg_leak_volume, avg_pressure;
    // in >> mode;  // 0x00=CPAP, 0x01=APAP
    // in >> ramp_up_time;  // Minutes
    // in >> initial_pressure;
    // in >> minimum_pressure;
    // in >> maximum_pressure;
    // in >> avg_leak_volume;
    // in >> avg_pressure;

    quint32 ts;
    ts = start.toSecsSinceEpoch();  // Create timestamp from session start
    if (!mach->SessionExists(ts)) {
        Session *sess = new Session(mach, ts);
        sess->really_set_first(qint64(ts) * 1000L);
        sess->really_set_last(qint64(finish.toSecsSinceEpoch()) * 1000L);

        sess->settings[CPAP_Mode] = -1;
        sess->settings[Yuwell_Humidity] = humidity_settings;

        EventList *LK = sess->AddEventList(CPAP_LeakTotal, EVL_Event, 0.1F);
        EventList *PR = sess->AddEventList(CPAP_Pressure, EVL_Event, 0.1F);
        EventList *IPAP = sess->AddEventList(CPAP_IPAP, EVL_Event, 0.1F);
        EventList *EPAP = sess->AddEventList(CPAP_EPAP, EVL_Event, 0.1F);
        EventList *OA = sess->AddEventList(CPAP_Obstructive, EVL_Event);
        EventList *CA = sess->AddEventList(CPAP_ClearAirway, EVL_Event);
        EventList *H =  sess->AddEventList(CPAP_Hypopnea, EVL_Event);
        EventList *TV = sess->AddEventList(CPAP_TidalVolume, EVL_Event, 1);
        EventList *RR = sess->AddEventList(CPAP_RespRate, EVL_Event, 1);

        qint64 ti;
        ti = qint64(ts) * 1000L;

        for (int i = 0; i < record_count; i++) {
            QCoreApplication::processEvents();
            QByteArray record;

            record = file.read(0x28);

            if (record.size() != 0x28) {
                qWarning() << "Yuwell Session Record Short" << filename;
                file.close();
                return false;
            }

            QDataStream in(record);
            in.setVersion(QDataStream::Qt_4_8);
            in.setByteOrder(QDataStream::LittleEndian);

            unsigned char mode, pressure, initial_pressure, ramp, leak_volume, minute_volume, inspiratory_ratio, respiratory_rate;
            short int tidal_volume;
            unsigned short int ipap_raw, epap_raw;  // LE u16, scale /10 to cmH2O; populated on BiPAP records, zero on CPAP/APAP
            unsigned char oai, hi, cai;
            in.skipRawData(1); // Always 0xF9
            in >> mode; // Weird that this records the session mode on every log line. Maybe you can change mode half-way through a session on this model?
            in >> ipap_raw;  // record bytes 0x02-0x03: IPAP * 10 (BiPAP modes only)
            in >> epap_raw;  // record bytes 0x04-0x05: EPAP * 10 (BiPAP modes only)
            in.skipRawData(6);
            in >> pressure;
            in >> initial_pressure;
            in.skipRawData(4);
            in >> ramp;
            in.skipRawData(2);
            in >> tidal_volume;
            in >> leak_volume;
            in.skipRawData(1);
            in >> minute_volume;
            in.skipRawData(5);
            in >> inspiratory_ratio;
            in.skipRawData(2);
            in >> respiratory_rate;
            in >> oai;
            in >> hi;
            in.skipRawData(1);
            in >> cai;
            in.skipRawData(1);

            // For some odd reason, these settings are the line items not the header
            if (sess->settings[CPAP_Mode] == -1) {
                sess->settings[Yuwell_Ramp] = ramp;
                if (mode == YUWELL_FORMATC_CPAP) {
                    sess->settings[CPAP_Mode] = (int)MODE_CPAP;
                    sess->settings[CPAP_Pressure] = initial_pressure / 10.0;
                } else if (mode == YUWELL_FORMATC_APAP) {
                    sess->settings[CPAP_Mode] = (int)MODE_APAP;
                    sess->settings[CPAP_PressureMin] = pressure / 10.0;
                    sess->settings[CPAP_PressureMax] = maximum_pressure / 10.0;
                } else if (mode == YUWELL_FORMATC_S || mode == YUWELL_FORMATC_T || mode == YUWELL_FORMATC_ST) {
                    sess->settings[CPAP_Mode] = (int)MODE_BILEVEL_FIXED;
                    sess->settings[CPAP_IPAP] = ipap_raw / 10.0;
                    sess->settings[CPAP_EPAP] = epap_raw / 10.0;
                } else if (mode == YUWELL_FORMATC_VGPS) {
                    sess->settings[CPAP_Mode] = (int)MODE_AVAPS;
                    sess->settings[CPAP_IPAP] = ipap_raw / 10.0;
                    sess->settings[CPAP_EPAP] = epap_raw / 10.0;
                } else if (mode == YUWELL_FORMATC_AUTOS) {
                    sess->settings[CPAP_Mode] = (int)MODE_BILEVEL_AUTO_VARIABLE_PS;
                    sess->settings[CPAP_IPAP] = ipap_raw / 10.0;
                    sess->settings[CPAP_EPAP] = epap_raw / 10.0;
                } else {
                    sess->settings[CPAP_Mode] = (int)MODE_UNKNOWN;
                }
            }

            // Samples are every 60 seconds. CPAP/APAP records carry a single pressure at byte 0x0C;
            // BiPAP modes (S/T/ST/VGPS/AUTOS) leave that byte zero and put IPAP/EPAP at 0x02-0x05 instead.
            if (mode == YUWELL_FORMATC_CPAP || mode == YUWELL_FORMATC_APAP) {
                PR->AddEvent(ti + (i * 60000), pressure);
            } else if (mode >= YUWELL_FORMATC_S && mode <= YUWELL_FORMATC_AUTOS) {
                IPAP->AddEvent(ti + (i * 60000), ipap_raw);
                EPAP->AddEvent(ti + (i * 60000), epap_raw);
            }
            TV->AddEvent(ti + (i * 60000), tidal_volume);
            RR->AddEvent(ti + (i * 60000), respiratory_rate);

            if (oai > 0) {
                OA->AddEvent(ti + (i * 60000), 0);
            }
            if (hi > 0) {
                H->AddEvent(ti + (i * 60000), 0);
            }
            if (cai > 0) {
                CA->AddEvent(ti + (i * 60000), 0);
            }
            if (leak_volume > 0) {
                LK->AddEvent(ti + (i * 60000), leak_volume);
            }
        }
        sess->SetChanged(true);
        Sessions[ts] = sess;
        sess->UpdateSummaries();
        mach->AddSession(sess);
    }

    return true;
}

int YuwellFormatC::Open() {
    // Start with m_filePath and do the import
    QStringList model_serials = GetModelSerials();

    Machine *m;

    int c = 0;
    for (int i = 0; i < model_serials.size(); i++) {
        MachineInfo info = m_loader->newInfo();
        info.serial = model_serials[i];
        m = p_profile->CreateMachine(info);
        try {
            if (m) {
                c += OpenMachine(m, model_serials[i]);
                m->Save();
            }
        } catch (OneTypePerDay& e) {
            Q_UNUSED(e)
            p_profile->DelMachine(m);
            QMessageBox::warning(nullptr, QObject::tr("Import Error"),
                                 QObject::tr("This device Record cannot be imported in this profile.")+"\n\n"
                                     +QObject::tr("The Day records overlap with already existing content."),
                                 QMessageBox::Ok);
            delete m;
        }
    }

    return c;
}

bool YuwellFormatD::Detect() {
    /*
     * root - \ MODEL-SERIAL
     *          | RunLog.bys <- 0 byte file
     *          | summer.bys <- 50 byte file containing zeroes
     *          | 00100001 (dir)
     *            \ 0100001d.BYS <- Flow rate data (This file is optional)
     *            | 0100001m.BYS <- Minute summary data
     *            | 0100001s.BYS <- Session summary data
     *
     * Models seen
     * -----------
     * YH-690, YH-680
     *
     * This is the BreathCare III format as confirmed with all units in the range (YH-680/YH-690)
     */
    QDir cardDir(m_filePath);

    QStringList model_serials = GetModelSerials();

    if (model_serials.size() == 0) {
        return false;
    }

    qDebug() << "Found Yuwell format D serial number: " << model_serials;
    return true;
}

QStringList YuwellFormatD::GetModelSerials() {
    /*
     * Find a model/serial number from the inside of a file
     * instead of a directory name. Much more source-of-truth
     */
    QDir cardDir(m_filePath);

    cardDir.setFilter(QDir::NoDotAndDotDot | QDir::Dirs | QDir::NoSymLinks);
    cardDir.setSorting(QDir::Name);

    QFileInfoList dlist = cardDir.entryInfoList();

    QStringList model_serials;

    for (int i = 0; i < dlist.size(); i++) {
        // Iterate until we find our first file with a model/serial number
        QFileInfo fi = dlist.at(i);
        QString filename = fi.fileName();

        if (filename.toUpper().startsWith("YH")) {
            // From here there are sub-directories with individual sessions
            QDir sessionsDir (m_filePath + "/" + filename);

            // There should be an empty RunLog.bys file in this directory
            if (!sessionsDir.exists("RunLog.bys")) {
                continue;
            }
            sessionsDir.setFilter(QDir::NoDotAndDotDot | QDir::Dirs | QDir::NoSymLinks);
            sessionsDir.setSorting(QDir::Name);

            QFileInfoList slist = sessionsDir.entryInfoList();

            for (int j = 0; j < slist.size(); j++) {
                // Each session directory should contain *s.bys containing the serial
                QFileInfo sfi = slist.at(j);
                QString sessionname = sfi.fileName();

                QDir sessionDir (m_filePath + "/" + filename + "/" + sessionname);
                sessionDir.setFilter(QDir::NoDotAndDotDot | QDir::Files | QDir::NoSymLinks);
                sessionDir.setSorting(QDir::Name);

                QStringList filters;
                filters << "*s.bys" << "*s.BYS";
                sessionDir.setNameFilters(filters);

                QFileInfoList flist = sessionDir.entryInfoList();
                if (flist.size() == 0) {
                    continue;
                }

                QFileInfo summary = flist.at(0);
                QFile bysFile(m_filePath + "/" + filename + "/" + sessionname + "/" + summary.fileName());

                if (bysFile.open(QFile::ReadOnly)) {
                    QByteArray header = bysFile.read(0x30);

                    if (header.size() == 0x30) {
                        QDataStream in(header);
                        in.setVersion(QDataStream::Qt_4_8);

                        QByteArray raw_model_serial(16, Qt::Uninitialized);

                        in.skipRawData(0x20);
                        in.readRawData(raw_model_serial.data(), 16);

                        QString model_serial(raw_model_serial);
                        if (model_serial.startsWith("YH") && !model_serials.contains(model_serial)) {
                            model_serials << model_serial;
                        }
                    }
                    bysFile.close();
                }
            }
        }
    }
    return model_serials;
}

int YuwellFormatD::OpenMachine(Machine *mach, const QString & serial)
{
    emit m_loader->updateMessage(QObject::tr("Getting Ready..."));
    emit m_loader->setProgressValue(0);
    QCoreApplication::processEvents();

    QString machinePath = m_filePath + "/" + serial;
    QDir dir(machinePath);

    if (!dir.exists() || (!dir.isReadable())) {
        return -1;
    }

    m_loader->backupData(mach, m_filePath);

    calc_leaks = p_profile->cpap->calculateUnintentionalLeaks();
    lpm4 = p_profile->cpap->custom4cmH2OLeaks();
    lpm20 = p_profile->cpap->custom20cmH2OLeaks();

    dir.setFilter(QDir::NoDotAndDotDot | QDir::Dirs | QDir::NoSymLinks);
    dir.setSorting(QDir::Name);
    QFileInfoList flist = dir.entryInfoList();

    emit m_loader->updateMessage(QObject::tr("Reading session directories..."));
    QCoreApplication::processEvents();

    Sessions.clear();

    QString filename, spath;
    emit m_loader->setProgressMax(flist.size());

    // There's only one file type with an extension .BYS containing a header and line data
    for (int i = 0; i < flist.size(); i++) {
        emit m_loader->setProgressValue(i);
        QFileInfo fi = flist.at(i);
        QString sessionname = fi.fileName();
        spath = machinePath + "/" + sessionname;

        OpenSession(mach, spath);
    }

    int c = Sessions.size();
    qDebug() << "Yuwell Format D Loader found" << c << "sessions";

    emit m_loader->updateMessage(QObject::tr("Finishing up..."));
    QCoreApplication::processEvents();

    m_loader->finish();

    return c;
}

bool YuwellFormatD::OpenSession(Machine *mach, const QString & filename)
{
    QDir sessionDir(filename);

    // Read three files: *s.bys, *m.bys, *d.bys (last is optional)
    // Read from the session file (*s.bys) first to get the session start/stop, mode, FPS level and ramp time
    QStringList sFilters;
    sFilters << "*s.bys";
    sessionDir.setNameFilters(sFilters);

    QFileInfoList flist = sessionDir.entryInfoList();
    if (flist.size() == 0) {
        return false;
    }

    QFileInfo summary = flist.at(0);
    QFile bysFile(filename + "/" + summary.fileName());

    unsigned char start_year, start_month, start_day, start_hour, start_minute, start_second;
    unsigned char finish_year, finish_month, finish_day, finish_hour, finish_minute, finish_second;
    unsigned char mode, fps_level, ramp;

    QDateTime start, finish;
    quint32 ts;
    Session *sess = nullptr;

    if (bysFile.open(QFile::ReadOnly)) {
        QByteArray header = bysFile.read(0x4D);

        if (header.size() != 0x4D) {
            return false;
        }

        QDataStream in(header);
        in.setVersion(QDataStream::Qt_4_8);

        in.skipRawData(2);
        in >> start_year;
        in >> start_month;
        in >> start_day;
        in >> start_hour;
        in >> start_minute;
        in >> start_second;

        in >> finish_year;
        in >> finish_month;
        in >> finish_day;
        in >> finish_hour;
        in >> finish_minute;
        in >> finish_second;

        start = QDateTime(QDate(2000 + start_year, start_month, start_day), QTime(start_hour, start_minute, start_second), QTimeZone::systemTimeZone());
        finish = QDateTime(QDate(2000 + finish_year, finish_month, finish_day), QTime(finish_hour, finish_minute, finish_second), QTimeZone::systemTimeZone());

        ts = start.toSecsSinceEpoch();

        in.skipRawData(18);

        QByteArray raw_model_serial(16, Qt::Uninitialized);
        in.readRawData(raw_model_serial.data(), 16);
        QString model_serial(raw_model_serial);
        mach->setModel(model_serial);

        in.skipRawData(8); // Some rando date in here
        in >> mode;

        in.skipRawData(18);
        in >> fps_level;
        in >> ramp;

        if (!mach->SessionExists(ts)) {
            sess = new Session(mach, ts);
            sess->really_set_first(qint64(ts) * 1000L);
            sess->really_set_last(qint64(finish.toSecsSinceEpoch()) * 1000L);

            if (mode == YUWELL_CPAP) {
                sess->settings[CPAP_Mode] = (int)MODE_CPAP;
                sess->settings[CPAP_Pressure] = -1; // Sentinel value to fill in later
            } else {
                sess->settings[CPAP_Mode] = (int)MODE_APAP;
                sess->settings[CPAP_PressureMin] = -1;
                sess->settings[CPAP_PressureMax] = -1;
            }

            // sess->settings[Yuwell_Humidity] = humidity;
            sess->settings[Yuwell_Ramp] = ramp;
        }
        bysFile.close();
    }

    if (!sess) {
        return false;
    }

    EventList *LK = sess->AddEventList(CPAP_LeakTotal, EVL_Event, 1);
    EventList *PR = sess->AddEventList(CPAP_Pressure, EVL_Event, 0.1F);
    EventList *OA = sess->AddEventList(CPAP_Obstructive, EVL_Event);
    EventList *CA = sess->AddEventList(CPAP_ClearAirway, EVL_Event);
    EventList *H =  sess->AddEventList(CPAP_Hypopnea, EVL_Event);
    EventList *FR =  sess->AddEventList(CPAP_FlowRate, EVL_Event, 1);
    EventList *P =  sess->AddEventList(OXI_Pulse, EVL_Event);
    EventList *O =  sess->AddEventList(OXI_SPO2, EVL_Event);

    QStringList mFilters;
    mFilters << "*m.bys";
    sessionDir.setNameFilters(mFilters);

    QFileInfoList mlist = sessionDir.entryInfoList();
    if (mlist.size() == 0) {
        return false;
    }
    QFileInfo minutes = mlist.at(0);
    QFile bysMFile(filename + "/" + minutes.fileName());

    if (bysMFile.open(QFile::ReadOnly)) {
        QByteArray mHeader = bysMFile.read(0x08);

        if (mHeader.size() != 0x08) {
            return false;
        }
        short int record_count;

        QDataStream in(mHeader);
        in.setVersion(QDataStream::Qt_4_8);
        in.setByteOrder(QDataStream::LittleEndian);

        in.skipRawData(6);

        in >> record_count;

        qint64 ti;
        ti = qint64(ts) * 1000L;

        unsigned char max_pressure = 0;

        // Records start here at 18 bytes each
        for (int i = 0; i < record_count; i++) {
            QCoreApplication::processEvents();
            QByteArray record;

            record = bysMFile.read(18);

            if (record.size() != 18) {
                qWarning() << "Yuwell Session Record Short " << minutes.fileName();
                bysMFile.close();
                return false;
            }

            QDataStream in(record);
            in.setVersion(QDataStream::Qt_4_8);
            in.setByteOrder(QDataStream::LittleEndian);

            unsigned char pressure, oai, cai, hi, leakage, spo2, pulse;
            in >> pressure;
            in.skipRawData(1);
            in >> oai;
            in >> cai;
            in >> hi;
            in.skipRawData(4);
            in >> leakage;
            in.skipRawData(5);
            in >> spo2;
            in >> pulse;
            in.skipRawData(2);

            if (sess->settings[CPAP_Mode] == (int)MODE_CPAP) {
                if (sess->settings[CPAP_Pressure] == -1) {
                    sess->settings[CPAP_Pressure] = pressure / 10.0;
                }
            } else {
                if (sess->settings[CPAP_PressureMin] == -1) {
                    sess->settings[CPAP_PressureMin] = pressure / 10.0;
                }
            }

            if (pressure > max_pressure) {
                max_pressure = pressure;
            }

            PR->AddEvent(ti + (i * 60000), pressure);

            if (oai > 0) {
                OA->AddEvent(ti + (i * 60000), 0);
            }
            if (hi > 0) {
                H->AddEvent(ti + (i * 60000), 0);
            }
            if (cai > 0) {
                CA->AddEvent(ti + (i * 60000), 0);
            }
            if (leakage > 0) {
                LK->AddEvent(ti + (i * 60000), leakage);
            }
            if (pulse > 0 && pulse < 249) {
                P->AddEvent(ti + (i * 60000), pulse);
            }
            if (spo2 > 0) {
                O->AddEvent(ti + (i * 60000), spo2);
            }
        }

        if (sess->settings[CPAP_Mode] == (int)MODE_APAP) {
            sess->settings[CPAP_PressureMax] = max_pressure / 10.0;
        }

        bysMFile.close();
    }
    // Flow rate data, this is optional data and has only been seen with the YH-690
    QStringList dFilters;
    dFilters << "*d.bys";
    sessionDir.setNameFilters(dFilters);

    QFileInfoList dlist = sessionDir.entryInfoList();
    if (dlist.size() > 0) {
        QFileInfo flow_rate = dlist.at(0);
        QFile bysDFile(filename + "/" + flow_rate.fileName());

        if (bysDFile.open(QFile::ReadOnly)) {
            QByteArray mHeader = bysDFile.read(0x08);

            if (mHeader.size() != 0x08) {
                return false;
            }
            short int record_count;

            QDataStream in(mHeader);
            in.setVersion(QDataStream::Qt_4_8);
            in.setByteOrder(QDataStream::LittleEndian);

            in.skipRawData(6);

            in >> record_count;

            qint64 ti;
            ti = qint64(ts) * 1000L;

            // Records start here at 1200 bytes each
            // For 1 minute of flow rate data. Two datasets in each minute
            // 10 flow rate samples per second. 600 samples for each dataset
            // Actual flow rate is in the second dataset.
            for (int i = 0; i < record_count; i++) {
                QCoreApplication::processEvents();
                QByteArray record;

                record = bysDFile.read(1200);

                if (record.size() != 1200) {
                    qWarning() << "Yuwell Session Record Short " << minutes.fileName();
                    bysDFile.close();
                    return false;
                }

                QDataStream in(record);
                in.setVersion(QDataStream::Qt_4_8);
                in.setByteOrder(QDataStream::LittleEndian);

                in.skipRawData(600); // Unsure what the first 600 byte dataset is in each minute
                for (int j = 0; j < 600; j++) {
                    unsigned char flow_rate;
                    in >> flow_rate;

                    // Flow rate events are every 1/10th of a second plus the minute offset
                    FR->AddEvent(ti + (i * 60000) + (j * 100), (signed int)-70 + flow_rate);
                }
            }
            bysDFile.close();
        }
    }
    sess->SetChanged(true);
    Sessions[ts] = sess;
    sess->UpdateSummaries();
    mach->AddSession(sess);

    return true;
}

int YuwellFormatD::Open() {
    // Start with m_filePath and do the import
    QStringList model_serials = GetModelSerials();

    Machine *m;

    int c = 0;
    for (int i = 0; i < model_serials.size(); i++) {
        MachineInfo info = m_loader->newInfo();
        info.serial = model_serials[i];
        m = p_profile->CreateMachine(info);
        try {
            if (m) {
                c += OpenMachine(m, model_serials[i]);
                m->Save();
            }
        } catch (OneTypePerDay& e) {
            Q_UNUSED(e)
            p_profile->DelMachine(m);
            QMessageBox::warning(nullptr, QObject::tr("Import Error"),
                                 QObject::tr("This device Record cannot be imported in this profile.")+"\n\n"
                                     +QObject::tr("The Day records overlap with already existing content."),
                                 QMessageBox::Ok);
            delete m;
        }
    }

    return c;
}

YuwellLoader::YuwellLoader() {
    m_type = MT_CPAP;

    const QString YUWELL_ICON = ":/icons/yuwell.png";

    QString s = newInfo().series;
    m_pixmap_paths[s] = YUWELL_ICON;
    m_pixmaps[s] = QPixmap(YUWELL_ICON);
}

YuwellLoader::~YuwellLoader() {}

bool yuwell_initialized = false;
void YuwellLoader::Register() {
    if (yuwell_initialized)
        return;

    RegisterLoader(new YuwellLoader());

    yuwell_initialized = true;
}

YuwellFormat* YuwellLoader::YuwellFactory(const QString & givenPath) {
    // Add formats here as we find them
    QList<YuwellFormat*> candidates;
    candidates << new YuwellFormatA(*this, givenPath);
    candidates << new YuwellFormatB(*this, givenPath);
    candidates << new YuwellFormatC(*this, givenPath);
    candidates << new YuwellFormatD(*this, givenPath);

    YuwellFormat* match = nullptr;
    for(YuwellFormat* format : candidates) {
        if (match == nullptr && format->Detect()) {
            match = format;
        } else {
            delete format;
        }
    }

    return match;
}

bool YuwellLoader::Detect(const QString & givenPath) {
    /*
     * For each format, detect
     */
    QDir dir(givenPath);

    if (!dir.exists()) {
        return false;
    }

    YuwellFormat* formatter = YuwellFactory(givenPath);
    if (!formatter) {
        return false;
    }

    delete formatter;
    return true;
}

bool YuwellLoader::backupData (Machine * mach, const QString & path) {
    QDir ipath(path);
    QDir bpath(mach->getBackupPath());

    // Compare QDirs rather than QStrings because separators may be different, especially on Windows.
    if (ipath == bpath) {
        // Don't create backups if importing from backup folder
        rebuild_from_backups = true;
        create_backups = false;
    } else {
        rebuild_from_backups = false;
        create_backups = p_profile->session->backupCardData();
    }

    if (rebuild_from_backups || !create_backups)
        return true;

    // Copy input data to backup location
    copyPath(ipath.absolutePath(), bpath.absolutePath(), true);

    return true;
}

int YuwellLoader::Open(const QString & dirPath) {
    // Do the import
    YuwellFormat* formatter = YuwellFactory(dirPath);
    if (formatter) {
        formatter->Open();
        return 1;
    }
    return 0;

}

void YuwellLoader::finish() {
    finishAddingSessions();
}
