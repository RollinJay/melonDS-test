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

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSslSocket>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include "NakamaClient.h"

// ---- WebSocket -----------------------------------------------------------------

static constexpr int kMaxMessage = 16 << 20;

WebSocketClient::WebSocketClient(QObject* parent) : QObject(parent)
{
}

WebSocketClient::~WebSocketClient()
{
    if (socket)
        socket->abort();
}

void WebSocketClient::open(const QString& h, int p, bool tls, const QString& pathAndQuery)
{
    if (socket)
    {
        socket->abort();
        socket->deleteLater();
    }
    host = h;
    port = p;
    path = pathAndQuery;
    buffer.clear();
    fragments.clear();
    handshaken = false;
    closing = false;

    // a plain socket unless TLS is wanted (QSslSocket needs a TLS backend)
#if QT_CONFIG(ssl)
    QSslSocket* ssl = tls ? new QSslSocket(this) : nullptr;
    socket = ssl ? ssl : new QTcpSocket(this);
#else
    socket = new QTcpSocket(this);
#endif
    connect(socket, &QTcpSocket::readyRead, this, &WebSocketClient::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &WebSocketClient::onDisconnected);
    connect(socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError)
    {
        if (!closing)
            fail(socket->errorString());
    });

#if QT_CONFIG(ssl)
    if (ssl)
    {
        if (!QSslSocket::supportsSsl())
        {
            fail("TLS is not available on this computer");
            return;
        }
        connect(ssl, &QSslSocket::encrypted, this, &WebSocketClient::onConnected);
        ssl->connectToHostEncrypted(host, (quint16)port);
    }
    else
#else
    // this build's Qt has no TLS support
    if (tls)
        fail("TLS is not available in this build of melonDS");
    else
#endif
    {
        connect(socket, &QTcpSocket::connected, this, &WebSocketClient::onConnected);
        socket->connectToHost(host, (quint16)port);
    }
}

void WebSocketClient::onConnected()
{
    QByteArray nonce(16, 0);
    for (int i = 0; i < 16; i++)
        nonce[i] = (char)QRandomGenerator::global()->bounded(256);
    key = nonce.toBase64();

    QByteArray req;
    req += "GET " + path.toUtf8() + " HTTP/1.1\r\n";
    req += "Host: " + host.toUtf8() + ":" + QByteArray::number(port) + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    req += "\r\n";
    socket->write(req);
}

void WebSocketClient::fail(const QString& reason)
{
    if (closing) return;
    closing = true;
    bool was = handshaken;
    handshaken = false;
    if (socket)
        socket->abort();
    emit closed(reason.isEmpty() ? (was ? QString("connection closed") : QString("could not connect")) : reason);
}

void WebSocketClient::onDisconnected()
{
    fail(QString());
}

void WebSocketClient::close()
{
    if (!socket) return;
    if (handshaken && !closing)
    {
        QByteArray code;
        code.append((char)0x03);
        code.append((char)0xE8);   // 1000: normal closure
        sendFrame(0x8, code);
        socket->flush();
    }
    closing = true;
    handshaken = false;
    socket->disconnectFromHost();
}

void WebSocketClient::sendText(const QByteArray& text)
{
    if (handshaken)
        sendFrame(0x1, text);
}

void WebSocketClient::sendFrame(int opcode, const QByteArray& payload)
{
    QByteArray frame;
    frame.append((char)(0x80 | opcode));
    qint64 len = payload.size();
    if (len < 126)
        frame.append((char)(0x80 | len));
    else if (len < 65536)
    {
        frame.append((char)(0x80 | 126));
        frame.append((char)(len >> 8));
        frame.append((char)len);
    }
    else
    {
        frame.append((char)(0x80 | 127));
        for (int i = 7; i >= 0; i--)
            frame.append((char)(len >> (i * 8)));
    }
    // clients mask every frame
    char mask[4];
    for (char& m : mask)
        m = (char)QRandomGenerator::global()->bounded(256);
    frame.append(mask, 4);
    int start = frame.size();
    frame.append(payload);
    for (qint64 i = 0; i < len; i++)
        frame[start + i] = frame[start + i] ^ mask[i & 3];
    socket->write(frame);
}

