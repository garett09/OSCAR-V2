# Compliance Report Chart Upgrades Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add clinical reference bands, a central-apnea split, prescribed pressure bounds, inline legends, an Overnight Oxygen Summary, and a worst-night SpO2 trace to the OSCAR compliance report charts — keeping the AirView-faithful bar style, full vector quality, and the 7-page budget.

**Architecture:** All charts are QPainter→SVG embedded as true PDF vectors via `ChartObjectHandler` (compliancereport.cpp). We add two pure, unit-tested helper functions (worst-night selection, SpO2 zone percentages) declared in the header, two new QPainter draw helpers (reference bands, inline legend) in the anonymous namespace, then thread them through the existing render functions and add two new oximetry blocks placed in existing whitespace.

**Tech Stack:** C++17, Qt 5 (QtGui/QtSvg, QPainter, QSvgGenerator/QSvgRenderer), qmake, QtTest (AutoTest harness).

**Spec:** `docs/superpowers/specs/2026-06-17-compliance-report-chart-upgrades-design.md`

**Build commands (macOS):**
- App build: `qmake OSCAR_QT.pro && make -j$(sysctl -n hw.ncpu)`
- Test build/run: `qmake OSCAR_QT.pro CONFIG+=test && make -j$(sysctl -n hw.ncpu)` then run the produced `OSCAR` binary — in `UNITTEST_MODE` it executes all AutoTests and prints `PASS`/`FAIL` per slot.
- Visual check: launch the normal (non-test) `OSCAR` build, open a profile, run **Export → Compliance Report** (the dialog from `ComplianceReportDialog`), save the PDF, inspect the named page.

---

## File Structure

- **Modify** `oscar/compliancereport.h` — declare 2 pure helpers + their small structs (outside any class, so the test file can call them).
- **Modify** `oscar/compliancereport.cpp` — define the 2 pure helpers; add `drawReferenceBands`/`drawLegend`; edit `renderAhiChart`, `renderLeakChart`, `renderPressureChart`, `renderChannelBarChart`; add `renderLowestOxygenNight`; add the Overnight Oxygen Summary HTML; place the two new blocks.
- **Create** `oscar/tests/compliancereporttests.h` / `.cpp` — QtTest unit tests for the 2 pure helpers.
- **Modify** `oscar/oscar.pro` — add the new test sources/headers under the `test {` block.

---

## Task 1: Test scaffold + pure helper `selectWorstNightIndex`

**Files:**
- Modify: `oscar/compliancereport.h` (add struct + declaration near top, after includes, before `struct SelectedChannel`)
- Modify: `oscar/compliancereport.cpp` (add definition after the anonymous namespace closes, i.e. after `} // namespace` at :1437)
- Create: `oscar/tests/compliancereporttests.h`, `oscar/tests/compliancereporttests.cpp`
- Modify: `oscar/oscar.pro`

- [ ] **Step 1: Declare the helper in the header**

In `oscar/compliancereport.h`, after the `#include` lines and before `struct SelectedChannel`:

```cpp
// Per-night oximetry summary used to pick the single "worst" night for the
// Lowest-Oxygen Night trace. Pure data — no Qt rendering — so it is unit-tested.
struct NightO2Stat {
    QDate   date;
    double  nadir;     // lowest valid SpO2 that night (artifact-floored)
    double  t90Secs;   // seconds below 90% that night
    bool    valid;     // true only if the night had usable oximetry data
};

// Returns the index of the worst night: lowest nadir wins; ties broken by most
// time below 90%, then earliest date. Returns -1 when no night is valid.
int selectWorstNightIndex(const QList<NightO2Stat>& nights);
```

- [ ] **Step 2: Create the test file (failing test)**

Create `oscar/tests/compliancereporttests.h`:

```cpp
/* Compliance Report Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef COMPLIANCEREPORTTESTS_H
#define COMPLIANCEREPORTTESTS_H

#include "AutoTest.h"

class ComplianceReportTests : public QObject
{
    Q_OBJECT

private slots:
    void testSelectWorstNightIndex();
    void testSpo2ZonePercents();
};

DECLARE_TEST(ComplianceReportTests)

#endif // COMPLIANCEREPORTTESTS_H
```

Create `oscar/tests/compliancereporttests.cpp`:

