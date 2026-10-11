/* SleepLib Session Split Header
 *
 * Cuts a freshly imported session into pieces at the day-split time, so oximetry,
 * sleep-stage and position recordings line up with CPAP days that the device itself
 * splits at noon (ResMed). Used by Machine::AddSession() (GitLab #216).
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef SESSION_SPLIT_H
#define SESSION_SPLIT_H

#include <QList>
#include <QTime>

#include "SleepLib/machine_common.h"

class Session;
class EventList;

/*! \namespace SessionSplit
    \brief Helpers that cut an in-memory Session at given instants.

    Only a session still holding its EventLists can be cut, i.e. one built by a loader
    and not yet stored. Nothing here touches the profile or the database; the caller
    (Machine::AddSession) decides whether and where to cut.
    */
namespace SessionSplit {

/*! \brief Returns the day-split instants lying strictly inside (first, last).
    \param first      start of the range, ms since epoch
    \param last       end of the range, ms since epoch
    \param splitTime  local wall-clock time at which an OSCAR day starts
    \return ascending list of instants, ms since epoch; empty if the range crosses none
    */
QList<qint64> boundariesWithin(qint64 first, qint64 last, const QTime & splitTime);

/*! \brief True if a sample of this channel holds its value until the next sample
    (SpO2, pulse, sleep stage, ...), false for flags and spans, which mark instants.
    Decides whether a cut repeats the last value at the cut on both sides. */
bool holdsValue(ChannelID code);

/*! \brief Cuts \a s at \a boundary.

    \a s keeps everything before the boundary and ends at it; the returned session holds
    everything at or after it and starts at it. An EventList that crosses the boundary is
    divided between the two; for channels that hold their value (see holdsValue()) the
    last value before the cut is repeated at the cut on both sides, so neither graph
    starts or ends with a gap. Mask-on slices are clipped the same way.

    Per-recording session settings (sleep-stage totals, ZQ and the like) stay with \a s,
    which keeps the original session ID; only the SpO2 baseline used for desaturation
    detection is copied, as it applies to every part of the recording.

    Cached summaries of \a s are discarded; both sessions are marked changed so that
    Machine::Save() recomputes and stores them.

    \param s         session to cut; must still hold its EventLists
    \param boundary  instant to cut at, ms since epoch, in the session's own (raw) time
    \param newId     session ID for the new session
    \return the new session, or nullptr (and \a s unchanged) if either side would hold
            no samples
    */
Session * splitAt(Session * s, qint64 boundary, SessionID newId);

/*! \brief Discards the cached per-channel summaries of \a s (counts, averages, min/max,
    first/last, ...), so the next UpdateSummaries() computes them from its EventLists. */
void clearSummaryCache(Session * s);

} // namespace SessionSplit

#endif // SESSION_SPLIT_H
