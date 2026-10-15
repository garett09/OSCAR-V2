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
#include <QStringList>
#include <QVariant>
#include <QDebug>

namespace {
//! \brief Bind an optional metric: SQL NULL when it is absent, the value otherwise.
template <class T>
QVariant bindOptional(const std::optional<T> & v)
{
    return v ? QVariant::fromValue(*v) : QVariant();
}

//! \brief Read a REAL column that may be NULL.
std::optional<double> optionalDouble(const QVariant & v)
{
    return v.isNull() ? std::nullopt : std::optional<double>(v.toDouble());
}

//! \brief Read an INTEGER column that may be NULL.
std::optional<int> optionalInt(const QVariant & v)
{
    return v.isNull() ? std::nullopt : std::optional<int>(v.toInt());
}
} // namespace

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
    query.addBindValue(bindOptional(data.rdi));
    query.addBindValue(bindOptional(data.oahi));
    query.addBindValue(bindOptional(data.cahi));
    query.addBindValue(bindOptional(data.obstructiveCount));
    query.addBindValue(bindOptional(data.unclassifiedCount));
    query.addBindValue(bindOptional(data.hypopneaCount));
    query.addBindValue(bindOptional(data.reraCount));
    query.addBindValue(bindOptional(data.clearAirwayCount));
    query.addBindValue(bindOptional(data.obstructiveHypopneaCount));
    query.addBindValue(bindOptional(data.centralHypopneaCount));
    query.addBindValue(bindOptional(data.allApneaCount));
    query.addBindValue(bindOptional(data.pressureAvg));
    query.addBindValue(bindOptional(data.pressureMin));
    query.addBindValue(bindOptional(data.pressureMax));
    query.addBindValue(bindOptional(data.pressure95th));
    query.addBindValue(bindOptional(data.leakTotalAvg));
    query.addBindValue(bindOptional(data.leakTotal95th));
    query.addBindValue(bindOptional(data.leakTotalMax));
    query.addBindValue(bindOptional(data.spo2Avg));
    query.addBindValue(bindOptional(data.spo2Min));
    query.addBindValue(bindOptional(data.pulseAvg));
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
    query.addBindValue(bindOptional(data.rdi));
    query.addBindValue(bindOptional(data.oahi));
    query.addBindValue(bindOptional(data.cahi));
    query.addBindValue(bindOptional(data.obstructiveCount));
    query.addBindValue(bindOptional(data.unclassifiedCount));
    query.addBindValue(bindOptional(data.hypopneaCount));
    query.addBindValue(bindOptional(data.reraCount));
    query.addBindValue(bindOptional(data.clearAirwayCount));
    query.addBindValue(bindOptional(data.obstructiveHypopneaCount));
    query.addBindValue(bindOptional(data.centralHypopneaCount));
    query.addBindValue(bindOptional(data.allApneaCount));
    query.addBindValue(bindOptional(data.pressureAvg));
    query.addBindValue(bindOptional(data.pressureMin));
    query.addBindValue(bindOptional(data.pressureMax));
    query.addBindValue(bindOptional(data.pressure95th));
    query.addBindValue(bindOptional(data.leakTotalAvg));
    query.addBindValue(bindOptional(data.leakTotal95th));
    query.addBindValue(bindOptional(data.leakTotalMax));
    query.addBindValue(bindOptional(data.spo2Avg));
    query.addBindValue(bindOptional(data.spo2Min));
    query.addBindValue(bindOptional(data.pulseAvg));
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
        data.rdi = optionalDouble(query.value("rdi"));
        data.oahi = optionalDouble(query.value("oahi"));
        data.cahi = optionalDouble(query.value("cahi"));
        data.obstructiveCount = optionalInt(query.value("obstructive_count"));
        data.unclassifiedCount = optionalInt(query.value("unclassified_count"));
        data.hypopneaCount = optionalInt(query.value("hypopnea_count"));
        data.reraCount = optionalInt(query.value("rera_count"));
        data.clearAirwayCount = optionalInt(query.value("clear_airway_count"));
        data.obstructiveHypopneaCount = optionalInt(query.value("obstructive_hypopnea_count"));
        data.centralHypopneaCount = optionalInt(query.value("central_hypopnea_count"));
        data.allApneaCount = optionalInt(query.value("all_apnea_count"));
        data.pressureAvg = optionalDouble(query.value("pressure_avg"));
        data.pressureMin = optionalDouble(query.value("pressure_min"));
        data.pressureMax = optionalDouble(query.value("pressure_max"));
        data.pressure95th = optionalDouble(query.value("pressure_95th"));
        data.leakTotalAvg = optionalDouble(query.value("leak_total_avg"));
        data.leakTotal95th = optionalDouble(query.value("leak_total_95th"));
        data.leakTotalMax = optionalDouble(query.value("leak_total_max"));
        data.spo2Avg = optionalDouble(query.value("spo2_avg"));
        data.spo2Min = optionalDouble(query.value("spo2_min"));
        data.pulseAvg = optionalDouble(query.value("pulse_avg"));
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

