<p align="center"><img src="https://raw.githubusercontent.com/melonDS-emu/melonDS/master/res/icon/melon_128x128.png"></p>
<h2 align="center"><b>melonDS</b></h2>
<p align="center">
<a href="http://melonds.kuribo64.net/" alt="melonDS website"><img src="https://img.shields.io/badge/website-melonds.kuribo64.net-%2331352e.svg"></a>
<a href="http://melonds.kuribo64.net/downloads.php" alt="Release: 1.1"><img src="https://img.shields.io/badge/release-1.1-%235c913b.svg"></a>
<a href="https://www.gnu.org/licenses/gpl-3.0" alt="License: GPLv3"><img src="https://img.shields.io/badge/License-GPL%20v3-%23ff554d.svg"></a>
<a href="https://kiwiirc.com/client/irc.badnik.net/?nick=IRC-Source_?#melonds" alt="IRC channel: #melonds"><img src="https://img.shields.io/badge/IRC%20chat-%23melonds-%23dd2e44.svg"></a>
<a href="https://discord.gg/pAMAtExcqV" alt="Discord"><img src="https://img.shields.io/badge/Discord-Kuribo64-7289da?logo=discord&logoColor=white"></a>
<br>
<a href="https://github.com/melonDS-emu/melonDS/actions/workflows/build-windows.yml?query=event%3Apush"><img src="https://github.com/melonDS-emu/melonDS/actions/workflows/build-windows.yml/badge.svg" /></a>
<a href="https://github.com/melonDS-emu/melonDS/actions/workflows/build-ubuntu.yml?query=event%3Apush"><img src="https://github.com/melonDS-emu/melonDS/actions/workflows/build-ubuntu.yml/badge.svg" /></a>
<a href="https://github.com/melonDS-emu/melonDS/actions/workflows/build-macos.yml?query=event%3Apush"><img src="https://github.com/melonDS-emu/melonDS/actions/workflows/build-macos.yml/badge.svg" /></a>
<a href="https://github.com/melonDS-emu/melonDS/actions/workflows/build-bsd.yml?query=event%3Apush"><img src="https://github.com/melonDS-emu/melonDS/actions/workflows/build-bsd.yml/badge.svg" /></a>
</p>
DS emulator, sorta

The goal is to do things right and fast, akin to blargSNES (but hopefully better). But also to, you know, have a fun challenge :)
<hr>

## About this fork

