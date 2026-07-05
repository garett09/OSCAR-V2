# AirView V2 Compliance Report — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a selectable "AirView V2" theme to OSCAR's Compliance Report — a fixed-layout 7-page A4 PDF drawn directly with QPainter, reproducing the supplied mockup — while leaving the existing "Classic" report's output unchanged.

**Architecture:** Refactor the stat-gathering half of `ComplianceReport::exportReport()` into a shared `gatherReportData()` → `ReportData` struct (single source of truth). A "Report style" dropdown dispatches: Classic → existing `QTextDocument` path (reads from `ReportData`); AirView V2 → new `renderAirViewV2Pdf()` in `compliancereport_airviewv2.cpp` that paints each A4 page directly to the `QPrinter`. Helpers currently trapped in an anonymous namespace are hoisted to a new `compliancereport_internal.h`.

**Tech Stack:** C++17, Qt 6 (`QPainter`, `QPrinter`, `QPdfWriter`/`QPrinter::PdfFormat`, `QFont`, `QColor`), qmake6. Build via `./build.sh`. Test harness: Qt Test (`oscar/tests/`, `AutoTest.h`, `DECLARE_TEST`).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-06-28-airview-v2-compliance-report-design.md` (authoritative for fields/decisions).
- Geometry source of truth: `docs/superpowers/specs/2026-06-28-airview-v2-mockup.html` (decoded mockup; 794-px artboard ≈ A4 at 96 dpi, so layout scale `s = pageRect.width()/794.0 ≈ 1.0`). Translate coordinates from there.
- Fonts: SF Pro (sans) + SF Mono (mono, ALL numbers); fallback "Helvetica Neue"/"Menlo"/"Courier New".
- No fabricated data: GATED sections render only when their source data exists, else omitted (never "0"). Absent scalars render as "—".
- Classic output must remain visually identical (the `gatherReportData` extraction is behavior-preserving).
- Copyright header on every new file: GPL block matching existing files (see `compliancereport.cpp:1-7`).
- New files registered in `oscar/oscar.pro` (no CMake exists).
- Build incrementally during iteration: `cd ~/oscar-code/build-qt6 && make -C oscar -j$(sysctl -n hw.logicalcpu)`. First-time / clean: `~/oscar-code/build.sh`.

## Verification model (read first)

This is a GUI PDF generator, not a library — most tasks verify by **compile + run + inspect PDF**, not assertion tests. Conventions used below:
- **BUILD:** `cd ~/oscar-code/build-qt6 && make -C oscar -j$(sysctl -n hw.logicalcpu)` → expect `0` errors, app relinks.
- **RUN+PDF:** launch the app, open a profile, `File ▸ Export ▸ Compliance Report` (or the existing menu entry), pick the style, save to `/tmp/avtest.pdf`, then inspect:
  - page count: `mdls -name kMDItemNumberOfPages /tmp/avtest.pdf` (macOS) — expect the documented N.
  - visual: `open /tmp/avtest.pdf` and compare against the mockup page.
- **UNIT:** Task 4 adds a real Qt Test for the one pure, data-independent calc (usage quartiles). Run the test target as the other `*tests.cpp` are run.
- A task's "Expected" lines state the concrete pass condition. Commit only after the task's verification passes.

---

### Task 1: Hoist shared internals into `compliancereport_internal.h`

Helpers AirView V2 reuses are in an anonymous namespace (`compliancereport.cpp:278–1878`) → invisible to a second `.cpp`. Move the **declarations** AirView V2 + `ReportData` need to a new header with external linkage; keep definitions in `compliancereport.cpp` but **outside** the anon namespace.

**Files:**
- Create: `oscar/compliancereport_internal.h`
- Modify: `oscar/compliancereport.cpp` (move `SettingsChangeBlock` :511, `StatsRow` :524, `FilteredStats` :370, `Spo2ZonePercents` :927, `NightO2Stat` :900 out of the anon namespace; remove `static` from `getSettingStr` :289 and `renderLowestOxygenNight` :1798; ensure `colorForChannel` :871, `unitForChannel` :885, `calcFilteredStats` :471, `calcDayFilteredStats` :498, `calcDayAvgPercentile` :1734, `calcDayAvgMax` :1764, `spo2ZonePercents` :929, `getSettingsChanges` :532 have external linkage)
- Modify: `oscar/oscar.pro:484` (add header to `HEADERS`)

**Interfaces:**
- Produces (declared in the new header, used by Tasks 2,3,6–13):
```cpp
// compliancereport_internal.h
#ifndef COMPLIANCEREPORT_INTERNAL_H
#define COMPLIANCEREPORT_INTERNAL_H
#include <QString><QColor><QDate><QMap><QList><QVector>
#include "SleepLib/profiles.h"   // Profile, Day, ChannelID, MachineType, CPAPMode

struct FilteredStats { /* copy fields verbatim from compliancereport.cpp:370-392 */ };
struct Spo2ZonePercents { double pctGe95; double pct90to95; double pctLt90; };
struct NightO2Stat { /* copy from :900-907 */ };
struct StatsRow { /* copy from :524-530 */ };
struct SettingsChangeBlock {
    /* copy existing members from :511-522, THEN add: */
    double p95Leak = 0.0;        // Task 3
    double deltaAhiVsBase = 0.0; // Task 3 (valid only when baseline known)
};

