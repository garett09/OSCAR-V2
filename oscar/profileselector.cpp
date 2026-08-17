/* Profile Selector Implementation
 *
 * Copyright (c) 2019-2026 The OSCAR Team
 * Copyright (c) 2018 Mark Watkins
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#define TEST_MACROS_ENABLEDoff
#include "test_macros.h"

#include <QMessageBox>
#include <QProgressDialog>
#include <QFileInfo>
#include <QDir>

#include "profileselector.h"
#include "ui_profileselector.h"


#include "SleepLib/profiles.h"
#include "daily.h"
//#include "overview.h"
//#include "statistics.h"
#include "mainwindow.h"
#include "newprofile.h"
#include "cprogressbar.h"
//#include "version.h"

// Database includes
#include "database/profile_repository.h"
#include "database/machine_repository.h"
#include "database/user_info_repository.h"
#include "database/app_preferences_repository.h"
#include "database/database_manager.h"

static const QString kSortPrefCategory = QStringLiteral("ProfileSelector");
static const QString kSortPrefColumnKey = QStringLiteral("sortColumn");
static const QString kSortPrefOrderKey  = QStringLiteral("sortOrder");

extern MainWindow * mainwin;

MySortFilterProxyModel2::MySortFilterProxyModel2(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    dateColumn = 4;
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

bool MySortFilterProxyModel2::filterAcceptsRow(int sourceRow,
        const QModelIndex &sourceParent) const
{
    QModelIndex index0 = sourceModel()->index(sourceRow, 0, sourceParent);
    QModelIndex index1 = sourceModel()->index(sourceRow, 1, sourceParent);
    QModelIndex index2 = sourceModel()->index(sourceRow, 2, sourceParent);
    QModelIndex index5 = sourceModel()->index(sourceRow, 5, sourceParent);
#if QT_VERSION < QT_VERSION_CHECK(6,0,0)
    return (sourceModel()->data(index0).toString().contains(filterRegExp())
        || sourceModel()->data(index1).toString().contains(filterRegExp())
        || sourceModel()->data(index2).toString().contains(filterRegExp())
        || sourceModel()->data(index5).toString().contains(filterRegExp())
        );
#else
    QRegularExpression filterExp = filterRegularExpression();
    return (sourceModel()->data(index0).toString().contains(filterExp)
            || sourceModel()->data(index1).toString().contains(filterExp)
            || sourceModel()->data(index2).toString().contains(filterExp)
            || sourceModel()->data(index5).toString().contains(filterExp)
           );
#endif
}


ProfileSelector::ProfileSelector(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ProfileSelector)
{
    ui->setupUi(this);

    // Force light appearance so the profile selector is legible in system dark mode.
    // QPalette changes are ignored by native Windows style renderers, so use QSS instead.
    // Qt::setColorScheme() (Qt 6.5+) is unavailable on the minimum supported versions.
    setAutoFillBackground(true);
    QPalette p;
    p.setColor(QPalette::Window, Qt::white);
    setPalette(p);
    setStyleSheet(
        "QTableView { background-color: white; color: black; alternate-background-color: #f2f2f2; }"
        "QTableView::item:selected { background-color: #3a7fc2; color: white; }"
        "QLabel { color: black; background-color: transparent; }"
        "QPushButton { color: black; background-color: #f0f0f0;"
        "    border: 1px solid #adadad; border-radius: 3px; padding: 3px 8px; }"
        "QPushButton:hover { background-color: #e5f1fb; border-color: #0078d7; }"
        "QPushButton:pressed { background-color: #cce4f7; border-color: #0078d7; }"
        "QPushButton:disabled { color: #a0a0a0; border-color: #d0d0d0; }"
        "QToolButton { color: black; }"
        "QLineEdit { background-color: white; color: black; border: 1px solid #adadad; border-radius: 2px; }"
        "QGroupBox { color: black; border: 1px solid #c0c0c0; border-radius: 4px;"
        "    margin-top: 8px; padding-top: 4px; }"
        "QGroupBox::title { color: black; subcontrol-origin: margin; left: 8px; }"
        "#frame_2 { background-color: white; }"
    );

    model = nullptr;
    proxy = nullptr;

    showDiskUsage = false;  // in case I want to preference it later
    on_diskSpaceInfo_linkActivated(showDiskUsage ? "show" : "hide");

    ui->versionLabel->setText("");
    ui->diskSpaceInfo->setVisible(false);

    QItemSelectionModel * sm = ui->profileView->selectionModel();
    if (sm)
        connect(sm, SIGNAL(currentRowChanged(QModelIndex,QModelIndex)), this, SLOT(on_selectionChanged(QModelIndex,QModelIndex)));
    ui->buttonEditProfile->setEnabled(false);
    ui->buttonOpenProfile->setEnabled(false);

    connect(ui->profileView->horizontalHeader(), &QHeaderView::sortIndicatorChanged,
            this, [](int column, Qt::SortOrder order) {
                if (!DatabaseManager::instance().isOpen()) return;
                AppPreferencesRepository repo;
                repo.save(kSortPrefCategory, kSortPrefColumnKey, column);
                repo.save(kSortPrefCategory, kSortPrefOrderKey,  static_cast<int>(order));
            });
}

ProfileSelector::~ProfileSelector()
{
    QItemSelectionModel * sm = ui->profileView->selectionModel();
    disconnect(sm, SIGNAL(currentRowChanged(QModelIndex,QModelIndex)), this, SLOT(on_selectionChanged(QModelIndex,QModelIndex)));
    delete ui;
}

const Qt::GlobalColor openProfileHighlightColor = Qt::darkGreen;

void ProfileSelector::updateProfileList()
{
    QItemSelectionModel * sm = ui->profileView->selectionModel();
    if (sm) disconnect(sm, SIGNAL(currentRowChanged(QModelIndex,QModelIndex)), this, SLOT(on_selectionChanged(QModelIndex,QModelIndex)));

    QString name;
    int w=0;
    if (proxy) delete proxy;
    if (model) delete model;

    const int columns = 6;
    model = new QStandardItemModel(0, columns, this);
    model->setHeaderData(0, Qt::Horizontal, tr("Profile"));
    model->setHeaderData(1, Qt::Horizontal, tr("Ventilator Brand"));
    model->setHeaderData(2, Qt::Horizontal, tr("Ventilator Model"));
    model->setHeaderData(3, Qt::Horizontal, tr("Other Data"));
    model->setHeaderData(4, Qt::Horizontal, tr("Last Imported"));
    model->setHeaderData(5, Qt::Horizontal, tr("Name"));

    ui->profileView->setStyleSheet("QHeaderView::section { background-color: lightgrey; color: black; }");

    int row = 0;
//    int sel = -1;

    QFontMetrics fm(ui->profileView->font());
    //#if QT_VERSION < QT_VERSION_CHECK(5,11,0) // not sure when this was fixed
        QPalette palette = ui->profileView->palette();
        palette.setColor(QPalette::Highlight, "#3a7fc2");
        palette.setColor(QPalette::HighlightedText, "white");
        ui->profileView->setPalette(palette);
    //#endif

    ui->profileView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->profileView->setFocusPolicy(Qt::NoFocus);
//    ui->profileView->setSelectionMode(QAbstractItemView::NoSelection);
    ui->profileView->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->profileView->setSelectionMode(QAbstractItemView::SingleSelection);

    // Try database first for fast profile list display
    ProfileRepository profileRepo;
    MachineRepository machineRepo;
    UserInfoRepository userRepo;
    
    QList<ProfileData> dbProfiles = profileRepo.findAll();
    
    if (!dbProfiles.isEmpty()) {
        qDebug() << "ProfileSelector: Loading profile list from database (" << dbProfiles.size() << "profiles)";
        
        for (const ProfileData& profileData : dbProfiles) {
            name = profileData.username;
            
            model->insertRows(row, 1, QModelIndex());
            model->setData(model->index(row, 0, QModelIndex()), name);
            model->setData(model->index(row, 0, QModelIndex()), name, Qt::UserRole+2);
            
            // Get user info from database
            UserInfoData userInfo = userRepo.findByProfile(profileData.id);
            QString usersname;
            if (!userInfo.lastName.isEmpty()) {
                usersname = formattedName(userInfo.lastName, userInfo.firstName);
            }
            model->setData(model->index(row, 5, QModelIndex()), usersname);
            model->setData(model->index(row, 5, QModelIndex()), nameSortKey(userInfo.lastName, userInfo.firstName), MySortFilterProxyModel2::NameSortRole);
            model->setData(model->index(row, 3, QModelIndex()), tr("Id: ") + QString::number(userInfo.id));

            // Get most recent CPAP machine from database
            QList<MachineData> allMachines = machineRepo.findByProfile(profileData.id);
            
            // Filter for CPAP machines and find most recent
            MachineData* mostRecent = nullptr;
            QDateTime latestImport;
            
            for (MachineData& mach : allMachines) {
                if (mach.machineType == MT_CPAP) {
                    QDateTime imported = QDateTime::fromString(mach.lastImported, Qt::ISODate);
                    if (!mostRecent || imported > latestImport) {
                        mostRecent = &mach;
                        latestImport = imported;
                    }
                }
            }
            
            if (mostRecent) {
                model->setData(model->index(row, 1, QModelIndex()), mostRecent->brand);
                model->setData(model->index(row, 2, QModelIndex()), mostRecent->model);
                model->setData(model->index(row, 4, QModelIndex()), 
                               latestImport.toString(QLocale::system().dateFormat(QLocale::ShortFormat)));
            }
            
            // Highlight currently open profile
            QBrush bg = QColor(Qt::black);
            QFont font = QApplication::font();
            if (p_profile && p_profile->user->userName() == name) {
                bg = QBrush(openProfileHighlightColor);
                font.setBold(true);
            }
            for (int i=0; i<columns; i++) {
                model->setData(model->index(row, i, QModelIndex()), bg, Qt::ForegroundRole);
            }
            
            QRect rect = fm.boundingRect(name);
            if (rect.width() > w) w = rect.width();
            
            row++;
        }
    }

    /***
      else {
        qDebug() << "ProfileSelector: Database empty, falling back to Profiles::profiles map";
        
        // Fall back to old method if database is empty
        QMap<QString, Profile *>::iterator pi;
        for (pi = Profiles::profiles.begin(); pi != Profiles::profiles.end(); pi++) {
            Profile *prof = pi.value();
            name = pi.key();

            Machine * mach = prof->GetMachine(MT_CPAP);  // only interested in last cpap machine...
            if (!mach) {
                qDebug() << "Couldn't find device info for" << name;
            }

            model->insertRows(row, 1, QModelIndex());
            QString usersname;
            if (!prof->user->lastName().isEmpty()) {
                usersname = formattedName(prof->user->lastName(), prof->user->firstName());
            }

            model->setData(model->index(row, 0, QModelIndex()), name);
            model->setData(model->index(row, 0, QModelIndex()), name, Qt::UserRole+2);
            model->setData(model->index(row, 5, QModelIndex()), usersname);
            model->setData(model->index(row, 5, QModelIndex()), nameSortKey(prof->user->lastName(), prof->user->firstName()), MySortFilterProxyModel2::NameSortRole);
            if (mach) {
                model->setData(model->index(row, 1, QModelIndex()), mach->brand());
                model->setData(model->index(row, 2, QModelIndex()), mach->model());
                model->setData(model->index(row, 4, QModelIndex()), mach->lastImported().toString(QLocale::system().dateFormat(QLocale::ShortFormat)));
            }
            QBrush bg = QColor(Qt::black);
            QFont font = QApplication::font();
            if (prof == p_profile) {
                bg = QBrush(openProfileHighlightColor);
                font.setBold(true);
            }
            for (int i=0; i<columns; i++) {
                model->setData(model->index(row, i, QModelIndex()), bg, Qt::ForegroundRole);
            }

            QRect rect = fm.boundingRect(name);
            if (rect.width() > w) w = rect.width();

            row++;
        }
    }
    w+=20;
***/
//    ui->profileView->setMinimumWidth(w);

    // Rebuilding the model always replaces the selection model, so any previously
    // selected profile's info is no longer valid. Reset the panel and buttons to
    // their no-selection defaults; on_selectionChanged() repopulates them if/when
    // a row is (re)selected, and the currently-open profile's info is restored below.
    ui->profileInfoLabel->setText(row == 0 ? tr("You must create a profile")
                                            : tr("Please select or create a profile..."));
    ui->profileInfoGroupBox->setTitle(tr("Profile: None"));
    ui->diskSpaceInfo->setVisible(false);
    ui->buttonOpenProfile->setEnabled(false);
    ui->buttonEditProfile->setEnabled(false);

    proxy = new MySortFilterProxyModel2(this);
    proxy->setSourceModel(model);
    proxy->setSortCaseSensitivity(Qt::CaseInsensitive);

    ui->profileView->setModel(proxy);
    ui->profileView->setSortingEnabled(true);

    {
        int col = 0;
        Qt::SortOrder order = Qt::AscendingOrder;
        if (DatabaseManager::instance().isOpen()) {
            AppPreferencesRepository repo;
            const QList<AppPrefData> rows = repo.loadByCategory(kSortPrefCategory);
            for (const AppPrefData& row : rows) {
                if (row.key == kSortPrefColumnKey)     col   = row.value.toInt();
                else if (row.key == kSortPrefOrderKey) order = static_cast<Qt::SortOrder>(row.value.toInt());
            }
        }
        ui->profileView->sortByColumn(col, order);
    }

    QHeaderView *headerView = ui->profileView->horizontalHeader();
    headerView->setStretchLastSection(true);
    headerView->setSectionResizeMode(QHeaderView::Stretch);

    sm = ui->profileView->selectionModel();
    connect(sm, SIGNAL(currentRowChanged(QModelIndex,QModelIndex)), this, SLOT(on_selectionChanged(QModelIndex,QModelIndex)));

    // The rebuild above left the panel/buttons at their no-selection defaults.
    // If a profile is currently open, restore its info instead of leaving it blank.
    if (p_profile) {
        updateProfileHighlight(p_profile->user->userName());
    }
}

