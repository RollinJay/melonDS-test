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

#ifndef NETSOCKET_H
#define NETSOCKET_H

#include <string>
#include <vector>

#include "types.h"

namespace melonDS::RollbackNet
{

// An IPv4 address and port, both in host byte order.
struct NetAddress
{
    u32 IP = 0;
    u16 Port = 0;

    bool IsValid() const { return IP != 0 && Port != 0; }
    std::string IPString() const;       // "a.b.c.d"
    std::string ToString() const;       // "a.b.c.d:port"
    bool operator==(const NetAddress& o) const { return IP == o.IP && Port == o.Port; }
    bool operator!=(const NetAddress& o) const { return !(*this == o); }

    // a dotted IPv4 address
    static bool ParseIP(const std::string& text, u32& ip);
};

// Resolves "host:port", "host" (with defaultPort), "a.b.c.d:port" to an IPv4
// address. Blocks while the name is looked up.
bool ResolveAddress(const std::string& hostport, u16 defaultPort, NetAddress& out);

// The IPv4 addresses of this computer's network interfaces that are up,
// loopback excepted, sorted.
std::vector<u32> LocalIPv4Addresses();

// A non-blocking UDP socket on every interface.
class UdpSocket
{
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // port 0 binds a free port
    bool Open(u16 port, std::string& error);
    void Close();
    bool IsOpen() const;
    u16 LocalPort() const { return Port; }

    // false if the system did not take the packet (it counts as lost)
    bool SendTo(const void* data, int len, const NetAddress& to);
    // the next waiting datagram: its length (copied to buffer, cut at maxlen),
    // or -1 when none is waiting
    int RecvFrom(void* buffer, int maxlen, NetAddress& from);
    // waits up to timeoutMs for a datagram (true: one is waiting)
    bool WaitReadable(int timeoutMs);

private:
#ifdef _WIN32
    using Handle = uintptr_t;
#else
    using Handle = int;
#endif
    Handle Sock;
    u16 Port = 0;
};

}

#endif // NETSOCKET_H
