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
#include <stdexcept>

#include "RollbackSession.h"
#include "NetplayMachine.h"
#include "NetplayBus.h"
#include "Platform.h"

#include "ggponet.h"

namespace melonDS::RollbackNet
{

using Platform::Log;
using Platform::LogLevel;

// size of one player's input on the wire: PlayerInput::Bits, little endian
static constexpr int kInputBytes = 4;

// GGPO's callbacks carry no context pointer. GGPO calls them only from inside
// the API functions, on the calling thread, so the session that made the call
// is kept in a thread-local variable for the duration of the call.
static thread_local RollbackSession* CurrentSession = nullptr;

struct RollbackSession::CallbackScope
{
    RollbackSession* Previous;
    explicit CallbackScope(RollbackSession* s) : Previous(CurrentSession) { CurrentSession = s; }
    ~CallbackScope() { CurrentSession = Previous; }
};

// GGPO calls these; a callback that throws makes the GGPO call return
// GGPO_ERRORCODE_GENERAL_FAILURE (see ggpo main.cpp), and the session fails.
struct GGPOCallbacks
{
    static RollbackSession& Session()
    {
        if (!CurrentSession)
            throw std::runtime_error("GGPO callback outside of a session call");
        return *CurrentSession;
    }

    static bool __cdecl BeginGame(const char*) { return true; }

    static bool __cdecl SaveGameState(unsigned char** buffer, int* len, int* checksum, int frame)
    {
        return Session().OnSave(buffer, len, checksum, frame);
    }

    static bool __cdecl LoadGameState(unsigned char* buffer, int len)
    {
        return Session().OnLoad(buffer, len);
    }

    static bool __cdecl LogGameState(char*, unsigned char*, int) { return true; }

    static void __cdecl FreeBuffer(void* buffer)
    {
        if (CurrentSession && buffer)
            CurrentSession->OnFree(buffer);
    }

    static bool __cdecl AdvanceFrame(int)
    {
        return Session().OnAdvance();
    }