bool SessionSummariesRepository::rebuildFromChannels(QSqlDatabase& db, qint64 machineId)
{
    // Every statement is scoped by this predicate on the session's machine. With
    // machineId == 0 it is empty, so one statement text serves both callers.
    const QString scope = machineId > 0
        ? QString(" AND s.machine_id = %1").arg(machineId)
        : QString();
    const QString scopedSessions = QString("(SELECT s.id FROM sessions s WHERE 1=1%1)").arg(scope);

    // Which channels each machine has ever reported (count > 0 on some session). A temp
    // table keeps the per-row CASEs below to an indexed lookup.
    QStringList sql;
    sql << "DROP TABLE IF EXISTS temp.machine_reported"
        << "CREATE TEMP TABLE machine_reported AS"
           " SELECT DISTINCT s.machine_id, sc.channel_id"
           " FROM session_channels sc JOIN sessions s ON s.id = sc.session_id"
           " WHERE sc.count > 0" + scope
        << "CREATE INDEX temp.idx_machine_reported ON machine_reported(machine_id, channel_id)";

    // "The row's machine has reported one of these channels."
    const QString reported =
        "EXISTS (SELECT 1 FROM sessions s JOIN machine_reported mr ON mr.machine_id = s.machine_id"
        " WHERE s.id = session_summaries.session_id AND mr.channel_id IN (%1))";

    // Count columns: NULL if the machine never reported the channel, else the row's
    // count or 0. Channel ids follow schema.cpp: ClearAirway 4097, Obstructive 4098,
    // Hypopnea 4099, Apnea 4100, RERA 4102, AllApnea 4112, ObstructiveHypopnea 4113,
    // CentralHypopnea 4114.
    struct CountCol { const char * column; int channel; };
    static const CountCol countCols[] = {
        { "clear_airway_count",         4097 }, { "obstructive_count",      4098 },
        { "hypopnea_count",             4099 }, { "unclassified_count",     4100 },
        { "rera_count",                 4102 }, { "all_apnea_count",        4112 },
        { "obstructive_hypopnea_count", 4113 }, { "central_hypopnea_count", 4114 },
    };
    for (const CountCol & c : countCols) {
        sql << QString("UPDATE session_summaries SET %1 = CASE WHEN %2"
                       " THEN COALESCE((SELECT sc.count FROM session_channels sc"
                       "   WHERE sc.session_id = session_summaries.session_id AND sc.channel_id = %3), 0)"
                       " ELSE NULL END"
                       " WHERE session_id IN %4")
               .arg(QString::fromLatin1(c.column))
               .arg(reported.arg(c.channel))
               .arg(c.channel)
               .arg(scopedSessions);
    }

    // Indices from cph (events per mask-on hour as a REAL): the INTEGER counts truncate
    // ResMed summary-only sessions' fractional counts (#285, see the v18 migration note).
    // Groups follow ahiChannels / oahiChannels / cahiChannels in schema.cpp. A session
    // with no rows in a group sums to NULL, hence the COALESCE to 0.
    const QString sumCph = "COALESCE((SELECT SUM(sc.cph) FROM session_channels sc"
                           " WHERE sc.session_id = session_summaries.session_id"
                           " AND sc.channel_id IN (%1)), 0)";
    sql << QString("UPDATE session_summaries SET"
                   " ahi = %1,"
                   " rdi = CASE WHEN %2 THEN %3 ELSE NULL END,"
                   " oahi = CASE WHEN %4 THEN %5 ELSE NULL END,"
                   " cahi = CASE WHEN %4 THEN %6 ELSE NULL END"
                   " WHERE session_id IN %7")
           .arg(sumCph.arg("4097, 4098, 4099, 4100, 4112, 4113, 4114"))
           .arg(reported.arg("4102"))
           .arg(sumCph.arg("4097, 4098, 4099, 4100, 4102, 4112, 4113, 4114"))
           .arg(reported.arg("4113, 4114"))
           .arg(sumCph.arg("4098, 4099, 4100, 4112, 4113"))
           .arg(sumCph.arg("4097, 4114"))
           .arg(scopedSessions);

    // Continuous statistics: NULL when the session has no row for the source channel
    // (Pressure 0x110C, LeakTotal 0x1117, OXI_SPO2 0x1801, OXI_Pulse 0x1800).
    struct StatCols { const char * assignments; int channel; };
    static const StatCols statCols[] = {
        { "pressure_avg = NULL, pressure_min = NULL, pressure_max = NULL, pressure_95th = NULL", 4364 },
        { "leak_total_avg = NULL, leak_total_95th = NULL, leak_total_max = NULL",                4375 },
        { "spo2_avg = NULL, spo2_min = NULL",                                                    6145 },
        { "pulse_avg = NULL",                                                                    6144 },
    };
    for (const StatCols & c : statCols) {
        sql << QString("UPDATE session_summaries SET %1"
                       " WHERE NOT EXISTS (SELECT 1 FROM session_channels sc"
                       "   WHERE sc.session_id = session_summaries.session_id AND sc.channel_id = %2)"
                       " AND session_id IN %3")
               .arg(QString::fromLatin1(c.assignments))
               .arg(c.channel)
               .arg(scopedSessions);
    }
    // A stored 0 with pressure present means the percentile was never computed (events
    // were not loaded when the row was written); therapy pressure itself is never 0.
    sql << QString("UPDATE session_summaries SET pressure_95th = NULL"
                   " WHERE pressure_95th = 0 AND pressure_avg IS NOT NULL"
                   " AND session_id IN %1").arg(scopedSessions)
        << "DROP TABLE IF EXISTS temp.machine_reported";

    QSqlQuery q(db);
    for (const QString & statement : sql) {
        if (!q.exec(statement)) {
            qCritical() << "SessionSummariesRepository::rebuildFromChannels failed:"
                        << q.lastError().text() << "in" << statement.left(80);
            DatabaseManager::instance().checkQueryError("SessionSummariesRepository::rebuildFromChannels", q);
            return false;
        }
    }
    return true;
}
