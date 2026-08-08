#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QMessageBox>
#include <algorithm>
#include <memory>

#include "SleepLib/loader_plugins/bmcDataParsing.h"
#include "SleepLib/loader_plugins/bmcG3xDataParsing.h"
#include "SleepLib/loader_plugins/bmcg3x_loader.h"

BmcG3xLoader::BmcG3xLoader()
    : BmcLoader()
{
}

bool BmcG3xLoader::Detect(const QString& givenpath)
{
    QDir dir(givenpath);
    if (!dir.exists()) {
        return false;
    }

    // Keep legacy BMC (.USR-based) cards exclusively owned by BmcLoader.
    if (BmcData::DirectoryHasBmcData(givenpath)) {
        return false;
    }

    return BmcG3xData::DirectoryHasBmcG3xData(givenpath);
}

MachineInfo BmcG3xLoader::PeekInfo(const QString& path)
{
    if (!Detect(path)) {
        return MachineInfo();
    }

    auto parser = std::make_unique<BmcG3xData>(path);
    auto bmcMachineInfo = parser->ReadMachineInfo();

    MachineInfo info = newInfo();
    info.type = MachineType::MT_CPAP;
    info.brand = "BMC";
    info.model = bmcMachineInfo.Model;
    info.modelnumber = bmcMachineInfo.Model;
    info.series = "BMC";
    info.serial = bmcMachineInfo.SerialNumber;
    info.version = bmcg3x_version;
    if (!bmcMachineInfo.FirmwareVersion.isEmpty()) {
        info.properties["firmware"] = bmcMachineInfo.FirmwareVersion;
    }

    return info;
}

