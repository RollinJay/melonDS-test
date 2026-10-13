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

#ifndef NETPLAYBYTES_H
#define NETPLAYBYTES_H

#include <cstring>
#include <string>
#include <vector>

#include "types.h"

namespace melonDS::RollbackNet
{

// Little-endian serialization for netplay states and messages, independent of
// the host's byte order and structure layout.
class ByteWriter
{
public:
    explicit ByteWriter(std::vector<u8>& out) : Out(out) {}

    void U8(u8 v) { Out.push_back(v); }
    void U16(u16 v) { U8(v & 0xFF); U8(v >> 8); }
    void U32(u32 v) { U16(v & 0xFFFF); U16(v >> 16); }
    void U64(u64 v) { U32(v & 0xFFFFFFFF); U32(v >> 32); }
    void Bytes(const void* data, size_t len)
    {
        const u8* p = static_cast<const u8*>(data);
        Out.insert(Out.end(), p, p + len);
    }
    void Blob(const std::vector<u8>& v) { U32((u32)v.size()); Bytes(v.data(), v.size()); }
    void Str(const std::string& s) { U32((u32)s.size()); Bytes(s.data(), s.size()); }

private:
    std::vector<u8>& Out;
};

class ByteReader
{
public:
    ByteReader(const u8* data, size_t len) : Data(data), Len(len) {}

    bool Ok() const { return !Failed; }
    size_t Pos() const { return Offset; }
    size_t Left() const { return Len - Offset; }

    u8 U8()
    {
        if (!Need(1)) return 0;
        return Data[Offset++];
    }
    u16 U16() { u16 lo = U8(); return lo | (u16(U8()) << 8); }
    u32 U32() { u32 lo = U16(); return lo | (u32(U16()) << 16); }
    u64 U64() { u64 lo = U32(); return lo | (u64(U32()) << 32); }
    bool Bytes(void* out, size_t len)
    {
        if (!Need(len)) return false;
        memcpy(out, Data + Offset, len);
        Offset += len;
        return true;
    }
    // A length-prefixed byte string of at most `max` bytes.
    bool Blob(std::vector<u8>& out, size_t max)
    {
        u32 len = U32();
        if (Failed || len > max || !Need(len)) { Failed = true; return false; }
        out.assign(Data + Offset, Data + Offset + len);
        Offset += len;
        return true;
    }
    bool Str(std::string& out, size_t max)
    {
        u32 len = U32();
        if (Failed || len > max || !Need(len)) { Failed = true; return false; }
        out.assign(reinterpret_cast<const char*>(Data + Offset), len);
        Offset += len;
        return true;
    }

private:
    bool Need(size_t n)
    {
        if (Failed || n > Len - Offset)
        {
            Failed = true;
            return false;
        }
        return true;
    }

    const u8* Data;
    size_t Len;
    size_t Offset = 0;
    bool Failed = false;
};

}

#endif // NETPLAYBYTES_H