void ProfileSelector::updateProfileHighlight(QString name)
{

    QFont font = QApplication::font();
    font.setBold(false);
    QBrush bg = QColor(Qt::black);
    for (int row=0;row < model->rowCount(); row++) {
        for (int i=0; i<model->columnCount(); i++) {
            QModelIndex idx = model->index(row, i, QModelIndex());
            model->setData(idx, bg, Qt::ForegroundRole);
            model->setData(idx, font, Qt::FontRole);
        }
    }

    bg = QBrush(openProfileHighlightColor);
    font.setBold(true);

    for (int row=0;row < proxy->rowCount(); row++) {
        QModelIndex idx = proxy->index(row, 0, QModelIndex());

        if (proxy->data(idx).toString().compare(name)==0) {
            on_selectionChanged(idx, QModelIndex());

            for (int i=0; i<proxy->columnCount(); i++) {
                idx = proxy->index(row, i, QModelIndex());
                proxy->setData(idx, bg, Qt::ForegroundRole);
                proxy->setData(idx, font, Qt::FontRole);
            }
            break;
        }
    }
}

Profile *ProfileSelector::SelectProfile(QString profname)
{
    auto pit = Profiles::profiles.find(profname);
    if (pit == Profiles::profiles.end()) return nullptr;

    Profile * prof = pit.value();

    if (prof != p_profile) {
        updateProfileHighlight(profname);
    }

    return prof;
}