```cpp
/* Compliance Report Unit Tests — see header for license. */

#include "compliancereporttests.h"
#include "../compliancereport.h"

void ComplianceReportTests::testSelectWorstNightIndex()
{
    // empty -> -1
    QCOMPARE(selectWorstNightIndex({}), -1);

    // all invalid -> -1
    QList<NightO2Stat> allInvalid = {
        { QDate(2026,5,1), 95.0, 0.0, false },
        { QDate(2026,5,2), 80.0, 50.0, false },
    };
    QCOMPARE(selectWorstNightIndex(allInvalid), -1);

    // distinct nadirs -> lowest nadir wins (index 1)
    QList<NightO2Stat> distinct = {
        { QDate(2026,5,1), 92.0, 10.0, true },
        { QDate(2026,5,2), 75.0, 30.0, true },
        { QDate(2026,5,3), 88.0, 20.0, true },
    };
    QCOMPARE(selectWorstNightIndex(distinct), 1);

    // tie on nadir -> more time below 90% wins (index 2)
    QList<NightO2Stat> tieNadir = {
        { QDate(2026,5,1), 85.0, 10.0, true },
        { QDate(2026,5,2), 90.0,  5.0, true },
        { QDate(2026,5,3), 85.0, 40.0, true },
    };
    QCOMPARE(selectWorstNightIndex(tieNadir), 2);

    // tie on nadir AND t90 -> earliest (lowest index) wins (index 0)
    QList<NightO2Stat> tieBoth = {
        { QDate(2026,5,1), 85.0, 10.0, true },
        { QDate(2026,5,2), 85.0, 10.0, true },
    };
    QCOMPARE(selectWorstNightIndex(tieBoth), 0);
}

void ComplianceReportTests::testSpo2ZonePercents()
{
    // placeholder until Task 2 implements spo2ZonePercents()
    QVERIFY(true);
}
```

- [ ] **Step 3: Wire the test into the build**

In `oscar/oscar.pro`, inside the `test {` block, add to the `SOURCES +=` list (after `tests/clocksynctests.cpp`):

```
        tests/clocksynctests.cpp \
        tests/compliancereporttests.cpp
```

and to the `HEADERS +=` list (after `tests/clocksynctests.h`):

```
        tests/clocksynctests.h \
        tests/compliancereporttests.h
```

(Add a trailing `\` to the previous last line of each list.)

- [ ] **Step 4: Build tests and verify the new test FAILS to link/compile**

Run: `qmake OSCAR_QT.pro CONFIG+=test && make -j$(sysctl -n hw.ncpu)`
Expected: link error — `undefined symbol selectWorstNightIndex` (declared, not defined yet).

- [ ] **Step 5: Implement `selectWorstNightIndex`**

In `oscar/compliancereport.cpp`, immediately after `} // namespace` (:1437), add:

```cpp
int selectWorstNightIndex(const QList<NightO2Stat>& nights)
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
```

- [ ] **Step 6: Build and run tests — verify PASS**

Run: `qmake OSCAR_QT.pro CONFIG+=test && make -j$(sysctl -n hw.ncpu)` then run the `OSCAR` binary.
Expected: output contains `PASS   : ComplianceReportTests::testSelectWorstNightIndex()`.

- [ ] **Step 7: Commit**

```bash
git add oscar/compliancereport.h oscar/compliancereport.cpp oscar/tests/compliancereporttests.h oscar/tests/compliancereporttests.cpp oscar/oscar.pro
git commit -m "feat: add worst-night selection helper for compliance report + test scaffold"
```

---

## Task 2: Pure helper `spo2ZonePercents`

**Files:**
- Modify: `oscar/compliancereport.h`
- Modify: `oscar/compliancereport.cpp`
- Modify: `oscar/tests/compliancereporttests.cpp`

- [ ] **Step 1: Declare in header**

In `oscar/compliancereport.h`, after the `selectWorstNightIndex` declaration:

```cpp
// SpO2 time-in-zone percentages for the Overnight Oxygen Summary, derived from
// cumulative "time above threshold" seconds. Pure math — unit-tested.
struct Spo2ZonePercents { double pctGe95; double pct90to95; double pctLt90; };

// totalSec = total SpO2 recording seconds; secAbove95/secAbove90 = cumulative
// seconds with SpO2 strictly above each threshold. Returns zeros if totalSec<=0.
Spo2ZonePercents spo2ZonePercents(double totalSec, double secAbove95, double secAbove90);
```

- [ ] **Step 2: Replace the placeholder test with the real one (failing)**

In `oscar/tests/compliancereporttests.cpp`, replace the body of `testSpo2ZonePercents()`:

```cpp
void ComplianceReportTests::testSpo2ZonePercents()
{
    // no recording -> all zero
    Spo2ZonePercents z0 = spo2ZonePercents(0.0, 0.0, 0.0);
    QCOMPARE(z0.pctGe95, 0.0); QCOMPARE(z0.pct90to95, 0.0); QCOMPARE(z0.pctLt90, 0.0);

    // 10h total; 92% >=95, 7% in 90-95, 1% <90
    double total = 36000.0, above95 = 33120.0, above90 = 35640.0;
    Spo2ZonePercents z = spo2ZonePercents(total, above95, above90);
    QVERIFY(qAbs(z.pctGe95   - 92.0) < 1e-6);
    QVERIFY(qAbs(z.pct90to95 -  7.0) < 1e-6);
    QVERIFY(qAbs(z.pctLt90   -  1.0) < 1e-6);
    QVERIFY(qAbs((z.pctGe95 + z.pct90to95 + z.pctLt90) - 100.0) < 1e-6);

    // clamp: inconsistent inputs never produce negatives
    Spo2ZonePercents zc = spo2ZonePercents(100.0, 120.0, 130.0);
    QVERIFY(zc.pctGe95 >= 0.0 && zc.pct90to95 >= 0.0 && zc.pctLt90 >= 0.0);
}
```

- [ ] **Step 3: Build tests — verify FAIL (undefined symbol)**

Run: `qmake OSCAR_QT.pro CONFIG+=test && make -j$(sysctl -n hw.ncpu)`
Expected: link error `undefined symbol spo2ZonePercents`.

- [ ] **Step 4: Implement `spo2ZonePercents`**

In `oscar/compliancereport.cpp`, after `selectWorstNightIndex`:

```cpp
Spo2ZonePercents spo2ZonePercents(double totalSec, double secAbove95, double secAbove90)
{
    Spo2ZonePercents z { 0.0, 0.0, 0.0 };
    if (totalSec <= 0.0) return z;
    double ge95   = qBound(0.0, secAbove95, totalSec);
    double ge90   = qBound(ge95, secAbove90, totalSec);   // monotonic: >=95 ⊆ >=90
    z.pctGe95   = 100.0 * ge95 / totalSec;
    z.pct90to95 = 100.0 * (ge90 - ge95) / totalSec;
    z.pctLt90   = 100.0 * (totalSec - ge90) / totalSec;
    return z;
}
```

- [ ] **Step 5: Build and run tests — verify PASS**

Run: build with `CONFIG+=test`, run `OSCAR`.
Expected: `PASS   : ComplianceReportTests::testSpo2ZonePercents()` and Task 1's test still PASS.

- [ ] **Step 6: Commit**

```bash
git add oscar/compliancereport.h oscar/compliancereport.cpp oscar/tests/compliancereporttests.cpp
git commit -m "feat: add SpO2 time-in-zone percentage helper + test"
```

---

## Task 3: Draw helpers — reference bands + inline legend

**Files:**
- Modify: `oscar/compliancereport.cpp` (anonymous namespace, near `drawThresholdLine` :626)

- [ ] **Step 1: Add `drawReferenceBands`**

In `oscar/compliancereport.cpp`, just before `drawThresholdLine` (:626):

```cpp
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
```

- [ ] **Step 2: Add `drawLegend`**

Immediately after `drawReferenceBands`:

```cpp
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
```

- [ ] **Step 3: Build the app — verify it compiles**

Run: `qmake OSCAR_QT.pro && make -j$(sysctl -n hw.ncpu)`
Expected: builds clean (helpers unused yet → ensure no `-Werror` unused-function failure by leaving them `static` and consuming them in Task 4 in the same PR; if the build errors on unused, proceed directly to Task 4 before building).

- [ ] **Step 4: Commit**

```bash
git add oscar/compliancereport.cpp
git commit -m "feat: add reference-band and inline-legend chart helpers"
```

---

## Task 4: Events/hour chart — split Central apnea + zone + legend

**Files:**
- Modify: `oscar/compliancereport.cpp` — `renderAhiChart` (:1109–1198)

- [ ] **Step 1: Replace the two-segment computation with three segments**

In `renderAhiChart`, replace the `aiVals/hiVals` loop (:1121–1136) with:

```cpp
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
```

- [ ] **Step 2: Draw the controlled zone before the grid**

In `renderAhiChart`, right after the `drawYGridAndLabels(...)` call (:1147), insert:

```cpp
    if (yMax >= 5.0)
        drawReferenceBands(painter, chartRect, 0.0, yMax,
                           { { 0.0, 5.0, QColor(34, 197, 94, 38) } }); // controlled ≤5
```

Note: `drawReferenceBands` must be called BEFORE grid for cleanliness; if grid already drawn, the low alpha keeps gridlines visible — acceptable. Prefer moving it directly after `drawYGridAndLabels`.

- [ ] **Step 3: Replace the stacked-bar draw with three segments**

Replace the stacked-bar block (:1156–1186, the `aiColor`/`hiColor` declarations through the draw loop) with:

```cpp
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
```

- [ ] **Step 4: Add the legend before `return c.finish();`**

Just before `return c.finish();` (:1197):

```cpp
    drawLegend(painter, chartRect,
               { { 0, oaColor, "Obstructive" },
                 { 0, caColor, "Central" },
                 { 0, hiColor, "Hypopnea" } },
               /*topRight=*/false);  // right margin holds the "AHI 5" label
```

- [ ] **Step 5: Build the app**

Run: `qmake OSCAR_QT.pro && make -j$(sysctl -n hw.ncpu)`
Expected: clean build.

- [ ] **Step 6: Visual verify**

Launch `OSCAR`, export a Compliance Report for a profile with central events. On the Trends page, the Events/hour chart shows three stacked colors, a faint green zone up to 5, and a legend "Obstructive / Central / Hypopnea". Confirm a night with central apnea shows the rose segment.

- [ ] **Step 7: Commit**

```bash
git add oscar/compliancereport.cpp
git commit -m "feat: split central apnea in events chart, add controlled zone + legend"
```

---

## Task 5: Leak chart — good-seal zone + legend

**Files:**
- Modify: `oscar/compliancereport.cpp` — `renderLeakChart` (:1200–1245)

- [ ] **Step 1: Draw bands after `drawYGridAndLabels`**

In `renderLeakChart`, right after `drawYGridAndLabels(painter, chartRect, yMax, 4);` (:1217):

```cpp
    if (redline > 0.0)
        drawReferenceBands(painter, chartRect, 0.0, yMax,
                           { { 0.0,     redline, QColor(34, 197, 94, 30) },   // good seal
                             { redline, yMax,    QColor(239, 68, 68, 26) } }); // elevated
```

- [ ] **Step 2: Add legend before `return c.finish();`**

Just before `return c.finish();` (:1244):

```cpp
    drawLegend(painter, chartRect,
               { { 0, QColor("#fbbf24"), "95th percentile" },
                 { 1, QColor("#0f172a"), "median" },
                 { 2, QColor("#ef4444"), "leak limit" } },
               /*topRight=*/false);  // right margin holds the redline value
```

- [ ] **Step 3: Build**

Run: `qmake OSCAR_QT.pro && make -j$(sysctl -n hw.ncpu)`
Expected: clean build.

- [ ] **Step 4: Visual verify**

Export report; the Leak chart shows a green band below the 24 L/min line, a faint red tint above, and the legend. Bars + median ticks unchanged.

- [ ] **Step 5: Commit**

```bash
git add oscar/compliancereport.cpp
git commit -m "feat: add good-seal reference band + legend to leak chart"
```

---

## Task 6: Pressure chart — prescribed bounds by mode + legend

**Files:**
- Modify: `oscar/compliancereport.cpp` — `renderPressureChart` (:1247–1294)

**Note on data:** the function receives `pressChan`. Mode and the set pressure(s) are available per the report's settings blocks (the same data that prints "Fixed 10.6" / pressure-min/max in the history table). Read the prescribed values via `Day::settings_max(CPAP_PressureMax)` / `Day::settings_min(CPAP_PressureMin)` for auto modes and `Day::settings_max(CPAP_Pressure)` (or the fixed set value already used by the history table) for fixed mode. Confirm the exact channel constants against `schema.cpp` at implementation; fall back to drawing no bound if a value is unavailable.

- [ ] **Step 1: Compute prescribed bounds across the range**

In `renderPressureChart`, after the per-day `vals/medians` loop (:1266) and before `double yMax = ...`:

```cpp
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
```

- [ ] **Step 2: Include bounds in the y-range**

Replace the `double yMax = qCeil(maxVal / 4.0) * 4.0;` line (:1267) with:

```cpp
    if (isAuto) maxVal = qMax(maxVal, setMax);
    else        maxVal = qMax(maxVal, setFixed);
    double yMax = qCeil(maxVal / 4.0) * 4.0;
```

