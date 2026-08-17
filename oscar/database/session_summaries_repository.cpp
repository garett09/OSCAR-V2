/* Session Summaries Repository Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file implements the SessionSummariesRepository class for database
 * access to session summaries (cached high-level metrics like AHI).
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "session_summaries_repository.h"
#include "database_manager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>

SessionSummariesRepository::SessionSummariesRepository()
{
}

SessionSummariesRepository::~SessionSummariesRepository()
{
}

qint64 SessionSummariesRepository::create(const SessionSummaryData& data)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "SessionSummariesRepository::create() - Database not open";
        return -1;
    }

    QSqlQuery query(db);
    query.prepare(
        "INSERT INTO session_summaries "
        "(session_id, profile_id, ahi, rdi, oahi, cahi, obstructive_count, unclassified_count, hypopnea_count, rera_count, "
        " clear_airway_count, obstructive_hypopnea_count, central_hypopnea_count, all_apnea_count, "
        " pressure_avg, pressure_min, pressure_max, pressure_95th, "
        " leak_total_avg, leak_total_95th, leak_total_max, "
        " spo2_avg, spo2_min, pulse_avg, hours_used, mask_on_hours) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
    );

    query.addBindValue(data.sessionId);
    query.addBindValue(data.profileId);
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
    query.addBindValue(data.spo2Avg);
    query.addBindValue(data.spo2Min);
    query.addBindValue(data.pulseAvg);
    query.addBindValue(data.hoursUsed);
    query.addBindValue(data.maskOnHours);

    if (!query.exec()) {
        qWarning() << "SessionSummariesRepository::create() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("SessionSummariesRepository::create", query);
        return -1;
    }

    return query.lastInsertId().toLongLong();
}

bool SessionSummariesRepository::update(const SessionSummaryData& data)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "SessionSummariesRepository::update() - Database not open";
        return false;
    }

    QSqlQuery query(db);
    query.prepare(
        "UPDATE session_summaries SET "
        "ahi = ?, rdi = ?, oahi = ?, cahi = ?, obstructive_count = ?, unclassified_count = ?, "
        "hypopnea_count = ?, rera_count = ?, clear_airway_count = ?, "
        "obstructive_hypopnea_count = ?, central_hypopnea_count = ?, all_apnea_count = ?, "
        "pressure_avg = ?, pressure_min = ?, "
        "pressure_max = ?, pressure_95th = ?, leak_total_avg = ?, leak_total_95th = ?, "
        "leak_total_max = ?, spo2_avg = ?, spo2_min = ?, pulse_avg = ?, "
        "hours_used = ?, mask_on_hours = ?, updated_at = CURRENT_TIMESTAMP "
        "WHERE id = ?"
    );

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
    query.addBindValue(data.spo2Avg);
    query.addBindValue(data.spo2Min);
    query.addBindValue(data.pulseAvg);
    query.addBindValue(data.hoursUsed);
    query.addBindValue(data.maskOnHours);
    query.addBindValue(data.id);

    if (!query.exec()) {
        qWarning() << "SessionSummariesRepository::update() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("SessionSummariesRepository::update", query);
        return false;
    }

    return true;
}

SessionSummaryData SessionSummariesRepository::findBySession(qint64 sessionId)
{
    SessionSummaryData data;
    
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "SessionSummariesRepository::findBySession() - Database not open";
        return data;
    }

    QSqlQuery query(db);
    // Columns are read by name so that adding a column to the SELECT list never
    // silently shifts the positional indexes of the ones after it.
    query.prepare(
        "SELECT id, session_id, ahi, rdi, oahi, cahi, obstructive_count, unclassified_count, "
        "hypopnea_count, rera_count, clear_airway_count, "
        "obstructive_hypopnea_count, central_hypopnea_count, all_apnea_count, "
        "pressure_avg, pressure_min, pressure_max, pressure_95th, "
        "leak_total_avg, leak_total_95th, leak_total_max, spo2_avg, spo2_min, pulse_avg, "
        "hours_used, mask_on_hours, created_at, updated_at "
        "FROM session_summaries WHERE session_id = ?"
    );
    query.addBindValue(sessionId);

    if (!query.exec()) {
        qWarning() << "SessionSummariesRepository::findBySession() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("SessionSummariesRepository::findBySession", query);
        return data;
    }

    if (query.next()) {
        data.id = query.value("id").toLongLong();
        data.sessionId = query.value("session_id").toLongLong();
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
        data.spo2Avg = query.value("spo2_avg").toDouble();
        data.spo2Min = query.value("spo2_min").toDouble();
        data.pulseAvg = query.value("pulse_avg").toDouble();
        data.hoursUsed = query.value("hours_used").toDouble();
        data.maskOnHours = query.value("mask_on_hours").toDouble();
        data.createdAt = query.value("created_at").toDateTime();
        data.updatedAt = query.value("updated_at").toDateTime();
    }

    return data;
}

bool SessionSummariesRepository::remove(qint64 id)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "SessionSummariesRepository::remove() - Database not open";
        return false;
    }

    QSqlQuery query(db);
    query.prepare("DELETE FROM session_summaries WHERE id = ?");
    query.addBindValue(id);

    if (!query.exec()) {
        qWarning() << "SessionSummariesRepository::remove() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("SessionSummariesRepository::remove", query);
        return false;
    }

    return true;
}

bool SessionSummariesRepository::removeBySession(qint64 sessionId)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "SessionSummariesRepository::removeBySession() - Database not open";
        return false;
    }

    QSqlQuery query(db);
    query.prepare("DELETE FROM session_summaries WHERE session_id = ?");
    query.addBindValue(sessionId);

    if (!query.exec()) {
        qWarning() << "SessionSummariesRepository::removeBySession() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("SessionSummariesRepository::removeBySession", query);
        return false;
    }

    return true;
}

bool SessionSummariesRepository::exists(qint64 sessionId)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "SessionSummariesRepository::exists() - Database not open";
        return false;
    }

    QSqlQuery query(db);
    query.prepare("SELECT COUNT(*) FROM session_summaries WHERE session_id = ?");
    query.addBindValue(sessionId);

    if (!query.exec()) {
        qWarning() << "SessionSummariesRepository::exists() failed:" << query.lastError().text();
        DatabaseManager::instance().checkQueryError("SessionSummariesRepository::exists", query);
        return false;
    }

    if (query.next()) {
        return query.value(0).toInt() > 0;
    }

    return false;
}

bool SessionSummariesRepository::createOrUpdate(const SessionSummaryData& data)
{
    if (data.sessionId == 0) {
        qWarning() << "SessionSummariesRepository::createOrUpdate() - Invalid sessionId (0)";
        return false;
    }
    
    // Check if summary already exists
    SessionSummaryData existing = findBySession(data.sessionId);
    
    if (existing.id > 0) {
        // Update existing record
        SessionSummaryData updateData = data;
        updateData.id = existing.id;
        return update(updateData);
    } else {
        // Create new record
        qint64 newId = create(data);
        return newId > 0;
    }
}
