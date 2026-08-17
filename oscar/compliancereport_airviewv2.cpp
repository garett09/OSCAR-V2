/* Compliance Report Exporter Source
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "compliancereport_airviewv2.h"
#include "version.h"
#include "SleepLib/schema.h"

#include <algorithm>
#include <functional>

// ResMed device-setting channels (defined in resmed_loader.cpp; same externs as
// compliancereport.cpp so getSettingStr can be called here).
extern ChannelID RMAS1x_Comfort;
extern ChannelID RMS9_SmartStart;
extern ChannelID RMS9_HumidStatus;
extern ChannelID RMS9_HumidLevel;
extern ChannelID RMS9_Temp;
extern ChannelID RMS9_TempEnable;
extern ChannelID RMS9_ABFilter;
extern ChannelID RMS9_ClimateControl;
extern ChannelID RMS9_Mask;
extern ChannelID RMS9_RampEnable;
extern ChannelID RMAS11_PtView;

#include <QColor>
#include <QDate>
#include <QFont>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QPageLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QPrinter>
#include <QRectF>
#include <QString>
#include <QtMath>
#include <QVector>
#include <QPair>
#include <QObject>

// ============================================================================
// AirView V2 design system (file-local). Every helper here is reusable by the
// 7 page builders to come; the only externally visible symbol stays
// renderAirViewV2Pdf(). Geometry is expressed in the mockup's 794px-artboard
// coordinates and scaled by `s = page.width()/794.0` at every call site.
// ============================================================================
namespace {

// --- Step 1: palette ---------------------------------------------------------
namespace AV {
    // Core
    const QColor NAVY   ("#0B3D74");
    const QColor ACCENT ("#1F6FB2");
    const QColor GREEN  ("#2E8B57");
    const QColor AMBER  ("#C9881F");
    const QColor RED    ("#C0392B");
    const QColor INK    ("#1A2733");
    // Ink / grey ramp
    const QColor GREY1  ("#54616B");
    const QColor GREY2  ("#8693A0");
    const QColor GREY3  ("#9AA6B0");
    const QColor CAPTION("#6B7884");
    const QColor BODY   ("#454F58");
    const QColor TBODY  ("#38454F");
    const QColor LEG    ("#97A2AD");
    const QColor METH   ("#7A8794");
    // Hairlines / borders
    const QColor HAIR1  ("#C7D2DA");
    const QColor HAIR2  ("#DCE4EA");
    const QColor HAIR3  ("#E6ECF1");
    const QColor TBORDER("#ECF1F4");
    // Fills
    const QColor FILL_BLUE ("#F4F8FB");
    const QColor FILL_GREY ("#F7FAFC");
    const QColor TH_BG     ("#EAF2F9");
    const QColor TINT_AMBER("#FCF8F0");
    const QColor TINT_RED  ("#FBEDEC");
    // Gridlines
    const QColor GRID    ("#EEF0F5");
    const QColor GRID_DOT("#ECF1F4");
    const QColor BASE    ("#C7D2DA");
    // Event colors
    const QColor EV_CENTRAL("#6E6FE6");
    const QColor EV_HYP    ("#AEB0F2");
    const QColor EV_OBS    ("#312E81");
    const QColor EV_UNK    ("#C5C9D4");
    // Misc accent text
    const QColor BAND_SUB("#AFC6DD");
    const QColor AMBER_DK("#9A6F1A");
    // Sleep-stage
    const QColor SLEEP_DEEP ("#0B3D74");
    const QColor SLEEP_LIGHT("#5E93C4");
    const QColor SLEEP_REM  ("#C9881F");
    const QColor SLEEP_AWAKE("#C2CDD6");

    // Per-channel chart colours: { median–95th band, median tick }
    struct ChannelColors { QColor band; QColor median; };
    const ChannelColors LEAK_COLORS    {QColor("#2FA37A"), QColor("#17684C")};
    const ChannelColors MINVENT_COLORS {QColor("#3B82C4"), QColor("#1E4E79")};
    const ChannelColors RESPRATE_COLORS{QColor("#8B5CC4"), QColor("#4A2B7A")};
    const ChannelColors TIDALVOL_COLORS{QColor("#DD8636"), QColor("#955018")};
    const ChannelColors PULSE_COLORS   {QColor("#D2596A"), QColor("#8A2A38")};
    const ChannelColors PRESSURE_COLORS{QColor("#6F8FBF"), QColor("#284B7A")};

    // SpO2 nightly-range bar
    const QColor SPO2_NORMAL("#C2E2EC");
} // namespace AV

// --- Step 2: fonts -----------------------------------------------------------
static QFont avSans(double px, int weight = QFont::Normal, double trackingPx = 0.0) {
    QFont f("SF Pro Text");
    f.setStyleHint(QFont::SansSerif);
    if (!QFontInfo(f).family().contains("SF Pro", Qt::CaseInsensitive)) f.setFamily("Helvetica Neue");
    f.setPixelSize(qMax(1, (int)qRound(px)));
    f.setWeight((QFont::Weight)weight);
    if (trackingPx != 0.0) f.setLetterSpacing(QFont::AbsoluteSpacing, trackingPx);
    return f;
}
static QFont avMono(double px, int weight = QFont::Normal) {
    QFont f("SF Mono");
    f.setStyleHint(QFont::Monospace);
    if (!QFontInfo(f).family().contains("SF Mono", Qt::CaseInsensitive)) f.setFamily("Menlo");
    f.setPixelSize(qMax(1, (int)qRound(px)));
    f.setWeight((QFont::Weight)weight);
    return f;
}

// --- Step 3: canonical helper ------------------------------------------------

// Corner radius of the module system (bands, cards, callouts, chart frames),
// in artboard px. One token so every rounded element matches.
static const double AV_RAD = 5.0;

static void drawBand(QPainter& p, const QRectF& r, const QString& title, const QString& sub, double s) {
    // Rounded top corners; square bottom (the card below butts flush).
    const double rad = AV_RAD*s;
    QPainterPath bp;
    bp.addRoundedRect(r, rad, rad);
    p.fillPath(bp, AV::NAVY);
    p.fillRect(QRectF(r.left(), r.top() + rad, r.width(), r.height() - rad), AV::NAVY);
    QRectF t = r.adjusted(15*s, 0, -15*s, 0);
    p.setPen(Qt::white);
    p.setFont(avSans(11.5*s, QFont::Bold, 0.14*11.5*s));
    p.drawText(t, Qt::AlignVCenter | Qt::AlignLeft, title.toUpper());
    if (!sub.isEmpty()) {
        p.setPen(AV::BAND_SUB);
        p.setFont(avSans(9.5*s, QFont::Medium));
        p.drawText(t, Qt::AlignVCenter | Qt::AlignRight, sub);
    }
}

// --- Step 4: structural helpers ---------------------------------------------

// HAIR1 1px border; joinTop suppresses the top edge so the card butts a band
// (bottom corners rounded to close the band+card module); standalone cards are
// rounded on all four corners.
static void drawCard(QPainter& p, const QRectF& r, bool joinTop, double s) {
    const double a = 0.5*s, rad = AV_RAD*s;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(AV::HAIR1, 1*s));
    if (joinTop) {
        QPainterPath cp;
        cp.moveTo(r.left()+a,  r.top());
        cp.lineTo(r.left()+a,  r.bottom()-a-rad);
        cp.quadTo(r.left()+a,  r.bottom()-a, r.left()+a+rad, r.bottom()-a);
        cp.lineTo(r.right()-a-rad, r.bottom()-a);
        cp.quadTo(r.right()-a, r.bottom()-a, r.right()-a, r.bottom()-a-rad);
        cp.lineTo(r.right()-a, r.top());
        p.drawPath(cp);
    } else {
        p.drawRoundedRect(r.adjusted(a, a, -a, -a), rad, rad);
    }
}

// Chart-card chrome shared by pages 3/4/5: rounded HAIR1 frame with a
// FILL_BLUE header strip (clipped to the rounded top) and a HAIR2 rule.
static void avCardChrome(QPainter& p, const QRectF& card, double hdrH, double s) {
    const double a = 0.5*s, rad = AV_RAD*s;
    QPainterPath clip;
    clip.addRoundedRect(card, rad, rad);
    p.save();
    p.setClipPath(clip);
    p.fillRect(QRectF(card.left(), card.top(), card.width(), hdrH), AV::FILL_BLUE);
    p.restore();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(AV::HAIR2, 1*s));
    p.drawLine(QPointF(card.left(), card.top() + hdrH), QPointF(card.right(), card.top() + hdrH));
    p.setPen(QPen(AV::HAIR1, 1*s));
    p.drawRoundedRect(card.adjusted(a, a, -a, -a), rad, rad);
}

// label sans GREY1 12px left; value mono INK 12px right; bottom HAIR3 hairline;
// optional 8px rounded swatch before the label. The value is shrunk-to-fit
// (down to a floor) and elided only as a last resort, so a long label+value
// pair (e.g. "Time < 90% (T90)" + "00:03:48" in a half-width column) never
// collides.
static void drawKVRow(QPainter& p, const QRectF& r, const QString& label, const QString& value,
                      double s, const QColor* leadingSwatch = nullptr) {
    double labelX = r.left();
    if (leadingSwatch) {
        const double d = 8*s;
        QPainterPath sp;
        sp.addRoundedRect(QRectF(r.left(), r.center().y() - d/2, d, d), 2*s, 2*s);
        p.fillPath(sp, *leadingSwatch);
        labelX = r.left() + d + 7*s;
    }
    const QFont lf = avSans(12*s, QFont::Normal);
    p.setFont(lf);
    p.setPen(AV::GREY1);
    p.drawText(QRectF(labelX, r.top(), r.width() - (labelX - r.left()), r.height()),
               Qt::AlignVCenter | Qt::AlignLeft, label);
    const double availW = r.right() - labelX
                          - QFontMetricsF(lf).horizontalAdvance(label) - 8*s;
    double fpx = 12*s;
    QFont vf = avMono(fpx, QFont::DemiBold);
    QFontMetricsF fm(vf);
    while (fm.horizontalAdvance(value) > availW && fpx > 9.0*s) {
        fpx -= 0.5*s; vf = avMono(fpx, QFont::DemiBold); fm = QFontMetricsF(vf);
    }
    const QString shown = (fm.horizontalAdvance(value) > availW)
        ? fm.elidedText(value, Qt::ElideRight, qMax(availW, 0.0)) : value;
    p.setFont(vf);
    p.setPen(AV::INK);
    p.drawText(r, Qt::AlignVCenter | Qt::AlignRight, shown);
    p.setPen(QPen(AV::HAIR3, 1*s));
    p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.right(), r.bottom()));
}

// Bordered grid: per cell label .icl (GREY2 8.5px uppercase .1em) over value .icv
// (INK 13.5px 600); HAIR2 internal dividers; HAIR1 outer border.
// colWeights (optional): per-column width proportions, like the mockup's
// CSS grid-template-columns. Empty/mismatched → equal columns. Values are
// shrunk-to-fit (down to a floor) and elided only as a last resort, so a long
// value (device name, pressure, date range) never hard-clips the cell edge.
static void drawInfoGrid(QPainter& p, const QRectF& r, const QVector<QPair<QString,QString>>& cells,
                         int cols, double s, const QVector<double>& colWeights = QVector<double>(),
                         bool roundTop = false, bool roundBottom = false) {
    if (cols < 1) cols = 1;
    const int n = cells.size();
    int rows = (n + cols - 1) / cols;
    if (rows < 1) rows = 1;
    const double ch = r.height() / rows;
    const double padX = 14*s;
    // Vertically center the label+value pair so cells keep top/bottom margin
    // even when rows are compact (e.g. the page-2 device grid).
    const double blockH = 8.5*s + 3*s + 13.5*s;
    const double padTop = qMax(8.0*s, (ch - blockH) / 2.0);

    // Column x-offsets (relative to r.left()) from weights; default equal.
    QVector<double> w = colWeights;
    if (w.size() != cols) { w.clear(); for (int c = 0; c < cols; c++) w.append(1.0); }
    double wsum = 0.0; for (double v : w) wsum += v;
    if (wsum <= 0.0) wsum = cols;
    QVector<double> colX(cols + 1, 0.0);
    { double acc = 0.0; for (int c = 0; c < cols; c++) { colX[c] = acc; acc += r.width() * w[c] / wsum; } colX[cols] = r.width(); }

    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < n; i++) {
        const int col = i % cols, row = i / cols;
        const QRectF cell(r.left() + colX[col], r.top() + row*ch, colX[col+1] - colX[col], ch);
        const double availW = cell.width() - 2*padX;
        // label
        p.setFont(avSans(8.5*s, QFont::DemiBold, 0.1*8.5*s));
        p.setPen(AV::GREY2);
        p.drawText(QRectF(cell.left()+padX, cell.top()+padTop, availW, 12*s),
                   Qt::AlignLeft | Qt::AlignTop, cells[i].first.toUpper());
        // value — shrink font to fit availW (floor 10px), then elide if still too wide
        double fpx = 13.5*s;
        QFont vf = avSans(fpx, QFont::DemiBold);
        QFontMetricsF fm(vf);
        const QString val = cells[i].second;
        while (fm.horizontalAdvance(val) > availW && fpx > 10.0*s) {
            fpx -= 0.5*s; vf = avSans(fpx, QFont::DemiBold); fm = QFontMetricsF(vf);
        }
        const QString shown = (fm.horizontalAdvance(val) > availW)
            ? fm.elidedText(val, Qt::ElideRight, availW) : val;
        p.setFont(vf);
        p.setPen(AV::INK);
        p.drawText(QRectF(cell.left()+padX, cell.top()+padTop + 8.5*s + 3*s, availW, 15*s),
                   Qt::AlignLeft | Qt::AlignTop, shown);
    }
    // internal dividers
    p.setPen(QPen(AV::HAIR2, 1*s));
    for (int c = 1; c < cols; c++) {
        const double x = r.left() + colX[c];
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.top() + rows*ch));
    }
    for (int rr = 1; rr < rows; rr++) {
        const double yy = r.top() + rr*ch;
        p.drawLine(QPointF(r.left(), yy), QPointF(r.right(), yy));
    }
    // outer border — corners round only on the module's open edges
    p.setPen(QPen(AV::HAIR1, 1*s));
    const QRectF br = r.adjusted(0.5*s, 0.5*s, -0.5*s, -0.5*s);
    if (!roundTop && !roundBottom) {
        p.drawRect(br);
    } else {
        const double rad = AV_RAD*s;
        QPainterPath op;
        op.addRoundedRect(br, rad, rad);
        if (!roundTop)    op.addRect(QRectF(br.left(), br.top(), br.width(), rad));
        if (!roundBottom) op.addRect(QRectF(br.left(), br.bottom() - rad, br.width(), rad));
        p.drawPath(op.simplified());
    }
}

// Generic statistics table (page 7). The rect's top/left/width set the table
// origin and width; the total height is computed from a fixed header + row
// height (the caller advances y by headerH + rows*rowH). First column ~28% of
// the width (left-aligned label, sans); the remaining columns share the rest
// equally (right-aligned, mono). Header row: TH_BG bg, NAVY DemiBold uppercase.
// Body: TBODY text, FILL_GREY zebra on alternate rows, per-row TBORDER bottom
// hairline. Emphasis (column detected by header text): the "MEDIAN" column is
// rendered DemiBold INK; the "MIN" column value in AMBER_DK.
static const double AV_STAT_HEADER_H = 22.0;   // header strip height (artboard px)
static const double AV_STAT_ROW_H    = 20.0;   // body row height (artboard px)
// colWeights (optional): per-column width proportions. Empty/mismatched → the
// default (first column 28%, the rest equal). Cell text is shrunk-to-fit (down
// to an 8px floor) and elided only as a last resort, so extra columns never clip.
static void drawStatTable(QPainter& p, const QRectF& r, const QStringList& headers,
                          const QVector<QStringList>& rows, double s,
                          const QVector<double>& colWeights = QVector<double>()) {
    const int cols = headers.size();
    if (cols < 1) return;
    const double headerH = AV_STAT_HEADER_H*s, rowH = AV_STAT_ROW_H*s, padX = 8*s;

    // Column left-edges (relative to r.left()).
    QVector<double> colL(cols + 1, 0.0);
    if (colWeights.size() == cols) {
        double wsum = 0.0; for (double v : colWeights) wsum += v;
        if (wsum <= 0.0) wsum = cols;
        double acc = 0.0; for (int c = 0; c < cols; c++) { colL[c] = acc; acc += r.width() * colWeights[c] / wsum; }
        colL[cols] = r.width();
    } else {
        const double firstW = r.width() * 0.28;
        const double restW  = (cols > 1) ? (r.width() - firstW) / (cols - 1) : 0.0;
        for (int c = 0; c < cols; c++) colL[c] = (c == 0) ? 0.0 : firstW + (c - 1) * restW;
        colL[cols] = r.width();
    }
    auto colRect = [&](int c, double top, double h) -> QRectF {
        return QRectF(r.left() + colL[c], top, colL[c+1] - colL[c], h);
    };
    // Draw text shrunk-to-fit the cell (so an extra column never hard-clips).
    auto drawFit = [&](const QRectF& cell, int align, const QString& text, QFont f) {
        const double avail = cell.width();
        double px = f.pixelSize();
        QFontMetricsF fm(f);
        while (fm.horizontalAdvance(text) > avail && px > 8.0*s) {
            px -= 0.5*s; f.setPixelSize(qMax(1, (int)qRound(px))); fm = QFontMetricsF(f);
        }
        const QString shown = (fm.horizontalAdvance(text) > avail)
            ? fm.elidedText(text, Qt::ElideRight, avail) : text;
        p.setFont(f);
        p.drawText(cell, align, shown);
    };

    // Detect the Median / Min columns by header text for the spec emphases.
    int medianCol = -1, minCol = -1;
    for (int c = 0; c < cols; c++) {
        const QString h = headers[c].trimmed().toLower();
        if (h == QStringLiteral("median")) medianCol = c;
        if (h == QStringLiteral("min"))    minCol = c;
    }

    // Header row.
    p.fillRect(QRectF(r.left(), r.top(), r.width(), headerH), AV::TH_BG);
    p.setPen(AV::NAVY);
    for (int c = 0; c < cols; c++) {
        const QRectF cell = colRect(c, r.top(), headerH).adjusted(padX, 0, -padX, 0);
        const int align = (c == 0 ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter;
        drawFit(cell, align, headers[c].toUpper(), avSans(9.5*s, QFont::DemiBold, 0.07*9.5*s));
    }

    // Body rows.
    double y = r.top() + headerH;
    for (int ri = 0; ri < rows.size(); ri++) {
        const QStringList& row = rows[ri];
        if (ri % 2 == 1) p.fillRect(QRectF(r.left(), y, r.width(), rowH), AV::FILL_GREY);
        for (int c = 0; c < cols && c < row.size(); c++) {
            const QRectF cell = colRect(c, y, rowH).adjusted(padX, 0, -padX, 0);
            if (c == 0) {
                p.setPen(AV::TBODY);
                drawFit(cell, Qt::AlignLeft | Qt::AlignVCenter, row[c], avSans(11*s, QFont::Normal));
            } else {
                const bool isMedian = (c == medianCol);
                const bool isMin    = (c == minCol);
                p.setPen(isMedian ? AV::INK : (isMin ? AV::AMBER_DK : AV::TBODY));
                drawFit(cell, Qt::AlignRight | Qt::AlignVCenter, row[c],
                        avMono(11*s, isMedian ? QFont::DemiBold : QFont::Normal));
            }
        }
        p.setPen(QPen(AV::TBORDER, 1*s));
        p.drawLine(QPointF(r.left(), y + rowH), QPointF(r.right(), y + rowH));
        y += rowH;
    }
}

// bg by band (0 FILL_GREY, 1 TINT_AMBER, 2 TINT_RED); 3px status top rule in the
// dot color; 8px status dot + label (GREY2 8px uppercase .08em); value mono 25px
// INK; unit 10px GREY2 suffix; caption 8.5px CAPTION. The dot/band MUST be
// value-driven by the caller — never a fixed color on a clinical scorecard.
static void drawScorecardCell(QPainter& p, const QRectF& r, const QColor& dot,
                              const QString& label, const QString& value, const QString& unit,
                              const QString& caption, int band, double s) {
    p.fillRect(r, band >= 2 ? AV::TINT_RED : band == 1 ? AV::TINT_AMBER : AV::FILL_GREY);
    p.fillRect(QRectF(r.left(), r.top(), r.width(), 3*s), dot);
    const double padX = 13*s, padTop = 14*s, dotD = 8*s;
    const double cx = r.left() + padX, cy = r.top() + padTop;

    // status dot
    p.setBrush(dot);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(cx, cy, dotD, dotD));
    p.setBrush(Qt::NoBrush);

    // label
    p.setFont(avSans(8*s, QFont::DemiBold, 0.08*8*s));
    p.setPen(AV::GREY2);
    p.drawText(QRectF(cx + dotD + 6*s, cy - 1*s, r.right() - padX - (cx + dotD + 6*s), dotD + 2*s),
               Qt::AlignVCenter | Qt::AlignLeft, label.toUpper());

    // value + unit suffix (baseline-aligned via shared bottom)
    const double valTop = cy + dotD + 7*s;
    const double valBoxH = 25*s * 1.1;
    p.setFont(avMono(25*s, QFont::DemiBold));
    p.setPen(AV::INK);
    QFontMetricsF vm(p.font());
    const double vw = vm.horizontalAdvance(value);
    p.drawText(QRectF(cx, valTop, r.width() - 2*padX, valBoxH), Qt::AlignLeft | Qt::AlignBottom, value);
    if (!unit.isEmpty()) {
        p.setFont(avSans(10*s, QFont::Normal));
        p.setPen(AV::GREY2);
        p.drawText(QRectF(cx + vw + 3*s, valTop, r.width(), valBoxH),
                   Qt::AlignLeft | Qt::AlignBottom, unit);
    }

    // caption
    p.setFont(avSans(8.5*s, QFont::Normal));
    p.setPen(AV::CAPTION);
    p.drawText(QRectF(cx, valTop + valBoxH + 1*s, r.width() - 2*padX, 12*s),
               Qt::AlignLeft | Qt::AlignTop, caption);
}

// FILL_BLUE bg, HAIR1 border, 3px ACCENT left border, mono "i" ACCENT glyph +
// BODY 10.5px wrapped text.
static void drawInfoCallout(QPainter& p, const QRectF& r, const QString& text, double s) {
    const double rad = AV_RAD*s;
    QPainterPath cp;
    cp.addRoundedRect(r, rad, rad);
    p.fillPath(cp, AV::FILL_BLUE);
    p.save();
    p.setClipPath(cp);
    p.fillRect(QRectF(r.left(), r.top(), 3*s, r.height()), AV::ACCENT);
    p.restore();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(AV::HAIR1, 1*s));
    p.drawRoundedRect(r.adjusted(0.5*s, 0.5*s, -0.5*s, -0.5*s), rad, rad);

    const double padX = 16*s, padTop = 9*s;
    p.setFont(avMono(12*s, QFont::Bold));
    p.setPen(AV::ACCENT);
    p.drawText(QRectF(r.left() + padX, r.top() + padTop, 12*s, 16*s),
               Qt::AlignLeft | Qt::AlignTop, QStringLiteral("i"));

    const double textX = r.left() + padX + 12*s + 11*s;
    p.setFont(avSans(10.5*s, QFont::Normal));
    p.setPen(AV::BODY);
    p.drawText(QRectF(textX, r.top() + padTop, r.right() - padX - textX, r.height() - 2*padTop),
               Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, text);
}

// top HAIR3 rule; left disclaimer (GREY3 8.5px); right "n / N" mono GREY3; if a
// methodology is supplied, a full-width para (METH 8.5px, bold "Methodology."
// lead-in) is drawn ABOVE the standard footer line.
static void drawFooter(QPainter& p, const QRectF& r, int pageNo, int pageCount,
                       const QString& methodology, double s) {
    const double padX = 34*s;
    const double L = r.left() + padX, R = r.right() - padX;
    const double lineH = 20*s;
    const double lineTop = r.bottom() - lineH;

    if (!methodology.isEmpty()) {
        const QString full = QStringLiteral("Methodology.  ") + methodology;
        const QRectF mr(L, r.top(), R - L, lineTop - r.top() - 6*s);
        p.setFont(avSans(8.5*s, QFont::Normal));
        p.setPen(AV::METH);
        p.drawText(mr, Qt::TextWordWrap | Qt::AlignTop | Qt::AlignLeft, full);
        // bold lead-in overdraw
        p.setFont(avSans(8.5*s, QFont::Bold));
        p.drawText(mr, Qt::AlignTop | Qt::AlignLeft, QStringLiteral("Methodology."));
    }

    p.setPen(QPen(AV::HAIR3, 1*s));
    p.drawLine(QPointF(L, lineTop), QPointF(R, lineTop));

    p.setFont(avSans(8.5*s, QFont::Normal));
    p.setPen(AV::GREY3);
    const QString disc = QStringLiteral("OSCAR %1 · open-source CPAP report software · "
                                        "device-estimated indices, not a diagnostic polysomnogram")
                             .arg(getVersion().minimalString());
    p.drawText(QRectF(L, lineTop, (R - L) * 0.85, lineH), Qt::AlignVCenter | Qt::AlignLeft, disc);

    p.setFont(avMono(8.5*s, QFont::Normal));
    p.drawText(QRectF(L, lineTop, R - L, lineH), Qt::AlignVCenter | Qt::AlignRight,
               QStringLiteral("%1 / %2").arg(pageNo).arg(pageCount));
}

// The mockup masthead mark: SVG path "M1 7.5 Q4.3 1 7.6 7.5 T14.2 7.5 T20.8 7.5
// T27.4 7.5" on a 0 0 28 15 viewBox, stroke ACCENT. T (smooth-quad) control
// points are pre-reflected here.
static void drawWaveMark(QPainter& p, const QRectF& box, double strokeW) {
    const double sx = box.width() / 28.0, sy = box.height() / 15.0;
    auto P = [&](double vx, double vy) { return QPointF(box.left() + vx*sx, box.top() + vy*sy); };
    QPainterPath path;
    path.moveTo(P(1, 7.5));
    path.quadTo(P(4.3, 1),    P(7.6, 7.5));
    path.quadTo(P(10.9, 14),  P(14.2, 7.5));
    path.quadTo(P(17.5, 1),   P(20.8, 7.5));
    path.quadTo(P(24.1, 14),  P(27.4, 7.5));
    QPen pen(AV::ACCENT, strokeW);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setBrush(Qt::NoBrush);
    p.setPen(pen);
    p.drawPath(path);
}

// waveform mark + "OSCAR"/"Sleep Therapy Insights" wordmark.
// large(p1): right = section title (NAVY 15px .16em) + "Generated <date> · OSCAR <ver>".
// compact(p2-7): smaller wordmark w/ left-border separator; right = section title
// (NAVY 11px .14em) + "<patient> · <date range>".
static void drawMasthead(QPainter& p, const QRectF& r, bool large, const ReportData& rd,
                         const QString& section, double s) {
    const double padX = 34*s;
    const double left = r.left() + padX, right = r.right() - padX, cy = r.center().y();
    const QString ver = getVersion().minimalString();

    QString dateRange;
    if (rd.start.isValid() && rd.end.isValid())
        dateRange = rd.start.toString("MMM d") + QStringLiteral(" – ") + rd.end.toString("MMM d, yyyy");

    if (large) {
        const double mw = 28*s, mh = 15*s;
        drawWaveMark(p, QRectF(left, cy - mh/2, mw, mh), 2*s);

        const double tx = left + mw + 11*s;
        const double oscarH = 21*s, tagH = 8*s, gap = 3*s;
        const double top = cy - (oscarH + gap + tagH) / 2;
        p.setFont(avSans(21*s, QFont::Bold, 0.04*21*s));
        p.setPen(AV::NAVY);
        p.drawText(QRectF(tx, top, 320*s, oscarH*1.25), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("OSCAR"));
        p.setFont(avSans(8*s, QFont::Medium, 0.2*8*s));
        p.setPen(AV::GREY2);
        p.drawText(QRectF(tx, top + oscarH + gap, 320*s, tagH*1.6),
                   Qt::AlignLeft | Qt::AlignTop, QStringLiteral("Sleep Therapy Insights").toUpper());

        const double titleH = 15*s, subH = 10*s;
        const double rtop = cy - (titleH + 3*s + subH) / 2;
        p.setFont(avSans(15*s, QFont::Bold, 0.16*15*s));
        p.setPen(AV::NAVY);
        p.drawText(QRectF(left, rtop, right - left, titleH*1.35), Qt::AlignRight | Qt::AlignTop, section.toUpper());
        p.setFont(avMono(10*s, QFont::Normal));
        p.setPen(AV::GREY2);
        const QString gen = QStringLiteral("Generated %1 · OSCAR %2")
                                .arg(QDate::currentDate().toString("MMM d, yyyy")).arg(ver);
        p.drawText(QRectF(left, rtop + titleH + 3*s, right - left, subH*1.5), Qt::AlignRight | Qt::AlignTop, gen);

        p.setPen(QPen(AV::NAVY, 2*s));
        p.drawLine(QPointF(r.left(), r.bottom() - 1*s), QPointF(r.right(), r.bottom() - 1*s));
    } else {
        const double mw = 22*s, mh = 12*s;
        drawWaveMark(p, QRectF(left, cy - mh/2, mw, mh), 2.0 * mw / 28.0);

        double x = left + mw + 9*s;
        p.setFont(avSans(13*s, QFont::Bold, 0.03*13*s));
        p.setPen(AV::NAVY);
        QFontMetricsF om(p.font());
        const QString oscar = QStringLiteral("OSCAR");
        const double ow = om.horizontalAdvance(oscar);
        p.drawText(QRectF(x, cy - 9*s, ow + 6*s, 18*s), Qt::AlignVCenter | Qt::AlignLeft, oscar);
        x += ow + 9*s;
        p.setPen(QPen(AV::HAIR1, 1*s));
        p.drawLine(QPointF(x, cy - 7*s), QPointF(x, cy + 7*s));
        x += 9*s;
        p.setFont(avSans(8*s, QFont::Medium, 0.16*8*s));
        p.setPen(AV::GREY3);
        p.drawText(QRectF(x, cy - 8*s, 320*s, 16*s), Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("Sleep Therapy Insights").toUpper());

        const double titleH = 11*s, subH = 9*s;
        const double rtop = cy - (titleH + 2*s + subH) / 2;
        p.setFont(avSans(11*s, QFont::DemiBold, 0.14*11*s));
        p.setPen(AV::NAVY);
        p.drawText(QRectF(left, rtop, right - left, titleH*1.35), Qt::AlignRight | Qt::AlignTop, section.toUpper());
        p.setFont(avMono(9*s, QFont::Normal));
        p.setPen(AV::GREY3);
        const QString sub = rd.patientName.isEmpty() ? dateRange
                                                     : (rd.patientName + QStringLiteral(" · ") + dateRange);
        p.drawText(QRectF(left, rtop + titleH + 2*s, right - left, subH*1.5), Qt::AlignRight | Qt::AlignTop, sub);

        p.setPen(QPen(AV::HAIR1, 1*s));
        p.drawLine(QPointF(r.left(), r.bottom() - 0.5*s), QPointF(r.right(), r.bottom() - 0.5*s));
    }
}

// --- Step 5: page-frame + formatting helpers --------------------------------

// 34px side padding (mockup); top/bottom handled per-section.
static QRectF avContentRect(const QRectF& page, double s) {
    return page.adjusted(34*s, 0, -34*s, 0);
}

// Decimal hours → "H:mm" (e.g. 7.15 → "7:09", 214.28 → "214:17").
static QString hmm(double hours) {
    if (hours < 0) hours = 0;
    const int totalMin = qRound(hours * 60.0);
    return QStringLiteral("%1:%2").arg(totalMin / 60).arg(totalMin % 60, 2, 10, QChar('0'));
}

// Seconds → "M min" (e.g. 1200 → "20 min").
static QString minsFromSecs(double secs) {
    return QStringLiteral("%1 min").arg(qRound(secs / 60.0));
}

// Best consecutive-30-night compliance window (CMS-style criterion: ≥ threshold
// hours on ≥ 70% of nights in a consecutive 30-day span). The whole-period
// adherence % is NOT the payer criterion, so page 2 reports this explicitly.
struct AvCmsWindow { bool valid; bool met; QDate start; int pct; };
static AvCmsWindow avBestCmsWindow(const ReportData& rd) {
    if (rd.totalDays < 30 || !rd.start.isValid()) return { false, false, QDate(), 0 };
    QVector<int> ok(rd.totalDays, 0);
    for (int i = 0; i < rd.totalDays; i++) {
        const QDate d = rd.start.addDays(i);
        ok[i] = (rd.daysMap.contains(d)
                 && rd.daysMap[d]->hours(MT_CPAP) >= rd.complianceThreshold) ? 1 : 0;
    }
    int run = 0, best = -1;
    QDate bestStart;
    for (int i = 0; i < rd.totalDays; i++) {
        run += ok[i];
        if (i >= 30) run -= ok[i - 30];
        if (i >= 29 && run > best) { best = run; bestStart = rd.start.addDays(i - 29); }
    }
    const int pct = qRound(best * 100.0 / 30.0);
    return { true, pct >= 70, bestStart, pct };
}

// One label/value row inside a side-panel card, optionally with a leading swatch.
struct KVRowDef { QString label; QString value; const QColor* swatch; };

// ============================================================================
// Step 6: chart primitives. Each chart draws into a caller-supplied QRectF (the
// full chart area inside a card). The helper reserves a left gutter for y-axis
// value labels and a bottom strip for x-date labels (plus a legend row where a
// legend is shown); the remaining inner rect is the data plot. All geometry
// scales by `s`. Pages 3/4/5/11/12 reuse these — the public signatures are kept
// stable. Per-night data extraction mirrors the classic SvgChart renderers in
// compliancereport.cpp byte-for-byte (renderUsageChart / renderAhiChart /
// renderLeakChart / renderChannelBarChart / renderPressureChart).
// ============================================================================

// Upper-percentile preference. Mirrors the file-local upperPctValue() in
// compliancereport.cpp (that symbol has internal linkage, so it is reproduced
// here) so per-night 95th-percentile math stays identical to the classic charts.
static double avUpperPct() {
    if (!p_profile) return 0.95;
    return qBound(0.50, p_profile->general->prefCalcPercentile() / 100.0, 0.999);
}

// Label matching the percentile actually used (e.g. "95th"). Mirrors Classic's
// upperPctLabel(); every on-page label must use this instead of a literal "95th"
// so a non-default calculation preference never mislabels a statistic.
static QString avUpperPctLabel() {
    return QStringLiteral("%1th").arg(qRound(avUpperPct() * 100.0));
}

// Oximetry artifact filtering (mirrors compliancereport.cpp's file-local
// constants; those have internal linkage). Used with calcDayFilteredStats so
// page-5 per-night nadirs apply the same probe-loss filtering as rd.minSpO2.
static const double AV_SPO2_ARTIFACT_FLOOR       = 40.0;  // % (probe-loss reads ~0)
static const double AV_SPO2_MAX_FALL_PCT_PER_SEC =  4.0;

// Reserved-gutter sizes (in 794px-artboard units, scaled by s at use).
static const double AV_AXIS_W = 27.0;   // y-label gutter (left): label + tick dash
static const double AV_XLAB_H = 30.0;   // x-date label strip (date-number row + "Sun" week-start row)
static const double AV_LEG_H  = 16.0;   // legend row (below the date strip)

// Inner plot (data) rect inside the full chart rect r.
static QRectF avPlotArea(const QRectF& r, double s, bool hasLegend) {
    const double botH = AV_XLAB_H*s + (hasLegend ? AV_LEG_H*s : 0.0);
    return QRectF(r.left() + AV_AXIS_W*s, r.top(),
                  r.width() - AV_AXIS_W*s, r.height() - botH);
}

// Per-day column width; bars/boxes are centred at (i+0.5)*colW like the mockup.
static double avColW(const QRectF& plot, int totalDays) {
    return plot.width() / qMax(1, totalDays);
}

// "Nice" rounded y-axis maximum — tiers copied from renderChannelBarChart.
static double avNiceMax(double maxVal) {
    const double raw = maxVal * 1.1;
    double y;
    if      (raw <= 5.0)    y = qCeil(raw);
    else if (raw <= 20.0)   y = qCeil(raw / 2.0)   * 2.0;
    else if (raw <= 100.0)  y = qCeil(raw / 5.0)   * 5.0;
    else if (raw <= 500.0)  y = qCeil(raw / 25.0)  * 25.0;
    else if (raw <= 2000.0) y = qCeil(raw / 100.0) * 100.0;
    else                    y = qCeil(raw / 500.0) * 500.0;
    return y < 1.0 ? 1.0 : y;
}

// Horizontal gridlines + right-aligned y value labels in the left gutter, with
// AirView-style axis detail: each labelled tick carries a small dash at the
// axis, and the midpoint between labels gets a shorter dash plus a faint
// dotted gridline (doubling the readable grid density). The bottom tick
// (== yMin) is a solid BASE baseline; labelled interior lines use GRID. One
// tick value may be recoloured via `highlight*` (the green "4 h" usage label).
static void avDrawYGrid(QPainter& p, const QRectF& plot, double yMin, double yMax,
                        const QVector<double>& ticks, int prec, double s,
                        bool hasHighlight = false, double highlightVal = 0.0,
                        const QColor& highlightCol = QColor()) {
    if (yMax <= yMin) yMax = yMin + 1.0;
    auto yOf = [&](double v){ return plot.bottom() - (v - yMin) / (yMax - yMin) * plot.height(); };
    const double majTick = 3.5*s, minTick = 2.2*s, labGap = 2.5*s;

    // Minor (half-step) detail between labelled ticks.
    for (int i = 0; i + 1 < ticks.size(); i++) {
        const double my = yOf((ticks[i] + ticks[i + 1]) / 2.0);
        QPen mp(AV::GRID_DOT, 1*s);
        mp.setDashPattern({2, 3});
        p.setPen(mp);
        p.drawLine(QPointF(plot.left(), my), QPointF(plot.right(), my));
        p.setPen(QPen(AV::GREY3, 1*s));
        p.drawLine(QPointF(plot.left() - minTick, my), QPointF(plot.left(), my));
    }

    for (double t : ticks) {
        const double yy = yOf(t);
        p.setPen(QPen(qAbs(t - yMin) < 1e-9 ? AV::BASE : AV::GRID, 1*s));
        p.drawLine(QPointF(plot.left(), yy), QPointF(plot.right(), yy));
        p.setPen(QPen(AV::GREY2, 1*s));                    // labelled tick dash
        p.drawLine(QPointF(plot.left() - majTick, yy), QPointF(plot.left(), yy));
        const bool hi = hasHighlight && qAbs(t - highlightVal) < 1e-9;
        p.setFont(avMono(8.5*s, QFont::Normal));
        p.setPen(hi ? highlightCol : AV::GREY3);
        p.drawText(QRectF(plot.left() - AV_AXIS_W*s, yy - 7*s, AV_AXIS_W*s - majTick - labGap, 14*s),
                   Qt::AlignRight | Qt::AlignVCenter, QString::number(t, 'f', prec));
    }
}

// x-tick cadence — copied from labelIntervalForDays() in compliancereport.cpp.
static int avLabelInterval(int totalDays) {
    if (totalDays > 90) return 7;
    if (totalDays > 60) return 5;
    if (totalDays > 30) return 3;
    if (totalDays > 10) return 2;
    return 1;
}

// x-axis: a small tick stub at EVERY day (AirView-style axis detail; labelled
// days get a slightly longer stub) + month/day date labels on the label
// cadence (month name on the first tick of a month, mirroring Classic).
static void avDrawXDates(QPainter& p, const QRectF& plot, const ReportData& rd, double s) {
    const int totalDays = qMax(1, rd.totalDays);
    const double colW = avColW(plot, totalDays);
    const int interval = avLabelInterval(totalDays);
    p.setPen(QPen(AV::BASE, 1*s));
    for (int i = 0; i < totalDays; i++) {
        const double tx = plot.left() + colW * (i + 0.5);
        const double stub = (i % interval == 0) ? 3.5*s : 2.0*s;
        p.drawLine(QPointF(tx, plot.bottom()), QPointF(tx, plot.bottom() + stub));
    }
    int prevMonth = -1;
    for (int i = 0; i < totalDays; i += interval) {
        const QDate d = rd.start.addDays(i);
        const bool newMonth = (d.month() != prevMonth);
        prevMonth = d.month();
        const double tx = plot.left() + colW * (i + 0.5);
        p.setFont(avMono(9*s, QFont::Normal));
        p.setPen(AV::GREY1);
        const QString lab = (newMonth || d.day() == 1) ? d.toString("MMM d") : QString::number(d.day());
        p.drawText(QRectF(tx - 30*s, plot.bottom() + 4*s, 60*s, 12*s),
                   Qt::AlignHCenter | Qt::AlignTop, lab);
    }
    // Second pass: a small "Sun" marker centred under EACH Sunday (week start),
    // on a row beneath the date-number row — mirrors the real AirView usage chart.
    p.setFont(avMono(8*s, QFont::Normal));
    p.setPen(AV::GREY3);
    for (int i = 0; i < totalDays; i++) {
        if (rd.start.addDays(i).dayOfWeek() != 7) continue;   // Qt: Sun == 7
        const double tx = plot.left() + colW * (i + 0.5);
        p.drawText(QRectF(tx - 30*s, plot.bottom() + 4*s + 12*s, 60*s, 11*s),
                   Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("Sun"));
    }
}

// Vertical gridlines: a faint dotted line at every labelled date tick plus a
// slightly stronger solid line per Sunday (week start), running the full plot
// height. Call right after avDrawYGrid and BEFORE the bars/data so the grid
// sits behind the data, like the real AirView usage chart.
static void avDrawXGrid(QPainter& p, const QRectF& plot, const ReportData& rd, double s) {
    const int totalDays = qMax(1, rd.totalDays);
    const double colW = avColW(plot, totalDays);
    const int interval = avLabelInterval(totalDays);
    QPen dp(AV::GRID_DOT, 1*s);
    dp.setDashPattern({2, 3});
    p.setPen(dp);
    for (int i = 0; i < totalDays; i += interval) {
        if (rd.start.addDays(i).dayOfWeek() == 7) continue;   // Sunday drawn below
        const double tx = plot.left() + colW * (i + 0.5);
        p.drawLine(QPointF(tx, plot.top()), QPointF(tx, plot.bottom()));
    }
    p.setPen(QPen(AV::GRID, 1*s));
    for (int i = 0; i < totalDays; i++) {
        if (rd.start.addDays(i).dayOfWeek() != 7) continue;   // Qt: Sun == 7
        const double tx = plot.left() + colW * (i + 0.5);
        p.drawLine(QPointF(tx, plot.top()), QPointF(tx, plot.bottom()));
    }
}

// Legend item: rounded-square swatch by default; `dashed` draws a short dashed
// line glyph instead (for "mean"/"redline" entries).
struct AvLegendItem { QColor color; QString label; bool dashed; };

// Flex-row legend: swatch/glyph + avSans(9) CAPTION label, left-to-right.
static void drawChartLegend(QPainter& p, const QRectF& r, const QVector<AvLegendItem>& items, double s) {
    const double sw = 9*s, gapSwatch = 5*s, gapItem = 13*s;
    const double cy = r.center().y();
    double x = r.left();
    for (const AvLegendItem& it : items) {
        if (it.dashed) {
            QPen lp(it.color, 1*s);
            lp.setDashPattern({4, 3});
            p.setPen(lp);
            p.drawLine(QPointF(x, cy), QPointF(x + 14*s, cy));
            x += 14*s + gapSwatch;
        } else {
            QPainterPath sp;
            sp.addRoundedRect(QRectF(x, cy - sw/2, sw, sw), 2*s, 2*s);
            p.fillPath(sp, it.color);
            x += sw + gapSwatch;
        }
        p.setFont(avSans(9*s, QFont::Normal));
        p.setPen(AV::CAPTION);
        QFontMetricsF fm(p.font());
        const double tw = fm.horizontalAdvance(it.label);
        p.drawText(QRectF(x, r.top(), tw + 2*s, r.height()), Qt::AlignVCenter | Qt::AlignLeft, it.label);
        x += tw + gapItem;
    }
}

// Convenience overload matching the brief's swatch-only signature.
static void drawChartLegend(QPainter& p, const QRectF& r, const QVector<QPair<QColor,QString>>& items, double s) {
    QVector<AvLegendItem> conv;
    conv.reserve(items.size());
    for (const auto& it : items) conv.push_back({ it.first, it.second, false });
    drawChartLegend(p, r, conv, s);
}

// 1) Daily usage bars. Per-night therapy hours; data mirrors renderUsageChart
//    (Day::hours(MT_CPAP); missing day = gap). Green ≥ threshold, amber below.
static void drawUsageBars(QPainter& p, const QRectF& r, const ReportData& rd, double s) {
    const int totalDays = qMax(1, rd.totalDays);
    const QRectF plot = avPlotArea(r, s, /*hasLegend*/false);

    double maxH = 12.0;
    for (int i = 0; i < totalDays; i++) {
        const QDate d = rd.start.addDays(i);
        if (rd.daysMap.contains(d)) maxH = qMax(maxH, (double)rd.daysMap[d]->hours(MT_CPAP));
    }
    const double yMin = 0.0, yMax = qMax(12.0, (double)qCeil(maxH));

    QVector<double> ticks;
    for (double t = 0.0; t <= yMax + 1e-6; t += 4.0) ticks.push_back(t);
    avDrawYGrid(p, plot, yMin, yMax, ticks, 0, s,
                /*hasHighlight*/true, rd.complianceThreshold, AV::GREEN);
    avDrawXGrid(p, plot, rd, s);
    avDrawXDates(p, plot, rd, s);

    auto yOf = [&](double v){ return plot.bottom() - (v - yMin) / (yMax - yMin) * plot.height(); };
    const double colW = avColW(plot, totalDays), barW = colW * 0.52, rad = 1.5*s;

    p.setPen(Qt::NoPen);
    for (int i = 0; i < totalDays; i++) {
        const QDate d = rd.start.addDays(i);
        if (!rd.daysMap.contains(d)) continue;             // missing day → gap (no bar)
        const double hrs = rd.daysMap[d]->hours(MT_CPAP);
        const double x = plot.left() + colW * (i + 0.5) - barW/2.0;
        if (hrs <= 0.0) {
            // Device record but 0 therapy hours: small amber stub so "worn, no
            // therapy" reads differently from "no data" (mirrors Classic).
            p.fillRect(QRectF(x, plot.bottom() - 3*s, barW, 3*s), AV::AMBER);
            continue;
        }
        const double yTop = yOf(hrs), h = plot.bottom() - yTop;
        if (h <= 0.0) continue;
        const QColor col = (hrs >= rd.complianceThreshold) ? AV::GREEN : AV::AMBER;
        QPainterPath bp;                                   // rounded top, square bottom
        bp.addRoundedRect(QRectF(x, yTop, barW, h), rad, rad);
        p.fillPath(bp, col);
        if (h > rad) p.fillRect(QRectF(x, yTop + rad, barW, h - rad), col);
    }

    // Dashed compliance-threshold line + white-bg label (threshold is a user
    // preference — bar coloring above uses the same rd.complianceThreshold).
    const double ty = yOf(rd.complianceThreshold);
    QPen thr(AV::GREEN, 1.5*s);
    thr.setDashPattern({4, 3});
    p.setPen(thr);
    p.drawLine(QPointF(plot.left(), ty), QPointF(plot.right(), ty));
    p.setFont(avSans(9*s, QFont::DemiBold));
    const QString tl = QObject::tr("%1 h compliance threshold")
                           .arg(complianceThresholdStr(rd.complianceThreshold));
    QFontMetricsF fm(p.font());
    const double tw = fm.horizontalAdvance(tl);
    const QRectF lblBox(plot.right() - tw - 6*s, ty - 14*s, tw + 6*s, 12*s);
    p.fillRect(lblBox, Qt::white);
    p.setPen(AV::GREEN);
    p.drawText(lblBox.adjusted(0, 0, -3*s, 0), Qt::AlignVCenter | Qt::AlignRight, tl);
}

