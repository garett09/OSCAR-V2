/* SleepLib Preferences Implementation
 *
 * Copyright (c) 2019-2026 The OSCAR Team
 * Copyright (c) 2011-2018 Mark Watkins
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#define TEST_MACROS_ENABLEDoff
#include <test_macros.h>

#include <QString>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>
#include <QVariant>
#include <QDateTime>
#include <QTimeZone>
#include <QDir>
#include <QDesktopServices>
#include <QDebug>
#include <QSettings>
#include <QMessageBox>
#include <QTranslator>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include "windows.h"
#include "lmcons.h"
#endif

#include "common.h"
#include "preferences.h"
#include "../database/app_preferences_repository.h"
#include "../database/database_manager.h"

const QString &getUserName()
{
    static QString userName;
    userName = getenv("USER");

    if (userName.isEmpty()) {
        userName = QObject::tr("Windows User");

#if defined (Q_OS_WIN)
#if defined(UNICODE)

//        if (QSysInfo::WindowsVersion >= QSysInfo::WV_NT) {
            TCHAR winUserName[UNLEN + 1]; // UNLEN is defined in LMCONS.H
            DWORD winUserNameSize = sizeof(winUserName);
            GetUserNameW(winUserName, &winUserNameSize);
            userName = QString::fromStdWString(winUserName);
//        } else
#else
        {
            char winUserName[UNLEN + 1]; // UNLEN is defined in LMCONS.H
            DWORD winUserNameSize = sizeof(winUserName);
            GetUserNameA(winUserName, &winUserNameSize);
            userName = QString::fromLocal8Bit(winUserName);
        }
#endif
#endif
    }

    return userName;
}


static QString g_appDataPath;

QString GetAppData()
{
    if (!g_appDataPath.isEmpty())
        return g_appDataPath;
    QSettings settings;
    QString path = settings.value("Settings/AppData").toString();
    if (path.isEmpty())
        path = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + getModifiedAppData();
    return path;
}

void SetAppData(const QString& path, bool persist)
{
    g_appDataPath = path;
    if (!persist)
        return;
    QSettings settings;
    settings.setValue("Settings/AppData", path);
}


Preferences::Preferences()
{
    p_name = "Preferences";
    p_path = GetAppData();
}

Preferences::Preferences(QString name, QString filename)
{
    if (name.endsWith(STR_ext_XML)) {
        p_name = name.section(".", 0, 0);
    } else {
        p_name = name;
    }

    if (filename.isEmpty()) {
        p_filename = GetAppData() + "/" + p_name + STR_ext_XML;
    } else {
        if (!filename.contains("/")) {
            p_filename = GetAppData() + "/";
        } else { p_filename = ""; }

        p_filename += filename;

        if (!p_filename.endsWith(STR_ext_XML)) { p_filename += STR_ext_XML; }
    }
}

Preferences::~Preferences()
{
    //Save(); // Don't..Save calls a virtual function.
}

/*int Preferences::GetCode(QString s)
{
    int prefcode=0;
    for (QHash<int,QString>::iterator i=p_codes.begin(); i!=p_codes.end(); i++) {
        if (i.value()==s) return i.key();
        prefcode++;
    }
    p_codes[prefcode]=s;
    return prefcode;
}*/

const QString Preferences::Get(QString name)
{
    QString temp;
    QChar obr = QChar('{');
    QChar cbr = QChar('}');
    QString t, a, ref; // How I miss Regular Expressions here..

    if (p_preferences.find(name) != p_preferences.end()) {
        temp = "";
        t = p_preferences[name].toString();
        #if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            if (p_preferences[name].type() != QVariant::String)
        #else
            if (p_preferences[name].typeId() != QMetaType::QString)
        #endif
        {
            return t;
        }
    } else {
        t = name; // parse the string..
    }

    while (t.contains(obr)) {
        temp += t.section(obr, 0, 0);
        a = t.section(obr, 1);

        if (a.startsWith("{")) {
            temp += obr;
            t = a.section(obr, 1);
            continue;
        }

        ref = a.section(cbr, 0, 0);

        if (ref.toLower() == "home") {
            temp += GetAppData();
        } else if (ref.toLower() == "user") {
            temp += getUserName();
        } else if (ref.toLower() == "sep") { // redundant in QT
            temp += "/";
        } else {
            temp += Get(ref);
        }

        t = a.section(cbr, 1);
    }

    temp += t;
    temp.replace("}}", "}"); // Make things look a bit better when escaping braces.

    return temp;
}


