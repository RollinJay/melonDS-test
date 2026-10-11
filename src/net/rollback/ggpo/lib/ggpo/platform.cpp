/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include <chrono>
#include <random>
#include <string>
#include <thread>

#include "types.h"

#ifdef _WIN32
#  include <process.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#endif

int Platform::GetProcessID()
{
#ifdef _WIN32
   return _getpid();
#else
   return (int)getpid();
#endif
}

void Platform::AssertFailed(const char *msg)
{
   // melonDS: an assertion fails the call into the library instead of ending
   // the program (see ASSERT in types.h and the API functions in main.cpp)
   throw GGPOAssertion(msg);
}

unsigned int Platform::GetCurrentTimeMS()
{
   using namespace std::chrono;
   static const steady_clock::time_point start = steady_clock::now();
   return (unsigned int)duration_cast<milliseconds>(steady_clock::now() - start).count();
}

void Platform::SleepMS(int ms)
{
   std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

uint32_t Platform::Random()
{
   // one generator per thread: sessions may run on different threads
   thread_local std::mt19937 rng([] {
      std::random_device rd;
      std::seed_seq seq{ rd(), rd(), (unsigned)GetProcessID(),
                         (unsigned)std::chrono::steady_clock::now().time_since_epoch().count() };
      return std::mt19937(seq);
   }());
   return (uint32_t)rng();
}

static const char *ConfigValue(const char *name)
{
   std::string env;
   for (const char *p = name; *p; p++) {
      env += (*p == '.') ? '_' : (char)toupper((unsigned char)*p);
   }
   return getenv(env.c_str());
}

int Platform::GetConfigInt(const char *name)
{
   const char *v = ConfigValue(name);
   return v ? atoi(v) : 0;
}

bool Platform::GetConfigBool(const char *name)
{
   const char *v = ConfigValue(name);
   return v && (atoi(v) != 0 || strcmp(v, "true") == 0);
}

#ifdef _WIN32
static bool InitWinsock()
{
   static const bool ok = [] {
      WSADATA data;
      return WSAStartup(MAKEWORD(2, 2), &data) == 0;
   }();
   return ok;
}
#endif

GGPOSocket Platform::CreateUdpSocket(uint16_t port)
{
#ifdef _WIN32
   if (!InitWinsock()) {
      return GGPO_INVALID_SOCKET;
   }
#endif
   GGPOSocket s = socket(AF_INET, SOCK_DGRAM, 0);
   if (s == GGPO_INVALID_SOCKET) {
      return s;
   }

#ifdef _WIN32
   u_long nonblocking = 1;
   ioctlsocket(s, FIONBIO, &nonblocking);
   // Without this, an ICMP "port unreachable" from a peer that is not listening
   // yet makes the next recvfrom fail with WSAECONNRESET.
   BOOL report = FALSE;
   DWORD unused = 0;
   WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &report, sizeof report,
            NULL, 0, &unused, NULL, NULL);
#else
   fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif

   sockaddr_in sin;
   memset(&sin, 0, sizeof sin);
   sin.sin_family = AF_INET;
   sin.sin_addr.s_addr = htonl(INADDR_ANY);
   sin.sin_port = htons(port);
   if (bind(s, (sockaddr *)&sin, sizeof sin) != 0) {
      CloseSocket(s);
      return GGPO_INVALID_SOCKET;
   }
   return s;
}

void Platform::CloseSocket(GGPOSocket s)
{
#ifdef _WIN32
   closesocket(s);
#else
   close(s);
#endif
}

int Platform::SendTo(GGPOSocket s, const char *buffer, int len, const struct sockaddr_in *to)
{
   int res = (int)sendto(s, buffer, len, 0, (const sockaddr *)to, sizeof *to);
   return res < 0 ? -1 : res;
}

int Platform::RecvFrom(GGPOSocket s, char *buffer, int len, struct sockaddr_in *from)
{
   for (;;) {
#ifdef _WIN32
      int fromlen = sizeof *from;
#else
      socklen_t fromlen = sizeof *from;
#endif
      int res = (int)recvfrom(s, buffer, len, 0, (sockaddr *)from, &fromlen);
      if (res >= 0) {
         return res;
      }
#ifdef _WIN32
      int err = WSAGetLastError();
      if (err == WSAECONNRESET || err == WSAEMSGSIZE) {
         continue;   // an ICMP error, or a datagram larger than any GGPO sends
      }
#else
      if (errno == EINTR) {
         continue;
      }
#endif
      return -1;
   }
}
