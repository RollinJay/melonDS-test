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
#include <random>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#endif

#include "P2PLink.h"
#include "NetJson.h"
#include "kcp/ikcp.h"

namespace melonDS::RollbackNet
{

// datagram tags (first byte), as in IKEMEN GO's src/nakama_p2pmux.go
static constexpr u8 kTagControl = 0xA0;
static constexpr u8 kTagStream = 0xA1;
static constexpr u8 kTagGGPO = 0xA2;

static constexpr u8 kCtrlKeepalive = 0x01;
static constexpr u8 kCtrlBye = 0x02;
static constexpr u8 kCtrlPing = 0x03;
static constexpr u8 kCtrlPong = 0x04;

static constexpr u32 kPunchInterval = 75;         // ms between hello rounds
static constexpr u32 kResignalInterval = 500;     // ms between signals until ready
static constexpr u32 kStunRetry = 500;            // ms between STUN requests to one server
static constexpr u32 kStunTimeout = 3000;         // ms before the next server is tried
static constexpr u32 kPingInterval = 1000;
static constexpr u32 kKeepaliveInterval = 2000;   // a keepalive goes out after this much quiet
static constexpr u32 kPeerSilenceTimeout = 8000;  // nothing received this long: the peer is gone
static constexpr u32 kByeGrace = 750;             // a bye ends the link after this (late data)

static constexpr int kStreamMTU = 1200;           // with the tag and IP/UDP headers, < 1280 bytes
static constexpr size_t kMaxMessage = 16u << 20;
static constexpr size_t kMaxGGPOQueue = 1024;

static constexpr u32 kStunCookie = 0x2112A442;

static std::string RandomToken()
{
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 16; i++)
    {
        u8 b = (u8)rd();
        out += hex[b >> 4];
        out += hex[b & 0xF];
    }
    return out;
}

// the KCP conversation id both peers derive from the two tokens (IKEMEN's p2pConv)
static u32 ConvFromTokens(std::string a, std::string b, const char* purpose)
{
    if (b < a) std::swap(a, b);
    u32 h = 2166136261u;
    auto feed = [&h](const std::string& s)
    {
        for (unsigned char c : s)
        {
            h ^= c;
            h *= 16777619u;
        }
    };
    feed(a);
    feed("|");
    feed(b);
    feed("|");
    feed(purpose);
    return h;
}

// ---- signal ------------------------------------------------------------------

std::string P2PSignal::ToJson() const
{
    JsonValue v = JsonValue::MakeObject();
    v.Set("magic", P2PLink::kMagic);
    v.Set("match_id", MatchID);
    v.Set("token", Token);
    JsonValue cands = JsonValue::MakeArray();
    for (const P2PCandidate& c : Candidates)
    {
        JsonValue cv = JsonValue::MakeObject();
        cv.Set("type", c.Type);
        cv.Set("address", c.Address);
        cv.Set("port", (int)c.Port);
        cands.Append(std::move(cv));
    }
    v.Set("candidates", std::move(cands));
    if (!From.empty()) v.Set("from", From);
    if (!To.empty()) v.Set("to", To);
    return v.Serialize();
}

bool P2PSignal::FromJson(const std::string& json, P2PSignal& out)
{
    JsonValue v;
    if (!JsonValue::Parse(json, v) || !v.IsObject())
        return false;
    if (v["magic"].AsString() != P2PLink::kMagic)
        return false;
    out = {};
    out.MatchID = v["match_id"].AsString();
    out.Token = v["token"].AsString();
    out.From = v["from"].AsString();
    out.To = v["to"].AsString();
    const JsonValue& cands = v["candidates"];
    for (size_t i = 0; i < cands.Size() && i < 32; i++)
    {
        P2PCandidate c;
        c.Type = cands[i]["type"].AsString();
        c.Address = cands[i]["address"].AsString();
        double port = cands[i]["port"].AsNumber();
        if (port <= 0 || port > 65535) continue;
        c.Port = (u16)port;
        out.Candidates.push_back(c);
    }
    return !out.Token.empty();
}

// ---- link --------------------------------------------------------------------