- [ ] **Step 3: Draw the bounds after the bars**

After `drawBars(...)` (:1291) and before `return c.finish();`:

```cpp
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
```

- [ ] **Step 4: Build**

Run: `qmake OSCAR_QT.pro && make -j$(sysctl -n hw.ncpu)`
Expected: clean build (if `settings_min/max` or channel names differ, adjust per `day.h`/`schema.cpp`).

- [ ] **Step 5: Visual verify (both modes)**

Export for a **fixed CPAP** profile → one dashed "set pressure" line + legend. Export for an **APAP** profile → two dashed bounds (min/max) + legend; confirm a high-pressure night reaches the upper bound.

- [ ] **Step 6: Commit**

```bash
git add oscar/compliancereport.cpp
git commit -m "feat: draw prescribed pressure bounds (mode-aware) + legend"
```

---

## Task 7: SpO2 nightly chart — clinical bands + 88% line + legend

**Files:**
- Modify: `oscar/compliancereport.cpp` — `renderChannelBarChart` (:1296–1397), SpO2 branch only

- [ ] **Step 1: Draw bands + 88% line for SpO2 after `drawYGridAndLabels`**

In `renderChannelBarChart`, right after `drawYGridAndLabels(painter, chartRect, yMax, 4, yMin);` (:1384):

```cpp
    if (chanId == OXI_SPO2) {
        drawReferenceBands(painter, chartRect, yMin, yMax,
                           { { 95.0, 100.0, QColor(34, 197, 94, 34) },   // normal
                             { 90.0,  95.0, QColor(234, 179, 8, 30) },   // mild
                             { yMin,  90.0, QColor(239, 68, 68, 26) } }); // concern
    }
```

- [ ] **Step 2: Add the 88% threshold line next to the existing 90% line**

Replace the existing SpO2 threshold block (:1393–1394):

```cpp
    // SpO2: draw clinically significant 90% desaturation threshold line
    if (chanId == OXI_SPO2)
        drawThresholdLine(painter, chartRect, yMax, 90.0, QColor("#ef4444"), yMin);
```

with:

```cpp
    if (chanId == OXI_SPO2) {
        drawThresholdLine(painter, chartRect, yMax, 90.0, QColor("#ef4444"), yMin);
        if (yMin < 88.0)
            drawThresholdLine(painter, chartRect, yMax, 88.0, QColor("#f59e0b"), yMin);
    }
```

- [ ] **Step 3: Add per-chart legends before `return c.finish();`**

Just before `return c.finish();` (:1396):

```cpp
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
```

- [ ] **Step 4: Build**

Run: `qmake OSCAR_QT.pro && make -j$(sysctl -n hw.ncpu)`
Expected: clean build.

- [ ] **Step 5: Visual verify**

Export report. SpO2 nightly chart shows green/amber/red bands, 90% + 88% dashed lines, and the SpO2 legend. Other channel charts (minute vent, resp rate, etc.) show only the "95th percentile / median" legend — **no bands**.

- [ ] **Step 6: Commit**

```bash
git add oscar/compliancereport.cpp
git commit -m "feat: add clinical bands, 88% line, and legends to channel charts"
```

---

## Task 8: Overnight Oxygen Summary panel

**Files:**
- Modify: `oscar/compliancereport.cpp` — the oximetry HTML section (near the existing SpO2 stats, around :2396–2411) and add a small HTML builder.

**Note:** Reuse already-computed values: `spo2DropIndex` (:2396), `t90Secs` (:1746), `minSpO2`, `avgSpO2Period`, `spo2RecordingSecs` (:1750). Add `secAbove95 = day-summed timeAboveThreshold(OXI_SPO2, 95)` and `secAbove90` over the range (mirror how `t90Secs`/recording are accumulated). Add `t88Secs` via `timeBelowThreshold(OXI_SPO2, 88)` summed the same way as `t90Secs`.

- [ ] **Step 1: Accumulate the zone seconds where the period oximetry loop runs**

In the loop that already sums `spo2RecordingSecs`/`t90Secs` (around :1740–1750), add accumulators:

```cpp
        secAbove95Total += day->timeAboveThreshold(OXI_SPO2, 95.0) * 60.0;
        secAbove90Total += day->timeAboveThreshold(OXI_SPO2, 90.0) * 60.0;
        t88SecsTotal    += day->timeBelowThreshold(OXI_SPO2, 88.0) * 60.0;
```