void ProfileSelector::on_profileView_doubleClicked(const QModelIndex &index)
{
    QModelIndex idx = proxy->index(index.row(), 0, QModelIndex());
    QString profname = proxy->data(idx, Qt::UserRole+2).toString();

    //if (SelectProfile(profname)) {
    mainwin->OpenProfile(profname);
    //}
}

void ProfileSelector::on_profileFilter_textChanged(const QString &arg1)
{
    #if QT_VERSION < QT_VERSION_CHECK(6,0,0)
        QRegExp regExp("*"+arg1+"*", Qt::CaseInsensitive, QRegExp::Wildcard);
        proxy->setFilterRegExp(regExp);
    #else
        QRegularExpression regExp = QRegularExpression::fromWildcard("*" + arg1 + "*", Qt::CaseInsensitive);
        proxy->setFilterRegularExpression(regExp);
    #endif
}

// Clear filter list
void ProfileSelector::on_resetFilterButton_clicked()
{
    ui->profileFilter->clear();
}

QString ProfileSelector::selectedProfileName() const
{
    const QModelIndex idx = ui->profileView->currentIndex();
    if (!idx.isValid()) return QString();
    return proxy->data(proxy->index(idx.row(), 0, QModelIndex()), Qt::UserRole + 2).toString();
}