P2PLink::P2PLink()
{
    Epoch = std::chrono::steady_clock::now();
    GGPOTransportDesc.context = this;
    GGPOTransportDesc.send_to = &P2PLink::TransportSendTo;
    GGPOTransportDesc.recv_from = &P2PLink::TransportRecvFrom;

    if (const char* loss = getenv("MELONDS_NETPLAY_LOSS"))
        SimulatedLoss = std::clamp(atoi(loss), 0, 90);
    LossRng.seed(std::random_device()());
}

P2PLink::~P2PLink()
{
    Close();
}

u32 P2PLink::NowMS() const
{
    return (u32)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Epoch).count();
}

const char* P2PLink::StateName(State s)
{
    switch (s)
    {
    case State::Idle: return "idle";
    case State::Gathering: return "gathering";
    case State::Connecting: return "connecting";
    case State::Ready: return "ready";
    case State::Failed: return "failed";
    case State::Closed: return "closed";
    }
    return "?";
}

bool P2PLink::Begin(const Options& opt, Mode mode, std::string& error)
{
    Close();

    std::lock_guard lock(Lock);
    Opt = opt;
    LinkMode = mode;
    Error.clear();
    Token = RandomToken();
    RemoteToken.clear();
    Local.clear();
    Remote.clear();
    Known.clear();
    Peer = {};
    Inbox.clear();
    GGPOQueue.clear();
    StreamIn.clear();
    SRTT = 0;
    RttSamples = 0;
    ByeReceived = false;
    StunAddrs.clear();
    StunIndex = 0;
    StunResolved = false;

    if (!Socket.Open(opt.LocalPort, error))
    {
        // a port someone forwarded may be taken (a second instance on this
        // computer): any port still works for hole punching
        if (opt.LocalPort == 0 || mode == Mode::Host || !Socket.Open(0, error))
            return false;
    }

    StartTime = NowMS();
    LastPunch = LastSignal = LastPing = 0;
    LastRecv = StartTime;
    LastSend = StartTime;
    CurState = (mode == Mode::Host) ? State::Connecting : State::Gathering;
    StopFlag = false;
    Thread = std::thread(&P2PLink::Run, this);
    return true;
}

bool P2PLink::StartSignaled(const Options& opt, const std::string& matchID,
                            const std::string& selfID, const std::string& peerID,
                            std::function<void(const std::string&)> sendSignal, std::string& error)
{
    if (matchID.empty())
    {
        error = "No match to connect through.";
        return false;
    }
    {
        // set before the thread starts
        std::lock_guard lock(Lock);
        MatchID = matchID;
        SelfID = selfID;
        PeerID = peerID;
        SignalSender = std::move(sendSignal);
    }
    return Begin(opt, Mode::Signaled, error);
}

bool P2PLink::StartHost(const Options& opt, const std::string& code, std::string& error)
{
    if (opt.LocalPort == 0)
    {
        error = "Hosting needs a port the guest can reach.";
        return false;
    }
    {
        std::lock_guard lock(Lock);
        MatchID = "direct:" + code;
        SelfID.clear();
        PeerID.clear();
        SignalSender = nullptr;
    }
    return Begin(opt, Mode::Host, error);
}

bool P2PLink::StartGuest(const Options& opt, const std::string& hostAddress, const std::string& code, std::string& error)
{
    if (hostAddress.empty())
    {
        error = "No host address.";
        return false;
    }
    {
        std::lock_guard lock(Lock);
        MatchID = "direct:" + code;
        SelfID.clear();
        PeerID.clear();
        SignalSender = nullptr;
        GuestHost = hostAddress;
        GuestTarget = {};
    }
    return Begin(opt, Mode::Guest, error);
}

void P2PLink::HandleSignal(const std::string& json)
{
    P2PSignal sig;
    if (!P2PSignal::FromJson(json, sig))
        return;

    std::lock_guard lock(Lock);
    if (LinkMode != Mode::Signaled || sig.MatchID != MatchID)
        return;
    // a match may echo our own signal back to us
    if (sig.Token == Token)
        return;
    // in a lobby or channel, signals of other pairs pass by too
    if ((!sig.To.empty() && !SelfID.empty() && sig.To != SelfID) ||
        (!sig.From.empty() && !PeerID.empty() && sig.From != PeerID))
        return;
    if (CurState == State::Ready)
        return;
    RemoteToken = sig.Token;
    Remote = sig.Candidates;
}