void WebSocketClient::onReadyRead()
{
    buffer += socket->readAll();

    if (!handshaken)
    {
        int end = buffer.indexOf("\r\n\r\n");
        if (end < 0)
        {
            if (buffer.size() > 16384)
                fail("bad answer from the server");
            return;
        }
        QByteArray head = buffer.left(end);
        buffer.remove(0, end + 4);

        QList<QByteArray> lines = head.split('\n');
        if (lines.isEmpty() || !lines[0].contains(" 101"))
        {
            fail("the server refused the connection (" + QString::fromUtf8(lines.value(0).trimmed()) + ")");
            return;
        }
        QByteArray expected = QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                                       QCryptographicHash::Sha1).toBase64();
        bool accepted = false;
        for (const QByteArray& line : lines)
        {
            int colon = line.indexOf(':');
            if (colon < 0) continue;
            if (line.left(colon).trimmed().toLower() == "sec-websocket-accept")
                accepted = line.mid(colon + 1).trimmed() == expected;
        }
        if (!accepted)
        {
            fail("the server's answer is not a WebSocket");
            return;
        }
        handshaken = true;
        emit opened();
    }

    while (buffer.size() >= 2)
    {
        const uchar* b = (const uchar*)buffer.constData();
        bool fin = b[0] & 0x80;
        int opcode = b[0] & 0x0F;
        bool masked = b[1] & 0x80;
        qint64 len = b[1] & 0x7F;
        int pos = 2;
        if (len == 126)
        {
            if (buffer.size() < 4) return;
            len = (b[2] << 8) | b[3];
            pos = 4;
        }
        else if (len == 127)
        {
            if (buffer.size() < 10) return;
            len = 0;
            for (int i = 0; i < 8; i++)
                len = (len << 8) | b[2 + i];
            pos = 10;
        }
        if (len > kMaxMessage)
        {
            fail("message too large");
            return;
        }
        uchar mask[4] = {};
        if (masked)
        {
            if (buffer.size() < pos + 4) return;
            memcpy(mask, b + pos, 4);
            pos += 4;
        }
        if (buffer.size() < pos + len) return;

        QByteArray payload = buffer.mid(pos, (int)len);
        buffer.remove(0, pos + (int)len);
        if (masked)
        {
            for (qint64 i = 0; i < len; i++)
                payload[(int)i] = payload[(int)i] ^ mask[i & 3];
        }

        switch (opcode)
        {
        case 0x0:   // continuation
            fragments += payload;
            if (fragments.size() > kMaxMessage)
            {
                fail("message too large");
                return;
            }
            if (fin)
            {
                if (fragmentOpcode == 0x1 || fragmentOpcode == 0x2)
                    emit textReceived(fragments);
                fragments.clear();
            }
            break;
        case 0x1:
        case 0x2:
            if (fin)
                emit textReceived(payload);
            else
            {
                fragmentOpcode = opcode;
                fragments = payload;
            }
            break;
        case 0x8:   // close
            if (!closing)
            {
                sendFrame(0x8, payload.left(2));
                socket->flush();
            }
            fail("the server closed the connection");
            return;
        case 0x9:   // ping
            sendFrame(0xA, payload);
            break;
        default:
            break;
        }
        if (!socket || closing) return;
    }
}

// ---- Nakama --------------------------------------------------------------------

NakamaClient::NakamaClient(QObject* parent) : QObject(parent)
{
    net = new QNetworkAccessManager(this);
    ws = new WebSocketClient(this);
    pingTimer = new QTimer(this);
    pingTimer->setInterval(5000);

    connect(ws, &WebSocketClient::opened, this, [this]
    {
        connected = true;
        connecting = false;
        pingTimer->start();
        sendPing();
        emit connectionChanged(true, QString());
    });
    connect(ws, &WebSocketClient::closed, this, [this](QString reason)
    {
        bool was = connected || connecting;
        connected = false;
        connecting = false;
        pingTimer->stop();
        pendingJoins.clear();
        pingCid.clear();
        // the server drops a closed socket's matchmaker tickets
        ticket.clear();
        ticketCid.clear();
        cancelledTicketCids.clear();
        if (was)
            emit connectionChanged(false, reason);
    });
    connect(ws, &WebSocketClient::textReceived, this, &NakamaClient::onSocketText);
    connect(pingTimer, &QTimer::timeout, this, &NakamaClient::sendPing);
}

NakamaClient::~NakamaClient()
{
}

QString NakamaClient::baseURL() const
{
    return QString("%1://%2:%3").arg(server.SSL ? "https" : "http").arg(server.Host).arg(server.Port);
}

void NakamaClient::connectToServer(const Server& srv, const QString& device, const QString& username)
{
    disconnectFromServer();
    server = srv;
    deviceID = device;
    requestedUsername = username;
    token.clear();
    refreshToken.clear();
    connecting = true;
    authenticate(!username.isEmpty());
}