(declare `double secAbove95Total=0, secAbove90Total=0, t88SecsTotal=0;` beside the existing accumulators; note `timeAboveThreshold` returns minutes here per the existing `* 60.0` convention at :1750 — match it.)

- [ ] **Step 2: Build the summary HTML block**

Where the oximetry section HTML is assembled (just before the existing "Oxygen Saturation" stats table, ~:2407), insert:

```cpp
    {
        Spo2ZonePercents z = spo2ZonePercents(spo2RecordingSecs, secAbove95Total, secAbove90Total);
        auto pct1 = [](double v){ return QString::number(v, 'f', 0) + "%"; };
        auto hm   = [](double secs){ int m=int(secs/60.0); return QString::number(m)+" min"; };
        html += "<div style='margin:6px 0 10px 0;'>";
        html += "<div style='font-size:11pt;font-weight:700;color:#0f172a;margin-bottom:3px;'>Overnight Oxygen Summary</div>";
        html += "<div style='font-size:8.5pt;color:#64748b;margin-bottom:4px;'>Percent of night at each oxygen level</div>";
        html += "<table width='100%' cellspacing='0' cellpadding='0' style='height:18px;'><tr>";
        if (z.pctGe95   > 0) html += "<td style='background:#22c55e;color:#fff;font-size:8pt;text-align:center;width:" + QString::number(z.pctGe95) + "%;'>Normal ≥95% · " + pct1(z.pctGe95) + "</td>";
        if (z.pct90to95 > 0) html += "<td style='background:#eab308;color:#fff;font-size:8pt;text-align:center;width:" + QString::number(z.pct90to95) + "%;'>90–95%</td>";
        if (z.pctLt90   > 0) html += "<td style='background:#ef4444;color:#fff;font-size:8pt;text-align:center;width:" + QString::number(z.pctLt90) + "%;'>&lt;90</td>";
        html += "</tr></table>";
        html += "<table width='100%' style='font-size:9pt;color:#334155;margin-top:4px;'>";
        html += "<tr><td>Oxygen Desaturation Index (≥3% drops): <b>" + QString::number(spo2DropIndex,'f',1) + " / hr</b></td><td>Lowest oxygen: <b>" + (minSpO2>0?QString::number(minSpO2,'f',0)+"%":QString("N/A")) + "</b></td><td>Average: <b>" + QString::number(avgSpO2Period,'f',0) + "%</b></td></tr>";
        html += "<tr><td>Time below 90%: <b>" + hm(t90Secs) + "</b></td><td>Time below 88%: <b>" + hm(t88SecsTotal) + "</b></td><td></td></tr>";
        html += "</table></div>";
    }
```

(If the actual desaturation drop threshold OSCAR uses is not 3%, change the "≥3% drops" label to the real value — do not hard-code a wrong number. Confirm against the oximeter import / settings.)

- [ ] **Step 3: Build**

Run: `qmake OSCAR_QT.pro && make -j$(sysctl -n hw.ncpu)`
Expected: clean build.

- [ ] **Step 4: Visual verify**

Export for a profile with oximetry. The Overnight Oxygen Summary appears with a green/amber/red time-in-range bar and the indices, before the detailed oxygen stats table. Confirm bar segments sum visually to the full width and percentages match the stats. Confirm a non-oximetry profile shows nothing new.

- [ ] **Step 5: Commit**

```bash
git add oscar/compliancereport.cpp
git commit -m "feat: add Overnight Oxygen Summary panel to compliance report"
```

---

## Task 9: Lowest-Oxygen Night trace

**Files:**
- Modify: `oscar/compliancereport.cpp` — add `renderLowestOxygenNight(...)` near the other render functions; call it in the oximetry section.

- [ ] **Step 1: Add the render function**

Add near the other `render*Chart` functions (anonymous namespace), above `} // namespace` (:1437):