P2PLink::State P2PLink::GetState() const
{
    std::lock_guard lock(Lock);
    return CurState;
}

std::string P2PLink::GetError() const
{
    std::lock_guard lock(Lock);
    return Error;
}

bool P2PLink::PeerSaidBye() const
{
    std::lock_guard lock(Lock);
    return ByeReceived;
}

u16 P2PLink::LocalPort() const
{
    std::lock_guard lock(Lock);
    return Socket.LocalPort();
}

std::vector<P2PCandidate> P2PLink::LocalCandidates() const
{
    std::lock_guard lock(Lock);
    return Local;
}

NetAddress P2PLink::PeerAddress() const
{
    std::lock_guard lock(Lock);
    return Peer;
}

int P2PLink::RoundTripMs() const
{
    std::lock_guard lock(Lock);
    return RttSamples ? (int)(SRTT + 0.5) : -1;
}

std::string P2PLink::CurrentMatchID() const
{
    return MatchID;
}

void P2PLink::Fail(const std::string& error)
{
    if (CurState == State::Failed || CurState == State::Closed)
        return;
    Error = error;
    CurState = State::Failed;
}

void P2PLink::Shut(const std::string& reason)
{
    if (CurState == State::Failed || CurState == State::Closed)
        return;
    Error = reason;
    CurState = State::Closed;
}

void P2PLink::Close()
{
    {
        std::lock_guard lock(Lock);
        if (CurState == State::Ready && Socket.IsOpen())
        {
            u8 bye[5] = {kCtrlBye, (u8)Conv, (u8)(Conv >> 8), (u8)(Conv >> 16), (u8)(Conv >> 24)};
            if (Kcp) ikcp_flush(Kcp);
            for (int i = 0; i < 3; i++)
                SendTagged(kTagControl, bye, sizeof bye, Peer);
        }
        if (CurState != State::Idle && CurState != State::Failed)
            CurState = State::Closed;
    }

    StopFlag = true;
    if (Thread.joinable())
        Thread.join();

    std::lock_guard lock(Lock);
    Socket.Close();
    if (Kcp)
    {
        ikcp_release(Kcp);
        Kcp = nullptr;
    }
}

bool P2PLink::SendTagged(u8 tag, const void* data, int len, const NetAddress& to)
{
    u8 buf[2048];
    if (len < 0 || len + 1 > (int)sizeof buf)
        return false;
    buf[0] = tag;
    memcpy(buf + 1, data, len);
    if (SimulatedLoss && (int)(LossRng() % 100) < SimulatedLoss)
        return true;    // lost on the way
    bool ok = Socket.SendTo(buf, len + 1, to);
    if (ok) LastSend = NowMS();
    return ok;
}

std::string P2PLink::HandshakePacket(const char* kind) const
{
    JsonValue v = JsonValue::MakeObject();
    v.Set("magic", kMagic);
    v.Set("kind", kind);
    v.Set("match_id", MatchID);
    v.Set("token", Token);
    return v.Serialize();
}

void P2PLink::Run()
{
    // names are looked up here, off the caller's thread (it may block)
    Mode mode;
    std::vector<std::string> stunNames;
    std::string guestHost;
    {
        std::lock_guard lock(Lock);
        mode = LinkMode;
        stunNames = Opt.StunServers;
        guestHost = GuestHost;
    }

    if (mode == Mode::Signaled)
    {
        std::vector<NetAddress> addrs;
        for (const std::string& name : stunNames)
        {
            NetAddress a;
            if (ResolveAddress(name, 3478, a))
                addrs.push_back(a);
        }
        std::lock_guard lock(Lock);
        StunAddrs = addrs;
        StunIndex = 0;
        StunSent = 0;
        StunStarted = NowMS();
        StunResolved = true;
        if (StunAddrs.empty())
            FinishGathering();
    }
    else if (mode == Mode::Guest)
    {
        NetAddress target;
        bool ok = ResolveAddress(guestHost, 7064, target);
        std::lock_guard lock(Lock);
        if (!ok)
            Fail("Could not find the host " + guestHost + ".");
        else
        {
            GuestTarget = target;
            CurState = State::Connecting;
        }
    }

    while (!StopFlag)
    {
        Socket.WaitReadable(4);

        std::vector<std::string> signals;
        std::function<void(const std::string&)> sender;
        {
            std::lock_guard lock(Lock);
            if (CurState == State::Failed || CurState == State::Closed)
            {
                // keep the socket open (the owner reads the state and closes)
                Drain();
            }
            else
            {
                Drain();
                Tick(signals);
            }
            sender = SignalSender;
        }
        if (sender)
        {
            for (const std::string& s : signals)
                sender(s);
        }
    }
}