void NakamaClient::useSession(const Server& srv, const QString& tok, const QString& refresh)
{
    disconnectFromServer();
    server = srv;
    deviceID.clear();
    requestedUsername.clear();
    token = tok;
    refreshToken = refresh;
    connecting = true;
    if (!parseClaims(token, uid, uname, expires))
    {
        token.clear();
        connecting = false;
        emit connectionChanged(false, "The session it was given is not a Nakama session.");
        return;
    }
    withFreshToken([this](bool ok, QString error)
    {
        if (!connecting)
            return;     // cancelled meanwhile
        if (!ok)
        {
            connecting = false;
            emit connectionChanged(false, "Could not renew the session: " + error);
            return;
        }
        openSocket();
    });
}

void NakamaClient::disconnectFromServer()
{
    if (connected || connecting)
    {
        ws->close();
        bool was = connected || connecting;
        connected = false;
        connecting = false;
        pingTimer->stop();
        pendingJoins.clear();
        ticket.clear();
        ticketCid.clear();
        cancelledTicketCids.clear();
        if (was)
            emit connectionChanged(false, QString());
    }
}

static QString ReplyError(QNetworkReply* reply, const QByteArray& body)
{
    QJsonObject obj = QJsonDocument::fromJson(body).object();
    QString msg = obj.value("message").toString();
    if (msg.isEmpty()) msg = obj.value("error").toString();
    if (msg.isEmpty()) msg = reply->errorString();
    return msg;
}

void NakamaClient::authenticate(bool withUsername)
{
    QUrl url(baseURL() + "/v2/account/authenticate/device");
    QUrlQuery q;
    q.addQueryItem("create", "true");
    if (withUsername)
        q.addQueryItem("username", requestedUsername);
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", "Basic " + (server.Key.toUtf8() + ":").toBase64());
    QJsonObject body {{"id", deviceID}};
    QNetworkReply* reply = net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, withUsername]
    {
        reply->deleteLater();
        QByteArray data = reply->readAll();
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!connecting && !connected)
            return;   // cancelled meanwhile
        if (status == 409 && withUsername)
        {
            // the name is taken: the server picks one (an existing account keeps its own)
            authenticate(false);
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
        {
            connecting = false;
            emit connectionChanged(false, "Could not sign in: " + ReplyError(reply, data));
            return;
        }
        onAuthenticated(QJsonDocument::fromJson(data).object());
    });
}

bool NakamaClient::parseClaims(const QString& tok, QString& outUid, QString& outName, qint64& outExp)
{
    QStringList parts = tok.split('.');
    if (parts.size() != 3) return false;
    QByteArray payload = QByteArray::fromBase64(parts[1].toUtf8(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    QJsonObject claims = QJsonDocument::fromJson(payload).object();
    outUid = claims.value("uid").toString();
    outName = claims.value("usn").toString();
    outExp = (qint64)claims.value("exp").toDouble();
    return !outUid.isEmpty();
}

bool NakamaClient::takeSession(const QJsonObject& session)
{
    QString newToken = session.value("token").toString();
    QString newUid, newName;
    qint64 newExpires = 0;
    if (!parseClaims(newToken, newUid, newName, newExpires))
        return false;
    token = newToken;
    QString refresh = session.value("refresh_token").toString();
    if (!refresh.isEmpty())
        refreshToken = refresh;
    uid = newUid;
    uname = newName;
    expires = newExpires;
    return true;
}

void NakamaClient::onAuthenticated(const QJsonObject& session)
{
    if (!takeSession(session))
    {
        connecting = false;
        emit connectionChanged(false, "The server sent an invalid session.");
        return;
    }
    openSocket();
}

void NakamaClient::openSocket()
{
    if (connected || ws->isOpen())
        return;

    QUrlQuery q;
    q.addQueryItem("lang", "en");
    q.addQueryItem("status", "true");
    q.addQueryItem("format", "json");
    q.addQueryItem("token", token);     // a JWT: nothing in it needs escaping
    ws->open(server.Host, server.Port, server.SSL, "/ws?" + q.toString(QUrl::FullyEncoded));
}

void NakamaClient::withFreshToken(std::function<void(bool, QString)> then)
{
    if (token.isEmpty())
    {
        then(false, "not signed in");
        return;
    }
    if (expires - QDateTime::currentSecsSinceEpoch() > 30)
    {
        then(true, QString());
        return;
    }

    QNetworkRequest req(QUrl(baseURL() + "/v2/account/session/refresh"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", "Basic " + (server.Key.toUtf8() + ":").toBase64());
    QJsonObject body {{"token", refreshToken}};
    QNetworkReply* reply = net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, then]
    {
        reply->deleteLater();
        QByteArray data = reply->readAll();
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300)
        {
            if (takeSession(QJsonDocument::fromJson(data).object()))
                then(true, QString());
            else
                then(false, "the server sent an invalid session");
            return;
        }
        // the refresh token expired too: sign in again with the device id, if
        // this client has one (a session that someone else opened ends here)
        if (deviceID.isEmpty())
        {
            then(false, ReplyError(reply, data));
            return;
        }
        QUrl url(baseURL() + "/v2/account/authenticate/device");
        QUrlQuery q;
        q.addQueryItem("create", "true");
        url.setQuery(q);
        QNetworkRequest req2(url);
        req2.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req2.setRawHeader("Authorization", "Basic " + (server.Key.toUtf8() + ":").toBase64());
        QJsonObject body2 {{"id", deviceID}};
        QNetworkReply* r2 = net->post(req2, QJsonDocument(body2).toJson(QJsonDocument::Compact));
        connect(r2, &QNetworkReply::finished, this, [this, r2, then]
        {
            r2->deleteLater();
            QByteArray d2 = r2->readAll();
            int s2 = r2->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (r2->error() != QNetworkReply::NoError || s2 < 200 || s2 >= 300)
            {
                then(false, ReplyError(r2, d2));
                return;
            }
            if (takeSession(QJsonDocument::fromJson(d2).object()))
                then(true, QString());
            else
                then(false, "the server sent an invalid session");
        });
    });
}

void NakamaClient::callRPC(const QString& id, const QJsonObject& payload, RpcDone done)
{
    withFreshToken([this, id, payload, done](bool ok, QString error)
    {
        if (!ok)
        {
            done(false, {}, error);
            return;
        }
        // ?unwrap: the body and the answer are the payloads themselves
        QNetworkRequest req(QUrl(baseURL() + "/v2/rpc/" + QString::fromUtf8(QUrl::toPercentEncoding(id)) + "?unwrap"));
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        QNetworkReply* reply = net->post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [reply, done]
        {
            reply->deleteLater();
            QByteArray data = reply->readAll();
            int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
            {
                done(false, {}, ReplyError(reply, data));
                return;
            }
            done(true, QJsonDocument::fromJson(data).object(), QString());
        });
    });
}

