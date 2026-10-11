/* SleepLib Session Split Implementation
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

#include <QDateTime>
#include <QTimeZone>

#include "SleepLib/session_split.h"
#include "SleepLib/session.h"
#include "SleepLib/event.h"
#include "SleepLib/schema.h"

namespace {

//! \brief A new, empty EventList with the same type, gain, rate and units as \a src.
//! A list built with fixed limits keeps them; otherwise its limits follow its own data.
EventList * emptyLike(EventList * src)
{
    const bool fixedLimits = !src->update_minmax();
    EventList * el = new EventList(src->type(), src->gain(), src->offset(),
                                   fixedLimits ? src->Min() : 0, fixedLimits ? src->Max() : 0,
                                   src->rate(), src->hasSecondField());
    el->setDimension(src->dimension());
    return el;
}

//! \brief Appends sample \a i of \a src to the event list \a dst at time \a t.
void appendSample(EventList * dst, EventList * src, quint32 i, qint64 t)
{
    if (dst->hasSecondField()) {
        const EventStoreType second = (quint32(src->getData2().size()) > i) ? src->raw2(i) : 0;
        dst->AddEvent(t, src->raw(i), second);
    } else {
        dst->AddEvent(t, src->raw(i));
    }
}

//! \brief Copies samples [from, to) of the waveform \a src into a new waveform list.
//! Sample times follow from the first one and the rate, so only the start moves.
EventList * waveformSlice(EventList * src, quint32 from, quint32 to)
{
    EventList * el = emptyLike(src);
    const qint64 first = src->time(from);
    const quint32 n = to - from;

    el->setFirst(first);
    el->getData() = src->getData().mid(from, n);
    if (quint32(src->getData2().size()) >= to) {
        el->getData2() = src->getData2().mid(from, n);
    }
    // Lists filled with AddEvent() (Viatom) also carry per-sample offsets from first().
    if (quint32(src->getTime().size()) >= to) {
        QVector<quint32> & times = el->getTime();
        times.resize(n);
        for (quint32 i = 0; i < n; ++i) {
            times[i] = quint32(qint64(src->getTime()[from + i]) + src->first() - first);
        }
    }
    el->setCount(n);
    el->setLast(src->time(to - 1));

    if (el->update_minmax()) {
        EventDataType lo = el->data(0), hi = lo;
        for (quint32 i = 1; i < n; ++i) {
            const EventDataType v = el->data(i);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        el->setMin(lo);
        el->setMax(hi);
    }
    return el;
}

//! \brief Divides \a src, which has samples on both sides of \a boundary, into
//! \a head (before it) and \a tail (at or after it). \a src itself is left untouched.
void cutEventList(EventList * src, qint64 boundary, bool carry, EventList *& head, EventList *& tail)
{
    const quint32 n = src->count();
    quint32 k = 0;
    while (k < n && src->time(k) < boundary) ++k;   // first sample at or after the cut

    if (src->type() == EVL_Waveform) {
        head = waveformSlice(src, 0, k);
        tail = waveformSlice(src, k, n);
        // The last sample before the cut covers the time up to it.
        head->setLast(boundary);
        tail->setLast(src->last());
        return;
    }

    head = emptyLike(src);
    for (quint32 i = 0; i < k; ++i) appendSample(head, src, i, src->time(i));
    if (carry && src->time(k - 1) < boundary) {
        appendSample(head, src, k - 1, boundary);    // hold the last value up to the cut...
    }

    tail = emptyLike(src);
    if (carry && src->time(k) > boundary) {
        appendSample(tail, src, k - 1, boundary);    // ...and from the cut to the next sample
    }
    for (quint32 i = k; i < n; ++i) appendSample(tail, src, i, src->time(i));
}

} // namespace

QList<qint64> SessionSplit::boundariesWithin(qint64 first, qint64 last, const QTime & splitTime)
{
    QList<qint64> result;
    if (last <= first || !splitTime.isValid()) return result;

    const QTimeZone local(QTimeZone::LocalTime);
    QDate date = QDateTime::fromMSecsSinceEpoch(first, local).date();

    // A recording longer than a few days is unusual; the cap only guards against a
    // corrupt end time producing an endless list.
    for (int i = 0; i < 400; ++i, date = date.addDays(1)) {
        const qint64 b = QDateTime(date, splitTime, local).toMSecsSinceEpoch();
        if (b <= first) continue;
        if (b >= last) break;
        result.append(b);
    }
    return result;
}

bool SessionSplit::holdsValue(ChannelID code)
{
    const int type = schema::channel[code].type();
    return (type & (schema::FLAG | schema::MINOR_FLAG | schema::SPAN)) == 0;
}

Session * SessionSplit::splitAt(Session * s, qint64 boundary, SessionID newId)
{
    if (!s || boundary <= s->realFirst() || boundary >= s->realLast()) return nullptr;

    // Both sides must hold a sample, or the cut would leave an empty session.
    bool before = false, after = false;
    for (auto it = s->eventlist.cbegin(); it != s->eventlist.cend(); ++it) {
        for (EventList * el : it.value()) {
            if (!el || el->count() == 0) continue;
            if (el->time(0) < boundary) before = true;
            if (el->time(el->count() - 1) >= boundary) after = true;
        }
    }
    if (!before || !after) return nullptr;

    Session * tail = new Session(s->machine(), newId);

    for (auto it = s->eventlist.begin(); it != s->eventlist.end(); ) {
        const bool carry = holdsValue(it.key());
        QVector<EventList *> headLists, tailLists;

        for (EventList * el : it.value()) {
            if (!el || el->count() == 0) {
                headLists.append(el);
                continue;
            }
            if (el->time(el->count() - 1) < boundary) {
                headLists.append(el);
            } else if (el->time(0) >= boundary) {
                tailLists.append(el);
            } else {
                EventList * h = nullptr, * t = nullptr;
                cutEventList(el, boundary, carry, h, t);
                headLists.append(h);
                tailLists.append(t);
                delete el;
            }
        }

        if (!tailLists.isEmpty()) tail->eventlist[it.key()] = tailLists;
        if (headLists.isEmpty()) {
            it = s->eventlist.erase(it);
        } else {
            it.value() = headLists;
            ++it;
        }
    }

    // Mask-on periods (Apple Health sleep runs) are clipped at the cut.
    QVector<SessionSlice> headSlices;
    for (const SessionSlice & slice : s->m_slices) {
        if (slice.end <= boundary) {
            headSlices.append(slice);
        } else if (slice.start >= boundary) {
            tail->m_slices.append(slice);
        } else {
            headSlices.append(SessionSlice(slice.start, boundary, slice.status));
            tail->m_slices.append(SessionSlice(boundary, slice.end, slice.status));
        }
    }
    s->m_slices = headSlices;

    // calcSPO2Drop() parks the baseline it used here; it applies to the whole recording.
    auto baseline = s->settings.constFind(OXI_SPO2Drop);
    if (baseline != s->settings.constEnd()) {
        tail->settings[OXI_SPO2Drop] = baseline.value();
    }

    // Display and threshold properties belong to the channels, not to a time range.
    tail->m_gain = s->m_gain;
    tail->m_physmin = s->m_physmin;
    tail->m_physmax = s->m_physmax;
    tail->m_lowerThreshold = s->m_lowerThreshold;
    tail->m_upperThreshold = s->m_upperThreshold;

    tail->really_set_first(boundary);
    tail->really_set_last(s->realLast());
    s->really_set_last(boundary);

    clearSummaryCache(s);
    s->SetChanged(true);
    tail->SetChanged(true);

    return tail;
}

void SessionSplit::clearSummaryCache(Session * s)
{
    if (!s) return;
    s->m_cnt.clear();
    s->m_sum.clear();
    s->m_avg.clear();
    s->m_wavg.clear();
    s->m_min.clear();
    s->m_max.clear();
    s->m_cph.clear();
    s->m_sph.clear();
    s->m_firstchan.clear();
    s->m_lastchan.clear();
    s->m_valuesummary.clear();
    s->m_timesummary.clear();
    s->m_timeBelowTheshold.clear();
    s->m_timeAboveTheshold.clear();
}