void P2PLink::Drain()
{
    u8 buf[64 * 1024];
    for (int n = 0; n < 4096; n++)
    {
        NetAddress from;
        int len = Socket.RecvFrom(buf, sizeof buf, from);
        if (len < 0)
            break;
        if (len == 0)
            continue;
        Dispatch(buf, len, from);
    }
}

void P2PLink::Dispatch(const u8* data, int len, const NetAddress& from)
{
    u8 tag = data[0];

    if (tag == '{')
    {
        if (len <= 4096)
            HandleHandshake(std::string((const char*)data, len), from);
        return;
    }

    if (tag < 0x40)
    {
        // STUN: a binding success answer to our request
        if (CurState != State::Gathering || len < 20)
            return;
        u16 type = (data[0] << 8) | data[1];
        u16 length = (data[2] << 8) | data[3];
        u32 cookie = ((u32)data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
        if (type != 0x0101 || cookie != kStunCookie || memcmp(data + 8, StunTx, 12) != 0)
            return;
        if (20 + (int)length > len)
            return;
        for (int off = 20; off + 4 <= 20 + length;)
        {
            u16 atype = (data[off] << 8) | data[off + 1];
            u16 alen = (data[off + 2] << 8) | data[off + 3];
            int vstart = off + 4;
            if (vstart + alen > len)
                return;
            if ((atype == 0x0020 || atype == 0x0001) && alen >= 8 && data[vstart + 1] == 0x01)
            {
                u16 port = (data[vstart + 2] << 8) | data[vstart + 3];
                u32 ip = ((u32)data[vstart + 4] << 24) | (data[vstart + 5] << 16) | (data[vstart + 6] << 8) | data[vstart + 7];
                if (atype == 0x0020)
                {
                    port ^= (u16)(kStunCookie >> 16);
                    ip ^= kStunCookie;
                }
                NetAddress mapped {ip, port};
                Local.push_back({"srflx", mapped.IPString(), mapped.Port});
                FinishGathering();
                return;
            }
            off = vstart + alen + ((4 - (alen % 4)) % 4);
        }
        return;
    }

    if (tag != kTagControl && tag != kTagStream && tag != kTagGGPO)
        return;
    if (CurState != State::Ready || !IsKnown(from))
        return;

    LastRecv = NowMS();
    const u8* payload = data + 1;
    int plen = len - 1;

    switch (tag)
    {
    case kTagControl:
        HandleControl(payload, plen);
        break;
    case kTagStream:
        if (Kcp)
        {
            ikcp_input(Kcp, (const char*)payload, plen);
            PumpStream();
            NextKcpUpdate = 0;    // answer (acks) soon
        }
        break;
    case kTagGGPO:
        if (GGPOQueue.size() < kMaxGGPOQueue && plen > 0)
            GGPOQueue.push_back({std::vector<u8>(payload, payload + plen)});
        break;
    }
}

void P2PLink::HandleHandshake(const std::string& json, const NetAddress& from)
{
    JsonValue v;
    if (!JsonValue::Parse(json, v) || !v.IsObject() || v["magic"].AsString() != kMagic)
        return;
    if (v["match_id"].AsString() != MatchID)
        return;
    std::string kind = v["kind"].AsString();
    std::string token = v["token"].AsString();
    if (token.empty() || token == Token)
        return;

    switch (LinkMode)
    {
    case Mode::Signaled:
        if (RemoteToken.empty() || token != RemoteToken)
            return;
        if (kind == "hello")
        {
            std::string ack = HandshakePacket("ack");
            // acks are sent redundantly; a later hello is answered again too
            int times = (CurState == State::Ready) ? 1 : 3;
            for (int i = 0; i < times; i++)
                Socket.SendTo(ack.data(), (int)ack.size(), from);
            if (CurState == State::Ready)
                AddKnown(from);
            else if (CurState == State::Connecting || CurState == State::Gathering)
                MarkReady(from);
        }
        else if (kind == "ack")
        {
            if (CurState == State::Ready)
                AddKnown(from);
            else if (CurState == State::Connecting || CurState == State::Gathering)
                MarkReady(from);
        }
        break;

    case Mode::Host:
        if (kind != "hello")
            return;
        if (CurState == State::Ready)
        {
            if (token != RemoteToken)
                return;     // someone else: the session already has its guest
            std::string ack = HandshakePacket("ack");
            Socket.SendTo(ack.data(), (int)ack.size(), from);
            AddKnown(from);
        }
        else if (CurState == State::Connecting)
        {
            RemoteToken = token;
            std::string ack = HandshakePacket("ack");
            for (int i = 0; i < 3; i++)
                Socket.SendTo(ack.data(), (int)ack.size(), from);
            MarkReady(from);
        }
        break;

    case Mode::Guest:
        if (kind != "ack")
            return;
        if (CurState == State::Ready)
        {
            if (token == RemoteToken)
                AddKnown(from);
        }
        else if (CurState == State::Connecting)
        {
            RemoteToken = token;
            MarkReady(from);
        }
        break;
    }
}

void P2PLink::HandleControl(const u8* data, int len)
{
    if (len < 1)
        return;
    switch (data[0])
    {
    case kCtrlPing:
        if (len >= 9)
        {
            u8 pong[9];
            pong[0] = kCtrlPong;
            memcpy(pong + 1, data + 1, 8);
            SendTagged(kTagControl, pong, sizeof pong, Peer);
        }
        break;
    case kCtrlPong:
        if (len >= 9)
        {
            u64 sent = 0;
            for (int i = 0; i < 8; i++)
                sent |= (u64)data[1 + i] << (i * 8);
            // the clock pings carry is nanoseconds since Epoch (as IKEMEN's)
            u64 now = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Epoch).count();
            if (now > sent)
            {
                double sample = (double)(now - sent) / 1e6;
                if (sample > 0 && sample < 10000)
                {
                    if (RttSamples == 0)
                        SRTT = sample;
                    else
                        SRTT += (sample - SRTT) / 8;
                    RttSamples++;
                }
            }
        }
        break;
    case kCtrlBye:
        if (len >= 5)
        {
            u32 conv = data[1] | (data[2] << 8) | (data[3] << 16) | ((u32)data[4] << 24);
            if (conv == Conv && !ByeReceived)
            {
                ByeReceived = true;
                ByeTime = NowMS();
            }
        }
        break;
    case kCtrlKeepalive:
    default:
        break;
    }
}

