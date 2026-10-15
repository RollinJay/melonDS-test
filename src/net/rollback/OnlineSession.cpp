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
#include <chrono>
#include <cmath>
#include <cstring>

#include "OnlineSession.h"
#include "NetplayBytes.h"
#include "P2PLink.h"

#include "NDS.h"
#include "NDSCart.h"
#include "FreeBIOS.h"
#include "Platform.h"
#include "version.h"
#include "sha1/sha1.hpp"
#include "xxhash/xxhash.h"

namespace melonDS::RollbackNet
{

using Platform::Log;
using Platform::LogLevel;

// a peer that names another protocol version or application is refused
static constexpr u32 kProtocolVersion = kOnlineProtocolVersion;
static constexpr const char* kApp = kOnlineApp;

void OnlineROMHash(const u8* rom, size_t len, u8 out[16])
{
    XXH128_hash_t h = XXH3_128bits(rom, len);
    for (int i = 0; i < 8; i++)
    {
        out[i] = (u8)(h.low64 >> (i * 8));
        out[8 + i] = (u8)(h.high64 >> (i * 8));
    }
}

static std::string Hex(const u8* data, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++)
    {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0xF]);
    }
    return out;
}

std::string NetplayIdentity(const u8* rom, size_t len)
{
    u8 romHash[16];
    OnlineROMHash(rom, len, romHash);
    std::string text = std::string(kApp) + "/" + std::to_string(kProtocolVersion) + "\n" + Hex(romHash, sizeof romHash);

    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, (const unsigned char*)text.data(), (uint32_t)text.size());
    u8 digest[20];
    SHA1Final(digest, &ctx);
    return Hex(digest, sizeof digest);
}

enum MessageType : u8
{
    Msg_Hello = 0x01,       // see SendHello
    Msg_Settings = 0x02,    // host -> guest, see SendSettings
    Msg_Save = 0x03,        // u8 player, u32 offset, u32 total, data
    Msg_Ready = 0x04,       // u64 hash of the machine just built
    Msg_Hash = 0x10,        // u32 frame, u64 hash: desync check
    Msg_Chat = 0x11,        // text
    Msg_Bye = 0x12,         // reason
    Msg_Error = 0x13,       // the session cannot go on: text
};

static constexpr size_t kSaveChunk = 32 * 1024;
static constexpr size_t kSaveWindow = 256 * 1024;     // unacknowledged bytes at most
static constexpr size_t kMaxSave = 32u << 20;         // DSi-size NAND saves are not supported

// ---- firmware ----------------------------------------------------------------

static std::u16string ToUTF16(const std::string& s, size_t maxchars)
{
    std::u16string out;
    for (size_t i = 0; i < s.size() && out.size() < maxchars;)
    {
        unsigned char c = s[i];
        u32 cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { i++; continue; }
        if (i + len > s.size()) break;
        for (int k = 1; k < len; k++)
            cp = (cp << 6) | (s[i + k] & 0x3F);
        i += len;
        if (cp > 0xFFFF) cp = '?';      // the DS's font has no characters there
        out.push_back((char16_t)cp);
    }
    return out;
}

Firmware MakeNetplayFirmware(int index, const PlayerProfile& profile)
{
    Firmware fw(0);
    auto& user = fw.GetEffectiveUserData();

    std::u16string name = ToUTF16(profile.Name.empty() ? std::string("Player") : profile.Name, 10);
    memset(user.Nickname, 0, sizeof(user.Nickname));
    memcpy(user.Nickname, name.data(), name.size() * sizeof(char16_t));
    user.NameLength = (u16)name.size();

    std::u16string msg = ToUTF16(profile.Message, 26);
    memset(user.Message, 0, sizeof(user.Message));
    memcpy(user.Message, msg.data(), msg.size() * sizeof(char16_t));
    user.MessageLength = (u16)msg.size();

    u8 lang = profile.Language <= Firmware::Language::Spanish ? profile.Language : Firmware::Language::English;
    user.Settings &= ~Firmware::Language::Reserved;
    user.Settings |= lang;

    user.FavoriteColor = profile.FavoriteColor & 0xF;
    user.BirthdayMonth = std::clamp<u8>(profile.BirthdayMonth, 1, 12);
    user.BirthdayDay = std::clamp<u8>(profile.BirthdayDay, 1, 31);

    // each console needs its own MAC address for local wireless
    auto& header = fw.GetHeader();
    header.MacAddr = {0x00, 0x09, 0xBF, 0x4E, 0x50, (u8)(0x10 + index)};
    header.UpdateChecksum();
    fw.UpdateChecksums();
    return fw;
}

