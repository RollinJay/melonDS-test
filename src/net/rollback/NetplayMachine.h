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

#ifndef NETPLAYMACHINE_H
#define NETPLAYMACHINE_H

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "types.h"
#include "Args.h"
#include "SPI_Firmware.h"
#include "NetplayBus.h"
#include "NetplayInput.h"

namespace melonDS
{
class NDS;
}

namespace melonDS::RollbackNet
{

// What every computer of a session must agree on to build the same consoles.
struct MachineConfig
{
    // shared by all consoles
    std::vector<u8> ROM;
    std::string ROMName;                   // used by direct boot
    ARM9BIOSImage ARM9BIOS {};
    ARM7BIOSImage ARM7BIOS {};
    bool DirectBoot = true;
    AudioInterpolation Interpolation = AudioInterpolation::None;
    AudioBitDepth BitDepth = AudioBitDepth::Auto;
    // the date and time every console's clock starts at
    int Year = 2026, Month = 1, Day = 1, Hour = 12, Minute = 0, Second = 0;

    // one entry per console (player)
    struct Console
    {
        Firmware FirmwareImage {0};
        std::vector<u8> Save;              // cartridge save memory; empty: a blank save
    };
    std::vector<Console> Consoles;
};

class NetplayMachine;

// Allocates without zero-filling: a machine state is several megabytes and
// is saved every frame, into buffers that are reused.
template <typename T>
struct UninitializedAllocator : std::allocator<T>
{
    template <typename U> struct rebind { using other = UninitializedAllocator<U>; };
    UninitializedAllocator() noexcept = default;
    template <typename U> UninitializedAllocator(const UninitializedAllocator<U>&) noexcept {}
    template <typename U> void construct(U* p) noexcept { ::new (static_cast<void*>(p)) U; }
    template <typename U, typename... Args> void construct(U* p, Args&&... args)
    {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }
};

// A saved machine. Each console's savestate has its own buffer, so that the
// consoles can be saved and loaded in parallel; the buffers keep their size
// between saves, so saving into the same MachineState again allocates nothing.
struct MachineState
{
    struct Console
    {
        u32 KeyInput = 0;
        u32 LastInput = 0;          // the PlayerInput it was last given
        std::vector<u8, UninitializedAllocator<u8>> Data;   // its savestate: the first Length bytes
        size_t Length = 0;
    };

    u32 Frame = 0;
    std::vector<Console> Consoles;
    std::vector<u8> Bus;

    // a single buffer, for files and tools (the layout NetplayMachine::LoadState reads)
    void Serialize(std::vector<u8>& out) const;
    bool Deserialize(const u8* data, size_t len);
};

// The userdata of a netplay console: the Platform callbacks of the frontend
// recognize it with ConsoleSlot::From() and hand the call to the machine.
class ConsoleSlot
{
public:
    // The slot whose userdata this is, or nullptr if `userdata` is not a netplay console.
    static ConsoleSlot* From(void* userdata);

    NetplayMachine& Machine() { return *Owner; }
    int Index() const { return Idx; }

    void MPBegin();
    void MPEnd();
    int MPSendPacket(u8* data, int len, u64 timestamp);
    int MPRecvPacket(u8* data, u64* timestamp);
    int MPSendCmd(u8* data, int len, u64 timestamp);
    int MPSendReply(u8* data, int len, u64 timestamp, u16 aid);
    int MPSendAck(u8* data, int len, u64 timestamp);
    int MPRecvHostPacket(u8* data, u64* timestamp);
    u16 MPRecvReplies(u8* data, u64 timestamp, u16 aidmask);

    // the microphone hears silence: anything else could not be reproduced exactly
    int MicReadInput(s16* data, int maxlength);

    // a console asked to stop (powered off, crashed)
    void SignalStop();

private:
    friend class NetplayMachine;
    u64 SysTime() const;
    NetplayMachine* Owner = nullptr;
    int Idx = 0;
};

// The emulated side of a netplay session: one DS per player, linked by a
// deterministic wireless bus, advanced together one frame at a time.
//
// Every computer in the session runs the whole machine: each player's
// console, not just its own. Only the players' inputs travel over the
// network. The machine is deterministic (the same inputs give the same
// states on every computer) and can be saved and restored as a whole, which
// is what rollback needs.
class NetplayMachine
{
public:
    // Builds and boots the consoles. `audioSampleRate` only matters for the
    // console whose audio the frontend plays.
    explicit NetplayMachine(MachineConfig&& config, double audioSampleRate = 48000.0);
    ~NetplayMachine();
    NetplayMachine(const NetplayMachine&) = delete;
    NetplayMachine& operator=(const NetplayMachine&) = delete;