void P2PLink::FinishGathering()
{
    if (CurState != State::Gathering)
        return;

    u16 port = Socket.LocalPort();
    std::vector<P2PCandidate> cands;
    for (u32 ip : LocalIPv4Addresses())
        cands.push_back({"host", NetAddress{ip, port}.IPString(), port});
    for (const P2PCandidate& c : Local)
        cands.push_back(c);    // srflx

    // unique
    std::vector<P2PCandidate> unique;
    for (const P2PCandidate& c : cands)
    {
        bool dup = false;
        for (const P2PCandidate& u : unique)
            dup |= (u.Type == c.Type && u.Address == c.Address && u.Port == c.Port);
        if (!dup) unique.push_back(c);
    }
    Local = unique;

    if (Local.empty())
    {
        Fail("This computer has no IPv4 address to connect from.");
        return;
    }
    CurState = State::Connecting;
    LastSignal = 0;     // send at once
}

void P2PLink::TickStun()
{
    if (!StunResolved)
        return;
    u32 now = NowMS();
    if (StunIndex >= StunAddrs.size())
    {
        FinishGathering();
        return;
    }
    if (now - StunStarted >= kStunTimeout)
    {
        // this server did not answer: the next one
        StunIndex++;
        StunStarted = now;
        StunSent = 0;
        if (StunIndex >= StunAddrs.size())
        {
            FinishGathering();
            return;
        }
    }
    if (StunSent == 0 || now - StunSent >= kStunRetry)
    {
        if (StunSent == 0)
        {
            std::random_device rd;
            for (u8& b : StunTx) b = (u8)rd();
        }
        u8 req[20];
        req[0] = 0x00; req[1] = 0x01;   // binding request
        req[2] = 0x00; req[3] = 0x00;   // no attributes
        req[4] = (u8)(kStunCookie >> 24); req[5] = (u8)(kStunCookie >> 16);
        req[6] = (u8)(kStunCookie >> 8);  req[7] = (u8)kStunCookie;
        memcpy(req + 8, StunTx, 12);
        Socket.SendTo(req, sizeof req, StunAddrs[StunIndex]);
        StunSent = now;
    }
}

