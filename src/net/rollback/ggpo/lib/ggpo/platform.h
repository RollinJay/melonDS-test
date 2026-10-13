/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

/*
 * melonDS: one platform layer for Windows, Linux and macOS, replacing
 * platform_windows.* and the unfinished platform_linux.*.
 */

#ifndef _GGPO_PLATFORM_H_
#define _GGPO_PLATFORM_H_

#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET GGPOSocket;
#  define GGPO_INVALID_SOCKET INVALID_SOCKET
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
typedef int GGPOSocket;
#  define GGPO_INVALID_SOCKET (-1)
#endif

class Platform {
public:  // functions
   static int GetProcessID();
   static void AssertFailed(const char *msg);
   static unsigned int GetCurrentTimeMS();
   static void SleepMS(int ms);

   // Random numbers for the connection's magic number and sync requests (not
   // for anything that needs to be unpredictable).
   static uint32_t Random();

   // Settings for diagnostics, read from the environment: a name like
   // "ggpo.network.delay" is read from GGPO_NETWORK_DELAY.
   static int GetConfigInt(const char *name);
   static bool GetConfigBool(const char *name);

   // The UDP socket GGPO opens when the application gives it no transport:
   // non-blocking, bound to port on every interface. Returns
   // GGPO_INVALID_SOCKET on failure.
   static GGPOSocket CreateUdpSocket(uint16_t port);
   static void CloseSocket(GGPOSocket s);
   // Returns the number of bytes sent, or -1.
   static int SendTo(GGPOSocket s, const char *buffer, int len, const struct sockaddr_in *to);
   // Returns the length of the next waiting datagram, copied to buffer, or -1
   // when none is waiting (or the socket failed).
   static int RecvFrom(GGPOSocket s, char *buffer, int len, struct sockaddr_in *from);
};

#endif
