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
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define NETPLAY_CPU_RELAX() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
#define NETPLAY_CPU_RELAX() __asm__ __volatile__("yield")
#else
#define NETPLAY_CPU_RELAX() do {} while (0)
#endif

#include "NetplayMachine.h"
#include "NetplayBytes.h"

#include "NDS.h"
#include "NDSCart.h"
#include "GPU.h"
#include "GPU_Soft.h"
#include "Savestate.h"
#include "Platform.h"
#include "xxhash/xxhash.h"

namespace melonDS::RollbackNet
{

using Platform::Log;
using Platform::LogLevel;

// ---- userdata registry -------------------------------------------------------
//
// The NDS objects of a machine get a ConsoleSlot as their userdata. The frontend's
// Platform callbacks receive that pointer and must know whether it is one of
// ours or one of their own (EmuInstance). Slots are registered here while they
// exist; lookups compare pointers only, without a lock, since the consoles of a
// machine call in from several threads at once.

static constexpr int kMaxSlots = 64;
static std::atomic<ConsoleSlot*> SlotRegistry[kMaxSlots];
static std::mutex SlotRegistryLock;     // for registering and unregistering only

static bool RegisterSlot(ConsoleSlot* slot)
{
    std::lock_guard lock(SlotRegistryLock);
    for (auto& entry : SlotRegistry)
    {
        if (!entry.load())
        {
            entry.store(slot);
            return true;
        }
    }
    return false;
}

static void UnregisterSlot(ConsoleSlot* slot)
{
    std::lock_guard lock(SlotRegistryLock);
    for (auto& entry : SlotRegistry)
    {
        if (entry.load() == slot)
            entry.store(nullptr);
    }
}

ConsoleSlot* ConsoleSlot::From(void* userdata)
{
    if (!userdata) return nullptr;
    for (auto& entry : SlotRegistry)
    {
        ConsoleSlot* slot = entry.load(std::memory_order_acquire);
        if (slot == userdata)
            return slot;
    }
    return nullptr;
}

void ConsoleSlot::MPBegin() { Owner->MPBus.Begin(Idx, SysTime()); }
void ConsoleSlot::MPEnd() { Owner->MPBus.End(Idx, SysTime()); }
u64 ConsoleSlot::SysTime() const { return Owner->Consoles[Idx]->GetSysTimestamp(); }
int ConsoleSlot::MPSendPacket(u8* data, int len, u64 timestamp) { return Owner->MPBus.SendPacket(Idx, SysTime(), data, len, timestamp); }
int ConsoleSlot::MPRecvPacket(u8* data, u64* timestamp) { return Owner->MPBus.RecvPacket(Idx, SysTime(), data, timestamp); }
int ConsoleSlot::MPSendCmd(u8* data, int len, u64 timestamp) { return Owner->MPBus.SendCmd(Idx, SysTime(), data, len, timestamp); }
int ConsoleSlot::MPSendReply(u8* data, int len, u64 timestamp, u16 aid) { return Owner->MPBus.SendReply(Idx, SysTime(), data, len, timestamp, aid); }
int ConsoleSlot::MPSendAck(u8* data, int len, u64 timestamp) { return Owner->MPBus.SendAck(Idx, SysTime(), data, len, timestamp); }
int ConsoleSlot::MPRecvHostPacket(u8* data, u64* timestamp) { return Owner->MPBus.RecvHostPacket(Idx, SysTime(), data, timestamp); }
u16 ConsoleSlot::MPRecvReplies(u8* data, u64 timestamp, u16 aidmask) { return Owner->MPBus.RecvReplies(Idx, data, timestamp, aidmask); }

int ConsoleSlot::MicReadInput(s16* data, int maxlength)
{
    if (maxlength > 0)
        memset(data, 0, maxlength * sizeof(s16));
    return std::max(maxlength, 0);
}

void ConsoleSlot::SignalStop()
{
    Owner->Stopped = true;
}

// ---- threads -----------------------------------------------------------------
//
// Runs a job for every console, spread over a fixed set of threads: console i
// always goes to thread i % count, the calling thread taking its share. A job
// is a round of a frame (tens of microseconds of work), so the threads wait for
// the next job by spinning for a while before they sleep.

// Spins for up to a given time: Step() returns false once it is over.
class SpinWait
{
public:
    explicit SpinWait(int microseconds)
        : Until(std::chrono::steady_clock::now() + std::chrono::microseconds(microseconds)) {}

