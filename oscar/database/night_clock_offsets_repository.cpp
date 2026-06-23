/* Night Clock Offsets Repository Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "night_clock_offsets_repository.h"
#include "database_manager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>

QSqlDatabase NightClockOffsetsRepository::database()
{
    return DatabaseManager::instance().database();
}

NightClockOffsetData NightClockOffsetsRepository::recordToData(QSqlQuery& q)
{
    NightClockOffsetData d;
    d.id            = q.value("id").toLongLong();
    d.machineId     = q.value("machine_id").toLongLong();
    d.refMachineId  = q.value("ref_machine_id").toLongLong();
    d.date          = q.value("date").toString();
    d.startDiffMs   = q.value("start_diff_ms").toLongLong();
    d.endDiffMs     = q.value("end_diff_ms").toLongLong();
    d.midMs         = q.value("mid_ms").toLongLong();
    d.used          = q.value("used").toInt() != 0;
    d.validated     = q.value("validated").toInt() != 0;
    QVariant va     = q.value("validator_agrees");
    d.validatorAgrees = va.isNull() ? -1 : va.toInt();
    d.recordedAt    = q.value("recorded_at").toString();
    return d;
}

void NightClockOffsetsRepository::upsertNights(qint64 machineId, qint64 refMachineId,
                                               const QVector<NightStartDiff>& nights)
{
    QSqlDatabase db = database();
    QSqlQuery q(db);
    q.prepare(R"(
        INSERT INTO night_clock_offsets
            (machine_id, ref_machine_id, date,
             start_diff_ms, end_diff_ms, mid_ms,
             used, validated, validator_agrees)
        VALUES
            (:machine_id, :ref_machine_id, :date,
             :start_diff_ms, :end_diff_ms, :mid_ms,
             :used, :validated, :validator_agrees)
        ON CONFLICT(machine_id, ref_machine_id, date) DO UPDATE SET
            start_diff_ms    = excluded.start_diff_ms,
            end_diff_ms      = excluded.end_diff_ms,
            mid_ms           = excluded.mid_ms,
            used             = excluded.used,
            validated        = excluded.validated,
            validator_agrees = excluded.validator_agrees,
            recorded_at      = datetime('now')
    )");

    for (const NightStartDiff& n : nights) {
        q.bindValue(":machine_id",     machineId);
        q.bindValue(":ref_machine_id", refMachineId);
        q.bindValue(":date",           n.date.toString(Qt::ISODate));
        q.bindValue(":start_diff_ms",  n.startDiffMs);
        q.bindValue(":end_diff_ms",    n.endDiffMs);
        q.bindValue(":mid_ms",         n.midMs);
        q.bindValue(":used",           n.used ? 1 : 0);
        q.bindValue(":validated",      n.validated ? 1 : 0);
        if (n.validated) {
            q.bindValue(":validator_agrees", n.validatorAgrees ? 1 : 0);
        } else {
            q.bindValue(":validator_agrees", QVariant(QMetaType::fromType<int>()));
        }
        if (!q.exec()) {
            qCritical() << "NightClockOffsetsRepository::upsertNights failed for"
                        << n.date.toString() << ":" << q.lastError().text();
            DatabaseManager::instance().checkQueryError(
                "NightClockOffsetsRepository::upsertNights", q);
        }
    }
}

QList<NightClockOffsetData> NightClockOffsetsRepository::findByMachines(
    qint64 machineId, qint64 refMachineId, const QDate& from, const QDate& to)
{
    QString sql = R"(
        SELECT id, machine_id, ref_machine_id, date,
               start_diff_ms, end_diff_ms, mid_ms,
               used, validated, validator_agrees, recorded_at
        FROM night_clock_offsets
        WHERE machine_id = :machine_id AND ref_machine_id = :ref_machine_id
    )";
    if (from.isValid()) sql += " AND date >= :from";
    if (to.isValid())   sql += " AND date <= :to";
    sql += " ORDER BY date ASC";

    QSqlQuery q(database());
    q.prepare(sql);
    q.bindValue(":machine_id",     machineId);
    q.bindValue(":ref_machine_id", refMachineId);
    if (from.isValid()) q.bindValue(":from", from.toString(Qt::ISODate));
    if (to.isValid())   q.bindValue(":to",   to.toString(Qt::ISODate));

    QList<NightClockOffsetData> rows;
    if (!q.exec()) {
        qCritical() << "NightClockOffsetsRepository::findByMachines failed:"
                    << q.lastError().text();
        DatabaseManager::instance().checkQueryError(
            "NightClockOffsetsRepository::findByMachines", q);
        return rows;
    }
    while (q.next()) rows.append(recordToData(q));
    return rows;
}
