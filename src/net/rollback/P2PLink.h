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

#ifndef P2PLINK_H
#define P2PLINK_H

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "types.h"
#include "NetSocket.h"
#include "ggponet.h"

struct IKCPCB;

namespace melonDS::RollbackNet
{

// One way to reach this computer: an address of one of its network interfaces
// ("host"), or the address its router shows the internet ("srflx", found
// with a STUN server).
struct P2PCandidate
{
    std::string Type;
    std::string Address;
    u16 Port = 0;
};

// What a player sends the other through the signaling service (a Nakama
// match) to open a connection: who it is, a random token that identifies
// this attempt, and where it can be reached. The JSON form is the one IKEMEN
// GO's Nakama layer uses, so the same server modules relay it.
struct P2PSignal
{
    std::string MatchID;
    std::string Token;
    std::string From;           // Nakama user ids
    std::string To;
    std::vector<P2PCandidate> Candidates;

    std::string ToJson() const;
    // false if json is not a P2P signal
    static bool FromJson(const std::string& json, P2PSignal& out);
};

// A UDP connection to the other player, opened through both players' NATs,
// that carries everything for the rest of the session on one socket:
//
//  - a reliable, ordered stream of messages (KCP), for setting the session up
//    and for messages during play (Send/Receive);
//  - GGPO's packets (Transport);
//  - keepalives, which keep the NAT mappings open, and round-trip pings.
//
// Opening it: in a matched session (StartSignaled) each player gathers its
// candidates, sends them in a signal through the signaling service, and both
// send small "hello" packets to all of the other's candidates until one gets
// through ("hole punching"); the first address a valid hello or its "ack"
// comes from becomes the peer's address. In a direct session (StartHost,
// StartGuest) the guest knows the host's address and the host must be
// reachable on its port (port forwarded, or the same network).
//
// The first byte of every datagram says what it carries (as in IKEMEN GO's
// Nakama layer): '{' the handshake (JSON), 0xA0 control, 0xA1 the stream,
// 0xA2 GGPO. STUN answers start with a byte below 0x40.
//
// The link runs a network thread; its methods can be called from any thread.
class P2PLink
{
public:
    static constexpr const char* kMagic = "IKEMEN-P2P/1";

    enum class State
    {
        Idle,
        Gathering,      // finding this computer's addresses (STUN)
        Connecting,     // hole punching, or waiting for the guest
        Ready,          // connected
        Failed,         // could not connect (GetError)
        Closed,         // the connection ended (by Close, the peer, or silence)
    };

    struct Options
    {
        u16 LocalPort = 0;                      // 0: any free port
        std::vector<std::string> StunServers;   // "host:port", tried in order
        int ConnectTimeoutMs = 20000;           // 0: wait as long as needed (a direct host)
    };

    P2PLink();
    ~P2PLink();
    P2PLink(const P2PLink&) = delete;
    P2PLink& operator=(const P2PLink&) = delete;

    // A matched session. sendSignal is called (on the network thread) with the
    // local signal's JSON, every half second until the link is ready; pass on
    // the peer's signals with HandleSignal.
    bool StartSignaled(const Options& opt, const std::string& matchID,
                       const std::string& selfID, const std::string& peerID,
                       std::function<void(const std::string&)> sendSignal, std::string& error);
    void HandleSignal(const std::string& json);

    // A direct session: the host waits for a guest that names the same
    // session code; the guest connects to "host:port".
    bool StartHost(const Options& opt, const std::string& code, std::string& error);
    bool StartGuest(const Options& opt, const std::string& hostAddress, const std::string& code, std::string& error);

    State GetState() const;
    std::string GetError() const;
    static const char* StateName(State s);

    u16 LocalPort() const;
    std::vector<P2PCandidate> LocalCandidates() const;
    NetAddress PeerAddress() const;     // where packets go, once ready
    int RoundTripMs() const;            // smoothed; -1 until measured

    // The message stream (once ready). A message is a type byte and up to
    // 16 MB of data; messages arrive whole and in order.
    bool Send(u8 type, const void* data, size_t len);
    bool Send(u8 type, const std::vector<u8>& data) { return Send(type, data.data(), data.size()); }
    bool Receive(u8& type, std::vector<u8>& data);
    // bytes sent but not yet acknowledged by the peer (to pace large transfers)
    size_t UnacknowledgedBytes() const;

    // GGPO's packets go through this (valid as long as the link exists). It names
    // the peer by PeerAddress().
    const GGPOTransport* Transport() const { return &GGPOTransportDesc; }

    // Ends the link, telling the peer.
    void Close();

private:
    enum class Mode { Signaled, Host, Guest };

    struct Datagram
    {
        std::vector<u8> Data;
    };

    bool Begin(const Options& opt, Mode mode, std::string& error);
    void Run();
    void Drain();
    void Dispatch(const u8* data, int len, const NetAddress& from);
    void HandleHandshake(const std::string& json, const NetAddress& from);
    void HandleControl(const u8* data, int len);
    void Tick(std::vector<std::string>& signalsOut);
    void TickStun();
    void FinishGathering();
    void MarkReady(const NetAddress& peer);
    void AddKnown(const NetAddress& addr);
    bool IsKnown(const NetAddress& addr) const;
    bool SendTagged(u8 tag, const void* data, int len, const NetAddress& to);
    std::string HandshakePacket(const char* kind) const;
    void SendPunches();
    void PumpStream();
    void Fail(const std::string& error);
    void Shut(const std::string& reason);
    u32 NowMS() const;
    std::string CurrentMatchID() const;

    static int KcpOutput(const char* buf, int len, IKCPCB* kcp, void* user);
    static int __cdecl TransportSendTo(void* context, const char* buffer, int len, const struct sockaddr_in* to);
    static int __cdecl TransportRecvFrom(void* context, char* buffer, int len, struct sockaddr_in* from);

    mutable std::mutex Lock;
    std::thread Thread;
    std::atomic<bool> StopFlag {false};

    UdpSocket Socket;
    Options Opt;
    Mode LinkMode = Mode::Signaled;
    State CurState = State::Idle;
    std::string Error;

    std::string MatchID;            // signaled: the Nakama match; direct: "direct:<code>"
    std::string SelfID, PeerID;
    std::string Token, RemoteToken;
    std::vector<P2PCandidate> Local, Remote;
    std::function<void(const std::string&)> SignalSender;
    std::string GuestHost;
    NetAddress GuestTarget;

    NetAddress Peer;
    std::vector<NetAddress> Known;

    std::chrono::steady_clock::time_point Epoch;
    u32 StartTime = 0;
    u32 LastPunch = 0, LastSignal = 0, LastPing = 0, LastRecv = 0;
    std::atomic<u32> LastSend {0};
    u32 ByeTime = 0;
    bool ByeReceived = false;

    // STUN
    std::vector<NetAddress> StunAddrs;
    size_t StunIndex = 0;
    u8 StunTx[12] {};
    u32 StunSent = 0, StunStarted = 0;
    bool StunResolved = false;

    // the stream
    IKCPCB* Kcp = nullptr;
    u32 Conv = 0;
    u32 NextKcpUpdate = 0;
    std::vector<u8> StreamIn;
    std::deque<std::pair<u8, std::vector<u8>>> Inbox;

    // GGPO
    std::deque<Datagram> GGPOQueue;
    GGPOTransport GGPOTransportDesc {};

    // round trip
    double SRTT = 0;
    int RttSamples = 0;

    // testing: MELONDS_NETPLAY_LOSS=percent drops that share of the packets sent
    int SimulatedLoss = 0;
    std::minstd_rand LossRng;
};

}

#endif // P2PLINK_H
