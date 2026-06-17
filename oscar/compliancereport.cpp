/* Compliance Report Exporter Source
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "compliancereport.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QPrinter>
#include <QTextDocument>
#include <QGuiApplication>
#include <QStyleHints>
#include <QDebug>
#include <QLocale>
#include <cmath>
#include <algorithm>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QBuffer>
#include <QSet>
#include <QSvgGenerator>
#include <QSvgRenderer>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextCursor>
#include "mainwindow.h"
#include "version.h"
#include "SleepLib/profiles.h"
#include "SleepLib/common.h"
#include "SleepLib/day.h"
#include "SleepLib/session.h"
#include "SleepLib/event.h"
#include "SleepLib/schema.h"

extern MainWindow *mainwin;
extern ChannelID RMS9_EPR;
extern ChannelID RMS9_EPRLevel;
extern ChannelID RMAS1x_Comfort;

ComplianceReportDialog::ComplianceReportDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Export Compliance Report (PDF)"));
    setMinimumWidth(380);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Range selector
    QHBoxLayout *rangeLayout = new QHBoxLayout();
    QLabel *rangeLabel = new QLabel(tr("Date Range:"), this);
    m_rangeCombo = new QComboBox(this);
    m_rangeCombo->addItem(tr("Last 30 Days"), 30);
    m_rangeCombo->addItem(tr("Last 60 Days"), 60);
    m_rangeCombo->addItem(tr("Last 90 Days"), 90);
    m_rangeCombo->addItem(tr("Since Last Settings Change"), -1);
    m_rangeCombo->addItem(tr("Custom Range"), 0);
    rangeLayout->addWidget(rangeLabel);
    rangeLayout->addWidget(m_rangeCombo);
    mainLayout->addLayout(rangeLayout);

    // Custom Date Editors
    QDate defaultEnd = (p_profile && p_profile->LastDay(MT_CPAP).isValid())
        ? p_profile->LastDay(MT_CPAP) : QDate::currentDate().addDays(-1);
    QHBoxLayout *dateLayout = new QHBoxLayout();
    m_startLabel = new QLabel(tr("Start:"), this);
    m_startDateEdit = new QDateEdit(defaultEnd.addDays(-29), this);
    m_startDateEdit->setCalendarPopup(true);
    m_endLabel = new QLabel(tr("End:"), this);
    m_endDateEdit = new QDateEdit(defaultEnd, this);
    m_endDateEdit->setCalendarPopup(true);

    dateLayout->addWidget(m_startLabel);
    dateLayout->addWidget(m_startDateEdit);
    dateLayout->addWidget(m_endLabel);
    dateLayout->addWidget(m_endDateEdit);
    mainLayout->addLayout(dateLayout);

    // Channels Checklist Section
    QLabel *channelsLabel = new QLabel(tr("Select Channels to Include:"), this);
    mainLayout->addWidget(channelsLabel);

    m_channelsListWidget = new QListWidget(this);
    m_channelsListWidget->setMinimumHeight(180);
    mainLayout->addWidget(m_channelsListWidget);

    // Define all selectable channels in OSCAR
    ChannelID leakChan = NoChannel;
    if (p_profile) {
        leakChan = p_profile->hasChannel(CPAP_Leak) ? CPAP_Leak : (p_profile->hasChannel(CPAP_LeakTotal) ? CPAP_LeakTotal : NoChannel);
    }

    struct TempChannelInfo {
        ChannelID id;
        QString label;
        int type; // MachineType
    };

    QList<TempChannelInfo> candidateChannels = {
        {CPAP_Pressure,   tr("Pressure"),    MT_CPAP},
    };
    if (p_profile && p_profile->hasChannel(CPAP_EPAP))
        candidateChannels.append({CPAP_EPAP, tr("EPAP"), MT_CPAP});  // shown for CPAP+EPR and BiPAP/ASV
    candidateChannels.append({CPAP_MinuteVent, tr("Minute Vent."), MT_CPAP});
    candidateChannels.append({CPAP_RespRate,   tr("Respiratory Rate"),  MT_CPAP});
    candidateChannels.append({CPAP_FlowLimit,  tr("Flow Limit."), MT_CPAP});
    if (leakChan != NoChannel)
        candidateChannels.append({leakChan, tr("Leak Rate"), MT_CPAP});
    candidateChannels.append({CPAP_Snore,       tr("Snore"),        MT_CPAP});
    candidateChannels.append({CPAP_Ti,          tr("Insp. Time"),   MT_CPAP});
    candidateChannels.append({CPAP_Te,          tr("Exp. Time"),    MT_CPAP});
    candidateChannels.append({CPAP_TidalVolume, tr("Tidal Volume"), MT_CPAP});
    candidateChannels.append({OXI_Pulse,        tr("Pulse Rate"),   MT_OXIMETER});
    candidateChannels.append({OXI_SPO2,         tr("SpO2"),         MT_OXIMETER});

    if (p_profile) {
        for (const auto& ch : candidateChannels) {
            if (p_profile->hasChannel(ch.id)) {
                QListWidgetItem *item = new QListWidgetItem(ch.label, m_channelsListWidget);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Checked); // Checked by default
                item->setData(Qt::UserRole, ch.id);
                item->setData(Qt::UserRole + 1, ch.type);
            }
        }
    }

    // Dialog Buttons
    QDialogButtonBox *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_rangeCombo, &QComboBox::currentIndexChanged, this, &ComplianceReportDialog::onRangeChanged);

    // Set initial custom date fields visibility
    onRangeChanged(0);
}

QDate ComplianceReportDialog::startDate() const
{
    int days = m_rangeCombo->currentData().toInt();
    if (days > 0) {
        return endDate().addDays(-days + 1);
    } else if (days == -1) {
        return lastSettingsChangeStartDate();
    }
    return m_startDateEdit->date();
}

QDate ComplianceReportDialog::endDate() const
{
    int days = m_rangeCombo->currentData().toInt();
    if (days > 0) {
        if (p_profile) {
            QDate lastDay = p_profile->LastDay(MT_CPAP);
            if (lastDay.isValid()) {
                return lastDay;
            }
        }
        return QDate::currentDate().addDays(-1);
    } else if (days == -1) {
        if (p_profile) {
            QDate lastDay = p_profile->LastDay(MT_CPAP);
            if (lastDay.isValid()) {
                return lastDay;
            }
        }
        return QDate::currentDate().addDays(-1);
    }
    return m_endDateEdit->date();
}

QDate ComplianceReportDialog::lastSettingsChangeStartDate() const
{
    if (!p_profile) {
        return QDate::currentDate().addDays(-30);
    }
    QDate lastDayDate = p_profile->LastDay(MT_CPAP);
    if (!lastDayDate.isValid()) {
        return QDate::currentDate().addDays(-30);
    }
    Day* lastDay = p_profile->GetGoodDay(lastDayDate, MT_CPAP);
    if (!lastDay) {
        return lastDayDate;
    }
    QString targetMode = lastDay->getCPAPModeStr();
    QString targetPressure = lastDay->getPressureSettings();
    QString targetRelief = lastDay->getPressureRelief();
    Machine* targetMachine = lastDay->machine(MT_CPAP);

    QDate curDate = lastDayDate.addDays(-1);
    QDate firstDayDate = p_profile->FirstDay(MT_CPAP);
    QDate matchStart = lastDayDate;

    while (curDate >= firstDayDate) {
        Day* curDay = p_profile->GetGoodDay(curDate, MT_CPAP);
        if (curDay) {
            QString mode = curDay->getCPAPModeStr();
            QString pressure = curDay->getPressureSettings();
            QString relief = curDay->getPressureRelief();
            Machine* mach = curDay->machine(MT_CPAP);

            if (mode == targetMode && pressure == targetPressure && relief == targetRelief && mach == targetMachine) {
                matchStart = curDate;
            } else {
                break;
            }
        }
        curDate = curDate.addDays(-1);
    }
    return matchStart;
}

QList<SelectedChannel> ComplianceReportDialog::selectedChannels() const
{
    QList<SelectedChannel> selected;
    for (int i = 0; i < m_channelsListWidget->count(); i++) {
        QListWidgetItem *item = m_channelsListWidget->item(i);
        if (item->checkState() == Qt::Checked) {
            SelectedChannel sc;
            sc.id = item->data(Qt::UserRole).toUInt();
            sc.label = item->text();
            sc.type = item->data(Qt::UserRole + 1).toInt();
            selected.append(sc);
        }
    }
    return selected;
}

void ComplianceReportDialog::onRangeChanged(int index)
{
    int days = m_rangeCombo->itemData(index).toInt();
    bool isCustom = (days == 0);
    
    m_startDateEdit->setEnabled(isCustom);
    m_endDateEdit->setEnabled(isCustom);
    
    if (!isCustom) {
        m_startDateEdit->setDate(startDate());
        m_endDateEdit->setDate(endDate());
    }
}

namespace {

QString imageToBase64DataUrl(const QImage& img)
{
    QByteArray byteArray;
    QBuffer buffer(&byteArray);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return QString("data:image/png;base64,") + byteArray.toBase64();
}

static QString getSettingStr(Day* day, ChannelID cid) {
    if (!day || !day->settingExists(cid)) return QString();
    double val = day->settings_wavg(cid);
    schema::Channel & chan = schema::channel[cid];
    if (chan.datatype() == schema::LOOKUP) {
        QString opt = chan.option(qRound(val));
        if (!opt.isEmpty()) return opt;
    }
    QString unit = chan.units();
    if (unit.isEmpty()) return QString::number(val, 'f', 1);
    return QString::number(val, 'f', 1) + " " + unit;
}

// Clinically-safe minimum that discards sensor dropout / artifact samples.
//
// p_profile->calcMin() returns Day::Min(), the lowest raw sample of the day,
// which includes the physiologically impossible zeros an oximeter emits when the
// probe loses contact (SpO2 -> 0%, pulse -> 0 bpm). Once a dropout zeroes the
// day summary it cannot be recovered, so we re-walk the raw event samples and
// ignore any value at or below floorVal before taking the minimum.
//
// Floors are set low enough to reject probe-loss (which reads ~0%) without
// clipping genuine pathology: severe OSA / obesity-hypoventilation desaturations
// can briefly reach the low 40s%, and a 50% floor would silently discard them and
// over-report the period low as safer than it was — the worst direction to err for
// the single most clinically scrutinised number on the report. Pulse oximeters are
// unreliable below ~70% anyway, so 40% is a conservative artifact boundary.
static const double SPO2_ARTIFACT_FLOOR  = 40.0;  // % (probe-loss reads ~0)
static const double PULSE_ARTIFACT_FLOOR = 30.0;  // bpm

// Single-pass collector for the dropout-filtered oximetry statistics. The four
// quantities the report needs — min, max, time-weighted mean, and seconds spent
// below a threshold — all derive from one walk over the same raw event samples,
// so computing them together avoids re-opening and re-walking the same sessions
// once per statistic (the oximetry page alone used to make six full passes over
// the same SpO2/Pulse data). Every value at or below floorVal is treated as
// sensor-off dropout and excluded from min/max/wavg.
//
//  - min / max:      lowest / highest sample above floorVal. `found` is false
//                    when no non-dropout sample exists (min/max then read 0.0,
//                    matching the old helpers).
//  - wavg:           each sample weighted by the interval to the next sample,
//                    mirroring Profile::calcWavg for evenly-sampled oximetry; the
//                    trailing sample of each EventList carries no interval and is
//                    omitted, as before.
//  - timeBelowSecs:  gap-integrated time the signal sits in (floorVal, threshold],
//                    mirroring Session::timeBelowThreshold but discarding the
//                    probe-loss 0% spans that would otherwise inflate T90.
struct FilteredStats {
    bool   found = false;
    double min = 0.0;
    double max = 0.0;
    double wavg = 0.0;
    double timeBelowSecs = 0.0;
};

// belowThreshold is consulted only for timeBelowSecs; callers that don't need a
// time-below figure can pass any value at or below floorVal (the in-range test
// never fires, so timeBelowSecs stays 0) and ignore the field.
static FilteredStats calcFilteredStats(ChannelID code, MachineType mt,
                                       const QDate& start, const QDate& end,
                                       double floorVal, double belowThreshold)
{
    FilteredStats r;
    if (!p_profile) return r;

    double weightedSum = 0.0, totalTime = 0.0, totalMs = 0.0;

    // Use MT_UNKNOWN so we iterate the same day objects that OSCAR's native
    // oximetry view does. Oximeter sessions may be stored on days whose machine
    // type is MT_UNKNOWN, so getDays(MT_OXIMETER) can miss them.
    QList<Day*> days = p_profile->getDays(MT_UNKNOWN, start, end);
    for (Day* day : days) {
        if (!day) continue;
        for (Session* sess : day->getSessions(mt)) {
            bool alreadyLoaded = sess->eventsLoaded();
            if (!alreadyLoaded) sess->OpenEvents();
            auto EVEC = sess->eventlist.find(code);
            if (EVEC == sess->eventlist.end()) {
                if (!alreadyLoaded) sess->TrashEvents();
                continue;
            }
            for (EventList* el : EVEC.value()) {
                const quint32 cnt = el->count();
                qint64 started = 0, ti = 0;
                for (quint32 i = 0; i < cnt; ++i) {
                    ti = el->time(i);
                    double v = el->data(i);

                    // Time-below gap integration runs over every sample so a
                    // dropout read still closes an open below-threshold span.
                    bool inRange = (v > floorVal && v <= belowThreshold);
                    if (started == 0) {
                        if (inRange) started = ti;
                    } else if (!inRange) {
                        totalMs += ti - started;
                        started = 0;
                    }

                    if (v <= floorVal) continue;   // dropout: out of min/max/wavg

                    if (!r.found) { r.min = r.max = v; r.found = true; }
                    else { if (v < r.min) r.min = v; if (v > r.max) r.max = v; }

                    if (i + 1 < cnt) {
                        double dt = double(el->time(i + 1) - ti);
                        if (dt > 0.0) { weightedSum += v * dt; totalTime += dt; }
                    }
                }
                if (started) totalMs += ti - started;
            }
            if (!alreadyLoaded) sess->TrashEvents();
        }
    }

    r.wavg = totalTime > 0.0 ? weightedSum / totalTime : 0.0;
    r.timeBelowSecs = totalMs / 1000.0;
    return r;
}

struct SettingsChangeBlock {
    QDate start;
    QDate end;
    int days = 0;
    double hours = 0.0;
    double ahiEvents = 0.0;
    QString deviceStr;
    QString relief;
    QString mode;
    QString pressure;
    double minSpo2 = -1.0;  // < 0 means no oximetry data for this block
};

struct StatsRow {
    QString label;
    QString value;
    QString color;      // empty = default (label grey #64748b, value dark #0f172a)
    bool bold = false;  // true = label is also dark #0f172a and bold
    bool boldValue = false; // true = value is bold
};

QList<SettingsChangeBlock> getSettingsChanges(const QDate& start, const QDate& end, const QList<Day*>& daysList) {
    QList<SettingsChangeBlock> blocks;
    if (daysList.isEmpty()) return blocks;

    SettingsChangeBlock curBlock;
    bool hasBlock = false;

    for (Day* day : daysList) {
        if (!day) continue;
        Machine* mach = day->machine(MT_CPAP);
        if (!mach) continue;

        QString relief = day->getPressureRelief();
        QString mode = day->getCPAPModeStr();
        QString pressure = day->getPressureSettings();
        QString deviceStr = mach->brand() + " " + mach->model() + " (" + mach->serial() + ")";

        if (!hasBlock) {
            curBlock.start = day->date();
            curBlock.end = day->date();
            curBlock.days = 1;
            curBlock.hours = day->hours(MT_CPAP);
            curBlock.ahiEvents = day->count(CPAP_ClearAirway) + day->count(CPAP_Obstructive) + day->count(CPAP_Apnea) + day->count(CPAP_Hypopnea);
            curBlock.deviceStr = deviceStr;
            curBlock.relief = relief;
            curBlock.mode = mode;
            curBlock.pressure = pressure;
            hasBlock = true;
        } else {
            if (curBlock.relief == relief && curBlock.mode == mode && curBlock.pressure == pressure && curBlock.deviceStr == deviceStr) {
                curBlock.end = day->date();
                curBlock.days++;
                curBlock.hours += day->hours(MT_CPAP);
                curBlock.ahiEvents += day->count(CPAP_ClearAirway) + day->count(CPAP_Obstructive) + day->count(CPAP_Apnea) + day->count(CPAP_Hypopnea);
            } else {
                blocks.append(curBlock);
                curBlock.start = day->date();
                curBlock.end = day->date();
                curBlock.days = 1;
                curBlock.hours = day->hours(MT_CPAP);
                curBlock.ahiEvents = day->count(CPAP_ClearAirway) + day->count(CPAP_Obstructive) + day->count(CPAP_Apnea) + day->count(CPAP_Hypopnea);
                curBlock.deviceStr = deviceStr;
                curBlock.relief = relief;
                curBlock.mode = mode;
                curBlock.pressure = pressure;
            }
        }
    }
    if (hasBlock) {
        blocks.append(curBlock);
    }
    return blocks;
}

// ---- Shared QPainter chart helpers ----

const int CHART_MAX_W = 1500;  // SVG canvas width — fixed; charts always span the full page width
const int CHART_TITLE_H = 50; // pixels reserved at top of canvas for the title
const int CHART_H = 360 + CHART_TITLE_H; // SVG canvas height (410px total)
static const int CHART_DISPLAY_MIN_W = 200;
const int CHART_LM = 86;   // left margin: y-axis labels
const int CHART_RM = 150;  // right margin: last x label half-width (60) + gap,
                           // also fits threshold annotations ("AHI 5", redline)
// Cap on bar width. Short ranges have wide columns; without a cap their bars
// would render as a few very fat bars. Capping spreads the bars across the
// full-width plot area instead (AirView style). Long ranges fall below this
// naturally and are unaffected.
const double CHART_BAR_MAX_W = 70.0;

// Canvas always spans the full page width (no day-count shrink) so the chart
// never leaves trailing whitespace on short ranges — matching AirView, which
// widens the plot area to fill the page and spreads the bars out. Bar width is
// capped (CHART_BAR_MAX_W) so short ranges spread rather than fatten.
static int chartCanvasWidth(int /*totalDays*/)
{
    return CHART_MAX_W;
}

// Bar width for a given column: fills 75% of the column but never exceeds
// CHART_BAR_MAX_W, so short (wide-column) ranges spread their bars out instead
// of stretching them fat.
static double chartBarWidth(double colWidth)
{
    return qMin(qMax(colWidth * 0.75, 1.5), CHART_BAR_MAX_W);
}

static int labelIntervalForDays(int totalDays)
{
    if (totalDays > 90) return 7;
    if (totalDays > 60) return 5;
    if (totalDays > 30) return 3;
    if (totalDays > 10) return 2;
    return 1;
}