static void PutProfile(ByteWriter& w, const PlayerProfile& p)
{
    w.Str(p.Name);
    w.Str(p.Message);
    w.U8(p.Language);
    w.U8(p.BirthdayMonth);
    w.U8(p.BirthdayDay);
    w.U8(p.FavoriteColor);
}

static void GetProfile(ByteReader& r, PlayerProfile& p)
{
    r.Str(p.Name, 64);
    r.Str(p.Message, 128);
    p.Language = r.U8();
    p.BirthdayMonth = r.U8();
    p.BirthdayDay = r.U8();
    p.FavoriteColor = r.U8();
}

// ---- session -----------------------------------------------------------------

OnlineSession::OnlineSession(std::shared_ptr<P2PLink> link, bool isHost, LocalSetup&& local)
    : Link(std::move(link)), Host(isHost), Local(std::move(local))
{
    if (Local.Version.empty())
        Local.Version = MELONDS_VERSION;

    OnlineROMHash(Local.ROM.data(), Local.ROM.size(), LocalRomHash);

    Saves[LocalPlayer()] = Local.Save;
    SaveComplete[LocalPlayer()] = true;
    Names[LocalPlayer()] = Local.Profile.Name;
}

OnlineSession::~OnlineSession()
{
    if (CurPhase != Phase::Ended)
        Leave("");
}

const char* OnlineSession::PhaseName(Phase p)
{
    switch (p)
    {
    case Phase::WaitingForLink: return "connecting";
    case Phase::Handshake: return "handshake";
    case Phase::Transfer: return "transfer";
    case Phase::Synchronizing: return "synchronizing";
    case Phase::Running: return "running";
    case Phase::Ended: return "ended";
    }
    return "?";
}

const std::string& OnlineSession::PlayerName(int player) const
{
    static const std::string none;
    return (player == 0 || player == 1) ? Names[player] : none;
}

std::string OnlineSession::StatusText() const
{
    switch (CurPhase)
    {
    case Phase::WaitingForLink: return "Connecting to the other player...";
    case Phase::Handshake: return "Comparing games and settings...";
    case Phase::Transfer:
        {
            // what this computer sent, and what it received of the other's save
            size_t total = Local.Save.size() + Peer.SaveSize;
            size_t done = std::min(SaveSent, Local.Save.size()) +
                          std::min<size_t>(Saves[1 - LocalPlayer()].size(), Peer.SaveSize);
            int pct = total ? (int)(100.0 * done / total) : 100;
            return "Exchanging saves (" + std::to_string(pct) + "%)...";
        }
    case Phase::Synchronizing: return "Synchronizing...";
    case Phase::Running: return "Playing";
    case Phase::Ended: return Reason.empty() ? "The session ended." : Reason;
    }
    return "";
}

void OnlineSession::End(const std::string& reason, bool tellPeer, EndKind kind, const std::string& peerReason)
{
    if (CurPhase == Phase::Ended)
        return;
    if (tellPeer && Link && Link->GetState() == P2PLink::State::Ready)
    {
        // a failure fails the other player's session too; a player leaving does not
        std::vector<u8> msg;
        ByteWriter w(msg);
        if (kind == EndKind::Failed)
        {
            w.Str("The other player's melonDS ended the session: " + reason);
            Link->Send(Msg_Error, msg);
        }
        else
        {
            w.Str(peerReason);
            Link->Send(Msg_Bye, msg);
        }
    }
    Reason = reason;
    Ending = kind;
    CurPhase = Phase::Ended;
    if (Rollback)
    {
        SessionStats st = Rollback->Stats();
        Log(LogLevel::Info, "netplay: %u frames, %llu rollbacks (%llu frames simulated again, deepest %d), "
            "%d desync checks passed (last at frame %u)\n",
            st.Frame, (unsigned long long)st.Rollbacks, (unsigned long long)st.FramesResimulated,
            st.MaxRollbackDepth, st.ChecksPassed, st.LastCheckedFrame);
        Rollback->Close();
    }
    Log(LogLevel::Info, "netplay: session ended: %s\n", reason.c_str());
}