// 2) Stacked nightly AHI. Per-night index = count/hours, mirroring
//    renderAhiChart's extraction and its TWO-band stack (apnea + hypopnea =
//    AHI, the AirView breakdown). The apnea sub-types (central / obstructive /
//    unknown) live in the side stats column — a 4-colour stack made single
//    nights unreadable at ~5px bar width. yMax tiers copied from renderAhiChart.
static void drawAhiStacked(QPainter& p, const QRectF& r, const ReportData& rd, double s) {
    const int totalDays = qMax(1, rd.totalDays);
    const QRectF plot = avPlotArea(r, s, /*hasLegend*/true);

    QVector<double> hyp(totalDays,0), apn(totalDays,0);
    double maxTot = 0.5;
    for (int i = 0; i < totalDays; i++) {
        const QDate d = rd.start.addDays(i);
        if (!rd.daysMap.contains(d)) continue;
        Day* day = rd.daysMap[d];
        const double hrs = day->hours(MT_CPAP);
        if (hrs <= 0.0) continue;
        hyp[i] = day->count(CPAP_Hypopnea) / hrs;
        apn[i] = (day->count(CPAP_ClearAirway) + day->count(CPAP_Obstructive)
                  + day->count(CPAP_Apnea)) / hrs;
        maxTot = qMax(maxTot, hyp[i] + apn[i]);
    }

    double yMax;
    if      (maxTot > 15.0) yMax = qCeil(maxTot / 5.0) * 5.0;
    else if (maxTot > 10.0) yMax = 15.0;
    else if (maxTot > 5.0)  yMax = 10.0;
    else if (maxTot > 3.0)  yMax = 5.0;
    else if (maxTot > 1.5)  yMax = 3.0;
    else if (maxTot > 0.5)  yMax = 2.0;
    else                    yMax = 1.0;

    QVector<double> ticks;
    for (int k = 0; k <= 5; k++) ticks.push_back(yMax * k / 5.0);
    avDrawYGrid(p, plot, 0.0, yMax, ticks, (yMax <= 5.0 ? 1 : 0), s);
    avDrawXGrid(p, plot, rd, s);
    avDrawXDates(p, plot, rd, s);

    auto yOf = [&](double v){ return plot.bottom() - v / yMax * plot.height(); };
    const double colW = avColW(plot, totalDays), barW = colW * 0.52;

    p.setPen(Qt::NoPen);
    for (int i = 0; i < totalDays; i++) {
        const double x = plot.left() + colW * (i + 0.5) - barW/2.0;
        double yb = plot.bottom();
        const QPair<double,QColor> segs[2] = {      // bottom→top: apnea, hypopnea
            { apn[i], AV::EV_OBS }, { hyp[i], AV::EV_HYP } };
        for (const auto& seg : segs) {
            const double h = seg.first / yMax * plot.height();
            if (h <= 0.3*s) continue;
            yb -= h;
            p.fillRect(QRectF(x, yb, barW, h), seg.second);
        }
    }

    if (rd.AHI > 0.0 && rd.AHI <= yMax) {           // dashed mean line
        QColor mc = AV::EV_OBS; mc.setAlphaF(0.55);
        QPen mp(mc, 0.9*s);
        mp.setDashPattern({4, 4});
        p.setPen(mp);
        const double my = yOf(rd.AHI);
        p.drawLine(QPointF(plot.left(), my), QPointF(plot.right(), my));
    }

    const QRectF legRect(plot.left(), r.bottom() - AV_LEG_H*s, plot.width(), AV_LEG_H*s);
    drawChartLegend(p, legRect, QVector<AvLegendItem>{
        { AV::EV_OBS, QObject::tr("Apnea (all types)"), false },
        { AV::EV_HYP, QObject::tr("Hypopnea"),          false },
        { AV::EV_OBS, QObject::tr("period AHI %1").arg(QString::number(rd.AHI, 'f', 2)), true },
    }, s);
}