void NakamaClient::sendEnvelope(const QString& field, const QJsonObject& payload, const QString& cid)
{
    if (!connected) return;
    QJsonObject env;
    env.insert("cid", cid.isEmpty() ? QString::number(nextCid++) : cid);
    env.insert(field, payload);
    ws->sendText(QJsonDocument(env).toJson(QJsonDocument::Compact));
}

void NakamaClient::joinMatch(const QString& matchID)
{
    QString cid = QString::number(nextCid++);
    pendingJoins.insert(cid, PendingJoin {matchID, QString()});
    sendEnvelope("match_join", QJsonObject {{"match_id", matchID}}, cid);
}

void NakamaClient::joinMatchByToken(const QString& matchToken)
{
    QString cid = QString::number(nextCid++);
    pendingJoins.insert(cid, PendingJoin {QString(), matchToken});
    sendEnvelope("match_join", QJsonObject {{"token", matchToken}}, cid);
}

void NakamaClient::leaveMatch(const QString& matchID)
{
    sendEnvelope("match_leave", QJsonObject {{"match_id", matchID}});
}

void NakamaClient::sendMatchData(const QString& matchID, qint64 opCode, const QByteArray& data)
{
    QJsonObject p;
    p.insert("match_id", matchID);
    p.insert("op_code", (double)opCode);
    p.insert("data", QString::fromLatin1(data.toBase64()));
    p.insert("reliable", true);
    sendEnvelope("match_data_send", p);
}

void NakamaClient::addMatchmaker(const QString& query, int minCount, int maxCount, const QJsonObject& stringProperties)
{
    removeMatchmaker();
    if (!connected)
    {
        emit matchmakerFailed("not connected to the server");
        return;
    }
    ticketCid = QString::number(nextCid++);
    QJsonObject add {
        {"query", query},
        {"min_count", minCount},
        {"max_count", maxCount},
        {"count_multiple", 1},
        {"string_properties", stringProperties},
        {"numeric_properties", QJsonObject()},
    };
    sendEnvelope("matchmaker_add", add, ticketCid);
}

void NakamaClient::removeMatchmaker()
{
    if (!ticket.isEmpty())
    {
        sendEnvelope("matchmaker_remove", QJsonObject {{"ticket", ticket}});
        ticket.clear();
    }
    if (!ticketCid.isEmpty())
    {
        // removed when the server confirms it (also after another ticket was added)
        cancelledTicketCids.insert(ticketCid);
        ticketCid.clear();
    }
}