bool Preferences::Open(QString filename)
{
    if (!filename.isEmpty()) {
        p_filename = filename;
    }

    // When the DB is open and this is the app's own Preferences singleton, load from DB.
    if (p_name == "Preferences" && DatabaseManager::instance().isOpen()
            && p_filename.startsWith(GetAppData())) {
        AppPreferencesRepository repo;
        const QList<AppPrefData> rows = repo.loadAll();
        if (!rows.isEmpty()) {
            p_preferences.clear();
            for (const AppPrefData& row : rows) {
                if (row.dataType == "blob") continue; // blobs not exposed as QVariant here
                p_preferences[row.key] = repo.variantFromString(row.value, row.dataType);
            }
            return true;
        }
        // Table empty — fall through to XML import below (one-shot seeding on upgrade)
    }

    QDomDocument doc(p_name);
    QFile file(p_filename);
//    qDebug() << "Opening " << p_filename.toLocal8Bit().data();        // Not in OSCAR 2.0

    if (!file.open(QIODevice::ReadOnly)) {
//        qWarning() << "Could not open" << p_filename.toLocal8Bit().data() << " Error: " << file.error();
        if (file.error() == 5) {
//  Normal in OSCAR 2.0
//            qDebug() << "Preferences file not found -- normal on first use of OSCAR";
        }
        else {
            qWarning() << "Preferences::Open(): Could not open preferences file for reading, error code" << file.error() << file.errorString();
        }
        return false;
    }

    if (! doc.setContent(&file)) {
        qWarning() << "Preferences::Open(): Invalid XML Content in" << p_filename.toLocal8Bit().data();
        return false;
    }
    file.close();


    QDomElement root = doc.documentElement();

    if (root.tagName() != STR_AppName) {
        if (root.tagName() == "SleepyHead" ) {
            QString msg = QObject::tr("Using ") + p_filename + QObject::tr(", found SleepyHead -\n") +
                QObject::tr( "You must run the OSCAR Migration Tool");
            QMessageBox::warning(nullptr, STR_MessageBox_Error, msg, QMessageBox::Ok);
            exit(1);
        }
        return false;
    }

    root = root.firstChildElement();

    if (root.tagName() != p_name) {
        return false;
    }

    bool ok;
    p_preferences.clear();
    QDomNode n = root.firstChild();

    while (!n.isNull()) {
        QDomElement e = n.toElement();

        if (!e.isNull()) {
            QString name = e.tagName();
            QString type = e.attribute("type").toLower();
            QString value = e.text();

            if (type == "double") {
                double d;
                d = value.toDouble(&ok);

                if (ok) {
                    p_preferences[name] = d;
                } else {
                    qDebug() << "Preferences::Open(): XML Error:" << name << "=" << value << "??";
                }
            } else if (type == "qlonglong") {
                qint64 d;
                d = value.toLongLong(&ok);

                if (ok) {
                    p_preferences[name] = d;
                } else {
                    qDebug() << "Preferences::Open(): XML Error:" << name << "=" << value << "??";
                }
            } else if (type == "int") {
                int d;
                d = value.toInt(&ok);

                if (ok) {
                    p_preferences[name] = d;
                } else {
                    qDebug() << "Preferences::Open(): XML Error:" << name << "=" << value << "??";
                }
            } else if (type == "bool") {
                QString v = value.toLower();

                if ((v == "true") || (v == "on") || (v == "yes")) {
                    p_preferences[name] = true;
                } else if ((v == "false") || (v == "off") || (v == "no")) {
                    p_preferences[name] = false;
                } else {
                    int d;
                    d = value.toInt(&ok);

                    if (ok) {
                        p_preferences[name] = d != 0;
                    } else {
                        qDebug() << "Preferences::Open(): XML Error:" << name << "=" << value << "??";
                    }
                }
            } else if (type == "qdatetime") {
                QDateTime d;
                d = QDateTime::fromString(value, "yyyy-MM-dd HH:mm:ss");

                if (d.isValid()) {
                    p_preferences[name] = d;
                } else {
                    qWarning() << "Preferences::Open(): XML Error: Invalid DateTime record" << name << value;
                }

            } else if (type == "qtime") {
                QTime d;
                d = QTime::fromString(value, "hh:mm:ss");

                if (d.isValid()) {
                    p_preferences[name] = d;
                } else {
                    qWarning() << "Preferences::Open(): XML Error: Invalid Time record" << name << value;
                }

            } else  {
                p_preferences[name] = value;
            }

        }

        n = n.nextSibling();
    }

    root = root.nextSiblingElement();

    //////////////////////////////////////////////////////////////////////////////////////
    // This is a dirty hack to clean up a legacy issue
    // The old Profile system used to have devices in Profile.xml
    // We need to clean up this mistake up here, because C++ polymorphism won't otherwise
    // let us open properly in constructor
    //////////////////////////////////////////////////////////////////////////////////////
    if ((p_name == "Profile") && (root.tagName().toLower() == "machines")) {

        // Save this sucker
        QDomDocument doc("Machines");

        doc.appendChild(root);

        QFile file(p_path+"/machines.xml");

        // Don't do anything if machines.xml already exists.. the user ran the old version!
        if (!file.exists()) {
            if (!file.open(QFile::WriteOnly)) {
                qWarning() << "Preferences::Open(): Could not open" << filename << "for writing, error code" << file.error() << file.errorString();
            } else {
                file.write(doc.toByteArray());
                file.close();
            }
        }
    }

    // One-shot seeding: if the DB is open and this is the app's own Preferences.xml
    // (not a foreign file opened by the importer), seed the DB and delete the file.
    // Wrapped in a transaction so a mid-loop crash rolls back to an empty table,
    // causing the next launch to fall through to XML seeding again.
    // If an outer transaction is already active, join it rather than starting a nested
    // one to avoid committing the outer transaction via a direct db.commit() call.
    if (p_name == "Preferences" && DatabaseManager::instance().isOpen()
            && p_filename.startsWith(GetAppData())) {
        AppPreferencesRepository repo;
        DatabaseManager& dbMgr = DatabaseManager::instance();
        bool ownTransaction = !dbMgr.inTransaction();
        QSqlDatabase db = dbMgr.database();
        if (ownTransaction) db.transaction();
        bool seeded = true;
        for (auto i = p_preferences.begin(); i != p_preferences.end(); ++i) {
            if (i.value().typeId() == QMetaType::UnknownType) continue;
            if (!repo.save("general", i.key(), i.value())) seeded = false;
        }
        if (ownTransaction) {
            if (seeded) {
                db.commit();
                QFile::remove(p_filename);
            } else {
                db.rollback();
                qWarning() << "Preferences::Open(): Failed to seed app_preferences table from XML; keeping XML file";
            }
        }
    }

    return true;
}

