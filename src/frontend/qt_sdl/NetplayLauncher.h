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

#ifndef NETPLAYLAUNCHER_H
#define NETPLAYLAUNCHER_H

#include <QString>
#include <QStringList>

#include "NakamaClient.h"

// What melonDS takes from a launcher such as Nakama Matchmaker for online
// play (docs/NETPLAY.md, "From a launcher"): a server and account to
// use for one run, a match the launcher found, and the identity by which
// matchmakers pair players.
namespace NetplayLauncher
{

// A server and the account to use on it (--netplay-config): one entry of a
// server profile file in the format of IKEMEN GO's servers.json
//   {"version": 1, "default": "<id>", "servers": [{"id", "name", "host", "port",
//    "server_key", "ssl", "device_id", "username", "stun_servers": [...]}]}
// The device id is the account's: the same id signs in to the same account.
struct ServerProfile
{
    QString ID;
    QString Name;
    NakamaClient::Server Server;
    QString DeviceID;
    QString Username;           // the name to use online (may be empty)
    QStringList Stun;           // may be empty
};

// Reads the profile `id` of a server profile file (its default when id is
// empty). False, and why, when the file holds no usable profile.
bool ReadServerProfile(const QString& path, const QString& id, ServerProfile& out, QString& error);

// A match that a launcher made, from its connection file (version 1 of
// Nakama Matchmaker's docs/launch.md): the server, the launcher's session on
// it, the match and the players in the matchmaker's order (player 1 hosts).
struct ConnectionFile
{
    NakamaClient::Server Server;
    QString Token, RefreshToken;
    QString MatchID;            // an authoritative match...
    QString MatchToken;         // ...or the token of a relayed one
    QStringList UserIDs, Names; // player 1 first
    int Self = -1;              // this player's index
    QString ROMPath;            // game.profile: the ROM, as melonDS loads it
    QString Identity;           // the identity the launcher matched on
    QStringList Stun;
    int Port = 0;               // UDP port; 0: melonDS chooses
    int InputDelay = -1;        // frames; -1: melonDS chooses
};

// Takes the connection file that the environment variable NETPLAY_CONNECT
// names: reads it, deletes it and removes the variable, so that programs
// melonDS starts do not get the launcher's session. False with error empty
// when the variable is not set; false with an error when the file is not a
// two-player melonDS match.
bool TakeConnectionFile(ConnectionFile& out, QString& error);

// The identity of the ROM that melonDS loads for these files (a ROM file, or
// an archive and the ROM in it, as EmuInstance::loadROM takes them): see
// melonDS::RollbackNet::NetplayIdentity. Empty, and why, if they cannot be read.
QString IdentityOfFiles(const QStringList& files, QString& error);

// The same for a ROM path as the command line takes it ("game.nds",
// "game.zip" holding one ROM, or "game.zip|game.nds"), without asking
// anything: works without a window.
QString IdentityOfPath(const QString& path, QString& error);

}

#endif // NETPLAYLAUNCHER_H