// ----------------------------------------------------------------------------
// Robust data-adaptive y-axis scaling (AirView V2 box-whisker charts).
//
// computeYRange() derives a per-channel axis from the nightly distribution:
// data-driven floor (not always 0), outlier-resistant ceiling, enforced minimum
// span (so a stable fixed-CPAP trace doesn't fill the panel), sparse-data
// fallback, and clinical guardrails (pinned reference values, high-side-only
// outlier clipping for artifact-prone channels). SpO2 is handled separately in
// drawSpo2Chart so a low desaturation is never hidden.
// ----------------------------------------------------------------------------
struct AvYRange { double yMin, yMax, tickStep; bool clipped; double clipValue; };

struct AvChanParams {
    bool   floorAtZero;     // force floor to 0
    double padFrac;         // headroom added above the high anchor
    double minSpan;         // minimum yMax-yMin (prevents false drama)
    double minStep;         // smallest allowed tick step
    double floorMin;        // floor never below this (when not floored at 0)
    double ceilMax;         // ceiling never above this
    bool   forceCeilAtMax;  // pin ceiling to ceilMax (SpO2-style)
    double yMinAtMost;      // floor forced <= this (qQNaN = unused)
    double includeLo;       // value that must stay visible low  (qQNaN = none)
    double includeHi;       // value that must stay visible high (qQNaN = none)
    bool   allowHiClip;     // permit clipping a high outlier (pulse/leak only)
};

// "Nice" 1/2/2.5/5/10 * 10^k step for a target raw step size.
static double avNiceStep(double x) {
    if (x <= 0.0) return 1.0;
    const double mag = qPow(10.0, qFloor(qLn(x) / qLn(10.0)));
    const double n = x / mag;
    const double sden = (n <= 1.0) ? 1.0 : (n <= 2.0) ? 2.0
                      : (n <= 2.5) ? 2.5 : (n <= 5.0) ? 5.0 : 10.0;
    return sden * mag;
}

// Linear-interpolated percentile over an ascending-sorted vector (p in 0..100).
static double avPercentile(const QVector<double>& sortedAsc, double p) {
    const int len = sortedAsc.size();
    if (len == 0) return 0.0;
    if (len == 1) return sortedAsc.front();
    const double r = (p / 100.0) * (len - 1);
    const int lo = (int)qFloor(r);
    const int hi = qMin(lo + 1, len - 1);
    return sortedAsc[lo] + (r - lo) * (sortedAsc[hi] - sortedAsc[lo]);
}

// Core auto-scaler. mins/maxes/medians/p95s hold one entry per night with data.
// defMin/defMax are the channel's fallback range when there is no data.
static AvYRange avComputeYRange(QVector<double> mins, QVector<double> maxes,
                                const QVector<double>& medians, QVector<double> p95s,
                                const AvChanParams& cp, double defMin, double defMax) {
    Q_UNUSED(medians);
    const double OUTLIER_HEADROOM = 0.20, ZERO_SNAP_FRAC = 0.15, SPARSE_PAD = 0.15;
    const double TARGET_TICKS = 5.0;
    const int    MIN_NIGHTS_ROBUST = 4;

    const int N = mins.size();
    if (N == 0) {                                    // no data → channel default
        const double span = qMax(defMax - defMin, 1e-6);
        const double step = qMax(avNiceStep(span / TARGET_TICKS), cp.minStep);
        return { defMin, defMax, step, false, defMax };
    }

    std::sort(mins.begin(),  mins.end());
    std::sort(maxes.begin(), maxes.end());
    std::sort(p95s.begin(),  p95s.end());
    const double dataMin = mins.front();
    const double dataMax = maxes.back();

    double loAnchor, hiAnchor;
    bool sparse;
    if (N >= MIN_NIGHTS_ROBUST) {
        loAnchor = avPercentile(mins, 5.0);
        hiAnchor = qMax(avPercentile(maxes, 95.0), avPercentile(p95s, 90.0));
        sparse = false;
    } else {
        loAnchor = dataMin;
        hiAnchor = dataMax;
        sparse = true;
    }
    if (!qIsNaN(cp.includeLo)) loAnchor = qMin(loAnchor, cp.includeLo);
    if (!qIsNaN(cp.includeHi)) hiAnchor = qMax(hiAnchor, cp.includeHi);

    // ceiling (outlier-robust)
    double rawHi;
    bool clipped;
    if (sparse) {
        rawHi = hiAnchor * (1.0 + SPARSE_PAD);
        clipped = false;
    } else {
        const double cap = hiAnchor + OUTLIER_HEADROOM * (hiAnchor - loAnchor);
        rawHi = qMin(dataMax, cap);
        if (!qIsNaN(cp.includeHi)) rawHi = qMax(rawHi, cp.includeHi);
        clipped = (dataMax > rawHi + 1e-9);
    }
    // High-side clipping is allowed ONLY for artifact-prone channels (pulse/leak).
    // Every other channel extends to include the true maximum (no hidden finding).
    if (!cp.allowHiClip) { rawHi = qMax(rawHi, dataMax); clipped = false; }
    rawHi += cp.padFrac * (rawHi - loAnchor);

    // floor
    double rawLo;
    if (cp.floorAtZero) rawLo = 0.0;
    else if (!sparse && loAnchor <= ZERO_SNAP_FRAC * hiAnchor) rawLo = 0.0;
    else rawLo = qMax(cp.floorMin, loAnchor);
    if (!qIsNaN(cp.yMinAtMost)) rawLo = qMin(rawLo, cp.yMinAtMost);
    if (cp.forceCeilAtMax) rawHi = cp.ceilMax;

    // nice step + snap to step grid
    double span = qMax(rawHi - rawLo, 1e-6);
    double step = qMax(avNiceStep(span / TARGET_TICKS), cp.minStep);
    double yMin = qFloor(rawLo / step) * step;
    double yMax = qCeil(rawHi / step) * step;

    // enforce minimum span
    if ((yMax - yMin) < cp.minSpan) {
        const double center = (rawLo + rawHi) / 2.0;
        yMin = center - cp.minSpan / 2.0;
        yMax = center + cp.minSpan / 2.0;
        if (cp.floorAtZero || yMin < 0.0) {
            yMin = 0.0;
            yMax = qMax(yMax, cp.minSpan);
        }
        step = qMax(avNiceStep((yMax - yMin) / TARGET_TICKS), cp.minStep);
        yMin = qFloor(yMin / step) * step;
        yMax = qCeil(yMax / step) * step;
    }

    // final clamps
    if (cp.floorAtZero) yMin = 0.0;
    else yMin = qMax(yMin, cp.floorMin);
    if (!qIsNaN(cp.yMinAtMost)) yMin = qMin(yMin, cp.yMinAtMost);
    yMax = qMin(yMax, cp.ceilMax);
    if (cp.forceCeilAtMax) yMax = cp.ceilMax;
    if (yMax <= yMin) yMax = yMin + step;

    return { yMin, yMax, step, clipped, dataMax };
}

// 3) Per-night median–95th percentile band for a distribution channel. Data
//    extraction mirrors renderLeakChart / renderChannelBarChart: per night
//    median = percentile 0.5, band top = upper percentile (avUpperPct). Raw
//    nightly Min/Max whiskers are deliberately NOT drawn: Day::Min includes
//    sensor-dropout zeros (a single pulse probe-slip painted a whisker to 0,
//    contradicting the dropout-filtered header stats), and the extra encoding
//    doubled the legend without clinical benefit. The day map is passed
//    explicitly so oximetry pages can supply rd.oxiDaysMap; `colors` is the
//    design-system AV::ChannelColors (median–95th band / median tick).
static void drawChannelBand(QPainter& p, const QRectF& r, const ReportData& rd,
                            const QMap<QDate,Day*>& dayMap, ChannelID chan,
                            const AV::ChannelColors& colors, double redline, double s) {
    const int totalDays = qMax(1, rd.totalDays);
    const QRectF plot = avPlotArea(r, s, /*hasLegend*/true);

    QVector<double> med(totalDays,0), p95(totalDays,0);
    QVector<bool> has(totalDays,false);
    const double up = avUpperPct();
    for (int i = 0; i < totalDays; i++) {
        const QDate d = rd.start.addDays(i);
        if (chan == NoChannel || !dayMap.contains(d)) continue;
        Day* day = dayMap[d];
        const bool hasData = (chan == OXI_SPO2 || chan == OXI_Pulse)
                                 ? (day->timeAboveThreshold(chan, 0.0) > 0.0)
                                 : (day->hours(MT_CPAP) > 0.0);
        // channelHasData: a used night where THIS channel recorded nothing must
        // not paint a zero-height sliver or drag the auto-scaled floor to 0.
        if (!hasData || !day->channelHasData(chan)) continue;
        has[i] = true;
        med[i] = day->percentile(chan, 0.5);
        p95[i] = day->percentile(chan, up);
    }

    // Compact arrays for the auto-scaler: the median series anchors the floor,
    // the upper-percentile series the ceiling.
    QVector<double> aMed, aP95;
    for (int i = 0; i < totalDays; i++) {
        if (!has[i]) continue;
        aMed.push_back(med[i]); aP95.push_back(p95[i]);
    }

    // Map ChannelID -> auto-scale params (data-viz tuning x clinical guardrails).
    // ChannelIDs are runtime externs, so this is an if-chain, not a switch.
    const double NaNv = qQNaN();
    AvChanParams cp; double defMin = 0.0, defMax = 1.0;
    if (chan == CPAP_Pressure) {
        //     fl@0  pad   span step floorMin ceil  fCeil yMinAtMost inLo  inHi  clip
        cp = { false, 0.08, 4.0,  2.0,  0.0,  30.0, false, NaNv, NaNv, NaNv, false };
        defMin = 6.0; defMax = 14.0;
        if (rd.lastDay) {                            // pin the prescribed pressure(s)
            double sm = rd.lastDay->settings_min(CPAP_PressureMin);
            double sx = rd.lastDay->settings_max(CPAP_PressureMax);
            if (sm <= 0.0 && sx <= 0.0) {            // fixed CPAP: one set pressure
                const double f = rd.lastDay->settings_max(CPAP_Pressure);
                sm = f; sx = f;
            }
            if (sm > 0.0) cp.includeLo = sm;
            if (sx > 0.0) cp.includeHi = sx;
        }
    } else if (chan == CPAP_Leak || chan == CPAP_LeakTotal) {
        cp = { true, 0.10, 20.0, 5.0,  0.0, 120.0, false, NaNv, NaNv,
               (redline > 0.0 ? redline : NaNv), true };
        defMin = 0.0; defMax = 60.0;
    } else if (chan == OXI_Pulse) {
        cp = { false, 0.10, 30.0, 10.0, 30.0, 220.0, false, 50.0, NaNv, 100.0, true };
        defMin = 40.0; defMax = 120.0;
    } else if (chan == CPAP_MinuteVent) {
        cp = { true, 0.10, 4.0,  1.0,  0.0,  40.0, false, NaNv, NaNv, NaNv, false };
        defMin = 0.0; defMax = 12.0;
    } else if (chan == CPAP_RespRate) {
        cp = { false, 0.10, 8.0,  2.0,  0.0,  60.0, false, 8.0,  NaNv, NaNv, false };
        defMin = 0.0; defMax = 24.0;
    } else if (chan == CPAP_TidalVolume) {
        cp = { true, 0.10, 200.0, 50.0, 0.0, 3000.0, false, NaNv, NaNv, NaNv, false };
        defMin = 0.0; defMax = 600.0;
    } else {
        cp = { false, 0.10, 1.0, 1.0, 0.0, 1e9, false, NaNv, NaNv, NaNv, false };
        defMin = 0.0;
        defMax = avNiceMax(aP95.isEmpty() ? 1.0
                           : *std::max_element(aP95.constBegin(), aP95.constEnd()));
    }

    const AvYRange yr = avComputeYRange(aMed, aP95, aMed, aP95, cp, defMin, defMax);
    const double yMin = yr.yMin, yMax = yr.yMax, step = yr.tickStep;

    QVector<double> ticks;
    for (double t = yMin; t <= yMax + 1e-6; t += step) ticks.push_back(t);
    avDrawYGrid(p, plot, yMin, yMax, ticks, ((yMax - yMin) <= 5.0 ? 1 : 0), s);
    avDrawXGrid(p, plot, rd, s);
    avDrawXDates(p, plot, rd, s);

    auto yOf = [&](double v){
        const double y = plot.bottom() - (v - yMin) / (yMax - yMin) * plot.height();
        return qBound(plot.top(), y, plot.bottom());
    };

    const double colW = avColW(plot, totalDays), barW = colW * 0.52;
    p.setPen(Qt::NoPen);
    for (int i = 0; i < totalDays; i++) {
        if (!has[i]) continue;
        const double x = plot.left() + colW * (i + 0.5) - barW/2.0;
        const double yMdp = yOf(med[i]), yP = yOf(p95[i]);
        p.fillRect(QRectF(x, yP, barW, qMax(0.8*s, yMdp - yP)), colors.band);   // median–95th
        p.fillRect(QRectF(x, yMdp - 0.9*s, barW, 1.8*s),        colors.median); // median tick
    }

    // High-side outlier clip indicator (pulse/leak only). A night whose upper
    // percentile exceeds the robust ceiling renders flush to the top (yOf
    // already qBounds it); flag it with a small triangle on each clipped column
    // + a "peak <value>" annotation. Low values and findings are NEVER clipped
    // (allowHiClip is false elsewhere).
    if (yr.clipped) {
        const double tw = 6*s;
        p.setPen(Qt::NoPen);
        p.setBrush(colors.median);
        for (int i = 0; i < totalDays; i++) {
            if (!has[i] || p95[i] <= yMax + 1e-9) continue;
            const double cxp = plot.left() + colW * (i + 0.5);
            QPolygonF tri;
            tri << QPointF(cxp, plot.top())
                << QPointF(cxp - tw/2.0, plot.top() + tw)
                << QPointF(cxp + tw/2.0, plot.top() + tw);
            p.drawPolygon(tri);
        }
        p.setBrush(Qt::NoBrush);
        p.setFont(avSans(8.5*s, QFont::DemiBold));
        p.setPen(colors.median);
        p.drawText(QRectF(plot.left(), plot.top() + 1*s, plot.width() - 2*s, 12*s),
                   Qt::AlignRight | Qt::AlignTop,
                   QObject::tr("peak %1").arg(qRound(yr.clipValue)));
    }

    if (redline > 0.0) {                             // dashed redline + right label
        QPen rp(AV::RED, 1*s);
        rp.setDashPattern({5, 4});
        p.setPen(rp);
        const double ry = yOf(redline);
        p.drawLine(QPointF(plot.left(), ry), QPointF(plot.right(), ry));
        p.setFont(avSans(8.5*s, QFont::Normal));
        p.setPen(AV::RED);
        p.drawText(QRectF(plot.left(), ry - 12*s, plot.width() - 2*s, 12*s),
                   Qt::AlignRight | Qt::AlignVCenter, QObject::tr("redline %1").arg(qRound(redline)));
    }

    QVector<AvLegendItem> leg = {
        { colors.band,   QObject::tr("nightly median–%1").arg(avUpperPctLabel()), false },
        { colors.median, QObject::tr("median"), false },
    };
    if (redline > 0.0) leg.push_back({ AV::RED, QObject::tr("redline %1").arg(qRound(redline)), true });
    const QRectF legRect(plot.left(), r.bottom() - AV_LEG_H*s, plot.width(), AV_LEG_H*s);
    drawChartLegend(p, legRect, leg, s);
}