void OnlineSession::Leave(const std::string& reason)
{
    End(reason.empty() ? "You left the session." : reason, true, EndKind::Left, reason);
}

void OnlineSession::Abort(const std::string& reason)
{
    End(reason, true, EndKind::Failed);
}

void OnlineSession::Poll()
{
    if (CurPhase == Phase::Ended || !Link)
        return;

    P2PLink::State ls = Link->GetState();
    if (ls == P2PLink::State::Failed || ls == P2PLink::State::Closed)
    {
        // the other player said goodbye (its bye on the stream may not have
        // been read), or the connection failed
        if (Link->PeerSaidBye())
        {
            End("The other player left.", false, EndKind::PeerLeft);
            return;
        }
        std::string err = Link->GetError();
        End(err.empty() ? "The connection closed." : err, false);
        return;
    }
    if (CurPhase == Phase::WaitingForLink)
    {
        if (ls != P2PLink::State::Ready)
            return;
        CurPhase = Phase::Handshake;
    }

    if (!HelloSent)
        SendHello();

    u8 type;
    std::vector<u8> data;
    while (CurPhase != Phase::Ended && Link->Receive(type, data))
        HandleMessage(type, data);
    if (CurPhase == Phase::Ended)
        return;

    if (HelloReceived && !SaveSendDone)
        SendSaveChunks();

    // an automatic input delay needs the round trip, measured within a second or two
    if (Host && HelloReceived && !SettingsKnown &&
        (Local.InputDelay >= 0 || Link->RoundTripMs() >= 0 ||
         std::chrono::steady_clock::now() - HelloTime > std::chrono::milliseconds(2500)))
        SendSettings();

    if (CurPhase == Phase::Handshake || CurPhase == Phase::Transfer)
        TryBuild();

    if (Rollback)
    {
        if (CurPhase == Phase::Synchronizing)
            Rollback->Idle();
        HandleSessionEvents();
        if (CurPhase == Phase::Running)
            SendHashes();
    }
}

void OnlineSession::SendHello()
{
    std::vector<u8> msg;
    ByteWriter w(msg);
    w.U32(kProtocolVersion);
    w.Str(kApp);
    w.Str(Local.Version);
    w.Bytes(LocalRomHash, 16);
    w.Str(Local.ROMName);
    PutProfile(w, Local.Profile);
    w.U32((u32)Local.Save.size());
    w.U64(XXH3_64bits(Local.Save.data(), Local.Save.size()));
    w.U8((u8)(s8)std::clamp(Local.InputDelay, -1, 15));
    w.U8((u8)std::clamp(Local.MaxPrediction, 1, 8));
    w.U16((u16)std::clamp(Local.CheckInterval, 0, 3600));
    Link->Send(Msg_Hello, msg);
    HelloSent = true;
}

void OnlineSession::SendSettings()
{
    SessionSettings& s = Settings;
    s.MaxPrediction = std::clamp(std::min(Local.MaxPrediction, Peer.MaxPrediction), 1, 8);
    s.CheckInterval = Local.CheckInterval;
    if (Local.InputDelay >= 0)
        s.InputDelay = std::min(Local.InputDelay, 15);
    else
    {
        // about the one-way trip, in frames: rollback covers the rest
        int rtt = Link->RoundTripMs();
        if (rtt < 0) rtt = 60;
        s.InputDelay = std::clamp((int)std::lround(rtt / 2.0 / 16.7), 1, 6);
    }
    s.Year = Local.Year; s.Month = Local.Month; s.Day = Local.Day;
    s.Hour = Local.Hour; s.Minute = Local.Minute; s.Second = Local.Second;
    s.ROMName = Local.ROMName.empty() ? std::string("game.nds") : Local.ROMName;
    s.Profiles[0] = Local.Profile;
    s.Profiles[1] = Peer.Profile;
    s.SaveSize[0] = (u32)Local.Save.size();
    s.SaveHash[0] = XXH3_64bits(Local.Save.data(), Local.Save.size());
    s.SaveSize[1] = Peer.SaveSize;
    s.SaveHash[1] = Peer.SaveHash;

    std::vector<u8> msg;
    ByteWriter w(msg);
    w.U8((u8)s.InputDelay);
    w.U8((u8)s.MaxPrediction);
    w.U16((u16)s.CheckInterval);
    w.U16((u16)s.Year);
    w.U8((u8)s.Month); w.U8((u8)s.Day);
    w.U8((u8)s.Hour); w.U8((u8)s.Minute); w.U8((u8)s.Second);
    w.Str(s.ROMName);
    for (int i = 0; i < 2; i++)
    {
        PutProfile(w, s.Profiles[i]);
        w.U32(s.SaveSize[i]);
        w.U64(s.SaveHash[i]);
    }
    Link->Send(Msg_Settings, msg);
    SettingsKnown = true;
    Names[0] = s.Profiles[0].Name;
    Names[1] = s.Profiles[1].Name;
}

