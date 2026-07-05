/* ExportCSV Module Header — Version 1 (1.7.x-style) CSV export dialog
 *
 * Copyright (c) 2019-2026 The OSCAR Team
 * Copyright (C) 2011-2018 Mark Watkins
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef EXPORTCSV_H
#define EXPORTCSV_H

#include <QDateEdit>
#include <QDialog>
#include "SleepLib/machine_common.h"

namespace Ui {
class ExportCSV;
}

/*! \brief Describes a single data field to be exported. */
struct DumpField {
    DumpField() { code = NoChannel; mtype = MT_UNKNOWN; type = ST_CNT; }
    DumpField(ChannelID c, MachineType mt, SummaryType t) { code = c; mtype = mt; type = t; }
    DumpField(const DumpField &copy) {code = copy.code; mtype = copy.mtype; type = copy.type; }
    ChannelID code;
    MachineType mtype;
    SummaryType type;
};


/*! \class ExportCSV
    \brief Version 1 (1.7.x-style) dialog for exporting SleepLib data in CSV format.

    Offers three export modes: per-day summary, per-session summary, and raw event
    details.  Reproduces the CSV layout produced by OSCAR 1.7.x.
    */
class ExportCSV : public QDialog
{
    Q_OBJECT

  public:
    explicit ExportCSV(QWidget *parent = nullptr);
    ~ExportCSV();

  private slots:
    void on_filenameBrowseButton_clicked();
    void on_quickRangeCombo_activated(int index);
    void on_exportButton_clicked();
    void startDate_currentPageChanged(int year, int month);
    void endDate_currentPageChanged(int year, int month);

  private:
    void UpdateCalendarDay(QDateEdit *dateedit, QDate date);

    Ui::ExportCSV *ui;
    QList<DumpField> fields;
};

#endif // EXPORTCSV_H
