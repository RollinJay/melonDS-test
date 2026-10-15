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

#include <cstring>
#include <memory>
#include <string>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "NetplayLauncher.h"
#include "EmuInstance.h"
#include "Window.h"
#include "NDS_Header.h"
#include "OnlineSession.h"

using namespace melonDS;

namespace NetplayLauncher
{

static bool ReadJsonObject(const QString& path, QJsonObject& out, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = "cannot read " + path + ": " + file.errorString();
        return false;
    }
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
    {
        error = path + " is not a JSON object" +
                (parseError.error != QJsonParseError::NoError ? " (" + parseError.errorString() + ")" : QString());
        return false;
    }
    out = doc.object();
    return true;
}

// Nakama accepts device ids of 10 to 128 characters
static bool ValidDeviceID(const QString& id)
{
    return id.length() >= 10 && id.length() <= 128;
}

bool ReadServerProfile(const QString& path, const QString& id, ServerProfile& out, QString& error)
{
    QJsonObject root;
    if (!ReadJsonObject(path, root, error))
        return false;
    // the rules of IKEMEN GO's reader: version 1 (or none), default: the first server
    int version = root.value("version").toInt(0);
    if (version != 0 && version != 1)
    {
        error = path + " is a server profile file of version " + QString::number(version) +
                ", which this melonDS does not read";
        return false;
    }
    QJsonArray servers = root.value("servers").toArray();
    if (servers.isEmpty())
    {
        error = path + " holds no server profiles";
        return false;
    }
    QString want = id.trimmed();
    if (want.isEmpty())
        want = root.value("default").toString().trimmed();
    if (want.isEmpty())
        want = servers.first().toObject().value("id").toString();

    for (const QJsonValue& v : servers)
    {
        QJsonObject p = v.toObject();
        if (p.value("id").toString() != want)
            continue;

        ServerProfile sp;
        sp.ID = want;
        sp.Name = p.value("name").toString().trimmed();
        sp.Server.Host = p.value("host").toString().trimmed();
        sp.Server.Port = p.value("port").toInt(0);
        if (sp.Server.Port == 0)
            sp.Server.Port = 7350;
        sp.Server.Key = p.value("server_key").toString().trimmed();
        if (sp.Server.Key.isEmpty())
            sp.Server.Key = "defaultkey";
        sp.Server.SSL = p.value("ssl").toBool(false);
        sp.DeviceID = p.value("device_id").toString().trimmed();
        sp.Username = p.value("username").toString().trimmed().left(24);
        for (const QJsonValue& s : p.value("stun_servers").toArray())
        {
            QString stun = s.toString().trimmed();
            if (!stun.isEmpty())
                sp.Stun << stun;
        }

        if (sp.Server.Host.isEmpty())
        {
            error = "the server profile " + want + " in " + path + " names no host";
            return false;
        }
        if (sp.Server.Port < 1 || sp.Server.Port > 65535)
        {
            error = "the server profile " + want + " in " + path + " has no valid port";
            return false;
        }
        if (!sp.DeviceID.isEmpty() && !ValidDeviceID(sp.DeviceID))
        {
            error = "the device id of the server profile " + want + " in " + path +
                    " is not 10 to 128 characters long";
            return false;
        }
        out = sp;
        return true;
    }
    error = "there is no server profile " + want + " in " + path;
    return false;
}

static bool ValidIdentity(const QString& identity)
{
    static const QRegularExpression hex40("^[0-9a-f]{40}$");
    return hex40.match(identity).hasMatch();
}

