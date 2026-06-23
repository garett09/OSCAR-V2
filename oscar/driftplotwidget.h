/* Drift Plot Widget Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef DRIFTPLOTWIDGET_H
#define DRIFTPLOTWIDGET_H

#include <QWidget>
#include <QDate>
#include <QList>

class DriftPlotWidget : public QWidget
{
    Q_OBJECT
public:
    struct Point { QDate date; double offsetMs; };

    explicit DriftPlotWidget(QWidget *parent = nullptr);

    void setData(const QList<Point>& points);
    void setModel(double c0Ms, double slope);          // red line — newly fitted model
    void clearModel();
    void setReferenceModel(double c0Ms, double slope); // gray dashed line — existing model
    void clearReferenceModel();
    void setRefPoints(const QList<Point>& pts);        // hollow dots — reference device
    void clearRefPoints();
    int  refPointCount() const { return m_refPoints.size(); }
    void setDateRange(const QDate& start, const QDate& end); // fallback X-axis bounds
    void clear();

    void setCorrectionDates(const QList<QDate>& dates);
    void clearCorrectionDates();
    int  correctionDateCount() const { return m_correctionDates.size(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QList<Point> m_points;
    bool   m_hasModel    = false;
    double m_c0Ms        = 0.0;
    double m_slope       = 0.0;   // = c1 - 1.0

    bool   m_hasRefModel = false;
    double m_refC0Ms     = 0.0;
    double m_refSlope    = 0.0;

    QList<Point> m_refPoints;

    QDate m_rangeStart;
    QDate m_rangeEnd;

    QList<QDate> m_correctionDates;

    static double tNoon(QDate d);
};

#endif // DRIFTPLOTWIDGET_H