void ProfileSelector::on_buttonOpenProfile_clicked()
{
    if (ui->profileView->currentIndex().isValid()) {
        QString name = proxy->data(proxy->index(ui->profileView->currentIndex().row(), 0, QModelIndex()), Qt::UserRole+2).toString();
        mainwin->OpenProfile(name);
    }
}

void ProfileSelector::on_buttonEditProfile_clicked()
{
    if (ui->profileView->currentIndex().isValid()) {
        QString name = proxy->data(proxy->index(ui->profileView->currentIndex().row(), 0, QModelIndex()), Qt::UserRole+2).toString();
        qDebug() << "Editing" << name;

        Profile * prof = Profiles::profiles[name];
        //SelectProfile(name); // may not be necessary...

        NewProfile *newprof = new NewProfile(this,&name);
        newprof->edit(name);
        newprof->setWindowModality(Qt::ApplicationModal);
        newprof->setModal(true);
        if (newprof->exec() != NewProfile::Rejected) {
            QString usersname;
            if (!prof->user->lastName().isEmpty()) {
                usersname = formattedName(prof->user->lastName(), prof->user->firstName());
            }

            proxy->setData(proxy->index(ui->profileView->currentIndex().row(), 5, QModelIndex()), usersname);
            proxy->setData(proxy->index(ui->profileView->currentIndex().row(), 5, QModelIndex()), nameSortKey(prof->user->lastName(), prof->user->firstName()), MySortFilterProxyModel2::NameSortRole);
            //updateProfileList();
            if (prof == p_profile) updateProfileHighlight(name);
        }

        delete newprof;
    } else {
        QMessageBox::information(this, STR_MessageBox_Information, tr("Select a profile first"), QMessageBox::Ok);
    }
}

