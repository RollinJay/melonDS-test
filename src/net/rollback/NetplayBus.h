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

#ifndef NETPLAYBUS_H
#define NETPLAYBUS_H

#include <deque>
#include <vector>

#include "types.h"

namespace melonDS::RollbackNet
{

// The local wireless link between the consoles of a netplay machine.
//
// melonDS's LocalMP passes frames between emulator instances that run in
// separate threads, and waits for them with timeouts, so what a console
// receives depends on how fast each thread happens to run. A netplay machine
// instead advances its consoles in rounds (NetplayMachine::RunFrame): every
// console runs up to the same point of emulated time, then the round ends.
// What a console sends during a round reaches the other consoles when the
// round ends (Commit), in the order of the emulated time it was sent at, and
// nothing here waits: a console that looks for a frame gets the next one it
// has not read, or nothing. What each console receives then depends only on
// the emulated consoles, not on the order in which they ran during the round,
// so they can run in parallel threads, and every computer that runs the same
// consoles with the same inputs gets the same result. The bus is saved and
// restored with the consoles (Save/Load), between rounds.
//
// During a round, a console's calls only read what was committed before the
// round and its own entries, and only write its own entries: consoles may call
// in from different threads at the same time without locks. Commit, Save, Load
// and Reset run between rounds, on one thread.
//
// The calls mirror Platform::MP_*, with the console's index in the machine and
// its system clock (NDS::GetSysTimestamp), which all consoles of a machine share.
//
// A frame that waited in the bus before being read is given to the reader with
// its timestamp moved forward by that wait: the sender's clock as it reads now.
// A client sets its own clock from the association response's timestamp, so
// this keeps the clients' clocks in step with the host's, as on hardware.
// Without it a client's clock lags behind by however long the response waited,
// and with it every reply it sends; LocalMP's host waits for the replies, but
// here the host does not wait.
class NetplayBus
{
public:
    static constexpr int kMaxConsoles = 16;
    // larger than any 802.11 frame the DS sends; Wifi's receive buffer is 2 KB
    static constexpr int kMaxFrameLength = 2048;
    // a reply is stored in a 1 KB slot of Wifi::MPClientReplies
    static constexpr int kMaxReplyLength = 1024;
    // frames kept for a console that does not read them (like LocalMP's FIFO overflow)
    static constexpr size_t kMaxQueuedFrames = 1024;

    explicit NetplayBus(int numConsoles);

    void Reset();

    void Begin(int idx, u64 systime);
    void End(int idx, u64 systime);

    int SendPacket(int idx, u64 systime, const u8* data, int len, u64 timestamp);
    int SendCmd(int idx, u64 systime, const u8* data, int len, u64 timestamp);
    int SendReply(int idx, u64 systime, const u8* data, int len, u64 timestamp, u16 aid);
    int SendAck(int idx, u64 systime, const u8* data, int len, u64 timestamp);

    int RecvPacket(int idx, u64 systime, u8* data, u64* timestamp);
    int RecvHostPacket(int idx, u64 systime, u8* data, u64* timestamp);
    u16 RecvReplies(int idx, u8* data, u64 timestamp, u16 aidmask);

    // Ends a round: what the consoles sent during it becomes readable.
    void Commit();

    // Between rounds only (nothing waits to be committed then).
    void Save(std::vector<u8>& out) const;
    // Returns the number of bytes read, or 0 if the data is not a valid bus state.
    size_t Load(const u8* data, size_t len);

private:
    enum PacketType : u8
    {
        Packet_Regular = 0,
        Packet_Cmd = 1,
        Packet_Reply = 2,
        Packet_Ack = 3,
    };

    struct Packet
    {
        u8 Sender;
        u8 Type;
        u16 AID;
        u32 CmdSeq;         // CMD frames: their number; replies: the CMD frame they answer
        u64 Timestamp;      // sender's wireless clock (microseconds)
        u64 SendSys;        // system clock (cycles) when it was sent
        std::vector<u8> Data;
    };

    // what a console did during the current round, in the order it did it
    enum EntryKind : u8
    {
        Entry_Packet,
        Entry_Begin,
        Entry_End,
    };

    struct Entry
    {
        EntryKind Kind;
        u64 Time;
        Packet Pkt;
    };

    int Send(int idx, u64 systime, u8 type, const u8* data, int len, u64 timestamp, u16 aid);
    int Recv(int idx, u64 systime, u8* data, u64* timestamp);
    void CommitEntry(int idx, Entry& entry);
    void Prune();

    int NumConsoles;

    // committed: read by every console during a round, written only by Commit
    u16 Connected;
    // frames other than replies, in the order they were committed;
    // Log[0] has the sequence number BaseSeq
    std::deque<Packet> Log;
    u64 BaseSeq;
    u32 CmdSeq;                     // number of the last CMD frame
    int CmdHost;                    // its sender
    std::vector<Packet> Replies;    // replies to that CMD frame

    // per console: written only by that console during a round
    bool On[kMaxConsoles];          // its own view of Connected
    u64 ReadSeq[kMaxConsoles];      // sequence number of the next frame it reads
    int LastHost[kMaxConsoles];     // sender of the last CMD frame it read
    u32 LastCmdRead[kMaxConsoles];  // number of that CMD frame
    u32 RepliesTaken[kMaxConsoles]; // CMD frame whose replies it took (as host)
    std::vector<Entry> Pending[kMaxConsoles];
};

}

#endif // NETPLAYBUS_H
