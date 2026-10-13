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

#ifndef ROLLBACKSESSION_H
#define ROLLBACKSESSION_H

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "types.h"
#include "NetplayInput.h"

struct GGPOSession;
struct GGPOTransport;

namespace melonDS::RollbackNet
{

class NetplayMachine;
struct MachineState;

// Where GGPO reaches a remote player. With a transport (SessionConfig::Transport)
// this only names the player: the transport is given this address and routes
// the packet itself.
struct SessionPeer
{
    std::string Address;    // IPv4, dotted
    u16 Port = 0;
};

struct SessionConfig
{
    int LocalPlayer = 0;                    // this computer's player: its console's index
    std::vector<SessionPeer> Peers;         // one per player; the local player's entry is unused
    const GGPOTransport* Transport = nullptr; // nullptr: GGPO opens its own UDP socket on LocalPort
    u16 LocalPort = 0;

    // Frames between a button press and the frame it acts on. Each frame of
    // delay is a frame less of rollback on a given connection.
    int InputDelay = 2;
    // How far ahead of the other players' known inputs the session runs
    // (1-8) before it waits for them. A rollback simulates at most this many
    // frames again, so a lower limit keeps a slow computer from falling behind.
    int MaxPrediction = 8;
    int DisconnectTimeout = 5000;           // ms without packets before a player is dropped
    int DisconnectNotifyStart = 750;        // ms without packets before an "interrupted" event
    // A hash of the whole machine is taken every CheckInterval frames, once that frame
    // can no longer be rolled back, for comparison with the other computers (0: never).
    int CheckInterval = 60;
};

// Something the frontend should know about (shown to the player, or acted upon).
struct SessionEvent
{
    enum Type
    {
        ConnectedToPeer,        // Player
        SynchronizingWithPeer,  // Player, Count of Total round trips
        SynchronizedWithPeer,   // Player
        Running,                // every player is synchronized: frames run from now on
        Disconnected,           // Player
        Interrupted,            // Player, Timeout: ms left before Disconnected
        Resumed,                // Player
        Desync,                 // Player, Frame: that computer's machine differs from ours
        Failed,                 // the session stopped working (Text); it must be closed
    };
    Type Kind;
    int Player = -1;
    int Count = 0, Total = 0;
    int Timeout = 0;
    u32 Frame = 0;
    std::string Text;
};

// A hash of the machine at the start of a frame that can no longer be rolled back.
struct FrameHash
{
    u32 Frame = 0;
    u64 Hash = 0;
};

struct SessionStats
{
    int Ping = 0;                   // ms, to the slowest peer
    int LocalFramesBehind = 0;      // >0: we are behind that peer (GGPO's estimate)
    int RemoteFramesBehind = 0;
    int KbpsSent = 0;
    u32 Frame = 0;
    u64 Rollbacks = 0;              // rollbacks since the session started
    u64 FramesResimulated = 0;
    int LastRollbackDepth = 0;
    int MaxRollbackDepth = 0;
    int ChecksPassed = 0;           // desync checks that matched
    u32 LastCheckedFrame = 0;       // the last frame they matched at
};

// A GGPO peer-to-peer session that drives a NetplayMachine: GGPO exchanges the
// players' inputs and predicts the ones not received yet; when a prediction was
// wrong, the session loads the machine as it was at that frame and runs the
// frames since again with the right inputs.
//
// Everything runs on the thread that calls RunFrame and Idle; GGPO calls back
// into the session only from inside those calls.
class RollbackSession
{
public:
    explicit RollbackSession(NetplayMachine& machine);
    ~RollbackSession();
    RollbackSession(const RollbackSession&) = delete;
    RollbackSession& operator=(const RollbackSession&) = delete;

    bool Start(const SessionConfig& config, std::string& error);
    void Close();
    bool IsStarted() const { return Session != nullptr; }

    // every player is synchronized and frames can run
    bool IsRunning() const { return Running; }
    bool HasFailed() const { return Failed; }

    enum class FrameResult
    {
        Ran,        // the machine advanced one frame
        Waiting,    // not yet: synchronizing, or waiting for other players' inputs
        Failed,     // the session failed (see the Failed event)
    };

    // Once per frame: receives what arrived, rolls back if a prediction was
    // wrong, adds the local player's input and runs the next frame.
    FrameResult RunFrame(PlayerInput local);

    // Between frames, or while waiting: sends and receives packets (and rolls
    // back if needed) without running a new frame.
    void Idle();

    std::vector<SessionEvent> TakeEvents();

    // Pacing. GGPO estimates how many frames this computer runs ahead of the
    // others; this returns how long to wait, in frames, before the next frame
    // (0 most of the time): the frames GGPO recommends to give back, spread
    // over the next frames so that the picture does not stall.
    double TakeFrameWait();

    SessionStats Stats();

    // Desync checks: hashes of frames that can no longer change, to send to
    // the other computers, and theirs as they arrive. A hash that differs from
    // ours produces a Desync event.
    std::vector<FrameHash> TakeLocalHashes();
    void AddRemoteHash(int player, const FrameHash& hash);

private:
    struct CallbackScope;
    friend struct CallbackScope;
    friend struct GGPOCallbacks;

    bool OnSave(unsigned char** buffer, int* len, int* checksum, int frame);
    bool OnLoad(unsigned char* buffer, int len);
    void OnFree(void* buffer);
    bool OnAdvance();
    void OnEvent(const void* ggpoEvent);

    void Fail(const std::string& text);
    bool RunMachineFrame(const u8* inputs, int disconnectFlags, bool resim, bool beforeShown = false);
    void CollectFinalHashes();
    void Compare(int player, u32 frame);

    NetplayMachine& Machine;
    SessionConfig Config;
    GGPOSession* Session = nullptr;
    std::vector<int> Handles;           // GGPO player handle of each player
    int NumPlayers = 0;
    bool Running = false;
    bool Failed = false;
    std::deque<SessionEvent> Events;

    // save buffers: GGPO keeps up to 10 saved frames; the buffers are reused
    std::vector<std::unique_ptr<MachineState>> FreeBuffers;
    std::unordered_map<const u8*, std::unique_ptr<MachineState>> UsedBuffers;

    // the frame the machine is at, as GGPO counts (the frame about to run)
    u32 CurrentFrame = 0;
    // during a rollback: the frame the machine was at before it, where it ends
    u32 RollbackEnd = 0;

    // pacing
    double WaitBudget = 0.0;

    // desync checks
    std::map<u32, u64> PendingHashes;   // taken while the frame could still change
    std::map<u32, u64> FinalHashes;     // ours, final
    std::vector<FrameHash> Outgoing;
    std::vector<std::map<u32, u64>> RemoteHashes;
    bool DesyncReported = false;

    SessionStats Counters;
};

}

#endif // ROLLBACKSESSION_H
