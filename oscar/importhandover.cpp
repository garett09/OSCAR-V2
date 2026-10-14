/* Hand an import over to an OSCAR instance that is already running
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "importhandover.h"

#include <QAbstractEventDispatcher>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>

namespace ImportHandover
{

static const char *const RequestKey = "oscarImportRequest";
static const char *const AcceptedKey = "oscarImportAccepted";
static const char *const ReplyKey = "oscarImportReply";
static const int ProtocolVersion = 1;
// A client that connects has to send its request within this time.
static const int RequestTimeoutMs = 5000;
// The requester closes the connection when it has read the reply; one that does not is dropped.
static const int ReplyLingerMs = 10000;
// Set on a connection once its request line has been taken; whatever follows is ignored.
static const char *const TakenProperty = "importHandoverRequestTaken";
#ifndef Q_OS_WIN
// The name of the socket inside the data folder.
static const char *const SocketFileName = "oscar-import.sock";
// A Unix socket address holds 104 bytes of path on macOS and 108 on Linux, and QLocalServer
// first creates the socket in a temporary folder next to its final place, which is longer.
static const int MaxSocketPathLength = 90;
#endif

QString endpoint(const QString &dataDir)
{
    // Both sides must arrive at the same place for the same folder, whichever way it was
    // spelled on the command line.
    QString path = QDir(dataDir).canonicalPath();
    if (path.isEmpty()) {
        path = QDir::cleanPath(QDir(dataDir).absolutePath());
    }

#ifdef Q_OS_WIN
    path = path.toLower();
#else
    const QString socketFile = path + QLatin1Char('/') + QLatin1String(SocketFileName);
    if (QFile::encodeName(socketFile).size() <= MaxSocketPathLength) {
        return socketFile;
    }
#endif
    // A name in the system's place for local sockets: a named pipe on Windows, and on Unix
    // the way out for a data folder whose path is too long for a socket address. Unlike the
    // socket in the data folder, such a name could be taken by another user of the machine.
    const QByteArray hash = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStringLiteral("OSCAR-import-") + QString::fromLatin1(hash.left(16));
}

QByteArray encodeRequest(const Request &request)
{
    QJsonArray imports;
    for (const auto &task : request.tasks) {
        QJsonObject import;
        import["loader"] = task.first;
        import["path"] = task.second;
        imports.append(import);
    }
    QJsonObject obj;
    obj[RequestKey] = ProtocolVersion;
    obj["profile"] = request.profile;
    obj["imports"] = imports;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

bool decodeRequest(const QByteArray &line, Request &request, QString *error)
{
    auto fail = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return false;
    };

    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed());
    if (!doc.isObject()) {
        return fail(QStringLiteral("not a JSON object"));
    }
    const QJsonObject obj = doc.object();
    if (obj.value(RequestKey).toInt() != ProtocolVersion) {
        return fail(QStringLiteral("not an import request of version %1").arg(ProtocolVersion));
    }
    Request parsed;
    parsed.profile = obj.value("profile").toString();
    const QJsonArray imports = obj.value("imports").toArray();
    for (const QJsonValue &value : imports) {
        const QJsonObject import = value.toObject();
        const QString loader = import.value("loader").toString();
        const QString path = import.value("path").toString();
        if (loader.isEmpty() || path.isEmpty()) {
            return fail(QStringLiteral("an import without loader or path"));
        }
        parsed.tasks.append(qMakePair(loader, path));
    }
    if (parsed.tasks.isEmpty()) {
        return fail(QStringLiteral("no imports requested"));
    }
    request = parsed;
    return true;
}

QByteArray encodeReply(const Reply &reply)
{
    QJsonObject obj;
    obj[ReplyKey] = ProtocolVersion;
    obj["exitCode"] = reply.exitCode;
    obj["message"] = reply.message;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

bool decodeReply(const QByteArray &line, Reply &reply)
{
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed());
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject obj = doc.object();
    if (obj.value(ReplyKey).toInt() != ProtocolVersion || !obj.value("exitCode").isDouble()) {
        return false;
    }
    reply.exitCode = obj.value("exitCode").toInt();
    reply.message = obj.value("message").toString();
    return true;
}

static bool isAccepted(const QByteArray &line)
{
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed());
    return doc.isObject() && doc.object().value(AcceptedKey).toInt() == ProtocolVersion;
}

/* Reads one line from the socket. timeoutMs < 0 waits without limit. Returns false when the
   time is up or the connection closed before a whole line arrived; closed tells which. */