QColor  colorForChannel(ChannelID chanId);
QString unitForChannel(ChannelID chanId);
QString getSettingStr(Day* day, ChannelID cid);
FilteredStats calcFilteredStats(ChannelID, MachineType, QDate, QDate, double floor, double thr, double maxFall = 0.0);
FilteredStats calcDayFilteredStats(Day*, ChannelID, MachineType, double floor, double thr, double maxFall = 0.0);
double calcDayAvgPercentile(ChannelID, double pct, const QMap<QDate,Day*>&);
double calcDayAvgMax(ChannelID, const QMap<QDate,Day*>&);
Spo2ZonePercents spo2ZonePercents(double totalSec, double secAbove95, double secAbove90);
QString renderLowestOxygenNight(Day*, double nadir, const QString& title);
QList<SettingsChangeBlock> getSettingsChanges(QDate, QDate, const QList<Day*>&);
#endif
```
(Match each moved struct/function's CURRENT signature exactly — copy field/param lists from the cited lines, do not invent.)

- [ ] **Step 1: Create the header** with the content above, copying struct bodies/signatures verbatim from the cited line numbers.

- [ ] **Step 2: De-anonymize the definitions.** In `compliancereport.cpp`, move the listed structs and functions out of the `namespace { … }` (`:278`/`:1878`). Remove `static` from `getSettingStr` and `renderLowestOxygenNight`. `#include "compliancereport_internal.h"` near the top of `compliancereport.cpp`. Leave everything else inside the anon namespace untouched.

- [ ] **Step 3: Register header in build.** `oscar/oscar.pro:484`, after `compliancereport.h \`, add `    compliancereport_internal.h \`.

- [ ] **Step 4: BUILD.** Run the BUILD command. Expected: 0 errors (pure move — no behavior change). Fix any duplicate-symbol/ODR errors by ensuring each moved symbol is defined exactly once and only declared in the header.

- [ ] **Step 5: RUN+PDF regression.** Generate a **Classic** report to `/tmp/classic_before.pdf`. Expected: identical to a pre-change Classic report (this task changes linkage only).

- [ ] **Step 6: Commit.**
```bash
git add oscar/compliancereport_internal.h oscar/compliancereport.cpp oscar/oscar.pro
git commit -m "refactor(compliancereport): hoist shared helpers to compliancereport_internal.h"
```

---

### Task 2: Extract `gatherReportData()` + `ReportData` (behavior-preserving)

Move the pure-computation lines from `exportReport()` into a shared function. **Keep in `exportReport()`:** the no-data `QMessageBox` guard (`:1974–1977`), `QFileDialog` save (`:2008–2015`), the `getHoursMinutes`/`formatHoursMinutesShort` lambdas (`:2264–2277`), and Classic chart-SVG generation (`:2293–2297`). **Also move** the ODI/T88/SpO₂-zone math at `:2890–2903` (it lives in the render section today).

**Files:**
- Modify: `oscar/compliancereport_internal.h` (add `ReportData` + `SleepStageStats` — sleep fields populated in Task 3, declared now)
- Modify: `oscar/compliancereport.h` (declare `static ReportData ComplianceReport::gatherReportData(QDate, QDate);`)
- Modify: `oscar/compliancereport.cpp` (define `gatherReportData`; replace inline locals in `exportReport` with `rd.*`)

**Interfaces:**
- Produces:
```cpp
// compliancereport_internal.h — copy the ReportData + SleepStageStats blocks
// verbatim from the spec ("Shared data" section). Sleep + derived/gated fields
// are DECLARED here; POPULATED in Task 3 (leave them default-initialized now).
struct SleepStageStats { bool present = false; /* …spec… */ };
struct ReportData { /* …spec… */ SleepStageStats sleep; };

// compliancereport.h, inside class ComplianceReport (public):
static ReportData gatherReportData(QDate start, QDate end);
```
- Consumes: Task 1 helpers/structs.

- [ ] **Step 1: Add `ReportData`/`SleepStageStats` to the header**, copying the field list from the spec's "Shared data — gatherReportData() + ReportData" section. Default-init all derived/gated/sleep fields (`= 0.0` / `= false` / `{}`).

- [ ] **Step 2: Declare `gatherReportData`** in `compliancereport.h` as a `static` member (preserves `tr()` via `Q_DECLARE_TR_FUNCTIONS(ComplianceReport)`).

- [ ] **Step 3: Define `gatherReportData(start,end)`** in `compliancereport.cpp`: cut the pure-computation statements (the EXISTS-class fields enumerated in the spec, sourced at the lines the feasibility audit listed, **plus** ODI/T88/zones from `:2890–2903`) from `exportReport`, paste into the new function body, and `return rd;`. Use `p_profile` directly (global). Do NOT move the four kept items listed above.

- [ ] **Step 4: Rewrite `exportReport` to consume `rd`.** At the top (after the no-data guard + save dialog), `ReportData rd = gatherReportData(start, end);`. Replace each former local with `rd.<field>`. Keep the lambdas + chart-SVG gen + `QTextDocument` assembly intact, now reading `rd.*`.

- [ ] **Step 5: BUILD.** Expected: 0 errors.

- [ ] **Step 6: RUN+PDF regression.** Generate **Classic** to `/tmp/classic_after.pdf`. Expected: visually identical to `/tmp/classic_before.pdf` from Task 1 (same numbers, same layout). Diff page-by-page in a viewer.

- [ ] **Step 7: Commit.**
```bash
git add oscar/compliancereport_internal.h oscar/compliancereport.h oscar/compliancereport.cpp
git commit -m "refactor(compliancereport): extract gatherReportData into shared ReportData"
```

---

### Task 3: Populate derived + gated fields

Add the small new calcs (DERIVED) and gated sources (GATED) to `gatherReportData()` / `getSettingsChanges()`.

**Files:**
- Modify: `oscar/compliancereport.cpp` (`gatherReportData`, `getSettingsChanges`)

**Interfaces:**
- Consumes: `ReportData`/`SettingsChangeBlock` from Tasks 1–2.
- Produces: fully populated `rd` (used by all page builders).

- [ ] **Step 1: IQR quartiles.** After the existing median calc on the sorted `activeDaysHours` (`:2080-2089`), compute quartiles via the helper added in Task 4 (forward-declare/use `usageQuartiles`). For now inline-compute and assign `rd.q1HoursUsed`, `rd.q3HoursUsed` (Task 4 replaces the inline math with the tested helper).

- [ ] **Step 2: Baseline (GATED).**
```cpp
Machine* m = p_profile->GetMachine(MT_CPAP);
double base = m ? m->untreatedAHI() : 0.0;   // confirm accessor name at compliancereport.cpp:2423
rd.hasBaseline    = (base > 0.0);
rd.untreatedAHI   = base;
rd.baselinePctChange = rd.hasBaseline ? (rd.AHI - base) / base * 100.0 : 0.0;
```

- [ ] **Step 3: Oximetry-derived.**
```cpp
rd.oxiNotUsedDays    = rd.totalDays - rd.usedDaysOxiAny;
double oxiHrs = 0.0; for (Day* d : rd.oxiDaysMap) oxiHrs += d->timeAboveThreshold(OXI_SPO2, 0.0)/60.0; // hours
rd.oxiRecordingHrsAvg = rd.usedDaysOxiAny ? oxiHrs / rd.usedDaysOxiAny : 0.0;
rd.pctTimeInDesat = rd.spo2RecordingSecs > 0.0
    ? p_profile->calcSum(OXI_SPO2Drop, rd.oxiSourceType, rd.start, rd.end) / rd.spo2RecordingSecs * 100.0 : 0.0;
