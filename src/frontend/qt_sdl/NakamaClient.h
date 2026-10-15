/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef NAKAMACLIENT_H
#define NAKAMACLIENT_H

#include <functional>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QTcpSocket;
class QTimer;

// A minimal WebSocket client (RFC 6455) over Qt Network: text messages, pings
// answered, TLS when asked. Enough for Nakama's realtime API without a
// dependency on Qt WebSockets.
class WebSocketClient : public QObject
{
    Q_OBJECT

public:
    explicit WebSocketClient(QObject* parent = nullptr);
    ~WebSocketClient() override;

    // pathAndQuery: "/ws?token=..."
    void open(const QString& host, int port, bool tls, const QString& pathAndQuery);
    void sendText(const QByteArray& text);
    void close();
    bool isOpen() const { return handshaken; }

signals:
    void opened();
    void closed(QString reason);
    void textReceived(QByteArray text);

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();

private:
    void sendFrame(int opcode, const QByteArray& payload);
    void fail(const QString& reason);

    QTcpSocket* socket = nullptr;     // a QSslSocket for TLS
    QString host;
    int port = 0;
    QString path;
    QByteArray key;
    QByteArray buffer;
    QByteArray fragments;
    int fragmentOpcode = 0;
    bool handshaken = false;
    bool closing = false;
};

// What Nakama's matchmaker found: the players, in the order the matchmaker
// gives them (the same for every one of them), and the match they play in:
// an authoritative match that the server's matchmaker hook made (MatchID), or
// a match that Nakama relays (Token).
struct NakamaMatched
{
    QString Ticket;
    QString MatchID;
    QString Token;
    QStringList UserIDs;
    QStringList Names;          // their usernames
    int Self = -1;              // this player's index
};
Q_DECLARE_METATYPE(NakamaMatched)

// A client of a Nakama server (https://heroiclabs.com/nakama): signs in with
// a device id (or a session that someone else opened), keeps a realtime socket
// open, calls RPCs, exchanges match data and uses the matchmaker. Everything
// happens on the thread that owns it (the UI thread).
class NakamaClient : public QObject
{
    Q_OBJECT

public:
    struct Server
    {
        QString Host;
        int Port = 7350;
        QString Key = "defaultkey";
        bool SSL = false;
    };

    explicit NakamaClient(QObject* parent = nullptr);
    ~NakamaClient() override;

    // username names the account when the device's account is created
    void connectToServer(const Server& server, const QString& deviceID, const QString& username);
    // Signs in with a session that someone else opened (a launcher's), renewed
    // with its refresh token when it is about to expire: the same account.
    void useSession(const Server& server, const QString& token, const QString& refreshToken);
    void disconnectFromServer();
    bool isConnected() const { return connected; }
    bool isConnecting() const { return connecting; }

    QString userID() const { return uid; }
    QString username() const { return uname; }

    using RpcDone = std::function<void(bool ok, const QJsonObject& reply, const QString& error)>;
    void callRPC(const QString& id, const QJsonObject& payload, RpcDone done);

    void joinMatch(const QString& matchID);
    // a match that Nakama relays, by its matchmaker token (matchJoinedByToken)
    void joinMatchByToken(const QString& matchToken);
    void leaveMatch(const QString& matchID);
    void sendMatchData(const QString& matchID, qint64 opCode, const QByteArray& data);

    // Adds a matchmaker ticket (one at a time: an earlier one is removed):
    // matchmakerMatched when it is matched, matchmakerFailed if the server
    // refuses it.
    void addMatchmaker(const QString& query, int minCount, int maxCount, const QJsonObject& stringProperties);
    // removes the ticket, also one the server has not confirmed yet
    void removeMatchmaker();
    bool isMatchmaking() const;

signals:
    void connectionChanged(bool connected, QString error);
    void matchJoined(QString matchID);
    void matchJoinFailed(QString matchID, QString error);
    void matchJoinedByToken(QString matchToken, QString matchID);
    void matchJoinByTokenFailed(QString matchToken, QString error);
    void matchData(QString matchID, QString senderID, qint64 opCode, QByteArray data);
    // user ids that joined and left a match this client is in
    void matchPresence(QString matchID, QStringList joined, QStringList left);
    void matchmakerMatched(NakamaMatched matched);
    void matchmakerFailed(QString error);
    void serverError(QString message);
    void roundTripMeasured(int ms);

private:
    struct PendingJoin
    {
        QString MatchID;
        QString Token;
    };

    void authenticate(bool withUsername);
    void onAuthenticated(const QJsonObject& session);
    bool takeSession(const QJsonObject& session);
    void openSocket();
    void withFreshToken(std::function<void(bool ok, QString error)> then);
    void sendEnvelope(const QString& field, const QJsonObject& payload, const QString& cid = QString());
    void onSocketText(const QByteArray& text);
    void sendPing();
    QString baseURL() const;
    static bool parseClaims(const QString& token, QString& uid, QString& uname, qint64& expires);

    QNetworkAccessManager* net;
    WebSocketClient* ws;
    QTimer* pingTimer;

    Server server;
    QString deviceID;
    QString requestedUsername;
    QString token, refreshToken;
    QString uid, uname;
    qint64 expires = 0;
    bool connected = false;
    bool connecting = false;

    int nextCid = 1;
    QHash<QString, PendingJoin> pendingJoins;   // by cid
    QString ticket;             // the matchmaker ticket, once the server confirmed it
    QString ticketCid;          // a ticket being added
    QSet<QString> cancelledTicketCids;  // tickets removed before the server confirmed them
    QString pingCid;
    QElapsedTimer pingClock;
};

#endif // NAKAMACLIENT_H
