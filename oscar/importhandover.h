/* Hand an import over to an OSCAR instance that is already running
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef IMPORTHANDOVER_H
#define IMPORTHANDOVER_H

#include <functional>

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>

class QLocalServer;
class QLocalSocket;

/*! \namespace ImportHandover
    \brief Lets "OSCAR --import <loader> <path>" work while OSCAR is open.

    OSCAR allows one instance per data folder (oscar.lock), and that instance keeps the
    profile in memory. A second process therefore must not import into the same folder
    itself. Instead it asks the running instance to do the import: the running instance
    listens on a local socket (a named pipe on Windows), runs the import with the code
    the Import menu uses, refreshes its views and answers with the result. The second
    process prints the result and exits with it.

    On Unix the socket is a file inside the data folder, so both sides find it without
    relying on the environment and nobody but the folder's owner can put one there. A
    folder that cannot hold a socket (FAT/exFAT, a network share, some FUSE mounts) or
    whose path is too long for a socket address falls back to a name derived from the
    folder. On Windows it is a named pipe whose name is derived from the data folder.

    A request is carried out only when the running instance's main event loop is idle:
    no other part of OSCAR is then in the middle of something. Much of OSCAR processes
    events while it works, and an import that started inside such a call would pull the
    data out from under it.

    Only the user who runs OSCAR can connect, and a request can name nothing but a loader
    and a path - what that user can already do by starting "OSCAR --import".

    The exchange, one line of JSON each:
      request   {"oscarImportRequest":1,"profile":"name","imports":[{"loader":"Viatom","path":"/abs/path"}]}
      accepted  {"oscarImportAccepted":1}             at once: the instance is alive and will import
      reply     {"oscarImportReply":1,"exitCode":0,"message":"..."}        when the import is done
    */
namespace ImportHandover
{

//! \brief Exit codes of "OSCAR --import" when a running instance did (or refused) the import.
const int ExitOk = 0;       //!< every import succeeded
const int ExitFailed = 1;   //!< an import failed, or the request could not be served
const int ExitBusy = 75;    //!< the running instance cannot import right now; try again (EX_TEMPFAIL)

//! \brief Longest line accepted in either direction.
const int MaxLineLength = 64 * 1024;

struct Request
{
    QString profile;                        //!< --profile as given, empty if none
    QList<QPair<QString, QString>> tasks;   //!< loader name, absolute path - in the order given
};

struct Reply
{
    int exitCode = ExitFailed;
    QString message;
};

/*! \brief Where the instance that has \a dataDir open listens, in the order tried.

    On Unix the socket file in the data folder, then a name in the system's folder for
    temporary files for a data folder that cannot hold a socket. Only the name if the path
    of the socket file would be too long for a socket address. On Windows a pipe name. */
QStringList endpoints(const QString &dataDir);

QByteArray encodeRequest(const Request &request);
//! \return false (and a reason in \a error) if \a line is not a request this version understands.
bool decodeRequest(const QByteArray &line, Request &request, QString *error = nullptr);

QByteArray encodeReply(const Reply &reply);
bool decodeReply(const QByteArray &line, Reply &reply);

enum class SendResult
{
    Replied,              //!< the running instance answered; see the reply
    NoInstanceListening,  //!< nothing accepts requests for this data folder (yet)
    NotResponding,        //!< something accepted the connection but did not take the request in time
    ConnectionLost        //!< the running instance went away before it answered
};

/*! \brief Ask the instance that has \a dataDir open to run \a request, and wait for its answer.

    The running instance has \a acceptTimeoutMs to confirm that it took the request - a
    frozen instance must not hang the caller. After that the wait has no limit, however
    long the import takes; it ends early only if the instance goes away. Needs no event
    loop. */
SendResult send(const QString &dataDir, const Request &request, Reply &reply,
                int connectTimeoutMs = 2000, int acceptTimeoutMs = 30000);

/*! \class Server
    \brief The listening side, owned by the instance that holds the data folder's lock.

    Requests are served one at a time in the thread the server lives in, and only when
    that thread's outermost event loop has nothing else to do (see the namespace
    description). A request that arrives while OSCAR is working waits; one that arrives
    while a nested event loop is waiting for the user - a dialog or a menu is open - is
    answered ExitBusy without reaching the handler, and so are the requests still waiting
    when the server is closed. */
class Server : public QObject
{
    Q_OBJECT
  public:
    using Handler = std::function<Reply(const Request &)>;

    explicit Server(QObject *parent = nullptr);
    ~Server() override;

    /*! \brief Start accepting requests for \a dataDir, at the first of endpoints() that works.

        Call only while holding the data folder's lock: a socket left behind by a crashed
        instance is removed first, which is safe only because no other instance can be
        serving the same folder. */
    bool listen(const QString &dataDir, Handler handler);
    void close();
    QString errorString() const;
    //! \brief What listen() is listening on.
    QString listeningOn() const;

  private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();
    void onEventLoopIdle();

  private:
    struct Queued
    {
        QPointer<QLocalSocket> socket;
        Request request;
    };

    void takeRequest(QLocalSocket *socket);
    void serveQueued();
    void answer(QLocalSocket *socket, const Reply &reply);

    QLocalServer *m_server;
    Handler m_handler;
    QList<Queued> m_queue;                  //!< complete requests not served yet
    QPointer<QLocalSocket> m_inService;     //!< the requester the handler is working for
    bool m_serving = false;
};

} // namespace ImportHandover

#endif // IMPORTHANDOVER_H
