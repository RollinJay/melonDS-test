/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "types.h"
#include "udp.h"

Udp::Udp() :
   _socket(GGPO_INVALID_SOCKET),
   _use_transport(false),
   _callbacks(NULL),
   _poll(NULL)
{
   memset(&_transport, 0, sizeof _transport);
}

Udp::~Udp(void)
{
   if (_socket != GGPO_INVALID_SOCKET) {
      Platform::CloseSocket(_socket);
      _socket = GGPO_INVALID_SOCKET;
   }
}

void
Udp::Init(uint16 port, Poll *poll, Callbacks *callbacks)
{
   _callbacks = callbacks;

   _poll = poll;
   _poll->RegisterLoop(this);

   Log("binding udp socket to port %d.\n", port);
   _socket = Platform::CreateUdpSocket(port);
   if (_socket == GGPO_INVALID_SOCKET) {
      Log("could not bind udp socket to port %d.\n", port);
   }
}

void
Udp::Init(const GGPOTransport *transport, Poll *poll, Callbacks *callbacks)
{
   _callbacks = callbacks;
   _transport = *transport;
   _use_transport = true;

   _poll = poll;
   _poll->RegisterLoop(this);
}

void
Udp::SendTo(char *buffer, int len, int flags, struct sockaddr *dst, int destlen)
{
   struct sockaddr_in *to = (struct sockaddr_in *)dst;

   int res;
   if (_use_transport) {
      res = _transport.send_to(_transport.context, buffer, len, to);
   } else if (_socket != GGPO_INVALID_SOCKET) {
      res = Platform::SendTo(_socket, buffer, len, to);
   } else {
      res = -1;
   }
   // melonDS: a failed send is a lost packet, which GGPO recovers from by
   // resending, instead of an assertion.
   if (res < 0) {
      Log("error in sendto (len %d).\n", len);
      return;
   }
   if (LogEnabled()) {
      char dst_ip[64];
      Log("sent packet length %d to %s:%d (ret:%d).\n", len, inet_ntop(AF_INET, (void *)&to->sin_addr, dst_ip, ARRAY_SIZE(dst_ip)), ntohs(to->sin_port), res);
   }
}

bool
Udp::OnLoopPoll(void *cookie)
{
   // melonDS: aligned for the casts to UdpMsg below
   union {
      uint8    bytes[MAX_UDP_PACKET_SIZE];
      uint32   align;
   } recv_buf;
   sockaddr_in    recv_addr;

   if (!_use_transport && _socket == GGPO_INVALID_SOCKET) {
      return true;
   }

   for (;;) {
      memset(&recv_addr, 0, sizeof recv_addr);
      int len;
      if (_use_transport) {
         len = _transport.recv_from(_transport.context, (char *)recv_buf.bytes, MAX_UDP_PACKET_SIZE, &recv_addr);
      } else {
         len = Platform::RecvFrom(_socket, (char *)recv_buf.bytes, MAX_UDP_PACKET_SIZE, &recv_addr);
      }
      if (len < 0) {
         break;
      }

      UdpMsg *msg = (UdpMsg *)recv_buf.bytes;
      // melonDS: a datagram that is not a complete message of a known type
      // (cut short, or not from GGPO) is dropped before anything reads it.
      if (!msg->IsValid(len)) {
         Log("dropping invalid packet (len:%d).\n", len);
         continue;
      }
      if (LogEnabled()) {
         char src_ip[64];
         Log("recvfrom returned (len:%d  from:%s:%d).\n", len, inet_ntop(AF_INET, (void*)&recv_addr.sin_addr, src_ip, ARRAY_SIZE(src_ip)), ntohs(recv_addr.sin_port) );
      }
      _callbacks->OnMsg(recv_addr, msg, len);
   }
   return true;
}


void
Udp::Log(const char *fmt, ...)
{
   if (!LogEnabled()) {
      return;
   }
   char buf[1024];
   size_t offset;
   va_list args;

   snprintf(buf, ARRAY_SIZE(buf), "udp | ");
   offset = strlen(buf);
   va_start(args, fmt);
   vsnprintf(buf + offset, ARRAY_SIZE(buf) - offset - 1, fmt, args);
   buf[ARRAY_SIZE(buf)-1] = '\0';
   ::Log("%s", buf);
   va_end(args);
}
