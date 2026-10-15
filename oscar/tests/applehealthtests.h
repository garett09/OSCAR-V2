/* Apple Health Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef APPLEHEALTHTESTS_H
#define APPLEHEALTHTESTS_H

#include "AutoTest.h"

class AppleHealthLoader;
class QCoreApplication;
class QTemporaryDir;

class AppleHealthTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void testParser();
    void testParserCutoff();
    void testVitalsSourcePolicy();
    void testLoaderImport();
    void testLoaderIdempotency();
    void testLoaderImportsWeight();
    void testLoaderImportsZip();
    void testLoaderSkipsShiftedNights();
    void testLoaderRejectsGarbage();
    void testLoaderImportsSingleNonWatchSource();
    void testLoaderSkipsNonAppleOximeterNight();
    void testLoaderKeepsChosenSleepSourceVitals();
    void testLoaderSummarizesMultipleFiles();
    void testLoaderCancelledChooserImportsNothing();
    void testSPO2DropSparsity();
    void cleanupTestCase();

private:
    QCoreApplication *m_app = nullptr;
    QTemporaryDir *m_tempDir = nullptr;
    QString m_exportPath;
    QString m_garbagePath;
    QString m_previousAppData;
};

DECLARE_TEST(AppleHealthTests)

#endif // APPLEHEALTHTESTS_H