static void drawYGridAndLabels(QPainter& painter, const QRect& chartRect, double yMax, int gridCount,
                               double yMin = 0.0)
{
    painter.setFont(QFont("SF Pro Display", 30));
    double range = yMax - yMin;
    const int majorTick = 7;   // length of the labelled tick dash at the axis
    const int minorTick = 5;   // length of the unlabelled tick between labels
    const int labelGap  = 3;   // gap between the number and its tick dash
    // label area = chartRect.left() - majorTick - labelGap; widest label ("1000",
    // ~71px) must fit — keep these small enough that it never clips.
    for (int i = 0; i <= gridCount; ++i) {
        double val = yMin + (range / gridCount) * i;
        int y = chartRect.bottom() - static_cast<int>(((val - yMin) / range) * chartRect.height());
        if (i > 0 && i < gridCount) {
            painter.setPen(QPen(QColor("#e2e8f0"), 1, Qt::DotLine));
            painter.drawLine(chartRect.left(), y, chartRect.right(), y);
        }
        // Minor tick at the midpoint above this label (AirView-style axis detail).
        // Skipped above the top label so it never overruns the plot.
        if (i < gridCount) {
            int my = chartRect.bottom() - static_cast<int>(((val + range / gridCount / 2.0 - yMin) / range) * chartRect.height());
            painter.setPen(QPen(QColor("#64748b"), 1.8));
            painter.drawLine(chartRect.left() - minorTick, my, chartRect.left(), my);
        }
        // Labelled tick dash — the "32 -" mark sitting between the number and the axis.
        painter.setPen(QPen(QColor("#334155"), 2.4));
        painter.drawLine(chartRect.left() - majorTick, y, chartRect.left(), y);
        painter.setPen(QColor("#475569"));
        QString label = (val >= 10.0) ? QString::number(val, 'f', 0)
                                      : QString::number(val, 'f', 1);
        painter.drawText(QRect(0, y - 18, chartRect.left() - majorTick - labelGap, 38),
                         Qt::AlignRight | Qt::AlignVCenter, label);
    }
}

static void drawXGridLines(QPainter& painter, const QRect& chartRect, int totalDays, double colWidth, const QDate& start)
{
    for (int i = 0; i < totalDays; ++i) {
        QDate curDate = start.addDays(i);
        int cx = chartRect.left() + static_cast<int>(i * colWidth + colWidth / 2.0);
        if (curDate.dayOfWeek() == Qt::Sunday) {
            painter.setPen(QPen(QColor("#cbd5e1"), 1)); 
        } else {
            painter.setPen(QPen(QColor("#f1f5f9"), 1)); 
        }
        painter.drawLine(cx, chartRect.top(), cx, chartRect.bottom());
    }
}

static void drawXAxisLabels(QPainter& painter, const QRect& chartRect,
                             int totalDays, double colWidth, const QDate& start,
                             int canvasW)
{
    painter.setPen(QPen(QColor("#94a3b8"), 1.5));
    painter.drawLine(chartRect.bottomLeft(), chartRect.bottomRight());
    int interval = labelIntervalForDays(totalDays);
    painter.setFont(QFont("SF Pro Display", 30));

    // Sunday week indicator labels
    for (int i = 0; i < totalDays; ++i) {
        QDate curDate = start.addDays(i);
        if (curDate.dayOfWeek() == Qt::Sunday) {
            int cx = chartRect.left() + static_cast<int>(i * colWidth + colWidth / 2.0);
            painter.setPen(QColor("#94a3b8"));
            painter.drawText(QRect(cx - 40, chartRect.bottom() + 48, 80, 28),
                             Qt::AlignHCenter | Qt::AlignTop, "Sun");
        }
    }

    // Returns a label rect centred on cx, clamped so it never overflows the SVG canvas.
    auto labelRect = [canvasW](int cx, int y) -> QRect {
        const int w = 120, h = 38, margin = 4;
        int left = qBound(margin, cx - w / 2, canvasW - w - margin);
        return QRect(left, y, w, h);
    };

    // Date labels — show "MMM d" on the first tick of each month (not just day==1,
    // which misses months whose 1st falls before the chart's start date).
    int prevMonth = -1;
    for (int i = 0; i < totalDays; i += interval) {
        QDate curDate = start.addDays(i);
        int cx = chartRect.left() + static_cast<int>(i * colWidth + colWidth / 2.0);
        painter.setPen(QPen(QColor("#cbd5e1"), 1));
        painter.drawLine(cx, chartRect.bottom(), cx, chartRect.bottom() + 4);
        painter.setPen(QColor("#475569"));
        bool newMonth = (curDate.month() != prevMonth);
        prevMonth = curDate.month();
        if (newMonth || curDate.day() == 1) {
            painter.setFont(QFont("SF Pro Display", 30, QFont::Bold));
            painter.setPen(QColor("#1e293b"));
            painter.drawText(labelRect(cx, chartRect.bottom() + 8),
                             Qt::AlignHCenter | Qt::AlignTop, curDate.toString("MMM d"));
            painter.setFont(QFont("SF Pro Display", 30));
        } else {
            painter.drawText(labelRect(cx, chartRect.bottom() + 8),
                             Qt::AlignHCenter | Qt::AlignTop, QString::number(curDate.day()));
        }
    }

    // Always label the last day — interval steps may not land on it (e.g. 30-day
    // range with interval=2 labels May 5…Jun 2 but skips Jun 3 at index 29).
    // Guard: skip if last day is too close to the previous label (< half interval)
    // to avoid overlapping text.
    int lastIdx = totalDays - 1;
    int distFromPrevLabel = lastIdx % interval;  // steps since last interval tick
    if (lastIdx > 0 && distFromPrevLabel != 0 && distFromPrevLabel >= (interval + 1) / 2) {
        QDate lastDate = start.addDays(lastIdx);
        int cx = chartRect.left() + static_cast<int>(lastIdx * colWidth + colWidth / 2.0);
        painter.setPen(QPen(QColor("#cbd5e1"), 1));
        painter.drawLine(cx, chartRect.bottom(), cx, chartRect.bottom() + 4);
        bool newMonth = (lastDate.month() != prevMonth);
        if (newMonth || lastDate.day() == 1) {
            painter.setFont(QFont("SF Pro Display", 30, QFont::Bold));
            painter.setPen(QColor("#1e293b"));
            painter.drawText(labelRect(cx, chartRect.bottom() + 8),
                             Qt::AlignHCenter | Qt::AlignTop, lastDate.toString("MMM d"));
            painter.setFont(QFont("SF Pro Display", 30));
        } else {
            painter.setPen(QColor("#475569"));
            painter.setFont(QFont("SF Pro Display", 30));
            painter.drawText(labelRect(cx, chartRect.bottom() + 8),
                             Qt::AlignHCenter | Qt::AlignTop, QString::number(lastDate.day()));
        }
    }
}

// One shaded horizontal reference zone [lo,hi] in plot units. Use a low-alpha
// QColor so bars/gridlines drawn afterwards remain readable.
struct RefBand { double lo; double hi; QColor color; };

// Draw reference zones BEHIND everything else. Call right after the SvgChart
// background, before grid/bars.
static void drawReferenceBands(QPainter& painter, const QRect& chartRect,
                               double yMin, double yMax, const QList<RefBand>& bands)
{
    double range = yMax - yMin;
    if (range <= 0.0) return;
    painter.setPen(Qt::NoPen);
    for (const RefBand& b : bands) {
        double lo = qBound(yMin, qMin(b.lo, b.hi), yMax);
        double hi = qBound(yMin, qMax(b.lo, b.hi), yMax);
        if (hi <= lo) continue;
        int yHi = chartRect.bottom() - static_cast<int>(((hi - yMin) / range) * chartRect.height());
        int yLo = chartRect.bottom() - static_cast<int>(((lo - yMin) / range) * chartRect.height());
        painter.setBrush(b.color);
        painter.drawRect(QRect(chartRect.left(), yHi, chartRect.width(), yLo - yHi));
    }
}

// glyph: 0 = filled swatch, 1 = solid tick line, 2 = dashed line
struct LegendItem { int glyph; QColor color; QString label; };

// Compact single-row legend inside the plot. topRight=false anchors top-left
// (use that on charts whose right margin already carries a threshold label).
static void drawLegend(QPainter& painter, const QRect& chartRect,
                       const QList<LegendItem>& items, bool topRight = true)
{
    if (items.isEmpty()) return;
    QFont f("SF Pro Display", 24);
    painter.setFont(f);
    QFontMetrics fm(f);
    const int sw = 28, txtGap = 6, itemGap = 22, h = 30;
    int totalW = 0;
    for (const LegendItem& it : items)
        totalW += sw + txtGap + fm.horizontalAdvance(it.label) + itemGap;
    totalW -= itemGap;
    int x = topRight ? (chartRect.right() - totalW) : (chartRect.left() + 8);
    int y = chartRect.top() + 6;
    int cy = y + h / 2;
    for (const LegendItem& it : items) {
        if (it.glyph == 0) {
            painter.setPen(Qt::NoPen); painter.setBrush(it.color);
            painter.drawRect(QRect(x, cy - 7, sw, 14));
        } else {
            QPen p(it.color, (it.glyph == 1) ? 4.0 : 3.0,
                   (it.glyph == 2) ? Qt::DashLine : Qt::SolidLine);
            painter.setPen(p);
            painter.drawLine(x, cy, x + sw, cy);
        }
        int tw = fm.horizontalAdvance(it.label);
        painter.setPen(QColor("#475569"));
        painter.drawText(QRect(x + sw + txtGap, y, tw + 4, h),
                         Qt::AlignLeft | Qt::AlignVCenter, it.label);
        x += sw + txtGap + tw + itemGap;
    }
}

static void drawThresholdLine(QPainter& painter, const QRect& chartRect,
                               double yMax, double value, const QColor& color,
                               double yMin = 0.0)
{
    if (value < yMin || value > yMax) return;
    int y = chartRect.bottom() - static_cast<int>(((value - yMin) / (yMax - yMin)) * chartRect.height());
    painter.setPen(QPen(color, 3.0, Qt::DashLine));
    painter.drawLine(chartRect.left(), y, chartRect.right(), y);
}

static void drawBars(QPainter& painter, const QRect& chartRect, int totalDays,
                     const QList<double>& dailyValues, double yMax,
                     const QColor& barColor,
                     double thresholdVal = 0.0,
                     const QColor& belowColor = QColor(),
                     const QList<double>& dailyMedians = QList<double>(),
                     double yMin = 0.0)
{
    double range = yMax - yMin;
    double colWidth = static_cast<double>(chartRect.width()) / totalDays;
    double barW = chartBarWidth(colWidth);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < totalDays; ++i) {
        double val = (i < dailyValues.size()) ? dailyValues[i] : 0.0;
        if (val <= yMin) continue;  // no data or below floor
        double barH = ((val - yMin) / range) * chartRect.height();
        if (barH < 1.0) barH = 1.0;
        double x = chartRect.left() + i * colWidth + (colWidth - barW) / 2.0;
        double y = chartRect.bottom() - barH;
        QColor color = (thresholdVal > 0.0 && belowColor.isValid() && val < thresholdVal)
                       ? belowColor : barColor;
        painter.setBrush(color);
        double radius = qMin(3.0, barW / 2.0);
        QPainterPath path;
        path.addRoundedRect(QRectF(x, y, barW, barH), radius, radius);
        painter.drawPath(path);
        if (barH > radius)
            painter.fillRect(QRectF(x, y + radius, barW, barH - radius), color);

        if (i < dailyMedians.size() && dailyMedians[i] > yMin) {
            double medVal = dailyMedians[i];
            double medH = ((medVal - yMin) / range) * chartRect.height();
            double medY = chartRect.bottom() - medH;
            painter.setPen(QPen(QColor("#0f172a"), 5.0));
            painter.drawLine(QPointF(x, medY), QPointF(x + barW, medY));
            painter.setPen(Qt::NoPen);
        }
    }
}

static QColor colorForChannel(ChannelID chanId)
{
    if (chanId == CPAP_MinuteVent)  return QColor("#4db6ac"); // Softer teal
    if (chanId == CPAP_RespRate)    return QColor("#b39ddb"); // Softer purple
    if (chanId == CPAP_TidalVolume) return QColor("#80deea"); // Softer cyan
    if (chanId == CPAP_FlowLimit)   return QColor("#ffb74d"); // Softer orange/amber
    if (chanId == CPAP_Snore)       return QColor("#f48fb1"); // Softer pink
    if (chanId == CPAP_Ti)          return QColor("#c5e1a5"); // Softer lime green
    if (chanId == CPAP_Te)          return QColor("#e6ee9c"); // Softer yellow-green
    if (chanId == OXI_Pulse)        return QColor("#ef9a9a"); // Softer red
    if (chanId == OXI_SPO2)         return QColor("#90caf9"); // Softer blue
    return QColor("#cbd5e1"); // Softer grey
}

static QString unitForChannel(ChannelID chanId)
{
    if (chanId == CPAP_Pressure || chanId == CPAP_EPAP) return "cmH₂O";
    if (chanId == CPAP_Leak || chanId == CPAP_LeakTotal) return "L/min";
    if (chanId == CPAP_MinuteVent)                return "L/min";
    if (chanId == CPAP_RespRate)                  return "br/min";
    if (chanId == CPAP_TidalVolume)               return "mL";
    if (chanId == CPAP_Ti || chanId == CPAP_Te)   return "s";
    if (chanId == OXI_Pulse)                      return "bpm";
    if (chanId == OXI_SPO2)                       return "%";
    return "";
}

// Per-night oximetry summary used to pick the single "worst" night for the
// Lowest-Oxygen Night trace. Pure data — no Qt rendering.
struct NightO2Stat {
    QDate   date;
    double  nadir;     // lowest valid SpO2 that night (artifact-floored)
    double  t90Secs;   // seconds below 90% that night
    bool    valid;     // true only if the night had usable oximetry data
};

// Returns the index of the worst night: lowest nadir wins; ties broken by most
// time below 90%, then earliest date. Returns -1 when no night is valid.
static int selectWorstNightIndex(const QList<NightO2Stat>& nights)
{
    int best = -1;
    for (int i = 0; i < nights.size(); ++i) {
        const NightO2Stat& n = nights[i];
        if (!n.valid) continue;
        if (best < 0) { best = i; continue; }
        const NightO2Stat& b = nights[best];
        if (n.nadir < b.nadir) { best = i; }
        else if (n.nadir == b.nadir && n.t90Secs > b.t90Secs) { best = i; }
        // equal nadir and t90: keep earlier index (already `best`)
    }
    return best;
}

// SpO2 time-in-zone percentages for the Overnight Oxygen Summary, derived from
// cumulative "time above threshold" seconds. totalSec = total recording seconds;
// secAbove95/secAbove90 = cumulative seconds with SpO2 above each threshold.
struct Spo2ZonePercents { double pctGe95; double pct90to95; double pctLt90; };

static Spo2ZonePercents spo2ZonePercents(double totalSec, double secAbove95, double secAbove90)
{
    Spo2ZonePercents z { 0.0, 0.0, 0.0 };
    if (totalSec <= 0.0) return z;
    double ge95 = qBound(0.0, secAbove95, totalSec);
    double ge90 = qBound(ge95, secAbove90, totalSec);   // monotonic: >=95 ⊆ >=90
    z.pctGe95   = 100.0 * ge95 / totalSec;
    z.pct90to95 = 100.0 * (ge90 - ge95) / totalSec;
    z.pctLt90   = 100.0 * (totalSec - ge90) / totalSec;
    return z;
}

static void drawChartTitle(QPainter& painter, const QString& title, const QString& unit)
{
    // Render title inside the CHART_TITLE_H reservation at the top of the SVG canvas.
    // Baseline is placed at y=42 (within the 50px title band).
    const int x = 86; // same as lm, so title aligns with the chart area
    const int yBaseline = 42;

    QFont boldFont("SF Pro Display", 38, QFont::Bold);
    painter.setFont(boldFont);
    painter.setPen(QColor("#0f172a"));
    QFontMetrics boldFm(boldFont);
    int titleW = boldFm.horizontalAdvance(title);
    painter.drawText(x, yBaseline, title);

    if (!unit.isEmpty()) {
        QString unitStr = " (" + unit + ")";
        QFont unitFont("SF Pro Display", 30);
        painter.setFont(unitFont);
        painter.setPen(QColor("#64748b"));
        painter.drawText(x + titleW, yBaseline, unitStr);
    }
}

// One SVG chart canvas: buffer + generator + painter + plot rect, sized to the
// day count via chartCanvasWidth(). Owns the boilerplate every render function
// used to repeat (generator setup, render hints, background, title, margins).
struct SvgChart {
    QBuffer buffer;
    QSvgGenerator generator;
    QPainter painter;
    QRect chartRect;
    int canvasW;

    SvgChart(int totalDays, const QString& svgTitle,
             const QString& title, const QString& unit)
    {
        canvasW = chartCanvasWidth(totalDays);
        buffer.open(QIODevice::WriteOnly);
        generator.setOutputDevice(&buffer);
        generator.setSize(QSize(canvasW, CHART_H));
        generator.setViewBox(QRect(0, 0, canvasW, CHART_H));
        generator.setTitle(svgTitle);
        painter.begin(&generator);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.fillRect(QRect(0, 0, canvasW, CHART_H), Qt::white);
        drawChartTitle(painter, title, unit);
        const int tm = CHART_TITLE_H + 22, bm = 80;
        chartRect = QRect(CHART_LM, tm, canvasW - CHART_LM - CHART_RM, CHART_H - tm - bm);
    }

    QString finish()
    {
        painter.end();
        buffer.close();
        return QString("data:image/svg+xml;base64,") + buffer.data().toBase64();
    }
};

static QString makePageHeaderHtml(const QString& section,
                                   const QString& patientName,
                                   const QString& subInfo,
                                   const QString& logoBase64,
                                   const QString& subtitle = QString())
{
    QString html;
    html += "<table width='100%' cellpadding='0' cellspacing='0' style='border:none; margin-bottom:4pt;'><tr>";
    // Left: logo + section label + optional subtitle + OSCAR description
    html += "<td valign='top' style='border:none; padding:0;'>";
    if (!logoBase64.isEmpty())
        html += "<img src='" + logoBase64 + "' height='38' style='display:block; margin-bottom:3px;'/>";
    html += "<span style='font-size:8.5pt; font-weight:700; color:#454545;'>" + section.toUpper() + "</span>";
    if (!subtitle.isEmpty())
        html += "<br/><span style='font-size:18pt; font-weight:400; color:#0f172a;'>" + subtitle + "</span>";
    html += "<br/><span style='font-size:7.5pt; color:#64748b;'>" + QCoreApplication::translate("ComplianceReport", "OSCAR is free open-source CPAP report software") + "</span>";
    html += "</td>";
    // Right: patient name + sub-info
    html += "<td align='right' valign='top' style='border:none; padding:0; line-height:1.4;'>";
    html += "<span style='font-size:13.5pt; font-weight:700; color:#0f172a;'>" + patientName + "</span><br/>";
    html += "<span style='font-size:9.5pt; color:#475569;'>" + subInfo + "</span>";
    html += "</td>";
    html += "</tr><tr>";
    // Two-tone divider: indigo accent (60px) + light grey remainder
    html += "<td colspan='2' style='border:none; padding-top:5px;'>";
    html += "<table width='100%' cellpadding='0' cellspacing='0' style='border:none;'><tr>"
            "<td width='60' style='border:none; border-bottom:3px solid #454545;'></td>"
            "<td style='border:none; border-bottom:1px solid #cbd5e1;'></td>"
            "</tr></table>";
    html += "</td></tr></table>";
    return html;
}

// Returns inline HTML (no nested table) so the stats td has no internal row breaks
static QString makeStatsRows(const QList<StatsRow>& rows)
{
    QString html = "<table style='width:100%; border-collapse:collapse; font-size:8pt;'>";
    for (const auto& row : rows) {
        QString labelColor = row.color.isEmpty() ? (row.bold ? "#0f172a" : "#64748b") : row.color;
        QString valueColor = row.color.isEmpty() ? "#0f172a" : row.color;
        QString labelWeight = row.bold ? " font-weight:700;" : "";
        QString valueWeight = row.boldValue ? " font-weight:bold;" : "";
        html += "<tr>";
        html += "<td style='padding:1px 6px 1px 0; color:" + labelColor
                + "; white-space:nowrap;" + labelWeight + "'>" + row.label + "</td>";
        html += "<td align='right' style='padding:1px 0; color:"
                + valueColor + "; white-space:nowrap;" + valueWeight + "'>" + row.value + "</td>";
        html += "</tr>";
    }
    html += "</table>";
    return html;
}