static bool ParseConnectionFile(const QString& path, ConnectionFile& out, QString& error)
{
    QJsonObject o;
    if (!ReadJsonObject(path, o, error))
        return false;
    if (o.value("version").toInt() != 1)
    {
        error = path + " is not a version 1 connection file";
        return false;
    }

    ConnectionFile c;
    QJsonObject server = o.value("server").toObject();
    c.Server.Host = server.value("host").toString().trimmed();
    c.Server.Port = server.value("port").toInt(7350);
    c.Server.Key = server.value("key").toString("defaultkey");
    c.Server.SSL = server.value("ssl").toBool(false);
    QJsonObject session = o.value("session").toObject();
    c.Token = session.value("token").toString();
    c.RefreshToken = session.value("refresh_token").toString();
    QJsonObject match = o.value("match").toObject();
    c.MatchID = match.value("match_id").toString();
    c.MatchToken = match.value("token").toString();
    for (const QJsonValue& v : o.value("players").toArray())
    {
        QJsonObject p = v.toObject();
        c.UserIDs << p.value("user_id").toString();
        c.Names << p.value("name").toString();
    }
    c.Self = o.value("self").toInt(-1);
    QJsonObject game = o.value("game").toObject();
    QString kind = game.value("kind").toString();
    c.ROMPath = game.value("profile").toString();
    c.Identity = game.value("identity").toString();
    for (const QJsonValue& v : o.value("stun").toArray())
    {
        QString stun = v.toString().trimmed();
        if (!stun.isEmpty())
            c.Stun << stun;
    }
    c.Port = o.value("port").toInt(0);
    c.InputDelay = o.value("input_delay").toInt(-1);

    if (c.Server.Host.isEmpty() || c.Server.Port < 1 || c.Server.Port > 65535)
        error = "it names no server";
    else if (c.Token.isEmpty())
        error = "it holds no session";
    else if (c.MatchID.isEmpty() && c.MatchToken.isEmpty())
        error = "it names no match";
    else if (kind != "melonds")
        error = "it is a match of " + (kind.isEmpty() ? QString("another program") : kind) + ", not of melonDS";
    else if (c.UserIDs.size() != 2)
        error = QString("it is a match of %1 players; melonDS plays online with 2").arg(c.UserIDs.size());
    else if (c.Self < 0 || c.Self > 1)
        error = "it does not say which player this computer is";
    else if (c.UserIDs[0].isEmpty() || c.UserIDs[1].isEmpty() || c.UserIDs[0] == c.UserIDs[1])
        error = "its players are not two different users";
    else if (c.ROMPath.isEmpty())
        error = "it names no ROM (game.profile)";
    else if (!ValidIdentity(c.Identity))
        error = "its game identity is not 40 hexadecimal digits";
    else if (c.Port < 0 || c.Port > 65535)
        error = "its UDP port is not valid";
    if (!error.isEmpty())
    {
        error = "the launcher's match (" + path + ") cannot be played: " + error;
        return false;
    }
    if (c.InputDelay < -1 || c.InputDelay > 15)
        c.InputDelay = -1;
    for (int i = 0; i < 2; i++)
    {
        if (c.Names[i].trimmed().isEmpty())
            c.Names[i] = QString("Player %1").arg(i + 1);
    }
    out = c;
    return true;
}

bool TakeConnectionFile(ConnectionFile& out, QString& error)
{
    error.clear();
    QByteArray env = qgetenv("NETPLAY_CONNECT");
    // the file holds the launcher's session: read once, and passed on to nobody
    qunsetenv("NETPLAY_CONNECT");
    if (env.isEmpty())
        return false;
    QString path = QString::fromLocal8Bit(env);
    bool ok = ParseConnectionFile(path, out, error);
    QFile::remove(path);
    return ok;
}

QString IdentityOfFiles(const QStringList& files, QString& error)
{
    std::unique_ptr<u8[]> data;
    u32 len = 0;
    std::string basepath, romname;
    QString name = files.join("|");
    if (files.isEmpty() || !EmuInstance::loadROMData(files, data, len, basepath, romname) || !data)
    {
        error = "cannot read the ROM " + name;
        return QString();
    }

    // what NDSCart::ParseROM checks before it takes a ROM
    u32 padded = 1;
    while (padded < len && padded < 0x80000000u)
        padded <<= 1;
    NDSHeader header {};
    bool valid = len >= 0x1000 && len <= 512 * 1024 * 1024;
    if (valid)
    {
        memcpy(&header, data.get(), sizeof(header));
        valid = header.ARM9ROMOffset >= 0x200 && (u64)header.ARM9ROMOffset + header.ARM9Size <= padded &&
                header.ARM7ROMOffset >= 0x200 && (u64)header.ARM7ROMOffset + header.ARM7Size <= padded;
    }
    if (!valid)
    {
        error = name + " is not a DS ROM that melonDS can load";
        return QString();
    }
    return QString::fromStdString(RollbackNet::NetplayIdentity(data.get(), len));
}

QString IdentityOfPath(const QString& path, QString& error)
{
    QStringList files = MainWindow::romFilesWithoutAsking(path, error);
    if (files.isEmpty())
        return QString();
    return IdentityOfFiles(files, error);
}

}
