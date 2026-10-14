/* Import hand-over Unit Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "importhandovertests.h"
#include "importhandover.h"

#include <atomic>
#include <memory>
#include <thread>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>

using namespace ImportHandover;

namespace
{

// send() blocks, as it does in a second OSCAR process; run it beside the test's event loop.
struct Requester
{
    std::thread thread;
    std::atomic<bool> finished { false };
    SendResult result = SendResult::NoInstanceListening;
    Reply reply;

    Requester(const QString &dataDir, const Request &request, int acceptTimeoutMs = 10000)
    {
        thread = std::thread([this, dataDir, request, acceptTimeoutMs]() {
            result = send(dataDir, request, reply, 2000, acceptTimeoutMs);
            finished = true;
        });
    }
    ~Requester() { if (thread.joinable()) thread.join(); }

    // Runs the test's event loop until the answer is there. A real event loop, as in the
    // application: the server serves requests when that loop is idle.
    bool wait(int ms = 10000)
    {
        QEventLoop loop;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &loop, [this, &loop]() { if (finished) loop.quit(); });
        poll.start(5);
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
        if (finished) {
            thread.join();
        }
        return finished;
    }
};

Request oneImport(const QString &loader, const QString &path, const QString &profile = QString())
{
    Request request;
    request.profile = profile;
    request.tasks.append(qMakePair(loader, path));
    return request;
}

Reply ok(const QString &message = QString())
{
    Reply reply;
    reply.exitCode = ExitOk;
    reply.message = message;
    return reply;
}

// What a loader does while it imports: process events now and then, without waiting for any.
void spin(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents();
    }
}

// The application sitting in its event loop with nothing to do.
void idle(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// idle() until the condition holds.
template <typename Condition> bool idleUntil(Condition condition, int ms = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < ms) {
        idle(10);
    }
    return condition();
}

// A requester the test controls step by step, in the test's own thread.
bool connectTo(QLocalSocket &socket, const QString &dataDir)
{
    socket.connectToServer(endpoint(dataDir));
    return socket.waitForConnected(1000);
}

} // namespace

void ImportHandoverTests::initTestCase()
{
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char name[] = "importhandovertests";
        static char *argv[] = { name, nullptr };
        m_app = new QCoreApplication(argc, argv);
    }
}

void ImportHandoverTests::cleanupTestCase()
{
    delete m_app;
    m_app = nullptr;
}

void ImportHandoverTests::testRequestRoundTrip()
{
    Request request;
    request.profile = QString::fromUtf8("J\xc3\xbcrgen \"J\" O'Neil");
    request.tasks.append(qMakePair(QString("Viatom"), QString("/home/user/O2 Ring/data")));
    request.tasks.append(qMakePair(QString("CPAP"), QString::fromUtf8("C:\\Users\\Ren\xc3\xa9\\SD card")));

    const QByteArray line = encodeRequest(request);
    QVERIFY(line.endsWith('\n'));
    QCOMPARE(line.count('\n'), 1);     // one line, whatever the names contain

    Request decoded;
    QString error;
    QVERIFY2(decodeRequest(line, decoded, &error), qPrintable(error));
    QCOMPARE(decoded.profile, request.profile);
    QCOMPARE(decoded.tasks, request.tasks);   // same imports, same order
}

void ImportHandoverTests::testReplyRoundTrip()
{
    Reply reply;
    reply.exitCode = ExitBusy;
    reply.message = QString::fromUtf8("busy\nsecond line \xe2\x9c\x93");

    const QByteArray line = encodeReply(reply);
    QCOMPARE(line.count('\n'), 1);

    Reply decoded;
    QVERIFY(decodeReply(line, decoded));
    QCOMPARE(decoded.exitCode, int(ExitBusy));
    QCOMPARE(decoded.message, reply.message);

    QVERIFY(!decodeReply("{\"exitCode\":0}\n", decoded));                    // not marked as a reply
    QVERIFY(!decodeReply("{\"oscarImportReply\":1,\"message\":\"x\"}\n", decoded)); // no exit code
    QVERIFY(!decodeReply("{\"oscarImportAccepted\":1}\n", decoded));         // the confirmation is not a reply
    QVERIFY(!decodeReply("nonsense\n", decoded));
}

void ImportHandoverTests::testMalformedRequestsAreRejected()
{
    Request request;
    QString error;
    QVERIFY(!decodeRequest("", request, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!decodeRequest("GET / HTTP/1.1\n", request));
    QVERIFY(!decodeRequest("[1,2,3]\n", request));
    // a later, incompatible version of the request
    QVERIFY(!decodeRequest("{\"oscarImportRequest\":2,\"imports\":[{\"loader\":\"CPAP\",\"path\":\"/x\"}]}\n", request));
    // nothing to import
    QVERIFY(!decodeRequest("{\"oscarImportRequest\":1,\"imports\":[]}\n", request));
    // an import without a path
    QVERIFY(!decodeRequest("{\"oscarImportRequest\":1,\"imports\":[{\"loader\":\"CPAP\"}]}\n", request));
    // a rejected request leaves the output alone
    QVERIFY(request.tasks.isEmpty());

    // unknown fields are ignored, the profile is optional
    QVERIFY(decodeRequest("{\"oscarImportRequest\":1,\"later\":true,\"imports\":[{\"loader\":\"CPAP\",\"path\":\"/x\"}]}\n", request));
    QCOMPARE(request.tasks.size(), 1);
    QVERIFY(request.profile.isEmpty());
}

void ImportHandoverTests::testEndpointFollowsTheFolder()
{
    QTemporaryDir a, b;
    QVERIFY(a.isValid() && b.isValid());

    const QString place = endpoint(a.path());
    QCOMPARE(endpoint(a.path() + "/"), place);               // however the folder is spelled
    QCOMPARE(endpoint(a.path() + "/./"), place);
    QCOMPARE(endpoint(b.path() + "/../" + QDir(a.path()).dirName()), place);
    QVERIFY(endpoint(b.path()) != place);                    // another folder, another instance
#ifdef Q_OS_WIN
    QVERIFY(place.startsWith("OSCAR-import-"));              // a pipe name
#else
    QVERIFY(place.startsWith(QDir(a.path()).canonicalPath() + "/"));    // a socket inside the data folder
#endif
}

void ImportHandoverTests::testLongFolderPathFallsBackToAName()
{
    // A Unix socket address has room for about 100 bytes of path.
    QTemporaryDir dir;
    const QString deep = dir.path() + "/" + QString(40, 'a') + "/" + QString(40, 'b') + "/" + QString(40, 'c');
    QVERIFY(QDir().mkpath(deep));
    QVERIFY(endpoint(deep).startsWith("OSCAR-import-"));
    QVERIFY(endpoint(deep) != endpoint(dir.path() + "/" + QString(40, 'a')));

    Server server;
    QVERIFY2(server.listen(deep, [](const Request &) { return ok("deep"); }), qPrintable(server.errorString()));
    Requester requester(deep, oneImport("CPAP", "/x"));
    QVERIFY(requester.wait());
    QCOMPARE(requester.result, SendResult::Replied);
    QCOMPARE(requester.reply.message, QString("deep"));
}

void ImportHandoverTests::testNobodyListening()
{
    QTemporaryDir dir;
    Reply reply;
    QCOMPARE(send(dir.path(), oneImport("CPAP", "/x"), reply, 200), SendResult::NoInstanceListening);
}

void ImportHandoverTests::testRequestIsServedAndAnswered()
{
    QTemporaryDir dir;
    Request seen;
    Server server;
    QVERIFY2(server.listen(dir.path(), [&seen](const Request &request) {
        seen = request;
        return ok("done");
    }), qPrintable(server.errorString()));
#ifndef Q_OS_WIN
    QCOMPARE(server.listeningOn(), endpoint(dir.path()));         // the socket is in the data folder
    QVERIFY(QFile::exists(server.listeningOn()));
#endif

    Request request = oneImport("Viatom", "/data/ring", "hofer");
    request.tasks.append(qMakePair(QString("ZEO"), QString("/data/zeo.csv")));
    Requester requester(dir.path(), request);
    QVERIFY(requester.wait());

    QCOMPARE(requester.result, SendResult::Replied);
    QCOMPARE(requester.reply.exitCode, int(ExitOk));
    QCOMPARE(requester.reply.message, QString("done"));
    QCOMPARE(seen.profile, QString("hofer"));
    QCOMPARE(seen.tasks, request.tasks);

#ifndef Q_OS_WIN
    const QString socketFile = server.listeningOn();
    server.close();
    QVERIFY(!QFile::exists(socketFile));                          // nothing left in the data folder
#endif
}

void ImportHandoverTests::testRequestsAreServedOneAtATime()
{
    // The handler stands for an import: it takes a while and processes events meanwhile,
    // which is when a second request arrives. That one has to wait, not run inside the first.
    QTemporaryDir dir;
    int running = 0, mostAtOnce = 0;
    QStringList order;
    std::unique_ptr<Requester> second;
    Server server;
    QVERIFY(server.listen(dir.path(), [&](const Request &request) {
        mostAtOnce = qMax(mostAtOnce, ++running);
        const QString name = request.tasks.first().first;
        order << name;
        if (name == "first") {
            // The second request is made while the first is being served, and has arrived
            // (its requester is told so) before the first one is finished.
            second.reset(new Requester(dir.path(), oneImport("second", "/x")));
            spin(400);
        }
        --running;
        return ok(name);
    }));

    Requester first(dir.path(), oneImport("first", "/x"));
    QVERIFY(first.wait());
    QVERIFY(second != nullptr);
    QVERIFY(second->wait());

    QCOMPARE(mostAtOnce, 1);
    QCOMPARE(order, QStringList({"first", "second"}));
    QCOMPARE(first.reply.message, QString("first"));     // each requester gets its own answer
    QCOMPARE(second->reply.message, QString("second"));
}

void ImportHandoverTests::testMalformedRequestGetsAnAnswer()
{
    QTemporaryDir dir;
    bool handlerCalled = false;
    Server server;
    QVERIFY(server.listen(dir.path(), [&handlerCalled](const Request &) {
        handlerCalled = true;
        return Reply();
    }));

    QLocalSocket socket;
    QVERIFY(connectTo(socket, dir.path()));
    socket.write("{\"oscarImportRequest\":99}\n");
    QByteArray line;
    QVERIFY(idleUntil([&]() { return (line += socket.readAll()).contains('\n'); }));

    Reply reply;
    QVERIFY(decodeReply(line, reply));
    QCOMPARE(reply.exitCode, int(ExitFailed));
    QVERIFY(!handlerCalled);
}

void ImportHandoverTests::testRequesterGoneWhileBeingServed()
{
    // "OSCAR --import" is killed while the window imports for it. The import goes on; there
    // is just nobody to tell. (Run with the address sanitizer, this is the test for it.)
    QTemporaryDir dir;
    QLocalSocket requester;
    bool finished = false;
    Server server;
    QVERIFY(server.listen(dir.path(), [&](const Request &) {
        requester.abort();
        spin(200);          // the import carries on and processes events
        finished = true;
        return ok();
    }));

    QVERIFY(connectTo(requester, dir.path()));
    requester.write(encodeRequest(oneImport("CPAP", "/x")));
    QVERIFY(idleUntil([&]() { return finished; }));
    idle(100);              // whatever was scheduled for the socket happens now

    // ...and the server still works.
    Requester next(dir.path(), oneImport("CPAP", "/x"));
    QVERIFY(next.wait());
    QCOMPARE(next.result, SendResult::Replied);
}

void ImportHandoverTests::testRequesterGoneWhileWaitingForItsTurn()
{
    QTemporaryDir dir;
    QLocalSocket impatient;
    QStringList served;
    Server server;
    QVERIFY(server.listen(dir.path(), [&](const Request &request) {
        served << request.tasks.first().first;
        if (served.size() == 1) {
            // A second request arrives during the first import and is given up again.
            if (connectTo(impatient, dir.path())) {
                impatient.write(encodeRequest(oneImport("impatient", "/x")));
                spin(150);
                impatient.abort();
            }
            spin(150);
        }
        return ok();
    }));

    Requester first(dir.path(), oneImport("first", "/x"));
    QVERIFY(first.wait());
    idle(200);
    QCOMPARE(served, QStringList({"first"}));    // nothing is imported for someone who left
}

void ImportHandoverTests::testInstanceNotResponding()
{
    // Connecting works although nobody reads: an instance that is frozen. The requester
    // must come back.
    QTemporaryDir dir;
    QLocalServer frozen;
    QVERIFY(frozen.listen(endpoint(dir.path())));

    Requester requester(dir.path(), oneImport("CPAP", "/x"), 300);
    QVERIFY(requester.wait(5000));
    QCOMPARE(requester.result, SendResult::NotResponding);
}

void ImportHandoverTests::testInstanceGoneBeforeAnswering()
{
    // An instance that crashes or is closed during the import, after it took the request:
    // the requester must not hang.
    QTemporaryDir dir;
    QLocalServer raw;
    QVERIFY(raw.listen(endpoint(dir.path())));
    connect(&raw, &QLocalServer::newConnection, &raw, [&raw]() {
        QLocalSocket *socket = raw.nextPendingConnection();
        socket->waitForReadyRead(2000);
        socket->write("{\"oscarImportAccepted\":1}\n");
        socket->flush();
        socket->waitForBytesWritten(1000);
        socket->abort();
        socket->deleteLater();
    });

    Requester requester(dir.path(), oneImport("CPAP", "/x"));
    QVERIFY(requester.wait());
    QCOMPARE(requester.result, SendResult::ConnectionLost);
}

void ImportHandoverTests::testStaleSocketIsReplaced()
{
#ifdef Q_OS_WIN
    QSKIP("Named pipes leave nothing behind.");
#else
    // What a crashed instance leaves behind must not stop the next one from listening.
    QTemporaryDir dir;
    QFile leftover(endpoint(dir.path()));
    QVERIFY(leftover.open(QFile::WriteOnly));
    leftover.close();

    Server server;
    QVERIFY2(server.listen(dir.path(), [](const Request &) { return ok(); }), qPrintable(server.errorString()));
    QCOMPARE(server.listeningOn(), endpoint(dir.path()));

    Requester requester(dir.path(), oneImport("CPAP", "/x"));
    QVERIFY(requester.wait());
    QCOMPARE(requester.result, SendResult::Replied);
    QCOMPARE(requester.reply.exitCode, int(ExitOk));
#endif
}

void ImportHandoverTests::testCloseWithRequestsWaiting()
{
    // OSCAR is closed while an import is running for one requester and two more are queued.
    QTemporaryDir dir;
    std::unique_ptr<Requester> second, third;
    Server server;
    QVERIFY(server.listen(dir.path(), [&](const Request &) {
        second.reset(new Requester(dir.path(), oneImport("second", "/x")));
        third.reset(new Requester(dir.path(), oneImport("third", "/x")));
        // Both connect, are told "accepted" and wait. Leave them time enough for that even on
        // a slow or loaded machine; a requester that has not connected yet when the server
        // closes would get "nobody listening" instead.
        spin(2000);
        server.close();
        return ok("first");
    }));

    Requester first(dir.path(), oneImport("first", "/x"));
    QVERIFY(first.wait());
    QCOMPARE(first.result, SendResult::Replied);         // the one being served still gets its answer
    QVERIFY(second->wait());
    QVERIFY(third->wait());
    QCOMPARE(second->result, SendResult::ConnectionLost);
    QCOMPARE(third->result, SendResult::ConnectionLost);
}

void ImportHandoverTests::testWaitsUntilTheEventLoopIsIdle()
{
    // Some part of the application is busy and processes events while it works (as much of
    // OSCAR does). A request that arrives then must not be carried out inside that work.
    QTemporaryDir dir;
    bool working = false, servedWhileWorking = false, served = false;
    Server server;
    QVERIFY(server.listen(dir.path(), [&](const Request &) {
        servedWhileWorking = working;
        served = true;
        return ok();
    }));

    std::unique_ptr<Requester> requester;
    QTimer::singleShot(0, &server, [&]() {
        working = true;
        requester.reset(new Requester(dir.path(), oneImport("CPAP", "/x")));
        spin(500);          // the request arrives and is confirmed during this
        working = false;
    });
    QVERIFY(idleUntil([&]() { return requester != nullptr; }));
    QVERIFY(requester->wait());
    QCOMPARE(requester->result, SendResult::Replied);
    QVERIFY(served);
    QVERIFY(!servedWhileWorking);
}

void ImportHandoverTests::testBusyWhileANestedEventLoopWaits()
{
    // A dialog or a menu with its own event loop is open: the request is answered "busy"
    // at once and does not reach the handler.
    QTemporaryDir dir;
    bool handlerCalled = false;
    Server server;
    QVERIFY(server.listen(dir.path(), [&](const Request &) {
        handlerCalled = true;
        return ok();
    }));

    std::unique_ptr<Requester> requester;
    bool answeredWhileOpen = false;
    QTimer::singleShot(0, &server, [&]() {
        requester.reset(new Requester(dir.path(), oneImport("CPAP", "/x")));
        QEventLoop dialog;                          // what QDialog::exec() and QMenu::exec() run
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &dialog, [&]() {
            if (requester->finished) { answeredWhileOpen = true; dialog.quit(); }
        });
        poll.start(5);
        QTimer::singleShot(5000, &dialog, &QEventLoop::quit);
        dialog.exec();
    });
    QVERIFY(idleUntil([&]() { return requester != nullptr && requester->finished; }));
    QVERIFY(requester->wait());
    QVERIFY(answeredWhileOpen);
    QCOMPARE(requester->result, SendResult::Replied);
    QCOMPARE(requester->reply.exitCode, int(ExitBusy));
    QVERIFY(!handlerCalled);
}
