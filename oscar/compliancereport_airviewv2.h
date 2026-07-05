/* Compliance Report Exporter Source
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef COMPLIANCEREPORT_AIRVIEWV2_H
#define COMPLIANCEREPORT_AIRVIEWV2_H

#include <QList>
#include "compliancereport_internal.h"   // ReportData
#include "compliancereport.h"            // SelectedChannel

class QPrinter;

// Renders the full AirView V2 PDF (7 fixed-layout A4 pages) directly with QPainter.
// `printer` is an already-configured A4/portrait PdfFormat QPrinter with output file set.
void renderAirViewV2Pdf(const ReportData& rd, QPrinter& printer, const QList<SelectedChannel>& channels);

#endif // COMPLIANCEREPORT_AIRVIEWV2_H