void ProfileSelector::on_buttonNewProfile_clicked()
{
    if (p_profile)
        mainwin->CloseProfile();

    NewProfile *newprof = new NewProfile(this);
    newprof->skipWelcomeScreen();
    newprof->setWindowModality(Qt::ApplicationModal);
    newprof->setModal(true);
    if (newprof->exec() == NewProfile::Accepted) {
        p_profile = Profiles::Get(AppSetting->profileName());
        if (p_profile != nullptr) {
            QString name = p_profile->user->userName();
            p_profile = nullptr;
            mainwin->OpenProfile(name);
            // Mark this profile as locally created.
            if (p_profile) {
                p_profile->user->setSource(QStringLiteral("Local"));
                p_profile->Save();
            }
        } else {
            qWarning() << AppSetting->profileName() << "yielded a null profile";
            p_profile=nullptr;
        }
        updateProfileList();
    }
    delete newprof;
}

void ProfileSelector::on_buttonDestroyProfile_clicked()
{
    if (ui->profileView->currentIndex().isValid()) {
        QString name = proxy->data(proxy->index(ui->profileView->currentIndex().row(), 0, QModelIndex()), Qt::UserRole+2).toString();
        // profiles.value() avoids inserting a null entry when the profile has a missing directory
        Profile * profile = Profiles::profiles.value(name, nullptr);

        // For profiles whose directory is missing (e.g. a cancelled import that left a DB
        // row behind), profile is null. Resolve the path from the database instead.
        QString path;
        if (profile) {
            path = profile->Get(PrefMacro(STR_GEN_DataFolder));
        } else {
            ProfileRepository pathRepo;
            ProfileData pd = pathRepo.findByUsername(name);
            if (pd.id > 0) {
                path = ProfileRepository::resolvePath(pd.dataFolder, GetAppData() + "/Profiles");
            }
        }

        // Refuse to act when the resolved path is the Profiles directory itself rather
        // than a profile inside it, which happens when a row carries no username (e.g. a
        // cancelled import). Compare cleaned paths so a trailing slash on either side
        // cannot slip past the guard and offer the whole directory for deletion.
        if (QDir::cleanPath(path) == QDir::cleanPath(GetAppData() + "/Profiles")) {
            QMessageBox::warning(this, STR_MessageBox_Error, tr("The selected profile does not appear to contain any data and cannot be removed by OSCAR"), QMessageBox::Ok);
            return;
        }

        QDialog confirmdlg;
        QVBoxLayout layout(&confirmdlg);

        // 493 - 497 modified to allow for translation - CN
        QString warning = STR_MessageBox_Warning;
        QString line1 = tr("You are about to destroy profile '<b>%1</b>'.").arg(name);
        QString line2 = tr("Think carefully, as this will irretrievably delete the profile along with all <b>backup data</b> stored under<br/>%1.").arg(path);
        QString line3 = tr("Enter the word <b>DELETE</b> below (exactly as shown) to confirm.");
        QLabel message(QString("<b>%1:</b> %2<br/><br/>%3<br/><br/>%4").arg(warning, line1, line2, line3), &confirmdlg);

        layout.insertWidget(0,&message,1);
        QLineEdit lineedit(&confirmdlg);
        layout.insertWidget(1, &lineedit, 1);
        QHBoxLayout layout2;
        layout.insertLayout(2,&layout2,1);
        QPushButton cancel(QString(tr("Cancel")), &confirmdlg);
        QPushButton accept(QString(tr("Delete Profile")), &confirmdlg);
        layout2.addWidget(&cancel);
        layout2.addStretch(1);
        layout2.addWidget(&accept);
        confirmdlg.connect(&cancel, SIGNAL(clicked()), &confirmdlg, SLOT(reject()));
        confirmdlg.connect(&accept, SIGNAL(clicked()), &confirmdlg, SLOT(accept()));
        confirmdlg.connect(&lineedit, SIGNAL(returnPressed()), &confirmdlg, SLOT(accept()));

        if (confirmdlg.exec() != QDialog::Accepted)
            return;

        if (lineedit.text().compare(tr("DELETE"))!=0) {
            QMessageBox::information(NULL, tr("Sorry"), tr("You need to enter DELETE in capital letters."), QMessageBox::Ok);
            return;
        }
        
        qDebug() << "Deleting Profile" << name;
        if (profile == p_profile) {
            // Shut down if active
            mainwin->CloseProfile();
        }
        Profiles::profiles.remove(name);

        // Create progress dialog for database deletion (Phase 1.5)
        // Using QProgressDialog directly for percentage-based progress
        QProgressDialog *dbProgress = new QProgressDialog(
            tr("Preparing database for deletion..."), 
            QString(), 0, 100, this,
            Qt::WindowSystemMenuHint | Qt::WindowTitleHint);
        dbProgress->setWindowModality(Qt::WindowModal);
        dbProgress->setMinimumWidth(400);
        dbProgress->setValue(0);
        dbProgress->setWindowTitle(tr("Deleting profile ") + name);
        dbProgress->show();
        QCoreApplication::processEvents();
        
        // Delete from database with progress reporting (Phase 1.5)
        ProfileRepository profileRepo;
        ProfileData profileData = profileRepo.findByUsername(name);
        bool dbDeleteSuccess = false;
        
        if (profileData.id > 0) {
            // Use Phase 1.5 optimized deletion with progress callbacks
            dbDeleteSuccess = profileRepo.removeWithProgress(profileData.id, 
                [dbProgress](int percent, const QString& message) {
                    // Update progress dialog with database deletion progress
                    dbProgress->setValue(percent);
                    dbProgress->setLabelText(message);
                    QCoreApplication::processEvents();  // Keep UI responsive
                }
            );
            
            if (dbDeleteSuccess) {
                qDebug() << "Deleted profile from database:" << name << "id:" << profileData.id;
            } else {
                qWarning() << "Failed to delete profile from database:" << name;
                QMessageBox::warning(this, STR_MessageBox_Error,
                                   tr("Failed to delete profile from database. The operation has been rolled back."),
                                   QMessageBox::Ok);
                updateProfileList();
                dbProgress->close();
                delete dbProgress;
                return;
            }
//        } else {
//            dbProgress->close();
//            delete dbProgress;
        }

        // Now delete files from disk
        if (!path.isEmpty()) {
            qDebug() << "Counting items in" << path;

            int itemsProcessed = 0;
            dbProgress->setLabelText(tr("Deleting backup files..."));
            QCoreApplication::processEvents();  // Keep UI responsive
//            if (progressCallback) {
//                progressCallback(80, QObject::tr("Deleting backup files..."));
//            }
            bool deleteSuccess = removeDirWithProgress(path, dbProgress, 75, 100);
            
//            fileProgress->close();
//            delete fileProgress;
            
            if (!deleteSuccess) {
                QMessageBox::information(this, STR_MessageBox_Error,
                                         tr("There was an error deleting the profile directory, you need to manually remove it.")+QString("\n\n%1").arg(path),
                                         QMessageBox::Ok);
            } else {
                qDebug() << "Deleted" << itemsProcessed << "items from" << path;
                dbProgress->setValue(100);
                QCoreApplication::processEvents();
                QMessageBox::information(this, STR_MessageBox_Information, tr("Profile '%1' was successfully deleted").arg(name),QMessageBox::Ok);
            }
        } else {
            // No files to delete
            QMessageBox::information(this, STR_MessageBox_Information, tr("Profile '%1' was successfully deleted").arg(name),QMessageBox::Ok);
        }

        dbProgress->close();
        delete dbProgress;

        updateProfileList();

    }
}