This fork of [melonDS](https://github.com/melonDS-emu/melonDS) adds online play: the local wireless multiplayer of DS games, played over the internet by two players, each on their own computer with their own copy of the game. The license is unchanged (GPL version 3 or later, see [Licenses](#licenses)).

**What it adds**

- **Online play with rollback.** Each computer runs both players' DS consoles, and only button presses and touches cross the internet. melonDS does not wait for the other player's input: it assumes they still hold what they last held, and when their real input arrives and differs, it restores the game as it was a few frames back and runs those frames again with the right input. This is *rollback*; melonDS uses [GGPO](https://github.com/pond3r/ggpo) for it. To play, open a game, then System > Multiplayer > Play online (rollback)...
- **Meeting through a Nakama server.** [Nakama](https://heroiclabs.com/nakama/) is an open-source game server. melonDS uses one to find the other player and to set up a direct connection between the two computers; the game itself never goes through the server. Without a server, two players can still connect directly when the host's UDP port can be reached.
- **Game channels**: one per game, showing who is there, with chat and challenges between players. They need the fork's channel module, Lua code that the Nakama server runs (see below).
- **Quick match**: the server's matchmaker pairs two players of the same game, whether they ask from melonDS or from the [Nakama Matchmaker](https://github.com/RollinJay/Nakama-Matchmaker) launcher. It works on any Nakama server.
- **The same ROM on both sides.** Both players need the same dump of the game, not just the same game: melonDS compares hashes of the two ROMs and refuses to start otherwise. A game's *online identity*, a hash of its ROM and of the online protocol's version, is what matchmakers pair players on (`--netplay-identity` prints it).
- **Starting from a launcher.** A launcher can start melonDS signed in to the launcher's server and account, with a *server profile* file (`--netplay-config`), or hand melonDS a match it has already found (the `NETPLAY_CONNECT` environment variable).
- **The channel module** for Nakama, in [src/net/rollback/server](src/net/rollback/server/), under the MIT license rather than melonDS's GPL.

**Limits**: two players; DS mode only, with the software renderer and without the JIT during a session; no DS Download Play or Wi-Fi Connection; no relay server, so two players whose networks both prevent direct connections (symmetric NAT, some carrier-grade NAT) cannot play together.

**With the other projects**: [Nakama Matchmaker](https://github.com/RollinJay/Nakama-Matchmaker) is a launcher for this fork, the [mGBA fork](https://github.com/RollinJay/mgba) and [IKEMEN GO's Nakama fork](https://github.com/RollinJay/IkemenGo-Nakama). It finds matches and starts melonDS with them, and it can run a community server for DS games that has the channel module. melonDS uses the same connection handshake as IKEMEN GO's Nakama fork, and also plays on servers that run IKEMEN GO's server modules.

**Downloads**: the [nightly release](https://github.com/RollinJay/melonDS-test/releases/tag/nightly) holds builds of `master`, made at 8 PM Pacific time on each night that follows new commits: AppImages for Linux (x86_64, aarch64); Windows (x86_64, arm64); macOS (one app for Intel and Apple silicon); FreeBSD, NetBSD and OpenBSD (x86_64; the program alone, which uses the system's Qt 6 and other libraries). A nightly is published only when the Linux x86_64, Windows x86_64 and macOS builds succeed; another build that failed is left out, and the release notes name it. These are automatic builds, not stable releases; `SHA256SUMS.txt` lists their checksums. To build it yourself, follow [BUILD.md](./BUILD.md); the online play code is always built in.

**Details**: [docs/NETPLAY.md](docs/NETPLAY.md) explains how it works, how to play through a server, by direct connection and from a launcher, the command-line options, how to run a server, the limits, and measured speed.

## How to use

Firmware boot (not direct boot) requires a BIOS/firmware dump from an original DS or DS Lite.
DS firmwares dumped from a DSi or 3DS aren't bootable and only contain configuration data, thus they are only suitable when booting games directly.

### Possible firmware sizes

 * 128KB: DSi/3DS DS-mode firmware (reduced size due to lacking bootcode)
 * 256KB: regular DS firmware
 * 512KB: iQue DS firmware

DS BIOS dumps from a DSi or 3DS can be used with no compatibility issues. DSi BIOS dumps (in DSi mode) are not compatible. Or maybe they are. I don't know.

As for the rest, the interface should be pretty straightforward. If you have a question, don't hesitate to ask, though!

## How to build
See [BUILD.md](./BUILD.md) for build instructions.

## TODO LIST

 * better DSi emulation
 * better OpenGL rendering
 * netplay
 * the impossible quest of pixel-perfect 3D graphics
 * support for rendering screens to separate windows
 * emulating some fancy addons
 * other non-core shit (debugger, graphics viewers, etc)

### TODO LIST FOR LATER (low priority)

 * big-endian compatibility (Wii, etc)
 * LCD refresh time (used by some games for blending effects)
 * any feature you can eventually ask for that isn't outright stupid

## Credits

 * Martin for GBAtek, a good piece of documentation
 * Cydrak for the extra 3D GPU research
 * limittox for the icon
 * All of you comrades who have been testing melonDS, reporting issues, suggesting shit, etc

## Licenses

[![GNU GPLv3 Image](https://www.gnu.org/graphics/gplv3-127x51.png)](http://www.gnu.org/licenses/gpl-3.0.en.html)

melonDS is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

### External
* Images used in the Input Config Dialog - see `src/frontend/qt_sdl/InputConfig/resources/LICENSE.md`
