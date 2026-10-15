/* Daily Summary Repository Header*
 * Copyright (c) 2025-2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef DAILY_SUMMARY_REPOSITORY_H
#define DAILY_SUMMARY_REPOSITORY_H

#include <QSqlDatabase>
#include <QString>
#include <QDate>
#include <QList>
#include <optional>

// Forward declarations
class Day;

struct DailySummaryData {
    qint64 id = 0;
    qint64 profileId = 0;
    QString date;  // ISO format YYYY-MM-DD

    // Session counts
    int sessionCount = 0;
    int enabledSessionCount = 0;
    
    // Time metrics
    double totalHours = 0.0;
    double maskOnHours = 0.0;
    
    // Respiratory events. An absent optional is stored as SQL NULL and means "not
    // applicable to this day's device" (schema v19, GitLab #261); see
    // Notes/specs/2026-09-14-oh-ch-capability-gating-design.md §5.2. ahi is always present.
    double ahi = 0.0;
    std::optional<double> rdi;                   // NULL unless the device reports RERA
    std::optional<double> oahi;                  // Obstructive AHI (v18); NULL unless the device splits hypopneas by mechanism
    std::optional<double> cahi;                  // Central AHI (v18); oahi + cahi == ahi when present
    // Event counts. NULL when the device has never reported the channel; 0 = scored none.
    std::optional<int> obstructiveCount;
    std::optional<int> unclassifiedCount;
    std::optional<int> hypopneaCount;
    std::optional<int> reraCount;
    std::optional<int> clearAirwayCount;
    std::optional<int> obstructiveHypopneaCount; // Schema v18
    std::optional<int> centralHypopneaCount;     // Schema v18
    std::optional<int> allApneaCount;            // Schema v18; CPAP_AllApnea, an AHI contributor

    // Pressure statistics. NULL when the day has no pressure data.
    std::optional<double> pressureAvg;
    std::optional<double> pressureMin;
    std::optional<double> pressureMax;
    std::optional<double> pressure95th;

    // Leak statistics. NULL when the day has no leak data.
    std::optional<double> leakTotalAvg;
    std::optional<double> leakTotal95th;
    std::optional<double> leakTotalMax;
    std::optional<double> leakUnintentionalAvg;

    // Oximetry. NULL when the day has no oximetry data.
    std::optional<double> spo2Avg;
    std::optional<double> spo2Min;
    std::optional<double> pulseAvg;
    std::optional<double> pulseMin;
    std::optional<double> pulseMax;
    
    // Flags
    bool isCompliant = false;
    bool hasOximetry = false;
    
    // Metadata
    QString calculatedAt;
    QString sessionsHash;
};

class DailySummaryRepository
{
public:
    DailySummaryRepository();
    explicit DailySummaryRepository(const QSqlDatabase& database);
    
    // CRUD operations
    qint64 create(const DailySummaryData& data);
    bool update(const DailySummaryData& data);
    bool remove(qint64 id);
    
    // Query methods
    DailySummaryData findById(qint64 id);
    DailySummaryData findByProfileAndDate(qint64 profileId, const QDate& date);
    QList<DailySummaryData> findByProfile(qint64 profileId);
    QList<DailySummaryData> findRange(qint64 profileId, const QDate& startDate, const QDate& endDate);

    // Calculation methods
    bool calculateAndStore(qint64 profileId, const QDate& date);
    bool calculateAndStoreFromDay(Day* day, qint64 profileId);
    bool calculateRange(qint64 profileId, const QDate& startDate, const QDate& endDate);

    // Cache invalidation
    bool invalidateDate(qint64 profileId, const QDate& date);
    bool invalidateRange(qint64 profileId, const QDate& startDate, const QDate& endDate);

    // Utility methods
    bool exists(qint64 profileId, const QDate& date);
    int countDays(qint64 profileId, const QDate& startDate, const QDate& endDate);

private:
    QSqlDatabase db;

    DailySummaryData mapResultToData(const QSqlQuery& query);
    QString generateSessionsHash(qint64 profileId, const QDate& date);
    DailySummaryData calculateFromDay(Day* day);
};

#endif // DAILY_SUMMARY_REPOSITORY_H