bool Preferences::Save(QString filename)
{
    if (!filename.isEmpty()) {
        p_filename = filename;
    }

    // App preferences are stored exclusively in the database — never in XML.
    // Delete-then-reinsert inside a transaction so that keys removed via Erase() are
    // not resurrected on the next Open(), and so a partial write is never committed.
    // If an outer transaction is already active (e.g. during profile import), join it
    // instead of starting a nested one — direct db.transaction()/commit() bypasses
    // DatabaseManager::m_inTransaction tracking and would commit the outer transaction.
    if (p_name == "Preferences") {
        if (!DatabaseManager::instance().isOpen()) {
            qWarning() << "Preferences::Save(): database not open, skipping save";
            return false;
        }
        if (!p_filename.startsWith(GetAppData())) {
            qWarning() << "Preferences::Save(): p_filename" << p_filename
                       << "does not match AppData" << GetAppData() << "- skipping save";
            return false;
        }
        AppPreferencesRepository repo;
        DatabaseManager& dbMgr = DatabaseManager::instance();
        bool ownTransaction = !dbMgr.inTransaction();
        QSqlDatabase db = dbMgr.database();
        if (ownTransaction && !db.transaction()) {
            qWarning() << "Preferences::Save(): Failed to start transaction";
            return false;
        }
        bool ok = repo.removeCategory("general");
        if (ok) {
            for (auto i = p_preferences.begin(); i != p_preferences.end(); ++i) {
                if (i.value().typeId() == QMetaType::UnknownType) continue;
                if (!repo.save("general", i.key(), i.value())) { ok = false; break; }
            }
        }
        if (ownTransaction) {
            if (ok) {
                db.commit();
            } else {
                db.rollback();
                qWarning() << "Preferences::Save(): DB save failed, rolled back";
            }
        }
        return ok;
    }

    QDomDocument doc(p_name);

    QDomProcessingInstruction pi = doc.createProcessingInstruction("xml", "version=\"1.0\" encoding=\"UTF-8\"");
    doc.appendChild(pi);

    QDomElement droot = doc.createElement(STR_AppName);
    doc.appendChild(droot);

    QDomElement root = doc.createElement(p_name);
    droot.appendChild(root);
    for (QHash<QString, QVariant>::iterator i = p_preferences.begin(); i != p_preferences.end(); i++) {
        int type = i.value().typeId();
        if (type == QMetaType::UnknownType) { continue; }

        QDomElement cn = doc.createElement(i.key());
        cn.setAttribute("type", i.value().typeName());

        if (type == QMetaType::QDateTime) {
            cn.appendChild(doc.createTextNode(i.value().toDateTime().toString("yyyy-MM-dd HH:mm:ss")));
        } else if (type == QMetaType::QTime) {
            cn.appendChild(doc.createTextNode(i.value().toTime().toString("hh:mm:ss")));
        } else {
            cn.appendChild(doc.createTextNode(i.value().toString()));
        }

        root.appendChild(cn);
    }

    QFile file(p_filename);

    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Preferences::Save(): Could not open" << p_filename << "for writing, error code" << file.error() << file.errorString();
        return false;
    }

    QTextStream ts(&file);
    ts.setGenerateByteOrderMark(true);
    ts << doc.toString();
    file.close();

    return true;
}


AppWideSetting *AppSetting = nullptr;

