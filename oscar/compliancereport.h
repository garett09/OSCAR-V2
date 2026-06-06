/* Compliance Report Exporter Header
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef COMPLIANCEREPORT_H
#define COMPLIANCEREPORT_H

#include <QDialog>
#include <QDate>
#include <QComboBox>
#include <QDateEdit>
#include <QLabel>
#include <QPushButton>

#include <QCoreApplication>
#include <QListWidget>
#include <QList>

struct SelectedChannel
{
    unsigned int id;
    QString label;
    int type; // MachineType
};

class ComplianceReportDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ComplianceReportDialog(QWidget *parent = nullptr);
    QDate startDate() const;
    QDate endDate() const;
    QDate lastSettingsChangeStartDate() const;
    QList<SelectedChannel> selectedChannels() const;

private slots:
    void onRangeChanged(int index);

private:
    QComboBox *m_rangeCombo;
    QDateEdit *m_startDateEdit;
    QDateEdit *m_endDateEdit;
    QLabel *m_startLabel;
    QLabel *m_endLabel;
    QListWidget *m_channelsListWidget;
};

class ComplianceReport
{
    Q_DECLARE_TR_FUNCTIONS(ComplianceReport)
public:
    static void exportReport(QWidget *parent);
};

#endif // COMPLIANCEREPORT_H