    bool Step()
    {
        NETPLAY_CPU_RELAX();
        if ((++Count & 63) != 0)
            return true;
        return std::chrono::steady_clock::now() < Until;
    }

private:
    std::chrono::steady_clock::time_point Until;
    unsigned Count = 0;
};

class NetplayMachine::Pool
{
public:
    explicit Pool(int threads)
        : Count(std::max(threads, 1))
    {
        Done = std::make_unique<std::atomic<u64>[]>(Count);
        for (int t = 0; t < Count; t++)
            Done[t].store(0);
        for (int t = 1; t < Count; t++)
            Workers.emplace_back([this, t] { WorkerLoop(t); });
    }

    ~Pool()
    {
        {
            std::lock_guard lock(SleepLock);
            Quit = true;
        }
        Generation.fetch_add(1);
        Wake.notify_all();
        for (auto& w : Workers)
            w.join();
    }

    int ThreadCount() const { return Count; }

    // job(i) for every i in [0, n), each on thread i % ThreadCount()
    void Run(int n, const std::function<void(int)>& job)
    {
        if (Count == 1 || n <= 1)
        {
            for (int i = 0; i < n; i++)
                job(i);
            return;
        }

        Job = &job;
        Jobs = n;
        u64 gen = Generation.fetch_add(1) + 1;
        if (Sleepers.load() > 0)
        {
            std::lock_guard lock(SleepLock);
            Wake.notify_all();
        }

        for (int i = 0; i < n; i += Count)
            job(i);

        // every thread, also those without a job this time: none may still be
        // finishing this job when the next one starts
        for (int t = 1; t < Count; t++)
        {
            // a round takes microseconds: spin, but let other threads run if this
            // one waits much longer (the worker was preempted)
            SpinWait spin(200);
            while (Done[t].load(std::memory_order_acquire) != gen)
            {
                if (!spin.Step())
                    std::this_thread::yield();
            }
        }
        Job = nullptr;
    }

private:
    void WorkerLoop(int t)
    {
        u64 seen = 0;
        for (;;)
        {
            u64 gen;
            SpinWait spin(300);
            while ((gen = Generation.load(std::memory_order_acquire)) == seen)
            {
                if (spin.Step())
                    continue;
                // nothing for a while (between frames): sleep
                std::unique_lock lock(SleepLock);
                Sleepers.fetch_add(1);
                Wake.wait_for(lock, std::chrono::milliseconds(50),
                              [&] { return Quit || Generation.load() != seen; });
                Sleepers.fetch_sub(1);
                spin = SpinWait(300);
            }
            if (Quit)
                return;
            seen = gen;

            if (t < Jobs)
            {
                for (int i = t; i < Jobs; i += Count)
                    (*Job)(i);
            }
            Done[t].store(gen, std::memory_order_release);
        }
    }

    const int Count;
    std::vector<std::thread> Workers;
    std::atomic<u64> Generation {0};
    std::unique_ptr<std::atomic<u64>[]> Done;
    const std::function<void(int)>* Job = nullptr;
    int Jobs = 0;