static bool readLine(QLocalSocket &socket, QByteArray &line, int timeoutMs, bool &closed)
{
    closed = false;
    QElapsedTimer waited;
    waited.start();
    while (!socket.canReadLine()) {
        if (socket.bytesAvailable() > MaxLineLength) {
            closed = true;
            return false;
        }
        int remaining = -1;
        if (timeoutMs >= 0) {
            remaining = timeoutMs - int(waited.elapsed());
            if (remaining <= 0) {
                return false;
            }
        }
        if (!socket.waitForReadyRead(remaining) && !socket.canReadLine()) {
            // Either the time is up or the other side is gone.
            if (socket.state() != QLocalSocket::ConnectedState) {
                closed = true;
                return false;
            }
            if (timeoutMs >= 0 && waited.elapsed() >= timeoutMs) {
                return false;
            }
        }
    }
    line = socket.readLine(MaxLineLength + 1);
    return true;
}

SendResult send(const QString &dataDir, const Request &request, Reply &reply, int connectTimeoutMs, int acceptTimeoutMs)
{
    QLocalSocket socket;
    socket.connectToServer(endpoint(dataDir));
    if (!socket.waitForConnected(connectTimeoutMs)) {
        return SendResult::NoInstanceListening;
    }

    socket.write(encodeRequest(request));
    socket.flush();

    // First the confirmation that the request was taken. A connection can succeed although
    // the instance behind it is frozen, so this wait has a limit.
    QByteArray line;
    bool closed = false;
    if (!readLine(socket, line, acceptTimeoutMs, closed)) {
        return closed ? SendResult::ConnectionLost : SendResult::NotResponding;
    }
    if (isAccepted(line)) {
        // Then the result. It comes when the import is done, which can take minutes for a
        // first CPAP import, so this wait has no limit: if the running instance goes away,
        // the connection closes and the wait ends.
        if (!readLine(socket, line, -1, closed)) {
            return SendResult::ConnectionLost;
        }
    }
    // (A request that is refused outright is answered without the confirmation.)
    // Leaving this function closes the connection, which tells the instance the reply was read.
    return decodeReply(line, reply) ? SendResult::Replied : SendResult::ConnectionLost;
}

Server::Server(QObject *parent)
    : QObject(parent), m_server(new QLocalServer(this))
{
    // Requests come only from OSCAR started by the same user.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection, this, &Server::onNewConnection);
}

Server::~Server()
{
    close();
}

bool Server::listen(const QString &dataDir, Handler handler)
{
    m_handler = handler;
    const QString place = endpoint(dataDir);
    // On Unix a crashed instance leaves its socket file behind and listen() would fail.
    QLocalServer::removeServer(place);
    if (!m_server->listen(place)) {
        return false;
    }
    if (QAbstractEventDispatcher *dispatcher = QAbstractEventDispatcher::instance()) {
        connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, &Server::onEventLoopIdle, Qt::UniqueConnection);
    }
    return true;
}

void Server::close()
{
    m_server->close();
    // abort() makes onDisconnected() take the entry out of m_queue: work on a copy.
    const QList<Queued> waiting = m_queue;
    m_queue.clear();
    for (const Queued &queued : waiting) {
        if (queued.socket) {
            queued.socket->abort();
        }
    }
}

QString Server::errorString() const
{
    return m_server->errorString();
}

QString Server::listeningOn() const
{
    return m_server->fullServerName();
}

void Server::onNewConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead, this, &Server::onReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, &Server::onDisconnected);
        // Do not keep a connection that never sends its request. This instance may have
        // been too busy to read a request that did arrive in time, so look once more first.
        QTimer::singleShot(RequestTimeoutMs, socket, [this, socket]() {
            if (!socket->property(TakenProperty).toBool()) {
                socket->waitForReadyRead(0);    // fetch what has arrived but was not read yet
                takeRequest(socket);
            }
            if (!socket->property(TakenProperty).toBool()) {
                socket->abort();
            }
        });
        // The request can already be there before the signal was connected.
        takeRequest(socket);
    }
}

void Server::onReadyRead()
{
    if (QLocalSocket *socket = qobject_cast<QLocalSocket *>(sender())) {
        takeRequest(socket);
    }
}