double oxiHours = oxiHrs;  // reuse
rd.pulseChangeIndex = oxiHours > 0.0
    ? p_profile->calcCount(OXI_PulseChange, rd.oxiSourceType, rd.start, rd.end) / oxiHours : 0.0;
```
(Confirm `OXI_SPO2Drop`/`OXI_PulseChange` channel IDs exist — the data-mapping audit found them in `oximeterimport.cpp`. If a channel is absent for the profile, guard with `p_profile->hasChannel(...)` and leave the field 0 + a `bool` present-flag if you add one.)

- [ ] **Step 4: Sleep stages (GATED).** In `gatherReportData`, detect sleep-stage data and fill `rd.sleep`:
```cpp
if (!p_profile->GetMachines(MT_SLEEPSTAGE).isEmpty()) {
    // aggregate SC_SS_* statistics over [start,end] — see statistics.cpp:883-893
    // SC_SS_TST, SC_SS_BEDTIME, SC_SS_LATENCY, SC_SS_WAKEUP, SC_SS_*_PCT, ZEO_TimeIn*
    // set rd.sleep.present = (sleepStageNights > 0); fill pct/min/tst/bedtime/latency/wake/remOfTst.
}
```
Leave `rd.sleep.present = false` when no `MT_SLEEPSTAGE` machine.

- [ ] **Step 5: Per-block leak + delta in `getSettingsChanges`.** For each `SettingsChangeBlock`, compute `block.p95Leak` over the block's date range (build a per-block day map, call `calcDayAvgPercentile(leakChan, upperPctValue(), blockMap)`); set `block.deltaAhiVsBase = rd.hasBaseline ? (block.ahi - base)/base*100.0 : 0.0`. (Thread `base`/leakChan in, or compute inside.)

- [ ] **Step 6: BUILD.** Expected: 0 errors.

- [ ] **Step 7: RUN+PDF.** Classic unaffected (it doesn't read these new fields) → `/tmp/classic_after.pdf` still matches. Expected: identical Classic output.

- [ ] **Step 8: Commit.**
```bash
git add oscar/compliancereport.cpp
git commit -m "feat(compliancereport): populate derived + gated ReportData fields"
```

---

### Task 4: Unit-test the usage-quartile helper

The one pure, data-independent calc — give it a real Qt Test (pattern for any future report-math tests).

**Files:**
- Create: `oscar/tests/compliancereporttests.h`, `oscar/tests/compliancereporttests.cpp`
- Modify: `oscar/compliancereport_internal.h` (declare `usageQuartiles`), `oscar/compliancereport.cpp` (define it; call from Task 3 Step 1)
- Modify: `oscar/oscar.pro` (add the test source near `:820`)

**Interfaces:**
- Produces: `void usageQuartiles(const QVector<double>& sortedHours, double& q1, double& q3);` (linear-interpolated 25th/75th percentile; input MUST be pre-sorted ascending; empty → q1=q3=0).

- [ ] **Step 1: Write the failing test.**
```cpp
// compliancereporttests.cpp
#include "AutoTest.h"
#include "compliancereporttests.h"
#include "compliancereport_internal.h"
void ComplianceReportTests::testUsageQuartiles() {
    double q1=0,q3=0;
    QVector<double> v{1,2,3,4,5,6,7,8};   // pre-sorted
    usageQuartiles(v,q1,q3);
    QVERIFY(qAbs(q1-2.75) < 1e-6);
    QVERIFY(qAbs(q3-6.25) < 1e-6);
    QVector<double> empty; usageQuartiles(empty,q1,q3);
    QCOMPARE(q1,0.0); QCOMPARE(q3,0.0);
}
DECLARE_TEST(ComplianceReportTests)
```
```cpp
// compliancereporttests.h
#ifndef COMPLIANCEREPORTTESTS_H
#define COMPLIANCEREPORTTESTS_H
#include <QObject>
class ComplianceReportTests : public QObject { Q_OBJECT
private slots: void testUsageQuartiles(); };
#endif
```

- [ ] **Step 2: Register + build the test.** Add `        tests/compliancereporttests.cpp \` to `oscar.pro` near `:820`. BUILD. Expected: FAIL — `usageQuartiles` undefined (link error) — confirming the test is wired.

- [ ] **Step 3: Implement `usageQuartiles`** in `compliancereport.cpp` (declared in the internal header):
```cpp
static double pctile(const QVector<double>& s, double p) {     // s sorted asc, p in [0,1]
    if (s.isEmpty()) return 0.0; if (s.size()==1) return s[0];
    double idx = p*(s.size()-1); int lo=(int)idx; double f=idx-lo;
    return lo+1<s.size() ? s[lo]+f*(s[lo+1]-s[lo]) : s[lo];
}
void usageQuartiles(const QVector<double>& s, double& q1, double& q3){ q1=pctile(s,0.25); q3=pctile(s,0.75); }
```
Replace Task 3 Step 1's inline math with `usageQuartiles(activeDaysHours, rd.q1HoursUsed, rd.q3HoursUsed);`.

- [ ] **Step 4: Run the test.** Build + run the test target. Expected: PASS (both assertions).

- [ ] **Step 5: Commit.**
```bash
git add oscar/tests/compliancereporttests.* oscar/compliancereport_internal.h oscar/compliancereport.cpp oscar/oscar.pro
git commit -m "test(compliancereport): unit-test usage quartile helper"
```

---

### Task 5: Dialog — "Report style" selector

**Files:**
- Modify: `oscar/compliancereport.h` (enum + member + accessor + slot in `ComplianceReportDialog`)
- Modify: `oscar/compliancereport.cpp` (build combo; wire slot; accessor)

**Interfaces:**
- Produces: `enum class ReportStyle { Classic, AirViewV2 };` and `ReportStyle ComplianceReportDialog::reportStyle() const;` (used by Task 6 dispatch).

- [ ] **Step 1: Header.** In `ComplianceReportDialog` (`compliancereport.h:76-98`): add `public: enum ReportStyle { Classic, AirViewV2 };  ReportStyle reportStyle() const;`, `private slots: void onStyleChanged(int);`, `private: QComboBox *m_styleCombo;`.

- [ ] **Step 2: Build the combo** (in the constructor, before `m_airViewCheck` at `:151`):
```cpp
m_styleCombo = new QComboBox(this);
m_styleCombo->addItem(tr("Classic"),    (int)Classic);
m_styleCombo->addItem(tr("AirView V2"), (int)AirViewV2);
mainLayout->addWidget(new QLabel(tr("Report style:"), this));
mainLayout->addWidget(m_styleCombo);
connect(m_styleCombo, &QComboBox::currentIndexChanged, this, &ComplianceReportDialog::onStyleChanged);
```

- [ ] **Step 3: Slot + accessor.**
```cpp
void ComplianceReportDialog::onStyleChanged(int) {
    bool airv2 = (reportStyle() == AirViewV2);
    m_airViewCheck->setEnabled(!airv2);          // AirView-trim is Classic-only
    if (airv2) m_airViewCheck->setChecked(false);
}
ComplianceReportDialog::ReportStyle ComplianceReportDialog::reportStyle() const {
    return (ReportStyle)m_styleCombo->currentData().toInt();
}
```
Call `onStyleChanged(0)` once after construction (next to the existing `onRangeChanged(0)` at `:165`).

- [ ] **Step 4: BUILD.** Expected: 0 errors.

- [ ] **Step 5: RUN.** Open the dialog. Expected: "Report style" dropdown present; selecting "AirView V2" disables the AirView-format checkbox; selecting "Classic" re-enables it.

- [ ] **Step 6: Commit.**
```bash
git add oscar/compliancereport.h oscar/compliancereport.cpp
git commit -m "feat(compliancereport): add Report style selector to export dialog"
```

---

### Task 6: AirView V2 file skeleton + dispatch (one blank page)

Stand up the new renderer producing a single placeholder A4 page, and wire dispatch. Proves the QPainter→PDF path end-to-end before any layout.

**Files:**
- Create: `oscar/compliancereport_airviewv2.h`, `oscar/compliancereport_airviewv2.cpp`
- Modify: `oscar/compliancereport.cpp` (`exportReport` dispatch), `oscar/oscar.pro:304,484`

**Interfaces:**
- Produces: `void renderAirViewV2Pdf(const ReportData& rd, QPrinter& printer, const QList<SelectedChannel>& channels);`
- Consumes: `ReportData` (Task 2), `ReportStyle` (Task 5).

- [ ] **Step 1: Header** (`compliancereport_airviewv2.h`): GPL header, include guard, `#include "compliancereport_internal.h"`, `class QPrinter;`, declare `renderAirViewV2Pdf(...)` as above.