// ============================================================================
// Step 6b: oximetry / sleep primitives (pages 5–6). Same conventions as the
// chart primitives above; all geometry scales by `s`.
// ============================================================================

// 5) Single horizontal stacked distribution bar (~rounded ends). Each segment's
//    width is proportional to its fraction of the segment-sum (fractions need not
//    sum to 1). A small centred % label is drawn inside a segment when it fits.
static void drawDistributionBar(QPainter& p, const QRectF& r,
                                const QVector<QPair<double,QColor>>& segments, double s) {
    double total = 0.0;
    for (const auto& seg : segments) total += qMax(0.0, seg.first);
    if (total <= 0.0) return;

    const double rad = 3*s;
    QPainterPath clip;
    clip.addRoundedRect(r, rad, rad);
    p.save();
    p.setClipPath(clip);
    p.setPen(Qt::NoPen);

    double x = r.left();
    for (const auto& seg : segments) {
        const double frac = qMax(0.0, seg.first) / total;
        const double w = frac * r.width();
        if (w <= 0.0) continue;
        p.fillRect(QRectF(x, r.top(), w, r.height()), seg.second);
        const QString lbl = QString::number(qRound(frac * 100.0)) + QStringLiteral("%");
        p.setFont(avSans(8.5*s, QFont::DemiBold));
        QFontMetricsF fm(p.font());
        if (fm.horizontalAdvance(lbl) + 8*s < w) {
            p.setPen(seg.second.lightnessF() > 0.6 ? AV::INK : Qt::white);
            p.drawText(QRectF(x, r.top(), w, r.height()), Qt::AlignCenter, lbl);
            p.setPen(Qt::NoPen);
        }
        x += w;
    }
    p.restore();
}

// 6) Per-night SpO2 range chart over rd.oxiDaysMap. Deliberately different from
//    drawChannelBand: for SpO2 the clinical finding is the DEPTH of nightly
//    desaturation, and a median–95th band would collapse into a sliver at ~97%
//    and hide every dip — so each night plots its dropout-filtered min–max
//    range with a median tick, plus a bold period-low marker. No threshold
//    lines or zone tints: interpretation is left to the clinician. y-axis
//    floats to floor(min-2)..100, capped at 90 so ≥10 points of context remain
//    and a stable trace doesn't read as dramatic swings.
static void drawSpo2Chart(QPainter& p, const QRectF& r, const ReportData& rd, double s) {
    const int totalDays = qMax(1, rd.totalDays);
    const QRectF plot = avPlotArea(r, s, /*hasLegend*/true);

    QVector<double> med(totalDays,0), mn(totalDays,0), mx(totalDays,0);
    QVector<bool> has(totalDays,false);
    double dataMin = 100.0, lowVal = 200.0;
    int lowIdx = -1;
    for (int i = 0; i < totalDays; i++) {
        const QDate d = rd.start.addDays(i);
        if (!rd.oxiDaysMap.contains(d)) continue;
        Day* day = rd.oxiDaysMap[d];
        if (day->timeAboveThreshold(OXI_SPO2, 0.0) <= 0.0) continue;
        // Dropout-filtered nightly range (same probe-loss filter as rd.minSpO2),
        // so a probe-slip artifact never fakes a low or drags the axis.
        const FilteredStats fs = calcDayFilteredStats(day, OXI_SPO2, rd.oxiSourceType,
                                                      AV_SPO2_ARTIFACT_FLOOR, 0.0,
                                                      AV_SPO2_MAX_FALL_PCT_PER_SEC);
        if (!fs.found || fs.min <= 0.0) continue;
        has[i] = true;
        med[i] = day->percentile(OXI_SPO2, 0.5);
        mn[i]  = fs.min;
        mx[i]  = fs.max;
        dataMin = qMin(dataMin, fs.min);
        if (fs.min < lowVal) { lowVal = fs.min; lowIdx = i; }
    }

    // Ceiling is fixed at 100. The LOW floor follows the TRUE worst dip (snapped
    // to a step of 2) so a low desaturation is NEVER hidden as an "outlier";
    // capped at 90 so the axis always keeps ≥10 points of context. There is
    // deliberately NO lower clamp — if dataMin is 70, the floor follows it down.
    const double yMax = 100.0;
    const double yMin = qMin(90.0, (double)(qFloor((dataMin - 2.0) / 2.0) * 2.0));

    auto yOf = [&](double v){
        const double y = plot.bottom() - (v - yMin) / (yMax - yMin) * plot.height();
        return qBound(plot.top(), y, plot.bottom());
    };

    // AirView-style axis: baseline + labels/tick dashes every 5%, minor dash +
    // faint dotted gridline at intervening whole % (every 2% on deep ranges).
    {
        const double majTick = 3.5*s, minTick = 2.2*s, labGap = 2.5*s;
        p.setPen(QPen(AV::BASE, 1*s));
        p.drawLine(QPointF(plot.left(), plot.bottom()), QPointF(plot.right(), plot.bottom()));
        p.setFont(avMono(8.5*s, QFont::Normal));
        p.setPen(AV::GREY3);
        p.drawText(QRectF(plot.left() - AV_AXIS_W*s, plot.bottom() - 7*s,
                          AV_AXIS_W*s - majTick - labGap, 14*s),
                   Qt::AlignRight | Qt::AlignVCenter, QString::number((int)yMin));
        const int minorStep = ((yMax - yMin) > 16.0) ? 2 : 1;
        for (int v = (int)yMin + 1; v < (int)yMax; v++) {
            if (v % 5 == 0 || (v - (int)yMin) % minorStep != 0) continue;
            const double yy = yOf(v);
            QPen mp(AV::GRID_DOT, 1*s);
            mp.setDashPattern({2, 3});
            p.setPen(mp);
            p.drawLine(QPointF(plot.left(), yy), QPointF(plot.right(), yy));
            p.setPen(QPen(AV::GREY3, 1*s));
            p.drawLine(QPointF(plot.left() - minTick, yy), QPointF(plot.left(), yy));
        }
        for (int v = ((int)yMin/5 + 1)*5; v <= (int)yMax; v += 5) {
            const double yy = yOf(v);
            p.setPen(QPen(AV::GRID, 1*s));
            p.drawLine(QPointF(plot.left(), yy), QPointF(plot.right(), yy));
            p.setPen(QPen(AV::GREY2, 1*s));
            p.drawLine(QPointF(plot.left() - majTick, yy), QPointF(plot.left(), yy));
            p.setFont(avMono(8.5*s, QFont::Normal));
            p.setPen(AV::GREY3);
            p.drawText(QRectF(plot.left() - AV_AXIS_W*s, yy - 7*s,
                              AV_AXIS_W*s - majTick - labGap, 14*s),
                       Qt::AlignRight | Qt::AlignVCenter, QString::number(v));
        }
    }
    avDrawXGrid(p, plot, rd, s);
    avDrawXDates(p, plot, rd, s);

    // per-night min–max range bars (single neutral colour — no zone tints) +
    // median tick
    const double colW = avColW(plot, totalDays), barW = colW * 0.52;
    p.setPen(Qt::NoPen);
    for (int i = 0; i < totalDays; i++) {
        if (!has[i]) continue;
        const double x = plot.left() + colW * (i + 0.5) - barW/2.0;
        const double yt = yOf(mx[i]), yb = yOf(mn[i]);
        p.fillRect(QRectF(x, yt, barW, qMax(0.8*s, yb - yt)), AV::SPO2_NORMAL);
        const double ym = yOf(med[i]);
        p.fillRect(QRectF(x, ym - 0.9*s, barW, 1.8*s), AV::GREY1);
    }

    // bold period-low marker + label (only when a period low is recorded)
    if (rd.minSpO2Set && lowIdx >= 0) {
        const double x = plot.left() + colW * (lowIdx + 0.5);
        const double yv = yOf(lowVal);
        p.setBrush(AV::RED); p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(x, yv), 2.2*s, 2.2*s);
        p.setBrush(Qt::NoBrush);
        p.setFont(avSans(8.5*s, QFont::DemiBold));
        p.setPen(AV::RED);
        // Label with the same filtered nightly value that positions the dot.
        const QString lbl = QString::number(qRound(lowVal)) + QStringLiteral("%");
        QFontMetricsF fm(p.font());
        const double lw = fm.horizontalAdvance(lbl);
        const double lx = qBound(plot.left(), x - lw/2.0, plot.right() - lw);
        p.drawText(QRectF(lx, yv + 3*s, lw, 11*s), Qt::AlignHCenter | Qt::AlignTop, lbl);
    }

    QVector<AvLegendItem> leg = {
        { AV::SPO2_NORMAL, QObject::tr("nightly range (min–max)"), false },
        { AV::GREY1,       QObject::tr("median"),                  false },
    };
    if (rd.minSpO2Set && lowIdx >= 0)
        leg.push_back({ AV::RED, QObject::tr("period low"), false });
    const QRectF legRect(plot.left(), r.bottom() - AV_LEG_H*s, plot.width(), AV_LEG_H*s);
    drawChartLegend(p, legRect, leg, s);
}

// 7) Four sleep-stage summary cells across r (Deep / Light / REM / Awake): swatch
//    + uppercase label, mono % value, and a sub-time line (minutes via hmm()).
static void drawSleepStageCards(QPainter& p, const QRectF& r, const SleepStageStats& sl, double s) {
    struct Stage { QColor col; QString label; double pct; double mins; };
    const Stage stages[4] = {
        { AV::SLEEP_DEEP,  QObject::tr("Deep"),  sl.deepPct,  sl.deepMin  },
        { AV::SLEEP_LIGHT, QObject::tr("Light"), sl.lightPct, sl.lightMin },
        { AV::SLEEP_REM,   QObject::tr("REM"),   sl.remPct,   sl.remMin   },
        { AV::SLEEP_AWAKE, QObject::tr("Awake"), sl.awakePct, sl.awakeMin },
    };
    const double cw = r.width() / 4.0;
    for (int i = 0; i < 4; i++) {
        const QRectF c(r.left() + i*cw, r.top(), cw, r.height());
        double cy = c.top();
        const double sw = 9*s;
        QPainterPath sp;
        sp.addRoundedRect(QRectF(c.left(), cy + 2*s, sw, sw), 2*s, 2*s);
        p.fillPath(sp, stages[i].col);
        p.setFont(avSans(9*s, QFont::DemiBold, 0.08*9*s));
        p.setPen(AV::GREY2);
        p.drawText(QRectF(c.left() + sw + 6*s, cy, c.width() - sw - 6*s, 13*s),
                   Qt::AlignVCenter | Qt::AlignLeft, stages[i].label.toUpper());
        cy += 13*s + 4*s;
        const double valBoxH = 20*s;
        const QString val = QString::number(qRound(stages[i].pct));
        p.setFont(avMono(17*s, QFont::DemiBold));
        p.setPen(AV::INK);
        QFontMetricsF vm(p.font());
        const double vw = vm.horizontalAdvance(val);
        p.drawText(QRectF(c.left(), cy, c.width(), valBoxH), Qt::AlignLeft | Qt::AlignBottom, val);
        p.setFont(avSans(10*s, QFont::Normal));
        p.setPen(AV::GREY3);
        p.drawText(QRectF(c.left() + vw + 2*s, cy, c.width(), valBoxH),
                   Qt::AlignLeft | Qt::AlignBottom, QStringLiteral("%"));
        cy += valBoxH + 1*s;
        p.setFont(avSans(9.5*s, QFont::Normal));
        p.setPen(AV::CAPTION);
        p.drawText(QRectF(c.left(), cy, c.width(), 12*s), Qt::AlignLeft | Qt::AlignTop,
                   hmm(stages[i].mins / 60.0));
    }
}

// 8) Small ACCENT uppercase .1em subheader row (e.g. "Usage & saturation").
static void drawOximeterSubheader(QPainter& p, const QRectF& r, const QString& text, double s) {
    p.setFont(avSans(8.5*s, QFont::DemiBold, 0.1*8.5*s));
    p.setPen(AV::ACCENT);
    p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft, text.toUpper());
}

// ---- Clinical banding (sleep-medicine thresholds) ---------------------------
// band 0 = normal, 1 = borderline, 2 = abnormal. capAmber clamps 2→1 for
// device-estimated/uncalibrated indices (RERA, flow limitation) that must never
// read as a hard red. Value text uses legible darks; dots use the bright tokens.
struct AvBand { int band; QColor value; QColor dot; };
static AvBand avBand(double v, double lo, double hi, bool capAmber = false) {
    int b = (v >= hi) ? 2 : (v >= lo ? 1 : 0);
    if (capAmber && b == 2) b = 1;
    const QColor val = (b == 2) ? AV::RED : (b == 1) ? AV::AMBER_DK : AV::INK;
    const QColor dot = (b == 2) ? AV::RED : (b == 1) ? AV::AMBER    : AV::GREEN;
    return { b, val, dot };
}

// ============================================================================
// Page 1 — Compliance Summary. Canonical top-to-bottom page builder; pages 2–7
// follow this pattern. Geometry mirrors the mockup's 794px artboard.
// ============================================================================
// One row of an AirView-style per-chart stats column (label left, value right).
struct AvStat {
    QString label;
    QString value;
    QColor  valueColor;             // invalid → AV::INK
    const QColor* swatch = nullptr; // optional legend swatch before the label
    bool    openSwatch = false;     // outline swatch (AHI total) vs filled
};

// Draws the left-hand stats column beside a trend chart (mirrors ResMed AirView):
// label left in grey, value right-aligned bold; optional legend swatch; empty
// row = vertical spacer.
static void drawChartStats(QPainter& p, const QRectF& r, const QVector<AvStat>& lines, double s) {
    const double lineH = 19*s;
    double yy = r.top();
    for (const AvStat& ln : lines) {
        if (ln.label.isEmpty() && ln.value.isEmpty()) { yy += lineH * 0.55; continue; }
        double lx = r.left();
        if (ln.swatch) {
            const double sw = 9*s;
            const QRectF sq(r.left(), yy + (lineH - sw) / 2.0, sw, sw);
            if (ln.openSwatch) { p.setBrush(Qt::NoBrush); p.setPen(QPen(*ln.swatch, 1.3*s)); p.drawRect(sq); }
            else               { p.setPen(Qt::NoPen); p.setBrush(*ln.swatch); p.drawRect(sq); }
            lx += sw + 7*s;
        }
        p.setFont(avSans(9.5*s, QFont::Normal));
        p.setPen(AV::GREY1);
        p.drawText(QRectF(lx, yy, qMax(10.0*s, r.right() - lx - 46*s), lineH),
                   Qt::AlignVCenter | Qt::AlignLeft, ln.label);
        p.setFont(avMono(10*s, QFont::DemiBold));
        p.setPen(ln.valueColor.isValid() ? ln.valueColor : AV::INK);
        p.drawText(QRectF(r.left(), yy, r.width(), lineH),
                   Qt::AlignVCenter | Qt::AlignRight, ln.value);
        yy += lineH;
    }
}