```cpp
// Whole-night SpO2 trace for the single worst night of the period. Plots raw
// OXI_SPO2 samples on a clock-time axis with 90%/88% reference lines and
// desaturation marks; gaps (inter-session + probe-loss) are drawn as gaps.
static QString renderLowestOxygenNight(Day* day, const QString& dateLabel,
                                       double nadir, const QString& title)
{
    SvgChart c(1, "Lowest-Oxygen Night", title, "%");
    QPainter& painter = c.painter;
    const QRect& chartRect = c.chartRect;

    QList<Session*> sessions = day->getSessions(MT_OXIMETER, /*ignore_enabled=*/true);
    if (sessions.isEmpty()) sessions = day->getSessions(MT_CPAP, true); // SpO2 via CPAP source
    qint64 tStart = 0, tEnd = 0; bool haveSpan = false;
    for (Session* s : sessions) {
        if (!s->channelExists(OXI_SPO2)) continue;
        qint64 f = s->first(OXI_SPO2), l = s->last(OXI_SPO2);
        if (!haveSpan) { tStart = f; tEnd = l; haveSpan = true; }
        else { tStart = qMin(tStart, f); tEnd = qMax(tEnd, l); }
    }
    if (!haveSpan || tEnd <= tStart) return c.finish();

    double yMin = qMax(70.0, qMin(85.0, nadir - 5.0));
    double yMax = 100.0;
    drawReferenceBands(painter, chartRect, yMin, yMax,
                       { { 95.0,100.0, QColor(34,197,94,30) },
                         { 90.0, 95.0, QColor(234,179,8,26) },
                         { yMin, 90.0, QColor(239,68,68,22) } });
    drawYGridAndLabels(painter, chartRect, yMax, 5, yMin);
    drawThresholdLine(painter, chartRect, yMax, 90.0, QColor("#ef4444"), yMin);
    if (yMin < 88.0) drawThresholdLine(painter, chartRect, yMax, 88.0, QColor("#f59e0b"), yMin);

    double range = yMax - yMin;
    double span  = double(tEnd - tStart);
    auto xFor = [&](qint64 t){ return chartRect.left() + (double(t - tStart) / span) * chartRect.width(); };
    auto yFor = [&](double v){ return chartRect.bottom() - ((qBound(yMin,v,yMax) - yMin) / range) * chartRect.height(); };

    painter.setPen(QPen(QColor("#16a34a"), 2.0));
    const double GAP_MS = 120000.0;          // >2 min without a sample => break the line
    for (Session* s : sessions) {
        if (!s->channelExists(OXI_SPO2)) continue;
        QList<EventList*> evs = s->eventlist.value(OXI_SPO2);
        for (EventList* el : evs) {
            QPointF prev; bool havePrev = false; qint64 prevT = 0;
            for (quint32 i = 0; i < el->count(); ++i) {
                double v = el->data(i);
                qint64 t = el->time(i);
                if (v <= SPO2_ARTIFACT_FLOOR) { havePrev = false; continue; } // probe loss = gap
                QPointF p(xFor(t), yFor(v));
                if (havePrev && (t - prevT) <= GAP_MS) painter.drawLine(prev, p);
                prev = p; prevT = t; havePrev = true;
            }
        }
    }

    // desaturation marks
    painter.setPen(Qt::NoPen); painter.setBrush(QColor("#0f172a"));
    for (Session* s : sessions) {
        if (!s->channelExists(OXI_SPO2Drop)) continue;
        QList<EventList*> evs = s->eventlist.value(OXI_SPO2Drop);
        for (EventList* el : evs) {
            for (quint32 i = 0; i < el->count(); ++i) {
                double x = xFor(el->time(i));
                double y = chartRect.bottom() + 6;
                QPainterPath tri; tri.moveTo(x-4,y); tri.lineTo(x+4,y); tri.lineTo(x,y+7); tri.closeSubpath();
                painter.drawPath(tri);
            }
        }
    }
    return c.finish();
}
```

(Confirm the `Session` API for per-channel event lists at implementation — `Session::eventlist`, `channelExists`, `first/last` are used by `gLineChart.cpp`; match those exact names.)

- [ ] **Step 2: Select the worst night and emit the chart in the oximetry section**

Where the oximetry section builds its charts/HTML, after the Overnight Oxygen Summary, add:

```cpp
    {
        QList<NightO2Stat> nights;
        QList<Day*> dayPtrs;
        for (int i = 0; i < totalDays; ++i) {
            QDate d = start.addDays(i);
            NightO2Stat n { d, 0.0, 0.0, false };
            Day* day = daysMap.contains(d) ? daysMap[d] : nullptr;
            if (day && day->timeAboveThreshold(OXI_SPO2, 0.0) > 0.0) {
                double nadir = day->Min(OXI_SPO2);
                if (nadir <= SPO2_ARTIFACT_FLOOR) nadir = day->percentile(OXI_SPO2, 0.01);
                n.nadir   = nadir;
                n.t90Secs = day->timeBelowThreshold(OXI_SPO2, 90.0) * 60.0;
                n.valid   = true;
            }
            nights.append(n);
            dayPtrs.append(day);
        }
        int wi = selectWorstNightIndex(nights);
        if (wi >= 0) {
            const NightO2Stat& w = nights[wi];
            QString title = "Lowest-Oxygen Night — " + w.date.toString("MMM d");
            QString url = renderLowestOxygenNight(dayPtrs[wi], w.date.toString("MMM d"),
                                                  w.nadir, title);
            QString caption = "<div style='font-size:8pt;color:#94a3b8;'>single night · lowest reading " +
                              QString::number(w.nadir,'f',0) + "% · source: oximeter</div>";
            html += makeChartBlockHtml(url, caption, displayW, displayH, STATS_COL_W, chartRegistry);
        }
    }
```