void OnlineSession::SendSaveChunks()
{
    const std::vector<u8>& save = Local.Save;
    while (SaveSent < save.size() && Link->UnacknowledgedBytes() < kSaveWindow)
    {
        size_t n = std::min(kSaveChunk, save.size() - SaveSent);
        std::vector<u8> msg;
        ByteWriter w(msg);
        w.U8((u8)LocalPlayer());
        w.U32((u32)SaveSent);
        w.U32((u32)save.size());
        w.Bytes(save.data() + SaveSent, n);
        if (!Link->Send(Msg_Save, msg))
            return;
        SaveSent += n;
    }
    if (SaveSent >= save.size())
        SaveSendDone = true;
}

void OnlineSession::HandleMessage(u8 type, const std::vector<u8>& data)
{
    ByteReader r(data.data(), data.size());
    const int other = 1 - LocalPlayer();

    switch (type)
    {
    case Msg_Hello:
        {
            if (HelloReceived) return;
            Peer.Protocol = r.U32();
            r.Str(Peer.App, 64);
            r.Str(Peer.Version, 64);
            if (!r.Ok() || Peer.App != kApp)
            {
                End("The other player is not running a compatible melonDS.", true);
                return;
            }
            if (Peer.Protocol != kProtocolVersion)
            {
                End("The other player's melonDS (" + Peer.Version + ") speaks another netplay version than yours (" +
                    Local.Version + ").", true);
                return;
            }
            r.Bytes(Peer.RomHash, 16);
            r.Str(Peer.ROMTitle, 256);
            GetProfile(r, Peer.Profile);
            Peer.SaveSize = r.U32();
            Peer.SaveHash = r.U64();
            Peer.InputDelay = (s8)r.U8();
            Peer.MaxPrediction = r.U8();
            Peer.CheckInterval = r.U16();
            if (!r.Ok())
            {
                End("The other player sent a damaged greeting.", true);
                return;
            }
            if (memcmp(Peer.RomHash, LocalRomHash, 16) != 0)
            {
                End("You and the other player have different ROMs (" + Local.ROMName + " here, " +
                    Peer.ROMTitle + " there). Both need the same dump of the same game.", true);
                return;
            }
            if (Peer.SaveSize > kMaxSave)
            {
                End("The other player's save is too large to send.", true);
                return;
            }
            HelloReceived = true;
            HelloTime = std::chrono::steady_clock::now();
            Names[other] = Peer.Profile.Name;
            if (Peer.SaveSize == 0)
                SaveComplete[other] = true;
            // the host sends the settings from Poll, once the round trip is known
            if (CurPhase == Phase::Handshake)
                CurPhase = Phase::Transfer;
        }
        break;

    case Msg_Settings:
        {
            if (Host || SettingsKnown) return;
            SessionSettings& s = Settings;
            s.InputDelay = r.U8();
            s.MaxPrediction = r.U8();
            s.CheckInterval = r.U16();
            s.Year = r.U16();
            s.Month = r.U8(); s.Day = r.U8();
            s.Hour = r.U8(); s.Minute = r.U8(); s.Second = r.U8();
            r.Str(s.ROMName, 256);
            for (int i = 0; i < 2; i++)
            {
                GetProfile(r, s.Profiles[i]);
                s.SaveSize[i] = r.U32();
                s.SaveHash[i] = r.U64();
            }
            if (!r.Ok() || s.MaxPrediction < 1 || s.MaxPrediction > 8 || s.InputDelay > 15)
            {
                End("The host sent settings this version does not understand.", true);
                return;
            }
            SettingsKnown = true;
            Names[0] = s.Profiles[0].Name;
            Names[1] = s.Profiles[1].Name;
        }
        break;

    case Msg_Save:
        {
            int player = r.U8();
            u32 offset = r.U32();
            u32 total = r.U32();
            size_t n = r.Left();
            if (!r.Ok() || player != other || total > kMaxSave || total != Peer.SaveSize ||
                offset != Saves[other].size() || offset + n > total)
            {
                End("The other player's save arrived damaged.", true);
                return;
            }
            size_t at = Saves[other].size();
            Saves[other].resize(at + n);
            r.Bytes(Saves[other].data() + at, n);
            if (Saves[other].size() == total)
            {
                if (XXH3_64bits(Saves[other].data(), total) != Peer.SaveHash)
                {
                    End("The other player's save arrived damaged.", true);
                    return;
                }
                SaveComplete[other] = true;
            }
        }
        break;

    case Msg_Ready:
        PeerMachineHash = r.U64();
        if (!r.Ok()) return;
        PeerReady = true;
        if (MachinePtr && ReadySent)
            StartRollback();
        break;

    case Msg_Hash:
        if (Rollback)
        {
            FrameHash h;
            h.Frame = r.U32();
            h.Hash = r.U64();
            if (r.Ok())
                Rollback->AddRemoteHash(other, h);
        }
        break;

    case Msg_Chat:
        {
            std::string text;
            if (r.Str(text, 512))
                Chat.push_back({other, text});
        }
        break;

    case Msg_Bye:
        {
            std::string why;
            r.Str(why, 512);
            End(why.empty() ? "The other player left." : "The other player left: " + why, false, EndKind::PeerLeft);
        }
        break;

    case Msg_Error:
        {
            std::string why;
            r.Str(why, 512);
            End(why, false);
        }
        break;

    default:
        // a newer version's message: ignored
        break;
    }
}