static void avPage1(QPainter& p, const QRectF& page, const ReportData& rd,
                    int pageNo, int pageCount, double s) {
    const QRectF content = avContentRect(page, s);
    const double cx = content.left(), cw = content.width();
    double y = page.top();

    // (a) Masthead (large). Helper adds its own 34px padding + navy underline.
    const double mastH = 64*s;
    drawMasthead(p, QRectF(page.left(), y, page.width(), mastH), true, rd,
                 QObject::tr("Compliance Report"), s);
    y += mastH + 16*s;

    // (b) Patient / device info grid — 10 cells, 5 cols → 2 rows.
    const QString period = (rd.start.isValid() && rd.end.isValid())
        ? rd.start.toString("MMM d") + QStringLiteral(" – ") + rd.end.toString("MMM d, yyyy")
        : QStringLiteral("—");
    const QString device = (rd.brand + QStringLiteral(" ") + rd.model).trimmed();
    // Compact pressure for the narrow summary cell: just "Min 7.0 Max 11.0"
    // (strip the unit — the label says PRESSURE; EPR is shown on pages 2 & 3).
    QString pressure = rd.pressDisplay;
    pressure.remove(QStringLiteral("(cmH2O)")).remove(QStringLiteral("cmH2O"));
    pressure = pressure.trimmed();
    const QVector<QPair<QString,QString>> info = {
        { QObject::tr("Patient"),       rd.patientName.isEmpty() ? QStringLiteral("—") : rd.patientName },
        { QObject::tr("Date of birth"), rd.dob.isValid() ? rd.dob.toString("MMM d, yyyy") : QStringLiteral("—") },
        { QObject::tr("Age"),           rd.age >= 0 ? QObject::tr("%1 yrs").arg(rd.age) : QStringLiteral("—") },
        { QObject::tr("Report period"), period },
        { QObject::tr("Nights used"),   QString::number(rd.daysUsed) },
        { QObject::tr("Device"),        device.isEmpty() ? QStringLiteral("—") : device },
        { QObject::tr("Serial no."),    rd.serial.isEmpty() ? QStringLiteral("—") : rd.serial },
        { QObject::tr("Mode"),          rd.modeStr.isEmpty() ? QStringLiteral("—") : rd.modeStr },
        { QObject::tr("Pressure"),      pressure.trimmed().isEmpty() ? QStringLiteral("—") : pressure },
        { QObject::tr("Mask"),          rd.maskDescription.isEmpty() ? QStringLiteral("—") : rd.maskDescription },
    };
    const double gridH = 2 * 50*s;
    // Proportional columns (cols: Patient/Device · DOB/Serial · Age/Mode · Period/Pressure · Nights/Mask)
    // so the long values (device name, date range, pressure) get the width they need.
    drawInfoGrid(p, QRectF(cx, y, cw, gridH), info, 5, s, { 1.7, 1.1, 0.6, 1.4, 1.1 },
                 /*roundTop*/true, /*roundBottom*/false);
    y += gridH;

    // (c) Scorecard strip — 6 cells, butted under the info grid. Every dot,
    // tint and top rule is VALUE-DRIVEN (a fixed green dot on a clinical
    // scorecard is a truthfulness bug). Neutral grey = metric not bandable
    // (no data, or total-leak device with no seal redline).
    const double scH = 82*s, scW = cw / 6.0;
    const bool leakBandable = !rd.isTotalLeak && rd.leakChan != NoChannel && rd.leakRedline > 0;
    const QString thrStr = complianceThresholdStr(rd.complianceThreshold);
    // Banding where HIGHER is better (adherence, usage, SpO2): v ≥ okAt → 0,
    // v ≥ warnAt → 1, else 2. (avBand covers the higher-is-worse indices.)
    auto bandLow = [](double v, double okAt, double warnAt) -> int {
        return v >= okAt ? 0 : v >= warnAt ? 1 : 2;
    };
    auto dotFor = [](int b) -> QColor {
        return b >= 2 ? AV::RED : b == 1 ? AV::AMBER : b == 0 ? AV::GREEN : AV::GREY3;
    };
    const int adhBand  = bandLow(rd.pctCompliant, 70.0, 50.0);          // CMS-style 70%
    const int ahiBand  = avBand(rd.AHI, 5.0, 15.0).band;
    const int useBand  = bandLow(rd.avgHoursUsed, rd.complianceThreshold,
                                 qMax(0.0, rd.complianceThreshold - 1.0));
    const int leakBand = leakBandable
        ? avBand(rd.p95Leak, rd.leakRedline - 4.0, rd.leakRedline).band : -1;
    const int o2Band   = rd.minSpO2Set ? bandLow(rd.minSpO2, 90.0, 88.0) : -1;
    const int caiBand  = avBand(rd.CentralAI, 5.0, 10.0).band;

    struct Card { int band; QString label, value, unit, caption; };
    const QVector<Card> cards = {
        { adhBand,  QObject::tr("Adherence"),    QString::number(qRound(rd.pctCompliant)), QStringLiteral("%"),
          QObject::tr("%1/%2 ≥ %3 h").arg(rd.compliantDays).arg(rd.totalDays).arg(thrStr) },
        { ahiBand,  QObject::tr("Residual AHI"), QString::number(rd.AHI, 'f', 1), QStringLiteral("/h"),
          rd.hasBaseline ? QObject::tr("%1%2% vs baseline")
                               .arg(rd.baselinePctChange >= 0 ? QStringLiteral("+") : QString())
                               .arg(qRound(rd.baselinePctChange))
                         : QString() },
        { useBand,  QObject::tr("Avg usage"),    hmm(rd.avgHoursUsed), QStringLiteral("h"),
          QObject::tr("median %1").arg(hmm(rd.medianHoursUsed)) },
        { leakBand, QObject::tr("Leak %1").arg(avUpperPctLabel()),
          rd.leakChan != NoChannel ? QString::number(rd.p95Leak, 'f', 1) : QStringLiteral("—"),
          rd.leakChan != NoChannel ? QStringLiteral("L/m") : QString(),
          rd.leakChan == NoChannel ? QObject::tr("no leak data")
              : leakBandable       ? QObject::tr("redline %1").arg(qRound(rd.leakRedline))
                                   : QObject::tr("total leak") },
        { o2Band,   QObject::tr("Lowest SpO₂"),
          rd.minSpO2Set ? QString::number(qRound(rd.minSpO2)) : QStringLiteral("—"),
          rd.minSpO2Set ? QStringLiteral("%") : QString(),
          rd.minSpO2Set ? QObject::tr("T90 %1").arg(minsFromSecs(rd.t90Secs)) : QString() },
        { caiBand,  QObject::tr("Clear-airway idx"), QString::number(rd.CentralAI, 'f', 2), QStringLiteral("/h"),
          // CSR caption only when the device records that channel
          p_profile->hasChannel(CPAP_CSR)
              ? QObject::tr("CSR %1%").arg(qRound(rd.avgCsrPctPerNight)) : QString() },
    };
    // Cells clipped to a bottom-rounded strip; the grid above + this strip
    // read as one soft-cornered hero block.
    const double scRad = AV_RAD*s, scA = 0.5*s;
    QPainterPath scClip;
    scClip.addRoundedRect(QRectF(cx, y, cw, scH), scRad, scRad);
    scClip.addRect(QRectF(cx, y, cw, scRad));                 // square the top corners
    scClip = scClip.simplified();
    p.save();
    p.setClipPath(scClip);
    for (int i = 0; i < cards.size(); i++) {
        drawScorecardCell(p, QRectF(cx + i*scW, y, scW, scH), dotFor(cards[i].band),
                          cards[i].label, cards[i].value, cards[i].unit, cards[i].caption,
                          cards[i].band, s);
    }
    p.restore();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(AV::HAIR1, 1*s));
    QPainterPath scBorder;
    scBorder.addRoundedRect(QRectF(cx, y, cw, scH).adjusted(scA, scA, -scA, -scA), scRad, scRad);
    scBorder.addRect(QRectF(cx + scA, y + scA, cw - 2*scA, scRad));
    p.drawPath(scBorder.simplified());
    p.setPen(QPen(AV::HAIR2, 1*s));
    for (int i = 1; i < 6; i++)
        p.drawLine(QPointF(cx + i*scW, y), QPointF(cx + i*scW, y + scH));
    y += scH + 22*s;

    // (d) Usage section — band, chart card (reserved for Task 9), compliance cells.
    const double bandH = 30*s;
    drawBand(p, QRectF(cx, y, cw, bandH), QObject::tr("Usage"),
             QObject::tr("Daily therapy hours · compliance threshold %1 h").arg(thrStr), s);
    y += bandH;

    // Chart height sized so the section fills the room freed by the slimmer
    // panels below — a taller plot keeps 90-night bars readable.
    const double chartH = 300*s, cardPadX = 16*s, cardPadTop = 14*s, cardPadBottom = 10*s;
    const double usageCardH = cardPadTop + chartH + cardPadBottom;
    drawCard(p, QRectF(cx, y, cw, usageCardH), /*joinTop*/true, s);

    // Left stats column (mirrors ResMed AirView's usage summary), bars to the
    // right. One term everywhere: "nights" (the info grid + page 2 use it too).
    const double uStatsW = 170*s, uStatsGap = 14*s;
    const int notUsed4h = rd.daysUsed - rd.compliantDays;
    auto pctOf = [&](int n){ return rd.totalDays > 0 ? qRound(100.0 * n / rd.totalDays) : 0; };
    const QVector<AvStat> usageStats = {
        { QObject::tr("Nights used"),
          QStringLiteral("%1/%2 (%3%)").arg(rd.daysUsed).arg(rd.totalDays).arg(qRound(rd.pctUsed)), QColor(), nullptr, false },
        { QObject::tr("\xe2\x89\xa5 %1 h nights").arg(thrStr),
          QStringLiteral("%1 (%2%)").arg(rd.compliantDays).arg(qRound(rd.pctCompliant)), AV::GREEN, nullptr, false },
        { QObject::tr("< %1 h nights").arg(thrStr),
          QStringLiteral("%1 (%2%)").arg(notUsed4h).arg(pctOf(notUsed4h)), notUsed4h > 0 ? AV::RED : QColor(), nullptr, false },
        { QObject::tr("Nights not used"),
          QStringLiteral("%1 (%2%)").arg(rd.daysNotUsed).arg(pctOf(rd.daysNotUsed)), QColor(), nullptr, false },
        { QObject::tr("Nights no data"),
          QStringLiteral("%1 (%2%)").arg(rd.daysNoData).arg(pctOf(rd.daysNoData)), QColor(), nullptr, false },
        { QString(), QString(), QColor(), nullptr, false },  // spacer
        { QObject::tr("Used/night (avg)"), hmm(rd.avgHoursUsed) + QObject::tr(" h"), QColor(), nullptr, false },
    };
    drawChartStats(p, QRectF(cx + cardPadX, y + cardPadTop, uStatsW, chartH), usageStats, s);

    const QRectF usageChartRect(cx + cardPadX + uStatsW + uStatsGap, y + cardPadTop,
                                cw - 2*cardPadX - uStatsW - uStatsGap, chartH);
    drawUsageBars(p, usageChartRect, rd, s);
    y += usageCardH + 22*s;

    // (e) Two side-by-side panels: Mask & Seal (left) + Respiratory Events
    // (right). Plain KV-row cards: the gauges that used to sit here duplicated
    // the scorecard verdicts above, and their centred scale labels drifted from
    // the actual tick positions — dropped for a simpler, honest read.
    const double gap = 18*s, panelW = (cw - gap) / 2.0;
    const double leftX = cx, rightX = cx + panelW + gap;
    const double panelBandH = 30*s, panelCardH = 200*s, panelTop = y;

    auto drawPanel = [&](double px0, const QString& bandTitle, const QString& bandSub,
                         const QString& miniLabel, const QString& miniNote, const QColor& miniNoteColor,
                         const QVector<KVRowDef>& rows) {
        drawBand(p, QRectF(px0, panelTop, panelW, panelBandH), bandTitle, bandSub, s);
        const double cardTop = panelTop + panelBandH;
        drawCard(p, QRectF(px0, cardTop, panelW, panelCardH), /*joinTop*/true, s);
        const double ix = px0 + 16*s, iw = panelW - 32*s;
        double cyy = cardTop + 13*s;
        // mini-header: navy label (left) + status note (right)
        p.setFont(avSans(10*s, QFont::DemiBold, 0.06*10*s));
        p.setPen(AV::NAVY);
        p.drawText(QRectF(ix, cyy, iw, 14*s), Qt::AlignLeft | Qt::AlignVCenter, miniLabel.toUpper());
        if (!miniNote.isEmpty()) {
            p.setFont(avSans(10*s, QFont::Normal));
            p.setPen(miniNoteColor);
            p.drawText(QRectF(ix, cyy, iw, 14*s), Qt::AlignRight | Qt::AlignVCenter, miniNote);
        }
        cyy += 14*s + 9*s;
        for (const KVRowDef& r : rows) {
            drawKVRow(p, QRectF(ix, cyy, iw, 31*s), r.label, r.value, s, r.swatch);
            cyy += 31*s;
        }
    };

    // LEFT — Mask & Seal. Redline note and time-over-redline row only where a
    // seal redline is meaningful; no leak channel at all → explicit "not
    // recorded" instead of measured-looking zeros.
    const QColor leakNoteCol = !leakBandable ? AV::GREY2
        : leakBand >= 2 ? AV::RED : leakBand == 1 ? AV::AMBER_DK : AV::GREEN;
    QVector<KVRowDef> leakRows;
    if (rd.leakChan != NoChannel) {
        leakRows = {
            { QObject::tr("Median leak"),   QObject::tr("%1 L/min").arg(QString::number(rd.medianLeak, 'f', 1)), nullptr },
            { QObject::tr("%1 percentile").arg(avUpperPctLabel()),
              QObject::tr("%1 L/min").arg(QString::number(rd.p95Leak, 'f', 1)), nullptr },
            { QObject::tr("Maximum (avg)"), QObject::tr("%1 L/min").arg(QString::number(rd.maxLeak, 'f', 1)), nullptr },
        };
        if (leakBandable)
            leakRows.append({ QObject::tr("Time over redline"),
                              rd.timeAboveRedlineStr + QStringLiteral(" · ")
                                  + QString::number(rd.pctOverRedline, 'f', 1) + QStringLiteral("%"), nullptr });
    } else {
        leakRows = { { QObject::tr("Leak data"), QObject::tr("not recorded"), nullptr } };
    }
    drawPanel(leftX, QObject::tr("Mask & Seal"), rd.maskDescription,
              rd.leakChan == NoChannel ? QObject::tr("Leak")
                  : leakBandable       ? QObject::tr("Unintentional leak")
                                       : QObject::tr("Total leak (incl. mask vent)"),
              leakBandable ? QObject::tr("%1% over redline").arg(QString::number(rd.pctOverRedline, 'f', 1))
                           : QString(),
              leakNoteCol,
              leakRows);

    // RIGHT — Respiratory Events breakdown (headline AHI sits in the band sub).
    QVector<KVRowDef> eventRows = {
        { QObject::tr("Clear-airway apnea (CAI)"), QObject::tr("%1 /h").arg(QString::number(rd.CentralAI, 'f', 2)),     &AV::EV_CENTRAL },
        { QObject::tr("Hypopnea (HI)"),           QObject::tr("%1 /h").arg(QString::number(rd.HI, 'f', 2)),            &AV::EV_HYP },
        { QObject::tr("Obstructive apnea (OAI)"), QObject::tr("%1 /h").arg(QString::number(rd.ObstructiveAI, 'f', 2)), &AV::EV_OBS },
        { QObject::tr("Respiratory effort arousals (RERA)"), QObject::tr("%1 /h").arg(QString::number(rd.ReraIndex, 'f', 1)), nullptr },
    };
    if (p_profile->hasChannel(CPAP_CSR))   // CSR only for devices that score it
        eventRows.append({ QObject::tr("Cheyne–Stokes respiration"),
                           QObject::tr("%1 min/night · %2%")
                               .arg(qRound(rd.daysUsed > 0 ? rd.totalCsrMinutes / rd.daysUsed : 0.0))
                               .arg(qRound(rd.avgCsrPctPerNight)), nullptr });
    drawPanel(rightX, QObject::tr("Respiratory Events"),
              QObject::tr("AHI %1 /h").arg(QString::number(rd.AHI, 'f', 1)),
              QObject::tr("Residual AHI vs target 5 /h"),
              ahiBand == 0 ? QObject::tr("within target") : QObject::tr("above target"),
              ahiBand >= 2 ? AV::RED : ahiBand == 1 ? AV::AMBER_DK : AV::GREEN,
              eventRows);

    // (f) Footer.
    const double footH = 40*s;
    drawFooter(p, QRectF(page.left(), page.bottom() - footH, page.width(), footH),
               pageNo, pageCount, QString(), s);
}

// ============================================================================
// Page 2 — Therapy & Device Settings. Text-heavy page (no charts).
// ============================================================================

// One therapy row: optional status dot, label, right-aligned colored value, and
// an optional status chip (used on the AHI headline). headline = larger value.
static void drawTherapyRow(QPainter& p, const QRectF& r, const QString& label,
                           const QString& value, const QColor& valueColor, int band,
                           const QColor* dot, double s, bool headline = false,
                           const QString& chip = QString(), const QColor& chipColor = QColor()) {
    if (band == 2) p.fillRect(r, AV::TINT_RED);   // abnormal rows get a calm red tint
    double labelX = r.left();
    if (dot) {
        const double d = 8*s;
        p.setPen(Qt::NoPen); p.setBrush(*dot);
        p.drawEllipse(QRectF(r.left(), r.center().y() - d/2, d, d));
        p.setBrush(Qt::NoBrush);
        labelX = r.left() + d + 8*s;
    }
    p.setFont(avSans(12*s, headline ? QFont::DemiBold : QFont::Normal));
    p.setPen(headline ? AV::INK : AV::GREY1);
    p.drawText(QRectF(labelX, r.top(), r.width() * 0.60, r.height()),
               Qt::AlignVCenter | Qt::AlignLeft, label);

    double valRight = r.right();
    if (!chip.isEmpty()) {
        p.setFont(avSans(8.5*s, QFont::DemiBold, 0.04*8.5*s));
        QFontMetricsF cfm(p.font());
        const double cw2 = cfm.horizontalAdvance(chip) + 16*s;
        const double chh = 16*s;
        const QRectF chipR(r.right() - cw2, r.center().y() - chh/2, cw2, chh);
        QColor cbg = chipColor; cbg.setAlphaF(0.14);
        QPainterPath pp; pp.addRoundedRect(chipR, chh/2, chh/2);
        p.fillPath(pp, cbg);
        p.setPen(chipColor);
        p.drawText(chipR, Qt::AlignCenter, chip);
        valRight = chipR.left() - 8*s;
    }
    p.setFont(avMono((headline ? 15.0 : 12.0)*s, band == 2 ? QFont::Bold : QFont::DemiBold));
    p.setPen(valueColor.isValid() ? valueColor : AV::INK);
    p.drawText(QRectF(r.left(), r.top(), valRight - r.left(), r.height()),
               Qt::AlignVCenter | Qt::AlignRight, value);

    p.setPen(QPen(AV::HAIR3, 1*s));
    p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.right(), r.bottom()));
}