- [ ] **Step 2: Skeleton .cpp:**
```cpp
#include "compliancereport_airviewv2.h"
#include <QPainter><QPrinter><QPageLayout>
void renderAirViewV2Pdf(const ReportData& rd, QPrinter& printer, const QList<SelectedChannel>& channels) {
    printer.setPageMargins(QMarginsF(0,0,0,0), QPageLayout::Point);   // full-bleed; AirView V2 owns margins
    QPainter p(&printer);
    QRectF page = printer.pageLayout().paintRectPixels(printer.resolution());
    const double s = page.width() / 794.0;     // mockup artboard → page scale
    p.setPen(Qt::black);
    p.drawText(page, Qt::AlignCenter, QStringLiteral("AirView V2 — %1 nights").arg(rd.daysUsed));
    p.end();
}
```

- [ ] **Step 3: Dispatch** in `exportReport` (after `gatherReportData`, replacing the single render path):
```cpp
QPrinter printer(QPrinter::ScreenResolution);
printer.setOutputFormat(QPrinter::PdfFormat);
printer.setOutputFileName(savePath);
printer.setPageSize(QPageSize(QPageSize::A4));
printer.setPageOrientation(QPageLayout::Portrait);
if (dlg.reportStyle() == ComplianceReportDialog::AirViewV2) {
    renderAirViewV2Pdf(rd, printer, selectedChans);
    return;                       // AirView V2 owns the printer fully
}
printer.setPageMargins(QMarginsF(8,4,8,4), QPageLayout::Millimeter);   // existing Classic margins
/* …existing Classic QTextDocument path… */
```
(`#include "compliancereport_airviewv2.h"` at top of `compliancereport.cpp`.)