static QString makeChannelStatsHtml(double median, double p95, double max, double min,
                                     const QString& unitStr, const QString& accentHex, bool showMin = false)
{
    QString u = unitStr.isEmpty() ? "" : " " + unitStr;
    QString html = "<table style='width:100%; border-collapse:collapse; font-size:8pt;'>";

    auto addRow = [&](const QString& label, const QString& value,
                      const QString& labelColor, const QString& valColor, bool boldLabel) {
        html += "<tr>";
        html += "<td style='padding:1px 6px 1px 0; color:" + labelColor + "; white-space:nowrap;"
                + (boldLabel ? " font-weight:700;" : "") + "'>" + label + "</td>";
        html += "<td align='right' style='padding:1px 0; font-weight:bold; color:" + valColor + "; white-space:nowrap;'>" + value + "</td>";
        html += "</tr>";
    };

    addRow(QCoreApplication::translate("ComplianceReport", "Maximum (avg)"),
           QString::number(max,    'f', 1) + u, "#64748b", "#0f172a", false);
    addRow(QCoreApplication::translate("ComplianceReport", "95th % (avg)"),
           QString::number(p95,    'f', 1) + u, accentHex, accentHex, true);
    addRow(QCoreApplication::translate("ComplianceReport", "Median (avg)"),
           QString::number(median, 'f', 1) + u, "#0f172a", "#0f172a", true);
    if (showMin) {
        // Label as "Period Low" (not "Minimum (avg)") — this is the single
        // lowest non-artifact sample across the entire reporting period.
        // An "average minimum" would mask clinically significant events.
        addRow(QCoreApplication::translate("ComplianceReport", "Period Low"),
               QString::number(min,    'f', 1) + u, "#64748b", "#0f172a", false);
    }

    html += "</table>";
    return html;
}

// SpO2-specific sidebar: median (matches chart bars) as primary stat, nightly low
// (P5 avg) for context, and period-low in red when below 90%. Omits "Maximum (avg)"
// since SpO2 ceiling is always ~99% and carries no clinical meaning.
static QString makeSpO2StatsHtml(double p5avg, double median, double periodLow,
                                  const QString& accentHex)
{
    QString html = "<table style='width:100%; border-collapse:collapse; font-size:8pt;'>";

    auto addRow = [&](const QString& label, const QString& value,
                      const QString& labelColor, const QString& valColor,
                      bool boldLabel, bool boldVal) {
        html += "<tr>";
        html += "<td style='padding:1px 6px 1px 0; color:" + labelColor
                + "; white-space:nowrap;" + (boldLabel ? " font-weight:700;" : "") + "'>"
                + label + "</td>";
        html += "<td align='right' style='padding:1px 0; color:" + valColor
                + "; white-space:nowrap;" + (boldVal ? " font-weight:bold;" : "") + "'>"
                + value + "</td>";
        html += "</tr>";
    };

    // Median (avg) — primary stat, matches chart bars
    addRow(QCoreApplication::translate("ComplianceReport", "Median (avg)"),
           QString::number(median, 'f', 1) + " %",
           accentHex, accentHex, true, true);

    // Nightly Low (avg) — P5, artifact-resistant desaturation floor
    addRow(QCoreApplication::translate("ComplianceReport", "Nightly Low (avg)"),
           QString::number(p5avg, 'f', 1) + " %",
           "#0f172a", "#0f172a", true, true);

    // Period Low — red if below 90% (clinically concerning)
    QString periodLowColor = (periodLow > 0.0 && periodLow < 90.0) ? "#ef4444" : "#0f172a";
    addRow(QCoreApplication::translate("ComplianceReport", "Period Low"),
           periodLow > 0.0 ? QString::number(periodLow, 'f', 0) + " %" :
                             QCoreApplication::translate("ComplianceReport", "N/A"),
           "#64748b", periodLowColor, false, true);

    html += "</table>";
    return html;
}

static QString makeAhiStatsHtml(double AHI, double HI, double AI, double CentralAI, double ObstructiveAI, double UnknownAI, double ReraIndex, double avgCsrPctPerNight)
{
    QString html = "<table style='width:100%; border-collapse:collapse; font-size:8pt;'>";

    auto addRow = [&](const QString& label, const QString& swatchHtml, const QString& value, const QString& labelColor, const QString& valColor, bool boldLabel = false, const QString& unit = "") {
        html += "<tr>";

        // Column 1: Label and Swatch nested table (always rendered for structure consistency)
        html += "<td style='padding:1px 6px 1px 0; color:" + labelColor + "; white-space:nowrap;'>";
        html += "<table width='100%' style='width:100%; border-collapse:collapse; border:none; margin:0; padding:0;'>"
                "<tr>"
                "<td style='padding:0; color:" + labelColor + "; white-space:nowrap;" + (boldLabel ? " font-weight:600;" : "") + "'>" + label + "</td>"
                "<td align='right' width='20' style='width:20px; padding:0; text-align:right; font-size:8.5pt; white-space:nowrap;'>" + (swatchHtml.isEmpty() ? "&nbsp;" : swatchHtml) + "</td>"
                "</tr>"
                "</table>";
        html += "</td>";

        // Column 2: Value right-aligned
        html += "<td align='right' style='padding:1px 0; font-weight:bold; color:" + valColor + "; white-space:nowrap;'>" + value + "</td>";
        // Column 3: Unit suffix — empty for most rows; keeps numeric column aligned across all rows
        html += "<td style='padding:1px 0; font-weight:bold; color:" + valColor + "; white-space:nowrap;'>" + unit + "</td>";

        html += "</tr>";
    };

    addRow(QCoreApplication::translate("ComplianceReport", "AHI"),
           "<span style='color:#6366f1;'>&#9633;</span>",
           QString::number(AHI, 'f', 2),
           "#4f46e5", "#4f46e5", true);

    addRow(QCoreApplication::translate("ComplianceReport", "HI"),
           "<span style='color:#a5b4fc;'>&#9632;</span>",
           QString::number(HI, 'f', 2),
           "#6366f1", "#6366f1", true);

    addRow(QCoreApplication::translate("ComplianceReport", "AI"),
           "<span style='color:#6366f1;'>&#9632;</span>",
           QString::number(AI, 'f', 2),
           "#4338ca", "#4338ca", true);

    addRow(QCoreApplication::translate("ComplianceReport", "CAI"),
           "",
           QString::number(CentralAI, 'f', 2),
           "#64748b", "#0f172a");

    addRow(QCoreApplication::translate("ComplianceReport", "OAI"),
           "",
           QString::number(ObstructiveAI, 'f', 2),
           "#64748b", "#0f172a");

    addRow(QCoreApplication::translate("ComplianceReport", "UAI"),
           "",
           QString::number(UnknownAI, 'f', 2),
           "#64748b", "#0f172a");

    // No swatch: RERA is reported as an index only, it is not stacked in the
    // AHI chart (AHI excludes RERAs).
    addRow(QCoreApplication::translate("ComplianceReport", "RERA"),
           "",
           QString::number(ReraIndex, 'f', 2),
           "#64748b", "#0f172a");

    addRow(QCoreApplication::translate("ComplianceReport", "CSR% (period)"),
           "",
           QString::number(avgCsrPctPerNight, 'f', 1) + "%",
           "#64748b", "#0f172a");

    html += "</table>";
    return html;
}

static QString makePressureStatsHtml(Day* lastDay, double avgPressure, double p95Pressure, double maxPressure)
{
    CPAPMode mode = lastDay ? (CPAPMode)(int)lastDay->settings_max(CPAP_Mode) : MODE_UNKNOWN;
    QString modeStr = lastDay ? lastDay->getCPAPModeStr().toHtmlEscaped() : QCoreApplication::translate("ComplianceReport", "Unknown");
    QString eprStr = lastDay ? lastDay->getPressureRelief().toHtmlEscaped() : QCoreApplication::translate("ComplianceReport", "Off");

    bool isCpap = (mode == MODE_CPAP);

    QString html = "<table style='width:100%; border-collapse:collapse; font-size:8pt;'>";

    auto addRow = [&](const QString& label, const QString& value, const QString& labelColor, const QString& valColor, bool boldVal = true, bool boldLabel = false) {
        html += "<tr>";
        html += "<td style='padding:2px 0; color:" + labelColor + ";" + (boldLabel ? " font-weight:600;" : "") + "'>" + label + "</td>";
        html += "<td align='right' style='padding:2px 0; color:" + valColor + ";" + (boldVal ? " font-weight:bold;" : "") + "'>" + value + "</td>";
        html += "</tr>";
    };

    addRow(QCoreApplication::translate("ComplianceReport", "Mode"), modeStr, "#64748b", "#0f172a", true);
    addRow(QCoreApplication::translate("ComplianceReport", "Set EPR"), eprStr, "#64748b", "#0f172a", true);

    if (isCpap) {
        // Spacer
        html += "<tr><td colspan='2' style='height:8px;'></td></tr>";

        double setPres = lastDay ? lastDay->settings_min(CPAP_Pressure) : 0.0;
        QString setPresStr = lastDay ? lastDay->validPressure(setPres) : QString::number(0.0, 'f', 1);

        addRow(QCoreApplication::translate("ComplianceReport", "Set pressure"),
               setPresStr,
               "#0ea5e9", "#0ea5e9", true, true);
    } else {
        double maxPresSetting = lastDay ? lastDay->settings_max(CPAP_PressureMax) : 0.0;
        QString maxPresStr = lastDay ? lastDay->validPressure(maxPresSetting) : QString::number(0.0, 'f', 1);

        double minPresSetting = lastDay ? lastDay->settings_min(CPAP_PressureMin) : 0.0;
        QString minPresStr = lastDay ? lastDay->validPressure(minPresSetting) : QString::number(0.0, 'f', 1);

        addRow(QCoreApplication::translate("ComplianceReport", "Set Max Pressure"),
               maxPresStr,
               "#64748b", "#0f172a", true);
        addRow(QCoreApplication::translate("ComplianceReport", "Set Min Pressure"),
               minPresStr,
               "#64748b", "#0f172a", true);

        // Spacer
        html += "<tr><td colspan='2' style='height:8px;'></td></tr>";

        addRow(QCoreApplication::translate("ComplianceReport", "Maximum (avg)"),
               QString::number(maxPressure, 'f', 1),
               "#64748b", "#0f172a", true);
        addRow(QCoreApplication::translate("ComplianceReport", "95th % (avg)"),
               QString::number(p95Pressure, 'f', 1),
               "#0ea5e9", "#0ea5e9", true, true);
        addRow(QCoreApplication::translate("ComplianceReport", "Median (avg)"),
               QString::number(avgPressure, 'f', 1),
               "#64748b", "#0f172a", true);
    }

    html += "</table>";
    return html;
}

// statsPadTop: pixels to push stats down from top of the TD. -1 = auto (aligns with bar area).
static QString makeChartBlockHtml(const QString& base64Url, const QString& statsHtml,
                                   int displayW, int displayH, int statsColW,
                                   QVector<QByteArray>& chartReg, int statsPadTop = -1)
{
    // Register the chart's SVG for true-vector replay at print time and reference
    // it from a placeholder <img src="oscarchart:N">. embedVectorCharts() swaps
    // every placeholder for a ChartObjectType text object before printing, so the
    // chart lands in the PDF as vector paths/text instead of a rasterised PNG.
    const QString svgPrefix = QStringLiteral("data:image/svg+xml;base64,");
    int chartIdx = chartReg.size();
    chartReg.append(base64Url.startsWith(svgPrefix)
                        ? QByteArray::fromBase64(base64Url.mid(svgPrefix.length()).toLatin1())
                        : QByteArray());  // unknown format → object renders nothing
    // Title is embedded in the chart — the right cell holds a single object, so
    // QTextDocument cannot split the title from the chart across a page break.
    QString html;
    html += "<div style='page-break-inside:avoid; margin-bottom:8pt;'>";
    html += "<table width='100%' cellpadding='0' cellspacing='0' style='border:none;'>";
    html += "<tr>";
    // Offset stats top by the chart title area so text aligns with the bar area, not the embedded title.
    int titlePad = (statsPadTop >= 0) ? statsPadTop : (CHART_TITLE_H * displayH) / CHART_H;
    html += "<td width='" + QString::number(statsColW) + "' valign='top' style='padding-right:4px; padding-top:" + QString::number(titlePad) + "px;'>";
    html += statsHtml;
    html += "</td>";
    html += "<td width='" + QString::number(displayW) + "' valign='top'>";
    html += "<img src='oscarchart:" + QString::number(chartIdx) + "' width='" + QString::number(displayW) + "' height='" + QString::number(displayH) + "' style='display:block;' />";
    html += "</td>";
    html += "</tr>";
    html += "</table>";
    html += "</div>";
    return html;
}

QString renderUsageChart(const QMap<QDate, Day*>& daysMap, const QDate& start, const QDate& end, double thresholdHr,
                         const QString& title, const QString& unit)
{
    int totalDays = start.daysTo(end) + 1;
    SvgChart c(totalDays, "Usage Chart", title, unit);
    QPainter& painter = c.painter;
    const QRect& chartRect = c.chartRect;

    double maxH = thresholdHr;
    for (int i = 0; i < totalDays; ++i) {
        QDate d = start.addDays(i);
        if (daysMap.contains(d))
            maxH = qMax(maxH, (double)daysMap[d]->hours(MT_CPAP));
    }
    double yMax = 8.0;
    if (maxH > 12.0) yMax = 16.0;
    else if (maxH > 8.0) yMax = 12.0;

    drawYGridAndLabels(painter, chartRect, yMax, 4);

    double colWidth = static_cast<double>(chartRect.width()) / totalDays;
    drawXGridLines(painter, chartRect, totalDays, colWidth, start);
    drawXAxisLabels(painter, chartRect, totalDays, colWidth, start, c.canvasW);

    QList<double> vals;
    for (int i = 0; i < totalDays; ++i) {
        QDate d = start.addDays(i);
        vals.append(daysMap.contains(d) ? (double)daysMap[d]->hours(MT_CPAP) : 0.0);
    }
    drawBars(painter, chartRect, totalDays, vals, yMax,
             QColor("#22c55e"), thresholdHr, QColor("#ef4444")); // Clinical Green/Red
    drawThresholdLine(painter, chartRect, yMax, thresholdHr, QColor("#64748b"));

    // Draw a small amber stub at chart base for days that have a device record
    // but 0 therapy hours — distinguishes "worn, no therapy" from "no data at all".
    {
        double cw = static_cast<double>(chartRect.width()) / totalDays;
        double bw = chartBarWidth(cw);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#fbbf24"));
        for (int i = 0; i < totalDays; ++i) {
            QDate d = start.addDays(i);
            if (daysMap.contains(d) && daysMap[d]->hours(MT_CPAP) == 0.0) {
                double x = chartRect.left() + i * cw + (cw - bw) / 2.0;
                painter.fillRect(QRectF(x, chartRect.bottom() - 5, bw, 5), QColor("#fbbf24"));
            }
        }
    }

    return c.finish();
}

QString renderAhiChart(const QMap<QDate, Day*>& daysMap, const QDate& start, const QDate& end,
                       const QString& title, const QString& unit)
{
    int totalDays = start.daysTo(end) + 1;
    SvgChart c(totalDays, "AHI Chart", title, unit);
    QPainter& painter = c.painter;
    const QRect& chartRect = c.chartRect;

    // Per-day AI and HI for stacked bars. RERA is intentionally NOT stacked:
    // AHI excludes RERAs, and including them made bars cross the "AHI 5"
    // threshold line on nights whose true AHI was below 5.
    double maxAhi = 0.5;
    QList<double> oaVals, caVals, hiVals;
    for (int i = 0; i < totalDays; ++i) {
        QDate d = start.addDays(i);
        double oa = 0.0, ca = 0.0, hi = 0.0;
        if (daysMap.contains(d)) {
            Day* day = daysMap[d];
            double hrs = day->hours(MT_CPAP);
            if (hrs > 0.0) {
                oa = (day->count(CPAP_Obstructive) + day->count(CPAP_Apnea)) / hrs;
                ca = day->count(CPAP_ClearAirway) / hrs;
                hi = day->count(CPAP_Hypopnea) / hrs;
                maxAhi = qMax(maxAhi, oa + ca + hi);
            }
        }
        oaVals.append(oa);
        caVals.append(ca);
        hiVals.append(hi);
    }

    double yMax;
    if      (maxAhi > 15.0) yMax = qCeil(maxAhi / 5.0) * 5.0;
    else if (maxAhi > 10.0) yMax = 15.0;
    else if (maxAhi > 5.0)  yMax = 10.0;
    else if (maxAhi > 3.0)  yMax = 5.0;
    else if (maxAhi > 1.5)  yMax = 3.0;
    else if (maxAhi > 0.5)  yMax = 2.0;
    else                    yMax = 1.0;

    drawYGridAndLabels(painter, chartRect, yMax, 5);

    if (yMax >= 5.0)
        drawReferenceBands(painter, chartRect, 0.0, yMax,
                           { { 0.0, 5.0, QColor(34, 197, 94, 38) } }); // controlled ≤5

    double colWidth = static_cast<double>(chartRect.width()) / totalDays;
    drawXGridLines(painter, chartRect, totalDays, colWidth, start);
    drawXAxisLabels(painter, chartRect, totalDays, colWidth, start, c.canvasW);

    // Stacked bars (bottom→top): AI (dark indigo), HI (light indigo)
    // Two-pass overdraw: draw each layer top-down so upper colours overwrite lower.
    double barW = chartBarWidth(colWidth);
    const QColor oaColor("#4f46e5"); // obstructive — indigo
    const QColor caColor("#fb7185"); // central — rose (stands out)
    const QColor hiColor("#a5b4fc"); // hypopnea — lavender

    auto drawRoundedBar = [&](double x, double y, double w, double hgt, const QColor& col) {
        if (hgt <= 0.0) return;
        double r = qMin(3.0, w / 2.0);
        painter.setBrush(col); painter.setPen(Qt::NoPen);
        QPainterPath path; path.addRoundedRect(QRectF(x, y, w, hgt), r, r);
        painter.drawPath(path);
        if (hgt > r) painter.fillRect(QRectF(x, y + r, w, hgt - r), col);
    };

    painter.setPen(Qt::NoPen);
    for (int i = 0; i < totalDays; ++i) {
        double oa = oaVals[i], ca = caVals[i], hi = hiVals[i];
        double total = oa + ca + hi;
        if (total <= 0.0) continue;
        double x = chartRect.left() + i * colWidth + (colWidth - barW) / 2.0;
        double totalH = (total / yMax) * chartRect.height();
        double caHiH  = ((ca + hi) / yMax) * chartRect.height();
        double hiH    = (hi / yMax) * chartRect.height();
        if (totalH < 1.0) totalH = 1.0;
        double yTop = chartRect.bottom() - totalH;
        // overdraw top-down: full(OA) → (CA+HI) → HI
        drawRoundedBar(x, yTop, barW, totalH, oaColor);
        if (caHiH > 0.0) drawRoundedBar(x, yTop, barW, caHiH, caColor);
        if (hiH   > 0.0) drawRoundedBar(x, yTop, barW, hiH,   hiColor);
    }

    drawThresholdLine(painter, chartRect, yMax, 5.0, QColor("#10b981"));
    if (yMax >= 5.0) {
        int ty = chartRect.bottom() - static_cast<int>((5.0 / yMax) * chartRect.height());
        painter.setFont(QFont("SF Pro Display", 24));
        painter.setPen(QColor("#10b981"));
        painter.drawText(QRect(chartRect.right() + 4, ty - 18, 90, 36),
                         Qt::AlignLeft | Qt::AlignVCenter, "AHI 5");
    }

    drawLegend(painter, chartRect,
               { { 0, oaColor, "Obstructive" },
                 { 0, caColor, "Central" },
                 { 0, hiColor, "Hypopnea" } },
               /*topRight=*/false);  // right margin holds the "AHI 5" label

    return c.finish();
}

