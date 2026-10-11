# KCP (vendored)

`ikcp.c` and `ikcp.h` are KCP, a reliable transport over UDP, from
https://github.com/skywind3000/kcp at commit
`b1a7a2101dcbb96017681a500d6b82bbe5a88766`, unchanged except for whitespace
(LF line endings instead of CRLF, no spaces at line ends). MIT license, see
`LICENSE`.

melonDS's online play uses it for the session stream: the messages two
computers exchange to set a session up (versions, the ROM's hash, settings,
saves) and during play (desync checks, chat). See `P2PLink.cpp` and
`docs/NETPLAY.md`.