int BmcG3xLoader::Open(const QString& dirpath)
{
    this->sessionsLoaded = 0;

    QCoreApplication::processEvents();
    emit updateMessage(QObject::tr("Reading data..."));
    QCoreApplication::processEvents();

    const auto machine_info = PeekInfo(dirpath);
    auto parser = std::make_unique<BmcG3xData>(dirpath);
    parser->ReadData();

    QCoreApplication::processEvents();
    emit updateMessage(QObject::tr("Find sessions to import..."));
    QCoreApplication::processEvents();

    QDate firstImportDay = QDate(2000, 1, 1);

    Machine* mach = p_profile->lookupMachine(machine_info.serial, machine_info.loadername);
    if (mach) {
        qDebug() << "We have imported data for this machine before";
        mach->setInfo(machine_info);
        QDate lastDate = mach->LastDay();
        firstImportDay = lastDate;
        QDate purgeDate = mach->purgeDate();
        if (purgeDate.isValid()) {
            firstImportDay = std::min(firstImportDay, purgeDate);
        }
    } else {
        qDebug() << "We haven't imported data for this machine before";
        mach = p_profile->CreateMachine(machine_info);
    }

    // Warn if the firmware version has not been tested with this loader.
    // Tested user-facing versions: G3-2.11.x.x (SC.72), G3-2.12.x.x (SC.74/SC.75) and
    // E5-1.02.x.x on the E5 B25A Plus.
    // The version string comes from the .log file (e.g. "G3-2.11.02.33", "E5-1.02.05.02")
    // and matches what PAP-Link and the device display report.  If the .log was
    // unavailable, the fallback is the IDX internal build string, which has an "SC" field
    // in place of the release number (e.g. "G3-2.SC.72.01", "E5-1.SC.00.22.22") — hence the
    // separate contains() tests below.
    const QString fwVersion = machine_info.properties.value("firmware");
    if (!fwVersion.isEmpty()) {
        const bool knownFirmware = fwVersion.startsWith("G3-2.11.") ||
                                   fwVersion.startsWith("G3-2.12.") ||
                                   fwVersion.startsWith("E5-1.02.") ||
                                   fwVersion.contains("SC.72") ||        // IDX fallback
                                   fwVersion.contains("SC.74") ||        // IDX fallback
                                   fwVersion.contains("SC.75") ||        // IDX fallback
                                   fwVersion.startsWith("E5-1.SC.00.");  // IDX fallback
        if (!knownFirmware) {
            // Also record this in the debug log: the dialog is transient, but a support
            // request or bug report will normally include the log rather than a screenshot.
            qWarning() << "BmcG3xLoader: UNTESTED FIRMWARE" << fwVersion
                       << "- model" << machine_info.model
                       << "serial" << machine_info.serial
                       << "- import will continue";
            QMessageBox::information(QApplication::activeWindow(),
                QObject::tr("BMC G3X — Untested Firmware"),
                QObject::tr("Your BMC G3X device is running firmware \"%1\", which has not been tested with this version of OSCAR.").arg(fwVersion) + "\n\n" +
                QObject::tr("It may be similar enough to known firmware versions that import works correctly, but the OSCAR developers would like a .zip copy of this device's SD card to verify support. Import will continue."),
                QMessageBox::Ok);
        }
    }

    // Warn if the model has not been tested with this loader.
    // Tested models (from IDX product name field 0x0100): "G3 A20", "G3 B20A" and
    // "E5 B25A Plus" (bilevel; AutoS settings verified against PAP-Link).
    const QString model = machine_info.model;
    if (!model.isEmpty()) {
        const bool knownModel = model == QLatin1String("G3 A20") ||
                                model == QLatin1String("G3 B20A") ||
                                model == QLatin1String("E5 B25A Plus");
        if (!knownModel) {
            // Also record this in the debug log — see the firmware check above.
            qWarning() << "BmcG3xLoader: UNTESTED MODEL" << model
                       << "- firmware" << fwVersion
                       << "serial" << machine_info.serial
                       << "- import will continue";
            QMessageBox::information(QApplication::activeWindow(),
                QObject::tr("BMC G3X — Untested Model"),
                QObject::tr("Your BMC G3X device model \"%1\" has not been tested with this version of OSCAR.").arg(model) + "\n\n" +
                QObject::tr("It may be similar enough to known models that import works correctly, but the OSCAR developers would like a .zip copy of this device's SD card to verify support. Import will continue."),
                QMessageBox::Ok);
        }
    }

    QDateTime ignoreBefore = p_profile->session->ignoreOlderSessionsDate();
    bool ignoreOldSessions = p_profile->session->ignoreOlderSessions();

    if (ignoreOldSessions && (ignoreBefore.date() > firstImportDay)) {
        firstImportDay = ignoreBefore.date();
    }
    qDebug() << "First day to import: " << firstImportDay.toString();

    if (mach->getDatabaseId() == 0) {
        qDebug() << "BmcG3xLoader::Open: Saving machine to database before runTasks()";
        if (!mach->SaveToDatabase()) {
            qWarning() << "BmcG3xLoader::Open: Failed to save machine to database - session/event tables may stay empty";
        } else {
            qDebug() << "BmcG3xLoader::Open: Machine saved to database with ID" << mach->getDatabaseId();
        }
    }

    QString backupPath = mach->getBackupPath();
    if (QDir::cleanPath(dirpath) == QDir::cleanPath(backupPath)) {
        qDebug() << "BmcG3xLoader::Open: input is the backup directory, skipping backup creation";
    } else {
        emit updateMessage(QObject::tr("Creating data backup..."));
        QCoreApplication::processEvents();

        QDir backupDir(backupPath);
        if (backupDir.exists(backupPath)) {
            backupDir.removeRecursively();
        }
        backupDir.mkpath(backupPath);
        copyPath(dirpath, backupPath);
    }

    QList<BmcDataLink> linksToImport;
    const QList<BmcDataLink>& parserLinks = parser->GetSessionLinks();
    for (auto& link : parserLinks) {
        if (link.UsrSession.StartTimestamp.date() >= firstImportDay) {
            linksToImport.append(link);
        }
    }

    emit updateMessage(QObject::tr("Starting import..."));
    emit setProgressMax(linksToImport.length());
    emit setProgressValue(0);
    QCoreApplication::processEvents();

    for (int i = 0; i < linksToImport.length(); i++) {
        BmcDataLink* link = (BmcDataLink*)&linksToImport.at(i);
        queTask(new BmcLoaderTask(this, mach, parser.get(), link, linksToImport.length(), i));
    }

    runTasks();
    mach->Save();
    QCoreApplication::processEvents();

    return this->sessionsLoaded;
}

void BmcG3xLoader::initChannels()
{
    // Channels are shared with the legacy BMC loader and should only be registered once.
}

bool bmcg3x_initialized = false;
void BmcG3xLoader::Register()
{
    if (bmcg3x_initialized) {
        return;
    }

    qDebug() << "Registering BMC G3X Loader";
    RegisterLoader(new BmcG3xLoader());
    bmcg3x_initialized = true;
}
