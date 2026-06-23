/* Night Clock Offsets Repository Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef NIGHT_CLOCK_OFFSETS_REPOSITORY_H
#define NIGHT_CLOCK_OFFSETS_REPOSITORY_H

#include <QString>
#include <QList>
#include <QVector>
#include <QDate>
#include <QSqlDatabase>
#include "clocksyncanalyzer.h"

struct NightClockOffsetData {
    qint64  id            = 0;
    qint64  machineId     = 0;
    qint64  refMachineId  = 0;
    QString date;                // "YYYY-MM-DD"
    qint64  startDiffMs   = 0;
    qint64  endDiffMs     = 0;
    qint64  midMs         = 0;
    bool    used          = true;
    bool    validated     = false;
    int     validatorAgrees = -1; // -1 = NULL (not validated), 0 = disagrees, 1 = agrees
    QString recordedAt;
};

class NightClockOffsetsRepository
{
public:
    // Upsert a batch of per-night measurements for a CPAP/reference pair.
    // Safe to call on re-analysis — UNIQUE(machine_id, ref_machine_id, date)
    // ensures re-runs update rather than duplicate.
    void upsertNights(qint64 machineId, qint64 refMachineId,
                      const QVector<NightStartDiff>& nights);

    // Retrieve stored measurements for a pair. Pass invalid QDate for either
    // bound to omit that filter.
    QList<NightClockOffsetData> findByMachines(qint64 machineId, qint64 refMachineId,
                                               const QDate& from = QDate(),
                                               const QDate& to   = QDate());

private:
    QSqlDatabase database();
    NightClockOffsetData recordToData(QSqlQuery& q);
};

#endif // NIGHT_CLOCK_OFFSETS_REPOSITORY_H