- [ ] **Step 4: Register in build.** `oscar.pro:304` add `    compliancereport_airviewv2.cpp \` to SOURCES; `:484` add `    compliancereport_airviewv2.h \` to HEADERS.

- [ ] **Step 5: BUILD.** Expected: 0 errors.

- [ ] **Step 6: RUN+PDF.** Export with style "AirView V2" → `/tmp/avtest.pdf`. Expected: 1-page PDF reading "AirView V2 — N nights". Classic still works.

- [ ] **Step 7: Commit.**
```bash
git add oscar/compliancereport_airviewv2.* oscar/compliancereport.cpp oscar/oscar.pro
git commit -m "feat(compliancereport): AirView V2 renderer skeleton + dispatch"
```

---

### Task 7: Design tokens + core draw helpers

Add the palette, fonts, and the structural helpers. Build a temporary "swatch page" to visually verify them, then remove it in Task 8.

**Files:**
- Modify: `oscar/compliancereport_airviewv2.cpp` (anonymous-namespace tokens + helpers)

**Interfaces:**
- Produces (file-local helpers used by Tasks 8–14):
```cpp
struct AV { /* palette: static const QColor NAVY, ACCENT, GREEN, … (full list from spec "Palette") */ };
QFont avSans(double px, int weight=400, double tracking=0); // SF Pro + fallback; tracking via setLetterSpacing
QFont avMono(double px, int weight=400);                    // SF Mono + fallback; tabular figures
void drawBand(QPainter&, const QRectF&, const QString& title, const QString& subtitle, double s);
void drawCard(QPainter&, const QRectF&, bool joinTop, double s);
void drawKVRow(QPainter&, const QRectF&, const QString& label, const QString& value, double s, const QColor* swatch=nullptr);
void drawInfoGrid(QPainter&, const QRectF&, const QVector<QPair<QString,QString>>& cells, int cols, double s);
void drawScorecardCell(QPainter&, const QRectF&, QColor dot, const QString& label, const QString& value, const QString& unit, const QString& caption, bool tintAmber, double s);
void drawGauge(QPainter&, const QRectF&, double fillPct, QColor fill, const QVector<QPair<double,QColor>>& ticks, double s);
void drawInfoCallout(QPainter&, const QRectF&, const QString& text, double s);
void drawFooter(QPainter&, const QRectF&, int pageNo, int pageCount, const QString& methodology, double s);
void drawMasthead(QPainter&, const QRectF&, bool large, const ReportData& rd, const QString& section, double s);
```

- [ ] **Step 1: Palette + fonts.** Define `struct AV` with every `QColor` constant from the spec "Palette" section (core, grey ramp, hairlines, fills, gridlines, event colors, gauge fills, SpO₂ tints, sleep-stage, per-channel triples). Implement `avSans`/`avMono` with the fallback chains; set `QFont::Weight`, `setLetterSpacing(QFont::AbsoluteSpacing, tracking*s)`, and `setStyleHint`. Mono: `f.setStyleStrategy(QFont::PreferQuality)` + tabular via `f.setFeature("tnum",1)` (Qt 6.7+) else accept default.

- [ ] **Step 2: Implement `drawBand`** (canonical pattern — full code):
```cpp
void drawBand(QPainter& p, const QRectF& r, const QString& title, const QString& sub, double s){
    p.fillRect(r, AV::NAVY);
    p.setPen(Qt::white); p.setFont(avSans(11.5*s, 700, 0.14*11.5*s));
    QRectF t = r.adjusted(15*s,0,-15*s,0);
    p.drawText(t, Qt::AlignVCenter|Qt::AlignLeft, title.toUpper());
    if(!sub.isEmpty()){ p.setPen(QColor("#AFC6DD")); p.setFont(avSans(9.5*s,500));
        p.drawText(t, Qt::AlignVCenter|Qt::AlignRight, sub); }
}
```

- [ ] **Step 3: Implement the remaining helpers** following the same pattern, with geometry/colors/typography from the spec "Draw-helper inventory" and the mockup file (`…-mockup.html`): `drawCard`, `drawKVRow`, `drawInfoGrid`, `drawScorecardCell` (dot + 25px mono value + 10px unit + caption + amber tint bg), `drawGauge`, `drawInfoCallout` (3px ACCENT left border), `drawFooter` (+page-7 Methodology variant), `drawMasthead` (large vs compact per spec).

- [ ] **Step 4: Temporary swatch page.** In `renderAirViewV2Pdf`, replace the placeholder text with calls exercising each helper once (a band, a card with KV rows, a scorecard row, a gauge, a callout, a footer).

- [ ] **Step 5: BUILD + RUN+PDF.** Expected: a page showing each component styled per the mockup (navy band, hairline card, mono values, gauge, callout). Eyeball against the mockup's component styling.

- [ ] **Step 6: Commit.**
```bash
git add oscar/compliancereport_airviewv2.cpp
git commit -m "feat(compliancereport): AirView V2 design tokens + core draw helpers"
```

---

### Task 8: Page 1 — Compliance Summary (canonical page pattern)

Fully-worked page; Tasks 9–14 follow this structure. Introduce a `Cursor`/`y`-advancing layout and the page framework.

**Files:**
- Modify: `oscar/compliancereport_airviewv2.cpp`

**Interfaces:**
- Produces: `void avPage1(QPainter&, const QRectF& page, const ReportData& rd, double s);` and a page-frame helper `QRectF avContentRect(const QRectF& page, double s);` (applies the mockup's 34-px side padding). Consumes Task 7 helpers.

- [ ] **Step 1: Page frame + masthead.** `avPage1` computes `QRectF c = avContentRect(page,s);` then `drawMasthead(p, …, /*large*/true, rd, tr("Compliance Report"), s)`, advancing a running `double y`.

- [ ] **Step 2: Patient/device info grid.** Build the cell list from `rd` (patient `rd.patientName`, DOB `rd.dob`, age `rd.age`, period `rd.start..rd.end`, nights `rd.daysUsed`; device `rd.brand+rd.model`, serial `rd.serial`, mode `rd.modeStr`, pressure `rd.pressDisplay`+EPR `rd.relief`, mask `rd.maskDescription`) → `drawInfoGrid(...,/*cols*/5,s)`.

- [ ] **Step 3: Scorecard strip (6 cells).** For each metric call `drawScorecardCell` with value + caption + dot color (green/amber by threshold) + `tintAmber` for O₂/Central per mockup. **Baseline caption gating:** the Residual-AHI caption is `rd.hasBaseline ? QString("−%1% baseline").arg(...) : QString()`.

- [ ] **Step 4: Usage section.** `drawBand(tr("Usage"), …)` then a usage bar chart (defer the chart primitive to Task 9's `drawUsageBars`; for now draw the band + the 6 compliance cells via `drawInfoGrid`/`drawKVRow`). Then Mask&Seal `drawGauge` + leak KV rows, and Respiratory Events `drawGauge` + event KV rows (with leading swatches via `drawKVRow`'s `swatch` arg).

- [ ] **Step 5: Footer.** `drawFooter(p, …, 1, /*N*/7, QString(), s)` (final N wired in Task 14).

- [ ] **Step 6: Call `avPage1`** from `renderAirViewV2Pdf` (remove the swatch page). BUILD + RUN+PDF.
Expected: page 1 matches the mockup's page 1 — masthead, info grid, 6 scorecards, Usage band, two gauges. With a profile that has no diagnostic AHI, the "−…% baseline" caption is absent.

- [ ] **Step 7: Commit.**
```bash
git add oscar/compliancereport_airviewv2.cpp
git commit -m "feat(compliancereport): AirView V2 page 1 — Compliance Summary"
```

---

### Task 9: Chart primitives — usage bars, AHI stacked, box-whisker, pressure

Shared chart helpers used by pages 1,3,4,5. Each takes per-day data already in `rd`'s day maps.

**Files:**
- Modify: `oscar/compliancereport_airviewv2.cpp`

**Interfaces:**
- Produces:
```cpp
void drawUsageBars(QPainter&, const QRectF&, const ReportData& rd, double s);   // green/amber bars + 4h dashed threshold
void drawAhiStacked(QPainter&, const QRectF&, const ReportData& rd, double s);  // stacked events + dashed mean + legend
void drawBoxWhisker(QPainter&, const QRectF&, ChannelID, const ReportData& rd, const QColor triple[3], bool normalBand, double redline, double s);
void drawPressureChart(QPainter&, const QRectF&, const ReportData& rd, double s);
void drawChartLegend(QPainter&, const QRectF&, const QVector<struct AVLegend>&, double s);
```

- [ ] **Step 1: Implement `drawUsageBars`** — port per-day `hours(MT_CPAP)` loop from `renderUsageChart` (`compliancereport.cpp:1304`); bars colored `AV::GREEN`/`AV::AMBER` by `>= rd.complianceThreshold`, rounded top corners, `1.5px dashed GREEN` threshold line + white-bg "4 h" label + green "4" y-tick; mono axis labels; gridlines `AV::GRID` solid + `AV::GRID_DOT` dotted + `AV::BASE` baseline. Geometry from mockup usage chart.

- [ ] **Step 2: Implement `drawAhiStacked`** — port stacking from `renderAhiChart` (`:1356`); segment colors `EV_HYP/EV_CENTRAL/EV_OBS/EV_UNK`; dashed mean line `#312E81 0.9px "4 4" op .55`; call `drawChartLegend`.