void Server::takeRequest(QLocalSocket *socket)
{
    if (socket->property(TakenProperty).toBool() || socket->state() != QLocalSocket::ConnectedState) {
        return;
    }
    if (!socket->canReadLine()) {
        if (socket->bytesAvailable() > MaxLineLength) {
            qWarning() << "ImportHandover: request longer than" << MaxLineLength << "bytes, dropped";
            socket->setProperty(TakenProperty, true);
            socket->abort();
        }
        return;
    }
    const QByteArray line = socket->readLine(MaxLineLength + 1);
    socket->setProperty(TakenProperty, true);

    Request request;
    QString error;
    if (!decodeRequest(line, request, &error)) {
        qWarning().noquote() << "ImportHandover: request not understood:" << error;
        Reply reply;
        reply.exitCode = ExitFailed;
        reply.message = QStringLiteral("The running OSCAR did not understand the request (%1).").arg(error);
        answer(socket, reply);
        return;
    }

    // Tell the requester it has been heard; the result follows when the import is done.
    QJsonObject accepted;
    accepted[AcceptedKey] = ProtocolVersion;
    socket->write(QJsonDocument(accepted).toJson(QJsonDocument::Compact) + '\n');
    socket->flush();
    if (socket->state() != QLocalSocket::ConnectedState) {
        return;     // the requester has already left (it gave up, or was stopped): nothing to do for it
    }

    Queued queued;
    queued.socket = socket;
    queued.request = request;
    m_queue.append(queued);
    // Served by onEventLoopIdle(), when the event loop has dealt with everything else.
}

void Server::onEventLoopIdle()
{
    if (m_queue.isEmpty() || m_serving) {
        return;
    }
    // This is the event dispatcher saying that it has no more events and is about to wait.
    // At the outermost event loop that means nothing in the application is in the middle of
    // anything: the place to run an import. (Code that merely calls processEvents() while it
    // works does not get here - the dispatcher does not wait for it.)
    if (QThread::currentThread()->loopLevel() <= 1) {
        serveQueued();
        return;
    }
    // A nested event loop is waiting: a dialog or a menu is open, so the user is in the
    // middle of something. Do not keep the requesters waiting for that.
    const QList<Queued> waiting = m_queue;
    m_queue.clear();
    Reply busy;
    busy.exitCode = ExitBusy;
    busy.message = QStringLiteral("The running OSCAR is busy (a dialog or a menu is open).");
    for (const Queued &queued : waiting) {
        if (queued.socket) {
            answer(queued.socket, busy);
        }
    }
}

void Server::serveQueued()
{
    if (m_serving) {
        return;
    }
    m_serving = true;
    while (!m_queue.isEmpty()) {
        const Queued next = m_queue.takeFirst();
        if (!next.socket || next.socket->state() != QLocalSocket::ConnectedState) {
            continue;   // the requester left while it was waiting
        }
        // The requester may give up (be killed) while the handler works and processes
        // events. onDisconnected() leaves this one socket alone, so it is still there to
        // be answered - or rather cleaned up - afterwards.
        m_inService = next.socket;
        Reply reply;
        if (m_handler) {
            reply = m_handler(next.request);
        } else {
            reply.message = QStringLiteral("The running OSCAR does not take import requests.");
        }
        QLocalSocket *socket = m_inService;
        m_inService = nullptr;
        if (socket) {
            answer(socket, reply);
        }
    }
    m_serving = false;
}

void Server::answer(QLocalSocket *socket, const Reply &reply)
{
    if (socket->state() != QLocalSocket::ConnectedState) {
        socket->deleteLater();      // the requester is gone; onDisconnected() left the socket to us
        return;
    }
    socket->write(encodeReply(reply));
    socket->flush();
    // The requester closes the connection once it has read the reply, and onDisconnected()
    // then removes the socket. Closing from this side could discard a reply that has not
    // been read yet (a named pipe does). A requester that never closes is dropped.
    QTimer::singleShot(ReplyLingerMs, socket, [socket]() { socket->abort(); });
}

void Server::onDisconnected()
{
    QLocalSocket *socket = qobject_cast<QLocalSocket *>(sender());
    if (!socket) {
        return;
    }
    // A requester that gave up before its turn is not served.
    for (int i = m_queue.size() - 1; i >= 0; --i) {
        if (m_queue[i].socket == socket) {
            m_queue.removeAt(i);
        }
    }
    if (socket == m_inService) {
        return;     // the handler is working for it: serveQueued() deletes it when the handler is back
    }
    socket->deleteLater();
}

} // namespace ImportHandover