    static bool __cdecl OnEvent(GGPOEvent* info)
    {
        Session().OnEvent(info);
        return true;
    }
};

static const char* GGPOErrorText(GGPOErrorCode err)
{
    switch (err)
    {
    case GGPO_OK: return "no error";
    case GGPO_ERRORCODE_GENERAL_FAILURE: return "internal error";
    case GGPO_ERRORCODE_INVALID_SESSION: return "invalid session";
    case GGPO_ERRORCODE_INVALID_PLAYER_HANDLE: return "invalid player";
    case GGPO_ERRORCODE_PLAYER_OUT_OF_RANGE: return "player number out of range";
    case GGPO_ERRORCODE_PREDICTION_THRESHOLD: return "prediction limit reached";
    case GGPO_ERRORCODE_UNSUPPORTED: return "unsupported";
    case GGPO_ERRORCODE_NOT_SYNCHRONIZED: return "not synchronized";
    case GGPO_ERRORCODE_IN_ROLLBACK: return "in rollback";
    case GGPO_ERRORCODE_INPUT_DROPPED: return "input dropped";
    case GGPO_ERRORCODE_PLAYER_DISCONNECTED: return "player disconnected";
    case GGPO_ERRORCODE_TOO_MANY_SPECTATORS: return "too many spectators";
    case GGPO_ERRORCODE_INVALID_REQUEST: return "invalid request";
    default: return "unknown error";
    }
}

RollbackSession::RollbackSession(NetplayMachine& machine) : Machine(machine)
{
}

RollbackSession::~RollbackSession()
{
    Close();
}

bool RollbackSession::Start(const SessionConfig& config, std::string& error)
{
    Close();

    NumPlayers = Machine.NumConsoles();
    if (NumPlayers < 2 || NumPlayers > GGPO_MAX_PLAYERS)
    {
        error = "A rollback session has 2 to " + std::to_string(GGPO_MAX_PLAYERS) + " players.";
        return false;
    }
    if (config.LocalPlayer < 0 || config.LocalPlayer >= NumPlayers)
    {
        error = "The local player is not one of the machine's consoles.";
        return false;
    }
    if ((int)config.Peers.size() < NumPlayers)
    {
        error = "Every remote player needs an address.";
        return false;
    }

    Config = config;
    Config.MaxPrediction = std::clamp(Config.MaxPrediction, 1, GGPO_MAX_PREDICTION_FRAMES);
    Config.InputDelay = std::clamp(Config.InputDelay, 0, 15);
    Config.CheckInterval = std::max(Config.CheckInterval, 0);

    GGPOSessionCallbacks cb {};
    cb.begin_game = GGPOCallbacks::BeginGame;
    cb.save_game_state = GGPOCallbacks::SaveGameState;
    cb.load_game_state = GGPOCallbacks::LoadGameState;
    cb.log_game_state = GGPOCallbacks::LogGameState;
    cb.free_buffer = GGPOCallbacks::FreeBuffer;
    cb.advance_frame = GGPOCallbacks::AdvanceFrame;
    cb.on_event = GGPOCallbacks::OnEvent;

    GGPOSessionOptions opt {};
    opt.size = sizeof opt;
    opt.local_port = Config.LocalPort;
    opt.transport = Config.Transport;
    opt.max_prediction_frames = Config.MaxPrediction;

    CallbackScope scope(this);
    GGPOErrorCode err = ggpo_start_session_ex(&Session, &cb, "melonDS", NumPlayers, kInputBytes, &opt);
    if (!GGPO_SUCCEEDED(err) || !Session)
    {
        error = std::string("Could not start the rollback session: ") + GGPOErrorText(err);
        Session = nullptr;
        return false;
    }

    ggpo_set_disconnect_timeout(Session, Config.DisconnectTimeout);
    ggpo_set_disconnect_notify_start(Session, Config.DisconnectNotifyStart);

    Handles.assign(NumPlayers, GGPO_INVALID_HANDLE);
    for (int i = 0; i < NumPlayers; i++)
    {
        GGPOPlayer player {};
        player.size = sizeof player;
        player.player_num = i + 1;
        if (i == Config.LocalPlayer)
        {
            player.type = GGPO_PLAYERTYPE_LOCAL;
        }
        else
        {
            player.type = GGPO_PLAYERTYPE_REMOTE;
            const SessionPeer& peer = Config.Peers[i];
            if (peer.Address.empty() || peer.Address.size() >= sizeof(player.u.remote.ip_address))
            {
                error = "Player " + std::to_string(i + 1) + " has no valid address.";
                Close();
                return false;
            }
            strncpy(player.u.remote.ip_address, peer.Address.c_str(), sizeof(player.u.remote.ip_address) - 1);
            player.u.remote.port = peer.Port;
        }

        err = ggpo_add_player(Session, &player, &Handles[i]);
        if (!GGPO_SUCCEEDED(err))
        {
            error = "Could not add player " + std::to_string(i + 1) + ": " + GGPOErrorText(err);
            Close();
            return false;
        }
    }

    ggpo_set_frame_delay(Session, Handles[Config.LocalPlayer], Config.InputDelay);

    RemoteHashes.assign(NumPlayers, {});
    Counters = {};
    CurrentFrame = Machine.FrameCount();
    Failed = false;
    Running = false;    // until GGPO has synchronized with every peer
    return true;
}

void RollbackSession::Close()
{
    if (Session)
    {
        CallbackScope scope(this);
        ggpo_close_session(Session);
        Session = nullptr;
    }
    UsedBuffers.clear();
    FreeBuffers.clear();
    Handles.clear();
    Running = false;
    RollbackEnd = 0;
    PendingHashes.clear();
    FinalHashes.clear();
    Outgoing.clear();
    RemoteHashes.clear();
    DesyncReported = false;
    WaitBudget = 0.0;
}

void RollbackSession::Fail(const std::string& text)
{
    if (Failed) return;
    Failed = true;
    Log(LogLevel::Error, "netplay: %s\n", text.c_str());
    SessionEvent ev {SessionEvent::Failed};
    ev.Text = text;
    Events.push_back(ev);
}

RollbackSession::FrameResult RollbackSession::RunFrame(PlayerInput local)
{
    if (!Session || Failed)
        return FrameResult::Failed;

    CallbackScope scope(this);

    GGPOErrorCode err = ggpo_idle(Session, 0);
    if (!GGPO_SUCCEEDED(err))
    {
        Fail(std::string("The rollback session failed: ") + GGPOErrorText(err));
        return FrameResult::Failed;
    }
    if (Failed)
        return FrameResult::Failed;
    if (!Running)
        return FrameResult::Waiting;

    u8 bytes[kInputBytes];
    u32 bits = local.Sanitized().Bits;
    for (int i = 0; i < kInputBytes; i++)
        bytes[i] = (u8)(bits >> (i * 8));

    err = ggpo_add_local_input(Session, Handles[Config.LocalPlayer], bytes, kInputBytes);
    if (err == GGPO_ERRORCODE_PREDICTION_THRESHOLD || err == GGPO_ERRORCODE_NOT_SYNCHRONIZED)
        return FrameResult::Waiting;
    if (!GGPO_SUCCEEDED(err))
    {
        Fail(std::string("The local input could not be sent: ") + GGPOErrorText(err));
        return FrameResult::Failed;
    }

    u8 inputs[kInputBytes * GGPO_MAX_PLAYERS] {};
    int flags = 0;
    err = ggpo_synchronize_input(Session, inputs, kInputBytes * NumPlayers, &flags);
    if (!GGPO_SUCCEEDED(err))
    {
        if (err == GGPO_ERRORCODE_NOT_SYNCHRONIZED)
            return FrameResult::Waiting;
        Fail(std::string("The inputs could not be read: ") + GGPOErrorText(err));
        return FrameResult::Failed;
    }

    if (!RunMachineFrame(inputs, flags, false))
    {
        Fail("A console stopped.");
        return FrameResult::Failed;
    }

    err = ggpo_advance_frame(Session);
    if (!GGPO_SUCCEEDED(err))
    {
        Fail(std::string("The rollback session failed: ") + GGPOErrorText(err));
        return FrameResult::Failed;
    }
    if (Failed)
        return FrameResult::Failed;

    CollectFinalHashes();
    return FrameResult::Ran;
}

void RollbackSession::Idle()
{
    if (!Session || Failed)
        return;
    CallbackScope scope(this);
    GGPOErrorCode err = ggpo_idle(Session, 0);
    if (!GGPO_SUCCEEDED(err))
        Fail(std::string("The rollback session failed: ") + GGPOErrorText(err));
}

bool RollbackSession::RunMachineFrame(const u8* inputs, int disconnectFlags, bool resim, bool beforeShown)
{
    PlayerInput in[NetplayBus::kMaxConsoles] {};
    for (int i = 0; i < NumPlayers; i++)
    {
        u32 bits = 0;
        // a disconnected player's input reads as zero (GGPO clears it): no buttons
        for (int b = 0; b < kInputBytes; b++)
            bits |= (u32)inputs[i * kInputBytes + b] << (b * 8);
        in[i].Bits = bits;
    }
    (void)disconnectFlags;

    Machine.RunFrame(in, resim, beforeShown);
    CurrentFrame = Machine.FrameCount();
    return !Machine.StopRequested();
}

bool RollbackSession::OnSave(unsigned char** buffer, int* len, int* checksum, int frame)
{
    std::unique_ptr<MachineState> buf;
    if (!FreeBuffers.empty())
    {
        buf = std::move(FreeBuffers.back());
        FreeBuffers.pop_back();
    }
    else
    {
        buf = std::make_unique<MachineState>();
    }

    if ((u32)frame != Machine.FrameCount())
        Log(LogLevel::Warn, "netplay: GGPO saves frame %d, the machine is at %u\n", frame, Machine.FrameCount());

    if (!Machine.SaveState(*buf))
        throw std::runtime_error("the machine could not be saved");

    *checksum = 0;
    if (Config.CheckInterval > 0 && frame >= 0 && frame % Config.CheckInterval == 0)
    {
        u64 hash = NetplayMachine::HashState(*buf);
        PendingHashes[(u32)frame] = hash;
        *checksum = (int)(u32)hash;
    }

    // GGPO only keeps the pointer it is given and hands it back: give it the
    // MachineState itself
    *buffer = reinterpret_cast<unsigned char*>(buf.get());
    *len = (int)sizeof(MachineState);
    UsedBuffers[reinterpret_cast<const u8*>(buf.get())] = std::move(buf);
    return true;
}

bool RollbackSession::OnLoad(unsigned char* buffer, int len)
{
    // GGPO runs the frames from the loaded one up to where the machine is now
    RollbackEnd = Machine.FrameCount();
    auto it = UsedBuffers.find(reinterpret_cast<const u8*>(buffer));
    if (it == UsedBuffers.end() || !Machine.LoadState(*it->second))
        throw std::runtime_error("a saved frame could not be loaded");
    CurrentFrame = Machine.FrameCount();

    int depth = (int)(RollbackEnd - CurrentFrame);
    Counters.Rollbacks++;
    Counters.LastRollbackDepth = depth;
    Counters.MaxRollbackDepth = std::max(Counters.MaxRollbackDepth, depth);
    return true;
}

void RollbackSession::OnFree(void* buffer)
{
    auto it = UsedBuffers.find((const u8*)buffer);
    if (it == UsedBuffers.end())
        return;
    // keep a few for reuse (GGPO holds up to GGPO_MAX_PREDICTION_FRAMES + 2)
    if (FreeBuffers.size() < 4)
        FreeBuffers.push_back(std::move(it->second));
    UsedBuffers.erase(it);
}

bool RollbackSession::OnAdvance()
{
    // a frame simulated again during a rollback
    u8 inputs[kInputBytes * GGPO_MAX_PLAYERS] {};
    int flags = 0;
    GGPOErrorCode err = ggpo_synchronize_input(Session, inputs, kInputBytes * NumPlayers, &flags);
    if (!GGPO_SUCCEEDED(err))
        throw std::runtime_error(std::string("rollback: ") + GGPOErrorText(err));

    // the last frame of the rollback: the next frame the machine runs is shown
    bool last = Machine.FrameCount() + 1 >= RollbackEnd;
    if (!RunMachineFrame(inputs, flags, true, last))
        throw std::runtime_error("a console stopped");
    Counters.FramesResimulated++;

    err = ggpo_advance_frame(Session);
    if (!GGPO_SUCCEEDED(err))
        throw std::runtime_error(std::string("rollback: ") + GGPOErrorText(err));
    return true;
}

void RollbackSession::OnEvent(const void* ggpoEvent)
{
    const GGPOEvent* info = (const GGPOEvent*)ggpoEvent;
    SessionEvent ev {SessionEvent::Failed};
    switch (info->code)
    {
    case GGPO_EVENTCODE_CONNECTED_TO_PEER:
        ev.Kind = SessionEvent::ConnectedToPeer;
        ev.Player = info->u.connected.player - 1;
        break;
    case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
        ev.Kind = SessionEvent::SynchronizingWithPeer;
        ev.Player = info->u.synchronizing.player - 1;
        ev.Count = info->u.synchronizing.count;
        ev.Total = info->u.synchronizing.total;
        break;
    case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
        ev.Kind = SessionEvent::SynchronizedWithPeer;
        ev.Player = info->u.synchronized.player - 1;
        break;
    case GGPO_EVENTCODE_RUNNING:
        ev.Kind = SessionEvent::Running;
        Running = true;
        break;
    case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
        ev.Kind = SessionEvent::Disconnected;
        ev.Player = info->u.disconnected.player - 1;
        break;
    case GGPO_EVENTCODE_TIMESYNC:
        // not passed on: TakeFrameWait spreads it out
        WaitBudget += std::clamp(info->u.timesync.frames_ahead, 0, 16);
        return;
    case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
        ev.Kind = SessionEvent::Interrupted;
        ev.Player = info->u.connection_interrupted.player - 1;
        ev.Timeout = info->u.connection_interrupted.disconnect_timeout;
        break;
    case GGPO_EVENTCODE_CONNECTION_RESUMED:
        ev.Kind = SessionEvent::Resumed;
        ev.Player = info->u.connection_resumed.player - 1;
        break;
    default:
        return;
    }
    Events.push_back(ev);
}

std::vector<SessionEvent> RollbackSession::TakeEvents()
{
    std::vector<SessionEvent> out(Events.begin(), Events.end());
    Events.clear();
    return out;
}

double RollbackSession::TakeFrameWait()
{
    // at most a quarter of a frame per frame: a 4-frame lead is given back
    // over 16 frames instead of as one visible pause
    double wait = std::min(WaitBudget, 0.25);
    WaitBudget -= wait;
    return wait;
}

SessionStats RollbackSession::Stats()
{
    SessionStats s = Counters;
    s.Frame = CurrentFrame;
    if (!Session)
        return s;
    for (int i = 0; i < NumPlayers; i++)
    {
        if (i == Config.LocalPlayer) continue;
        GGPONetworkStats ns {};
        if (GGPO_SUCCEEDED(ggpo_get_network_stats(Session, Handles[i], &ns)))
        {
            s.Ping = std::max(s.Ping, ns.network.ping);
            s.KbpsSent += ns.network.kbps_sent;
            s.LocalFramesBehind = std::max(s.LocalFramesBehind, ns.timesync.local_frames_behind);
            s.RemoteFramesBehind = std::max(s.RemoteFramesBehind, ns.timesync.remote_frames_behind);
        }
    }
    return s;
}

void RollbackSession::CollectFinalHashes()
{
    if (Config.CheckInterval <= 0)
        return;

    // GGPO never runs more than MaxPrediction frames past the last frame whose
    // inputs it has from everyone, and rolls back as soon as an input arrives,
    // so a frame that far behind (plus a margin) is final.
    s64 limit = (s64)CurrentFrame - (Config.MaxPrediction + 2);
    while (!PendingHashes.empty() && (s64)PendingHashes.begin()->first <= limit)
    {
        auto it = PendingHashes.begin();
        FinalHashes[it->first] = it->second;
        Outgoing.push_back({it->first, it->second});
        for (int p = 0; p < NumPlayers; p++)
        {
            if (p != Config.LocalPlayer)
                Compare(p, it->first);
        }
        PendingHashes.erase(it);
    }

    // keep about a minute of hashes, for peers whose hashes arrive late
    while (FinalHashes.size() > 64)
        FinalHashes.erase(FinalHashes.begin());
}

std::vector<FrameHash> RollbackSession::TakeLocalHashes()
{
    std::vector<FrameHash> out;
    out.swap(Outgoing);
    return out;
}

void RollbackSession::AddRemoteHash(int player, const FrameHash& hash)
{
    if (player < 0 || player >= (int)RemoteHashes.size() || player == Config.LocalPlayer)
        return;
    auto& m = RemoteHashes[player];
    m[hash.Frame] = hash.Hash;
    while (m.size() > 64)
        m.erase(m.begin());
    Compare(player, hash.Frame);
}

void RollbackSession::Compare(int player, u32 frame)
{
    if (DesyncReported) return;
    auto mine = FinalHashes.find(frame);
    if (mine == FinalHashes.end()) return;
    auto theirs = RemoteHashes[player].find(frame);
    if (theirs == RemoteHashes[player].end()) return;
    if (mine->second == theirs->second)
    {
        Counters.ChecksPassed++;
        Counters.LastCheckedFrame = std::max(Counters.LastCheckedFrame, frame);
        return;
    }

    DesyncReported = true;
    Log(LogLevel::Error, "netplay: desync with player %d at frame %u (%016llx vs %016llx)\n",
        player + 1, frame, (unsigned long long)mine->second, (unsigned long long)theirs->second);
    SessionEvent ev {SessionEvent::Desync};
    ev.Player = player;
    ev.Frame = frame;
    Events.push_back(ev);
}

}
