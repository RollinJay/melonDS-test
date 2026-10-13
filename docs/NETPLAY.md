# Online play with rollback

melonDS can play DS multiplayer games over the internet. Each player uses their
own computer and their own copy of the game; the game's local wireless
multiplayer (DS Download Play excepted, see [Limits](#limits)) works as if the
DS consoles sat next to each other. This document explains how it works, how
to play, how to run a server, what it cannot do, and how fast it runs.

## How it works

### Every computer runs every DS

A DS multiplayer game is several DS consoles talking to each other by radio.
melonDS cannot send that radio traffic across the internet as it happens: the
consoles exchange messages hundreds of times a second and expect answers
within a fraction of a millisecond, far less than the time a packet takes to
cross the internet.

So each computer runs **all** the players' consoles, linked by an emulated
radio, as one *machine*. Only the players' button presses and touches cross
the internet. If every computer starts its machine identically and feeds it
the same inputs, every computer computes the same game; melonDS makes sure of
that. The consoles advance in steps of 32 microseconds of emulated time, and
what one sends over the radio during a step reaches the others when the step
ends, so nothing depends on how fast a computer happens to run; and
everything that could differ between computers, such as the clock and the
firmware settings, comes from the session. Within a step the consoles do not
depend on each other, so melonDS runs them on separate processor cores.

### Only your console is drawn and heard

Running two DS consoles costs more than running one, but only one of them is
on your screen. The others run *headless*: melonDS skips drawing their
pictures and producing their sound, except for the parts of the picture a
game reads back (display capture). The emulation itself is complete, so the
headless consoles behave exactly like the shown one. In a 3D test game, a
headless console took 1.1 ms per frame where a drawn one took 2.4 ms. When a
processor core is left over, your console's 3D picture is drawn on that core
while the consoles go on running (also the picture needed right after a
rollback, described next).

### Rollback hides the delay

Your inputs reach the other player tens of milliseconds after you press. With
*rollback*, melonDS does not wait for them: it runs the next frame at once,
assuming the other player still holds what they last held. When their real
input arrives and differs, melonDS restores the machine as it was at that
frame (a *rollback*), runs the frames since then again with the right input,
and shows the corrected result. Nobody sees the frames run again, so every
console runs them headless, yours included. Most of the time the guess is
right and nothing visible happens; when it is wrong, the game jumps a few
frames.

Two settings shape this:

- **Input delay**: your inputs take effect this many frames after you press,
  on both computers. Each frame of delay is a frame less to roll back. By
  default melonDS picks it from the round trip between the two computers.
- **Rollback limit**: how many frames melonDS may run ahead of the other
  player's known inputs (1 to 8, the most GGPO allows). At the limit it waits
  instead. A rollback of N frames means running N frames again within one
  frame's time, so a slow computer needs a lower limit (see [Speed](#speed)
  for measured costs). By the arithmetic (not measured), the automatic input
  delay (up to 6 frames) and a limit of 8 keep the game from waiting up to a
  round trip of about 400 ms, at the price of rollbacks up to 6 frames deep.

melonDS uses [GGPO](https://github.com/pond3r/ggpo) for the rollback logic.

### Finding the other player

Two computers on the internet usually cannot contact each other directly:
home routers block traffic nobody asked for (*NAT*). melonDS opens the
connection by *hole punching*: both computers send packets to each other at
the same time, so that each router sees an answer to its own traffic. To do
that, each computer first needs to know where the other can be reached; it
learns its own public address from a *STUN server*, and the two exchange
addresses through a *signaling server*.

melonDS uses a [Nakama](https://heroiclabs.com/nakama/) server for signaling,
and for the lobby around it: *game channels* in the manner of Fightcade, one
per game, where players see who is there, chat, and challenge each other.
The server never sees the game: once two players are paired, everything goes
directly between their computers. The same arrangement is used by
[IKEMEN GO's Nakama layer](https://github.com/RollinJay/IkemenGo-Nakama), and
the two share the hole-punching handshake.

Without a server, a *direct connection* also works when the host can be
reached on its UDP port (port forwarded on its router, or both on the same
network).

### What both players need

- **The same ROM.** Not just the same game: the same dump. melonDS compares a
  hash of the two files and refuses to start otherwise. ROMs are never sent.
- **A compatible melonDS.** Both must speak the same online protocol and
  build identical consoles; melonDS compares a hash of the two machines
  before the game starts.
- Nothing else: the session uses melonDS's built-in BIOS and firmware for
  every console, so no BIOS files are needed. Each player's console gets that
  player's firmware settings (birthday, language, color, message) on every
  computer, and as its nickname the name that player uses online.

### Games tested

Besides small test programs, two commercial games were played through their
local wireless multiplayer in sessions, from the console's start-up on:

- **Mario Kart DS** (Europe): Multiplayer, a group created on one console and
  joined from the other (multi-card play), the game settings, a cup and a
  course, and a VS race.
- **Jump Ultimate Stars** (Japan, with the English fan translation): the
  game's opening on first start, DS Wireless Battle with one player as leader,
  decks, stage and rules, and a battle.

For both games, three checks found no difference:

- Rolling back every frame during the race and the battle, and every 2 to 4
  frames from start-up on, gave the same game as not rolling back.
- Two machines in one program, standing in for the two players' computers
  (each showing its own player's console, getting the other player's inputs
  up to 8 frames late, guessing them and rolling back as GGPO does), stayed
  identical from start-up through the race and the battle, also with some of
  the guesses made at random (about 1,000 rollbacks per computer in Mario
  Kart DS, 2,900 in Jump Ultimate Stars), and ended exactly where a run
  without rollback ends.
- Two programs played against each other from start-up to the race and the
  battle over a link with about 60 ms of round trip (rollbacks up to 7 frames
  deep), comparing their machines every second.

That is no guarantee for other games, which may use parts of the hardware
these do not; a game that is not reproducible ends its session with a desync
message (see [Limits](#limits)).

### Saves

Each player's console starts from that player's save, which melonDS sends to
the other computers at the start (games such as Pokémon need each player's
own save to trade or battle). By default nothing is written back: online play
leaves your save file as it was. The setting *Keep what the game saves during
online play* writes the save your console ended the session with into your
save file when the session ends.

## Playing

Open the game you want to play (File > Open ROM...), then
System > Multiplayer > Play online (rollback)... The window that opens has
the game channels on the left, the channel you are in on the right, and the
current game at the bottom.

### Through a server

1. Settings...: enter the server's address, port (usually 7350) and key, as
   the server's operator gives them. Pick an online name and, if you like, a
   region.
2. Connect. melonDS opens the channel of the game you loaded. The list on
   the left shows the other channels and how many players are in each;
   double-click one to join it (to play in a channel, its game must be the
   one you loaded).
3. In a channel:
   - *Looking to play* tells the others you want a game.
   - Select a player and click *Challenge* (or double-click the player). They
     have 30 seconds to accept. The challenger hosts.
   - *Quick match* pairs you with the next player who asks for one.
4. Once paired, both computers connect, compare their ROMs, exchange saves
   and start. The game appears in the main window; your controls work as
   usual. The window title shows the ping, the input delay and the depth of
   the last rollback.
5. *Leave the game* ends the session for both players.

### From a launcher

A launcher, such as Nakama Matchmaker, can start melonDS on its own server and
account, so that the player needs no settings:

```sh
melonDS --netplay-config servers.json --netplay-online --netplay-quick --netplay-exit game.nds
```

`--netplay-config` names a file of server profiles in the format of IKEMEN
GO's `servers.json` (host, port, server key, TLS, device id, name, STUN
servers); `--netplay-server ID` picks one of its profiles instead of its
default. melonDS uses that profile in place of the online settings for this
run only and saves none of it; the device id makes it the launcher's account.
`--netplay-online` opens the online window and connects, which joins the
loaded game's channel; `--netplay-quick` then asks for a quick match there.

### Direct connection

One player hosts: Direct connection..., *Host*, a UDP port that the other
player can reach, and a session code (any word). The other player joins:
*Join*, the host's address and port (for example `203.0.113.5:7064`), and the
same session code.

The same from the command line, for scripts and external lobbies:

```sh
melonDS --netplay-host 7064 --netplay-code word game.nds
melonDS --netplay-join 203.0.113.5:7064 --netplay-code word game.nds
```

`--netplay-delay N` sets the input delay when hosting, and `--netplay-exit`
quits melonDS when the session ends.

### During a session

The game takes no orders but the players' inputs, so that both computers stay
identical: pause, reset, frame step, fast forward, savestates, cheats, RAM
editing and loading another game are unavailable until the session ends. The
lid stays open and the microphone hears silence. When the session ends, the
game you had loaded before comes back, paused.

### Settings

| Setting | Meaning |
|---|---|
| Server address, port, key, TLS | The Nakama server to connect to. |
| Online name | Your name in channels and in a game's chat (your firmware nickname if empty). Your console also uses it as its nickname, which games show to the other players; the DS keeps only its first 10 characters. |
| Region | Shown next to your name, so players can pick nearby opponents. |
| STUN servers | Servers that tell your computer its public address (comma separated). |
| UDP port | The port melonDS prefers for games; it only matters to players who forward it and to direct hosts. |
| Input delay | Automatic, or a number of frames. The host's applies to both players. |
| Rollback limit | 1 to 8 frames; the lower of the two players' applies. |
| Keep what the game saves | See [Saves](#saves). |

## Running a server

`src/net/rollback/server/` holds the server module (`melonds_channel.lua`), a
configuration example and the setup steps (README.md). A server needs Nakama
and PostgreSQL, and only TCP port 7350 open to the players.

## Limits

- **Two players online.** The machine and GGPO support up to four players,
  but connecting, channels and the session setup are for two.
- **Cost per player.** Every computer emulates every player's DS, and a
  rollback runs frames of every console again within one frame. See
  [Speed](#speed) for what that costs.
- **DS mode only**, with the software renderer and without the JIT for the
  session's consoles (the JIT and threaded rendering cannot be rolled back
  exactly). The main window's renderer settings apply again afterwards.
- **No DS Download Play, no Wi-Fi Connection, no GBA slot accessories, no
  microphone**: the session's consoles only reach each other.
- **No relay.** When both players are behind strict NATs (symmetric NAT, or
  some carrier-grade NATs), hole punching fails after about 20 seconds with
  "Could not open a connection to the other player". A relay (TURN) server
  would be needed; none is implemented.
- **No spectators** and no replays yet.
- **Desyncs are detected, not repaired.** Every 60 frames, once a frame can
  no longer be rolled back, both computers compare a hash of their whole
  machine. If they differ (a bug: some part of the emulation is not exactly
  reproducible), the session ends with a message naming the frame.
- Saves larger than 32 MB (DSi-style NAND saves) are not supported.

## Speed

A frame lasts 16.7 ms. Measured with `melonDS-netplay-tool bench` on a
2-core cloud machine (Intel Xeon at 2.1 GHz), two consoles on two threads:

| | Mario Kart DS race | Jump Ultimate Stars battle |
|---|---|---|
| A frame without rollback | 12.5 ms | 8 ms |
| A frame with a rollback of 1 frame | 25 ms | 16 ms |
| Each further frame rolled back | 8 ms | 7 ms |

On that machine every rollback makes its frame late in Mario Kart DS, and a
rollback of 1 frame just fits in Jump Ultimate Stars. A late frame is made
up by the frames after it while they have time to spare (4 ms each in that
race); when rollbacks come more often than that allows, both players' games
run slower than normal, since GGPO keeps the two computers in step. A faster
processor fits deeper rollbacks. A third core helps too: your console's 3D
image is then drawn on it (about 3.5 ms per frame in that race), including
the one needed right after a rollback.

Most of a rollback's cost is running the consoles again. melonDS interprets
the DS's processors there; its faster JIT recompiler cannot be used, because
a JIT that is rolled back does not give exactly the same results.

## For developers

### Where things are

| Path | What |
|---|---|
| `src/net/rollback/NetplayMachine.*` | The machine: the players' consoles, run in rounds on several threads, saved and restored as one state. |
| `src/net/rollback/NetplayBus.*` | The emulated radio link between the machine's consoles. |
| `src/net/rollback/RollbackSession.*` | GGPO driving the machine: callbacks, buffers, desync hashes. |
| `src/net/rollback/P2PLink.*`, `NetSocket.*`, `NetJson.*` | The connection: STUN, hole punching, one UDP socket shared by the session stream (KCP), GGPO and keepalives. |
| `src/net/rollback/OnlineSession.*` | A session from an open connection to the end of play: handshake, settings, saves, start, checks. |
| `src/net/rollback/ggpo/`, `kcp/` | Vendored GGPO (changed, see its README) and KCP. |
| `src/net/rollback/server/` | The Nakama channel module. |
| `src/net/rollback/tool/` | `melonDS-netplay-tool` (build with `-DBUILD_NETPLAY_TOOL=ON`). |
| `src/frontend/qt_sdl/NetplayOnlineDialog.*`, `NakamaClient.*` | The online window and the Nakama client. |
| `src/frontend/qt_sdl/EmuThread.cpp` (`netplay*`) | Running a session in place of the normal emulation. |

### Determinism

Rollback needs two things from the emulator: a savestate that captures
everything (loading it and running gives exactly what running from the
original did), and two machines built from the same inputs being identical.
The changes to the core for this:

- Savestates have a *rollback* mode (`Savestate::RollbackState`, in memory
  only). It also saves what a normal savestate leaves out or recomputes
  approximately on load: the CPUs' code and data timing state (`AR9R`,
  `AR7R`), the divider's and square root unit's operands and results
  (`NDSR`), the firmware flash's contents, which games write to (`SPFR`),
  and the renderers' state carried from one frame to the next (`GPUR`,
  including a 3D image a headless console postponed, or one being drawn on
  its own thread); the pending interrupt flags are recomputed from the
  registers. It leaves out what cannot matter: the 12 MB of main RAM melonDS
  reserves beyond the DS's 4 MB, the cart checksum, the timing tables (unless
  the registers they derive from changed), and the 3D vertices and polygons
  no longer in use. Loads went from 13 ms to under 1 ms per console. What it
  keeps must also be the same on both computers, since their hashes are
  compared: memory a rollback did not restore can still hold data from the
  frames it undid, so the parts of the 3D polygon and vertex lists that
  nothing reads (entries past a polygon's own vertices, vertices no polygon
  uses) are saved as zeroes.
- `NDS::RunFrame` is split into `BeginFrame`, `RunFrameSlice` and `EndFrame`,
  so that the machine can run its consoles in rounds of 32 µs while a radio
  is on (the steps of [Every computer runs every DS](#every-computer-runs-every-ds)). What a console sends during a round reaches the others at its end
  (`NetplayBus::Commit`, in emulated time order), which keeps a CMD frame
  and its replies within the 112 µs the host waits, and makes the consoles
  of a round independent: they run on one thread per console
  (`NetplayMachine::SetThreadCount`, `Netplay.Threads` in the frontend's
  configuration; the result is the same for any number).
- `Wifi` has a deterministic multiplayer mode: replies to a host's CMD frame
  are collected at a fixed point in emulated time instead of after a real
  timeout, and buffers sent over the radio are zero-filled.
- Headless drawing keeps the 2D units' own state as drawing would (the
  windows' horizontal state, `GPU2D::SkipScanline`), the protection unit's
  maps follow its registers exactly (`ARMv5::PUMapsFollowRegisters`), and
  fields that savestates held without initializing (the firmware chip's
  address register, the high-resolution position of a vertex that clipping
  made with W = 0) are reset.
- The last frame of a rollback renders the 3D image the next shown frame
  displays (`NetplayMachine::RunFrame`'s `beforeShown`, from
  `RollbackSession`), on its own thread when a core is free; otherwise that
  first frame after the rollback would render two images.
- `melonDS-netplay-tool synctest` rolls back every frame and checks the
  result (with `--shot`, pictures of frames shown right after a rollback);
  `twins` builds two machines in one process and compares them (catching
  uninitialized state); `pair` runs two computers of a session in one
  process, each guessing the other's late inputs and rolling back as GGPO
  does, and compares them (catching what a synctest cannot: data left from
  frames a rollback undid, as with the 3D lists above); `locate` runs a frame
  on and again after a load and names the first round and savestate sections
  that differ (how the divider and firmware omissions were found); `play`
  writes pictures and states for preparing input scripts; `bench` measures
  the costs in [Speed](#speed) (`--pattern` as a session runs frames,
  with where the time goes); `online` plays a whole session between two
  processes, with `GGPO_NETWORK_DELAY` and `MELONDS_NETPLAY_LOSS` (percent)
  to add latency and packet loss.

### Wire formats

The connection uses one UDP socket. The first byte of each datagram says what
it carries, as in IKEMEN GO's Nakama layer: `{` the hole-punching handshake
(JSON: `IKEMEN-P2P/1` hello and ack carrying a random token per side),
`0xA0` control (keepalive, ping, pong, bye), `0xA1` the session stream (KCP,
conversation id derived from both tokens), `0xA2` GGPO. STUN answers start
with a byte below `0x40`.

The session stream carries length-prefixed messages: hello (protocol version,
ROM hash, console settings, save size and hash, preferences), settings (from
the host), save chunks, ready (machine hash), then desync hashes, chat and
bye. A player's input is 4 bytes: 12 buttons, a touch flag and the touch
position.
