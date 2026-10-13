/*
    Copyright 2021-2023 melonDS team

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

#ifndef CLI_H
#define CLI_H

#include <QApplication>
#include <QStringList>

#include <optional>

namespace CLI {

struct CommandLineOptions
{
    std::optional<QString> dsRomPath;
    std::optional<QString> dsRomArchivePath;
    std::optional<QString> gbaRomPath;
    std::optional<QString> gbaRomArchivePath;
    bool fullscreen;
    bool boot;

    // a direct online session started at launch (with the ROM given)
    std::optional<int> netplayHost;         // UDP port to wait on
    std::optional<QString> netplayJoin;     // host:port
    std::optional<QString> netplayCode;
    std::optional<int> netplayDelay;
    bool netplayExit = false;               // quit when the session ends

    // online through a server named by a launcher (Nakama Matchmaker)
    std::optional<QString> netplayConfig;   // server profiles file
    std::optional<QString> netplayServer;   // profile id
    bool netplayOnline = false;             // connect at start
    bool netplayQuick = false;              // and ask for a quick match
};

extern CommandLineOptions* ManageArgs(QApplication& melon);

}

#endif
