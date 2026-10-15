/* Device Time Correction Repository Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef DEVICE_TIME_CORRECTION_REPOSITORY_H
#define DEVICE_TIME_CORRECTION_REPOSITORY_H

#include <QString>
#include <QList>
#include <QSqlDatabase>

struct DeviceTimeCorrectionData {
    qint64  id        = 0;
    qint64  machineId = 0;
    QString dateFrom;       // "YYYY-MM-DD"
    QString dateTo;         // empty = open-ended
    QString type;           // 'timezone'|'travel'|'dst'|'reset'|'offset'|'drift' (drift=continuous model, c1!=0)
    qint64  offsetMs  = 0;  // constant correction (c1 == 0)
    qint64  c0Ms      = 0;  // model drift intercept (c1 != 0)
    double  c1        = 0.0;
    QString reason;
    QString appliedAt;
    QString undoneAt;       // empty = active
};

class DeviceTimeCorrectionRepository
{
public:
    qint64  create(const DeviceTimeCorrectionData& data);

    QList<DeviceTimeCorrectionData> findActive(qint64 machineId);

    // Upsert a row with an explicit date range (marks prior matching row undone, inserts new).
    // Pass dateTo="" for open-ended. offsetMs=0 clears without inserting.
    qint64  upsertTyped(qint64 machineId, const QString& dateFrom, const QString& dateTo,
                        const QString& type, qint64 offsetMs);

    // Convenience: single-night (dateFrom == dateTo).
    qint64  upsertTyped(qint64 machineId, const QString& date, const QString& type, qint64 offsetMs);

    // Update date_to on an existing row (pass "" for open-ended / NULL).
    bool    updateDateTo(qint64 id, const QString& dateTo);

    qint64  upsertOffset(qint64 machineId, const QString& date, qint64 offsetMs);

    // Supersede any active row for this (machine, date range, 'drift') and insert a new
    // drift row carrying the (c0Ms, c1) model. dateTo="" = open-ended.
    qint64  upsertDrift(qint64 machineId, const QString& dateFrom, const QString& dateTo,
                        qint64 c0Ms, double c1);

    // Atomically replace corrections from dateFrom onward with one constant.
    // Preserve historical corrections before dateFrom. Roll back on any error.
    bool replaceWithConstant(qint64 machineId, const QString& dateFrom, qint64 offsetMs);

    bool    markUndone(qint64 id);

    QList<DeviceTimeCorrectionData> findManualOffsetRows(qint64 machineId);

    // Returns all rows for the machine including undone (superseded) ones.
    // Used by ClockDriftHistoryDialog when "Show undone" is checked.
    QList<DeviceTimeCorrectionData> findAllIncludingUndone(qint64 machineId);

private:
    QSqlDatabase database();
    DeviceTimeCorrectionData recordToData(QSqlQuery& q);
};

#endif // DEVICE_TIME_CORRECTION_REPOSITORY_H
