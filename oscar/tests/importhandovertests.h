/* Import hand-over Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef IMPORTHANDOVERTESTS_H
#define IMPORTHANDOVERTESTS_H

#include "tests/AutoTest.h"

class QCoreApplication;

class ImportHandoverTests : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase();
    void cleanupTestCase();
    void testRequestRoundTrip();
    void testReplyRoundTrip();
    void testMalformedRequestsAreRejected();
    void testEndpointFollowsTheFolder();
    void testLongFolderPathFallsBackToAName();
    void testFolderWithoutSocketsFallsBackToAName();
    void testNobodyListening();
    void testRequestIsServedAndAnswered();
    void testRequestsAreServedOneAtATime();
    void testMalformedRequestGetsAnAnswer();
    void testRequesterGoneWhileBeingServed();
    void testRequesterGoneWhileWaitingForItsTurn();
    void testInstanceNotResponding();
    void testInstanceGoneBeforeAnswering();
    void testStaleSocketIsReplaced();
    void testCloseWithRequestsWaiting();
    void testWaitsUntilTheEventLoopIsIdle();
    void testBusyWhileANestedEventLoopWaits();

  private:
    QCoreApplication *m_app = nullptr;   // sockets and timers need one; the test runner has none
};
DECLARE_TEST(ImportHandoverTests)

#endif // IMPORTHANDOVERTESTS_H
