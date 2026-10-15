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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QStringList>

#include "CLI.h"
#include "Platform.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace CLI
{

CommandLineOptions* ManageArgs(QApplication& melon)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("melonDS, a Nintendo DS emulator.\n"
                                     "With NETPLAY_CONNECT=<file> in its environment, melonDS plays the online match that a "
                                     "launcher (Nakama Matchmaker) found and handed over in that file, then quits: exit status 0 "
                                     "when a player left, 1 when it failed. docs/NETPLAY.md describes online play.");
    parser.addHelpOption();

    parser.addPositionalArgument("nds", "Nintendo DS ROM (or an archive file which contains it) to load into Slot-1");
    parser.addPositionalArgument("gba", "GBA ROM (or an archive file which contains it) to load into Slot-2");

    parser.addOption(QCommandLineOption({"b", "boot"}, "Whether to boot firmware on startup. Defaults to \"auto\" (boot if NDS rom given)", "auto/always/never", "auto"));
    parser.addOption(QCommandLineOption({"f", "fullscreen"}, "Start melonDS in fullscreen mode"));

    parser.addOption(QCommandLineOption("netplay-host", "Host a direct online session of the given ROM on this UDP port", "port"));
    parser.addOption(QCommandLineOption("netplay-join", "Join the direct online session at this address", "host:port"));
    parser.addOption(QCommandLineOption("netplay-code", "Session code of a direct online session (both players give the same)", "code"));
    parser.addOption(QCommandLineOption("netplay-delay", "Input delay in frames when hosting (default: from the round trip)", "frames"));
    parser.addOption(QCommandLineOption("netplay-config", "For this run, use the server and account of a server profile file (the format of IKEMEN GO's servers.json, as Nakama Matchmaker writes it) instead of the online settings", "file"));
    parser.addOption(QCommandLineOption("netplay-server", "The profile of --netplay-config to use (default: the file's default)", "id"));
    parser.addOption(QCommandLineOption("netplay-online", "Open the online window at start and connect to the server; it joins the channel of the given ROM"));
    parser.addOption(QCommandLineOption("netplay-quick", "Like --netplay-online, then start a quick match for the given ROM"));
    parser.addOption(QCommandLineOption("netplay-exit", "Quit melonDS when the online session ends: exit status 0 when a player left, 1 when it failed"));
    parser.addOption(QCommandLineOption("netplay-identity", "Print the online identity of a DS ROM (what matchmakers pair players by) and exit", "rom"));

#ifdef ARCHIVE_SUPPORT_ENABLED
    parser.addOption(QCommandLineOption({"a", "archive-file"}, "Specify file to load inside an archive given (NDS)", "rom"));
    parser.addOption(QCommandLineOption({"A", "archive-file-gba"}, "Specify file to load inside an archive given (GBA)", "rom"));
#endif

    parser.process(melon);

    CommandLineOptions* options = new CommandLineOptions;

    options->fullscreen = parser.isSet("fullscreen");

    if (parser.isSet("netplay-host"))
        options->netplayHost = parser.value("netplay-host").toInt();
    if (parser.isSet("netplay-join"))
        options->netplayJoin = parser.value("netplay-join");
    if (parser.isSet("netplay-code"))
        options->netplayCode = parser.value("netplay-code");
    if (parser.isSet("netplay-delay"))
        options->netplayDelay = parser.value("netplay-delay").toInt();
    options->netplayExit = parser.isSet("netplay-exit");
    if (parser.isSet("netplay-config"))
        options->netplayConfig = parser.value("netplay-config");
    if (parser.isSet("netplay-server"))
        options->netplayServer = parser.value("netplay-server");
    options->netplayQuick = parser.isSet("netplay-quick");
    options->netplayOnline = parser.isSet("netplay-online") || options->netplayQuick;

    if (options->netplayServer.has_value() && !options->netplayConfig.has_value())
    {
        fprintf(stderr, "melonDS: --netplay-server names a profile of --netplay-config, which is missing\n");
        exit(1);
    }
    if (options->netplayOnline && (options->netplayHost.has_value() || options->netplayJoin.has_value()))
    {
        fprintf(stderr, "melonDS: a direct session (--netplay-host, --netplay-join) and --netplay-online or --netplay-quick cannot start together\n");
        exit(1);
    }

    QStringList posargs = parser.positionalArguments();
    switch (posargs.size())
    {
        default:
            Log(LogLevel::Warn, "Too many positional arguments; ignoring 3 onwards\n");
        case 2:
            options->gbaRomPath = posargs[1];
        case 1:
            options->dsRomPath = posargs[0];
        case 0:
            break;
    }

    QString bootMode = parser.value("boot");
    if (bootMode == "auto")
    {
        options->boot = !posargs.empty();
    }
    else if (bootMode == "always")
    {
        options->boot = true;
        options->bootAuto = false;
    }
    else if (bootMode == "never")
    {
        options->boot = false;
        options->bootAuto = false;
    }
    else
    {
        Log(LogLevel::Error, "ERROR: -b/--boot only accepts auto/always/never as arguments\n");
        exit(1);
    }

#ifdef ARCHIVE_SUPPORT_ENABLED
    if (parser.isSet("archive-file"))
    {
        if (options->dsRomPath.has_value())
        {
            options->dsRomArchivePath = parser.value("archive-file");
        }
        else
        {
            Log(LogLevel::Error, "Option -a/--archive-file given, but no archive specified!");
        }
    }

    if (parser.isSet("archive-file-gba"))
    {
        if (options->gbaRomPath.has_value())
        {
            options->gbaRomArchivePath = parser.value("archive-file-gba");
        }
        else
        {
            Log(LogLevel::Error, "Option -A/--archive-file-gba given, but no archive specified!");
        }
    }
#endif

    return options;
}

}
