/* Preferences Repository Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file implements the PreferencesRepository class for database
 * access to profile preferences (key-value storage).
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "preferences_repository.h"
#include "database_manager.h"
#include "../SleepLib/profiles.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>
#include <QDate>
#include <QTime>

PreferencesRepository::PreferencesRepository()
{
}

PreferencesRepository::~PreferencesRepository()
{
}

qint64 PreferencesRepository::create(const PreferenceData& data)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "PreferencesRepository::create() - Database not open";
        return -1;
    }

    QSqlQuery query(db);
    query.prepare(
        "INSERT INTO profile_preferences "
        "(profile_id, category, key, value, data_type) "
        "VALUES (?, ?, ?, ?, ?)"
    );

    query.addBindValue(data.profileId);
    query.addBindValue(data.category);
    query.addBindValue(data.key);
    query.addBindValue(data.value);
    query.addBindValue(data.dataType);

    if (!query.exec()) {
        qWarning() << "PreferencesRepository::create() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("PreferencesRepository::create", query);
        return -1;
    }

    return query.lastInsertId().toLongLong();
}

bool PreferencesRepository::update(const PreferenceData& data)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "PreferencesRepository::update() - Database not open";
        return false;
    }

    QSqlQuery query(db);
    query.prepare(
        "UPDATE profile_preferences SET "
        "value = ?, data_type = ?, updated_at = CURRENT_TIMESTAMP "
        "WHERE id = ?"
    );

    query.addBindValue(data.value);
    query.addBindValue(data.dataType);
    query.addBindValue(data.id);

    if (!query.exec()) {
        qWarning() << "PreferencesRepository::update() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("PreferencesRepository::update", query);
        return false;
    }

    return true;
}

PreferenceData PreferencesRepository::find(qint64 profileId, const QString& category, const QString& key)
{
    PreferenceData data;
    
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "PreferencesRepository::find() - Database not open";
        return data;
    }

    QSqlQuery query(db);
    query.prepare(
        "SELECT id, profile_id, category, key, value, data_type "
        "FROM profile_preferences WHERE profile_id = ? AND category = ? AND key = ?"
    );
    query.addBindValue(profileId);
    query.addBindValue(category);
    query.addBindValue(key);

    if (!query.exec()) {
        qWarning() << "PreferencesRepository::find() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("PreferencesRepository::find", query);
        return data;
    }

    if (query.next()) {
        data.id = query.value(0).toLongLong();
        data.profileId = query.value(1).toLongLong();
        data.category = query.value(2).toString();
        data.key = query.value(3).toString();
        data.value = query.value(4).toString();
        data.dataType = query.value(5).toString();
    }

    return data;
}

QList<PreferenceData> PreferencesRepository::findByProfile(qint64 profileId)
{
    QList<PreferenceData> result;
    
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "PreferencesRepository::findByProfile() - Database not open";
        return result;
    }

    QSqlQuery query(db);
    query.prepare(
        "SELECT id, profile_id, category, key, value, data_type "
        "FROM profile_preferences WHERE profile_id = ?"
    );
    query.addBindValue(profileId);

    if (!query.exec()) {
        qWarning() << "PreferencesRepository::findByProfile() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("PreferencesRepository::findByProfile", query);
        return result;
    }

    while (query.next()) {
        PreferenceData data;
        data.id = query.value(0).toLongLong();
        data.profileId = query.value(1).toLongLong();
        data.category = query.value(2).toString();
        data.key = query.value(3).toString();
        data.value = query.value(4).toString();
        data.dataType = query.value(5).toString();
        result.append(data);
    }

    return result;
}

QList<PreferenceData> PreferencesRepository::findByCategory(qint64 profileId, const QString& category)
{
    QList<PreferenceData> result;
    
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "PreferencesRepository::findByCategory() - Database not open";
        return result;
    }

    QSqlQuery query(db);
    query.prepare(
        "SELECT id, profile_id, category, key, value, data_type "
        "FROM profile_preferences WHERE profile_id = ? AND category = ?"
    );
    query.addBindValue(profileId);
    query.addBindValue(category);

    if (!query.exec()) {
        qWarning() << "PreferencesRepository::findByCategory() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("PreferencesRepository::findByCategory", query);
        return result;
    }

    while (query.next()) {
        PreferenceData data;
        data.id = query.value(0).toLongLong();
        data.profileId = query.value(1).toLongLong();
        data.category = query.value(2).toString();
        data.key = query.value(3).toString();
        data.value = query.value(4).toString();
        data.dataType = query.value(5).toString();
        result.append(data);
    }

    return result;
}

bool PreferencesRepository::remove(qint64 profileId)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "PreferencesRepository::remove() - Database not open";
        return false;
    }

    QSqlQuery query(db);
    query.prepare("DELETE FROM profile_preferences WHERE profile_id = ?");
    query.addBindValue(profileId);

    if (!query.exec()) {
        qWarning() << "PreferencesRepository::remove() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("PreferencesRepository::remove", query);
        return false;
    }

    return true;
}

bool PreferencesRepository::removeCategory(qint64 profileId, const QString& category)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "PreferencesRepository::removeCategory() - Database not open";
        return false;
    }

    QSqlQuery query(db);
    query.prepare("DELETE FROM profile_preferences WHERE profile_id = ? AND category = ?");
    query.addBindValue(profileId);
    query.addBindValue(category);

    if (!query.exec()) {
        qWarning() << "PreferencesRepository::removeCategory() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("PreferencesRepository::removeCategory", query);
        return false;
    }

    return true;
}

bool PreferencesRepository::savePreference(qint64 profileId, const QString& category, 
                                          const QString& key, const QVariant& value)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "PreferencesRepository::savePreference() - Database not open";
        return false;
    }

    // IMPORTANT: Delete ALL existing entries for this (profile_id, key) 
    // regardless of category to prevent cross-category duplicates
    QSqlQuery deleteQuery(db);
    deleteQuery.prepare(
        "DELETE FROM profile_preferences WHERE profile_id = ? AND key = ?"
    );
    deleteQuery.addBindValue(profileId);
    deleteQuery.addBindValue(key);
    
    if (!deleteQuery.exec()) {
        qWarning() << "PreferencesRepository::savePreference() - Delete failed:" << deleteQuery.lastError().text();
        DatabaseManager::instance().checkQueryError("PreferencesRepository::savePreference", deleteQuery);
        return false;
    }

    // Now insert the new preference
    QSqlQuery insertQuery(db);
    insertQuery.prepare(
        "INSERT INTO profile_preferences "
        "(profile_id, category, key, value, data_type, updated_at) "
        "VALUES (?, ?, ?, ?, ?, CURRENT_TIMESTAMP)"
    );

    insertQuery.addBindValue(profileId);
    insertQuery.addBindValue(category);
    insertQuery.addBindValue(key);
    insertQuery.addBindValue(value.toString());
    insertQuery.addBindValue(dataTypeFromVariant(value));

    if (!insertQuery.exec()) {
        qWarning() << "PreferencesRepository::savePreference() - Insert failed:" << insertQuery.lastError().text();
        DatabaseManager::instance().checkQueryError("PreferencesRepository::savePreference", insertQuery);
        return false;
    }

    return true;
}

bool PreferencesRepository::saveAllPreferences(qint64 profileId,
                                               CPAPSettings* cpap,
                                               OxiSettings* oxi,
                                               SessionSettings* session,
                                               AppearanceSettings* appearance,
                                               UserSettings* general)
{
    bool success = true;
    
    if (cpap) {
        success &= saveCPAPSettings(profileId, cpap);
    }
    
    if (oxi) {
        success &= saveOxiSettings(profileId, oxi);
    }
    
    if (session) {
        success &= saveSessionSettings(profileId, session);
    }
    
    if (appearance) {
        success &= saveAppearanceSettings(profileId, appearance);
    }
    
    if (general) {
        success &= saveUserSettings(profileId, general);
    }
    
    return success;
}

bool PreferencesRepository::loadAllPreferences(qint64 profileId,
                                               CPAPSettings* cpap,
                                               OxiSettings* oxi,
                                               SessionSettings* session,
                                               AppearanceSettings* appearance,
                                               UserSettings* general)
{
    bool success = true;
    
    if (cpap) {
        success &= loadCPAPSettings(profileId, cpap);
    }
    
    if (oxi) {
        success &= loadOxiSettings(profileId, oxi);
    }
    
    if (session) {
        success &= loadSessionSettings(profileId, session);
    }
    
    if (appearance) {
        success &= loadAppearanceSettings(profileId, appearance);
    }
    
    if (general) {
        success &= loadUserSettings(profileId, general);
    }
    
    return success;
}

// Private helper methods

QString PreferencesRepository::dataTypeFromVariant(const QVariant& value)
{
    switch (value.type()) {
        case QVariant::Bool:
            return "bool";
        case QVariant::Int:
        case QVariant::LongLong:
            return "int";
        case QVariant::Double:
            return "float";
        case QVariant::Date:
            return "date";
        case QVariant::Time:
            return "time";
        case QVariant::DateTime:
            return "datetime";
        default:
            return "string";
    }
}

QVariant PreferencesRepository::variantFromString(const QString& value, const QString& dataType)
{
    if (dataType == "bool") {
        return QVariant(value.toLower() == "true" || value == "1");
    } else if (dataType == "int") {
        return QVariant(value.toLongLong());
    } else if (dataType == "float") {
        return QVariant(value.toDouble());
    } else if (dataType == "date") {
        return QVariant(QDate::fromString(value, Qt::ISODate));
    } else if (dataType == "time") {
        return QVariant(QTime::fromString(value, Qt::ISODate));
    } else if (dataType == "datetime") {
        return QVariant(QDateTime::fromString(value, Qt::ISODate));
    } else {
        return QVariant(value);
    }
}

bool PreferencesRepository::saveCPAPSettings(qint64 profileId, CPAPSettings* cpap)
{
    if (!cpap) return false;
    
    // CPAP settings have specific keys defined in profiles.h
    // Save only keys that are documented CPAP settings
    QStringList cpapKeys = {
        "ComplianceHours", "ClinicalMode", "ShowLeaksMode", "MaskStartDate",
        "MaskDescription", "MaskType", "CPAPPrescribedMode", "CPAPPrescribedMinPressure",
        "CPAPPrescribedMaxPressure", "UntreatedAHI", "CPAPNotes", "DateDiagnosed",
        "UserEventFlagging", "AutoImport", "BrickWarning", "WarnOnDifferentSDCard",
        "LastSDCardSerial", "LastSDCardLoader", "UserFlowRestriction",
        "UserEventDuration", "UserFlowRestriction2", "UserEventDuration2",
        "UserEventDuplicates", "ResyncFromUserFlagging", "AHIWindow", "AHIReset",
        "ClockDrift", "LeakRedline", "ShowLeakRedline", "CalculateUnintentionalLeaks",
        "Custom4cmH2OLeaks", "Custom20cmH2OLeaks", "EventPostcontext", "ConsolidateEvents"
    };
    
    for (const QString& key : cpapKeys) {
        if (cpap->m_pref->contains(key)) {
            QVariant value = (*cpap->m_pref)[key];
            if (!savePreference(profileId, "cpap", key, value)) {
                qWarning() << "Failed to save CPAP preference:" << key;
            }
        }
    }
    
    return true;
}

bool PreferencesRepository::saveOxiSettings(qint64 profileId, OxiSettings* oxi)
{
    if (!oxi) return false;
    
    // Oximetry settings keys defined in profiles.h (STR_OS_*)
    QStringList oxiKeys = {
        "EnableOximetry", "DefaultOxiDevice", "SyncOximeterClock", "OximeterType",
        "SkipOxiIntroScreen", "SPO2DropDuration", "SPO2DropPercentage",
        "PulseChangeDuration", "PulseChangeBPM", "OxiDiscardThreshold",
        "oxiDesaturationThreshold", "flagPulseAbove", "flagPulseBelow"
    };
    
    for (const QString& key : oxiKeys) {
        if (oxi->m_pref->contains(key)) {
            QVariant value = (*oxi->m_pref)[key];
            if (!savePreference(profileId, "oxi", key, value)) {
                qWarning() << "Failed to save Oxi preference:" << key;
            }
        }
    }
    
    return true;
}

bool PreferencesRepository::saveSessionSettings(qint64 profileId, SessionSettings* session)
{
    if (!session) return false;
    
    // Session/Import settings keys defined in profiles.h (STR_IS_*)
    QStringList sessionKeys = {
        "DaySplitTime", "PreloadSummaries", "CombineCloserSessions", 
        "IgnoreShorterSessions", "BackupCardData", "CompressBackupData",
        "CompressSessionData", "IgnoreOlderSessions", "IgnoreOlderSessionsDate",
        "LockSummarySessions", "WarnOnUntestedMachine", "WarnOnUnexpectedData",
        "SplitNonCPAPSessions"
    };
    
    for (const QString& key : sessionKeys) {
        if (session->m_pref->contains(key)) {
            QVariant value = (*session->m_pref)[key];
            if (!savePreference(profileId, "session", key, value)) {
                qWarning() << "Failed to save Session preference:" << key;
            }
        }
    }
    
    return true;
}

bool PreferencesRepository::saveAppearanceSettings(qint64 profileId, AppearanceSettings* appearance)
{
    if (!appearance) return false;
    
    // Appearance settings keys defined in profiles.h (STR_AS_*)
    QStringList appearanceKeys = {
        "EventFlagSessionBar", "ZombieMode"
    };
    
    for (const QString& key : appearanceKeys) {
        if (appearance->m_pref->contains(key)) {
            QVariant value = (*appearance->m_pref)[key];
            if (!savePreference(profileId, "appearance", key, value)) {
                qWarning() << "Failed to save Appearance preference:" << key;
            }
        }
    }
    
    return true;
}

bool PreferencesRepository::saveUserSettings(qint64 profileId, UserSettings* general)
{
    if (!general) return false;
    
    // User/General settings keys defined in profiles.h (STR_US_*)
    QStringList generalKeys = {
        "UnitSystem", "EventWindowSize", "SkipEmptyDays", "RebuildCache",
        "LinkGroups", "CalculateRDI", "PrefCalcMiddle", "PrefCalcPercentile",
        "PrefCalcMax", "ShowUnknownFlags", "StatReportMode", "StatReportDate",
        "StatReportRangeStart", "StatReportRangeEnd", "LastOverviewRange",
        "CustomOverviewRangeStart", "CustomOverviewRangeEnd"
    };
    
    for (const QString& key : generalKeys) {
        if (general->m_pref->contains(key)) {
            QVariant value = (*general->m_pref)[key];
            if (!savePreference(profileId, "general", key, value)) {
                qWarning() << "Failed to save General preference:" << key;
            }
        }
    }
    
    return true;
}

bool PreferencesRepository::loadCPAPSettings(qint64 profileId, CPAPSettings* cpap)
{
    if (!cpap) return false;
    
    QList<PreferenceData> prefs = findByCategory(profileId, "cpap");
    
    for (const PreferenceData& pref : prefs) {
        QVariant value = variantFromString(pref.value, pref.dataType);
        cpap->setPref(pref.key, value);
    }
    
//    qDebug() << "PreferencesRepository: Loaded" << prefs.size() << "CPAP preferences";
    return true;
}

bool PreferencesRepository::loadOxiSettings(qint64 profileId, OxiSettings* oxi)
{
    if (!oxi) return false;
    
    QList<PreferenceData> prefs = findByCategory(profileId, "oxi");
    
    for (const PreferenceData& pref : prefs) {
        QVariant value = variantFromString(pref.value, pref.dataType);
        oxi->setPref(pref.key, value);
    }
    
//    qDebug() << "PreferencesRepository: Loaded" << prefs.size() << "Oxi preferences";
    return true;
}

bool PreferencesRepository::loadSessionSettings(qint64 profileId, SessionSettings* session)
{
    if (!session) return false;
    
    QList<PreferenceData> prefs = findByCategory(profileId, "session");
    
    for (const PreferenceData& pref : prefs) {
        QVariant value = variantFromString(pref.value, pref.dataType);
        session->setPref(pref.key, value);
    }
    
//    qDebug() << "PreferencesRepository: Loaded" << prefs.size() << "Session preferences";
    return true;
}

bool PreferencesRepository::loadAppearanceSettings(qint64 profileId, AppearanceSettings* appearance)
{
    if (!appearance) return false;
    
    QList<PreferenceData> prefs = findByCategory(profileId, "appearance");
    
    for (const PreferenceData& pref : prefs) {
        QVariant value = variantFromString(pref.value, pref.dataType);
        appearance->setPref(pref.key, value);
    }
    
//    qDebug() << "PreferencesRepository: Loaded" << prefs.size() << "Appearance preferences";
    return true;
}

bool PreferencesRepository::loadUserSettings(qint64 profileId, UserSettings* general)
{
    if (!general) return false;
    
    QList<PreferenceData> prefs = findByCategory(profileId, "general");
    
    for (const PreferenceData& pref : prefs) {
        QVariant value = variantFromString(pref.value, pref.dataType);
        general->setPref(pref.key, value);
    }
    
//    qDebug() << "PreferencesRepository: Loaded" << prefs.size() << "General preferences";
    return true;
}
