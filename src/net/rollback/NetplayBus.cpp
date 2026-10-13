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

#include "NetplayBus.h"
#include "NetplayBytes.h"

namespace melonDS::RollbackNet
{

static constexpr u32 kBusStateMagic = 0x3255424E; // "NBU2"

NetplayBus::NetplayBus(int numConsoles)
    : NumConsoles(std::clamp(numConsoles, 1, kMaxConsoles))
{
    Reset();
}

void NetplayBus::Reset()
{
    Connected = 0;
    Log.clear();
    BaseSeq = 0;
    CmdSeq = 0;
    CmdHost = -1;
    Replies.clear();
    for (int i = 0; i < kMaxConsoles; i++)
    {
        On[i] = false;
        ReadSeq[i] = 0;
        LastHost[i] = -1;
        LastCmdRead[i] = 0;
        RepliesTaken[i] = 0;
        Pending[i].clear();
    }
}

void NetplayBus::Begin(int idx, u64 systime)
{
    if (idx < 0 || idx >= NumConsoles) return;

    // like LocalMP: a console that switches its wireless on only hears what is
    // sent afterwards (Commit moves it on past this round's earlier frames)
    ReadSeq[idx] = BaseSeq + Log.size();
    LastHost[idx] = -1;
    On[idx] = true;

    Entry e;
    e.Kind = Entry_Begin;
    e.Time = systime;
    Pending[idx].push_back(std::move(e));
}

void NetplayBus::End(int idx, u64 systime)
{
    if (idx < 0 || idx >= NumConsoles) return;

    On[idx] = false;

    Entry e;
    e.Kind = Entry_End;
    e.Time = systime;
    Pending[idx].push_back(std::move(e));
}

int NetplayBus::Send(int idx, u64 systime, u8 type, const u8* data, int len, u64 timestamp, u16 aid)
{
    if (idx < 0 || idx >= NumConsoles) return 0;
    if (len < 0) return 0;

    if (type == Packet_Reply)
    {
        if (len > kMaxReplyLength) len = kMaxReplyLength;
    }
    else if (len > kMaxFrameLength)
        return 0;

    Entry e;
    e.Kind = Entry_Packet;
    e.Time = systime;
    Packet& pkt = e.Pkt;
    pkt.Sender = (u8)idx;
    pkt.Type = type;
    pkt.AID = (type == Packet_Reply) ? aid : 0;
    // a reply answers the last CMD frame this console read
    pkt.CmdSeq = (type == Packet_Reply) ? LastCmdRead[idx] : 0;
    pkt.Timestamp = timestamp;
    pkt.SendSys = systime;
    if (len > 0 && data)
        pkt.Data.assign(data, data + len);
    Pending[idx].push_back(std::move(e));
    return len;
}

int NetplayBus::SendPacket(int idx, u64 systime, const u8* data, int len, u64 timestamp)
{
    return Send(idx, systime, Packet_Regular, data, len, timestamp, 0);
}

int NetplayBus::SendCmd(int idx, u64 systime, const u8* data, int len, u64 timestamp)
{
    return Send(idx, systime, Packet_Cmd, data, len, timestamp, 0);
}

int NetplayBus::SendReply(int idx, u64 systime, const u8* data, int len, u64 timestamp, u16 aid)
{
    return Send(idx, systime, Packet_Reply, data, len, timestamp, aid);
}

int NetplayBus::SendAck(int idx, u64 systime, const u8* data, int len, u64 timestamp)
{
    return Send(idx, systime, Packet_Ack, data, len, timestamp, 0);
}

int NetplayBus::Recv(int idx, u64 systime, u8* data, u64* timestamp)
{
    if (idx < 0 || idx >= NumConsoles) return 0;
    if (!On[idx]) return 0;

    if (ReadSeq[idx] < BaseSeq) ReadSeq[idx] = BaseSeq;
    u64 end = BaseSeq + Log.size();
    while (ReadSeq[idx] < end)
    {
        const Packet& pkt = Log[ReadSeq[idx] - BaseSeq];
        ReadSeq[idx]++;

        if (pkt.Sender == idx)
            continue;

        int len = (int)pkt.Data.size();
        if (len > 0)
            memcpy(data, pkt.Data.data(), len);

        if (pkt.Type == Packet_Cmd)
        {
            LastHost[idx] = pkt.Sender;
            LastCmdRead[idx] = pkt.CmdSeq;
        }

        if (timestamp)
        {
            // a frame read after it was sent carries the sender's clock as of now
            u64 ts = pkt.Timestamp;
            if (systime > pkt.SendSys)
                ts += ((systime - pkt.SendSys) * 1000000) / 33513982;
            *timestamp = ts;
        }
        return len;
    }

    return 0;
}

int NetplayBus::RecvPacket(int idx, u64 systime, u8* data, u64* timestamp)
{
    return Recv(idx, systime, data, timestamp);
}

int NetplayBus::RecvHostPacket(int idx, u64 systime, u8* data, u64* timestamp)
{
    if (idx < 0 || idx >= NumConsoles) return 0;

    // like LocalMP: tell the client when its host has switched its wireless off
    int host = LastHost[idx];
    if (host != -1 && !(Connected & (1 << host)))
        return -1;

    return Recv(idx, systime, data, timestamp);
}

u16 NetplayBus::RecvReplies(int idx, u8* data, u64 timestamp, u16 aidmask)
{
    if (idx < 0 || idx >= NumConsoles) return 0;
    if (idx != CmdHost) return 0;

    // a CMD frame sent during this round has no replies yet: those committed are
    // for an older one
    for (const Entry& e : Pending[idx])
    {
        if (e.Kind == Entry_Packet && e.Pkt.Type == Packet_Cmd)
            return 0;
    }

    // the replies to a CMD frame are taken once
    if (RepliesTaken[idx] == CmdSeq) return 0;
    RepliesTaken[idx] = CmdSeq;

    u16 ret = 0;
    for (const Packet& reply : Replies)
    {
        // empty replies only tell LocalMP's host to stop waiting
        if (reply.Data.empty()) continue;
        if (reply.AID < 1 || reply.AID > 15) continue;

        memcpy(&data[(reply.AID - 1) * 1024], reply.Data.data(), reply.Data.size());
        ret |= (1 << reply.AID);
    }
    return ret;
}

void NetplayBus::Commit()
{
    // everything the consoles did during the round, in emulated time order (and
    // by console index for the same time): independent of the order they ran in
    size_t pos[kMaxConsoles] {};
    for (;;)
    {
        int best = -1;
        for (int i = 0; i < NumConsoles; i++)
        {
            if (pos[i] >= Pending[i].size()) continue;
            if (best < 0 || Pending[i][pos[i]].Time < Pending[best][pos[best]].Time)
                best = i;
        }
        if (best < 0) break;
        CommitEntry(best, Pending[best][pos[best]++]);
    }

    for (int i = 0; i < NumConsoles; i++)
    {
        Pending[i].clear();
        On[i] = (Connected & (1 << i)) != 0;
    }
    Prune();
}

void NetplayBus::CommitEntry(int idx, Entry& entry)
{
    switch (entry.Kind)
    {
    case Entry_Begin:
        Connected |= (1 << idx);
        // what was committed before this point was sent before it switched on
        ReadSeq[idx] = BaseSeq + Log.size();
        LastHost[idx] = -1;
        return;

    case Entry_End:
        Connected &= ~(1 << idx);
        if (CmdHost == idx)
            Replies.clear();
        return;

    case Entry_Packet:
        break;
    }

    Packet& pkt = entry.Pkt;
    if (pkt.Type == Packet_Reply)
    {
        // replies to an older CMD frame, or sent while the host is not listening, are lost
        if (CmdHost < 0 || pkt.CmdSeq != CmdSeq || !(Connected & (1 << CmdHost)))
            return;
        Replies.push_back(std::move(pkt));
        return;
    }

    if (pkt.Type == Packet_Cmd)
    {
        CmdSeq++;
        CmdHost = idx;
        Replies.clear();
    }
    pkt.CmdSeq = CmdSeq;
    Log.push_back(std::move(pkt));

    if (Log.size() > kMaxQueuedFrames)
    {
        // a console that never reads its frames would make the log grow forever:
        // drop the oldest frame, as LocalMP's FIFO overflow would
        u64 first = BaseSeq + 1;
        for (int i = 0; i < NumConsoles; i++)
        {
            if (ReadSeq[i] < first)
                ReadSeq[i] = first;
        }
        Log.pop_front();
        BaseSeq = first;
    }
}

void NetplayBus::Prune()
{
    if (!Connected)
    {
        BaseSeq += Log.size();
        Log.clear();
        return;
    }

    u64 minRead = BaseSeq + Log.size();
    for (int i = 0; i < NumConsoles; i++)
    {
        if ((Connected & (1 << i)) && ReadSeq[i] < minRead)
            minRead = ReadSeq[i];
    }

    while (BaseSeq < minRead && !Log.empty())
    {
        Log.pop_front();
        BaseSeq++;
    }
}

static void WritePacket(ByteWriter& w, u8 sender, u8 type, u16 aid, u32 cmdseq, u64 timestamp, u64 sendsys, const std::vector<u8>& data)
{
    w.U8(sender);
    w.U8(type);
    w.U16(aid);
    w.U32(cmdseq);
    w.U64(timestamp);
    w.U64(sendsys);
    w.Blob(data);
}

void NetplayBus::Save(std::vector<u8>& out) const
{
    ByteWriter w(out);
    w.U32(kBusStateMagic);
    w.U8((u8)NumConsoles);
    w.U16(Connected);
    w.U64(BaseSeq);
    for (int i = 0; i < NumConsoles; i++)
    {
        w.U64(ReadSeq[i]);
        w.U32((u32)LastHost[i]);
        w.U32(LastCmdRead[i]);
        w.U32(RepliesTaken[i]);
    }
    w.U32(CmdSeq);
    w.U32((u32)CmdHost);

    w.U32((u32)Log.size());
    for (const Packet& pkt : Log)
        WritePacket(w, pkt.Sender, pkt.Type, pkt.AID, pkt.CmdSeq, pkt.Timestamp, pkt.SendSys, pkt.Data);

    w.U32((u32)Replies.size());
    for (const Packet& pkt : Replies)
        WritePacket(w, pkt.Sender, pkt.Type, pkt.AID, pkt.CmdSeq, pkt.Timestamp, pkt.SendSys, pkt.Data);
}

static bool ReadPacket(ByteReader& r, u8& sender, u8& type, u16& aid, u32& cmdseq, u64& timestamp, u64& sendsys, std::vector<u8>& data)
{
    sender = r.U8();
    type = r.U8();
    aid = r.U16();
    cmdseq = r.U32();
    timestamp = r.U64();
    sendsys = r.U64();
    return r.Blob(data, NetplayBus::kMaxFrameLength) && r.Ok();
}

size_t NetplayBus::Load(const u8* data, size_t len)
{
    ByteReader r(data, len);
    if (r.U32() != kBusStateMagic) return 0;
    if (r.U8() != NumConsoles) return 0;

    u16 connected = r.U16();
    u64 baseseq = r.U64();
    u64 readseq[kMaxConsoles] {};
    int lasthost[kMaxConsoles];
    u32 lastcmd[kMaxConsoles] {};
    u32 taken[kMaxConsoles] {};
    for (int i = 0; i < kMaxConsoles; i++) lasthost[i] = -1;
    for (int i = 0; i < NumConsoles; i++)
    {
        readseq[i] = r.U64();
        lasthost[i] = (int)r.U32();
        lastcmd[i] = r.U32();
        taken[i] = r.U32();
    }
    u32 cmdseq = r.U32();
    int cmdhost = (int)r.U32();

    std::deque<Packet> log;
    u32 nlog = r.U32();
    if (!r.Ok() || nlog > kMaxQueuedFrames + 1) return 0;
    for (u32 i = 0; i < nlog; i++)
    {
        Packet pkt;
        if (!ReadPacket(r, pkt.Sender, pkt.Type, pkt.AID, pkt.CmdSeq, pkt.Timestamp, pkt.SendSys, pkt.Data))
            return 0;
        if (pkt.Sender >= NumConsoles) return 0;
        log.push_back(std::move(pkt));
    }

    std::vector<Packet> replies;
    u32 nreplies = r.U32();
    if (!r.Ok() || nreplies > 256) return 0;
    for (u32 i = 0; i < nreplies; i++)
    {
        Packet pkt;
        if (!ReadPacket(r, pkt.Sender, pkt.Type, pkt.AID, pkt.CmdSeq, pkt.Timestamp, pkt.SendSys, pkt.Data))
            return 0;
        if (pkt.Sender >= NumConsoles) return 0;
        replies.push_back(std::move(pkt));
    }

    if (!r.Ok()) return 0;
    if (cmdhost < -1 || cmdhost >= NumConsoles) return 0;

    Connected = connected;
    BaseSeq = baseseq;
    for (int i = 0; i < kMaxConsoles; i++)
    {
        ReadSeq[i] = readseq[i];
        LastHost[i] = (lasthost[i] >= -1 && lasthost[i] < NumConsoles) ? lasthost[i] : -1;
        LastCmdRead[i] = lastcmd[i];
        RepliesTaken[i] = taken[i];
        On[i] = (Connected & (1 << i)) != 0;
        Pending[i].clear();
    }
    CmdSeq = cmdseq;
    CmdHost = cmdhost;
    Log = std::move(log);
    Replies = std::move(replies);
    return r.Pos();
}

}