static void avPage2(QPainter& p, const QRectF& page, const ReportData& rd,
                    int pageNo, int pageCount, double s)
{
    const QRectF content = avContentRect(page, s);
    const double cx = content.left(), cw = content.width();
    double y = page.top();

    // (a) Compact masthead
    const double mastH = 48*s;
    drawMasthead(p, QRectF(page.left(), y, page.width(), mastH), /*large*/false, rd,
                 QObject::tr("Therapy & Device Settings"), s);
    y += mastH + 14*s;

    const double bandH  = 30*s;
    const double rowH   = 30*s;
    const double padX   = 16*s;
    const double padTop = 10*s;
    const double padBot =  8*s;

    // (b) Therapy band + card with KV rows
    drawBand(p, QRectF(cx, y, cw, bandH), QObject::tr("Therapy"), QString(), s);
    y += bandH;

    // Clinical bands (sleep-medicine residual/treated cut-points; RERA & flow
    // limitation are device-estimated → capped at amber, never hard red).
    const AvBand ahiB  = avBand(rd.AHI,               5.0, 15.0);
    const AvBand caiB  = avBand(rd.CentralAI,         5.0, 10.0);
    const AvBand csrB  = avBand(rd.avgCsrPctPerNight, 1.0, 10.0);
    const AvBand reraB = avBand(rd.ReraIndex,         5.0, 15.0, /*capAmber*/true);
    const AvBand flB   = avBand(rd.p95FlowLimit,      0.1, 0.3,  /*capAmber*/true);
    // Leak 95th vs the ResMed unintentional-leak redline (only meaningful when the
    // device reports unintentional leak — not total leak).
    const bool leakBandable = !rd.isTotalLeak && rd.leakChan != NoChannel && rd.leakRedline > 0;
    const AvBand leakB = leakBandable
        ? avBand(rd.p95Leak, rd.leakRedline - 4.0, rd.leakRedline)
        : AvBand{0, AV::INK, AV::GREEN};
    const bool hasFL  = p_profile->hasChannel(CPAP_FLG);
    const bool hasCSR = p_profile->hasChannel(CPAP_CSR);

    const double subH = 18*s, trowH = 27*s;
    const int nData = 5 + (hasFL ? 1 : 0) + (hasCSR ? 1 : 0);  // AHI, residual, RERA, [FL], CAI, [CSR], leak
    const double therapyCardH = padTop + 3*subH + nData*trowH + padBot;
    drawCard(p, QRectF(cx, y, cw, therapyCardH), /*joinTop*/true, s);

    const double rx = cx + padX, rw = cw - 2*padX;
    double ky = y + padTop;
    auto sub     = [&](const QString& t){ drawOximeterSubheader(p, QRectF(rx, ky, rw, subH), t, s); ky += subH; };
    auto rowRect = [&]() -> QRectF { QRectF rr(rx, ky, rw, trowH); ky += trowH; return rr; };

    // — Respiratory control —
    sub(QObject::tr("Respiratory control"));
    {
        const QString chip = ahiB.band == 0 ? QObject::tr("Well controlled")
                           : ahiB.band == 1 ? QObject::tr("Mild residual")
                                            : QObject::tr("Inadequate");
        drawTherapyRow(p, rowRect(), QObject::tr("Apnea\xe2\x80\x93Hypopnea Index (AHI)"),
                       QString::number(rd.AHI, 'f', 1) + QStringLiteral(" /h"),
                       ahiB.value, ahiB.band, &ahiB.dot, s, /*headline*/true, chip, ahiB.dot);
    }
    {   // Residual events — merged breakdown with leading event-color swatches
        const QRectF r = rowRect();
        p.setFont(avSans(12*s, QFont::Normal)); p.setPen(AV::GREY1);
        p.drawText(QRectF(r.left(), r.top(), r.width()*0.4, r.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, QObject::tr("Residual events"));
        struct Seg { QColor c; QString t; };
        QVector<Seg> segs = {
            { AV::EV_OBS,     QObject::tr("Obstructive %1").arg(rd.ObstructiveAI, 0, 'f', 1) },
            { AV::EV_CENTRAL, QObject::tr("Clear airway %1").arg(rd.CentralAI, 0, 'f', 1) },
            { AV::EV_HYP,     QObject::tr("Hypopnea %1").arg(rd.HI, 0, 'f', 1) },
        };
        // Unclassified apneas count toward AHI — include them so the breakdown
        // reconciles with the headline value.
        if (rd.UnknownAI > 0.005)
            segs.append({ AV::EV_UNK, QObject::tr("Uncl. %1").arg(rd.UnknownAI, 0, 'f', 1) });
        p.setFont(avMono(11*s, QFont::DemiBold));
        QFontMetricsF fm(p.font());
        const double sw = 8*s, sgap = 5*s, between = 14*s;
        double total = 0; for (const auto& sg : segs) total += sw + sgap + fm.horizontalAdvance(sg.t);
        total += between * (segs.size() - 1);
        double x = r.right() - total;
        for (const auto& sg : segs) {
            QPainterPath pp; pp.addRoundedRect(QRectF(x, r.center().y() - sw/2, sw, sw), 2*s, 2*s);
            p.fillPath(pp, sg.c); x += sw + sgap;
            const double tw = fm.horizontalAdvance(sg.t);
            p.setPen(AV::INK); p.setFont(avMono(11*s, QFont::DemiBold));
            p.drawText(QRectF(x, r.top(), tw, r.height()), Qt::AlignVCenter | Qt::AlignLeft, sg.t);
            x += tw + between;
        }
        p.setPen(QPen(AV::HAIR3, 1*s)); p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.right(), r.bottom()));
    }
    drawTherapyRow(p, rowRect(), QObject::tr("Respiratory Effort\xe2\x80\x93Related Arousals (RERA)"),
                   QString::number(rd.ReraIndex, 'f', 1) + QStringLiteral(" /h"), reraB.value, reraB.band, nullptr, s);
    if (hasFL)
        drawTherapyRow(p, rowRect(), QObject::tr("Flow limitation (%1, device index)").arg(avUpperPctLabel()),
                       QString::number(rd.p95FlowLimit, 'f', 2), flB.value, flB.band, nullptr, s);

    // — Central & breathing pattern —
    sub(QObject::tr("Central & breathing pattern"));
    drawTherapyRow(p, rowRect(), QObject::tr("Clear-airway apnea index (CAI, device-scored)"),
                   QString::number(rd.CentralAI, 'f', 2) + QStringLiteral(" /h"), caiB.value, caiB.band, &caiB.dot, s);
    if (hasCSR)
        drawTherapyRow(p, rowRect(), QObject::tr("Cheyne\xe2\x80\x93Stokes respiration"),
                       QString("%1 min/night \xc2\xb7 %2%")
                           .arg(qRound(rd.daysUsed > 0 ? rd.totalCsrMinutes / rd.daysUsed : 0.0))
                           .arg(qRound(rd.avgCsrPctPerNight)),
                       csrB.value, csrB.band, &csrB.dot, s);

    // — Mask seal / leak —
    sub(QObject::tr("Mask seal / leak"));
    {
        const QString lv = QObject::tr("median %1 \xc2\xb7 %2 %3 \xc2\xb7 max(avg) %4 L/min")
            .arg(rd.medianLeak, 0, 'f', 1).arg(avUpperPctLabel())
            .arg(rd.p95Leak, 0, 'f', 1).arg(rd.maxLeak, 0, 'f', 1);
        const QColor* ld = leakBandable ? &leakB.dot : nullptr;
        // Label by what the device actually reports — calling total leak
        // "unintentional" would overstate seal leak on F&P/Philips devices.
        drawTherapyRow(p, rowRect(),
                       rd.isTotalLeak ? QObject::tr("Total leak (incl. mask vent)")
                                      : QObject::tr("Unintentional leak"), lv,
                       leakB.band == 2 ? AV::RED : AV::INK, leakB.band, ld, s);
    }

    y += therapyCardH + 14*s;

    // (c) Usage Detail band + card with KV rows
    const int ttm = qRound(rd.totalHours * 60.0);
    const QString totalHoursStr = QString("%1 h %2 m").arg(ttm / 60).arg(ttm % 60);

    drawBand(p, QRectF(cx, y, cw, bandH), QObject::tr("Usage Detail"), QString(), s);
    y += bandH;

    const QString thrStr = complianceThresholdStr(rd.complianceThreshold);
    // CMS-style criterion (≥ threshold h on ≥70% of nights in a consecutive
    // 30-day window) — the whole-period % above is not the payer test.
    const AvCmsWindow cms = avBestCmsWindow(rd);
    const QString cmsStr = !cms.valid
        ? QObject::tr("period shorter than 30 days")
        : QObject::tr("%1 \xc2\xb7 best window %2 \xe2\x80\x93 %3 (%4%)")
              .arg(cms.met ? QObject::tr("Met") : QObject::tr("Not met"))
              .arg(cms.start.toString("MMM d"))
              .arg(cms.start.addDays(29).toString("MMM d"))
              .arg(cms.pct);
    const QVector<QPair<QString,QString>> usageRows = {
        { QObject::tr("Nights used"),
          QString("%1 / %2 (%3%)")
            .arg(rd.daysUsed).arg(rd.totalDays).arg(qRound(rd.pctUsed)) },
        { QObject::tr("Nights \xe2\x89\xa5 %1 h").arg(thrStr),
          QString("%1 (%2%)").arg(rd.compliantDays).arg(qRound(rd.pctCompliant)) },
        { QObject::tr("Nights < %1 h").arg(thrStr),
          QString::number(rd.daysUsed - rd.compliantDays) },
        { QObject::tr("30-day compliance (\xe2\x89\xa5""70%)"), cmsStr },
        { QObject::tr("Total therapy hours"), totalHoursStr },
        { QObject::tr("Average (all \xc2\xb7 used)"),
          hmm(rd.avgHoursTotal) + QString::fromUtf8(" \xc2\xb7 ") + hmm(rd.avgHoursUsed) },
        { QObject::tr("Median per night"), hmm(rd.medianHoursUsed) },
    };
    const double usageCardH = padTop + usageRows.size() * rowH + padBot;
    drawCard(p, QRectF(cx, y, cw, usageCardH), /*joinTop*/true, s);
    {
        double ky = y + padTop;
        for (const auto& row : usageRows) {
            drawKVRow(p, QRectF(cx + padX, ky, cw - 2*padX, rowH), row.first, row.second, s);
            ky += rowH;
        }
    }
    y += usageCardH + 14*s;

    // (d) Device Settings band + info grid.
    // Mirrors compliancereport.cpp lines 2879–2939 ("Device Settings" section):
    // same channel IDs, same addDev-only-when-non-empty logic, same label strings.
    const QString lastDayStr = rd.lastDay
        ? rd.lastDay->date().toString("MMM d, yyyy")
        : rd.end.toString("MMM d, yyyy");
    drawBand(p, QRectF(cx, y, cw, bandH), QObject::tr("Device Settings"),
             QObject::tr("as on %1").arg(lastDayStr), s);
    y += bandH;

    QVector<QPair<QString,QString>> devCells;

    // Only add a cell when getSettingStr() returns a non-empty value (same guard as Classic addDev).
    auto addDevSetting = [&](const QString& label, ChannelID cid) {
        const QString v = getSettingStr(rd.lastDay, cid);
        if (!v.trimmed().isEmpty())
            devCells.append(qMakePair(label, v));
    };

    // Model — (brand + model) mirrors Classic devRows[0]
    const QString device = (rd.brand + QStringLiteral(" ") + rd.model).trimmed();
    if (!device.isEmpty())
        devCells.append(qMakePair(QObject::tr("Model"), device));

    // Mode
    if (!rd.modeStr.isEmpty())
        devCells.append(qMakePair(QObject::tr("Mode"), rd.modeStr));

    // Pressure — pressDisplay already has the cmH2O unit appended (see ReportData init)
    if (!rd.pressDisplay.trimmed().isEmpty())
        devCells.append(qMakePair(QObject::tr("Pressure"), rd.pressDisplay));

    // EPR — strip redundant "EPR: " prefix (mirrors Classic epr-stripping)
    {
        QString epr = rd.relief;
        if (epr.startsWith(QStringLiteral("EPR: "), Qt::CaseInsensitive)) epr = epr.mid(5);
        if (!epr.trimmed().isEmpty())
            devCells.append(qMakePair(QObject::tr("EPR"), epr));
    }

    // Ramp enable; ramp time/pressure only when ramp is active (mirrors Classic gate)
    addDevSetting(QObject::tr("Ramp"), RMS9_RampEnable);
    if (rd.lastDay && rd.lastDay->settingExists(CPAP_RampTime)
            && rd.lastDay->settings_wavg(CPAP_RampTime) > 0) {
        if (!rd.rampTime.trimmed().isEmpty())
            devCells.append(qMakePair(QObject::tr("Ramp Time"), rd.rampTime));
        if (!rd.rampPres.trimmed().isEmpty())
            devCells.append(qMakePair(QObject::tr("Ramp Pressure"), rd.rampPres));
    }

    // ResMed-specific channels (degrade cleanly for non-ResMed devices)
    addDevSetting(QObject::tr("Mask"),                 RMS9_Mask);
    addDevSetting(QObject::tr("Humidifier"),           RMS9_HumidStatus);
    addDevSetting(QObject::tr("Humidity Level"),       RMS9_HumidLevel);
    addDevSetting(QObject::tr("Climate Control"),      RMS9_ClimateControl);
    addDevSetting(QObject::tr("Temperature"),          RMS9_Temp);
    addDevSetting(QObject::tr("Temperature Enable"),   RMS9_TempEnable);
    addDevSetting(QObject::tr("Response"),             RMAS1x_Comfort);
    addDevSetting(QObject::tr("Antibacterial Filter"), RMS9_ABFilter);
    addDevSetting(QObject::tr("Smart Start"),          RMS9_SmartStart);
    addDevSetting(QObject::tr("Patient View"),         RMAS11_PtView);

    const int devCols = 2;
    // Cap the grid to the rows that fit above callout (64) + footer (40) +
    // gaps, dropping trailing cells (the least clinically relevant settings sit
    // at the end of the list) — a full ResMed settings dump must not overlap
    // the clinical-note callout.
    const int maxDevRows = qMax(1, (int)qFloor(
        (page.bottom() - 40*s - 12*s - 64*s - 14*s - y) / (40*s)));
    int devRows = qMax(1, ((int)devCells.size() + devCols - 1) / devCols);
    if (devRows > maxDevRows) {
        devRows = maxDevRows;
        devCells.resize(devRows * devCols);
    }
    const double devGridH = devRows * 40*s;
    if (!devCells.isEmpty())
        drawInfoGrid(p, QRectF(cx, y, cw, devGridH), devCells, devCols, s,
                     QVector<double>(), /*roundTop*/false, /*roundBottom*/true);
    y += devGridH + 14*s;

    // (e) Clinical note callout — data-driven, no fabrication. The lead-in
    // states provenance so the text cannot be mistaken for a clinician's read.
    QString note = QObject::tr("Auto impression (device data): ");
    note += (ahiB.band == 0 ? QObject::tr("AHI well controlled (%1/h)")
           : ahiB.band == 1 ? QObject::tr("mild residual AHI (%1/h)")
                            : QObject::tr("inadequate control, AHI %1/h"))
                .arg(QString::number(rd.AHI, 'f', 1));
    if (leakBandable) {
        if (leakB.band == 0)
            note += QObject::tr(", leak acceptable (%1 %2 < %3)")
                        .arg(avUpperPctLabel())
                        .arg(QString::number(rd.p95Leak, 'f', 1)).arg(qRound(rd.leakRedline));
        else
            note += QObject::tr(", elevated leak (%1 %2 \xe2\x89\xa5 %3) \xe2\x80\x94 AHI may be unreliable")
                        .arg(avUpperPctLabel())
                        .arg(QString::number(rd.p95Leak, 'f', 1)).arg(qRound(rd.leakRedline));
    }
    note += QStringLiteral(".");
    // TECSA-style flag needs absolute burden, not just proportion: treated
    // AHI ≥ 5 with CAI ≥ 5 and central-predominant (≥ 50% of AHI). A CAI of
    // 1.2 against an AHI of 2.0 is well-controlled therapy, not a warning.
    const bool centralPredominant = rd.AHI >= 5.0 && rd.CentralAI >= 5.0
                                    && rd.CentralAI >= 0.5 * rd.AHI;
    if (centralPredominant)
        note += QStringLiteral(" ") + QObject::tr(
            "Residual events central-predominant \xe2\x80\x94 monitor for "
            "treatment-emergent central apnea if symptoms recur.");
    else if (caiB.band >= 1)
        note += QStringLiteral(" ") + QObject::tr(
            "Elevated clear-airway apnea index (%1/h) \xe2\x80\x94 review centrals "
            "if symptoms persist.").arg(QString::number(rd.CentralAI, 'f', 1));
    if (csrB.band >= 2)
        note += QStringLiteral(" ") + QObject::tr(
            "Cheyne\xe2\x80\x93Stokes present (%1%) \xe2\x80\x94 consider cardiac / CSA evaluation.")
            .arg(qRound(rd.avgCsrPctPerNight));

    const double calloutH = 64*s;
    // Never let the callout collide with the footer, even if the device grid runs long.
    const double calloutY = qMin(y, page.bottom() - 40*s - 12*s - calloutH);
    drawInfoCallout(p, QRectF(cx, calloutY, cw, calloutH), note, s);

    // (f) Footer
    const double footH = 40*s;
    drawFooter(p, QRectF(page.left(), page.bottom() - footH, page.width(), footH),
               pageNo, pageCount, QString(), s);
}

// ============================================================================
// Page 3 — Efficacy Trends. Three chart cards stacked: stacked AHI, leak
// box-whisker, and pressure trace. Card heights are computed dynamically so
// the three cards fill the space between the section band and the footer.
// ============================================================================
static void avPage3(QPainter& p, const QRectF& page, const ReportData& rd,
                    int pageNo, int pageCount, double s)
{
    const QRectF content = avContentRect(page, s);
    const double cx = content.left(), cw = content.width();
    double y = page.top();

    // (a) Compact masthead
    const double mastH = 48*s;
    drawMasthead(p, QRectF(page.left(), y, page.width(), mastH), /*large*/false, rd,
                 QObject::tr("Efficacy Trends"), s);
    y += mastH + 14*s;

    // (b) Section band
    const double bandH = 30*s;
    drawBand(p, QRectF(cx, y, cw, bandH), QObject::tr("Efficacy Trends"),
             QObject::tr("nightly events, leak & pressure"), s);
    y += bandH;

    // (c) Chart cards. A local lambda draws each card: FILL_BLUE header strip
    //     (HAIR2 bottom rule, NAVY title left, BODY summary right) over a HAIR1-
    //     bordered card, then calls drawFn with the inner plot rect. Card heights
    //     are apportioned to fill the available space between the band and footer.
    const double footH  = 40*s;
    const double hdrH   = 28*s;   // card header strip height
    const double padX   = 14*s;   // horizontal padding inside card
    const double padTop = 10*s;   // gap between header bottom and chart top
    const double padBot =  8*s;   // gap between chart bottom and card bottom
    const double cardGap = 10*s;  // vertical gap between consecutive cards
    // Card 3 needs the same presence gate the Classic renderer applies to its
    // pressure chart: rd.avgPressure/p95Pressure/maxPressure are only ever
    // assigned inside `if (p_profile->hasChannel(CPAP_Pressure))`, so on a
    // bilevel/ASV profile (which records IPAP/EPAP instead) they stay 0.0 and the
    // card would render an empty default 6-14 axis beside a stats column of 0.0s.
    const bool hasPress = p_profile && p_profile->hasChannel(CPAP_Pressure);
    const int numCards = 1 + (rd.leakChan != NoChannel ? 1 : 0) + (hasPress ? 1 : 0);
    const double available = page.bottom() - footH - y;
    const double plotH = (available
                          - numCards * (hdrH + padTop + padBot)
                          - (numCards - 1) * cardGap) / numCards;
    const double cardH = hdrH + padTop + plotH + padBot;

    // Left stats column (AirView style) reserved beside each chart.
    const double statsW   = 170*s;
    const double statsGap = 14*s;

    // Generic lambda: draws one chart card at vertical position cardY, with a
    // left stats column and the chart to its right.
    auto drawChartCard = [&](double cardY, const QString& title, const QVector<AvStat>& stats,
                             auto drawFn) {
        avCardChrome(p, QRectF(cx, cardY, cw, cardH), hdrH, s);

        // Title: NAVY 10px bold, ~0.09em tracking, uppercase, left-aligned
        p.setFont(avSans(10*s, QFont::Bold, 0.09*10*s));
        p.setPen(AV::NAVY);
        p.drawText(QRectF(cx + padX, cardY, cw - 2*padX, hdrH),
                   Qt::AlignVCenter | Qt::AlignLeft, title.toUpper());

        // Left stats column
        drawChartStats(p, QRectF(cx + padX, cardY + hdrH + padTop, statsW, plotH), stats, s);

        // Chart plot rect (to the right of the stats column)
        const QRectF plotRect(cx + padX + statsW + statsGap, cardY + hdrH + padTop,
                              cw - 2*padX - statsW - statsGap, plotH);
        drawFn(plotRect);
    };

    const QColor evHyp = AV::EV_HYP, evObs = AV::EV_OBS;  // addressable for swatch pointers

    // Card 1: Residual AHI — stacked nightly bar chart + AHI breakdown legend
    {
        const QVector<AvStat> ahiStats = {
            { QObject::tr("AHI"),        QString::number(rd.AHI, 'f', 1),           evObs, &evObs, true  },
            { QObject::tr("HI"),         QString::number(rd.HI, 'f', 1),            QColor(), &evHyp, false },
            { QObject::tr("AI"),         QString::number(rd.AI, 'f', 1),            QColor(), &evObs, false },
            { QObject::tr("CAI"),        QString::number(rd.CentralAI, 'f', 2),     QColor(), nullptr, false },
            { QObject::tr("OAI"),        QString::number(rd.ObstructiveAI, 'f', 2), QColor(), nullptr, false },
            { QObject::tr("UAI"),        QString::number(rd.UnknownAI, 'f', 2),     QColor(), nullptr, false },
            { QObject::tr("RERA"),       QString::number(rd.ReraIndex, 'f', 1),     QColor(), nullptr, false },
            { QObject::tr("CSR% (avg)"), QString::number(rd.avgCsrPctPerNight, 'f', 1) + QStringLiteral("%"),
                                         QColor(), nullptr, false },
        };
        drawChartCard(y, QObject::tr("Residual AHI \xe2\x80\x94 events / hour"), ahiStats,
                      [&](const QRectF& r){ drawAhiStacked(p, r, rd, s); });
        y += cardH + cardGap;
    }

    // Card 2: Mask Leak Rate — box-whisker (skipped when no leak channel)
    if (rd.leakChan != NoChannel) {
        QVector<AvStat> leakStats;
        if (!rd.isTotalLeak)
            leakStats.append({ QObject::tr("Set threshold"),
                               QString("%1 L/min").arg(rd.leakRedline, 0, 'f', 1), QColor(), nullptr, false });
        leakStats.append({ QObject::tr("Maximum (avg)"), QString::number(rd.maxLeak, 'f', 1),    QColor(),   nullptr, false });
        leakStats.append({ QObject::tr("%1 % (avg)").arg(avUpperPctLabel()),  QString::number(rd.p95Leak, 'f', 1),    AV::AMBER,  nullptr, false });
        leakStats.append({ QObject::tr("Median (avg)"),  QString::number(rd.medianLeak, 'f', 1), QColor(),   nullptr, false });
        // Redline % only where a seal redline is meaningful (mirrors Classic's
        // total-leak suppression and this report's own pages 1/7).
        if (!rd.isTotalLeak)
            leakStats.append({ QObject::tr("Over redline"),  QString::number(rd.pctOverRedline, 'f', 1) + QStringLiteral("%"),
                               QColor(), nullptr, false });
        const double redline = rd.isTotalLeak ? 0.0 : rd.leakRedline;
        drawChartCard(y, QObject::tr("Mask Leak Rate \xe2\x80\x94 L / min"), leakStats,
                      [&](const QRectF& r){
                          drawChannelBand(p, r, rd, rd.daysMap, rd.leakChan,
                                          AV::LEAK_COLORS, redline, s);
                      });
        y += cardH + cardGap;
    }

    // Card 3: Mask Pressure — fixed line or auto band, with set-pressure stats
    // (skipped when the profile has no CPAP_Pressure channel)
    if (hasPress) {
        const QString epr = rd.relief.isEmpty() ? QObject::tr("Off") : rd.relief;
        // Set pressure from the same reliable source as the device-settings page
        // (rd.pressDisplay → "Min 7.0 Max 11.0"); strip the unit since the chart
        // title already says cmH₂O.
        QString setPress = rd.pressDisplay;
        setPress.remove(QStringLiteral("(cmH2O)")).remove(QStringLiteral("cmH2O"));
        setPress = setPress.trimmed();
        const QVector<AvStat> pressStats = {
            { QObject::tr("Mode"),          rd.modeStr.isEmpty() ? QStringLiteral("\xe2\x80\x94") : rd.modeStr, QColor(), nullptr, false },
            { QObject::tr("Set EPR"),       epr, QColor(), nullptr, false },
            { QObject::tr("Set pressure"),  setPress.isEmpty() ? QStringLiteral("\xe2\x80\x94") : setPress, QColor(), nullptr, false },
            { QString(), QString(), QColor(), nullptr, false },  // spacer
            { QObject::tr("Maximum (avg)"), QString::number(rd.maxPressure, 'f', 1), QColor(),   nullptr, false },
            { QObject::tr("%1 % (avg)").arg(avUpperPctLabel()),  QString::number(rd.p95Pressure, 'f', 1), AV::ACCENT, nullptr, false },
            { QObject::tr("Median (avg)"),  QString::number(rd.avgPressure, 'f', 1), QColor(),   nullptr, false },
        };
        drawChartCard(y, QObject::tr("Pressure \xe2\x80\x94 cmH\xe2\x82\x82O"), pressStats,
                      [&](const QRectF& r){
                          drawChannelBand(p, r, rd, rd.daysMap, CPAP_Pressure,
                                          AV::PRESSURE_COLORS, /*redline*/0, s);
                      });
        // no y increment — footer uses page.bottom() directly
    }

    // (d) Footer
    drawFooter(p, QRectF(page.left(), page.bottom() - footH, page.width(), footH),
               pageNo, pageCount, QString(), s);
}

