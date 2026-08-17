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
    
    // Respiratory events
    double ahi = 0.0;
    double rdi = 0.0;
    double oahi = 0.0;                  // Obstructive AHI (schema v18); oahi + cahi == ahi
    double cahi = 0.0;                  // Central AHI (schema v18)
    int obstructiveCount = 0;
    int unclassifiedCount = 0;
    int hypopneaCount = 0;
    int reraCount = 0;
    int clearAirwayCount = 0;
    int obstructiveHypopneaCount = 0;   // Schema v18
    int centralHypopneaCount = 0;       // Schema v18
    int allApneaCount = 0;              // Schema v18; CPAP_AllApnea, an AHI contributor

    // Pressure statistics
    double pressureAvg = 0.0;
    double pressureMin = 0.0;
    double pressureMax = 0.0;
    double pressure95th = 0.0;
    
    // Leak statistics
    double leakTotalAvg = 0.0;
    double leakTotal95th = 0.0;
    double leakTotalMax = 0.0;
    double leakUnintentionalAvg = 0.0;
    
    // Oximetry (if available)
    double spo2Avg = 0.0;
    double spo2Min = 0.0;
    double pulseAvg = 0.0;
    double pulseMin = 0.0;
    double pulseMax = 0.0;
    
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