- [ ] **Step 3: Implement `drawBoxWhisker`** — per night: min–max bar (`triple[0]`), median–95th band (`triple[1]`), median tick (`triple[2]`); optional `normalBand` (RR green op .08 + "normal 12–20" legend); dashed `redline` when `>0`. Reuse `calcDayAvgPercentile`/`calcDayAvgMax` per the existing leak chart logic. **Cull spans by full extent (x1Px), not onset** (project learning).

- [ ] **Step 4: Implement `drawPressureChart`** (port `renderPressureChart` :1512) and `drawChartLegend`.

- [ ] **Step 5: Wire `drawUsageBars` into Task 8's Usage section** (replace the placeholder). BUILD + RUN+PDF. Expected: page-1 usage chart renders with real bars + threshold.

- [ ] **Step 6: Commit.**
```bash
git add oscar/compliancereport_airviewv2.cpp
git commit -m "feat(compliancereport): AirView V2 chart primitives"
```

---

### Task 10: Page 2 — Therapy & Device Settings

**Files:** Modify `oscar/compliancereport_airviewv2.cpp`.
**Interfaces:** Produces `void avPage2(QPainter&, const QRectF&, const ReportData&, double s);`.

- [ ] **Step 1:** Compact masthead + `drawBand(tr("Therapy"))` + Therapy card via `drawKVRow`s (leak med/95th/max, AHI, AI·HI, apneas by type, RERA, RDI, 95% FL `rd.p95FlowLimit`, CSR).
- [ ] **Step 2:** `drawBand(tr("Usage Detail"))` card: days used, nights ≥/<4 h, total hours, averages, median, **IQR `rd.q1HoursUsed–rd.q3HoursUsed`**.
- [ ] **Step 3:** `drawBand(tr("Device Settings"))` → `drawInfoGrid(...,2)` with the full settings list (model, mode, set pressure, EPR, ramp, mask, humidifier+level, climate, temperature, temp-enable, response, antibacterial filter, smart start, patient view) from `rd` (sourced via existing `getSettingStr`/setting strings).
- [ ] **Step 4:** `drawInfoCallout(...)` clinical note + compact footer (page 2).
- [ ] **Step 5:** Wire `avPage2`; BUILD + RUN+PDF. Expected: page 2 matches mockup. **Commit** `feat(compliancereport): AirView V2 page 2 — Therapy & Device Settings`.