void P2PLink::SendPunches()
{
    std::string hello = HandshakePacket("hello");
    if (LinkMode == Mode::Guest)
    {
        if (GuestTarget.IsValid())
            Socket.SendTo(hello.data(), (int)hello.size(), GuestTarget);
        return;
    }
    if (RemoteToken.empty())
        return;
    for (const P2PCandidate& c : Remote)
    {
        NetAddress a;
        if (!NetAddress::ParseIP(c.Address, a.IP) || c.Port == 0)
            continue;
        a.Port = c.Port;
        Socket.SendTo(hello.data(), (int)hello.size(), a);
    }
}

void P2PLink::Tick(std::vector<std::string>& signalsOut)
{
    u32 now = NowMS();

    switch (CurState)
    {
    case State::Gathering:
        TickStun();
        break;

    case State::Connecting:
        if (Opt.ConnectTimeoutMs > 0 && now - StartTime >= (u32)Opt.ConnectTimeoutMs)
        {
            Fail("Could not open a connection to the other player.");
            return;
        }
        if (LinkMode == Mode::Signaled && (LastSignal == 0 || now - LastSignal >= kResignalInterval))
        {
            // the signaling service only passes it to whoever is there when it
            // is sent, so it goes out again until the link is ready
            P2PSignal sig;
            sig.MatchID = MatchID;
            sig.Token = Token;
            sig.From = SelfID;
            sig.To = PeerID;
            sig.Candidates = Local;
            signalsOut.push_back(sig.ToJson());
            LastSignal = now;
        }
        if (now - LastPunch >= kPunchInterval)
        {
            SendPunches();
            LastPunch = now;
        }
        break;

    case State::Ready:
        if (ByeReceived && now - ByeTime >= kByeGrace)
        {
            Shut("The other player left.");
            return;
        }
        if (now - LastRecv >= kPeerSilenceTimeout)
        {
            Shut("Lost the connection to the other player.");
            return;
        }
        if (now - LastPing >= kPingInterval)
        {
            u8 ping[9];
            ping[0] = kCtrlPing;
            u64 t = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Epoch).count();
            for (int i = 0; i < 8; i++)
                ping[1 + i] = (u8)(t >> (i * 8));
            SendTagged(kTagControl, ping, sizeof ping, Peer);
            LastPing = now;
        }
        if (now - LastSend.load() >= kKeepaliveInterval)
        {
            u8 ka = kCtrlKeepalive;
            SendTagged(kTagControl, &ka, 1, Peer);
        }
        if (Kcp && (NextKcpUpdate == 0 || (s32)(now - NextKcpUpdate) >= 0))
        {
            ikcp_update(Kcp, now);
            NextKcpUpdate = ikcp_check(Kcp, now);
            PumpStream();
        }
        break;

    default:
        break;
    }
}

void P2PLink::MarkReady(const NetAddress& peer)
{
    Peer = peer;
    Known.clear();
    Known.push_back(peer);
    LastRecv = NowMS();
    CurState = State::Ready;

    Conv = ConvFromTokens(Token, RemoteToken, "stream");
    if (Kcp) ikcp_release(Kcp);
    Kcp = ikcp_create(Conv, this);
    Kcp->output = &P2PLink::KcpOutput;
    // stream mode, and KCP's low-latency profile: every write goes out at once,
    // immediate acks, fast resend after two skipped acks, no congestion window
    Kcp->stream = 1;
    ikcp_nodelay(Kcp, 1, 10, 2, 1);
    ikcp_wndsize(Kcp, 128, 128);
    ikcp_setmtu(Kcp, kStreamMTU);
    NextKcpUpdate = 0;
}

void P2PLink::AddKnown(const NetAddress& addr)
{
    if (!IsKnown(addr) && Known.size() < 16)
        Known.push_back(addr);
}

