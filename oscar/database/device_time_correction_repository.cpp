/* Device Time Correction Repository Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "device_time_correction_repository.h"
#include "database_manager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>
#include <QDate>

QSqlDatabase DeviceTimeCorrectionRepository::database()
{
    return DatabaseManager::instance().database();
}

DeviceTimeCorrectionData DeviceTimeCorrectionRepository::recordToData(QSqlQuery& q)
{
    DeviceTimeCorrectionData d;
    d.id        = q.value("id").toLongLong();
    d.machineId = q.value("machine_id").toLongLong();
    d.dateFrom  = q.value("date_from").toString();
    d.dateTo    = q.value("date_to").isNull() ? QString() : q.value("date_to").toString();
    d.type      = q.value("type").toString();
    d.offsetMs  = q.value("offset_ms").toLongLong();
    d.c0Ms      = q.value("c0_ms").toLongLong();
    d.c1        = q.value("c1").toDouble();
    d.reason    = q.value("reason").toString();
    d.appliedAt = q.value("applied_at").toString();
    d.undoneAt  = q.value("undone_at").isNull() ? QString() : q.value("undone_at").toString();
    return d;
}

qint64 DeviceTimeCorrectionRepository::create(const DeviceTimeCorrectionData& data)
{
    QSqlQuery q(database());
    q.prepare(R"(
        INSERT INTO device_time_corrections
            (machine_id, date_from, date_to, type, offset_ms, c0_ms, c1, reason)
        VALUES
            (:machine_id, :date_from, :date_to, :type, :offset_ms, :c0_ms, :c1, :reason)
    )");
    q.bindValue(":machine_id", data.machineId);
    q.bindValue(":date_from",  data.dateFrom);
    q.bindValue(":date_to",    data.dateTo.isEmpty() ? QVariant() : QVariant(data.dateTo));
    q.bindValue(":type",       data.type);
    q.bindValue(":offset_ms",  data.offsetMs);
    q.bindValue(":c0_ms",      data.c0Ms);
    q.bindValue(":c1",         data.c1);
    q.bindValue(":reason",     data.reason.isEmpty() ? QVariant() : QVariant(data.reason));

    if (!q.exec()) {
        qCritical() << "DeviceTimeCorrectionRepository::create failed:" << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DeviceTimeCorrectionRepository::create", q);
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

QList<DeviceTimeCorrectionData> DeviceTimeCorrectionRepository::findActive(qint64 machineId)
{
    QSqlQuery q(database());
    q.prepare(R"(
        SELECT id, machine_id, date_from, date_to, type, offset_ms, c0_ms, c1,
               reason, applied_at, undone_at
        FROM device_time_corrections
        WHERE machine_id = :machine_id AND undone_at IS NULL
        ORDER BY date_from, id
    )");
    q.bindValue(":machine_id", machineId);

    QList<DeviceTimeCorrectionData> rows;
    if (!q.exec()) {
        qCritical() << "DeviceTimeCorrectionRepository::findActive failed:" << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DeviceTimeCorrectionRepository::findActive", q);
        return rows;
    }
    while (q.next()) {
        rows.append(recordToData(q));
    }
    return rows;
}

qint64 DeviceTimeCorrectionRepository::upsertTyped(qint64 machineId, const QString& dateFrom,
                                                   const QString& dateTo, const QString& type,
                                                   qint64 offsetMs)
{
    QSqlQuery q(database());
    if (dateTo.isEmpty()) {
        q.prepare(R"(
            UPDATE device_time_corrections
            SET undone_at = datetime('now')
            WHERE machine_id = :machine_id
              AND date_from = :date_from AND date_to IS NULL
              AND type = :type AND undone_at IS NULL
        )");
    } else {
        q.prepare(R"(
            UPDATE device_time_corrections
            SET undone_at = datetime('now')
            WHERE machine_id = :machine_id
              AND date_from = :date_from AND date_to = :date_to
              AND type = :type AND undone_at IS NULL
        )");
        q.bindValue(":date_to", dateTo);
    }
    q.bindValue(":machine_id", machineId);
    q.bindValue(":date_from",  dateFrom);
    q.bindValue(":type",       type);
    if (!q.exec()) {
        qCritical() << "DeviceTimeCorrectionRepository::upsertTyped mark-undone failed:" << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DeviceTimeCorrectionRepository::upsertTyped", q);
        return -1;
    }
    if (offsetMs == 0) return 0;

    DeviceTimeCorrectionData row;
    row.machineId = machineId;
    row.dateFrom  = dateFrom;
    row.dateTo    = dateTo;
    row.type      = type;
    row.offsetMs  = offsetMs;
    return create(row);
}

qint64 DeviceTimeCorrectionRepository::upsertTyped(qint64 machineId, const QString& date,
                                                   const QString& type, qint64 offsetMs)
{
    return upsertTyped(machineId, date, date, type, offsetMs);
}

qint64 DeviceTimeCorrectionRepository::upsertDrift(qint64 machineId, const QString& dateFrom,
                                                   const QString& dateTo, qint64 c0Ms, double c1)
{
    QSqlQuery q(database());
    if (dateTo.isEmpty()) {
        q.prepare(R"(
            UPDATE device_time_corrections
            SET undone_at = datetime('now')
            WHERE machine_id = :machine_id
              AND date_from = :date_from AND date_to IS NULL
              AND type = 'drift' AND undone_at IS NULL
        )");
    } else {
        q.prepare(R"(
            UPDATE device_time_corrections
            SET undone_at = datetime('now')
            WHERE machine_id = :machine_id
              AND date_from = :date_from AND date_to = :date_to
              AND type = 'drift' AND undone_at IS NULL
        )");
        q.bindValue(":date_to", dateTo);
    }
    q.bindValue(":machine_id", machineId);
    q.bindValue(":date_from",  dateFrom);
    if (!q.exec()) {
        qCritical() << "DeviceTimeCorrectionRepository::upsertDrift mark-undone failed:" << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DeviceTimeCorrectionRepository::upsertDrift", q);
        return -1;
    }

    DeviceTimeCorrectionData row;
    row.machineId = machineId;
    row.dateFrom  = dateFrom;
    row.dateTo    = dateTo;
    row.type      = "drift";
    row.c0Ms      = c0Ms;
    row.c1        = c1;
    return create(row);
}

bool DeviceTimeCorrectionRepository::updateDateTo(qint64 id, const QString& dateTo)
{
    QSqlQuery q(database());
    q.prepare("UPDATE device_time_corrections SET date_to = :date_to WHERE id = :id");
    q.bindValue(":date_to", dateTo.isEmpty() ? QVariant() : QVariant(dateTo));
    q.bindValue(":id", id);
    if (!q.exec()) {
        qCritical() << "DeviceTimeCorrectionRepository::updateDateTo failed:" << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DeviceTimeCorrectionRepository::updateDateTo", q);
        return false;
    }
    return true;
}

qint64 DeviceTimeCorrectionRepository::upsertOffset(qint64 machineId, const QString& date, qint64 offsetMs)
{
    return upsertTyped(machineId, date, date, "offset", offsetMs);
}

bool DeviceTimeCorrectionRepository::replaceWithConstant(qint64 machineId,
                                                          const QString& dateFrom,
                                                          qint64 offsetMs)
{
    const QDate from = QDate::fromString(dateFrom, Qt::ISODate);
    if (machineId <= 0 || !from.isValid()) return false;
    QSqlDatabase db = database();
    if (!db.transaction()) return false;

    QSqlQuery q(db);
    // Retain history, shortening corrections that cross the new start date.
    q.prepare("UPDATE device_time_corrections SET date_to = :until "
              "WHERE machine_id = :machine AND undone_at IS NULL "
              "AND date_from < :from AND (date_to IS NULL OR date_to >= :from)");
    q.bindValue(":until", from.addDays(-1).toString(Qt::ISODate));
    q.bindValue(":machine", machineId);
    q.bindValue(":from", dateFrom);
    if (!q.exec()) { db.rollback(); return false; }

    q.prepare("UPDATE device_time_corrections SET undone_at = datetime('now') "
              "WHERE machine_id = :machine AND undone_at IS NULL AND date_from >= :from");
    q.bindValue(":machine", machineId);
    q.bindValue(":from", dateFrom);
    if (!q.exec()) { db.rollback(); return false; }

    // A zero correction intentionally clears this date range.
    if (offsetMs != 0) {
        DeviceTimeCorrectionData row;
        row.machineId = machineId;
        row.dateFrom = dateFrom;
        row.type = "offset";
        row.offsetMs = offsetMs;
        if (create(row) <= 0) { db.rollback(); return false; }
    }
    if (!db.commit()) { db.rollback(); return false; }
    return true;
}

bool DeviceTimeCorrectionRepository::markUndone(qint64 id)
{
    QSqlQuery q(database());
    q.prepare("UPDATE device_time_corrections SET undone_at = datetime('now') WHERE id = :id");
    q.bindValue(":id", id);
    if (!q.exec()) {
        qCritical() << "DeviceTimeCorrectionRepository::markUndone failed:" << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DeviceTimeCorrectionRepository::markUndone", q);
        return false;
    }
    return true;
}

QList<DeviceTimeCorrectionData> DeviceTimeCorrectionRepository::findManualOffsetRows(qint64 machineId)
{
    QSqlQuery q(database());
    q.prepare(R"(
        SELECT id, machine_id, date_from, date_to, type, offset_ms, c0_ms, c1,
               reason, applied_at, undone_at
        FROM device_time_corrections
        WHERE machine_id = :machine_id
          AND type = 'offset'
          AND date_from = date_to
          AND undone_at IS NULL
        ORDER BY date_from
    )");
    q.bindValue(":machine_id", machineId);

    QList<DeviceTimeCorrectionData> rows;
    if (!q.exec()) {
        qCritical() << "DeviceTimeCorrectionRepository::findManualOffsetRows failed:" << q.lastError().text();
        DatabaseManager::instance().checkQueryError("DeviceTimeCorrectionRepository::findManualOffsetRows", q);
        return rows;
    }
    while (q.next()) {
        rows.append(recordToData(q));
    }
    return rows;
}

QList<DeviceTimeCorrectionData> DeviceTimeCorrectionRepository::findAllIncludingUndone(qint64 machineId)
{
    QSqlQuery q(database());
    q.prepare(R"(
        SELECT id, machine_id, date_from, date_to, type, offset_ms, c0_ms, c1,
               reason, applied_at, undone_at
        FROM device_time_corrections
        WHERE machine_id = :machine_id
        ORDER BY date_from ASC, id ASC
    )");
    q.bindValue(":machine_id", machineId);

    QList<DeviceTimeCorrectionData> rows;
    if (!q.exec()) {
        qCritical() << "DeviceTimeCorrectionRepository::findAllIncludingUndone failed:"
                    << q.lastError().text();
        DatabaseManager::instance().checkQueryError(
            "DeviceTimeCorrectionRepository::findAllIncludingUndone", q);
        return rows;
    }
    while (q.next()) {
        rows.append(recordToData(q));
    }
    return rows;
}