void OnlineSession::TryBuild()
{
    if (MachinePtr || !HelloReceived || !SettingsKnown || !SaveComplete[0] || !SaveComplete[1])
        return;

    // the saves must be the ones announced in the settings
    for (int i = 0; i < 2; i++)
    {
        if (Saves[i].size() != Settings.SaveSize[i] ||
            XXH3_64bits(Saves[i].data(), Saves[i].size()) != Settings.SaveHash[i])
        {
            End("The saves do not match the session's settings.", true);
            return;
        }
    }

    MachineConfig cfg;
    cfg.ROM = Local.ROM;
    cfg.ROMName = Settings.ROMName;
    cfg.ARM9BIOS = FreeBIOSGetNtrArm9();
    cfg.ARM7BIOS = FreeBIOSGetNtrArm7();
    cfg.DirectBoot = true;
    cfg.Year = Settings.Year; cfg.Month = Settings.Month; cfg.Day = Settings.Day;
    cfg.Hour = Settings.Hour; cfg.Minute = Settings.Minute; cfg.Second = Settings.Second;
    for (int i = 0; i < 2; i++)
    {
        MachineConfig::Console c;
        c.FirmwareImage = MakeNetplayFirmware(i, Settings.Profiles[i]);
        c.Save = Saves[i];
        cfg.Consoles.push_back(std::move(c));
    }

    MachinePtr = std::make_unique<NetplayMachine>(std::move(cfg), Local.AudioSampleRate);
    if (!MachinePtr->IsValid())
    {
        End("The game could not be started: " + MachinePtr->Error(), true);
        return;
    }
    MachinePtr->SetLocalConsole(LocalPlayer());
    if (Local.Threads > 0)
        MachinePtr->SetThreadCount(Local.Threads);

    MachineState st;
    if (!MachinePtr->SaveState(st))
    {
        End("The game could not be started.", true);
        return;
    }
    LocalMachineHash = NetplayMachine::HashState(st);

    std::vector<u8> msg;
    ByteWriter w(msg);
    w.U64(LocalMachineHash);
    Link->Send(Msg_Ready, msg);
    ReadySent = true;
    CurPhase = Phase::Synchronizing;

    if (PeerReady)
        StartRollback();
}