bool P2PLink::IsKnown(const NetAddress& addr) const
{
    for (const NetAddress& k : Known)
    {
        if (k == addr) return true;
    }
    return false;
}

int P2PLink::KcpOutput(const char* buf, int len, IKCPCB*, void* user)
{
    P2PLink* self = (P2PLink*)user;
    self->SendTagged(kTagStream, buf, len, self->Peer);
    return 0;
}

void P2PLink::PumpStream()
{
    if (!Kcp) return;
    for (;;)
    {
        int size = ikcp_peeksize(Kcp);
        if (size <= 0) break;
        size_t at = StreamIn.size();
        StreamIn.resize(at + size);
        int got = ikcp_recv(Kcp, (char*)StreamIn.data() + at, size);
        if (got <= 0)
        {
            StreamIn.resize(at);
            break;
        }
        StreamIn.resize(at + got);
    }

    // messages: u32 length (little endian) of what follows, type byte, data
    size_t pos = 0;
    while (StreamIn.size() - pos >= 5)
    {
        const u8* p = StreamIn.data() + pos;
        u32 len = p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
        if (len < 1 || len > kMaxMessage + 1)
        {
            Shut("The other player sent something this version does not understand.");
            StreamIn.clear();
            return;
        }
        if (StreamIn.size() - pos - 4 < len)
            break;
        u8 type = p[4];
        Inbox.emplace_back(type, std::vector<u8>(p + 5, p + 4 + len));
        pos += 4 + len;
    }
    if (pos)
        StreamIn.erase(StreamIn.begin(), StreamIn.begin() + pos);
}

bool P2PLink::Send(u8 type, const void* data, size_t len)
{
    if (len > kMaxMessage)
        return false;
    std::lock_guard lock(Lock);
    if (CurState != State::Ready || !Kcp)
        return false;

    u8 header[5];
    u32 total = (u32)len + 1;
    header[0] = (u8)total; header[1] = (u8)(total >> 8);
    header[2] = (u8)(total >> 16); header[3] = (u8)(total >> 24);
    header[4] = type;
    if (ikcp_send(Kcp, (const char*)header, sizeof header) < 0)
        return false;

    // KCP takes at most 127 segments per call
    const u8* p = (const u8*)data;
    size_t left = len;
    while (left > 0)
    {
        int chunk = (int)std::min<size_t>(left, 32 * 1024);
        if (ikcp_send(Kcp, (const char*)p, chunk) < 0)
            return false;
        p += chunk;
        left -= chunk;
    }
    ikcp_flush(Kcp);
    NextKcpUpdate = 0;
    return true;
}

bool P2PLink::Receive(u8& type, std::vector<u8>& data)
{
    std::lock_guard lock(Lock);
    if (Inbox.empty())
        return false;
    type = Inbox.front().first;
    data = std::move(Inbox.front().second);
    Inbox.pop_front();
    return true;
}

size_t P2PLink::UnacknowledgedBytes() const
{
    std::lock_guard lock(Lock);
    if (!Kcp) return 0;
    return (size_t)ikcp_waitsnd(Kcp) * Kcp->mss;
}

int P2PLink::TransportSendTo(void* context, const char* buffer, int len, const struct sockaddr_in*)
{
    P2PLink* self = (P2PLink*)context;
    std::lock_guard lock(self->Lock);
    if (self->CurState != State::Ready)
        return -1;
    return self->SendTagged(kTagGGPO, buffer, len, self->Peer) ? len : -1;
}

int P2PLink::TransportRecvFrom(void* context, char* buffer, int len, struct sockaddr_in* from)
{
    P2PLink* self = (P2PLink*)context;
    std::lock_guard lock(self->Lock);
    if (self->GGPOQueue.empty())
        return -1;
    Datagram d = std::move(self->GGPOQueue.front());
    self->GGPOQueue.pop_front();
    int n = std::min<int>(len, (int)d.Data.size());
    memcpy(buffer, d.Data.data(), n);
    // every packet is reported as coming from the peer's address, which is
    // the address GGPO was given for the player
    memset(from, 0, sizeof *from);
    from->sin_family = AF_INET;
    from->sin_addr.s_addr = htonl(self->Peer.IP);
    from->sin_port = htons(self->Peer.Port);
    return n;
}

}
