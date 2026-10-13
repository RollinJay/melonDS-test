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

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#include "NetSocket.h"

namespace melonDS::RollbackNet
{

#ifdef _WIN32
static const uintptr_t kInvalid = (uintptr_t)INVALID_SOCKET;

static bool InitSockets()
{
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
#else
static const int kInvalid = -1;
static bool InitSockets() { return true; }
#endif

std::string NetAddress::IPString() const
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (IP >> 24) & 0xFF, (IP >> 16) & 0xFF, (IP >> 8) & 0xFF, IP & 0xFF);
    return buf;
}

std::string NetAddress::ToString() const
{
    return IPString() + ":" + std::to_string(Port);
}

bool NetAddress::ParseIP(const std::string& text, u32& ip)
{
    unsigned a, b, c, d;
    char extra;
    if (sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    ip = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

bool ResolveAddress(const std::string& hostport, u16 defaultPort, NetAddress& out)
{
    std::string host = hostport;
    u16 port = defaultPort;
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos)
    {
        host = hostport.substr(0, colon);
        int p = atoi(hostport.c_str() + colon + 1);
        if (p <= 0 || p > 65535)
            return false;
        port = (u16)p;
    }
    if (host.empty() || port == 0)
        return false;

    u32 ip;
    if (NetAddress::ParseIP(host, ip))
    {
        out.IP = ip;
        out.Port = port;
        return true;
    }

    if (!InitSockets())
        return false;

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
        return false;

    bool found = false;
    for (addrinfo* ai = res; ai; ai = ai->ai_next)
    {
        if (ai->ai_family == AF_INET && ai->ai_addr)
        {
            const sockaddr_in* sin = (const sockaddr_in*)ai->ai_addr;
            out.IP = ntohl(sin->sin_addr.s_addr);
            out.Port = port;
            found = true;
            break;
        }
    }
    freeaddrinfo(res);
    return found;
}

std::vector<u32> LocalIPv4Addresses()
{
    std::vector<u32> out;

#ifdef _WIN32
    if (!InitSockets())
        return out;

    ULONG size = 16 * 1024;
    std::vector<u8> buf;
    ULONG ret = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && ret == ERROR_BUFFER_OVERFLOW; tries++)
    {
        buf.resize(size);
        ret = GetAdaptersAddresses(AF_INET,
                                   GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                   nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (ret == NO_ERROR)
    {
        for (IP_ADAPTER_ADDRESSES* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next)
            {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                u32 ip = ntohl(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
                if ((ip >> 24) != 127 && ip != 0)
                    out.push_back(ip);
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0)
    {
        for (ifaddrs* a = list; a; a = a->ifa_next)
        {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
                continue;
            if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK))
                continue;
            u32 ip = ntohl(((sockaddr_in*)a->ifa_addr)->sin_addr.s_addr);
            if ((ip >> 24) != 127 && ip != 0)
                out.push_back(ip);
        }
        freeifaddrs(list);
    }
#endif

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

UdpSocket::UdpSocket() : Sock(kInvalid)
{
}

UdpSocket::~UdpSocket()
{
    Close();
}

bool UdpSocket::IsOpen() const
{
    return Sock != kInvalid;
}

bool UdpSocket::Open(u16 port, std::string& error)
{
    Close();
    if (!InitSockets())
    {
        error = "The network could not be initialized.";
        return false;
    }

    Sock = (Handle)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (Sock == kInvalid)
    {
        error = "Could not create a UDP socket.";
        return false;
    }

#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket((SOCKET)Sock, FIONBIO, &nonblocking);
    // otherwise an ICMP "port unreachable" (a peer not listening yet) makes the
    // next recvfrom fail
    BOOL report = FALSE;
    DWORD unused = 0;
    WSAIoctl((SOCKET)Sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &report, sizeof report,
             nullptr, 0, &unused, nullptr, nullptr);
#else
    fcntl(Sock, F_SETFL, fcntl(Sock, F_GETFL, 0) | O_NONBLOCK);
#endif

    // room for bursts (a save sent at session start, GGPO's resends)
    int bufsize = 1 << 20;
    setsockopt(Sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof bufsize);
    setsockopt(Sock, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsize, sizeof bufsize);

    sockaddr_in sin {};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(port);
    if (bind(Sock, (sockaddr*)&sin, sizeof sin) != 0)
    {
        error = "Could not use UDP port " + std::to_string(port) + " (is another program using it?).";
        Close();
        return false;
    }

    sockaddr_in bound {};
#ifdef _WIN32
    int len = sizeof bound;
#else
    socklen_t len = sizeof bound;
#endif
    if (getsockname(Sock, (sockaddr*)&bound, &len) == 0)
        Port = ntohs(bound.sin_port);
    else
        Port = port;
    return true;
}

void UdpSocket::Close()
{
    if (Sock == kInvalid)
        return;
#ifdef _WIN32
    closesocket((SOCKET)Sock);
#else
    close(Sock);
#endif
    Sock = kInvalid;
    Port = 0;
}

bool UdpSocket::SendTo(const void* data, int len, const NetAddress& to)
{
    if (Sock == kInvalid || !to.IsValid())
        return false;
    sockaddr_in sin {};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(to.IP);
    sin.sin_port = htons(to.Port);
    return sendto(Sock, (const char*)data, len, 0, (const sockaddr*)&sin, sizeof sin) == len;
}

int UdpSocket::RecvFrom(void* buffer, int maxlen, NetAddress& from)
{
    if (Sock == kInvalid)
        return -1;
    for (;;)
    {
        sockaddr_in sin {};
#ifdef _WIN32
        int len = sizeof sin;
#else
        socklen_t len = sizeof sin;
#endif
        int res = (int)recvfrom(Sock, (char*)buffer, maxlen, 0, (sockaddr*)&sin, &len);
        if (res >= 0)
        {
            from.IP = ntohl(sin.sin_addr.s_addr);
            from.Port = ntohs(sin.sin_port);
            return res;
        }
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAECONNRESET || err == WSAEMSGSIZE)
            continue;
#else
        if (errno == EINTR)
            continue;
#endif
        return -1;
    }
}

bool UdpSocket::WaitReadable(int timeoutMs)
{
    if (Sock == kInvalid)
        return false;
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET((SOCKET)Sock, &set);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(0, &set, nullptr, nullptr, &tv) > 0;
#else
    pollfd pfd {};
    pfd.fd = Sock;
    pfd.events = POLLIN;
    return poll(&pfd, 1, timeoutMs) > 0;
#endif
}

}