void OnlineSession::StartRollback()
{
    if (Rollback)
        return;
    if (PeerMachineHash != LocalMachineHash)
    {
        End("Your game and the other player's start differently (different melonDS versions?).", true);
        return;
    }

    SessionConfig sc;
    sc.LocalPlayer = LocalPlayer();
    sc.Transport = Link->Transport();
    NetAddress peer = Link->PeerAddress();
    sc.Peers.resize(2);
    sc.Peers[1 - LocalPlayer()].Address = peer.IPString();
    sc.Peers[1 - LocalPlayer()].Port = peer.Port;
    sc.InputDelay = Settings.InputDelay;
    sc.MaxPrediction = Settings.MaxPrediction;
    sc.CheckInterval = Settings.CheckInterval;

    Rollback = std::make_unique<RollbackSession>(*MachinePtr);
    std::string err;
    if (!Rollback->Start(sc, err))
    {
        End(err, true);
        return;
    }
    Log(LogLevel::Info, "netplay: rollback session started (player %d, delay %d, prediction %d)\n",
        LocalPlayer() + 1, sc.InputDelay, sc.MaxPrediction);
}

void OnlineSession::HandleSessionEvents()
{
    for (const SessionEvent& ev : Rollback->TakeEvents())
    {
        switch (ev.Kind)
        {
        case SessionEvent::Running:
            CurPhase = Phase::Running;
            Chat.push_back({-1, "Connected. Input delay: " + std::to_string(Settings.InputDelay) + " frame(s)."});
            break;
        case SessionEvent::Interrupted:
            Chat.push_back({-1, "The connection is interrupted..."});
            break;
        case SessionEvent::Resumed:
            Chat.push_back({-1, "The connection is back."});
            break;
        case SessionEvent::Disconnected:
            End("The other player disconnected.", false);
            return;
        case SessionEvent::Desync:
            End("The games went out of sync at frame " + std::to_string(ev.Frame) +
                ": both computers no longer run the same game. This is a bug; please report it with the game's name.", true);
            return;
        case SessionEvent::Failed:
            End(ev.Text, true);
            return;
        default:
            break;
        }
    }
}

void OnlineSession::SendHashes()
{
    for (const FrameHash& h : Rollback->TakeLocalHashes())
    {
        std::vector<u8> msg;
        ByteWriter w(msg);
        w.U32(h.Frame);
        w.U64(h.Hash);
        Link->Send(Msg_Hash, msg);
    }
}

RollbackSession::FrameResult OnlineSession::RunFrame(PlayerInput local)
{
    if (CurPhase != Phase::Running || !Rollback)
        return RollbackSession::FrameResult::Waiting;

    RollbackSession::FrameResult res = Rollback->RunFrame(local);
    if (res == RollbackSession::FrameResult::Failed)
        HandleSessionEvents();
    return res;
}

double OnlineSession::TakeFrameWait()
{
    return Rollback ? Rollback->TakeFrameWait() : 0.0;
}

SessionStats OnlineSession::Stats()
{
    return Rollback ? Rollback->Stats() : SessionStats {};
}

int OnlineSession::RoundTripMs() const
{
    return Link ? Link->RoundTripMs() : -1;
}

void OnlineSession::SendChat(const std::string& text)
{
    if (text.empty() || !Link || CurPhase == Phase::Ended) return;
    std::vector<u8> msg;
    ByteWriter w(msg);
    w.Str(text.substr(0, 200));
    if (Link->Send(Msg_Chat, msg))
        Chat.push_back({LocalPlayer(), text.substr(0, 200)});
}

std::vector<ChatLine> OnlineSession::TakeChat()
{
    std::vector<ChatLine> out(Chat.begin(), Chat.end());
    Chat.clear();
    return out;
}

std::vector<u8> OnlineSession::LocalSave()
{
    std::vector<u8> out;
    if (!MachinePtr) return out;
    NDS* nds = MachinePtr->Console(LocalPlayer());
    const u8* data = nds->GetNDSSave();
    u32 len = nds->GetNDSSaveLength();
    if (data && len)
        out.assign(data, data + len);
    return out;
}

}
