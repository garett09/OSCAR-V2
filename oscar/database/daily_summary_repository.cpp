/* Daily Summary Repository Implementation
 *
 * Copyright (c) 2025-2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "daily_summary_repository.h"
#include "database_manager.h"
#include "profile_repository.h"
#include "../SleepLib/profiles.h"
#include "../SleepLib/day.h"
#include "../SleepLib/session.h"
#include "../SleepLib/machine.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>
#include <QCryptographicHash>

DailySummaryRepository::DailySummaryRepository()
    : db(DatabaseManager::instance().database())
{
}

DailySummaryRepository::DailySummaryRepository(const QSqlDatabase& database)
    : db(database)
{
}

qint64 DailySummaryRepository::create(const DailySummaryData& data)
{
    QSqlQuery query(db);
    
    // Use INSERT OR REPLACE for idempotent operation
    query.prepare(R"(
        INSERT OR REPLACE INTO daily_summaries (
            profile_id, date,
            session_count, enabled_session_count,
            total_hours, mask_on_hours,
            ahi, rdi, oahi, cahi,
            obstructive_count, unclassified_count, hypopnea_count, rera_count, clear_airway_count,
            obstructive_hypopnea_count, central_hypopnea_count, all_apnea_count,
            pressure_avg, pressure_min, pressure_max, pressure_95th,
            leak_total_avg, leak_total_95th, leak_total_max, leak_unintentional_avg,
            spo2_avg, spo2_min, pulse_avg, pulse_min, pulse_max,
            is_compliant, has_oximetry, sessions_hash
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )");

    query.addBindValue(data.profileId);
    query.addBindValue(data.date);
    query.addBindValue(data.sessionCount);
    query.addBindValue(data.enabledSessionCount);
    query.addBindValue(data.totalHours);
    query.addBindValue(data.maskOnHours);
    query.addBindValue(data.ahi);
    query.addBindValue(data.rdi);
    query.addBindValue(data.oahi);
    query.addBindValue(data.cahi);
    query.addBindValue(data.obstructiveCount);
    query.addBindValue(data.unclassifiedCount);
    query.addBindValue(data.hypopneaCount);
    query.addBindValue(data.reraCount);
    query.addBindValue(data.clearAirwayCount);
    query.addBindValue(data.obstructiveHypopneaCount);
    query.addBindValue(data.centralHypopneaCount);
    query.addBindValue(data.allApneaCount);
    query.addBindValue(data.pressureAvg);
    query.addBindValue(data.pressureMin);
    query.addBindValue(data.pressureMax);
    query.addBindValue(data.pressure95th);
    query.addBindValue(data.leakTotalAvg);
    query.addBindValue(data.leakTotal95th);
    query.addBindValue(data.leakTotalMax);
    query.addBindValue(data.leakUnintentionalAvg);
    query.addBindValue(data.spo2Avg);
    query.addBindValue(data.spo2Min);
    query.addBindValue(data.pulseAvg);
    query.addBindValue(data.pulseMin);
    query.addBindValue(data.pulseMax);
    query.addBindValue(data.isCompliant);
    query.addBindValue(data.hasOximetry);
    query.addBindValue(data.sessionsHash);
    
    if (!query.exec()) {
        qWarning() << "DailySummaryRepository::create failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DailySummaryRepository::create", query);
        return -1;
    }
    
    return query.lastInsertId().toLongLong();
}

bool DailySummaryRepository::update(const DailySummaryData& data)
{
    // For daily_summaries, we use INSERT OR REPLACE in create()
    // This method is here for compatibility but just calls create
    return create(data) > 0;
}

bool DailySummaryRepository::remove(qint64 id)
{
    QSqlQuery query(db);
    query.prepare("DELETE FROM daily_summaries WHERE id = ?");
    query.addBindValue(id);
    
    if (!query.exec()) {
        qWarning() << "DailySummaryRepository::remove failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DailySummaryRepository::remove", query);
        return false;
    }
    
    return query.numRowsAffected() > 0;
}

DailySummaryData DailySummaryRepository::findById(qint64 id)
{
    QSqlQuery query(db);
    query.prepare("SELECT * FROM daily_summaries WHERE id = ?");
    query.addBindValue(id);
    
    if (!query.exec()) {
        qWarning() << "DailySummaryRepository::findById failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DailySummaryRepository::findById", query);
        return DailySummaryData();
    }
    
    if (query.next()) {
        return mapResultToData(query);
    }
    
    return DailySummaryData();
}

DailySummaryData DailySummaryRepository::findByProfileAndDate(qint64 profileId, const QDate& date)
{
    QSqlQuery query(db);

    query.prepare("SELECT * FROM daily_summaries WHERE profile_id = ? AND date = ?");
    query.addBindValue(profileId);
    query.addBindValue(date.toString(Qt::ISODate));

    if (!query.exec()) {
        qWarning() << "DailySummaryRepository::findByProfileAndDate failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DailySummaryRepository::findByProfileAndDate", query);
        return DailySummaryData();
    }

    if (query.next()) {
        return mapResultToData(query);
    }

    return DailySummaryData();
}

QList<DailySummaryData> DailySummaryRepository::findByProfile(qint64 profileId)
{
    QList<DailySummaryData> summaries;
    
    QSqlQuery query(db);
    query.prepare("SELECT * FROM daily_summaries WHERE profile_id = ? ORDER BY date DESC");
    query.addBindValue(profileId);
    
    if (!query.exec()) {
        qWarning() << "DailySummaryRepository::findByProfile failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DailySummaryRepository::findByProfile", query);
        return summaries;
    }
    
    while (query.next()) {
        summaries.append(mapResultToData(query));
    }
    
    return summaries;
}

QList<DailySummaryData> DailySummaryRepository::findRange(qint64 profileId, const QDate& startDate, const QDate& endDate)
{
    QList<DailySummaryData> summaries;

    QSqlQuery query(db);

    query.prepare("SELECT * FROM daily_summaries WHERE profile_id = ? AND date BETWEEN ? AND ? ORDER BY date");
    query.addBindValue(profileId);
    query.addBindValue(startDate.toString(Qt::ISODate));
    query.addBindValue(endDate.toString(Qt::ISODate));

    if (!query.exec()) {
        qWarning() << "DailySummaryRepository::findRange failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DailySummaryRepository::findRange", query);
        return summaries;
    }

    while (query.next()) {
        summaries.append(mapResultToData(query));
    }

    return summaries;
}

bool DailySummaryRepository::calculateAndStore(qint64 profileId, const QDate& date)
{
    // Get the Day object from profile
    Profile* profile = p_profile;  // Global profile pointer
    if (!profile) {
        qWarning() << "DailySummaryRepository::calculateAndStore - No active profile";
        return false;
    }

    Day* day = profile->GetDay(date, MT_UNKNOWN);
    if (!day) {
        qDebug() << "DailySummaryRepository::calculateAndStore - No data for date" << date.toString();
        return false;
    }

    return calculateAndStoreFromDay(day, profileId);
}

bool DailySummaryRepository::calculateAndStoreFromDay(Day* day, qint64 profileId)
{
    if (!day) {
        qWarning() << "DailySummaryRepository::calculateAndStoreFromDay - Day is null";
        return false;
    }

    // Calculate statistics from day
    DailySummaryData data = calculateFromDay(day);
    data.profileId = profileId;
    data.date = day->date().toString(Qt::ISODate);

    // Check if we got any meaningful data (CPAP or oximetry)
    if (data.totalHours <= 0) {
        qDebug() << "DailySummaryRepository::calculateAndStoreFromDay: Skipping" << data.date << "- no session hours";
        return false;
    }

    // Store in database
    qint64 id = create(data);

    if (id > 0) {
        return true;
    } else {
        qWarning() << "DailySummaryRepository::calculateAndStoreFromDay: Failed to store daily summary for" << data.date;
    }

    return false;
}

DailySummaryData DailySummaryRepository::calculateFromDay(Day* day)
{
    DailySummaryData data;

    if (!day) {
        return data;
    }

    // Ensure summaries are loaded
    day->OpenSummary();

    // Count sessions and get hours using Day's built-in method
    data.sessionCount = day->sessions.size();
    data.enabledSessionCount = 0;

    for (Session* sess : day->sessions) {
        if (!sess) continue;
        if (sess->enabled()) {
            data.enabledSessionCount++;
        }
    }

    // Use Day's hours() method for accurate time calculation
    // Include hours from all therapy machines, not just CPAP
    data.totalHours = day->hours(MT_CPAP);
    data.maskOnHours = data.totalHours;  // For CPAP, mask-on time is total time
    
    // Also check for oximetry hours if CPAP hours are 0
    if (data.totalHours <= 0 && day->hasMachine(MT_OXIMETER)) {
        data.totalHours = day->hours(MT_OXIMETER);
    }
    
    // Use Day's built-in AHI/RDI calculation methods
    // These may return NaN if hours are 0, so check for validity
    if (day->hasMachine(MT_CPAP) && data.totalHours > 0) {
        EventDataType ahi = day->calcAHI();
        EventDataType rdi = day->calcRDI();
        EventDataType oahi = day->calcOAHI();
        EventDataType cahi = day->calcCAHI();
        // Only set if valid numbers (not NaN or inf)
        if (!qIsNaN(ahi) && !qIsInf(ahi)) {
            data.ahi = ahi;
        }
        if (!qIsNaN(rdi) && !qIsInf(rdi)) {
            data.rdi = rdi;
        }
        if (!qIsNaN(oahi) && !qIsInf(oahi)) {
            data.oahi = oahi;
        }
        if (!qIsNaN(cahi) && !qIsInf(cahi)) {
            data.cahi = cahi;
        }
    }

    // Event counts using Day's count() method
    data.obstructiveCount = static_cast<int>(day->count(CPAP_Obstructive));
    data.unclassifiedCount = static_cast<int>(day->count(CPAP_Apnea));
    data.hypopneaCount = static_cast<int>(day->count(CPAP_Hypopnea));
    data.reraCount = static_cast<int>(day->count(CPAP_RERA));
    data.clearAirwayCount = static_cast<int>(day->count(CPAP_ClearAirway));
    data.obstructiveHypopneaCount = static_cast<int>(day->count(CPAP_ObstructiveHypopnea));
    data.centralHypopneaCount = static_cast<int>(day->count(CPAP_CentralHypopnea));
    // CPAP_AllApnea contributes to AHI but was never stored before schema v18, which
    // is why SQL sums over these columns used to disagree with the stored ahi.
    data.allApneaCount = static_cast<int>(day->count(CPAP_AllApnea));
    
    // Pressure statistics using Day's aggregation methods
    if (day->channelHasData(CPAP_Pressure)) {
        data.pressureAvg = day->wavg(CPAP_Pressure);
        data.pressureMin = day->Min(CPAP_Pressure);
        data.pressureMax = day->Max(CPAP_Pressure);
        data.pressure95th = day->p90(CPAP_Pressure);
    }
    
    // Leak statistics
    if (day->channelHasData(CPAP_Leak)) {
        data.leakTotalAvg = day->wavg(CPAP_Leak);
        data.leakTotal95th = day->p90(CPAP_Leak);
        data.leakTotalMax = day->Max(CPAP_Leak);
    }
    
    if (day->channelHasData(CPAP_LeakFlag)) {
        data.leakUnintentionalAvg = day->wavg(CPAP_LeakFlag);
    }
    
    // Oximetry statistics
    if (day->channelHasData(OXI_SPO2)) {
        data.hasOximetry = true;
        data.spo2Avg = day->wavg(OXI_SPO2);
        data.spo2Min = day->Min(OXI_SPO2);
    }
    
    if (day->channelHasData(OXI_Pulse)) {
        data.hasOximetry = true;
        data.pulseAvg = day->wavg(OXI_Pulse);
        data.pulseMin = day->Min(OXI_Pulse);
        data.pulseMax = day->Max(OXI_Pulse);
    }
    
    // Compliance flag (4 hours minimum by default)
    data.isCompliant = (data.totalHours >= 4.0);

    // Generate hash for cache invalidation
    data.sessionsHash = generateSessionsHash(0, day->date());

    return data;
}

bool DailySummaryRepository::calculateRange(qint64 profileId, const QDate& startDate, const QDate& endDate)
{
    Profile* profile = p_profile;
    if (!profile) {
        qWarning() << "DailySummaryRepository::calculateRange - No active profile";
        return false;
    }
    
    int successCount = 0;
    int totalDays = startDate.daysTo(endDate) + 1;
    
    qDebug() << "DailySummaryRepository: Calculating summaries for" << totalDays << "days";
    
    QDate date = startDate;
    while (date <= endDate) {
        Day* day = profile->GetDay(date, MT_UNKNOWN);
        if (day && day->hasEnabledSessions()) {
            if (calculateAndStoreFromDay(day, profileId)) {
                successCount++;
            }
        }
        date = date.addDays(1);
    }
    
    qDebug() << "DailySummaryRepository: Calculated" << successCount << "of" << totalDays << "days";
    
    return successCount > 0;
}

bool DailySummaryRepository::invalidateDate(qint64 profileId, const QDate& date)
{
    QSqlQuery query(db);
    query.prepare("DELETE FROM daily_summaries WHERE profile_id = ? AND date = ?");
    query.addBindValue(profileId);
    query.addBindValue(date.toString(Qt::ISODate));
    
    if (!query.exec()) {
        qWarning() << "DailySummaryRepository::invalidateDate failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DailySummaryRepository::invalidateDate", query);
        return false;
    }
    
    return true;
}

bool DailySummaryRepository::invalidateRange(qint64 profileId, const QDate& startDate, const QDate& endDate)
{
    QSqlQuery query(db);
    query.prepare("DELETE FROM daily_summaries WHERE profile_id = ? AND date BETWEEN ? AND ?");
    query.addBindValue(profileId);
    query.addBindValue(startDate.toString(Qt::ISODate));
    query.addBindValue(endDate.toString(Qt::ISODate));
    
    if (!query.exec()) {
        qWarning() << "DailySummaryRepository::invalidateRange failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DailySummaryRepository::invalidateRange", query);
        return false;
    }
    
    return true;
}

bool DailySummaryRepository::exists(qint64 profileId, const QDate& date)
{
    QSqlQuery query(db);

    query.prepare("SELECT COUNT(*) FROM daily_summaries WHERE profile_id = ? AND date = ?");
    query.addBindValue(profileId);
    query.addBindValue(date.toString(Qt::ISODate));

    if (!query.exec()) {
        qWarning() << "DailySummaryRepository::exists failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DailySummaryRepository::exists", query);
        return false;
    }

    if (query.next()) {
        return query.value(0).toInt() > 0;
    }

    return false;
}

int DailySummaryRepository::countDays(qint64 profileId, const QDate& startDate, const QDate& endDate)
{
    QSqlQuery query(db);
    query.prepare("SELECT COUNT(*) FROM daily_summaries WHERE profile_id = ? AND date BETWEEN ? AND ?");
    query.addBindValue(profileId);
    query.addBindValue(startDate.toString(Qt::ISODate));
    query.addBindValue(endDate.toString(Qt::ISODate));
    
    if (!query.exec()) {
        qWarning() << "DailySummaryRepository::countDays failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("DailySummaryRepository::countDays", query);
        return 0;
    }
    
    if (query.next()) {
        return query.value(0).toInt();
    }
    
    return 0;
}

QString DailySummaryRepository::generateSessionsHash(qint64 profileId, const QDate& date)
{
    Q_UNUSED(profileId)
    // Generate a hash based on date for basic cache invalidation
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(date.toString(Qt::ISODate).toUtf8());
    hash.addData(QDateTime::currentDateTime().toString().toUtf8());
    return QString(hash.result().toHex());
}

DailySummaryData DailySummaryRepository::mapResultToData(const QSqlQuery& query)
{
    DailySummaryData data;
    
    data.id = query.value("id").toLongLong();
    data.profileId = query.value("profile_id").toLongLong();
    data.date = query.value("date").toString();

    data.sessionCount = query.value("session_count").toInt();
    data.enabledSessionCount = query.value("enabled_session_count").toInt();
    
    data.totalHours = query.value("total_hours").toDouble();
    data.maskOnHours = query.value("mask_on_hours").toDouble();
    
    data.ahi = query.value("ahi").toDouble();
    data.rdi = query.value("rdi").toDouble();
    data.oahi = query.value("oahi").toDouble();
    data.cahi = query.value("cahi").toDouble();
    data.obstructiveCount = query.value("obstructive_count").toInt();
    data.unclassifiedCount = query.value("unclassified_count").toInt();
    data.hypopneaCount = query.value("hypopnea_count").toInt();
    data.reraCount = query.value("rera_count").toInt();
    data.clearAirwayCount = query.value("clear_airway_count").toInt();
    data.obstructiveHypopneaCount = query.value("obstructive_hypopnea_count").toInt();
    data.centralHypopneaCount = query.value("central_hypopnea_count").toInt();
    data.allApneaCount = query.value("all_apnea_count").toInt();
    
    data.pressureAvg = query.value("pressure_avg").toDouble();
    data.pressureMin = query.value("pressure_min").toDouble();
    data.pressureMax = query.value("pressure_max").toDouble();
    data.pressure95th = query.value("pressure_95th").toDouble();
    
    data.leakTotalAvg = query.value("leak_total_avg").toDouble();
    data.leakTotal95th = query.value("leak_total_95th").toDouble();
    data.leakTotalMax = query.value("leak_total_max").toDouble();
    data.leakUnintentionalAvg = query.value("leak_unintentional_avg").toDouble();
    
    data.spo2Avg = query.value("spo2_avg").toDouble();
    data.spo2Min = query.value("spo2_min").toDouble();
    data.pulseAvg = query.value("pulse_avg").toDouble();
    data.pulseMin = query.value("pulse_min").toDouble();
    data.pulseMax = query.value("pulse_max").toDouble();
    
    data.isCompliant = query.value("is_compliant").toBool();
    data.hasOximetry = query.value("has_oximetry").toBool();
    
    data.calculatedAt = query.value("calculated_at").toString();
    data.sessionsHash = query.value("sessions_hash").toString();
    
    return data;
}