// ============================================================================
// Page 4 — Respiratory Channel Trends. Up to three box-whisker chart cards
// (Minute Ventilation, Respiratory Rate, Tidal Volume), shown only when the
// channel is both present on the device profile and selected in the dialog.
// No reference ranges are drawn — interpretation is left to the clinician.
// ============================================================================
static void avPage4(QPainter& p, const QRectF& page, const ReportData& rd,
                    const QList<SelectedChannel>& channels, int pageNo, int pageCount, double s)
{
    const QRectF content = avContentRect(page, s);
    const double cx = content.left(), cw = content.width();
    double y = page.top();

    // (a) Compact masthead
    const double mastH = 48*s;
    drawMasthead(p, QRectF(page.left(), y, page.width(), mastH), /*large*/false, rd,
                 QObject::tr("Respiratory Channels"), s);
    y += mastH + 14*s;

    // (b) Section band
    const double bandH = 30*s;
    drawBand(p, QRectF(cx, y, cw, bandH), QObject::tr("Respiratory Channel Trends"),
             QObject::tr("minute ventilation, rate & tidal volume"), s);
    y += bandH;

    // (c) Selection + presence gate
    auto wanted = [&](ChannelID cid) -> bool {
        if (!p_profile->hasChannel(cid)) return false;
        for (const auto& ch : channels) if ((ChannelID)ch.id == cid) return true;
        return false;
    };

    const bool wantMV  = wanted(CPAP_MinuteVent);
    const bool wantRR  = wanted(CPAP_RespRate);
    const bool wantTV  = wanted(CPAP_TidalVolume);
    const int numCards = (wantMV ? 1 : 0) + (wantRR ? 1 : 0) + (wantTV ? 1 : 0);

    if (numCards == 0) {
        // No channels selected: draw a muted note
        p.setFont(avSans(11*s, QFont::Normal));
        p.setPen(AV::GREY2);
        p.drawText(QRectF(cx, y, cw, page.bottom() - y - 40*s),
                   Qt::AlignCenter, QObject::tr("No respiratory channel data selected."));
    } else {
        // Chart cards share available space evenly
        const double footH  = 40*s;
        const double hdrH   = 28*s;
        const double padX   = 14*s;
        const double padTop = 10*s;
        const double padBot =  8*s;
        const double cardGap = 10*s;
        const double available = page.bottom() - footH - y;
        const double plotH = (available
                              - numCards * (hdrH + padTop + padBot)
                              - (numCards - 1) * cardGap) / numCards;
        const double cardH = hdrH + padTop + plotH + padBot;

        const double statsW   = 170*s;
        const double statsGap = 14*s;

        auto drawChartCard = [&](double cardY, const QString& title, const QVector<AvStat>& stats,
                                 auto drawFn) {
            avCardChrome(p, QRectF(cx, cardY, cw, cardH), hdrH, s);

            // Title: NAVY 10px bold uppercase left-aligned
            p.setFont(avSans(10*s, QFont::Bold, 0.09*10*s));
            p.setPen(AV::NAVY);
            p.drawText(QRectF(cx + padX, cardY, cw - 2*padX, hdrH),
                       Qt::AlignVCenter | Qt::AlignLeft, title.toUpper());

            // Left stats column + chart to its right
            drawChartStats(p, QRectF(cx + padX, cardY + hdrH + padTop, statsW, plotH), stats, s);
            const QRectF plotRect(cx + padX + statsW + statsGap, cardY + hdrH + padTop,
                                  cw - 2*padX - statsW - statsGap, plotH);
            drawFn(plotRect);
        };

        // Per-channel Maximum / 95th / Median (avg) stats column (AirView style).
        auto chanStats = [&](ChannelID cid, int decimals) -> QVector<AvStat> {
            auto fmt = [&](double v){ return QString::number(v, 'f', decimals); };
            return {
                { QObject::tr("Maximum (avg)"), fmt(calcDayAvgMax(cid, rd.daysMap)),               QColor(),   nullptr, false },
                { QObject::tr("%1 % (avg)").arg(avUpperPctLabel()),  fmt(calcDayAvgPercentile(cid, avUpperPct(), rd.daysMap)), AV::ACCENT, nullptr, false },
                { QObject::tr("Median (avg)"),  fmt(calcDayAvgPercentile(cid, 0.5, rd.daysMap)),   QColor(),   nullptr, false },
            };
        };

        // Card 1: Minute Ventilation
        if (wantMV) {
            drawChartCard(y, QObject::tr("Minute Ventilation \xe2\x80\x94 L / min"), chanStats(CPAP_MinuteVent, 1),
                          [&](const QRectF& r){
                              drawChannelBand(p, r, rd, rd.daysMap, CPAP_MinuteVent,
                                              AV::MINVENT_COLORS, /*redline*/0, s);
                          });
            y += cardH + cardGap;
        }

        // Card 2: Respiratory Rate
        if (wantRR) {
            drawChartCard(y, QObject::tr("Respiratory Rate \xe2\x80\x94 breaths / min"), chanStats(CPAP_RespRate, 0),
                          [&](const QRectF& r){
                              drawChannelBand(p, r, rd, rd.daysMap, CPAP_RespRate,
                                              AV::RESPRATE_COLORS, /*redline*/0, s);
                          });
            y += cardH + cardGap;
        }

        // Card 3: Tidal Volume
        if (wantTV) {
            drawChartCard(y, QObject::tr("Tidal Volume \xe2\x80\x94 mL"), chanStats(CPAP_TidalVolume, 0),
                          [&](const QRectF& r){
                              drawChannelBand(p, r, rd, rd.daysMap, CPAP_TidalVolume,
                                              AV::TIDALVOL_COLORS, /*redline*/0, s);
                          });
            // no y increment — last card
        }
    }

    // (d) Footer
    const double footH = 40*s;
    drawFooter(p, QRectF(page.left(), page.bottom() - footH, page.width(), footH),
               pageNo, pageCount, QString(), s);
}

// ============================================================================
// Page 5 — Oximetry & Oxygenation. GATED: drawn only when oximetry data exists
// (rd.usedDaysOxiAny > 0); returns false (drawing nothing) otherwise. Two stacked
// chart cards (SpO2 range, pulse box-whisker) over a two-up footer block:
// Overnight Oxygen Summary (left) + a compact Lowest-Oxygen Night panel (right).
// ============================================================================
static bool avPage5(QPainter& p, const QRectF& page, const ReportData& rd,
                    bool selSpo2, bool selPulse, int pageNo, int pageCount, double s)
{
    if (rd.usedDaysOxiAny == 0) return false;

    const QRectF content = avContentRect(page, s);
    const double cx = content.left(), cw = content.width();
    double y = page.top();

    // (a) Compact masthead
    const double mastH = 48*s;
    drawMasthead(p, QRectF(page.left(), y, page.width(), mastH), /*large*/false, rd,
                 QObject::tr("Oximetry"), s);
    y += mastH + 14*s;

    // (b) Section band
    const double bandH = 30*s;
    drawBand(p, QRectF(cx, y, cw, bandH), QObject::tr("Oximetry & Oxygenation"),
             QObject::tr("SpO₂, pulse & desaturation"), s);
    y += bandH;

    // (c) Geometry: reserve the footer + bottom two-up block, split the rest
    //     between the chart cards.
    const double footH      = 40*s;
    const double twoUpCardH = 178*s;
    const double twoUpGap   = 14*s;

    const double hdrH = 28*s, padX = 14*s, padTop = 10*s, padBot = 8*s, cardGap = 10*s;
    // Gate each card on data-in-range (hasSpO2/hasPulse only assert the channel
    // exists on the profile, not that this period recorded any) AND on the
    // dialog's channel selection — trend charts honour the checklist exactly
    // like Classic's channel charts. The summary block below stays data-gated.
    const bool showSpo2  = rd.hasSpO2 && rd.usedDaysOxi > 0 && selSpo2;
    const bool showPulse = rd.usedDaysPulse > 0 && selPulse;
    const int numCards = (showSpo2 ? 1 : 0) + (showPulse ? 1 : 0);
    // Both charts deselected → the summary block moves up under the band
    // instead of leaving a page-height hole above it.
    const double twoUpTop = (numCards == 0) ? y : (page.bottom() - footH - twoUpCardH);
    const double chartsBottom = twoUpTop - twoUpGap;
    const double available = chartsBottom - y;
    const double plotH = numCards > 0
        ? (available - numCards * (hdrH + padTop + padBot)
           - (numCards - 1) * cardGap) / numCards
        : 0.0;
    const double cardH = hdrH + padTop + plotH + padBot;

    auto drawChartCard = [&](double cardY, const QString& title, const QString& summary,
                             auto drawFn) {
        avCardChrome(p, QRectF(cx, cardY, cw, cardH), hdrH, s);
        p.setFont(avSans(10*s, QFont::Bold, 0.09*10*s));
        p.setPen(AV::NAVY);
        p.drawText(QRectF(cx + padX, cardY, cw - 2*padX, hdrH),
                   Qt::AlignVCenter | Qt::AlignLeft, title.toUpper());
        p.setFont(avSans(9.5*s));
        p.setPen(AV::BODY);
        p.drawText(QRectF(cx + padX, cardY, cw - 2*padX, hdrH),
                   Qt::AlignVCenter | Qt::AlignRight, summary);
        const QRectF plotRect(cx + padX, cardY + hdrH + padTop, cw - 2*padX, plotH);
        drawFn(plotRect);
    };

    // Card 1: SpO2 nightly range — only when SpO₂ data exists in the period
    if (showSpo2) {
        const QString sum = rd.minSpO2Set
            ? QObject::tr("Avg %1% · low %2%").arg(QString::number(rd.avgSpO2Period, 'f', 1))
                                              .arg(qRound(rd.minSpO2))
            : QObject::tr("Avg %1%").arg(QString::number(rd.avgSpO2Period, 'f', 1));
        drawChartCard(y, QObject::tr("Oxygen Saturation (SpO₂) — %"), sum,
                      [&](const QRectF& r){ drawSpo2Chart(p, r, rd, s); });
        y += cardH + cardGap;
    }

    // Card 2: Pulse rate box-whisker — only when pulse data exists in the period
    if (showPulse) {
        const QString sum = QObject::tr("Avg %1 · min/max %2/%3 bpm")
            .arg(qRound(rd.avgPulsePeriod)).arg(qRound(rd.minPulsePeriod)).arg(qRound(rd.maxPulsePeriod));
        drawChartCard(y, QObject::tr("Pulse Rate — bpm"), sum,
                      [&](const QRectF& r){
                          drawChannelBand(p, r, rd, rd.oxiDaysMap, OXI_Pulse,
                                          AV::PULSE_COLORS, /*redline*/0, s);
                      });
    }

    // (d) Two-up footer block.
    const double colGap = 14*s, colW = (cw - colGap) / 2.0;

    // Shared card frame: HAIR1 border + FILL_BLUE header strip + title (and an
    // optional right-aligned caption); returns the inner content rect.
    auto cardFrame = [&](double fx, double ft, double fw, const QString& title,
                         const QString& rightText) -> QRectF {
        avCardChrome(p, QRectF(fx, ft, fw, twoUpCardH), hdrH, s);
        p.setFont(avSans(10*s, QFont::Bold, 0.09*10*s));
        p.setPen(AV::NAVY);
        p.drawText(QRectF(fx + padX, ft, fw - 2*padX, hdrH),
                   Qt::AlignVCenter | Qt::AlignLeft, title.toUpper());
        if (!rightText.isEmpty()) {
            p.setFont(avSans(9*s, QFont::Normal));
            p.setPen(AV::CAPTION);
            p.drawText(QRectF(fx + padX, ft, fw - 2*padX, hdrH),
                       Qt::AlignVCenter | Qt::AlignRight, rightText);
        }
        return QRectF(fx + 15*s, ft + hdrH + 12*s, fw - 30*s, twoUpCardH - hdrH - 22*s);
    };

    // LEFT — Overnight Oxygen Summary
    {
        const QRectF in = cardFrame(cx, twoUpTop, colW, QObject::tr("Overnight Oxygen Summary"), QString());
        double iy = in.top();
        if (showSpo2) {
            // Round the three zones with drift correction (largest zone absorbs
            // the residual) so the bar and legend always total exactly 100%.
            int z95 = qRound(rd.spo2Zones.pctGe95);
            int z90 = qRound(rd.spo2Zones.pct90to95);
            int zlt = qRound(rd.spo2Zones.pctLt90);
            const int drift = 100 - (z95 + z90 + zlt);
            if      (z95 >= z90 && z95 >= zlt) z95 += drift;
            else if (z90 >= zlt)               z90 += drift;
            else                               zlt += drift;
            drawDistributionBar(p, QRectF(in.left(), iy, in.width(), 20*s),
                                { { (double)z95, AV::GREEN },
                                  { (double)z90, AV::AMBER },
                                  { (double)zlt, AV::RED   } }, s);
            iy += 20*s + 8*s;
            drawChartLegend(p, QRectF(in.left(), iy, in.width(), 12*s), QVector<AvLegendItem>{
                { AV::GREEN, QObject::tr("≥95% %1%").arg(z95),   false },
                { AV::AMBER, QObject::tr("90–95% %1%").arg(z90), false },
                { AV::RED,   QObject::tr("<90% %1%").arg(zlt),   false },
            }, s);
            iy += 12*s + 10*s;
        }

        const auto spo2 = [&](const QString& v){ return showSpo2 ? v : QString::fromUtf8("—"); };
        const QVector<QPair<QString,QString>> kvs = {
            { QObject::tr("ODI"),
              spo2(QObject::tr("%1 /h (≥%2% dip)").arg(rd.odi, 0, 'f', 1).arg(qRound(rd.dropPct))) },
            { QObject::tr("Time < 90% (T90)"), spo2(rd.t90Str.isEmpty() ? QStringLiteral("—") : rd.t90Str) },
            { QObject::tr("Time < 88%"),       spo2(minsFromSecs(rd.t88Secs)) },
            { QObject::tr("% time in desat"),  spo2(QString::number(rd.pctTimeInDesat, 'f', 1) + QStringLiteral("%")) },
            { QObject::tr("Lowest SpO₂"),
              spo2(rd.minSpO2Set ? QString::number(qRound(rd.minSpO2)) + QStringLiteral("%") : QStringLiteral("—")) },
            { QObject::tr("Average SpO₂"),     spo2(QString::number(rd.avgSpO2Period, 'f', 1) + QStringLiteral("%")) },
        };
        const double kvRowH = 22*s, kvColGap = 16*s, kvColW = (in.width() - kvColGap) / 2.0;
        for (int i = 0; i < kvs.size(); i++) {
            const int col = i / 3, row = i % 3;
            const double kx = in.left() + col * (kvColW + kvColGap);
            const double ky = iy + row * kvRowH;
            drawKVRow(p, QRectF(kx, ky, kvColW, kvRowH), kvs[i].first, kvs[i].second, s);
        }
    }

    // RIGHT — Lowest-Oxygen Night (compact QPainter panel: worst night's date,
    //          nadir, median + max for that night). Worst night = the oxi day with
    //          the lowest valid nightly SpO2 minimum.
    {
        QDate worstDate; double worstMin = 200.0, worstMed = 0.0, worstMax = 0.0;
        for (auto it = rd.oxiDaysMap.constBegin(); it != rd.oxiDaysMap.constEnd(); ++it) {
            Day* day = it.value();
            if (day->timeAboveThreshold(OXI_SPO2, 0.0) <= 0.0) continue;
            // Worst night by DROPOUT-FILTERED nadir (same filter as rd.minSpO2)
            // so a probe-slip artifact can never select the wrong night.
            const FilteredStats fs = calcDayFilteredStats(day, OXI_SPO2, rd.oxiSourceType,
                                                          AV_SPO2_ARTIFACT_FLOOR, 0.0,
                                                          AV_SPO2_MAX_FALL_PCT_PER_SEC);
            if (!fs.found || fs.min <= 0.0) continue;
            if (fs.min < worstMin) {
                worstMin  = fs.min;
                worstDate = it.key();
                worstMed  = day->percentile(OXI_SPO2, 0.5);
                worstMax  = fs.max;
            }
        }
        const QString dateStr = worstDate.isValid() ? worstDate.toString("MMM d") : QStringLiteral("—");
        const QRectF in = cardFrame(cx + colW + colGap, twoUpTop, colW,
                                    QObject::tr("Lowest-Oxygen Night"), dateStr);
        const double nadir = worstMin;   // this night's own filtered nadir
        const QVector<QPair<QString,QString>> kvs = {
            { QObject::tr("Date"),       dateStr },
            { QObject::tr("Lowest SpO₂"),
              worstDate.isValid() ? QString::number(qRound(nadir)) + QStringLiteral("%") : QStringLiteral("—") },
            { QObject::tr("Night median"),
              worstDate.isValid() ? QString::number(qRound(worstMed)) + QStringLiteral("%") : QStringLiteral("—") },
            { QObject::tr("Night max"),
              worstDate.isValid() ? QString::number(qRound(worstMax)) + QStringLiteral("%") : QStringLiteral("—") },
        };
        double ky = in.top();
        const double kvRowH = 24*s;
        for (const auto& kv : kvs) {
            drawKVRow(p, QRectF(in.left(), ky, in.width(), kvRowH), kv.first, kv.second, s);
            ky += kvRowH;
        }
        ky += 6*s;
        p.setFont(avSans(9*s, QFont::Normal));
        p.setPen(AV::GREY3);
        p.drawText(QRectF(in.left(), ky, in.width(), in.bottom() - ky),
                   Qt::TextWordWrap | Qt::AlignTop | Qt::AlignLeft,
                   QObject::tr("Lowest valid SpO₂ recorded across the night."));
    }

    // (e) Footer
    drawFooter(p, QRectF(page.left(), page.bottom() - footH, page.width(), footH),
               pageNo, pageCount, QString(), s);
    return true;
}

