/* gSleepStageChart Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef GSLEEPSTAGECHART_H
#define GSLEEPSTAGECHART_H

#include <QColor>
#include <QVector>

#include "Graphs/layer.h"

/*! \class gSleepStageChart
    \brief Draws sleep stages as fixed horizontal lanes with interval bars.
    */
class gSleepStageChart : public Layer
{
  public:
    gSleepStageChart(ChannelID code = SLEEP_Stage);
    virtual ~gSleepStageChart() {}

    //! \brief Builds a sorted, run-length-merged interval list for the selected day.
    virtual void SetDay(Day *d);

    //! \brief Draws stage bars, transition connectors, labels, and hover tooltips.
    virtual void paint(QPainter &painter, gGraph &w, const QRegion &region);

    virtual Layer * Clone() {
        gSleepStageChart *layer = new gSleepStageChart(NoChannel);
        Layer::CloneInto(layer);
        CloneInto(layer);
        return layer;
    }

    void CloneInto(gSleepStageChart *layer) {
        layer->m_intervals = m_intervals;
    }

  protected:
    struct Interval {
        qint64 start;
        qint64 end;
        int lane;
    };

    static constexpr int LaneCount = 4;  // Stage_Awake .. Stage_Deep
    // Awake, REM, Light, Deep — index = SleepStageValue - 1, same order as laneName().
    inline static const QColor LaneColors[LaneCount] = {
        QColor("#FF7D6B"),
        QColor("#65C5F4"),
        QColor("#3A7DFF"),
        QColor("#4F2A93")
    };

    QVector<Interval> m_intervals;
};

#endif // GSLEEPSTAGECHART_H
