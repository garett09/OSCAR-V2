/* Create New Profile Implementation
 *
 * Copyright (c) 2019-2026 The OSCAR Team
 * Copyright (c) 2011-2018 Mark Watkins
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#define TEST_MACROS_ENABLEDoff
#include "test_macros.h"

#include <QMessageBox>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QCryptographicHash>
#include <QFileDialog>
#include <QFile>
#include <QDesktopServices>
#include <QSettings>
#include <QComboBox>
#include <QTimeZone>
#include <QPalette>

#include "SleepLib/profiles.h"
#include "database/profile_repository.h"

#include "newprofile.h"
#include "staticQMessageBox.h"
#include "ui_newprofile.h"
#include "mainwindow.h"
#include "version.h"

extern MainWindow *mainwin;
extern bool openOk;


NewProfile::NewProfile(QWidget *parent, const QString *user) :
    QDialog(parent, Qt::WindowTitleHint | Qt::WindowCloseButtonHint),
    ui(new Ui::NewProfile)
{
    ui->setupUi(this);

    // Force light appearance so this dialog is legible in system dark mode.
    // Qt's ColorScheme::Light hint is not guaranteed on KDE Plasma, which can
    // inject dark palette colors even when the application requests light mode.
    setAutoFillBackground(true);
    QPalette p;
    p.setColor(QPalette::Window, Qt::white);
    setPalette(p);
    setStyleSheet(
        "QDialog { background-color: white; color: black; }"
        "QWidget { background-color: white; color: black; }"
        "QLabel { color: black; background-color: transparent; }"
        "QLineEdit { background-color: white; color: black; border: 1px solid #adadad; border-radius: 2px; }"
        "QTextEdit, QPlainTextEdit, QTextBrowser { background-color: white; color: black; }"
        "QComboBox { background-color: white; color: black; }"
        "QComboBox QAbstractItemView { background-color: white; color: black; }"
        "QDateEdit { background-color: white; color: black; }"
        "QDoubleSpinBox { background-color: white; color: black; }"
        "QCheckBox { color: black; background-color: transparent; }"
        "QGroupBox { color: black; border: 1px solid #c0c0c0; border-radius: 4px;"
        "    margin-top: 8px; padding-top: 4px; }"
        "QGroupBox::title { color: black; subcontrol-origin: margin; left: 8px; }"
        "QPushButton { color: black; background-color: #f0f0f0;"
        "    border: 1px solid #adadad; border-radius: 3px; padding: 3px 8px; }"
        "QPushButton:hover { background-color: #e5f1fb; border-color: #0078d7; }"
        "QPushButton:pressed { background-color: #cce4f7; border-color: #0078d7; }"
        "QPushButton:disabled { color: #a0a0a0; border-color: #d0d0d0; }"
    );

    if (user) {
      originalProfileName=*user;
      ui->userNameEdit->setText(*user);
//    ui->userNameEdit->setText(getUserName());
    }
    QLocale locale = QLocale::system();
    QString shortformat = locale.dateFormat(QLocale::ShortFormat);

    if (!shortformat.toLower().contains("yyyy")) {
        shortformat.replace("yy", "yyyy");
    }

    ui->dobEdit->setDisplayFormat(shortformat);
    ui->dateDiagnosedEdit->setDisplayFormat(shortformat);
    m_firstPage = 0;
    ui->backButton->setEnabled(false);
    ui->nextButton->setEnabled(false);

    ui->stackedWidget->setCurrentIndex(0);
    on_cpapModeCombo_activated(0);
    populateMaskCombo();
    ui->heightEdit2->setVisible(false);
    ui->heightEdit->setDecimals(0);
    ui->heightEdit->setSuffix(QString(" %1").arg(STR_UNIT_CM));

    {
        // Get all available countries
//        QList<QLocale::Country> countries = QLocale::Country::allCountries();
        QMap<QString, QLocale::Country> countryMap;

        for (int i = QLocale::AnyCountry; i <= QLocale::LastCountry; ++i) {
            QLocale::Country country = static_cast<QLocale::Country>(i);
            if (country != QLocale::AnyCountry) {
                QString countryName = QLocale::countryToString(country);
                if (!countryName.isEmpty()) {
                    countryMap.insert(countryName, country);
                }
            }
        }

        // Populate combo box (already sorted by map key)
        for (auto it = countryMap.constBegin(); it != countryMap.constEnd(); ++it) {
            ui->countryCombo->addItem(it.key(), static_cast<int>(it.value()));
        }

        // Set default to system locale country
        QLocale systemLocale = QLocale::system();
        QString systemCountryName = QLocale::countryToString(systemLocale.country());
        int systemIndex = ui->countryCombo->findText(systemCountryName);
        if (systemIndex != -1) {
            ui->countryCombo->setCurrentIndex(systemIndex);
        }
    }

    {
        // timezone list
        // Get system's current time zone
        QTimeZone systemTimeZone = QTimeZone::systemTimeZone();

        // Populate a combo box with all available time zones

        QList<QByteArray> availableTimeZoneIds = QTimeZone::availableTimeZoneIds();
        for (const QByteArray& tzId : availableTimeZoneIds) {
            QTimeZone tz(tzId);
            QDateTime now = QDateTime::currentDateTime();

            // Display format: "America/New_York (EST, UTC-05:00)"
            QString displayText = QString("%1 (%2, UTC%3)")
                                      .arg(QString::fromLatin1(tzId))
                                      .arg(tz.abbreviation(now))
                                      .arg(tz.offsetFromUtc(now) / 3600.0, 0, 'f', 2);

            ui->timezoneCombo->addItem(displayText, tzId);
        }

        // Set the default to system time zone
        int systemIndex = ui->timezoneCombo->findData(systemTimeZone.id());
        if (systemIndex != -1) {
            ui->timezoneCombo->setCurrentIndex(systemIndex);
        }
    }
    ui->versionLabel->setText("");

    ui->textBrowser->setHtml(getIntroHTML());
}

NewProfile::~NewProfile()
{
    delete ui;
}


QString NewProfile::getIntroHTML()
{
    return "<html>"
           "<body>"
           "<div align=center><h1>" + tr("Welcome to the Open Source CPAP Analysis Reporter") + "</h1></div>"

           "<p>" + tr("This software is being designed to assist you in reviewing the data produced by your CPAP Devices and related equipment.")
           + "</p>"

           "<p>" + tr("OSCAR has been released freely under the <a href='qrc:/COPYING'>GNU Public License v3</a>, and comes with no warranty, and without ANY claims to fitness for any purpose.")
           + "</p>"
           "<div align=center><font color=\"red\"><h2>" + tr("PLEASE READ CAREFULLY") + "</h2></font></div>"
           "<p>" + tr("OSCAR is intended merely as a data viewer, and definitely not a substitute for competent medical guidance from your Doctor.")
           + "</p>"

           "<p>" + tr("Accuracy of any data displayed is not and can not be guaranteed.") + "</p>"

           "<p>" + tr("Any reports generated are for PERSONAL USE ONLY, and NOT IN ANY WAY fit for compliance or medical diagnostic purposes.")
           + "</p>"

           "<p>" + tr("The authors will not be held liable for <u>anything</u> related to the use or misuse of this software.")
           + "</p>"

           "<div align=center>"
           "<p><b><font size=+1>" + tr("Use of this software is entirely at your own risk.") +
           "</font></b></p>"

           "<p><i>" + tr("OSCAR is copyright &copy;2011-2018 Mark Watkins and portions &copy;2019-2026 The OSCAR Team") + "<i></p>"
           "</div>"
           "</body>"
           "</html>";
}

// Helper: populate the mask model combo with current 2024-2025 masks grouped by brand
void NewProfile::populateMaskCombo()
{
    QComboBox *combo = ui->maskModelCombo;
    combo->clear();
    combo->addItem(tr("-- None / Not Set --"), QString(""));

    // ResMed - Full Face
    combo->addItem(tr("─── ResMed ─── Full Face ───"), QString("__sep__"));
    combo->addItem(tr("ResMed AirFit F20"),         QString("ResMed AirFit F20"));
    combo->addItem(tr("ResMed AirTouch F20"),       QString("ResMed AirTouch F20"));
    combo->addItem(tr("ResMed AirFit F30"),         QString("ResMed AirFit F30"));
    combo->addItem(tr("ResMed AirFit F30i"),        QString("ResMed AirFit F30i"));
    combo->addItem(tr("ResMed AirFit F40"),         QString("ResMed AirFit F40"));
    // ResMed - Nasal
    combo->addItem(tr("─── ResMed ─── Nasal ───"), QString("__sep__"));
    combo->addItem(tr("ResMed AirFit N20"),         QString("ResMed AirFit N20"));
    combo->addItem(tr("ResMed AirFit N30"),         QString("ResMed AirFit N30"));
    combo->addItem(tr("ResMed AirFit N30i"),        QString("ResMed AirFit N30i"));
    // ResMed - Nasal Pillow
    combo->addItem(tr("─── ResMed ─── Nasal Pillow ───"), QString("__sep__"));
    combo->addItem(tr("ResMed AirFit P10"),         QString("ResMed AirFit P10"));
    combo->addItem(tr("ResMed AirFit P10 For Her"), QString("ResMed AirFit P10 For Her"));
    combo->addItem(tr("ResMed AirFit P30i"),        QString("ResMed AirFit P30i"));

    // Fisher & Paykel - Full Face
    combo->addItem(tr("─── Fisher & Paykel ─── Full Face ───"), QString("__sep__"));
    combo->addItem(tr("Fisher & Paykel Vitera"),         QString("Fisher & Paykel Vitera"));
    combo->addItem(tr("Fisher & Paykel Evora Full Face"), QString("Fisher & Paykel Evora Full Face"));
    // Fisher & Paykel - Nasal
    combo->addItem(tr("─── Fisher & Paykel ─── Nasal ───"), QString("__sep__"));
    combo->addItem(tr("Fisher & Paykel Solo"),   QString("Fisher & Paykel Solo"));
    combo->addItem(tr("Fisher & Paykel Eson 2"), QString("Fisher & Paykel Eson 2"));
    // Fisher & Paykel - Nasal Pillow
    combo->addItem(tr("─── Fisher & Paykel ─── Nasal Pillow ───"), QString("__sep__"));
    combo->addItem(tr("Fisher & Paykel Brevida"),   QString("Fisher & Paykel Brevida"));
    combo->addItem(tr("Fisher & Paykel Nova Micro"), QString("Fisher & Paykel Nova Micro"));
    combo->addItem(tr("Fisher & Paykel Evora"),      QString("Fisher & Paykel Evora"));

    // Philips Respironics - Full Face
    combo->addItem(tr("─── Philips Respironics ─── Full Face ───"), QString("__sep__"));
    combo->addItem(tr("Philips DreamWear Full Face"), QString("Philips DreamWear Full Face"));
    combo->addItem(tr("Philips Amara View"),          QString("Philips Amara View"));
    // Philips Respironics - Nasal
    combo->addItem(tr("─── Philips Respironics ─── Nasal ───"), QString("__sep__"));
    combo->addItem(tr("Philips DreamWear Nasal"), QString("Philips DreamWear Nasal"));
    // Philips Respironics - Nasal Pillow
    combo->addItem(tr("─── Philips Respironics ─── Nasal Pillow ───"), QString("__sep__"));
    combo->addItem(tr("Philips DreamWear Nasal Pillow"), QString("Philips DreamWear Nasal Pillow"));
    combo->addItem(tr("Philips DreamWisp"),              QString("Philips DreamWisp"));

    // Disable / bold separator items
    auto *model = qobject_cast<QStandardItemModel*>(combo->model());
    if (model) {
        for (int i = 0; i < combo->count(); ++i) {
            if (combo->itemData(i).toString() == "__sep__") {
                QStandardItem *si = model->item(i);
                if (si) {
                    si->setFlags(si->flags() & ~Qt::ItemIsEnabled);
                    QFont f = si->font(); f.setBold(true); si->setFont(f);
                }
            }
        }
    }
}

#include <cmath>
int cmToFeetInch( double cm, double& inches ) {
    int tenthInches = std::round(cm * inches_per_cm * 10);
    int feet = std::floor(((double)tenthInches) / 120);
    inches = double(tenthInches - (feet *120))/10;
    return feet;
}

double feetInchToCm( int feet , double inches ) {
    double mm = std::round(10*cms_per_inch*(inches + (double)(feet *12)));
    return mm/10;
}

void NewProfile::on_nextButton_clicked()
{
    const QString xmlext = ".xml";

    int index = ui->stackedWidget->currentIndex();

    switch (index) {
    case 0:
        if (!ui->agreeCheckbox->isChecked()) {
            return;
        }

        // Reload Preferences object
        break;

    case 1:
        if (ui->userNameEdit->text().trimmed().isEmpty()) {
            staticQMessageBox::information(this, STR_MessageBox_Error, tr("Please provide a username for this profile"), QMessageBox::Ok);
            return;
        }

        if (ui->genderCombo->currentIndex() == 0) {
            //QMessageBox::information(this,tr("Notice"),tr("You did not specify Gender."),QMessageBox::Ok);
        }

        break;

    case 2:
        break;

    case 3:
        break;

    default:
        break;
    }

    int max_pages = ui->stackedWidget->count() - 1;

    if (index < max_pages) {
        index++;
        ui->stackedWidget->setCurrentIndex(index);
    } else {
        // Finish button clicked.
        newProfileName = ui->userNameEdit->text().simplified();
        QString profileName;
        if (originalProfileName.isEmpty() ) {
            profileName = newProfileName;
        } else {
            profileName = originalProfileName;
            ui->userNameEdit->setText(originalProfileName);
            //QString profileName = originalProfileName.isEmpty()? newProfileName : originalProfileName;
        }
        //QString profileName = originalProfileName.isEmpty()? newProfileName : originalProfileName;

        if ( staticQMessageBox::question(this, tr("Profile Changes"), tr("Accept and save this information?"),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {

            Profile *profile = Profiles::Get(profileName);
            if (!profile) { // No profile, create one.
                profile = Profiles::Create(profileName);
            }

            Profile &prof = *profile;
            profile->user->setFirstName(ui->firstNameEdit->text());
            profile->user->setLastName(ui->lastNameEdit->text());
            profile->user->setDOB(ui->dobEdit->date());
            profile->user->setEmail(ui->emailEdit->text());
            profile->user->setPhone(ui->phoneEdit->text());
            profile->user->setAddress(ui->addressEdit->toPlainText());

            profile->user->setGender((Gender)ui->genderCombo->currentIndex());

            profile->cpap->setDateDiagnosed(ui->dateDiagnosedEdit->date());
            profile->cpap->setUntreatedAHI(ui->untreatedAHIEdit->value());
            profile->cpap->setMode((CPAPMode)ui->cpapModeCombo->currentIndex());
            profile->cpap->setMinPressure(ui->minPressureEdit->value());
            profile->cpap->setMaxPressure(ui->maxPressureEdit->value());
            profile->cpap->setNotes(ui->cpapNotes->toPlainText());
            // Save mask model
            {
                QString sel = ui->maskModelCombo->currentData().toString();
                if (sel == "__sep__") sel = QString();
                profile->cpap->setMaskDescription(sel);

                MaskType mtype = Mask_Unknown;
                if (!sel.isEmpty()) {
                    if (sel == "ResMed AirFit F20" ||
                        sel == "ResMed AirTouch F20" ||
                        sel == "ResMed AirFit F30" ||
                        sel == "ResMed AirFit F30i" ||
                        sel == "ResMed AirFit F40" ||
                        sel == "Fisher & Paykel Vitera" ||
                        sel == "Fisher & Paykel Evora Full Face" ||
                        sel == "Philips DreamWear Full Face" ||
                        sel == "Philips Amara View") {
                        mtype = Mask_FullFace;
                    } else if (sel == "ResMed AirFit N20" ||
                               sel == "ResMed AirFit N30" ||
                               sel == "ResMed AirFit N30i" ||
                               sel == "Fisher & Paykel Solo" ||
                               sel == "Fisher & Paykel Eson 2" ||
                               sel == "Philips DreamWear Nasal" ||
                               sel == "Philips DreamWisp") {
                        mtype = Mask_StandardNasal;
                    } else if (sel == "ResMed AirFit P10" ||
                               sel == "ResMed AirFit P10 For Her" ||
                               sel == "ResMed AirFit P30i" ||
                               sel == "Fisher & Paykel Brevida" ||
                               sel == "Fisher & Paykel Nova Micro" ||
                               sel == "Fisher & Paykel Evora" ||
                               sel == "Philips DreamWear Nasal Pillow") {
                        mtype = Mask_NasalPillows;
                    }
                }
                profile->cpap->setMaskType(mtype);
            }
            profile->doctor->setName(ui->doctorNameEdit->text());
            profile->doctor->setPracticeName(ui->doctorPracticeEdit->text());
            profile->doctor->setAddress(ui->doctorAddressEdit->toPlainText());
            profile->doctor->setPhone(ui->doctorPhoneEdit->text());
            profile->doctor->setEmail(ui->doctorEmailEdit->text());
            profile->doctor->setPatientID(ui->doctorPatientIDEdit->text());
            profile->user->setTimeZone(ui->timezoneCombo->currentText());
            profile->user->setCountry(ui->countryCombo->currentText());

            UnitSystem us = US_Metric;
            if (ui->heightCombo->currentIndex() == 1) { us = US_English; };
            if (profile->general->unitSystem() != us) {
                profile->general->setUnitSystem(us);
                if (mainwin && mainwin->getDaily()) { mainwin->getDaily()->UnitsChanged(); }
            }

            if (m_height_modified) {
                profile->user->setHeight(m_tmp_height_cm);
                // also call unitsChanged if height also changed. Need for update BMI.
                if (mainwin && mainwin->getDaily()) { mainwin->getDaily()->UnitsChanged(); }
            }
            AppSetting->setProfileName(profileName);

            profile->Save();
            if ( !originalProfileName.isEmpty() && !newProfileName.isEmpty() && (originalProfileName != newProfileName)) {
                QDir profilesDir(p_pref->Get("{home}/Profiles/"));

                // Check for conflicts before attempting the rename.
                ProfileRepository profileRepo;
                bool dirConflict = profilesDir.exists(newProfileName);
                bool dbConflict  = !dirConflict && (profileRepo.findByUsername(newProfileName).id != 0);
                if (dirConflict || dbConflict) {
                    staticQMessageBox::information(this,
                        tr("Profile Name Already In Use"),
                        tr("The name \"%1\" is already used by another profile. Please choose a different name.").arg(newProfileName),
                        QMessageBox::Ok);
                    index=1;
                    ui->stackedWidget->setCurrentIndex(index);
                    ui->userNameEdit->setText(newProfileName);
                } else if (profilesDir.exists(originalProfileName)) {
                    bool status = profilesDir.rename(originalProfileName, newProfileName);
                    if (status) {  // successful rename
                        // Fix the in-memory map: remove old key, insert new with correct pointer.
                        Profiles::profiles.remove(originalProfileName);
                        Profiles::profiles[newProfileName] = profile;

                        if (profile == p_profile) {
                            // Renaming the currently open profile: update settings and restart
                            // so all in-memory state (window title, paths, tabs) picks up the new name.
                            AppSetting->setProfileName(newProfileName);
                            if (mainwin) mainwin->CloseProfile();
                            QCoreApplication::processEvents();
                            // Update the database AFTER CloseProfile() so that Save() inside
                            // CloseProfile() can still find "originalProfileName" in the DB.
                            ProfileData profileData = profileRepo.findByUsername(originalProfileName);
                            if (profileData.id != 0) {
                                profileData.username = newProfileName;
                                profileData.dataFolder = QString("%PROFDIR%/") + newProfileName;
                                profileRepo.update(profileData);
                            } else {
                                qWarning() << "NewProfile: could not find profile in DB for rename:" << originalProfileName;
                            }
                            mainwin->RestartApplication();
                            QCoreApplication::processEvents();
                            exit(0);
                        } else {
                            // Renaming a non-open profile: update the DB and refresh the
                            // profile selector — no restart needed.
                            ProfileData profileData = profileRepo.findByUsername(originalProfileName);
                            if (profileData.id != 0) {
                                profileData.username = newProfileName;
                                profileData.dataFolder = QString("%PROFDIR%/") + newProfileName;
                                profileRepo.update(profileData);
                            } else {
                                qWarning() << "NewProfile: could not find profile in DB for rename:" << originalProfileName;
                            }
                            if (mainwin) {
                                mainwin->refreshProfileSelector();
                            }
                            this->accept();
                        }
                    } else {
                        staticQMessageBox::information(this,
                            tr("Rename Failed"),
                            tr("Could not rename the profile directory. Check that no files in the profile are open and that you have write permission."),
                            QMessageBox::Ok);
                        index=1;
                        ui->stackedWidget->setCurrentIndex(index);
                        ui->userNameEdit->setText(newProfileName);
                    }
                } else {
                    qWarning() << "Rename Profile failed";
                }
            } else {
                if (mainwin) {
                    mainwin->GenerateStatistics();
                }
                this->accept();
            }
        }
    }

    if (index >= max_pages) {
        ui->nextButton->setText(tr("Finish"));
    } else {
        ui->nextButton->setText(tr("Next"));
    }

    ui->backButton->setEnabled(true);

}

void NewProfile::on_backButton_clicked()
{
    ui->nextButton->setText(tr("Next"));

    if (ui->stackedWidget->currentIndex() > m_firstPage) {
        ui->stackedWidget->setCurrentIndex(ui->stackedWidget->currentIndex() - 1);
    }

    if (ui->stackedWidget->currentIndex() == m_firstPage) {
        ui->backButton->setEnabled(false);
    } else {
        ui->backButton->setEnabled(true);
    }


}


void NewProfile::on_cpapModeCombo_activated(int index)
{
    if (index == 0) {
        ui->maxPressureEdit->setVisible(false);
    } else {
        ui->maxPressureEdit->setVisible(true);
    }
}

void NewProfile::on_agreeCheckbox_clicked(bool checked)
{
    ui->nextButton->setEnabled(checked);
}

void NewProfile::skipWelcomeScreen()
{
    ui->agreeCheckbox->setChecked(true);
    ui->stackedWidget->setCurrentIndex(m_firstPage = 1);
    ui->backButton->setEnabled(false);
    ui->nextButton->setEnabled(true);
}

void NewProfile::edit(const QString name)
{
    skipWelcomeScreen();
    Profile *profile = Profiles::Get(name);

    if (!profile) {
        profile = Profiles::Create(name);
    }

    ui->userNameEdit->setText(name);
    // ui->userNameEdit->setReadOnly(true);
    ui->firstNameEdit->setText(profile->user->firstName());
    ui->lastNameEdit->setText(profile->user->lastName());

    ui->dobEdit->setDate(profile->user->DOB());

    if (profile->user->gender() == Male) {
        ui->genderCombo->setCurrentIndex(1);
    } else if (profile->user->gender() == Female) {
        ui->genderCombo->setCurrentIndex(2);
    } else { ui->genderCombo->setCurrentIndex(0); }

    ui->heightEdit->setValue(profile->user->height());
    ui->addressEdit->setText(profile->user->address());
    ui->emailEdit->setText(profile->user->email());
    ui->phoneEdit->setText(profile->user->phone());
    ui->dateDiagnosedEdit->setDate(profile->cpap->dateDiagnosed());
    ui->cpapNotes->clear();
    ui->cpapNotes->appendPlainText(profile->cpap->notes());
    ui->minPressureEdit->setValue(profile->cpap->minPressure());
    ui->maxPressureEdit->setValue(profile->cpap->maxPressure());
    ui->untreatedAHIEdit->setValue(profile->cpap->untreatedAHI());
    ui->cpapModeCombo->setCurrentIndex((int)profile->cpap->mode());
    // Load mask model
    {
        QString saved = profile->cpap->maskDescription();
        int idx = 0;
        if (!saved.isEmpty()) {
            for (int i = 0; i < ui->maskModelCombo->count(); ++i) {
                if (ui->maskModelCombo->itemData(i).toString() == saved) { idx = i; break; }
            }
        }
        ui->maskModelCombo->setCurrentIndex(idx);
    }
    on_cpapModeCombo_activated(profile->cpap->mode());

    ui->doctorNameEdit->setText(profile->doctor->name());
    ui->doctorPracticeEdit->setText(profile->doctor->practiceName());
    ui->doctorPhoneEdit->setText(profile->doctor->phone());
    ui->doctorEmailEdit->setText(profile->doctor->email());
    ui->doctorAddressEdit->setText(profile->doctor->address());
    ui->doctorPatientIDEdit->setText(profile->doctor->patientID());

    // If we can't find the timezone (old data), just set the local time zone as a best guess
    int i = ui->timezoneCombo->findText(profile->user->timeZone());
    if (i == -1) {
        QTimeZone systemTimeZone = QTimeZone::systemTimeZone();
        int systemIndex = ui->timezoneCombo->findData(systemTimeZone.id());
        if (systemIndex != -1) {
            ui->timezoneCombo->setCurrentIndex(systemIndex);
        }
    }
    else
        ui->timezoneCombo->setCurrentIndex(i);
    i = ui->countryCombo->findText(profile->user->country());
    if (i == -1) {
        // Set default to system locale country
        QString systemCountryName = tr("Select Country");
        ui->countryCombo->addItem(systemCountryName);
        i = ui->countryCombo->findText(systemCountryName);
    }
    ui->countryCombo->setCurrentIndex(i);

    UnitSystem us = profile->general->unitSystem();
    i = (int)us - 1;

    if (i < 0) { i = 0; }

    ui->heightCombo->setCurrentIndex(i);

    m_tmp_height_cm = profile->user->height();
    m_height_modified = false;
    on_heightCombo_currentIndexChanged(i);
}

void NewProfile::on_heightCombo_currentIndexChanged(int index)
{
    ui->heightEdit->blockSignals(true);
    ui->heightEdit2->blockSignals(true);
    if (index == 0) {
        //metric
        ui->heightEdit->setDecimals(1);
        ui->heightEdit->setSuffix(QString(" %1").arg(STR_UNIT_CM));
        ui->heightEdit->setValue(m_tmp_height_cm);
        ui->heightEdit2->setVisible(false);
    } else {        //english
        ui->heightEdit->setDecimals(0);  // feet are always a whole number.
        ui->heightEdit2->setDecimals(1);  // inches can be seen as double.
        ui->heightEdit->setSuffix(QString(" %1").arg(STR_UNIT_FOOT));
        ui->heightEdit2->setVisible(true);
        ui->heightEdit2->setSuffix(QString(" %1").arg(STR_UNIT_INCH));
        double inches=0;
        ui->heightEdit->setValue(cmToFeetInch(m_tmp_height_cm,inches));
        ui->heightEdit2->setValue(inches);
    }
    ui->heightEdit->blockSignals(false);
    ui->heightEdit2->blockSignals(false);
}

void NewProfile::on_heightEdit_valueChanged(double ) {
    m_height_modified = true;
    double cm = ui->heightEdit->value();
    if (ui->heightCombo->currentIndex() == 1) {
        //US_English;
        cm = feetInchToCm (cm,ui->heightEdit2->value());
    };
    m_tmp_height_cm = cm;
}

void NewProfile::on_heightEdit2_valueChanged(double value) {
    on_heightEdit_valueChanged(value);
}

void NewProfile::on_textBrowser_anchorClicked(const QUrl &arg1)
{
    QDialog *dlg = new QDialog(this);
    dlg->setMinimumWidth(600);
    dlg->setMinimumHeight(500);
    QVBoxLayout *layout = new QVBoxLayout();
    dlg->setLayout(layout);
    QTextBrowser *browser = new QTextBrowser(this);
    dlg->layout()->addWidget(browser);
    QPushButton *button = new QPushButton(tr("Close this window"), browser);

    QFile f(arg1.toString().replace("qrc:", ":"));
    openOk = f.open(QIODevice::ReadOnly);
    QTextStream ts(&f);
    QString text = ts.readAll();
    connect(button, SIGNAL(clicked()), dlg, SLOT(close()));
    dlg->layout()->addWidget(button);
    browser->setPlainText(text);
    dlg->exec();

    delete dlg;
}
