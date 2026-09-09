/* gSleepStageChart Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#define TEST_MACROS_ENABLEDoff
#include <test_macros.h>

#include <algorithm>

#include <QDateTime>
#include <QPainter>
#include <QPen>

#include "gSleepStageChart.h"
#include "gGraph.h"
#include "gGraphView.h"
#include "SleepLib/profiles.h"

namespace {

// Zeo/Dreem emit one event per 30-second sample, so identical consecutive stages
// must coalesce into a single bar; Apple Health emits one 2-event list per
// interval, so lists must be sorted together first. The tolerance covers seams.
const qint64 MergeTolerance = 1000;
const qint64 ConnectorGapThreshold = 5 * 60 * 1000;

QString laneName(int lane)
{
    static const QString names[] = {
        QObject::tr("Awake"),
        QObject::tr("REM"),
        QObject::tr("Light"),  // Stage_Light; Apple Health calls this "Core"
        QObject::tr("Deep")
    };

    if (lane < 0 || lane > 3) {
        return QString();
    }
    return names[lane];
}

QString formatDuration(qint64 durationMs)
{
    const qint64 totalSeconds = qMax<qint64>(0, durationMs / 1000);
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;

    return QString("%1:%2:%3")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
}

} // namespace

gSleepStageChart::gSleepStageChart(ChannelID code)
    : Layer(code)
{
    setMinimumHeight(76);
}

void gSleepStageChart::SetDay(Day *d)
{
    Layer::SetDay(d);
    m_intervals.clear();

    if (!m_day) {
        return;
    }

    QVector<Interval> intervals;

    for (const auto &sess : m_day->sessions) {
        if (!sess || !sess->enabled()) {
            continue;
        }

        // Answered from summary counts, so sessions without stage data
        // are skipped before their (possibly large) event data is loaded.
        if (!sess->channelExists(m_code)) {
            continue;
        }

        if (!sess->eventsLoaded()) {
            sess->OpenEvents();
        }

        auto eventLists = sess->eventlist.find(m_code);
        if (eventLists == sess->eventlist.end()) {
            continue;
        }

        const qint64 drift = sess->correctionMs();

        for (const auto &el : eventLists.value()) {
            if (!el) {
                continue;
            }

            const quint32 count = el->count();
            if (count == 0) {
                continue;
            }

            for (quint32 i = 0; i < count; ++i) {
                // SLEEP_Stage is stored negated (see SleepStageValue); lane = stage - 1.
                // qRound folds Zeo's fractional 3.75 "Deep (2)" sample into the Deep lane.
                const int lane = qRound(-el->data(i)) - 1;
                if (lane < 0 || lane >= LaneCount) {
                    continue;
                }

                // An interval runs to the next event; the run's closing duplicate from
                // EndEventList lands on its own timestamp and drops out below (end <= start).
                // A lone event has no successor, so fall back to the session end.
                // Note sess->last() already includes correctionMs(); el->time() does not.
                const qint64 start = el->time(i) + drift;
                qint64 end = (i + 1 < count ? el->time(i + 1) : el->last()) + drift;
                if (count == 1) {
                    end = qMax(end, sess->last());
                }

                if (end <= start) {
                    continue;
                }

                intervals.append({start, end, lane});
            }
        }
    }

    std::sort(intervals.begin(), intervals.end(), [](const Interval &a, const Interval &b) {
        if (a.start != b.start) {
            return a.start < b.start;
        }
        return a.end < b.end;
    });

    for (const auto &interval : intervals) {
        if (!m_intervals.isEmpty()) {
            Interval &current = m_intervals.last();
            if (interval.lane == current.lane && interval.start <= current.end + MergeTolerance) {
                current.end = qMax(current.end, interval.end);
                continue;
            }
        }

        m_intervals.append(interval);
    }
}

void gSleepStageChart::paint(QPainter &painter, gGraph &w, const QRegion &region)
{
    if (!m_visible || !m_day) {
        return;
    }

    const QRect rect = region.boundingRect();
    const int left = rect.left();
    const int top = rect.top();
    const int width = rect.width();
    const int height = rect.height();
    if (width <= 0 || height <= 0) {
        return;
    }

    double minx;
    double maxx;
    if (w.blockZoom()) {
        minx = w.rmin_x;
        maxx = w.rmax_x;
    } else {
        minx = w.min_x;
        maxx = w.max_x;
    }

    const double xx = maxx - minx;
    if (xx <= 0) {
        return;
    }

    const double xmult = width / xx;
    const double laneHeight = height / double(LaneCount);
    const double barHeight = qMin(laneHeight * 0.55, 14.0 * w.printScaleY());
    auto laneY = [top, laneHeight, barHeight](int lane) {
        return top + laneHeight * lane + (laneHeight - barHeight) / 2.0;
    };
    auto laneMid = [&laneY, barHeight](int lane) {
        return laneY(lane) + barHeight / 2.0;
    };

    // The graph view shares one painter across all graphs; restore this before returning.
    const bool hadAntialiasing = painter.testRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::Antialiasing, true);

    for (int lane = 0; lane < LaneCount; ++lane) {
        const QString name = laneName(lane);
        int textWidth;
        int textHeight;
        GetTextExtent(name, textWidth, textHeight);
        w.renderText(name, left - textWidth - 8,
                     laneMid(lane) + textHeight / 2.0 - 1);
    }

    painter.setClipRect(left, top, width, height);
    painter.setClipping(true);

    painter.setPen(QPen(QColor(0, 0, 0, 60), 1));
    for (int i = 1; i < m_intervals.size(); ++i) {
        const Interval &previous = m_intervals.at(i - 1);
        const Interval &current = m_intervals.at(i);
        // A negative gap means the intervals overlap — Apple Health nests stage
        // intervals within a session, and a day can hold several devices' sessions.
        // A transition line there would be spurious.
        const qint64 gap = current.start - previous.end;
        if (previous.lane == current.lane || gap < 0 || gap > ConnectorGapThreshold) {
            continue;
        }

        const double x = (current.start - minx) * xmult + left;
        if (x < left || x > left + width) {
            continue;
        }

        painter.drawLine(QPointF(x, laneMid(previous.lane)),
                         QPointF(x, laneMid(current.lane)));
    }

    const bool monochrome = w.printing() && AppSetting->monochromePrinting();
    const QPoint mouse = w.graphView()->currentMousePos();
    const int tooltipTimeout = AppSetting->tooltipTimeout();
    bool hover = false;

    for (const auto &interval : m_intervals) {
        if (interval.end < minx || interval.start > maxx) {
            continue;
        }

        const double x1 = (interval.start - minx) * xmult + left;
        const double x2 = (interval.end - minx) * xmult + left;
        const double barWidth = qMax(2.0, x2 - x1);
        const QRectF barRect(x1, laneY(interval.lane), barWidth, barHeight);
        const double radius = qMin(barHeight * 0.5, barWidth * 0.5);

        painter.setPen(Qt::NoPen);
        painter.setBrush(monochrome ? Qt::black : LaneColors[interval.lane]);
        painter.drawRoundedRect(barRect, radius, radius, Qt::AbsoluteSize);

        const double hoverSlop = barWidth < 6.0 ? 3.0 : 0.0;
        // Reports render into a pixmap with unrelated geometry, so the live mouse
        // position must not bake a highlight (or pop a tooltip) into the print.
        if (!w.printing() && !w.selectingArea() && !hover &&
                barRect.adjusted(-hoverSlop, -2, hoverSlop, 2).contains(mouse)) {
            hover = true;
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(Qt::red, 1));
            painter.drawRoundedRect(barRect, radius, radius, Qt::AbsoluteSize);

            const QString start = QDateTime::fromMSecsSinceEpoch(interval.start)
                    .time().toString("HH:mm:ss");
            const QString end = QDateTime::fromMSecsSinceEpoch(interval.end)
                    .time().toString("HH:mm:ss");
            const QString label = QString("%1\n%2 \u2013 %3\n%4")
                    .arg(laneName(interval.lane))
                    .arg(start)
                    .arg(end)
                    .arg(QObject::tr("Duration: %1").arg(formatDuration(interval.end - interval.start)));
            w.ToolTip(label, x1 - 10, laneY(interval.lane) + 3 * w.printScaleY(),
                      TT_AlignRight, tooltipTimeout);
        }
    }

    painter.setClipping(false);
    painter.setRenderHint(QPainter::Antialiasing, hadAntialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(COLOR_Outline);
    painter.drawRect(rect);

    if (AppSetting->lineCursorMode()) {
        const double time = w.currentTime();
        if (time > minx && time < maxx) {
            const double xpos = (time - minx) * xmult;
            painter.setPen(QPen(QBrush(QColor(0, 255, 0, 255)), 1));
            painter.drawLine(left + xpos, top - w.marginTop() - 3,
                             left + xpos, top + height + w.bottom - 1);
        }
    }
}