QString renderLeakChart(const QMap<QDate, Day*>& daysMap, const QDate& start, const QDate& end, ChannelID leakChan, double redline,
                        const QString& title, const QString& unit)
{
    int totalDays = start.daysTo(end) + 1;
    SvgChart c(totalDays, "Leak Rate Chart", title, unit);
    QPainter& painter = c.painter;
    const QRect& chartRect = c.chartRect;

    double maxVal = qMax(redline > 0 ? redline : 10.0, 10.0);
    for (int i = 0; i < totalDays; ++i) {
        QDate d = start.addDays(i);
        if (daysMap.contains(d) && leakChan != NoChannel && daysMap[d]->hours(MT_CPAP) > 0)
            maxVal = qMax(maxVal, (double)daysMap[d]->percentile(leakChan, 0.95));
    }
    double yMax = qCeil(maxVal / 5.0) * 5.0;
    if (yMax < 10.0) yMax = 10.0;

    drawYGridAndLabels(painter, chartRect, yMax, 4);

    if (redline > 0.0)
        drawReferenceBands(painter, chartRect, 0.0, yMax,
                           { { 0.0,     redline, QColor(34, 197, 94, 30) },   // good seal
                             { redline, yMax,    QColor(239, 68, 68, 26) } }); // elevated

    double colWidth = static_cast<double>(chartRect.width()) / totalDays;
    drawXGridLines(painter, chartRect, totalDays, colWidth, start);
    drawXAxisLabels(painter, chartRect, totalDays, colWidth, start, c.canvasW);

    QList<double> vals;
    QList<double> medians;
    for (int i = 0; i < totalDays; ++i) {
        QDate d = start.addDays(i);
        double v = (daysMap.contains(d) && leakChan != NoChannel && daysMap[d]->hours(MT_CPAP) > 0)
                   ? (double)daysMap[d]->percentile(leakChan, 0.95) : 0.0;
        double med = (daysMap.contains(d) && leakChan != NoChannel && daysMap[d]->hours(MT_CPAP) > 0)
                   ? (double)daysMap[d]->percentile(leakChan, 0.5) : 0.0;
        vals.append(v);
        medians.append(med);
    }
    drawBars(painter, chartRect, totalDays, vals, yMax, QColor("#fbbf24"), 0.0, QColor(), medians);
    if (redline > 0.0) {
        drawThresholdLine(painter, chartRect, yMax, redline, QColor("#ef4444"));
        int ty = chartRect.bottom() - static_cast<int>((redline / yMax) * chartRect.height());
        painter.setFont(QFont("SF Pro Display", 24));
        painter.setPen(QColor("#ef4444"));
        painter.drawText(QRect(chartRect.right() + 4, ty - 18, 90, 36),
                         Qt::AlignLeft | Qt::AlignVCenter, QString::number(redline, 'f', 0));
    }

    drawLegend(painter, chartRect,
               { { 0, QColor("#fbbf24"), "95th percentile" },
                 { 1, QColor("#0f172a"), "median" },
                 { 2, QColor("#ef4444"), "leak limit" } },
               /*topRight=*/false);  // right margin holds the redline value

    return c.finish();
}

QString renderPressureChart(const QMap<QDate, Day*>& daysMap, const QDate& start, const QDate& end, ChannelID pressChan,
                            const QString& title, const QString& unit)
{
    int totalDays = start.daysTo(end) + 1;
    SvgChart c(totalDays, "Therapy Pressure Chart", title, unit);
    QPainter& painter = c.painter;
    const QRect& chartRect = c.chartRect;

    double maxVal = 12.0;
    QList<double> vals;
    QList<double> medians;
    for (int i = 0; i < totalDays; ++i) {
        QDate d = start.addDays(i);
        bool hasData = daysMap.contains(d) && pressChan != NoChannel && daysMap[d]->hours(MT_CPAP) > 0;
        double v   = hasData ? (double)daysMap[d]->percentile(pressChan, 0.95) : 0.0;
        double med = hasData ? (double)daysMap[d]->percentile(pressChan, 0.5)  : 0.0;
        if (hasData) maxVal = qMax(maxVal, v);
        vals.append(v);
        medians.append(med);
    }

    // Prescribed pressure context. Fixed CPAP -> single setpoint; auto -> min/max.
    bool   isAuto = false;
    double setMin = 0.0, setMax = 0.0, setFixed = 0.0;
    for (int i = 0; i < totalDays; ++i) {
        QDate d = start.addDays(i);
        if (!daysMap.contains(d) || daysMap[d]->hours(MT_CPAP) <= 0) continue;
        Day* day = daysMap[d];
        double mn = day->settings_min(CPAP_PressureMin);
        double mx = day->settings_max(CPAP_PressureMax);
        if (mn > 0.0 || mx > 0.0) {           // auto machine reported a window
            isAuto = true;
            if (mn > 0.0) setMin = (setMin == 0.0) ? mn : qMin(setMin, mn);
            if (mx > 0.0) setMax = qMax(setMax, mx);
        } else {
            double f = day->settings_max(CPAP_Pressure);
            if (f > 0.0) setFixed = qMax(setFixed, f);
        }
    }

    if (isAuto) maxVal = qMax(maxVal, setMax);
    else        maxVal = qMax(maxVal, setFixed);
    double yMax = qCeil(maxVal / 4.0) * 4.0;
    if (yMax < 12.0) yMax = 12.0;

    // Auto-floor: pressure never goes to 0; compress axis to the actual therapy
    // range so small APAP excursions are visible (e.g. 8-14 cmH₂O shows as 6-14,
    // not 0-14). Step in 2 cmH₂O increments, hard floor at 4 cmH₂O.
    double yMin = 0.0;
    {
        double dataFloor = yMax;
        for (double med : medians) {
            if (med > 0.0) dataFloor = qMin(dataFloor, med);
        }
        if (dataFloor < yMax) {
            double candidate = std::floor((dataFloor - 2.0) / 2.0) * 2.0;
            yMin = qMax(4.0, candidate);
        }
    }

    drawYGridAndLabels(painter, chartRect, yMax, 4, yMin);

    double colWidth = static_cast<double>(chartRect.width()) / totalDays;
    drawXGridLines(painter, chartRect, totalDays, colWidth, start);
    drawXAxisLabels(painter, chartRect, totalDays, colWidth, start, c.canvasW);

    drawBars(painter, chartRect, totalDays, vals, yMax, QColor("#3b82f6"), 0.0, QColor(), medians, yMin);

    QList<LegendItem> legend = { { 0, QColor("#3b82f6"), "95th percentile" },
                                 { 1, QColor("#0f172a"), "median" } };
    if (isAuto && setMax > 0.0) {
        drawThresholdLine(painter, chartRect, yMax, setMax, QColor("#64748b"), yMin);
        if (setMin > yMin) drawThresholdLine(painter, chartRect, yMax, setMin, QColor("#64748b"), yMin);
        legend.append({ 2, QColor("#64748b"), "set min / max" });
    } else if (setFixed > 0.0) {
        drawThresholdLine(painter, chartRect, yMax, setFixed, QColor("#64748b"), yMin);
        legend.append({ 2, QColor("#64748b"), "set pressure" });
    }
    drawLegend(painter, chartRect, legend, /*topRight=*/true);

    return c.finish();
}

QString renderChannelBarChart(ChannelID chanId, const QColor& barColor,
                              const QMap<QDate, Day*>& daysMap,
                              const QDate& start, const QDate& end,
                              const QString& title, const QString& unit,
                              MachineType mt = MT_CPAP)
{
    int totalDays = start.daysTo(end) + 1;
    SvgChart c(totalDays, "Channel Bar Chart", title, unit);
    QPainter& painter = c.painter;
    const QRect& chartRect = c.chartRect;

    // SpO2: bars show nightly median (P50) — clinically expected, matches doctor's mental model.
    // P5 tick marks the nightly desaturation low inside each bar (artifact-resistant near-worst).
    // All other channels: bars = P95, tick = P50.
    const double barPct  = (chanId == OXI_SPO2) ? 0.50 : 0.95;
    const bool   showTick = true;

    double maxVal = 1.0;
    QList<double> vals;
    QList<double> medians;
    for (int i = 0; i < totalDays; ++i) {
        QDate d = start.addDays(i);
        double v = 0.0;
        double med = 0.0;
        bool hasData = false;
        if (daysMap.contains(d)) {
            // Oximetry channels: presence keyed on recorded SpO2/pulse samples, not
            // machine hours — the oximetry source may be the CPAP device itself.
            if (chanId == OXI_SPO2 || chanId == OXI_Pulse) {
                hasData = daysMap[d]->timeAboveThreshold(chanId, 0.0) > 0.0;
            } else {
                hasData = daysMap[d]->hours(mt) > 0.0;
            }
        }
        if (hasData) {
            v = daysMap[d]->percentile(chanId, barPct);
            med = (chanId == OXI_SPO2) ? daysMap[d]->percentile(chanId, 0.05)
                                       : daysMap[d]->percentile(chanId, 0.5);
            maxVal = qMax(maxVal, v);
        }
        vals.append(v);
        medians.append(med);
    }

    // Compute yMax from peak data, then auto-floor yMin for channels that are
    // physiologically impossible at 0. Channels that legitimately bottom out at 0
    // (Leak, FlowLimit, Snore, Ti, Te) keep yMin=0 so zero-event nights show as
    // empty bars.
    double yMin = 0.0;
    double yMax;
    if (chanId == OXI_SPO2) {
        yMax = 100.0;
    } else {
        double raw = maxVal * 1.1;
        if      (raw <= 5.0)    yMax = qCeil(raw);
        else if (raw <= 20.0)   yMax = qCeil(raw / 2.0)   * 2.0;
        else if (raw <= 100.0)  yMax = qCeil(raw / 5.0)   * 5.0;
        else if (raw <= 500.0)  yMax = qCeil(raw / 25.0)  * 25.0;
        else if (raw <= 2000.0) yMax = qCeil(raw / 100.0) * 100.0;
        else                    yMax = qCeil(raw / 500.0)  * 500.0;
        if (yMax < 1.0) yMax = 1.0;
    }

    // Auto-floor: channels that can never physiologically reach 0.
    // Derive floor from the minimum nightly median, rounded down to a clean step
    // with a small padding, then clamp against an artifact floor where applicable.
    const bool autoFloor = (chanId == OXI_SPO2   || chanId == OXI_Pulse     ||
                             chanId == CPAP_RespRate || chanId == CPAP_TidalVolume ||
                             chanId == CPAP_MinuteVent || chanId == CPAP_Ti || chanId == CPAP_Te);
    if (autoFloor) {
        double dataFloor = yMax;
        double artifactFloor = (chanId == OXI_SPO2)  ? SPO2_ARTIFACT_FLOOR :
                               (chanId == OXI_Pulse) ? PULSE_ARTIFACT_FLOOR : 0.0;
        // Use medians (tick array) as floor reference for all channels.
        // For SpO2 medians holds P5 (nightly low), for others P50 — both are stable floor estimates.
        const QList<double>& floorSrc = medians;
        for (double v : floorSrc) {
            if (v > artifactFloor) dataFloor = qMin(dataFloor, v);
        }
        if (dataFloor < yMax) {
            // Step size mirrors yMax rounding: keep labels clean
            double step = (yMax <= 5.0) ? 0.5 : (yMax <= 20.0) ? 1.0 :
                          (yMax <= 100.0) ? 5.0 : (yMax <= 500.0) ? 25.0 : 100.0;
            double candidate = std::floor((dataFloor - step) / step) * step;
            yMin = qMax(artifactFloor, candidate);
        }
    }

    drawYGridAndLabels(painter, chartRect, yMax, 4, yMin);

    if (chanId == OXI_SPO2) {
        drawReferenceBands(painter, chartRect, yMin, yMax,
                           { { 95.0, 100.0, QColor(34, 197, 94, 34) },   // normal
                             { 90.0,  95.0, QColor(234, 179, 8, 30) },   // mild
                             { yMin,  90.0, QColor(239, 68, 68, 26) } }); // concern
    }

    double colWidth = static_cast<double>(chartRect.width()) / totalDays;
    drawXGridLines(painter, chartRect, totalDays, colWidth, start);
    drawXAxisLabels(painter, chartRect, totalDays, colWidth, start, c.canvasW);

    drawBars(painter, chartRect, totalDays, vals, yMax, barColor, 0.0, QColor(), medians, yMin);

    if (chanId == OXI_SPO2) {
        drawThresholdLine(painter, chartRect, yMax, 90.0, QColor("#ef4444"), yMin);
        if (yMin < 88.0)
            drawThresholdLine(painter, chartRect, yMax, 88.0, QColor("#f59e0b"), yMin);
    }

    if (chanId == OXI_SPO2) {
        drawLegend(painter, chartRect,
                   { { 0, barColor,            "nightly median" },
                     { 1, QColor("#0f172a"),   "nightly low" },
                     { 2, QColor("#ef4444"),   "90% / 88%" } },
                   /*topRight=*/false);
    } else {
        drawLegend(painter, chartRect,
                   { { 0, barColor,          "95th percentile" },
                     { 1, QColor("#0f172a"), "median" } },
                   /*topRight=*/true);
    }

    return c.finish();
}

// Compute the average of per-night percentile values, matching AirView's
// "Median (avg)" / "95th % (avg)" methodology where each night is weighted
// equally instead of pooling all samples into one global distribution.
static double calcDayAvgPercentile(ChannelID chan, EventDataType pct,
                                    const QMap<QDate, Day*>& daysMap,
                                    MachineType mt = MT_CPAP)
{
    double sum = 0.0;
    int count = 0;
    for (auto it = daysMap.constBegin(); it != daysMap.constEnd(); ++it) {
        Day* day = it.value();
        if (!day) continue;
        bool hasData = (mt == MT_OXIMETER || mt == MT_UNKNOWN)
            ? day->timeAboveThreshold(chan, 0.0) > 0.0
            : day->hours(mt) > 0.0;
        if (!hasData) continue;
        // Include the night iff the channel actually recorded data that night,
        // counting legitimate-zero values (e.g. a night with zero flow limitation
        // or zero leak). The old "v > 0.0" guard silently dropped those clean
        // nights, averaging only over bad nights and inflating the result for
        // channels that physiologically bottom out at zero (FlowLimit, Snore).
        // channelHasData() also excludes used-nights where this channel was simply
        // not recorded, so an absent channel never contributes a spurious 0.
        if (!day->channelHasData(chan)) continue;
        sum += day->percentile(chan, pct);
        count++;
    }
    return count > 0 ? sum / count : 0.0;
}

// "Maximum (avg)" is DELIBERATELY the 99.5th percentile, not Day::Max.
// The raw nightly maximum is dominated by non-therapy artifacts: removing the
// mask while the blower runs spikes leak to 60+ L/min, and the same transition
// produces absurd tidal volume (>1000 ml) and respiratory rate (50+ br/min)
// readings. ResMed's AirView avoids this by excluding mask-off spans before
// taking the maximum — segmentation OSCAR cannot replicate from imported data —
// so P99.5 is the artifact guard that keeps "Maximum (avg)" clinically sane.

} // namespace

// ---- True-vector chart embedding (see ChartObjectHandler in the header) ----

QSizeF ChartObjectHandler::intrinsicSize(QTextDocument* /*doc*/, int /*posInDocument*/,
                                         const QTextFormat& format)
{
    // The on-page size was fixed when the placeholder <img> was laid out; reuse it
    // so swapping in the vector object never reflows the page.
    return format.property(ChartSizeProperty).toSizeF();
}

void ChartObjectHandler::drawObject(QPainter* painter, const QRectF& rect,
                                    QTextDocument* /*doc*/, int /*posInDocument*/,
                                    const QTextFormat& format)
{
    int idx = format.property(ChartIndexProperty).toInt();
    if (idx < 0 || idx >= m_svg.size() || m_svg[idx].isEmpty()) return;
    QSvgRenderer renderer(m_svg[idx]);
    if (!renderer.isValid()) return;
    // `painter` is the printer's painter during QTextDocument::print(), so this
    // renders the SVG as native PDF vector content (the viewBox is scaled to fit
    // rect; rect already preserves the chart's aspect ratio, so no distortion).
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);
    renderer.render(painter, rect);
    painter->restore();
}

// Swap every "oscarchart:N" placeholder <img> the report HTML emitted for a real
// ChartObjectType text object so it prints as vector. Must run after setHtml() and
// before print(); the handler is registered first so the layout can size the
// objects as they are inserted.
static void embedVectorCharts(QTextDocument& doc, ChartObjectHandler* handler)
{
    doc.documentLayout()->registerHandler(ChartObjectHandler::ChartObjectType, handler);

    struct Hit { int pos; int index; qreal w; qreal h; };
    QVector<Hit> hits;
    const QString prefix = QStringLiteral("oscarchart:");
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            QTextFragment frag = it.fragment();
            if (!frag.isValid()) continue;
            QTextCharFormat cf = frag.charFormat();
            if (!cf.isImageFormat()) continue;
            QTextImageFormat img = cf.toImageFormat();
            if (!img.name().startsWith(prefix)) continue;
            bool ok = false;
            int idx = img.name().mid(prefix.length()).toInt(&ok);
            if (!ok) continue;
            hits.append({ frag.position(), idx, img.width(), img.height() });
        }
    }

    // Replace from last to first so the earlier recorded positions stay valid as
    // each one-character placeholder is overwritten in place.
    for (int i = hits.size() - 1; i >= 0; --i) {
        const Hit& h = hits[i];
        QTextCursor cur(&doc);
        cur.setPosition(h.pos);
        cur.setPosition(h.pos + 1, QTextCursor::KeepAnchor);
        QTextCharFormat ofmt;
        ofmt.setObjectType(ChartObjectHandler::ChartObjectType);
        ofmt.setProperty(ChartObjectHandler::ChartIndexProperty, h.index);
        ofmt.setProperty(ChartObjectHandler::ChartSizeProperty, QSizeF(h.w, h.h));
        cur.insertText(QString(QChar::ObjectReplacementCharacter), ofmt);
    }
}