// ============================================================================
// Page 6 — Sleep Architecture & Oximeter Statistics. GATED: drawn only when
// oximetry data exists OR sleep-stage data is present; returns false otherwise.
// The Sleep Architecture section is itself gated on rd.sleep.present (rendered as
// nothing when absent — no empty card).
// ============================================================================
static bool avPage6(QPainter& p, const QRectF& page, const ReportData& rd,
                    const QList<SelectedChannel>& channels, int pageNo, int pageCount, double s)
{
    bool includeBedtime = false;
    bool includeSleepStages = false;
    bool includeOxiStats = false;
    for (const auto& ch : channels) {
        if ((ChannelID)ch.id == COMPLIANCE_Bedtime) includeBedtime = true;
        else if ((ChannelID)ch.id == COMPLIANCE_SleepStages) includeSleepStages = true;
        else if ((ChannelID)ch.id == COMPLIANCE_OximeterStats) includeOxiStats = true;
    }

    const bool showSleep = rd.sleep.present && includeSleepStages;
    const bool showOxi = rd.usedDaysOxiAny > 0 && includeOxiStats;

    if (!showOxi && !showSleep) return false;

    const QRectF content = avContentRect(page, s);
    const double cx = content.left(), cw = content.width();
    double y = page.top();

    // (a) Compact masthead
    const double mastH = 48*s;
    drawMasthead(p, QRectF(page.left(), y, page.width(), mastH), /*large*/false, rd,
                 QObject::tr("Sleep & Oximeter Stats"), s);
    y += mastH + 14*s;

    // (b) Section band
    const double bandH = 30*s;
    const QString bandSub = showSleep
        ? QObject::tr("per-night averages · %1 sleep-stage nights").arg(rd.sleep.sleepStageNights)
        : QObject::tr("oximeter summary");
    drawBand(p, QRectF(cx, y, cw, bandH), QObject::tr("Sleep Architecture & Oximeter"), bandSub, s);
    y += bandH;

    const double padX = 16*s, kvRowH = 22*s, subH = 14*s;

    // (c) Sleep Architecture — ONLY when sleep-stage data is present.
    if (showSleep) {
        const SleepStageStats& sl = rd.sleep;
        const double cardPadTop = 12*s, cardPadBot = 12*s;
        const double stageCardsH = 54*s, barH = 28*s;
        const double kvRows = 2;       // 6 KV in 3 columns → 2 rows
        const double caveatH = 16*s;   // consumer-device validity caveat line
        const double contentH = subH + 8*s + stageCardsH + 12*s + barH + 14*s + kvRows*kvRowH + caveatH;
        const double cardH = cardPadTop + contentH + cardPadBot;
        drawCard(p, QRectF(cx, y, cw, cardH), /*joinTop*/true, s);

        const double ix = cx + padX, iw = cw - 2*padX;
        double iy = y + cardPadTop;
        drawOximeterSubheader(p, QRectF(ix, iy, iw, subH), QObject::tr("Sleep architecture"), s);
        iy += subH + 8*s;
        drawSleepStageCards(p, QRectF(ix, iy, iw, stageCardsH), sl, s);
        iy += stageCardsH + 12*s;
        drawDistributionBar(p, QRectF(ix, iy, iw, barH),
                            { { sl.deepPct,  AV::SLEEP_DEEP  },
                              { sl.lightPct, AV::SLEEP_LIGHT },
                              { sl.remPct,   AV::SLEEP_REM   },
                              { sl.awakePct, AV::SLEEP_AWAKE } }, s);
        iy += barH + 14*s;

        const QVector<QPair<QString,QString>> kvs = {
            { QObject::tr("Total sleep time"), hmm(sl.tstMin / 60.0) },
            { QObject::tr("Avg bedtime"),      includeBedtime ? (sl.avgBedtime.isEmpty() ? QStringLiteral("—") : sl.avgBedtime) : QStringLiteral("—") },
            { QObject::tr("Sleep latency"),    QString::number(qRound(sl.sleepLatencyMin)) + QObject::tr(" min") },
            { QObject::tr("Avg wake"),         includeBedtime ? (sl.avgWake.isEmpty() ? QStringLiteral("—") : sl.avgWake) : QStringLiteral("—") },
            { QObject::tr("REM of TST"),       QString::number(qRound(sl.remOfTstPct)) + QStringLiteral("%") },
            { QObject::tr("Sleep-stage nights"), QString::number(sl.sleepStageNights) },
        };
        const double colGap = 18*s, colW = (iw - 2*colGap) / 3.0;
        for (int i = 0; i < kvs.size(); i++) {
            const int col = i % 3, row = i / 3;
            const double kx = ix + col * (colW + colGap);
            const double ky = iy + row * kvRowH;
            drawKVRow(p, QRectF(kx, ky, colW, kvRowH), kvs[i].first, kvs[i].second, s);
        }
        // Validity caveat: consumer sleep staging is not PSG-scored.
        p.setFont(avSans(8.5*s, QFont::Normal));
        p.setPen(AV::GREY3);
        p.drawText(QRectF(ix, iy + 2*kvRowH + 5*s, iw, 12*s),
                   Qt::AlignLeft | Qt::AlignTop,
                   QObject::tr("Sleep stages are consumer-device estimates, not PSG-validated — interpret as trends only."));
        y += cardH + 18*s;
    }

    // (d) Oximeter Statistics — only when oximetry data exists. Two columns:
    //     "Usage & saturation" (6 rows) | "Pulse & desaturation" (4 rows).
    if (showOxi) {
        const double cardPadTop = 10*s, cardPadBot = 10*s;
        const auto spo2  = [&](const QString& v){ return (rd.hasSpO2 && rd.usedDaysOxi > 0) ? v : QString::fromUtf8("—"); };
        const auto pulse = [&](const QString& v){ return rd.usedDaysPulse > 0 ? v : QString::fromUtf8("—"); };
        const QVector<QPair<QString,QString>> usage = {
            { QObject::tr("Total / used nights"),
              QStringLiteral("%1 / %2").arg(rd.totalDays).arg(rd.usedDaysOxiAny) },
            { QObject::tr("Nights not used"),      QString::number(rd.oxiNotUsedDays) },
            { QObject::tr("Average hours / night"), hmm(rd.oxiRecordingHrsAvg) },
            { QObject::tr("Average SpO₂"),         spo2(QString::number(rd.avgSpO2Period, 'f', 1) + QStringLiteral("%")) },
            { QObject::tr("Minimum SpO₂"),
              rd.minSpO2Set ? QString::number(qRound(rd.minSpO2)) + QStringLiteral("%") : QStringLiteral("—") },
            { QObject::tr("Desaturation drop index"), spo2(QString::number(rd.odi, 'f', 1) + QObject::tr(" /h")) },
        };
        const QVector<QPair<QString,QString>> pulseRows = {
            { QObject::tr("Average pulse rate"), pulse(QString::number(qRound(rd.avgPulsePeriod)) + QObject::tr(" bpm")) },
            { QObject::tr("Min / max pulse"),
              pulse(QObject::tr("%1 / %2 bpm").arg(qRound(rd.minPulsePeriod)).arg(qRound(rd.maxPulsePeriod))) },
            { QObject::tr("Pulse change index"), pulse(QString::number(rd.pulseChangeIndex, 'f', 1) + QObject::tr(" /h")) },
            { QObject::tr("% time in desat"),    spo2(QString::number(rd.pctTimeInDesat, 'f', 1) + QStringLiteral("%")) },
        };
        const int maxRows = qMax(usage.size(), pulseRows.size());
        const double contentH = subH + 6*s + maxRows * kvRowH;
        const double cardH = cardPadTop + contentH + cardPadBot;
        drawCard(p, QRectF(cx, y, cw, cardH), /*joinTop*/!showSleep, s);

        const double colGap = 28*s, colW = (cw - 2*padX - colGap) / 2.0;
        const double leftX = cx + padX, rightX = leftX + colW + colGap;
        const double topY = y + cardPadTop;

        drawOximeterSubheader(p, QRectF(leftX, topY, colW, subH),
                              QObject::tr("Usage & saturation"), s);
        drawOximeterSubheader(p, QRectF(rightX, topY, colW, subH),
                              QObject::tr("Pulse & desaturation"), s);
        double ly = topY + subH + 6*s;
        for (const auto& kv : usage) {
            drawKVRow(p, QRectF(leftX, ly, colW, kvRowH), kv.first, kv.second, s);
            ly += kvRowH;
        }
        double ry = topY + subH + 6*s;
        for (const auto& kv : pulseRows) {
            drawKVRow(p, QRectF(rightX, ry, colW, kvRowH), kv.first, kv.second, s);
            ry += kvRowH;
        }
    }

    // (e) Footer
    const double footH = 40*s;
    drawFooter(p, QRectF(page.left(), page.bottom() - footH, page.width(), footH),
               pageNo, pageCount, QString(), s);
    return true;
}

// ============================================================================
// Page 7 — Diagnostics & History. Detailed per-channel statistics, additional
// therapy indices, a compact glossary, and the device-setting change history
// (with outcomes). Footer carries the report's methodology paragraph.
// ============================================================================
static void avPage7(QPainter& p, const QRectF& page, const ReportData& rd,
                    const QList<SelectedChannel>& channels, int pageNo, int pageCount, double s)
{
    const QRectF content = avContentRect(page, s);
    const double cx = content.left(), cw = content.width();
    double y = page.top();

    // (a) Compact masthead
    const double mastH = 48*s;
    drawMasthead(p, QRectF(page.left(), y, page.width(), mastH), /*large*/false, rd,
                 QObject::tr("Diagnostics & History"), s);
    y += mastH + 14*s;

    const double bandH = 30*s;
    const double headerH = AV_STAT_HEADER_H*s, statRowH = AV_STAT_ROW_H*s;
    const double footH = 80*s;   // methodology footer (used by the history cap too)

    // (b) Detailed Channel Statistics ----------------------------------------
    // True iff the channel recorded any data in the supplied day map.
    auto channelHasAnyData = [&](ChannelID cid, const QMap<QDate, Day*>& map) -> bool {
        for (auto it = map.constBegin(); it != map.constEnd(); ++it)
            if (it.value() && it.value()->channelHasData(cid)) return true;
        return false;
    };
    // Respect the dialog's channel checklist, like Classic's detailed table.
    auto selectedHas = [&](ChannelID cid) -> bool {
        for (const auto& ch : channels)
            if ((ChannelID)ch.id == cid) return true;
        return false;
    };

    const ChannelID statChans[] = {
        CPAP_Pressure, CPAP_MinuteVent, CPAP_RespRate, rd.leakChan, CPAP_Snore,
        CPAP_Ti, CPAP_Te, CPAP_TidalVolume, OXI_Pulse, OXI_SPO2
    };

    QVector<QStringList> statRows;
    for (ChannelID cid : statChans) {
        if (cid == NoChannel || !p_profile->hasChannel(cid)) continue;
        if (!selectedHas(cid)) continue;
        const bool isOxi = (cid == OXI_SPO2 || cid == OXI_Pulse);
        const QMap<QDate, Day*>& map = isOxi ? rd.oxiDaysMap : rd.daysMap;
        if (!channelHasAnyData(cid, map)) continue;
        const MachineType mt = isOxi ? MT_OXIMETER : MT_CPAP;

        // Min mirrors Classic's per-channel "Period Low": oximetry channels use the
        // dropout-filtered period lows, all others p_profile->calcMin over the range.
        double cMin;
        if      (cid == OXI_SPO2)   cMin = rd.minSpO2;
        else if (cid == OXI_Pulse)  cMin = rd.minPulsePeriod;
        else                        cMin = p_profile->calcMin(cid, MT_CPAP, rd.start, rd.end);
        const double cMed = calcDayAvgPercentile(cid, 0.5, map, mt);
        const double c95  = calcDayAvgPercentile(cid, avUpperPct(), map, mt);
        const double cMax = calcDayAvgMax(cid, map, mt);

        statRows.append(QStringList{
            schema::channel[cid].label(),
            unitForChannel(cid),
            QString::number(cMin, 'f', 1),
            QString::number(cMed, 'f', 1),
            QString::number(c95,  'f', 1),
            QString::number(cMax, 'f', 1),
        });
    }

    // Section drawn only when at least one selected channel has data — an
    // empty checklist must not print a header-only table.
    if (!statRows.isEmpty()) {
        drawBand(p, QRectF(cx, y, cw, bandH), QObject::tr("Detailed Channel Statistics"), QString(), s);
        y += bandH;
        const double statTableH = headerH + statRows.size() * statRowH;
        drawStatTable(p, QRectF(cx, y, cw, statTableH),
                      QStringList{ QObject::tr("Channel"), QObject::tr("Unit"), QObject::tr("Min"),
                                   QObject::tr("Median"), avUpperPctLabel(), QObject::tr("Max") },
                      statRows, s);
        y += statTableH + 16*s;
    }

    // (c) Additional Indices --------------------------------------------------
    drawBand(p, QRectF(cx, y, cw, bandH), QObject::tr("Additional Indices"), QString(), s);
    y += bandH;

    const int ttm = qRound(rd.totalHours * 60.0);
    const QString totalHoursStr = QStringLiteral("%1 h %2 m").arg(ttm / 60).arg(ttm % 60);
    QVector<QPair<QString,QString>> indices = {
        { QObject::tr("Cumulative time in apnea"), rd.timeInApneaStr.isEmpty() ? QStringLiteral("—") : rd.timeInApneaStr },
    };
    // CSR / flow-limitation rows only for devices that record those channels
    // (mirrors Classic and page 2's own gates) — never print a measured-looking
    // zero for a channel the device doesn't have.
    if (p_profile->hasChannel(CPAP_CSR))
        indices.append({ QObject::tr("Time in Cheyne–Stokes"),
                         rd.timeInCsrStr.isEmpty() ? QStringLiteral("—") : rd.timeInCsrStr });
    // Redline row only where a seal redline is meaningful (mirrors Classic's
    // total-leak suppression); label follows the configurable redline value.
    if (rd.leakChan != NoChannel && !rd.isTotalLeak && rd.leakRedline > 0)
        indices.append({ QObject::tr("Over leak redline (%1)").arg(qRound(rd.leakRedline)),
          rd.timeAboveRedlineStr + QStringLiteral(" · ") + QString::number(rd.pctOverRedline, 'f', 1) + QStringLiteral("%") });
    indices.append({ QObject::tr("Total therapy hours"),      totalHoursStr });
    indices.append({ QObject::tr("Minimum SpO₂"),
          rd.minSpO2Set ? QString::number(qRound(rd.minSpO2)) + QStringLiteral("%") : QStringLiteral("—") });
    // t90Str is always formatted ("00:00:00" with no data), so gate on actual
    // SpO₂ nights — a dash, matching the Minimum SpO₂ row above.
    indices.append({ QObject::tr("Time SpO₂ < 90%"),
          rd.usedDaysOxi > 0 && !rd.t90Str.isEmpty() ? rd.t90Str : QStringLiteral("—") });
    if (p_profile->hasChannel(CPAP_FLG))
        indices.append({ QObject::tr("Flow limitation (%1)").arg(avUpperPctLabel()),
                         QString::number(rd.p95FlowLimit, 'f', 2) });
    const double idxRowH = 22*s, idxColGap = 28*s, idxColW = (cw - idxColGap) / 2.0;
    const int idxRowsPerCol = (indices.size() + 1) / 2;   // left column gets the extra row
    for (int i = 0; i < indices.size(); i++) {
        const int col = i / idxRowsPerCol, row = i % idxRowsPerCol;
        const double kx = cx + col * (idxColW + idxColGap);
        const double ky = y + row * idxRowH;
        drawKVRow(p, QRectF(kx, ky, idxColW, idxRowH), indices[i].first, indices[i].second, s);
    }
    y += idxRowsPerCol * idxRowH + 16*s;

    // (d) Glossary ------------------------------------------------------------
    drawBand(p, QRectF(cx, y, cw, bandH), QObject::tr("Glossary"), QString(), s);
    y += bandH + 8*s;

    const QString glossary = QObject::tr(
        "AHI — Apnea–Hypopnea Index (events/h).  "
        "CAI — Clear-Airway Apnea Index (device-scored open-airway apneas; a proxy "
        "for, not equivalent to, PSG-scored central apnea).  HI — Hypopnea Index.  "
        "OAI — Obstructive Apnea Index.  "
        "RERA — Respiratory Effort–Related Arousal.  RDI — Respiratory Disturbance Index "
        "(AHI + RERA).  ODI — Oxygen Desaturation Index (≥%1% dips/h).  "
        "T90 — time with SpO₂ below 90%.  EPR — Expiratory Pressure Relief.  "
        "CSR — Cheyne–Stokes Respiration.").arg(qRound(rd.dropPct));
    // Footnote-sized: the glossary is reference material, not findings.
    const double glossH = 44*s;
    p.setFont(avSans(8.5*s, QFont::Normal));
    p.setPen(AV::CAPTION);
    p.drawText(QRectF(cx, y, cw, glossH), Qt::TextWordWrap | Qt::AlignTop | Qt::AlignLeft, glossary);
    y += glossH + 16*s;

    // (e) Therapy History — setting changes & outcomes ------------------------
    drawBand(p, QRectF(cx, y, cw, bandH),
             QObject::tr("Therapy History — Setting Changes & Outcomes"), QString(), s);
    y += bandH;

    // Columns chosen for a reviewing physician: when/how-long the settings ran,
    // what they were (mode + set pressure), whether the patient actually used the
    // device (avg use · % nights ≥ threshold), and how they responded
    // (residual AHI, central AI, leak, oxygenation).
    const QStringList histHeaders{
        QObject::tr("Period"), QObject::tr("Days"), QObject::tr("Mode"),
        QObject::tr("Pressure"), QObject::tr("Usage"), QObject::tr("AHI"),
        QObject::tr("CAI"), QObject::tr("%1 Leak").arg(avUpperPctLabel()), QObject::tr("Min SpO₂") };
    const QVector<double> histWeights{ 2.3, 0.7, 1.0, 1.4, 1.5, 0.7, 0.7, 1.0, 1.0 };

    QVector<QStringList> histRows;
    for (const SettingsChangeBlock& block : rd.settingsBlocks) {
        const QString period = block.start.toString("MMM d") + QStringLiteral(" – ")
                             + block.end.toString("MMM d, yyyy");
        const double blockAhi = block.hours > 0.0 ? block.ahiEvents / block.hours : 0.0;
        const int pctCompliant = block.days > 0
            ? qRound(100.0 * block.compliantNights / block.days) : 0;
        const QString pressure = !block.pressureShort.isEmpty() ? block.pressureShort : block.pressure;
        const QString usage = QString("%1 h \xc2\xb7 %2%")
            .arg(block.avgUseHrs, 0, 'f', 1).arg(pctCompliant);
        histRows.append(QStringList{
            period,
            QString::number(block.days),
            block.mode.isEmpty() ? QStringLiteral("—") : block.mode,
            pressure.isEmpty() ? QStringLiteral("—") : pressure,
            usage,
            QString::number(blockAhi, 'f', 1),
            QString::number(block.cai, 'f', 2),
            QString::number(block.p95Leak, 'f', 1),
            block.minSpo2 > 0.0 ? QString::number(qRound(block.minSpo2)) : QStringLiteral("—"),
        });
    }
    // Fixed-layout page: cap the table to the rows that fit above the
    // methodology footer, keeping the MOST RECENT setting periods (the list is
    // chronological). A truncation note replaces silent overflow.
    const double noteH = 14*s;
    int omitted = 0;
    {
        int fitRows = (int)qFloor((page.bottom() - footH - 6*s - y - headerH) / statRowH);
        if (fitRows < histRows.size()) {
            fitRows = qMax(0, (int)qFloor((page.bottom() - footH - 6*s - noteH - y - headerH) / statRowH));
            omitted = histRows.size() - fitRows;
            histRows = histRows.mid(omitted);
        }
    }
    const double histTableH = headerH + histRows.size() * statRowH;
    drawStatTable(p, QRectF(cx, y, cw, histTableH), histHeaders, histRows, s, histWeights);
    if (omitted > 0) {
        p.setFont(avSans(8.5*s, QFont::Normal));
        p.setPen(AV::GREY3);
        p.drawText(QRectF(cx, y + histTableH + 2*s, cw, noteH), Qt::AlignLeft | Qt::AlignTop,
                   QObject::tr("%1 earlier setting period(s) omitted — showing the most recent.").arg(omitted));
    }

    // (f) Footer with methodology paragraph (the page-7 variant).
    const QString methodology = QObject::tr(
        "Indices are device-estimated and do not constitute a diagnostic "
        "polysomnogram. Percentile statistics are computed per night and then "
        "averaged across the reporting period; the upper percentile shown (%1) "
        "follows OSCAR's calculation preference. Channel maxima are likewise "
        "per-night averages (99.5th percentile when the max-calculation "
        "preference is enabled). Channel minimums are period lows with "
        "sensor dropout excluded. Setting-change blocks group consecutive nights "
        "sharing the same device, mode, pressure and relief settings.")
        .arg(avUpperPctLabel());
    drawFooter(p, QRectF(page.left(), page.bottom() - footH, page.width(), footH),
               pageNo, pageCount, methodology, s);
}

} // anonymous namespace

// ============================================================================
// Renders the AirView V2 PDF (all pages; 4/5/6 are gated on selection/data).
// ============================================================================
void renderAirViewV2Pdf(const ReportData& rd, QPrinter& printer, const QList<SelectedChannel>& channels)
{
    // AirView V2 owns the printer; full-bleed margins (the design draws its own padding).
    printer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
    QPainter p(&printer);
    QRectF page = printer.pageLayout().paintRectPixels(printer.resolution());
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // Mockup artboard is 794px wide (A4@96dpi) → layout scale.
    const double s = page.width() / 794.0;

    // Dialog channel selection: trend-chart pages honour the checklist exactly
    // like Classic's channel charts do (summary/statistics sections stay
    // data-gated, also matching Classic).
    auto selHas = [&](ChannelID cid) -> bool {
        for (const auto& ch : channels)
            if ((ChannelID)ch.id == cid) return true;
        return false;
    };
    const bool selSpo2 = selHas(OXI_SPO2), selPulse = selHas(OXI_Pulse);

    bool includeBedtime = false;
    bool includeSleepStages = false;
    bool includeOxiStats = false;
    for (const auto& ch : channels) {
        if ((ChannelID)ch.id == COMPLIANCE_Bedtime) includeBedtime = true;
        else if ((ChannelID)ch.id == COMPLIANCE_SleepStages) includeSleepStages = true;
        else if ((ChannelID)ch.id == COMPLIANCE_OximeterStats) includeOxiStats = true;
    }

    // Assemble only the pages that will actually be emitted, then number them
    // n / N over the emitted count so gated pages (4/5/6) never leave numbering
    // gaps and footers always show the correct total.
    const bool hasP4 = (p_profile->hasChannel(CPAP_MinuteVent)  && selHas(CPAP_MinuteVent))
                    || (p_profile->hasChannel(CPAP_RespRate)    && selHas(CPAP_RespRate))
                    || (p_profile->hasChannel(CPAP_TidalVolume) && selHas(CPAP_TidalVolume));
    const bool hasP5 = rd.usedDaysOxiAny > 0 && includeOxiStats;
    const bool hasP6 = (rd.usedDaysOxiAny > 0 && includeOxiStats) || (rd.sleep.present && includeSleepStages);
    QVector<std::function<void(int,int)>> pages;
    pages.append([&](int n, int N){ avPage1(p, page, rd, n, N, s); });
    pages.append([&](int n, int N){ avPage2(p, page, rd, n, N, s); });
    pages.append([&](int n, int N){ avPage3(p, page, rd, n, N, s); });
    if (hasP4) pages.append([&](int n, int N){ avPage4(p, page, rd, channels, n, N, s); });
    if (hasP5) pages.append([&](int n, int N){ avPage5(p, page, rd, selSpo2, selPulse, n, N, s); });
    if (hasP6) pages.append([&](int n, int N){ avPage6(p, page, rd, channels, n, N, s); });
    pages.append([&](int n, int N){ avPage7(p, page, rd, channels, n, N, s); });

    const int N = pages.size();
    for (int i = 0; i < N; ++i) {
        if (i) printer.newPage();
        pages[i](i + 1, N);
    }

    p.end();
}