    std::mutex SleepLock;
    std::condition_variable Wake;
    std::atomic<int> Sleepers {0};
    bool Quit = false;
};

// ---- machine -----------------------------------------------------------------

static constexpr u32 kMachineStateMagic = 0x484D504E; // "NPMH"
static constexpr u32 kMachineStateVersion = 2;

NetplayMachine::NetplayMachine(MachineConfig&& config, double audioSampleRate)
    : MPBus((int)config.Consoles.size())
{
    int n = (int)config.Consoles.size();
    if (n < 1 || n > NetplayBus::kMaxConsoles)
    {
        ErrorText = "A netplay session needs between 1 and 16 consoles.";
        return;
    }
    if (config.ROM.empty())
    {
        ErrorText = "No ROM.";
        return;
    }

    for (int i = 0; i < n; i++)
    {
        auto slot = std::make_unique<ConsoleSlot>();
        slot->Owner = this;
        slot->Idx = i;
        if (!RegisterSlot(slot.get()))
        {
            ErrorText = "Too many netplay consoles at once.";
            return;
        }

        NDSArgs args;
        args.ARM9BIOS = std::make_unique<ARM9BIOSImage>(config.ARM9BIOS);
        args.ARM7BIOS = std::make_unique<ARM7BIOSImage>(config.ARM7BIOS);
        args.Firmware = config.Consoles[i].FirmwareImage;
        args.JIT = std::nullopt;
        args.GDB = std::nullopt;
        args.BitDepth = config.BitDepth;
        args.Interpolation = config.Interpolation;
        args.OutputSampleRate = audioSampleRate;

        auto nds = std::make_unique<NDS>(std::move(args), slot.get());

        // protection unit maps that depend only on the registers: rollback loads
        // keep them when the registers did not change (ARMv5::CP15DoSavestate)
        nds->ARM9.PUMapsFollowRegisters = true;

        // The software renderer's threaded mode renders the 3D image again on its own
        // thread each time a state is saved or loaded, from texture caches that a load
        // does not restore. Rollback saves every frame, so the 3D image is rendered on
        // the emulation thread and saved with the state (Renderer::DoRollbackState).
        RendererSettings rs {};
        rs.ScaleFactor = 1;
        rs.Threaded = false;
        rs.HiresCoordinates = false;
        rs.BetterPolygons = false;
        nds->GetRenderer().SetRenderSettings(rs);

        const std::vector<u8>& save = config.Consoles[i].Save;
        std::unique_ptr<u8[]> sram;
        if (!save.empty())
        {
            sram = std::make_unique<u8[]>(save.size());
            memcpy(sram.get(), save.data(), save.size());
        }
        NDSCart::NDSCartArgs cartargs {
            .SDCard = std::nullopt,
            .SRAM = std::move(sram),
            .SRAMLength = (u32)save.size(),
        };

        auto cart = NDSCart::ParseROM(config.ROM.data(), (u32)config.ROM.size(), slot.get(), std::move(cartargs));
        if (!cart)
        {
            ErrorText = "The ROM could not be loaded.";
            Slots.push_back(std::move(slot));
            return;
        }

        nds->SetNDSCart(std::move(cart));
        nds->Reset();

        if (config.DirectBoot || nds->NeedsDirectBoot())
            nds->SetupDirectBoot(config.ROMName);

        nds->RTC.SetDateTime(config.Year, config.Month, config.Day, config.Hour, config.Minute, config.Second);
        nds->Wifi.SetDeterministicMP(true);
        nds->SPU.SetOutputEnabled(false);
        nds->Start();

        Consoles.push_back(std::move(nds));
        Slots.push_back(std::move(slot));
    }

    ConsoleSaveRoom.assign(n, 0);
    LastInput.assign(n, 0);
    SetThreadCount(0);
    Valid = true;

    // A freshly booted console is not quite the console its savestate describes:
    // loading a state recomputes some tables (memory timings, ...) that direct boot
    // leaves otherwise. Start from a loaded state, so that rolling back to frame 0
    // gives the same console as the first run of frame 0.
    MachineState initial;
    if (!SaveState(initial) || !LoadState(initial))
    {
        Valid = false;
        ErrorText = "The consoles could not be saved and restored.";
    }
}

NetplayMachine::~NetplayMachine()
{
    Threads.reset();
    // the consoles call into Platform (through their slots) while shutting down
    Consoles.clear();
    for (auto& slot : Slots)
        UnregisterSlot(slot.get());
}

void NetplayMachine::SetThreadCount(int threads)
{
    // The threads spin while they wait for each other (a round lasts microseconds),
    // which only pays when each has a core of its own: by default, one per console
    // up to the number of cores.
    int cores = (int)std::thread::hardware_concurrency();
    if (threads <= 0)
        threads = cores > 0 ? cores : 1;
    threads = std::clamp(threads, 1, std::max(1, NumConsoles()));
    if (Threads && Threads->ThreadCount() == threads)
        return;
    Threads.reset();
    Threads = std::make_unique<Pool>(threads);
}

int NetplayMachine::ThreadCount() const
{
    return Threads ? Threads->ThreadCount() : 1;
}

bool NetplayMachine::UsesAsyncRender() const
{
    if (AsyncRender >= 0)
        return AsyncRender > 0;
    // by default, when a core is left over by the consoles' threads
    int cores = (int)std::thread::hardware_concurrency();
    return cores > ThreadCount();
}

void NetplayMachine::SetLocalConsole(int idx)
{
    Local = idx;
}

void NetplayMachine::RunFrame(const PlayerInput* inputs, bool resim, bool beforeShown)
{
    const int n = NumConsoles();
    bool done[NetplayBus::kMaxConsoles] {};
    bool run[NetplayBus::kMaxConsoles] {};

    for (int i = 0; i < n; i++)
    {
        NDS* nds = Consoles[i].get();
        PlayerInput in = inputs[i].Sanitized();

        nds->SetKeyMask(in.KeyMask());
        if (in.Touching())
            nds->TouchScreen(in.TouchX(), in.TouchY());
        else
            nds->ReleaseScreen();

        bool shown = !resim && i == Local;
        bool render3D = i == Local && (!resim || beforeShown);
        nds->SPU.SetOutputEnabled(shown);
        nds->GetRenderer().SetHeadless(HeadlessEnabled && !shown, render3D);
        nds->GetRenderer().SetAsyncRender(render3D && UsesAsyncRender());
        nds->BeginFrame();
        LastInput[i] = in.Bits;
    }

    // The consoles advance in rounds. While none has its wireless switched on, a
    // round is a console's whole frame. Otherwise every console runs up to the
    // same point of a shared clock, kSliceCycles at a time, so that what one sends
    // reaches the others about when it would on hardware. What they send reaches
    // the others at the end of the round (NetplayBus), so the consoles of a round
    // are independent of each other and run in parallel. Every decision here
    // depends only on the consoles' state, so every computer takes the same steps.
    u64 limit = UINT64_MAX;
    std::function<void(int)> job = [&](int i)
    {
        if (run[i] && Consoles[i]->RunFrameSlice(limit))
            done[i] = true;
    };

    int left = n;
    int round = 0;
    while (left > 0)
    {
        bool wifi = false;
        u64 base = UINT64_MAX;
        for (int i = 0; i < n; i++)
        {
            if (done[i]) continue;
            NDS* nds = Consoles[i].get();
            wifi |= nds->IsWifiPowered();
            base = std::min(base, nds->GetSysTimestamp());
        }

        limit = UINT64_MAX;
        if (wifi || AlwaysSlice)
            limit = ((base / kSliceCycles) + 1) * kSliceCycles;

        for (int i = 0; i < n; i++)
            run[i] = !done[i] && Consoles[i]->GetSysTimestamp() < limit;

        Threads->Run(n, job);

        left = 0;
        for (int i = 0; i < n; i++)
            left += done[i] ? 0 : 1;

        // what the consoles sent during the round reaches the others now
        MPBus.Commit();

        if (RoundHook)
            RoundHook(round);
        round++;
    }

    for (int i = 0; i < n; i++)
        Consoles[i]->EndFrame();

    Frame++;
}

// ---- states --------------------------------------------------------------------
//
// melonDS savestates do not keep the key input (it is set before each frame) or the
// touch screen position, but a key interrupt fires on a change from the previous key
// input, so both are kept with each console's savestate and restored as they were.

bool NetplayMachine::SaveConsole(int idx, MachineState::Console& out)
{
    NDS* nds = Consoles[idx].get();
    out.KeyInput = nds->KeyInput;
    out.LastInput = LastInput[idx];

    // Save straight into the buffer, which grows until a state fits. A state's
    // size changes from frame to frame (the 3D vertices in use, the textures of a
    // 3D image a headless console has not drawn yet), so leave room.
    for (int attempt = 0; attempt < 8; attempt++)
    {
        size_t room = std::max<size_t>(ConsoleSaveRoom[idx], 8 * 1024 * 1024);
        if (out.Data.size() < room)
            out.Data.resize(room);

        Savestate state(out.Data.data(), (u32)out.Data.size(), true);
        state.RollbackState = true;
        if (nds->DoSavestate(&state) && !state.Error)
        {
            out.Length = state.Length();
            // leave 2 MB more than the largest state so far
            ConsoleSaveRoom[idx] = std::max(ConsoleSaveRoom[idx], out.Length + 2 * 1024 * 1024);
            return true;
        }
        ConsoleSaveRoom[idx] = out.Data.size() * 2;
    }
    Log(LogLevel::Error, "netplay: could not save console %d\n", idx);
    return false;
}

bool NetplayMachine::LoadConsole(int idx, const MachineState::Console& in)
{
    NDS* nds = Consoles[idx].get();
    if (in.Length > in.Data.size() || in.Length > UINT32_MAX)
        return false;

    Savestate state(const_cast<u8*>(in.Data.data()), (u32)in.Length, false);
    state.RollbackState = true;
    if (state.Error || !nds->DoSavestate(&state) || state.Error)
    {
        Log(LogLevel::Error, "netplay: could not load console %d\n", idx);
        return false;
    }

    PlayerInput last;
    last.Bits = in.LastInput;
    if (last.Touching())
        nds->TouchScreen(last.TouchX(), last.TouchY());
    else
        nds->ReleaseScreen();
    nds->KeyInput = in.KeyInput;
    LastInput[idx] = in.LastInput;
    return true;
}

bool NetplayMachine::SaveState(MachineState& out)
{
    const int n = NumConsoles();
    out.Frame = Frame;
    if ((int)out.Consoles.size() != n)
        out.Consoles.resize(n);

    bool ok[NetplayBus::kMaxConsoles] {};
    Threads->Run(n, [&](int i) { ok[i] = SaveConsole(i, out.Consoles[i]); });
    for (int i = 0; i < n; i++)
    {
        if (!ok[i]) return false;
    }

    out.Bus.clear();
    MPBus.Save(out.Bus);
    return true;
}

bool NetplayMachine::LoadState(const MachineState& in)
{
    const int n = NumConsoles();
    if ((int)in.Consoles.size() != n)
        return false;

    bool ok[NetplayBus::kMaxConsoles] {};
    Threads->Run(n, [&](int i) { ok[i] = LoadConsole(i, in.Consoles[i]); });
    for (int i = 0; i < n; i++)
    {
        if (!ok[i]) return false;
    }

    if (MPBus.Load(in.Bus.data(), in.Bus.size()) != in.Bus.size())
        return false;

    Frame = in.Frame;
    return true;
}

// Single-buffer layout (little-endian):
//   u32 magic, u32 version, u32 frame, u8 console count
//   per console: u32 key input, u32 last input given (PlayerInput), u32 length, savestate
//   u32 bus state length, bus state

static u32 GetU32(const u8* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

void MachineState::Serialize(std::vector<u8>& out) const
{
    out.clear();
    ByteWriter w(out);
    w.U32(kMachineStateMagic);
    w.U32(kMachineStateVersion);
    w.U32(Frame);
    w.U8((u8)Consoles.size());
    for (const Console& c : Consoles)
    {
        w.U32(c.KeyInput);
        w.U32(c.LastInput);
        w.U32((u32)c.Length);
        w.Bytes(c.Data.data(), c.Length);
    }
    w.Blob(Bus);
}

bool MachineState::Deserialize(const u8* data, size_t len)
{
    ByteReader r(data, len);
    if (r.U32() != kMachineStateMagic || r.U32() != kMachineStateVersion)
        return false;
    Frame = r.U32();
    int n = r.U8();
    if (!r.Ok() || n < 1 || n > NetplayBus::kMaxConsoles)
        return false;
    Consoles.resize(n);
    for (Console& c : Consoles)
    {
        c.KeyInput = r.U32();
        c.LastInput = r.U32();
        u32 statelen = r.U32();
        if (!r.Ok() || statelen > len)
            return false;
        c.Data.resize(statelen);
        if (!r.Bytes(c.Data.data(), statelen))
            return false;
        c.Length = statelen;
    }
    return r.Blob(Bus, len) && r.Ok();
}

bool NetplayMachine::SaveState(std::vector<u8>& out)
{
    MachineState state;
    if (!SaveState(state))
        return false;
    state.Serialize(out);
    return true;
}

bool NetplayMachine::LoadState(const u8* data, size_t len)
{
    MachineState state;
    if (!state.Deserialize(data, len))
        return false;
    return LoadState(state);
}

// the hash of one console's savestate without its GPUR sections or any length
// that they change
static void HashConsole(XXH3_state_t* st, u32 keyinput, u32 lastinput, const u8* state, size_t statelen)
{
    u8 inputs[8];
    for (int b = 0; b < 4; b++)
    {
        inputs[b] = (u8)(keyinput >> (b * 8));
        inputs[4 + b] = (u8)(lastinput >> (b * 8));
    }
    XXH3_64bits_update(st, inputs, 8);

    // the savestate header's magic and version; not its length
    XXH3_64bits_update(st, state, std::min<size_t>(statelen, 8));
    for (size_t off = 0x10; off + 16 <= statelen;)
    {
        u32 seclen = GetU32(state + off + 4);
        if (seclen < 16 || off + seclen > statelen) break;
        if (memcmp(state + off, "GPUR", 4) != 0)
            XXH3_64bits_update(st, state + off, seclen);
        off += seclen;
    }
}

u64 NetplayMachine::HashState(const MachineState& state)
{
    XXH3_state_t* st = XXH3_createState();
    XXH3_64bits_reset(st);

    u8 header[5];
    for (int b = 0; b < 4; b++)
        header[b] = (u8)(state.Frame >> (b * 8));
    header[4] = (u8)state.Consoles.size();
    XXH3_64bits_update(st, header, 5);

    for (const MachineState::Console& c : state.Consoles)
        HashConsole(st, c.KeyInput, c.LastInput, c.Data.data(), c.Length);
    XXH3_64bits_update(st, state.Bus.data(), state.Bus.size());

    u64 hash = XXH3_64bits_digest(st);
    XXH3_freeState(st);
    return hash;
}

u64 NetplayMachine::HashState(const u8* data, size_t len)
{
    MachineState state;
    if (!state.Deserialize(data, len))
        return XXH3_64bits(data, len);
    return HashState(state);
}

}
