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

#ifndef ONLINESESSION_H
#define ONLINESESSION_H

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "types.h"
#include "NetplayInput.h"
#include "NetplayMachine.h"
#include "RollbackSession.h"
#include "SPI_Firmware.h"

namespace melonDS::RollbackNet
{

class P2PLink;

// A player's console settings, as the other computers build that console:
// what the game reads from the DS's firmware.
struct PlayerProfile
{
    std::string Name = "Player";    // the player's name; the DS nickname is its first 10 characters (UTF-8)
    std::string Message;            // up to 26 characters
    u8 Language = 1;                // 0 Japanese, 1 English, 2 French, 3 German, 4 Italian, 5 Spanish
    u8 BirthdayMonth = 1;
    u8 BirthdayDay = 1;
    u8 FavoriteColor = 0;           // 0-15
};

// The firmware of console `index` of a netplay machine: melonDS's generated
// firmware with the player's settings and a MAC address of its own.
Firmware MakeNetplayFirmware(int index, const PlayerProfile& profile);

// What this computer brings to a session.
struct LocalSetup
{
    std::vector<u8> ROM;            // never sent: the other player needs the same ROM
    std::string ROMName;            // the file name (the host's names the game for direct boot)
    PlayerProfile Profile;
    std::vector<u8> Save;           // this player's save, sent to the other computer
    std::string Version;            // melonDS version text, shown when they differ

    // preferences; in a session, the host's apply (MaxPrediction: the lower one)
    int InputDelay = -1;            // frames; -1: chosen from the round trip
    int MaxPrediction = 8;          // 1-8
    int CheckInterval = 60;         // frames between desync checks; 0 disables them
    double AudioSampleRate = 48000.0;
    int Threads = 0;                // threads running the consoles (0: one per console, up to the cores)
    // the date and time every console starts at (the host's are used)
    int Year = 2026, Month = 1, Day = 1, Hour = 12, Minute = 0, Second = 0;
};

struct ChatLine
{
    int Player;                     // -1: a notice from melonDS
    std::string Text;
};

// An online session between two computers, from an open P2PLink to the end
// of play:
//
//  1. Handshake: each side sends a hello (versions, the ROM's hash, its player's
//     console settings, its save's size). Different ROMs end the session here.
//  2. The host (player 1) decides the settings (input delay, prediction limit,
//     clock) and sends them.
//  3. Each side sends its save; then both build the same machine (one console
//     per player, each with its own player's firmware settings and save) and
//     compare a hash of it.
//  4. GGPO synchronizes, and the game runs: RunFrame each frame.
//
// During play the stream carries desync checks (a hash of every 60th frame
// once it is final), chat and the leave notice. Call everything from one thread.
class OnlineSession
{
public:
    enum class Phase
    {
        WaitingForLink,     // the P2PLink is not ready yet
        Handshake,          // hellos, settings
        Transfer,           // saves
        Synchronizing,      // GGPO's synchronization
        Running,
        Ended,
    };

    // The link may still be connecting; the session starts when it is ready.
    OnlineSession(std::shared_ptr<P2PLink> link, bool isHost, LocalSetup&& local);
    ~OnlineSession();
    OnlineSession(const OnlineSession&) = delete;
    OnlineSession& operator=(const OnlineSession&) = delete;

    // Advances the setup and handles messages; call it every few milliseconds
    // until Running, then once per frame (RunFrame does not call it).
    void Poll();

    Phase GetPhase() const { return CurPhase; }
    static const char* PhaseName(Phase p);
    // what the player sees while the session starts ("Sending saves (40%)")
    std::string StatusText() const;
    // why it ended (empty if it has not)
    const std::string& EndReason() const { return Reason; }
    bool IsHost() const { return Host; }

    // Once Synchronizing or Running:
    NetplayMachine* Machine() { return MachinePtr.get(); }
    int LocalPlayer() const { return Host ? 0 : 1; }
    int NumPlayers() const { return 2; }
    const std::string& PlayerName(int player) const;
    int InputDelay() const { return Settings.InputDelay; }

    // runs one frame (see RollbackSession::RunFrame); Ended afterwards if the session failed
    RollbackSession::FrameResult RunFrame(PlayerInput local);
    // GGPO's pacing: frames to wait before the next frame (RollbackSession::TakeFrameWait)
    double TakeFrameWait();
    SessionStats Stats();
    int RoundTripMs() const;

    void SendChat(const std::string& text);
    std::vector<ChatLine> TakeChat();

    // Ends the session, telling the other player why.
    void Leave(const std::string& reason = "");

    // The save memory of this player's console as it is now, for keeping
    // what the game saved during the session.
    std::vector<u8> LocalSave();

private:
    struct PeerHello
    {
        u32 Protocol = 0;
        std::string App, Version;
        u8 RomHash[16] {};
        std::string ROMTitle;
        PlayerProfile Profile;
        u32 SaveSize = 0;
        u64 SaveHash = 0;
        int InputDelay = -1, MaxPrediction = 8, CheckInterval = 60;
    };
    struct SessionSettings
    {
        int InputDelay = 2;
        int MaxPrediction = 8;
        int CheckInterval = 60;
        int Year = 2026, Month = 1, Day = 1, Hour = 12, Minute = 0, Second = 0;
        std::string ROMName;
        PlayerProfile Profiles[2];
        u32 SaveSize[2] {};
        u64 SaveHash[2] {};
    };

    void End(const std::string& reason, bool tellPeer);
    void SendHello();
    void SendSettings();
    void SendSaveChunks();
    void HandleMessage(u8 type, const std::vector<u8>& data);
    void TryBuild();
    void StartRollback();
    void HandleSessionEvents();
    void SendHashes();

    std::shared_ptr<P2PLink> Link;
    bool Host;
    LocalSetup Local;
    Phase CurPhase = Phase::WaitingForLink;
    std::string Reason;

    u8 LocalRomHash[16] {};
    bool HelloSent = false, HelloReceived = false, SettingsKnown = false;
    std::chrono::steady_clock::time_point HelloTime;
    PeerHello Peer;
    SessionSettings Settings;

    // saves: [player]
    std::vector<u8> Saves[2];
    bool SaveComplete[2] {};
    size_t SaveSent = 0;
    bool SaveSendDone = false;

    std::unique_ptr<NetplayMachine> MachinePtr;
    std::unique_ptr<RollbackSession> Rollback;
    u64 LocalMachineHash = 0;
    bool ReadySent = false, PeerReady = false;
    u64 PeerMachineHash = 0;

    std::deque<ChatLine> Chat;
    std::string Names[2];
};

}

#endif // ONLINESESSION_H