bool NakamaClient::isMatchmaking() const
{
    return !ticket.isEmpty() || !ticketCid.isEmpty();
}

static QStringList PresenceUserIDs(const QJsonValue& list)
{
    QStringList out;
    for (const QJsonValue& v : list.toArray())
        out << v.toObject().value("user_id").toString();
    return out;
}

void NakamaClient::sendPing()
{
    if (!connected) return;
    if (!pingCid.isEmpty() && pingClock.elapsed() < 15000)
        return;     // still waiting for the last one
    pingCid = QString::number(nextCid++);
    pingClock.start();
    sendEnvelope("ping", QJsonObject(), pingCid);
}

void NakamaClient::onSocketText(const QByteArray& text)
{
    QJsonObject env = QJsonDocument::fromJson(text).object();
    if (env.isEmpty()) return;
    QString cid = env.value("cid").toString();

    if (!cid.isEmpty() && pendingJoins.contains(cid))
    {
        PendingJoin join = pendingJoins.take(cid);
        QString error = env.value("error").toObject().value("message").toString();
        if (join.Token.isEmpty())
        {
            if (env.contains("match"))
                emit matchJoined(join.MatchID);
            else
                emit matchJoinFailed(join.MatchID, error);
        }
        else
        {
            // joined by token: the match's id comes with the answer
            QString matchID = env.value("match").toObject().value("match_id").toString();
            if (!matchID.isEmpty())
                emit matchJoinedByToken(join.Token, matchID);
            else
                emit matchJoinByTokenFailed(join.Token, error.isEmpty() ? QString("the server sent no match") : error);
        }
        return;
    }
    if (!cid.isEmpty() && cancelledTicketCids.remove(cid))
    {
        // a ticket removed before the server confirmed it
        QString oldTicket = env.value("matchmaker_ticket").toObject().value("ticket").toString();
        if (!oldTicket.isEmpty())
            sendEnvelope("matchmaker_remove", QJsonObject {{"ticket", oldTicket}});
        return;
    }
    if (!cid.isEmpty() && cid == ticketCid)
    {
        ticketCid.clear();
        QString newTicket = env.value("matchmaker_ticket").toObject().value("ticket").toString();
        if (!newTicket.isEmpty())
            ticket = newTicket;
        else
        {
            QString error = env.value("error").toObject().value("message").toString();
            emit matchmakerFailed(error.isEmpty() ? QString("the server refused the ticket") : error);
        }
        return;
    }
    if (env.contains("matchmaker_matched"))
    {
        QJsonObject o = env.value("matchmaker_matched").toObject();
        NakamaMatched m;
        m.Ticket = o.value("ticket").toString();
        if (m.Ticket.isEmpty() || m.Ticket != ticket)
            return;     // a ticket removed meanwhile
        ticket.clear();
        m.MatchID = o.value("match_id").toString();
        m.Token = o.value("token").toString();
        QString selfSession = o.value("self").toObject().value("presence").toObject().value("session_id").toString();
        for (const QJsonValue& v : o.value("users").toArray())
        {
            QJsonObject presence = v.toObject().value("presence").toObject();
            if (!selfSession.isEmpty() && presence.value("session_id").toString() == selfSession)
                m.Self = m.UserIDs.size();
            m.UserIDs << presence.value("user_id").toString();
            m.Names << presence.value("username").toString();
        }
        emit matchmakerMatched(m);
        return;
    }
    if (env.contains("match_presence_event"))
    {
        QJsonObject o = env.value("match_presence_event").toObject();
        emit matchPresence(o.value("match_id").toString(), PresenceUserIDs(o.value("joins")), PresenceUserIDs(o.value("leaves")));
        return;
    }
    if (!cid.isEmpty() && cid == pingCid && env.contains("pong"))
    {
        pingCid.clear();
        emit roundTripMeasured((int)pingClock.elapsed());
        return;
    }
    if (env.contains("match_data"))
    {
        QJsonObject md = env.value("match_data").toObject();
        // op_code comes as a string (protobuf JSON for int64)
        QJsonValue opv = md.value("op_code");
        qint64 op = opv.isString() ? opv.toString().toLongLong() : (qint64)opv.toDouble();
        QByteArray data = QByteArray::fromBase64(md.value("data").toString().toLatin1());
        QString sender = md.value("presence").toObject().value("user_id").toString();
        emit matchData(md.value("match_id").toString(), sender, op, data);
        return;
    }
    if (env.contains("error"))
    {
        emit serverError(env.value("error").toObject().value("message").toString());
        return;
    }
}
