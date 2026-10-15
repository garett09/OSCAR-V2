/* Session Summaries Repository Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file defines the SessionSummariesRepository class for database
 * access to session summaries (cached high-level metrics like AHI).
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef SESSION_SUMMARIES_REPOSITORY_H
#define SESSION_SUMMARIES_REPOSITORY_H

#include <QString>
#include <QDateTime>
#include <QSqlDatabase>
#include <optional>

/*!
 * \struct SessionSummaryData
 * \brief Data structure for session summary records
 */
struct SessionSummaryData
{
    qint64 id = 0;                      // Database primary key
    qint64 sessionId = 0;               // Foreign key to sessions table (UNIQUE)
    qint64 profileId = 0;               // Foreign key to profiles table (Schema v12 denormalization)
    
    // Primary metrics. An absent optional is stored as SQL NULL and means "not
    // applicable to this device" (schema v19, GitLab #261); see
    // Notes/specs/2026-09-14-oh-ch-capability-gating-design.md §5.2. ahi is always present.
    double ahi = 0.0;                            // Apnea-Hypopnea Index
    std::optional<double> rdi;                   // Respiratory Disturbance Index; NULL unless the device reports RERA
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

    // Pressure statistics. NULL when the session has no pressure data.
    std::optional<double> pressureAvg;
    std::optional<double> pressureMin;
    std::optional<double> pressureMax;
    std::optional<double> pressure95th;          // NULL also when events were not loaded

    // Leak statistics. NULL when the session has no leak data.
    std::optional<double> leakTotalAvg;
    std::optional<double> leakTotal95th;
    std::optional<double> leakTotalMax;

    // Oximetry. NULL when the session has no oximetry data.
    std::optional<double> spo2Avg;
    std::optional<double> spo2Min;
    std::optional<double> pulseAvg;
    
    // Usage
    double hoursUsed = 0.0;
    double maskOnHours = 0.0;
    
    QDateTime createdAt;
    QDateTime updatedAt;
};

/*!
 * \class SessionSummariesRepository
 * \brief Repository for CRUD operations on session_summaries table
 */
class SessionSummariesRepository
{
public:
    SessionSummariesRepository();
    ~SessionSummariesRepository();
    
    /*!
     * \brief Create a new session summary record
     * \param data Summary data to insert
     * \return Database ID of created summary, or -1 on error
     */
    qint64 create(const SessionSummaryData& data);
    
    /*!
     * \brief Update an existing session summary
     * \param data Summary data with valid id
     * \return true if successful, false otherwise
     */
    bool update(const SessionSummaryData& data);
    
    /*!
     * \brief Find summary by session ID
     * \param sessionId Session database ID
     * \return SessionSummaryData (id will be 0 if not found)
     */
    SessionSummaryData findBySession(qint64 sessionId);
    
    /*!
     * \brief Delete a session summary
     * \param id Database primary key
     * \return true if successful, false otherwise
     */
    bool remove(qint64 id);
    
    /*!
     * \brief Delete summary by session ID
     * \param sessionId Session database ID
     * \return true if successful, false otherwise
     */
    bool removeBySession(qint64 sessionId);
    
    /*!
     * \brief Check if summary exists for session
     * \param sessionId Session database ID
     * \return true if exists, false otherwise
     */
    bool exists(qint64 sessionId);

    /*!
     * \brief Rebuild every metric column of session_summaries from session_channels.
     *
     * Applies the NULL rule of Notes/specs/2026-09-14-oh-ch-capability-gating-design.md
     * §5.2 to rows that already exist: a count column is NULL when the row's machine has
     * never reported the channel (no session_channels row with count > 0 on any of its
     * sessions), else the session's count or 0; ahi is the sum of session_channels.cph
     * over the AHI channels (#285); rdi / oahi / cahi likewise, NULLed when the machine
     * reports no RERA / no hypopnea mechanism; pressure, leak and oximetry statistics are
     * NULLed when the session has no row for the source channel.
     *
     * Used by the v18→v19 migration (all machines) and by Machine::Save() when a machine
     * reports a channel for the first time after rows were already stored (GitLab #261).
     *
     * \param db        Connection to use; the caller owns the transaction.
     * \param machineId machines.id to restrict to, or 0 for every machine.
     * \return true if every statement succeeded.
     */
    static bool rebuildFromChannels(QSqlDatabase& db, qint64 machineId);
    
    /*!
     * \brief Create or update session summary
     * \param data Summary data to insert or update
     * \return true if successful, false otherwise
     * 
     * If a summary already exists for the session, it will be updated.
     * Otherwise, a new summary will be created.
     */
    bool createOrUpdate(const SessionSummaryData& data);
};

#endif // SESSION_SUMMARIES_REPOSITORY_H
