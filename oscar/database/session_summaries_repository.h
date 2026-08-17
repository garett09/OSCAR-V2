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

/*!
 * \struct SessionSummaryData
 * \brief Data structure for session summary records
 */
struct SessionSummaryData
{
    qint64 id = 0;                      // Database primary key
    qint64 sessionId = 0;               // Foreign key to sessions table (UNIQUE)
    qint64 profileId = 0;               // Foreign key to profiles table (Schema v12 denormalization)
    
    // Primary metrics
    double ahi = 0.0;                   // Apnea-Hypopnea Index
    double rdi = 0.0;                   // Respiratory Disturbance Index
    double oahi = 0.0;                  // Obstructive AHI (schema v18); oahi + cahi == ahi
    double cahi = 0.0;                  // Central AHI (schema v18)

    // Event counts
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
    
    // Oximetry
    double spo2Avg = 0.0;
    double spo2Min = 0.0;
    double pulseAvg = 0.0;
    
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