---

### Task 11: Page 3 — Efficacy Trends

**Files:** Modify `oscar/compliancereport_airviewv2.cpp`.
**Interfaces:** Produces `void avPage3(QPainter&, const QRectF&, const ReportData&, double s);`.

- [ ] **Step 1:** Compact masthead + `drawBand(tr("Efficacy Trends"))`.
- [ ] **Step 2:** Three chart cards (each: header strip with title + summary line, then chart): `drawAhiStacked`; `drawBoxWhisker(rd.leakChan, leak-triple, normalBand=false, rd.isTotalLeak?0:rd.leakRedline)`; `drawPressureChart`.
- [ ] **Step 3:** Footer (page 3). Wire `avPage3`; BUILD + RUN+PDF. Expected: matches mockup. **Commit** `feat(compliancereport): AirView V2 page 3 — Efficacy Trends`.

---

### Task 12: Page 4 — Respiratory Channels (honors selected channels)

**Files:** Modify `oscar/compliancereport_airviewv2.cpp`.
**Interfaces:** Produces `void avPage4(QPainter&, const QRectF&, const ReportData&, const QList<SelectedChannel>&, double s);`.

- [ ] **Step 1:** Compact masthead + `drawBand(tr("Respiratory Channel Trends"))`.
- [ ] **Step 2:** For each of Minute Vent / Resp Rate / Tidal Volume that is in `channels` AND `p_profile->hasChannel(cid)`: a chart card via `drawBoxWhisker` with the channel's color triple (RR uses `normalBand=true`). Skip absent channels (no empty card).
- [ ] **Step 3:** Footer (page 4). Wire `avPage4`; BUILD + RUN+PDF. Expected: only available/selected channels appear. **Commit** `feat(compliancereport): AirView V2 page 4 — Respiratory Channels`.

---

### Task 13: Pages 5 & 6 — Oximetry / Sleep & Oximeter Stats (gated)

**Files:** Modify `oscar/compliancereport_airviewv2.cpp`.
**Interfaces:** Produces `bool avPage5(...)` and `bool avPage6(...)` (return `true` if the page was emitted, for page-count math in Task 14). Also `void drawDistributionBar(QPainter&, const QRectF&, const QVector<QPair<double,QColor>>& segs, double s);`, `void drawSpo2Chart(...)`, `void drawSleepStageCards(...)`, `void drawOximeterSubheader(...)`, `void drawStatTable(...)` (stat table also reused by Task 14).

- [ ] **Step 1: Distribution bar + SpO₂ chart helpers.** Implement `drawDistributionBar` (segmented proportions + labels). Implement `drawSpo2Chart`: line + dual dashed thresholds (90% `AMBER "5 4"`, 88% `RED "5 4"`), colored 90/88 axis ticks, bold red period-low marker, per-night low tints.
- [ ] **Step 2: Page 5 (gated `rd.usedDaysOxiAny>0`).** If 0 → `return false`. Else: masthead + `drawBand(tr("Oximetry & Oxygenation"))`; `drawSpo2Chart`; pulse chart (`drawBoxWhisker` pulse triple); Overnight O₂ Summary = `drawDistributionBar` (≥95 GREEN / 90–95 AMBER / <90 RED) + KV (ODI₃ `rd.odi`/`rd.dropPct`, T90 `rd.t90Str`, T88 `rd.t88Secs`, % desat `rd.pctTimeInDesat`, lowest `rd.minSpO2`, avg `rd.avgSpO2Period`); `renderLowestOxygenNight` detail; footer; `return true`.
- [ ] **Step 3: Sleep cards + oximeter subheader + stat table helpers.** Implement `drawSleepStageCards`, `drawOximeterSubheader`, `drawStatTable` (TH_BG header, zebra rows, bold median col, amber-dark min cell — per spec).
- [ ] **Step 4: Page 6 (gated).** If `rd.usedDaysOxiAny==0` AND `!rd.sleep.present` → `return false`. Masthead + `drawBand(tr("Sleep Architecture & Oximeter"))`. **Sleep section only if `rd.sleep.present`** (`drawOximeterSubheader` + `drawSleepStageCards` + `drawDistributionBar` stage bar + KV: TST, bedtime, latency, wake, REM-of-TST). Then Oximeter Statistics subheaders + KV (total/used days, days not used `rd.oxiNotUsedDays`, avg hours/night + recording `rd.oxiRecordingHrsAvg`, avg/min SpO₂, desat drop index `rd.odi`, avg/min/max pulse, pulse change index `rd.pulseChangeIndex`). Footer; `return true`.
- [ ] **Step 5:** Wire pages 5–6; BUILD. RUN+PDF on (a) oximetry profile → both pages, sleep card omitted; (b) CPAP-only → pages 5–6 absent. Expected as stated; no NaN/empty. **Commit** `feat(compliancereport): AirView V2 pages 5-6 — Oximetry, Sleep & Oximeter stats (gated)`.