QString ProfileSelector::formattedName(const QString &lastName, const QString &firstName)
{
    // Translators: name display order. English default is "Last, First".
    // Override to "%2 %1" for locales that prefer "First Last" (e.g. French).
    return tr("%1, %2").arg(lastName, firstName);
}

QString ProfileSelector::nameSortKey(const QString &lastName, const QString &firstName)
{
    if (firstName.isEmpty())
        return lastName;
    return lastName + ", " + firstName;
}

QString ProfileSelector::formatSize(qint64 size)
{
    QStringList units = { tr("Bytes"), tr("KB"), tr("MB"), tr("GB"), tr("TB"), tr("PB") };
    int i;
    double outputSize = size;
    for (i=0; i<units.size()-1; i++) {
        if (outputSize < 1024) break;
        outputSize = outputSize/1024;
    }
    return QString("%0 %1").arg(outputSize, 0, 'f', 2).arg(units[i]);
}

qint64 getDatabaseTotalSize() {
    QString dbPath = GetAppData() + "/oscar.db";
    qint64 totalSize = 0;

    // Main database file
    QFileInfo dbFile(dbPath);
    if (dbFile.exists()) {
        totalSize += dbFile.size();
    }

    // WAL file
    QFileInfo walFile(dbPath + "-wal");
    if (walFile.exists()) {
        totalSize += walFile.size();
    }

    // Shared memory file
    QFileInfo shmFile(dbPath + "-shm");
    if (shmFile.exists()) {
        totalSize += shmFile.size();
    }

    return totalSize;
}
QString ProfileSelector::getProfileDiskInfo(Profile *profile)
{
    QString html;
    if (profile) {
//        qint64 sizeSummaries = profile->diskSpaceSummaries(); // Remove for OSCAR 2.0
//        qint64 sizeEvents = profile->diskSpaceEvents();
        qint64 sizeDatabase = getDatabaseTotalSize();
        qint64 sizeBackups = profile->diskSpaceBackups();

        html += "<table>"
//                "<tr><td align=right>"+tr("Summaries:")+"</td><td>"+formatSize(sizeSummaries)+"</td></tr>"
//                "<tr><td align=right>"+tr("Events:")+"</td><td>"+formatSize(sizeEvents)+"</td></tr>"
                "<tr><td align=right>"+tr("Backups:")+"</td><td>"+formatSize(sizeBackups)+"</td></tr>"
                "<tr><td align=right>"+tr("Database (all profiles):")+"</td><td>"+formatSize(sizeDatabase)+"</td></tr>"
                "</table>";
    }
    return html;

}