void ComplianceReport::exportReport(QWidget *parent)
{
    if (!p_profile) {
        QMessageBox::warning(parent, tr("No Profile"), tr("Please open a profile first."));
        return;
    }

    ComplianceReportDialog dlg(parent);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    QDate start = dlg.startDate();
    QDate end = dlg.endDate();
    QList<SelectedChannel> selectedChans = dlg.selectedChannels();

    if (start > end) {
        QMessageBox::warning(parent, tr("Invalid Date Range"), tr("Start date must be before or equal to end date."));
        return;
    }

    // Check if data exists in range
    int totalDays = start.daysTo(end) + 1;
    int presentDays = p_profile->countDays(MT_CPAP, start, end);
    if (presentDays == 0) {
        QMessageBox::warning(parent, tr("No Data"), tr("No CPAP data found in the selected date range."));
        return;
    }

    // Map dates to Day* objects
    QList<Day*> daysList = p_profile->getDays(MT_CPAP, start, end);
    QMap<QDate, Day*> daysMap;
    for (Day* day : daysList) {
        if (!day) continue;
        daysMap[day->date()] = day;
    }

    // Days with no data at all vs days in map but with 0 therapy hours
    int daysNoData = 0;
    for (int i = 0; i < totalDays; ++i) {
        if (!daysMap.contains(start.addDays(i))) ++daysNoData;
    }
    int daysNotUsed = 0;
    for (auto it = daysMap.begin(); it != daysMap.end(); ++it) {
        if (it.value()->hours(MT_CPAP) == 0.0) ++daysNotUsed;
    }

    // "Used" means actual therapy was recorded (> 0 hours). countDays counts any
    // day with an enabled session, which includes 0-hour summary/brick records;
    // counting those as "used" double-counts them (they also land in daysNotUsed)
    // and dilutes avgHoursUsed so it disagrees with the median. The usage buckets
    // are then mutually exclusive: daysUsed (compliant + non-compliant) + daysNotUsed
    // + daysNoData == totalDays.
    int daysUsed = 0;
    for (auto it = daysMap.begin(); it != daysMap.end(); ++it) {
        if (it.value()->hours(MT_CPAP) > 0.0) ++daysUsed;
    }

    // Prompt for save file name
    QString defaultFilename = QString("Compliance_Report_%1_%2.pdf")
        .arg(p_profile->user->userName())
        .arg(end.toString("yyyyMMdd"));
    QString savePath = QFileDialog::getSaveFileName(parent, tr("Save Compliance Report PDF"), defaultFilename, tr("PDF Files (*.pdf)"));
    if (savePath.isEmpty()) {
        return;
    }

    // Gather statistics
    QList<Day*> allDays = p_profile->getDays(MT_UNKNOWN, start, end);
    // Determine the machine type that actually holds oximetry data for this profile.
    MachineType oxiSourceType = MT_OXIMETER;
    if (p_profile->countDays(MT_OXIMETER, start, end) == 0) {
        ChannelID spo2id  = schema::channel["SPO2"].id();
        ChannelID pulseid = schema::channel["Pulse"].id();
        if ((spo2id  != NoChannel && p_profile->channelAvailable(spo2id))
                || (pulseid != NoChannel && p_profile->channelAvailable(pulseid))) {
            oxiSourceType = MT_CPAP;
        }
    }

    int usedDaysOxi = 0;
    for (Day* day : allDays) {
        if (!day) continue;
        if (day->timeAboveThreshold(OXI_SPO2, 0.0) > 0.0) {
            usedDaysOxi++;
        }
    }
    QList<SettingsChangeBlock> settingsBlocks = getSettingsChanges(start, end, daysList);
    if (usedDaysOxi > 0) {
        for (auto& block : settingsBlocks) {
            double v = calcFilteredStats(OXI_SPO2, oxiSourceType, block.start, block.end, SPO2_ARTIFACT_FLOOR, 0.0).min;
            block.minSpo2 = (v > SPO2_ARTIFACT_FLOOR) ? v : -1.0;
        }
    }

    int compliantDays = p_profile->countCompliantDays(MT_CPAP, start, end);
    double pctCompliant = (double)compliantDays / totalDays * 100.0;
    double pctUsed = (double)daysUsed / totalDays * 100.0;
    double complianceThreshold = p_profile->cpap ? p_profile->cpap->complianceHours() : 4.0;

    double totalHours = p_profile->calcHours(MT_CPAP, start, end);
    // daysUsed counts only days with > 0 hours, so it can be 0 even when the
    // range has enabled-but-0-hour summary records (presentDays > 0). Guard the
    // divide so the PDF never shows "nan hours nan minutes".
    double avgHoursUsed = daysUsed > 0 ? totalHours / daysUsed : 0.0;
    double avgHoursTotal = totalHours / totalDays;

    // Median usage calculation
    QVector<double> activeDaysHours;
    double totalCsrMinutes = 0.0;
    double sumCsrPctPerNight = 0.0;   // Σ nightly CSR% — for AirView-style per-night average
    for (Day* day : daysList) {
        if (!day) continue;
        double hrs = day->hours(MT_CPAP);
        if (hrs > 0.0) {
            activeDaysHours.append(hrs);
            if (p_profile->hasChannel(CPAP_CSR)) {
                double csrPct = day->calcPON(CPAP_CSR);
                totalCsrMinutes += hrs * (csrPct / 100.0) * 60.0;
                sumCsrPctPerNight += csrPct;
            }
        }
    }
    std::sort(activeDaysHours.begin(), activeDaysHours.end());
    double medianHoursUsed = 0.0;
    if (!activeDaysHours.isEmpty()) {
        int size = activeDaysHours.size();
        if (size % 2 == 1) {
            medianHoursUsed = activeDaysHours[size / 2];
        } else {
            medianHoursUsed = (activeDaysHours[size / 2 - 1] + activeDaysHours[size / 2]) / 2.0;
        }
    }
    double avgCsrMinutesPerNight = daysUsed > 0 ? totalCsrMinutes / daysUsed : 0.0;
    // AirView reports "CSR% (avg)" as the mean of each night's CSR%, weighting every
    // night equally — consistent with the per-night averaging used for every other
    // "(avg)" statistic. The previous period-weighted form (CSR hours / total hours)
    // let long nights dominate and diverged from AirView when night lengths varied.
    double avgCsrPctPerNight = daysUsed > 0 ? sumCsrPctPerNight / daysUsed : 0.0;

    // AHI
    double ahiHours = p_profile->calcHours(MT_CPAP, start, end);
    double CentralAI = ahiHours > 0 ? p_profile->calcCount(CPAP_ClearAirway, MT_CPAP, start, end) / ahiHours : 0.0;
    double ObstructiveAI = ahiHours > 0 ? p_profile->calcCount(CPAP_Obstructive, MT_CPAP, start, end) / ahiHours : 0.0;
    double UnknownAI = ahiHours > 0 ? p_profile->calcCount(CPAP_Apnea, MT_CPAP, start, end) / ahiHours : 0.0;
    double AI = CentralAI + ObstructiveAI + UnknownAI;
    double HI = ahiHours > 0 ? p_profile->calcCount(CPAP_Hypopnea, MT_CPAP, start, end) / ahiHours : 0.0;
    double AHI = AI + HI;
    double ReraIndex = ahiHours > 0 ? p_profile->calcCount(CPAP_RERA, MT_CPAP, start, end) / ahiHours : 0.0;
    double RDI = AHI + ReraIndex;

    // Leaks
    ChannelID leakChan = p_profile->hasChannel(CPAP_Leak) ? CPAP_Leak : (p_profile->hasChannel(CPAP_LeakTotal) ? CPAP_LeakTotal : NoChannel);
    double medianLeak = 0.0;
    double p95Leak = 0.0;
    double maxLeak = 0.0;
    if (leakChan != NoChannel) {
        medianLeak = calcDayAvgPercentile(leakChan, 0.5,  daysMap);
        p95Leak    = calcDayAvgPercentile(leakChan, 0.95, daysMap);
        maxLeak    = calcDayAvgPercentile(leakChan, 0.995, daysMap);  // P99.5: see artifact-guard note above
    }

    // 95th percentile flow limitation
    double p95FlowLimit = 0.0;
    if (p_profile->hasChannel(CPAP_FLG))
        p95FlowLimit = calcDayAvgPercentile(CPAP_FLG, 0.95, daysMap);  // per-night avg, consistent with all other channels

    // Apnea Time and Leak Redline calculations
    double totalApneaSecs = p_profile->calcSum(CPAP_ClearAirway, MT_CPAP, start, end) +
                             p_profile->calcSum(CPAP_Obstructive, MT_CPAP, start, end) +
                             p_profile->calcSum(CPAP_Apnea, MT_CPAP, start, end);
    int totalApneaSecsInt = round(totalApneaSecs);
    int apneaH = totalApneaSecsInt / 3600;
    int apneaM = (totalApneaSecsInt % 3600) / 60;
    int apneaS = totalApneaSecsInt % 60;
    QString timeInApneaStr = QString("%1:%2:%3")
        .arg(apneaH, 2, 10, QChar('0'))
        .arg(apneaM, 2, 10, QChar('0'))
        .arg(apneaS, 2, 10, QChar('0'));

    // CSR Time calculation
    int totalCsrSecsInt = round(totalCsrMinutes * 60.0);
    int csrH = totalCsrSecsInt / 3600;
    int csrM = (totalCsrSecsInt % 3600) / 60;
    int csrS = totalCsrSecsInt % 60;
    QString timeInCsrStr = QString("%1:%2:%3")
        .arg(csrH, 2, 10, QChar('0'))
        .arg(csrM, 2, 10, QChar('0'))
        .arg(csrS, 2, 10, QChar('0'));

    double totalLeakSecs = 0.0;
    double totalLeakAboveRedlineSecs = 0.0;
    double leakRedline = p_profile->cpap ? p_profile->cpap->leakRedline() : 24.0;
    for (Day* day : daysList) {
        if (!day) continue;
        if (leakChan != NoChannel) {
            double hrs = day->hours(MT_CPAP);
            totalLeakSecs += hrs * 3600.0;
            totalLeakAboveRedlineSecs += day->timeAboveThreshold(leakChan, leakRedline) * 60.0;
        }
    }
    double pctOverRedline = totalLeakSecs > 0 ? (totalLeakAboveRedlineSecs / totalLeakSecs) * 100.0 : 0.0;
    int leakAboveRedlineSecsInt = round(totalLeakAboveRedlineSecs);
    int larH = leakAboveRedlineSecsInt / 3600;
    int larM = (leakAboveRedlineSecsInt % 3600) / 60;
    int larS = leakAboveRedlineSecsInt % 60;
    QString timeAboveRedlineStr = QString("%1:%2:%3")
        .arg(larH, 2, 10, QChar('0'))
        .arg(larM, 2, 10, QChar('0'))
        .arg(larS, 2, 10, QChar('0'));

    // Additional therapy indices
    double flIndex = (ahiHours > 0 && p_profile->hasChannel(CPAP_FlowLimit))
        ? p_profile->calcCount(CPAP_FlowLimit, MT_CPAP, start, end) / ahiHours : 0.0;
    // Dropout-filtered SpO2 stats over the whole period, computed in one pass and
    // reused everywhere — period-low, T90 (time < 90% excluding probe-loss 0%
    // spans), and the time-weighted average. Reusing one walk means the report
    // never shows two different "minimum SpO2" numbers and never re-reads the same
    // samples for each statistic.
    double t90Secs = 0.0;
    double spo2RecordingSecs = 0.0;  // total SpO2 sample time, for T90-as-% of recording
    double minSpO2 = 0.0, avgSpO2Period = 0.0;
    // Key the oximetry day map on either oximetry channel: a profile may record
    // Pulse without SpO2 (or vice-versa). Keying on SpO2 alone left Pulse-only
    // profiles with an empty map, blanking every Pulse stat/chart downstream.
    QMap<QDate, Day*> oxiDaysMapEarly;
    bool hasSpO2  = p_profile->hasChannel(OXI_SPO2);
    bool hasPulse = p_profile->hasChannel(OXI_Pulse);
    if (hasSpO2 || hasPulse) {
        for (Day* day : allDays) {
            if (!day) continue;
            bool hasOxi = (hasSpO2  && day->timeAboveThreshold(OXI_SPO2,  0.0) > 0.0) ||
                          (hasPulse && day->timeAboveThreshold(OXI_Pulse, 0.0) > 0.0);
            if (hasOxi) oxiDaysMapEarly[day->date()] = day;
        }
    }
    if (hasSpO2) {
        FilteredStats spo2 = calcFilteredStats(OXI_SPO2, oxiSourceType, start, end, SPO2_ARTIFACT_FLOOR, 90.0);
        t90Secs       = spo2.timeBelowSecs;
        minSpO2       = spo2.min;
        avgSpO2Period = spo2.wavg;
        for (Day* day : oxiDaysMapEarly)
            spo2RecordingSecs += day->timeAboveThreshold(OXI_SPO2, 0.0) * 60.0;
    }
    bool minSpO2Set = (minSpO2 > 0.0);

    // Dropout-filtered Pulse stats over the whole period, same single-pass reuse
    // (period min/max + time-weighted average).
    double minPulsePeriod = 0.0, maxPulsePeriod = 0.0, avgPulsePeriod = 0.0;
    if (p_profile->hasChannel(OXI_Pulse)) {
        FilteredStats pulse = calcFilteredStats(OXI_Pulse, oxiSourceType, start, end, PULSE_ARTIFACT_FLOOR, 0.0);
        minPulsePeriod = pulse.min;
        maxPulsePeriod = pulse.max;
        avgPulsePeriod = pulse.wavg;
    }
    int t90SecsInt = (int)t90Secs;
    int t90H = t90SecsInt / 3600;
    int t90M = (t90SecsInt % 3600) / 60;
    int t90S = t90SecsInt % 60;
    QString t90Str = QString("%1:%2:%3")
        .arg(t90H, 2, 10, QChar('0')).arg(t90M, 2, 10, QChar('0')).arg(t90S, 2, 10, QChar('0'));

    // Device Settings & Info
    Machine *cpap = p_profile->GetMachine(MT_CPAP);
    QString brand = cpap ? cpap->brand().toHtmlEscaped() : tr("Unknown");
    QString model = cpap ? cpap->model().toHtmlEscaped() : tr("Unknown");
    QString serial = cpap ? cpap->serial().toHtmlEscaped() : tr("Unknown");

    // Leak reporting convention differs by manufacturer:
    //  - ResMed reports UNINTENTIONAL leak (mask vent flow already subtracted);
    //    >24 L/min indicates a poor seal, so the 24 L/min redline is meaningful.
    //  - Philips Respironics (and others) report TOTAL leak (intentional mask
    //    exhaust + seal leak), where 30-50 L/min is normal and a 24 L/min redline
    //    would be alarmingly wrong.
    // The channel itself is the authoritative discriminator: CPAP_Leak is always
    // unintentional leak, CPAP_LeakTotal is always total leak.
    bool isTotalLeak = (leakChan == CPAP_LeakTotal);
    QString leakTypeLabel = tr("Leak Rate");
    double leakChartRedline = isTotalLeak ? 0.0 : leakRedline;  // hide 24 L/min line for total leak

    Day *lastDay = p_profile->GetGoodDay(p_profile->LastDay(MT_CPAP), MT_CPAP);
    QString modeStr = lastDay ? lastDay->getCPAPModeStr().toHtmlEscaped() : tr("Unknown");
    QString pressureSettings = lastDay ? lastDay->getPressureSettings().toHtmlEscaped() : tr("Unknown");
    QString relief = lastDay ? lastDay->getPressureRelief().toHtmlEscaped() : tr("None");

    // Gather comfort settings
    QString humidLevel = getSettingStr(lastDay, CPAP_HumidSetting);
    if (humidLevel.isEmpty()) humidLevel = getSettingStr(lastDay, PRS1_HumidLevel);
    humidLevel = humidLevel.toHtmlEscaped();
    QString rampTime = getSettingStr(lastDay, CPAP_RampTime).toHtmlEscaped();
    QString rampPres = getSettingStr(lastDay, CPAP_RampPressure);

    QString pressDisplay = pressureSettings;
    if (!pressDisplay.contains("cmH2O", Qt::CaseInsensitive)) {
        pressDisplay += " cmH2O";
    }

    // Patient DOB and Age calculation
    QDate dob = p_profile->user->DOB();
    int age = -1;
    if (dob.isValid()) {
        age = QDate::currentDate().year() - dob.year();
        if (QDate::currentDate() < dob.addYears(age))
            age--;
    }

    // Local duration formatting helper
    auto getHoursMinutes = [](double hrsVal) {
        int h = floor(hrsVal);
        int m = round((hrsVal - h) * 60.0);
        if (m >= 60) {
            h += 1;
            m -= 60;
        }
        return qMakePair(h, m);
    };
    
    auto formatHoursMinutesShort = [&getHoursMinutes](double hrsVal) {
        QPair<int, int> hm = getHoursMinutes(hrsVal);
        return QString("%1h %2m").arg(hm.first).arg(hm.second);
    };

    // Determine device mode early — used to gate pressure stats/charts for CPAP
    CPAPMode p1Mode = lastDay ? (CPAPMode)(int)lastDay->settings_max(CPAP_Mode) : MODE_UNKNOWN;
    bool p1IsCpap = (p1Mode == MODE_CPAP);

    // Calculate CPAP Pressure Stats
    double avgPressure = 0.0;
    double p95Pressure = 0.0;
    double maxPressure = 0.0;
    if (p_profile->hasChannel(CPAP_Pressure)) {
        avgPressure = calcDayAvgPercentile(CPAP_Pressure, 0.5,  daysMap);
        p95Pressure = calcDayAvgPercentile(CPAP_Pressure, 0.95, daysMap);
        maxPressure = calcDayAvgPercentile(CPAP_Pressure, 0.995, daysMap);
    }

    // Render compact bar charts in vector SVG format (no legend inside image)
    QString usageChartUrl = renderUsageChart(daysMap, start, end, complianceThreshold, tr("Daily Usage"), tr("hours"));
    QString ahiChartUrl   = renderAhiChart(daysMap, start, end, tr("AHI"), tr("events/hr"));
    QString leakChartUrl  = renderLeakChart(daysMap, start, end, leakChan, leakChartRedline, tr("Leak Rate"), tr("L/min"));
    QString pressChartUrl = renderPressureChart(daysMap, start, end, CPAP_Pressure, tr("Pressure"), tr("cmH₂O"));

    // Set up printer early so we can compute the actual usable content width.
    QPrinter printer(QPrinter::ScreenResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(savePath);
    printer.setPageSize(QPageSize(QPageSize::A4));
    printer.setPageOrientation(QPageLayout::Portrait);
    printer.setPageMargins(QMarginsF(8, 4, 8, 4), QPageLayout::Millimeter);

    // Stats column is wide enough for the longest label/value pair.
    const int STATS_COL_W = 135;
    // Chart fills the remaining content width minus stats column and its padding.
    // Measure the page in PIXELS at the printer resolution — that is the unit
    // QTextDocument lays out with. paintRect() in Points would mismatch the px
    // <img> width and overflow the page edge.
    const qreal DOC_MARGIN = 4.0;  // QTextDocument default; pinned at print time below.
    const int contentW = printer.pageLayout().paintRectPixels(printer.resolution()).width();
    // Subtract the margin QTextDocument reserves inside contentW (both sides) so
    // the chart image fits the actually-usable width, plus cell padding slack.
    const int usableW = contentW - 2 * static_cast<int>(DOC_MARGIN);
    // Scale the on-page chart size with the canvas width so short ranges print
    // as compact charts (constant bar width) instead of stretching to full page.
    // The scale factor canvasW/CHART_MAX_W is the same for every chart in the
    // report (all charts span the same date range), so fonts stay uniform.
    const int chartCanvasW = chartCanvasWidth(totalDays);
    const int fullDisplayW = qMax(CHART_DISPLAY_MIN_W, usableW - STATS_COL_W - 16);
    int displayW = qMax(CHART_DISPLAY_MIN_W, (fullDisplayW * chartCanvasW) / CHART_MAX_W);
    int displayH = (CHART_H * displayW) / chartCanvasW;

    // SVG bytes of every chart, in placeholder order. Populated by makeChartBlockHtml
    // and replayed as vector by embedVectorCharts() at print time.
    QVector<QByteArray> chartRegistry;

    // Reuse oximeter daysMap built earlier
    const QMap<QDate, Day*>& oxiDaysMap = oxiDaysMapEarly;

    // Build channel charts for every checked channel that has data
    // Skip channels already covered by the four fixed charts above
    QSet<ChannelID> fixedChanIds;
    fixedChanIds.insert(CPAP_Pressure);
    fixedChanIds.insert(CPAP_Snore);    // event-count channel, stats table only
    fixedChanIds.insert(CPAP_Ti);       // stats table only
    fixedChanIds.insert(CPAP_Te);       // stats table only
    if (leakChan != NoChannel)
        fixedChanIds.insert(leakChan);

    struct ChannelChartBlock {
        QString base64Url, statsHtml;
    };
    QList<ChannelChartBlock> chanCharts;

    for (const auto& ch : selectedChans) {
        ChannelID cid = static_cast<ChannelID>(ch.id);
        MachineType mtype = static_cast<MachineType>(ch.type);
        if (mtype == MT_OXIMETER) {
            mtype = oxiSourceType;
        }
        if (fixedChanIds.contains(cid)) continue;
        if (!p_profile->hasChannel(cid)) continue;
        // EPAP is only a clinically independent channel in BiLevel/ASV modes.
        // In CPAP and APAP, EPAP = set pressure - EPR level — derivative, not worth charting.
        if (cid == CPAP_EPAP && (p1Mode == MODE_CPAP || p1Mode == MODE_APAP)) continue;

        // Only oximetry channels chart over the oximetry days map. Keying this on
        // machine type broke when SpO2 came from the CPAP device itself
        // (oxiSourceType == MT_CPAP): every CPAP channel then matched and charts
        // like Minute Vent silently dropped nights without oximetry data.
        const bool isOxiChan = (cid == OXI_SPO2 || cid == OXI_Pulse);
        const QMap<QDate, Day*>& useDaysMap = isOxiChan ? oxiDaysMap : daysMap;

        bool hasData = false;
        for (Day* day : useDaysMap) {
            if (day->timeAboveThreshold(cid, 0.0) > 0.0 && day->percentile(cid, 0.95) > 0.0) {
                hasData = true;
                break;
            }
        }
        if (!hasData) continue;

        QColor color = colorForChannel(cid);
        QString unit = unitForChannel(cid);
        QString chanChartUrl = renderChannelBarChart(cid, color, useDaysMap, start, end, ch.label, unit, mtype);

        double cMedian = calcDayAvgPercentile(cid, 0.5,  useDaysMap, mtype);
        double c95     = calcDayAvgPercentile(cid, 0.95, useDaysMap, mtype);
        double cMax    = calcDayAvgPercentile(cid, 0.995, useDaysMap, mtype);  // P99.5 artifact guard
        // Oximetry channels: exclude sensor dropout artifacts from the minimum.
        // Reuse the period-low values computed once above instead of re-walking.
        double cMin;
        if (cid == OXI_SPO2)        cMin = minSpO2;
        else if (cid == OXI_Pulse)  cMin = minPulsePeriod;
        else                        cMin = p_profile->calcMin(cid, (MachineType)ch.type, start, end);

        ChannelChartBlock block;
        block.base64Url = chanChartUrl;
        if (cid == OXI_SPO2) {
            // SpO2 sidebar: P5 avg (matches chart bars) + median + period-low.
            // P95/maximum omitted — SpO2 ceiling is always ~99%, carries no clinical meaning.
            double c5 = calcDayAvgPercentile(cid, 0.05, useDaysMap, mtype);
            block.statsHtml = makeSpO2StatsHtml(c5, cMedian, cMin, color.name());
        } else {
            bool isOxi = (cid == OXI_Pulse || cid == OXI_SPO2);
            block.statsHtml = makeChannelStatsHtml(cMedian, c95, cMax, cMin, unit, color.name(), isOxi);
        }
        chanCharts.append(block);
    }

    // Load OSCAR logo for page headers
    QImage logoImage(":/icons/logo-md.png");
    QString logoBase64 = logoImage.isNull() ? QString() : imageToBase64DataUrl(logoImage);

    // Common header strings
    QString patientNameStr = (p_profile->user->lastName() + ", " + p_profile->user->firstName()).toHtmlEscaped();
    QString dateRangeStr   = start.toString("MMM d, yyyy") + " - " + end.toString("MMM d, yyyy");
    QString page1SubInfo;
    if (dob.isValid())
        page1SubInfo = "DOB: <b>" + dob.toString("MMM d, yyyy") + "</b>"
                       " &nbsp;|&nbsp; Age: <b>" + QString::number(age) + " yrs</b><br/>";
    page1SubInfo += "Range: <b>" + dateRangeStr + "</b>";
    // Baseline (diagnostic) AHI -> current AHI, shown in the header so the
    // doctor sees the whole therapy journey without consuming page-1 body space.
    // Only appended when a pre-treatment baseline was entered at diagnosis.
    {
        double untreatedAHI = p_profile->cpap ? p_profile->cpap->untreatedAHI() : 0.0;
        QDate diagDate = p_profile->cpap ? p_profile->cpap->dateDiagnosed() : QDate();
        if (untreatedAHI > 0.0) {
            double pctChange = (untreatedAHI - AHI) / untreatedAHI * 100.0;
            QString sign = pctChange >= 0.0 ? "-" : "+";  // reduction shown as negative
            QString diagStr = diagDate.isValid()
                ? " " + tr("(dx %1)").arg(diagDate.toString("MMM yyyy")) : QString();
            page1SubInfo += "<br/>" + tr("Baseline AHI") + ": <b>" + QString::number(untreatedAHI,'f',1)
                            + "</b>" + diagStr + " &rarr; <b>" + QString::number(AHI,'f',1) + "</b>"
                            " (<b>" + sign + QString::number(qAbs(pctChange),'f',0) + "%</b>)";
        }
    }
    QString pageSubInfo    = "Range: <b>" + dateRangeStr + "</b>";

    // Generate HTML report (highly compatible styling for QTextDocument)
    QString html = "<html><head><style>";
    html += "body { font-family: \"SF Pro Display\", \"SF Pro Text\", -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, \"Helvetica Neue\", Arial, sans-serif; color: #0f172a; margin: 0pt; line-height: 1.25; background-color: #ffffff; }";
    html += "</style></head><body>";

    // ==========================================
    // PAGE 1: EXECUTIVE SUMMARY & STATS
    // ==========================================
    
    html += makePageHeaderHtml(tr("Sleep Therapy Insights"), patientNameStr, page1SubInfo, logoBase64, tr("Compliance Report"));

    // AirView-style sections: Usage, Device, Therapy

    // Pre-compute set pressure values for device section
    QString p1MinPresStr, p1MaxPresStr;
    if (lastDay) {
        if (p1IsCpap) {
            double sp = lastDay->settings_min(CPAP_Pressure);
            p1MinPresStr = lastDay->validPressure(sp) + " cmH2O";
        } else {
            double mn = lastDay->settings_min(CPAP_PressureMin);
            double mx = lastDay->settings_max(CPAP_PressureMax);
            p1MinPresStr = lastDay->validPressure(mn) + " cmH2O";
            p1MaxPresStr = lastDay->validPressure(mx) + " cmH2O";
        }
    }
    int nonCompliantUsedDays = daysUsed - compliantDays;
    double pctNonCompliant = totalDays > 0 ? (double)nonCompliantUsedDays / totalDays * 100.0 : 0.0;

    const QString cellL      = "style='padding:1.5px 6px; color:#475569; font-size:9.5pt; border-bottom:1px solid #e2e8f0;'";
    const QString cellR      = "style='padding:1.5px 6px; color:#0f172a; font-size:9.5pt; border-bottom:1px solid #e2e8f0;'";
    const QString cellRB     = "style='padding:1.5px 6px; font-weight:700; color:#0f172a; font-size:10.5pt; border-bottom:1px solid #e2e8f0;'";
    const QString cellLThick = "style='padding:1.5px 6px; color:#475569; font-size:9.5pt; border-bottom:2px solid #94a3b8;'";
    const QString cellRBThick= "style='padding:1.5px 6px; font-weight:700; color:#0f172a; font-size:10.5pt; border-bottom:2px solid #94a3b8;'";
    const QString tblStyle   = "border-collapse:collapse; border:none; margin-bottom:3pt; width:100%; page-break-inside:avoid;";

    const QString cellHoursVal   = "style='padding:3px 2px; color:#0f172a; font-size:9.5pt; border-bottom:1px solid #e2e8f0; text-align:right; width:35px;'";
    const QString cellHoursLbl   = "style='padding:3px 2px; color:#0f172a; font-size:9.5pt; border-bottom:1px solid #e2e8f0; text-align:left; width:50px;'";
    const QString cellMinutesVal = "style='padding:3px 2px; color:#0f172a; font-size:9.5pt; border-bottom:1px solid #e2e8f0; text-align:right; width:25px;'";
    const QString cellMinutesLbl = "style='padding:3px 8px 3px 2px; color:#0f172a; font-size:9.5pt; border-bottom:1px solid #e2e8f0; text-align:left; width:60px;'";

    QPair<int, int> totalHM = getHoursMinutes(totalHours);
    QPair<int, int> avgTotalHM = getHoursMinutes(avgHoursTotal);
    QPair<int, int> avgUsedHM = getHoursMinutes(avgHoursUsed);
    QPair<int, int> medianUsedHM = getHoursMinutes(medianHoursUsed);

    // === USAGE ===
    html += "<table width='100%' cellpadding='0' cellspacing='0' style='" + tblStyle + "'>";
    html += "<tr style='background-color:#454545;'>"
            "<td style='padding:6px 8px; font-size:11pt; font-weight:700; color:#ffffff;'>" + tr("Usage") + "</td>"
            "<td colspan='4' align='right' style='padding:5px 8px; font-size:9pt; font-weight:700; color:#ffffff;'>" + dateRangeStr + "</td></tr>";
    html += "<tr><td " + cellL + "><b style='color:#0f172a;'>" + tr("Usage days") + "</b></td>"
            "<td colspan='4' align='right' " + cellRB + ">" + QString("%1/%2 days (%3%)").arg(daysUsed).arg(totalDays).arg(QString::number(pctUsed,'f',0)) + "</td></tr>";
    html += "<tr><td " + cellL + "><b style='color:#0f172a;'>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&gt;= " + QString::number(complianceThreshold,'f',0) + tr(" hours") + "</b></td>"
            "<td colspan='4' align='right' " + cellRB + ">" + QString("%1 days (%2%)").arg(compliantDays).arg(QString::number(pctCompliant,'f',0)) + "</td></tr>";
    html += "<tr><td " + cellLThick + "><b style='color:#0f172a;'>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&lt; " + QString::number(complianceThreshold,'f',0) + tr(" hours") + "</b></td>"
            "<td colspan='4' align='right' " + cellRBThick + ">" + QString("%1 days (%2%)").arg(nonCompliantUsedDays).arg(QString::number(pctNonCompliant,'f',0)) + "</td></tr>";
    html += "<tr><td " + cellL + ">" + tr("Usage hours") + "</td>"
            "<td colspan='4' align='right' " + cellR + ">"
            + QString::number(totalHM.first) + tr(" hours ") + QString::number(totalHM.second) + tr(" minutes")
            + "</td></tr>";
    html += "<tr><td " + cellL + ">&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;" + tr("Average usage (total days)") + "</td>"
            "<td colspan='4' align='right' " + cellR + ">"
            + QString::number(avgTotalHM.first) + tr(" hours ") + QString::number(avgTotalHM.second) + tr(" minutes")
            + "</td></tr>";
    html += "<tr><td " + cellL + ">&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;" + tr("Average usage (days used)") + "</td>"
            "<td colspan='4' align='right' " + cellR + ">"
            + QString::number(avgUsedHM.first) + tr(" hours ") + QString::number(avgUsedHM.second) + tr(" minutes")
            + "</td></tr>";
    html += "<tr><td " + cellL + ">&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;" + tr("Median usage (days used)") + "</td>"
            "<td colspan='4' align='right' " + cellR + ">"
            + QString::number(medianUsedHM.first) + tr(" hours ") + QString::number(medianUsedHM.second) + tr(" minutes")
            + "</td></tr>";
    html += "</table>";

    // === DEVICE ===
    html += "<table width='100%' cellpadding='0' cellspacing='0' style='" + tblStyle + "'>";
    html += "<tr style='background-color:#454545;'>"
            "<td colspan='2' style='padding:6px 8px; font-size:11pt; font-weight:700; color:#ffffff;'>" + brand + " " + model + "</td></tr>";
    html += "<tr><td " + cellL + ">" + tr("Serial number") + "</td><td align='right' " + cellR + ">" + serial + "</td></tr>";
    html += "<tr><td " + cellL + ">" + tr("Mode") + "</td><td align='right' " + cellR + ">" + modeStr + "</td></tr>";
    if (lastDay && p1IsCpap) {
        html += "<tr><td " + cellL + ">" + tr("Pressure") + "</td><td align='right' " + cellR + ">" + p1MinPresStr + "</td></tr>";
    } else if (lastDay) {
        html += "<tr><td " + cellL + ">" + tr("Min Pressure") + "</td><td align='right' " + cellR + ">" + p1MinPresStr + "</td></tr>";
        html += "<tr><td " + cellL + ">" + tr("Max Pressure") + "</td><td align='right' " + cellR + ">" + p1MaxPresStr + "</td></tr>";
    }
    bool isResMed = brand.contains("ResMed", Qt::CaseInsensitive);
    bool eprIsOn = false;
    QString eprModeStr, eprLevelStr, responseStr;
    if (isResMed && lastDay) {
        if (lastDay->settingExists(RMS9_EPR)) {
            double eprVal = lastDay->settings_wavg(RMS9_EPR);
            if (eprVal > 0.0) {
                eprIsOn = true;
                eprModeStr = getSettingStr(lastDay, RMS9_EPR).toHtmlEscaped();
                double lvl = lastDay->settings_wavg(RMS9_EPRLevel);
                eprLevelStr = QString::number(qRound(lvl));
                if (lastDay->settingExists(RMAS1x_Comfort)) {
                    responseStr = getSettingStr(lastDay, RMAS1x_Comfort).toHtmlEscaped();
                }
            }
        }
    }

    if (eprIsOn) {
        html += "<tr><td " + cellL + ">" + tr("EPR") + "</td><td align='right' " + cellR + ">" + eprModeStr + "</td></tr>";
        html += "<tr><td " + cellL + ">" + tr("EPR level") + "</td><td align='right' " + cellR + ">" + eprLevelStr + "</td></tr>";
        if (!responseStr.isEmpty()) {
            html += "<tr><td " + cellL + ">" + tr("Response") + "</td><td align='right' " + cellR + ">" + responseStr + "</td></tr>";
        }
    } else {
        html += "<tr><td " + cellL + ">" + tr("EPR / Pressure Relief") + "</td><td align='right' " + cellR + ">" + relief + "</td></tr>";
    }
    if (!humidLevel.isEmpty())
        html += "<tr><td " + cellL + ">" + tr("Humidifier Level") + "</td><td align='right' " + cellR + ">" + humidLevel + "</td></tr>";
    if (!rampTime.isEmpty() && rampTime != "0.0 min") {
        html += "<tr><td " + cellL + ">" + tr("Ramp Time") + "</td><td align='right' " + cellR + ">" + rampTime + "</td></tr>";
        if (!rampPres.isEmpty())
            html += "<tr><td " + cellL + ">" + tr("Ramp Pressure") + "</td><td align='right' " + cellR + ">" + rampPres.toHtmlEscaped() + "</td></tr>";
    } else {
        html += "<tr><td " + cellL + ">" + tr("Ramp") + "</td><td align='right' " + cellR + ">" + tr("Off") + "</td></tr>";
    }

    // Mask: show if configured in Preferences
    {
        QString maskDesc = p_profile->cpap ? p_profile->cpap->maskDescription() : QString();
        if (!maskDesc.isEmpty()) {
            MaskType mtype = p_profile->cpap ? p_profile->cpap->maskType() : Mask_Unknown;
            if (mtype == Mask_Unknown) {
                if (maskDesc == "ResMed AirFit F20" ||
                    maskDesc == "ResMed AirTouch F20" ||
                    maskDesc == "ResMed AirFit F30" ||
                    maskDesc == "ResMed AirFit F30i" ||
                    maskDesc == "ResMed AirFit F40" ||
                    maskDesc == "Fisher & Paykel Vitera" ||
                    maskDesc == "Fisher & Paykel Evora Full Face" ||
                    maskDesc == "Philips DreamWear Full Face" ||
                    maskDesc == "Philips Amara View") {
                    mtype = Mask_FullFace;
                } else if (maskDesc == "ResMed AirFit N20" ||
                           maskDesc == "ResMed AirFit N30" ||
                           maskDesc == "ResMed AirFit N30i" ||
                           maskDesc == "Fisher & Paykel Solo" ||
                           maskDesc == "Fisher & Paykel Eson 2" ||
                           maskDesc == "Philips DreamWear Nasal" ||
                           maskDesc == "Philips DreamWisp") {
                    mtype = Mask_StandardNasal;
                } else if (maskDesc == "ResMed AirFit P10" ||
                           maskDesc == "ResMed AirFit P10 For Her" ||
                           maskDesc == "ResMed AirFit P30i" ||
                           maskDesc == "Fisher & Paykel Brevida" ||
                           maskDesc == "Fisher & Paykel Nova Micro" ||
                           maskDesc == "Fisher & Paykel Evora" ||
                           maskDesc == "Philips DreamWear Nasal Pillow") {
                    mtype = Mask_NasalPillows;
                }
            }
            
            QString typeSuffix;
            if (mtype == Mask_FullFace) typeSuffix = " (" + tr("Full Face") + ")";
            else if (mtype == Mask_StandardNasal) typeSuffix = " (" + tr("Nasal") + ")";
            else if (mtype == Mask_NasalPillows) typeSuffix = " (" + tr("Nasal Pillow") + ")";
            else if (mtype == Mask_Hybrid) typeSuffix = " (" + tr("Hybrid") + ")";

            html += "<tr><td " + cellL + ">" + tr("Mask") + "</td><td align='right' " + cellR + ">" + (maskDesc + typeSuffix).toHtmlEscaped() + "</td></tr>";
        }
    }

    html += "</table>";

    // === THERAPY ===
    const QString tkL = "style='padding:1.5px 6px; color:#475569; font-size:9.5pt; width:31%; border-bottom:1px solid #e2e8f0;'";
    const QString tkM = "style='padding:1.5px 6px; color:#64748b; font-size:9.5pt; width:23%; border-bottom:1px solid #e2e8f0;'";
    const QString tkR = "style='padding:1.5px 6px; color:#64748b; font-size:9.5pt; width:23%; border-bottom:1px solid #e2e8f0; text-align:right;'";

    html += "<table width='100%' cellpadding='0' cellspacing='0' style='" + tblStyle + "'>";
    html += "<tr style='background-color:#454545;'><td colspan='4' style='padding:6px 8px; font-size:11pt; font-weight:700; color:#ffffff;'>" + tr("Therapy") + "</td></tr>";

    if (p_profile->hasChannel(CPAP_Pressure) && !p1IsCpap) {
        html += "<tr>"
                "<td " + tkL + ">" + tr("Pressure - cmH₂O") + "</td>"
                "<td " + tkM + ">" + tr("Median: ") + "<b style='color:#0f172a;'>" + QString::number(avgPressure,'f',1) + "</b></td>"
                "<td " + tkM + ">" + tr("95th percentile: ") + "<b style='color:#0f172a;'>" + QString::number(p95Pressure,'f',1) + "</b></td>"
                "<td " + tkR + ">" + tr("Maximum: ") + "<b style='color:#0f172a;'>" + QString::number(maxPressure,'f',1) + "</b></td>"
                "</tr>";
    }
    if (leakChan != NoChannel) {
        html += "<tr>"
                "<td " + tkL + ">" + leakTypeLabel + tr(" - L/min") + "</td>"
                "<td " + tkM + ">" + tr("Median: ") + "<b style='color:#0f172a;'>" + QString::number(medianLeak,'f',1) + "</b></td>"
                "<td " + tkM + ">" + tr("95th percentile: ") + "<b style='color:#0f172a;'>" + QString::number(p95Leak,'f',1) + "</b></td>"
                "<td " + tkR + ">" + tr("Maximum: ") + "<b style='color:#0f172a;'>" + QString::number(maxLeak,'f',1) + "</b></td>"
                "</tr>";
    }
    html += "<tr>"
            "<td " + tkL + ">" + tr("Events per hour") + "</td>"
            "<td " + tkM + ">AI: <b style='color:#0f172a;'>" + QString::number(AI,'f',1) + "</b></td>"
            "<td " + tkM + ">HI: <b style='color:#0f172a;'>" + QString::number(HI,'f',1) + "</b></td>"
            "<td " + tkR + ">AHI: <b style='color:#0f172a;'>" + QString::number(AHI,'f',1) + "</b></td>"
            "</tr>";
    html += "<tr>"
            "<td " + tkL + ">" + tr("Apnea Index") + "</td>"
            "<td " + tkM + ">" + tr("Central: ") + "<b style='color:#0f172a;'>" + QString::number(CentralAI,'f',1) + "</b></td>"
            "<td " + tkM + ">" + tr("Obstructive: ") + "<b style='color:#0f172a;'>" + QString::number(ObstructiveAI,'f',1) + "</b></td>"
            "<td " + tkR + ">" + tr("Unknown: ") + "<b style='color:#0f172a;'>" + QString::number(UnknownAI,'f',1) + "</b></td>"
            "</tr>";
    html += "<tr>"
            "<td colspan='3' " + tkL + ">" + tr("Respiratory Effort-Related Arousal Index (RERA)") + "</td>"
            "<td " + tkR + "><b style='color:#0f172a;'>" + QString::number(ReraIndex,'f',1) + "</b></td>"
            "</tr>";
    html += "<tr>"
            "<td colspan='3' " + tkL + ">" + tr("Respiratory Disturbance Index (RDI)") + "</td>"
            "<td " + tkR + "><b style='color:#0f172a;'>" + QString::number(RDI,'f',2) + "</b></td>"
            "</tr>";
    html += "<tr>"
            "<td colspan='3' " + tkL + ">" + tr("95% Flow Limitation") + "</td>"
            "<td " + tkR + "><b style='color:#0f172a;'>" + QString::number(p95FlowLimit,'f',2) + "</b></td>"
            "</tr>";
    if (p_profile->hasChannel(CPAP_CSR)) {
        int csrAvgMins = qRound(avgCsrMinutesPerNight);
        html += "<tr>"
                "<td colspan='3' " + tkL + ">" + tr("Cheyne-Stokes respiration (average duration per night)") + "</td>"
                "<td " + tkR + "><b style='color:#0f172a;'>" + QString::number(csrAvgMins) + tr(" min (%1%)").arg(QString::number(avgCsrPctPerNight,'f',0)) + "</b></td>"
                "</tr>";
    }
    html += "</table>";

    // Page 1 Footer
    html += "<div style='font-size: 8.5pt; color: #94a3b8; margin-top: 8pt; border-top: 1px solid #f1f5f9; padding-top: 4px;'>"
            + tr("Printed on %1 - OSCAR Sleep Insights Report - OSCAR %2").arg(QDateTime::currentDateTime().toString("MMM d, yyyy")).arg(getVersion().minimalString())
            + "</div>";
 
    // ==========================================
    // PAGE 2: TREND CHARTS
    // ==========================================
    html += "<div style='page-break-before: always;'></div>";
 
    // Branded Header Page 2
    html += makePageHeaderHtml(tr("Therapy Trends"), patientNameStr, pageSubInfo, logoBase64);

    // Visual Trends Section — page 2
    html += "<div style='font-size:11pt; font-weight:700; color:#1e1b4b; border-bottom:2px solid #e2e8f0; padding-bottom:3px; margin-top:4pt; margin-bottom:6pt;'>" + tr("Usage & Efficacy Trend Charts").toUpper() + "</div>";

    // Usage chart
    {
        double pctNotUsed = totalDays > 0 ? (double)daysNotUsed / totalDays * 100.0 : 0.0;
        double pctNoData  = totalDays > 0 ? (double)daysNoData  / totalDays * 100.0 : 0.0;
        QString usageStats = makeStatsRows(QList<StatsRow>{
            { tr("Usage days"),
              QString("%1/%2 (%3%)").arg(daysUsed).arg(totalDays).arg(QString::number(pctUsed,'f',0)),
              "", false, true },
            { tr(">= %1 hr days").arg(QString::number(complianceThreshold,'f',0)),
              QString("%1 (%2%)").arg(compliantDays).arg(QString::number(pctCompliant,'f',0)),
              "#22c55e", true, true },
            { tr("&lt; %1 hr days").arg(QString::number(complianceThreshold,'f',0)),
              QString("%1 (%2%)").arg(nonCompliantUsedDays).arg(QString::number(pctNonCompliant,'f',0)),
              "#ef4444", true, true },
            { tr("Days not used"),
              QString("%1 (%2%)").arg(daysNotUsed).arg(QString::number(pctNotUsed,'f',0)),
              "", false, true },
            { tr("Days no data"),
              QString("%1 (%2%)").arg(daysNoData).arg(QString::number(pctNoData,'f',0)),
              "", false, true },
            { tr("Used/day (avg.)"),
              QString("%1 hrs.").arg(avgHoursUsed, 0, 'f', 1),
              "", false, true },
        });
        html += makeChartBlockHtml(usageChartUrl, usageStats, displayW, displayH, STATS_COL_W, chartRegistry);
    }

    // AHI chart
    {
        QString ahiStats = makeAhiStatsHtml(AHI, HI, AI, CentralAI, ObstructiveAI, UnknownAI, ReraIndex, avgCsrPctPerNight);
        html += makeChartBlockHtml(ahiChartUrl, ahiStats, displayW, displayH, STATS_COL_W, chartRegistry, 0);
    }

    // Leak chart
    if (leakChan != NoChannel) {
        // medianLeak / p95Leak / maxLeak computed as per-night averages (maxLeak = 99.5th percentile avg)
        QList<StatsRow> leakRows;
        if (!isTotalLeak)
            leakRows.append({ tr("Set threshold"), QString::number(leakRedline, 'f', 1) + tr(" L/min"), "", false, true });
        leakRows.append({ tr("95th % (avg)"), QString::number(p95Leak,    'f', 1) + tr(" L/min"), "#f97316", true,  true });
        leakRows.append({ tr("Median (avg)"), QString::number(medianLeak, 'f', 1) + tr(" L/min"), "#f97316", true,  true });
        leakRows.append({ tr("Max (avg)"),    QString::number(maxLeak,    'f', 1) + tr(" L/min"), "",        false, true });
        QString leakStats = makeStatsRows(leakRows);
        html += makeChartBlockHtml(leakChartUrl, leakStats, displayW, displayH, STATS_COL_W, chartRegistry);
        if (isTotalLeak) {
            html += "<div style='font-size:8pt; color:#64748b; margin-top:-4pt; margin-bottom:8pt;'>"
                    + tr("Total Leak includes intentional mask exhaust flow; values of 30-50 L/min are typically normal. "
                         "No unintentional-leak redline is shown because this device does not report seal leak separately.")
                    + "</div>";
        }
    }

    // Pressure chart
    if (p_profile->hasChannel(CPAP_Pressure)) {
        QString pressStats = makePressureStatsHtml(lastDay, avgPressure, p95Pressure, maxPressure);
        html += makeChartBlockHtml(pressChartUrl, pressStats, displayW, displayH, STATS_COL_W, chartRegistry);
    }

    // Channel trend charts (Minute Vent, Resp Rate, Tidal Volume, etc.) — own page(s)
    if (!chanCharts.isEmpty()) {
        html += "<div style='page-break-before: always;'></div>";
        html += makePageHeaderHtml(tr("Channel Trends"), patientNameStr, pageSubInfo, logoBase64);
        html += "<div style='font-size:11pt; font-weight:700; color:#1e1b4b; border-bottom:2px solid #e2e8f0; padding-bottom:3px; margin-top:4pt; margin-bottom:6pt;'>" + tr("Channel Trend Charts").toUpper() + "</div>";
        for (int i = 0; i < chanCharts.size(); ++i) {
            if (i > 0 && i % 3 == 0) {
                html += "<div style='page-break-before: always;'></div>";
                html += makePageHeaderHtml(tr("Channel Trends"), patientNameStr, pageSubInfo, logoBase64);
                html += "<div style='font-size:11pt; font-weight:700; color:#1e1b4b; border-bottom:2px solid #e2e8f0; padding-bottom:3px; margin-top:4pt; margin-bottom:6pt;'>" + tr("Channel Trend Charts").toUpper() + "</div>";
            }
            const auto& cc = chanCharts[i];
            html += makeChartBlockHtml(cc.base64Url, cc.statsHtml, displayW, displayH, STATS_COL_W, chartRegistry);
        }
    }
 
    // Page 2 Footer
    html += "<div style='font-size: 8.5pt; color: #94a3b8; margin-top: 10pt; border-top: 1px solid #f1f5f9; padding-top: 4px;'>"
            + tr("Printed on %1 - OSCAR Sleep Insights Report - OSCAR %2").arg(QDateTime::currentDateTime().toString("MMM d, yyyy")).arg(getVersion().minimalString())
            + "</div>";
 
    // ==========================================
    // PAGE 3: OXIMETRY & SLEEP STAGE LOG
    // ==========================================
    int stageNights = 0;
    double avgBedtimeHrs = 0.0, avgWakeupHrs = 0.0, avgWakeMin = 0.0, avgREMMin = 0.0, avgLightMin = 0.0, avgDeepMin = 0.0, avgLatencyMin = 0.0, avgTSTMin = 0.0;
    QString bedtimeStr, wakeupStr;
    auto minsToHM = [](double mins) -> QString {
        int h = static_cast<int>(mins) / 60;
        int m = static_cast<int>(mins) % 60;
        return QString("%1h %2m").arg(h).arg(m, 2, 10, QChar('0'));
    };

    {
        QList<Day*> stageDays = p_profile->getDays(MT_SLEEPSTAGE, start, end);
        double sumWake = 0, sumREM = 0, sumLight = 0, sumDeep = 0, sumAwakenings = 0, sumLatency = 0, sumBedtimeHrs = 0, sumWakeupHrs = 0;

        for (Day* sday : stageDays) {
            if (!sday) continue;
            QList<Session*> ssessions = sday->getSessions(MT_SLEEPSTAGE, false);
            if (ssessions.isEmpty()) continue;

            qint64 bedtimeMs = 0, wakeupMs = 0;
            double dayWake = 0, dayREM = 0, dayLight = 0, dayDeep = 0, dayLatency = 0;
            bool hasData = false;

            for (Session* sess : ssessions) {
                if (!sess->enabled()) continue;
                qint64 sf = sess->realFirst();
                if (sf > 0 && (bedtimeMs == 0 || sf < bedtimeMs)) bedtimeMs = sf;
                qint64 sl = sess->realLast();
                if (sl > 0 && (wakeupMs == 0 || sl > wakeupMs)) wakeupMs = sl;
                auto addSetting = [&](ChannelID ch, double &out) {
                    auto it = sess->settings.find(ch);
                    if (it != sess->settings.end()) out += it.value().toDouble();
                };
                addSetting(ZEO_TimeInWake,  dayWake);
                addSetting(ZEO_TimeInREM,   dayREM);
                addSetting(ZEO_TimeInLight, dayLight);
                addSetting(ZEO_TimeInDeep,  dayDeep);
                addSetting(ZEO_TimeToZ,     dayLatency);
                hasData = true;
            }

            if (!hasData || bedtimeMs == 0 || wakeupMs == 0) continue;

            QDateTime bdtDT = QDateTime::fromMSecsSinceEpoch(bedtimeMs, QTimeZone::systemTimeZone());
            double bedHr = bdtDT.time().hour() + bdtDT.time().minute() / 60.0 + bdtDT.time().second() / 3600.0;
            if (bedHr < 12.0) bedHr += 24.0;
            QDateTime wktDT = QDateTime::fromMSecsSinceEpoch(wakeupMs, QTimeZone::systemTimeZone());
            double wakeHr = wktDT.time().hour() + wktDT.time().minute() / 60.0 + wktDT.time().second() / 3600.0;
            if (wakeHr < 12.0) wakeHr += 24.0;

            sumBedtimeHrs += bedHr; sumWakeupHrs += wakeHr; sumWake += dayWake; sumREM += dayREM; sumLight += dayLight; sumDeep += dayDeep; sumLatency += dayLatency;
            stageNights++;
        }

        if (stageNights > 0) {
            avgBedtimeHrs = sumBedtimeHrs / stageNights; avgWakeupHrs = sumWakeupHrs / stageNights; avgWakeMin = sumWake / stageNights; avgREMMin = sumREM / stageNights; avgLightMin = sumLight / stageNights; avgDeepMin = sumDeep / stageNights; avgLatencyMin = sumLatency / stageNights;
            if (avgBedtimeHrs >= 24.0) avgBedtimeHrs -= 24.0;
            int bedHour = static_cast<int>(avgBedtimeHrs) % 24, bedMin = static_cast<int>((avgBedtimeHrs - static_cast<int>(avgBedtimeHrs)) * 60.0);
            bedtimeStr = QString("%1:%2 %3").arg(bedHour % 12 == 0 ? 12 : bedHour % 12).arg(bedMin, 2, 10, QChar('0')).arg(bedHour >= 12 ? tr("pm") : tr("am"));
            if (avgWakeupHrs >= 24.0) avgWakeupHrs -= 24.0;
            int wakeHour = static_cast<int>(avgWakeupHrs) % 24, wakeMin = static_cast<int>((avgWakeupHrs - static_cast<int>(avgWakeupHrs)) * 60.0);
            wakeupStr = QString("%1:%2 %3").arg(wakeHour % 12 == 0 ? 12 : wakeHour % 12).arg(wakeMin, 2, 10, QChar('0')).arg(wakeHour >= 12 ? tr("pm") : tr("am"));
            avgTSTMin = avgREMMin + avgLightMin + avgDeepMin;
        }
    }

    if (usedDaysOxi > 0 || stageNights > 0) {
        html += "<div style='page-break-before: always;'></div>";
        QString title = (usedDaysOxi > 0 && stageNights > 0) ? tr("Oximetry &amp; Sleep Stage Statistics") : (usedDaysOxi > 0 ? tr("Oximeter Statistics") : tr("Sleep Stage Statistics"));
        html += makePageHeaderHtml(title, patientNameStr, pageSubInfo, logoBase64);

        if (usedDaysOxi > 0) {
            // Sum actual session durations from the oximetry source, filtered to days
            // in oxiDaysMap (which only contains days with recorded SpO2 data).
            // Using session->hours() rather than timeAboveThreshold(OXI_SPO2, 0.0)
            // avoids a unit bug in the timesummary fast path: waveform channels
            // (e.g. Viatom stores OXI_SPO2 as EVL_Waveform with 1000 ms rate)
            // accumulated count*rate_ms in timesummary, but the fast path divided
            // by 60 treating the values as seconds, giving a result 1000x too large.
            double oximeterHours = 0.0;
            for (Day* day : oxiDaysMap) {
                for (Session* sess : day->getSessions(oxiSourceType)) {
                    oximeterHours += sess->hours();
                }
            }
            double spo2DropIndex = oximeterHours > 0 ? p_profile->calcCount(OXI_SPO2Drop, oxiSourceType, start, end) / oximeterHours : 0.0;
            double pctTimeSpo2Drop = oximeterHours > 0 ? (100.0 / oximeterHours) * (p_profile->calcSum(OXI_SPO2Drop, oxiSourceType, start, end) / 3600.0) : 0.0;
            double pulseChangeIndex = oximeterHours > 0 ? p_profile->calcCount(OXI_PulseChange, oxiSourceType, start, end) / oximeterHours : 0.0;
            html += "<div style='font-size: 11pt; font-weight: 700; color: #0f172a; margin-bottom: 2px;'>" + tr("Oximeter Statistics").toUpper() + "</div>";
            Machine* oxiMach = p_profile->GetMachine(MT_OXIMETER); if (!oxiMach && oxiSourceType != MT_OXIMETER) oxiMach = p_profile->GetMachine(oxiSourceType);
            QString oxiDevice = oxiMach ? (oxiMach->brand() + " " + oxiMach->model() + (oxiMach->serial().isEmpty() ? "" : " (" + oxiMach->serial() + ")")).trimmed() : tr("Unknown oximeter");
            html += "<div style='font-size: 8.5pt; color: #64748b; margin-bottom: 4px;'>" + tr("Device") + ": <b style='color:#0f172a;'>" + oxiDevice.toHtmlEscaped() + "</b></div>";
            html += "<table width='100%' cellpadding='2' cellspacing='0' style='border: none; border-collapse: collapse; font-size: 8.5pt; line-height: 1.1; margin-bottom: 6px; page-break-inside: avoid;'>";
            html += "<tr style='background-color: #454545; color: #ffffff;'><th align='left' style='padding: 1.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>Statistics Group</th><th align='left' style='padding: 1.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>Metric Description</th><th align='right' style='padding: 1.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>Selected Period Value</th></tr>";
            html += "<tr><td rowspan='3' valign='top' style='font-weight: bold; color: #454545; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px; background-color: #f8fafc;'>Oximeter Usage</td><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>Total Days / Used Days</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + QString::number(totalDays) + tr(" days / %1 days").arg(usedDaysOxi) + "</td></tr>";
            html += "<tr><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>Days Not Used</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + QString::number(totalDays - usedDaysOxi) + " days" + "</td></tr>";
            html += "<tr><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>Average Hours per Night</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + formatHoursMinutesShort(oximeterHours / usedDaysOxi) + "</td></tr>";
            html += "<tr><td rowspan='4' valign='top' style='font-weight: bold; color: #454545; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px; background-color: #f8fafc;'>Oxygen Saturation</td><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>Average SpO2</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + QString::number(avgSpO2Period, 'f', 2) + "%" + "</td></tr>";
            html += "<tr><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>Minimum SpO2</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + (minSpO2 > 0.0 ? QString::number(minSpO2, 'f', 0) + "%" : tr("N/A")) + "</td></tr>";
            html += "<tr><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>SpO2 Desaturation Drop Index</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + QString::number(spo2DropIndex, 'f', 2) + " / hr" + "</td></tr>";
            html += "<tr><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>% of Time in SpO2 Drop</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + QString::number(pctTimeSpo2Drop, 'f', 2) + "%" + "</td></tr>";
            html += "<tr><td rowspan='3' valign='top' style='font-weight: bold; color: #454545; padding: 1.5px 6px; background-color: #f8fafc;'>Pulse Rate</td><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>Average Pulse Rate</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + QString::number(avgPulsePeriod, 'f', 2) + " bpm" + "</td></tr>";
            html += "<tr><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>Minimum / Maximum Pulse Rate</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + QString("%1 / %2 bpm").arg(QString::number(minPulsePeriod, 'f', 1)).arg(QString::number(maxPulsePeriod, 'f', 1)) + "</td></tr>";
            html += "<tr><td style='color: #475569; padding: 1.5px 6px;'>Pulse Change Index</td><td align='right' style='font-weight: bold; color: #0f172a; padding: 1.5px 6px;'>" + QString::number(pulseChangeIndex, 'f', 2) + " / hr" + "</td></tr></table>";
        }
        if (usedDaysOxi > 0 && stageNights > 0) html += "<div style='line-height: 2pt;'>&nbsp;</div>";
        if (stageNights > 0) {
            double totalMin = avgWakeMin + avgTSTMin;
            auto pct = [&](double part) -> QString { return totalMin <= 0 ? tr("N/A") : QString::number(100.0 * part / totalMin, 'f', 1) + "%"; };
            html += "<div style='font-size: 11pt; font-weight: 700; color: #0f172a; margin-bottom: 2px;'>" + tr("Sleep Stage Statistics").toUpper() + "</div>";
            html += "<div style='font-size: 8.5pt; color: #64748b; margin-bottom: 4px;'>" + tr("Averaged per night across %1 night(s) of sleep stage data").arg(stageNights) + "</div>";
            html += "<table width='100%' cellpadding='2' cellspacing='0' style='border: none; border-collapse: collapse; font-size: 8.5pt; line-height: 1.1; margin-bottom: 6px; page-break-inside: avoid;'>";
            html += "<tr style='background-color: #454545; color: #ffffff;'><th align='left' style='padding: 1.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>" + tr("Statistics Group") + "</th><th align='left' style='padding: 1.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>" + tr("Metric Description") + "</th><th align='right' style='padding: 1.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>" + tr("Avg per Night") + "</th></tr>";
            html += "<tr><td rowspan='4' valign='top' style='font-weight: bold; color: #454545; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px; background-color: #f8fafc;'>" + tr("Sleep Timing") + "</td><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + tr("Average Bedtime") + "</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + bedtimeStr + "</td></tr>";
            html += "<tr><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + tr("Average Sleep Latency") + "</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + minsToHM(avgLatencyMin) + "</td></tr>";
            html += "<tr><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + tr("Average Wake Time") + "</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + wakeupStr + "</td></tr>";
            html += "<tr><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + tr("Average Total Sleep Time") + "</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + minsToHM(avgTSTMin) + "</td></tr>";
            html += "<tr><td rowspan='4' valign='top' style='font-weight: bold; color: #454545; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px; background-color: #f8fafc;'>" + tr("Stage % of Night") + "</td><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + tr("Awake") + "</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + pct(avgWakeMin) + " (" + minsToHM(avgWakeMin) + ")</td></tr>";
            html += "<tr><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + tr("REM Sleep") + "</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + pct(avgREMMin) + " (" + minsToHM(avgREMMin) + ")</td></tr>";
            html += "<tr><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + tr("Light Sleep") + "</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + pct(avgLightMin) + " (" + minsToHM(avgLightMin) + ")</td></tr>";
            html += "<tr><td style='color: #475569; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + tr("Deep Sleep") + "</td><td align='right' style='font-weight: bold; color: #0f172a; border-bottom: 1px solid #e8edf2; padding: 1.5px 6px;'>" + pct(avgDeepMin) + " (" + minsToHM(avgDeepMin) + ")</td></tr></table>";
        }
        html += "<div style='font-size: 8.5pt; color: #94a3b8; margin-top: 8pt; border-top: 1px solid #f1f5f9; padding-top: 4px;'>" + tr("Printed on %1 - OSCAR Sleep Insights Report - OSCAR %2").arg(QDateTime::currentDateTime().toString("MMM d, yyyy")).arg(getVersion().minimalString()) + "</div>";
    }

    // ==========================================
    // PAGE 4: DIAGNOSTICS & PROVIDER
    // ==========================================
    html += "<div style='page-break-before: always;'></div>";
 
    // Branded Header Diagnostics Page
    html += makePageHeaderHtml(tr("Therapy Diagnostics"), patientNameStr, pageSubInfo, logoBase64);
 

 
    // Detailed Statistics Table
    if (!selectedChans.isEmpty()) {
        html += "<div style='font-size: 11pt; font-weight: 700; color: #1e1b4b; margin-bottom: 4px;'>" + tr("Detailed Channel Statistics").toUpper() + "</div>";
        html += "<table width='100%' cellpadding='0' cellspacing='0' style='border: none; border-collapse: collapse; margin-bottom: 4pt;'>";
        html += "<tr style='background-color: #454545; color: #ffffff;'>";
        html += "<th width='30%' align='left' style='font-size: 9pt; font-weight: 700; color: #ffffff; padding: 2px 5px;'>Therapy Channel</th>";
        html += "<th width='10%' align='left' style='font-size: 9pt; font-weight: 700; color: #ffffff; padding: 2px 5px;'>Unit</th>";
        html += "<th width='15%' align='right' style='font-size: 9pt; font-weight: 700; color: #ffffff; padding: 2px 5px;'>Minimum</th>";
        html += "<th width='15%' align='right' style='font-size: 9pt; font-weight: 700; color: #ffffff; padding: 2px 5px;'>Median</th>";
        html += "<th width='15%' align='right' style='font-size: 9pt; font-weight: 700; color: #ffffff; padding: 2px 5px;'>95th %</th>";
        html += "<th width='15%' align='right' style='font-size: 9pt; font-weight: 700; color: #ffffff; padding: 2px 5px;'>Maximum</th>";
        html += "</tr>";
 
        int chRowIdx = 0;
        for (const auto& ch : selectedChans) {
            ChannelID chId = static_cast<ChannelID>(ch.id);
            // EPAP is only a distinct clinical pressure in BiLevel/ASV modes.
            if (chId == CPAP_EPAP && (p1Mode == MODE_CPAP || p1Mode == MODE_APAP)) continue;

            // Oximetry channels: exclude sensor dropout artifacts from the minimum
            // (SpO2 <= 50%, pulse <= 30 bpm) so the table never shows 0.0.
            MachineType chMt = (MachineType)ch.type;
            double cMin;
            if (ch.id == OXI_SPO2)       cMin = minSpO2;          // period low, computed once above
            else if (ch.id == OXI_Pulse) cMin = minPulsePeriod;   // period low, computed once above
            else                          cMin = p_profile->calcMin(ch.id, chMt, start, end);
            const QMap<QDate, Day*>& chMap = (chMt == MT_OXIMETER || chMt == MT_UNKNOWN) ? oxiDaysMap : daysMap;
            double cMed = calcDayAvgPercentile(ch.id, 0.5,  chMap, chMt);
            double c95  = calcDayAvgPercentile(ch.id, 0.95, chMap, chMt);
            double cMax = calcDayAvgPercentile(ch.id, 0.995, chMap, chMt);  // P99.5 artifact guard
            QString rowBg = (chRowIdx++ % 2 == 0) ? "#ffffff" : "#f8fafc";
 
            QString chUnit = unitForChannel(static_cast<ChannelID>(ch.id));
            html += "<tr style='background-color: " + rowBg + ";'>";
            html += "<td align='left' style='padding: 1.5px 5px; font-weight: 600; color: #1e293b; border-bottom: 1px solid #f1f5f9; font-size: 9pt;'>" + ch.label + "</td>";
            html += "<td align='left' style='padding: 1.5px 5px; color: #64748b; border-bottom: 1px solid #f1f5f9; font-size: 8.5pt;'>" + chUnit + "</td>";
            html += "<td align='right' style='padding: 1.5px 5px; color: #334155; border-bottom: 1px solid #f1f5f9; font-size: 9pt;'>" + QString::number(cMin, 'f', 2) + "</td>";
            html += "<td align='right' style='padding: 1.5px 5px; color: #334155; border-bottom: 1px solid #f1f5f9; font-size: 9pt;'>" + QString::number(cMed, 'f', 2) + "</td>";
            html += "<td align='right' style='padding: 1.5px 5px; color: #334155; border-bottom: 1px solid #f1f5f9; font-size: 9pt;'>" + QString::number(c95, 'f', 2) + "</td>";
            html += "<td align='right' style='padding: 1.5px 5px; color: #334155; border-bottom: 1px solid #f1f5f9; font-size: 9pt;'>" + QString::number(cMax, 'f', 2) + "</td>";
            html += "</tr>";
        }
        
        html += "</table>";
        html += "<div style='line-height: 4pt;'>&nbsp;</div>";
    }  // end channel stats guard

    // Additional Therapy Indices — always shown regardless of channel selection
    {
        html += "<div style='font-size: 11pt; font-weight: 700; color: #1e1b4b; margin-bottom: 4px;'>" + tr("Additional Therapy Indices").toUpper() + "</div>";
        html += "<table width='100%' cellpadding='3' cellspacing='0' style='border: none; border-collapse: collapse; margin-bottom: 4pt;'>";
        html += "<tr>";
        html += "<td style='font-weight: 600; font-size: 9.5pt; padding: 2.5px 8px; border-bottom: 1px solid #f1f5f9;'>Total Cumulative Time in Apnea:</td>";
        html += "<td align='right' style='font-weight: 700; color: #1e293b; padding: 2.5px 8px; border-bottom: 1px solid #f1f5f9; font-size: 9.5pt;'>" + timeInApneaStr + "</td>";
        html += "</tr>";
        
        if (p_profile->hasChannel(CPAP_CSR)) {
            html += "<tr>";
            html += "<td style='font-weight: 600; font-size: 9.5pt; padding: 2.5px 8px; border-bottom: 1px solid #f1f5f9;'>Total Time in Cheyne-Stokes Respiration (CSR):</td>";
            html += "<td align='right' style='font-weight: 700; color: #1e293b; padding: 2.5px 8px; border-bottom: 1px solid #f1f5f9; font-size: 9.5pt;'>" + timeInCsrStr + "</td>";
            html += "</tr>";
        }
 
        // The leak-redline statistic is only meaningful for unintentional leak
        // (ResMed). For total leak there is no 24 L/min seal-leak threshold.
        if (leakChan != NoChannel && !isTotalLeak) {
            html += "<tr>";
            html += "<td style='font-weight: 600; font-size: 9.5pt; padding: 2.5px 8px; border-bottom: 1px solid #f1f5f9;'>" + tr("Therapy Time Over Unintentional Leak Redline (") + QString::number(leakRedline,'f',0) + tr(" L/min):") + "</td>";
            html += "<td align='right' style='font-weight: 700; color: #ef4444; padding: 2.5px 8px; border-bottom: 1px solid #f1f5f9; font-size: 9.5pt;'>" + timeAboveRedlineStr + " (" + QString::number(pctOverRedline, 'f', 3) + " %)</td>";
            html += "</tr>";
        }
        // Total period therapy hours
        html += "<tr>";
        html += "<td style='font-weight: 600; font-size: 9.5pt; padding: 2.5px 8px; border-bottom: 1px solid #f1f5f9;'>Total Therapy Hours (Period):</td>";
        html += "<td align='right' style='font-weight: 700; color: #1e293b; padding: 2.5px 8px; border-bottom: 1px solid #f1f5f9; font-size: 9.5pt;'>"
                + formatHoursMinutesShort(totalHours) + "</td>";
        html += "</tr>";
        // Flow Limitation Index
        if (p_profile->hasChannel(CPAP_FlowLimit)) {
            html += "<tr>";
            html += "<td style='font-weight: 600; font-size: 9.5pt; padding: 2.5px 8px; border-bottom: 1px solid #f1f5f9;'>Flow Limitation Index:</td>";
            html += "<td align='right' style='font-weight: 700; color: #1e293b; padding: 2.5px 8px; border-bottom: 1px solid #f1f5f9; font-size: 9.5pt;'>"
                    + QString::number(flIndex, 'f', 2) + tr(" /hr") + "</td>";
            html += "</tr>";
        }
        // SpO2 stats (if oximeter data available)
        if (usedDaysOxi > 0 && p_profile->hasChannel(OXI_SPO2)) {
            html += "<tr>";
            html += "<td style='font-weight: 600; font-size: 9.5pt; padding: 2.5px 8px; border-bottom: 1px solid #f1f5f9;'>Minimum SpO2 (period low):</td>";
            QColor spo2Color = (minSpO2Set && minSpO2 < 90.0) ? QColor("#ef4444") : QColor("#1e293b");
            html += "<td align='right' style='font-weight: 700; color:" + spo2Color.name() + "; padding: 2.5px 8px; border-bottom: 1px solid #f1f5f9; font-size: 9.5pt;'>"
                    + (minSpO2Set ? QString::number(minSpO2, 'f', 0) + "%" : tr("N/A")) + "</td>";
            html += "</tr>";
            html += "<tr>";
            html += "<td style='font-weight: 600; font-size: 9.5pt; padding: 2.5px 8px;'>Time with SpO2 &lt; 90%:</td>";
            // Flag red when T90 exceeds 2% of recorded oximetry time — the clinical
            // CT90 cutoff that warrants evaluation. An absolute seconds threshold
            // misjudged short vs long recordings (60s means more on a 4h night than
            // an 8h one). Falls back to the absolute guard if recording time is unknown.
            double t90Pct = spo2RecordingSecs > 0.0 ? (t90Secs / spo2RecordingSecs) * 100.0 : 0.0;
            QColor t90Color = (t90Pct > 2.0) ? QColor("#ef4444") : QColor("#1e293b");
            html += "<td align='right' style='font-weight: 700; color:" + t90Color.name() + "; padding: 2.5px 8px; font-size: 9.5pt;'>"
                    + t90Str + "</td>";
            html += "</tr>";
        }
        html += "</table>";
    }  // end Additional Therapy Indices block

    // Page 3 (or 4) Footer
    html += "<div style='font-size: 8.5pt; color: #94a3b8; margin-top: 8pt; border-top: 1px solid #f1f5f9; padding-top: 4px;'>"
            + tr("Printed on %1 - OSCAR Sleep Insights Report - OSCAR %2").arg(QDateTime::currentDateTime().toString("MMM d, yyyy")).arg(getVersion().minimalString())
            + "</div>";
    
    // PAGE LAST: Therapy History
    if (!settingsBlocks.isEmpty()) {
        html += "<div style='page-break-before: always;'></div>";
        html += makePageHeaderHtml(tr("Therapy History"), patientNameStr, pageSubInfo, logoBase64);

        html += "<div style='font-size: 11pt; font-weight: 700; color: #0f172a; margin-bottom: 6px;'>" + tr("Changes to Device Settings").toUpper() + "</div>";
        // No page-break-inside:avoid here: this log can grow long, and forcing the
        // whole table onto one page pushes the footer onto a page of its own.
        // Letting it flow keeps the footer attached to the last row.
        html += "<table width='100%' cellpadding='3' cellspacing='0' style='border: none; border-collapse: collapse; font-size: 9.5pt; line-height: 1.2; margin-bottom: 4pt;'>";
        html += "<tr style='background-color: #454545; color: #ffffff;'>";
        html += "<th align='left'  style='padding: 2.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>First</th>";
        html += "<th align='left'  style='padding: 2.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>Last</th>";
        html += "<th align='right' style='padding: 2.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>Days</th>";
        html += "<th align='right' style='padding: 2.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>AHI</th>";
        html += "<th align='left'  style='padding: 2.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>Pressure Relief</th>";
        html += "<th align='left'  style='padding: 2.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>Mode</th>";
        html += "<th align='left'  style='padding: 2.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>Pressure Settings</th>";
        if (usedDaysOxi > 0)
            html += "<th align='right' style='padding: 2.5px 6px; color: #ffffff; font-weight: bold; border-bottom: 1px solid #f1f5f9;'>Min SpO2</th>";
        html += "</tr>";

        int blockCount = settingsBlocks.size();
        for (int bi = 0; bi < blockCount; bi++) {
            const auto& block = settingsBlocks[bi];
            bool isCurrent = (bi == blockCount - 1);
            double blockAhi = block.hours > 0 ? block.ahiEvents / block.hours : 0.0;
            QString rowBg   = isCurrent ? "#d1fae5" : ((bi % 2 == 0) ? "#ffffff" : "#f8fafc");
            QString textCol = isCurrent ? "#059669" : "#0f172a";
            QString weightAhi = isCurrent ? "700" : "600";
            // AHI traffic-light: green <5, amber 5-15, red >=15.
            // Darker green (#15803d) so it stays legible on the pale-green current row (#d1fae5).
            QString ahiCol = (blockAhi >= 15.0) ? "#ef4444" : (blockAhi >= 5.0) ? "#f59e0b" : "#15803d";

            html += "<tr style='background-color: " + rowBg + ";'>";
            html += "<td style='border-bottom: 1px solid #f1f5f9; padding: 2.5px 6px; color: " + textCol + ";'>" + block.start.toString("MMM d yyyy") + "</td>";
            html += "<td style='border-bottom: 1px solid #f1f5f9; padding: 2.5px 6px; color: " + textCol + ";'>" + block.end.toString("MMM d yyyy") + "</td>";
            html += "<td align='right' style='border-bottom: 1px solid #f1f5f9; padding: 2.5px 6px; color: " + textCol + ";'>" + QString::number(block.days) + "</td>";
            html += "<td align='right' style='border-bottom: 1px solid #f1f5f9; padding: 2.5px 6px; white-space: nowrap; color: " + ahiCol + "; font-weight: " + weightAhi + ";'>" + QString::number(blockAhi, 'f', 2) + "</td>";
            html += "<td style='border-bottom: 1px solid #f1f5f9; padding: 2.5px 6px; color: " + textCol + ";'>" + block.relief.toHtmlEscaped() + "</td>";
            html += "<td style='border-bottom: 1px solid #f1f5f9; padding: 2.5px 6px; color: " + textCol + ";'>" + block.mode.toHtmlEscaped() + "</td>";
            html += "<td style='border-bottom: 1px solid #f1f5f9; padding: 2.5px 6px; color: " + textCol + "; font-weight: 600;'>" + block.pressure.toHtmlEscaped() + "</td>";
            if (usedDaysOxi > 0) {
                QString spo2Str = (block.minSpo2 >= 0.0) ? QString::number(block.minSpo2, 'f', 0) + "%" : tr("N/A");
                html += "<td align='right' style='border-bottom: 1px solid #f1f5f9; padding: 2.5px 6px; color: " + textCol + ";'>" + spo2Str + "</td>";
            }
            html += "</tr>";
        }
        html += "</table>";

        html += "<div style='font-size: 8.5pt; color: #94a3b8; margin-top: 3pt; border-top: 1px solid #f1f5f9; padding-top: 4px;'>"
                + tr("Printed on %1 - OSCAR Sleep Insights Report - OSCAR %2").arg(QDateTime::currentDateTime().toString("MMM d, yyyy")).arg(getVersion().minimalString())
                + "</div>";
    }

    html += "</body></html>";

    // Print to PDF
    QTextDocument doc;
    doc.setDocumentMargin(4.0);   // Pin to DOC_MARGIN used for displayW above.
    doc.setHtml(html);
    // Swap chart placeholders for true-vector chart objects, then print. The
    // handler must outlive doc.print() — it owns the SVG bytes it replays.
    ChartObjectHandler chartHandler(chartRegistry);
    embedVectorCharts(doc, &chartHandler);
    doc.print(&printer);

    mainwin->Notify(tr("Compliance report successfully saved to:\n\n%1").arg(savePath), tr("PDF Export Success"));
}