---

### Task 14: Page 7 + page assembly, numbering, conditional page set

**Files:** Modify `oscar/compliancereport_airviewv2.cpp`.
**Interfaces:** Produces `void avPage7(QPainter&, const QRectF&, const ReportData&, double s);` and finalizes `renderAirViewV2Pdf`.

- [ ] **Step 1: Page 7.** Masthead + Detailed Channel Statistics `drawStatTable` (Channel/Unit/Min/Median/95th/Max for Pressure, MinVent, RespRate, Leak, Snore, Insp/Exp time, Tidal Vol, Pulse, SpO₂ — build rows via `calcDayAvgPercentile`/`calcDayAvgMax`/`unitForChannel`); Additional Indices KV (cumulative apnea `rd.timeInApneaStr`, CSR `rd.timeInCsrStr`, over-redline `rd.timeAboveRedlineStr`+`rd.pctOverRedline`, total hours, min SpO₂, T90, 95% FL); Glossary (static strings); Therapy History `drawStatTable` from `rd.settingsBlocks` (Period/Days/Mode·Pressure/AHI/95th Leak `block.p95Leak`/Min SpO₂ — **Δ-vs-base column only if `rd.hasBaseline`**, value `block.deltaAhiVsBase`); page-7 footer with Methodology paragraph.

- [ ] **Step 2: Assemble with correct numbering.** Build the active page list first (1–4 + 7 always; 5 if `rd.usedDaysOxiAny>0`; 6 if `rd.usedDaysOxiAny>0 || rd.sleep.present`), `N = list.size()`. Iterate: draw page i, pass `(i+1, N)` to its footer, `printer.newPage()` between. Replace the per-page hard-coded `7`.
```cpp
QVector<std::function<void(int,int)>> pages;
pages << [&](int n,int N){ avPage1(p,page,rd,s); /*footer uses n,N*/ };
// …pages 2,3,4… ; conditionally 5,6 ; then 7
// (refactor each avPageX to take (pageNo,pageCount) for its footer)
int N = pages.size();
for (int i=0;i<N;++i){ pages[i](i+1,N); if(i+1<N) printer.newPage(); }
p.end();
```

- [ ] **Step 3: BUILD + RUN+PDF** on the oximetry profile. Expected: footers read "1 / N"…"N / N" with no gaps; page 7 tables + Methodology present; Δ column appears only when a diagnostic AHI is set.

- [ ] **Step 4: Commit.**
```bash
git add oscar/compliancereport_airviewv2.cpp
git commit -m "feat(compliancereport): AirView V2 page 7 + assembly, numbering, gating"
```

---

### Task 15: Verification matrix + polish

**Files:** none expected (fixes only if checks fail).

- [ ] **Step 1: Full oximetry profile (no sleep-stage).** RUN+PDF. Expected: pages 1–5,7 + page 6 (Oximeter Stats only); numbers match Classic for the same range.
- [ ] **Step 2: Profile WITH `MT_SLEEPSTAGE`** (e.g. Zeo testdata). Expected: page 6 shows the sleep card with real values.
- [ ] **Step 3: CPAP-only, no oximeter.** Expected: pages 5–6 omitted; numbering correct; no NaN/"—" gaps where data exists.
- [ ] **Step 4: No diagnostic AHI.** Expected: p1 baseline caption + p7 Δ column absent. Re-run with one entered → both appear.
- [ ] **Step 5: Short 3-day range.** Expected: charts compact, pagination intact.
- [ ] **Step 6: Classic regression.** Generate Classic for the same range; compare to a pre-Task-1 Classic PDF. Expected: visually identical.
- [ ] **Step 7: Font fallback sanity** — confirm layout uses font metrics (no clipped text) so off-macOS fallback won't break (inspection only).
- [ ] **Step 8: Final commit** (if any polish fixes).
```bash
git add -A && git commit -m "test(compliancereport): AirView V2 verification matrix + polish"
```

---

## Self-Review

**Spec coverage:** Dialog selector → T5. Shared `ReportData`/extraction → T2; anon-namespace hoist → T1; derived/gated fields → T3; quartile test → T4. QPainter engine + dispatch → T6. Design system (palette/fonts/helpers) → T7; charts → T9. Pages 1–7 → T8,10,11,12,13,14. Conditional rendering/numbering → T13,T14. Sleep gating → T3,T13; baseline gating → T3,T8,T14. Build wiring → T1,T4,T6. Verification matrix → T15. All spec sections mapped.

**Placeholder scan:** Page tasks (10–14) intentionally reference the committed mockup file for exact pixel geometry rather than duplicating ~800 lines of coordinates — but each names the exact helpers, `ReportData` fields, gating predicates, and section order, and T7/T8/T9 give fully-worked code as the pattern. No "TBD/handle edge cases/add validation" left.

**Type consistency:** `ReportData`/`SleepStageStats`/`SettingsChangeBlock` fields are defined once (T1–T3) and referenced by exact name in pages. Helper signatures in T7/T9/T13 match their call sites. `renderAirViewV2Pdf`, `gatherReportData`, `reportStyle()`, `usageQuartiles` names are consistent across tasks. `ReportStyle` enum (`Classic`/`AirViewV2`) consistent T5↔T6.