void ProfileSelector::on_diskSpaceInfo_linkActivated(const QString &link)
{
    QString html;

    if (link == "show") {
        QString name = selectedProfileName();
        auto it = Profiles::profiles.find(name);
        Profile *selected = (it != Profiles::profiles.end()) ? it.value() : nullptr;
        html += "<a href='hide'>"+tr("Hide disk usage information")+"</a>"+getProfileDiskInfo(selected);
        showDiskUsage = true;
    } else {
        html += "<a href='show'>"+tr("Show disk usage information")+"</a>";
        showDiskUsage = false;
    }
    ui->diskSpaceInfo->setText(html);
}

void ProfileSelector::on_selectionChanged(const QModelIndex &index, const QModelIndex &)
{
    bool enabled = false;
    if (index.isValid()) {
        QString name = proxy->data(proxy->index(index.row(), 0, QModelIndex()), Qt::UserRole+2).toString();
        auto prof = Profiles::profiles.find(name);
        if (prof != Profiles::profiles.end()) {
            enabled = true;
            QString html = QString();
            Profile * profile = prof.value();

            if (!profile->user->lastName().isEmpty() && !profile->user->firstName().isEmpty()) {
                html += tr("Name: ") + formattedName(profile->user->lastName(), profile->user->firstName()) + "<br/>";
            }
            if (!profile->user->phone().isEmpty()) {
                html += tr("Phone: %1").arg(profile->user->phone())+"<br/>";
            }
            if (!profile->user->email().isEmpty()) {
                html += tr("Email: <a href='mailto:%1'>%1</a>").arg(profile->user->email())+"<br/>";
            }
            if (!profile->user->address().isEmpty()) {
                html += "<br/>"+tr("Address:")+"<br/>"+profile->user->address().trimmed().replace("\n","<br/>")+"<br/>";
            }
            if (html.isEmpty()) {
                html += tr("No profile information given")+"<br/>";
            }
            ui->diskSpaceInfo->setVisible(true);
            ui->profileInfoGroupBox->setTitle(tr("Profile: %1").arg(name));
            ui->profileInfoLabel->setText(html);

            if (showDiskUsage) { // ugly, but uh....
                ui->diskSpaceInfo->setText("<a href='hide'>"+tr("Hide disk usage information")+"</a>"+getProfileDiskInfo(prof.value()));
            }
        } else {
           ui->diskSpaceInfo->setText(tr("Something went wrong"));
        }
    }
    ui->buttonOpenProfile->setEnabled(enabled);
    ui->buttonEditProfile->setEnabled(enabled);
}