(Use the same `displayW/displayH/STATS_COL_W/chartRegistry` variables the surrounding section already uses for `makeChartBlockHtml`.)

- [ ] **Step 3: Build**

Run: `qmake OSCAR_QT.pro && make -j$(sysctl -n hw.ncpu)`
Expected: clean build.

- [ ] **Step 4: Visual verify**

Export for a profile whose worst night has a real desaturation. The Lowest-Oxygen Night trace appears, titled with the date, green line, 90%/88% lines, ▼ desaturation marks, and **gaps shown as gaps** (turn the oximeter off mid-night in a test fixture, or pick a night with a known probe-loss span, and confirm no line is drawn across it / no drop-to-zero).

- [ ] **Step 5: Commit**

```bash
git add oscar/compliancereport.cpp
git commit -m "feat: add lowest-oxygen-night SpO2 trace to compliance report"
```

---

## Task 10: Page-budget verification + polish

**Files:**
- Modify: `oscar/compliancereport.cpp` (placement only, if needed)

- [ ] **Step 1: Confirm placement keeps 7 pages**

Export reports for: (a) the author's own profile (oximetry + fixed CPAP), (b) an APAP profile, (c) a CPAP-only profile with no oximetry. Count pages.
Expected: (a) and (b) stay 7 pages with the two new oximetry blocks in the existing whitespace under the pulse/SpO2 charts; (c) is unchanged from today.

- [ ] **Step 2: If a block overflows to a new page, move it**

If the trace pushes a page break, place the Overnight Oxygen Summary + Lowest-Oxygen Night together under the SpO2/pulse channel charts (the oximetry page) inside a single `page-break-inside:avoid` div so they pack into the whitespace. Adjust `displayH` for the trace down (e.g. to ~70% of a normal chart) if vertical room is tight.

- [ ] **Step 3: Full vector-quality check**

Open the PDF, zoom to 400% on the new trace and the Events chart. Confirm lines/text stay crisp (vector), bands sit behind bars, legends don't overlap data.

- [ ] **Step 4: Run the unit tests once more**

Run: `qmake OSCAR_QT.pro CONFIG+=test && make -j$(sysctl -n hw.ncpu)` then run `OSCAR`.
Expected: `ComplianceReportTests` both PASS; no AddressSanitizer errors.

- [ ] **Step 5: Commit**

```bash
git add oscar/compliancereport.cpp
git commit -m "chore: verify page budget and vector quality for chart upgrades"
```

---

## Self-Review (completed during authoring)

- **Spec coverage:** reference bands (Tasks 4,5,7,9) · central-apnea split (Task 4) · pressure bounds mode-branched (Task 6) · inline legends (Tasks 4–7) · Overnight Oxygen Summary (Task 8) · Lowest-Oxygen Night trace (Tasks 1,9) · accuracy rules: ODI label note (Task 8), gaps-as-gaps + artifact floor (Task 9), provenance caption (Tasks 8,9), bands only on clinical charts (Task 7 explicitly excludes raw channels) · page budget (Task 10). All spec sections map to a task.
- **Type consistency:** `NightO2Stat`, `Spo2ZonePercents`, `RefBand`, `LegendItem`, `selectWorstNightIndex`, `spo2ZonePercents`, `drawReferenceBands`, `drawLegend`, `renderLowestOxygenNight` are used with identical signatures across tasks.
- **Known confirm-at-implementation points (flagged, not placeholders):** exact APAP min/max channel constants (Task 6), `Session` per-channel eventlist accessor names (Task 9), the real desaturation drop threshold for the ODI label (Task 8), and whether `timeAboveThreshold` returns minutes vs seconds in the period loop (Task 8 matches the existing `*60.0` convention at :1750). Each has a stated fallback.
