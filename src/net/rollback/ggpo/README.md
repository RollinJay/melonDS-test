# GGPO (vendored)

This is GGPO, the rollback networking library, from
https://github.com/pond3r/ggpo at commit `7ddadef8546a7d99ff0b3530c6056bc8ee4b9c0a`
(MIT license, see `LICENSE`). melonDS's online play (`src/net/rollback`, see
`docs/NETPLAY.md`) uses its peer-to-peer backend.

Only the files melonDS builds are here: the peer-to-peer backend, the
synchronization layer and the UDP protocol. They were changed as follows.

## Changes

**Portable.** Upstream builds only on Windows. `lib/ggpo/platform.h/.cpp` now
cover Windows, Linux and macOS: time, sleeping, the process id, random
numbers, and the UDP socket GGPO opens when it is not given a transport.
Windows-only calls were replaced (`sprintf_s`, `strcpy_s`, `strncat_s`,
`Sleep`, `S_un`, `ASSERT(FALSE)`), `poll.h/.cpp` lost their Windows event
handles (nothing registered one), and `ggponet.h` defines `__cdecl` away on
other systems.

**A transport of the application's.** `ggpo_start_session_ex` takes a
`GGPOSessionOptions`: a `GGPOTransport` (two callbacks that send and receive
GGPO's packets), the local port when GGPO opens its own socket, and a
prediction limit below `GGPO_MAX_PREDICTION_FRAMES`. melonDS sends GGPO's
packets over the same UDP socket as the rest of a session's traffic.
`ggpo_start_session` still works as before.

**Failures return instead of ending the program.** A failed `ASSERT` throws
`GGPOAssertion`; every API function catches it (and any exception from the
application's callbacks) and returns `GGPO_ERRORCODE_GENERAL_FAILURE`, after
which the session is closed.

**Packets from the network are checked.** A datagram that is not a complete
message of a known type is dropped before anything reads it
(`UdpMsg::IsValid`). An input packet whose input size, frame numbers or bit
stream do not fit is dropped too, where upstream would read or write past
its buffers. Two assertions that a misbehaving peer could trigger became log
messages.

**Sizes.** `GAMEINPUT_MAX_BYTES` is 4 (one player's DS input) and
`GAMEINPUT_MAX_PLAYERS` 4.

**Initialization.** `UdpProtocol` members read before they were set (round
trip time, kbps, last receive time, state) and `Sync`'s saved frames start
initialized.

**Logging** is off unless the environment sets `GGPO_LOG=1` (written to
`ggpo-<pid>.log`; `GGPO_LOG_TIMESTAMPS=1` adds times). When it is off,
nothing is formatted. The test settings read from the environment remain:
`GGPO_NETWORK_DELAY` (milliseconds added to outgoing packets) and
`GGPO_OOP_PERCENT` (out-of-order packets).

**Whitespace:** spaces at line ends were removed.

**Removed:** the synctest and spectator backends with their API functions
(`ggpo_start_synctest`, `ggpo_start_spectating`), `DllMain`, and the
unfinished `ggpo_client_chat`. melonDS has its own synchronization test
(`melonDS-netplay-tool synctest`).

## Wire format

GGPO sends its message structures as they are in memory, so both sides must
agree on byte order and bit-field layout. All of melonDS's platforms
(x86-64, ARM64; GCC, Clang, MinGW) are little endian and lay the bit fields
of `UdpMsg::connect_status` out the same way.