    // false if a console could not be built (bad ROM, ...)
    bool IsValid() const { return Valid; }
    const std::string& Error() const { return ErrorText; }

    int NumConsoles() const { return (int)Consoles.size(); }
    NDS* Console(int idx) { return Consoles[idx].get(); }
    u32 FrameCount() const { return Frame; }

    // The console this computer's player sees and hears (-1: none). The others
    // run headless: their picture is drawn only where display capture needs it,
    // and their audio is dropped.
    void SetLocalConsole(int idx);
    int LocalConsole() const { return Local; }

    // false: draw every console's picture every frame (for measurements)
    void SetHeadlessEnabled(bool enable) { HeadlessEnabled = enable; }

    // Whether the shown console's 3D image is rendered on a thread of its own, off
    // the frame's critical path (SoftRenderer3D::SetAsyncRender): 1 yes, 0 no,
    // -1 (the default) when a processor core is left over by ThreadCount().
    void SetAsyncRender(int mode) { AsyncRender = mode; }
    bool UsesAsyncRender() const;

    // Advances every console by one frame with the given inputs (one per console).
    // `resim`: the frame is being simulated again after a rollback, so it is not
    // shown and its audio was already played: every console runs headless.
    // `beforeShown` (with `resim`): the next frame is shown (this is the last frame
    // of a rollback). The shown console then renders the 3D image the next frame
    // displays, as a shown frame does (on a thread of its own with
    // UsesAsyncRender()), instead of leaving it to the next frame, which would
    // then render two.
    void RunFrame(const PlayerInput* inputs, bool resim = false, bool beforeShown = false);

    // The whole machine state: every console, the bus and the frame counter.
    bool SaveState(MachineState& out);
    bool LoadState(const MachineState& state);
    // the same as a single buffer (MachineState::Serialize), for files and tools
    bool SaveState(std::vector<u8>& out);
    bool LoadState(const u8* data, size_t len);

    // A hash of a saved state, for comparing machines across computers. It leaves
    // out what the renderers keep from one frame to the next (the GPUR savestate
    // sections), which differs between a console that is shown and one that runs
    // headless without making their emulation differ. Both forms of the same
    // state give the same hash.
    static u64 HashState(const MachineState& state);
    static u64 HashState(const u8* data, size_t len);

    // How many threads run the consoles (and save and load them): 1 runs
    // everything on the calling thread, 0 picks the default: one per console, up
    // to the processor's cores. The results never depend on it (see NetplayBus).
    void SetThreadCount(int threads);
    int ThreadCount() const;

    // Length of one round while a console has its wireless switched on, in system
    // clock cycles (33.513982 MHz): 32 microseconds. What a console sends reaches
    // the others at the end of the round (NetplayBus), so a CMD frame reaches a
    // client up to one round late, and its reply reaches the host up to one more
    // round late. Both together must stay under the 112 microseconds between the
    // end of a CMD frame and the first reply the host reads (see
    // Wifi::CollectMPReplies), with room for the client's 8-microsecond timer step.
    static constexpr u64 kSliceCycles = 1072;

    // tools: called after each round of RunFrame, with its number (the machine
    // can be saved between rounds, for comparing two runs of a frame)
    std::function<void(int)> RoundHook;
    // tools: run in rounds even while every console has its wireless off
    bool AlwaysSlice = false;

    // set when a console stopped by itself (power off, crash); the session should end
    bool StopRequested() const { return Stopped.load(); }

private:
    friend class ConsoleSlot;

    bool Valid = false;
    std::string ErrorText;
    std::vector<std::unique_ptr<NDS>> Consoles;
    std::vector<std::unique_ptr<ConsoleSlot>> Slots;
    std::vector<size_t> ConsoleSaveRoom;    // buffer size a console's savestate needs
    std::vector<u32> LastInput;
    NetplayBus MPBus;
    int Local = -1;
    bool HeadlessEnabled = true;
    int AsyncRender = -1;
    u32 Frame = 0;
    std::atomic<bool> Stopped {false};

    bool SaveConsole(int idx, MachineState::Console& out);
    bool LoadConsole(int idx, const MachineState::Console& in);

    class Pool;
    std::unique_ptr<Pool> Threads;
};

}

#endif // NETPLAYMACHINE_H
