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

// The Platform functions melonDS's core needs, for the headless netplay tool.
// Netplay consoles (all the tool runs) are routed to their machine, as the Qt
// frontend does in its own Platform.cpp.

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

#include "Platform.h"
#include "SPI_Firmware.h"
#include "NetplayMachine.h"

using namespace melonDS;
using melonDS::RollbackNet::ConsoleSlot;

namespace HeadlessPlatform
{
int LogVerbosity = 1; // 0: errors only, 1: + warnings, 2: + info, 3: everything
}

namespace melonDS::Platform
{

void SignalStop(StopReason reason, void* userdata)
{
    if (ConsoleSlot* slot = ConsoleSlot::From(userdata))
        slot->SignalStop();
}

static std::string ModeString(FileMode mode, bool exists)
{
    std::string m;
    if (mode & FileMode::Append) m = "a";
    else if (!(mode & FileMode::Write)) m = "r";
    else if (mode & FileMode::NoCreate) m = "r";
    else if ((mode & FileMode::Preserve) && exists) m = "r";
    else m = "w";
    if ((mode & FileMode::ReadWrite) == FileMode::ReadWrite) m += '+';
    if (!(mode & FileMode::Text)) m += 'b';
    return m;
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    if ((mode & (FileMode::ReadWrite | FileMode::Append)) == FileMode::None)
        return nullptr;
    bool exists = FileExists(path);
    if ((mode & FileMode::NoCreate) && !exists)
        return nullptr;
    FILE* f = fopen(path.c_str(), ModeString(mode, exists).c_str());
    return reinterpret_cast<FileHandle*>(f);
}

std::string GetLocalFilePath(const std::string& filename)
{
    return filename;
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string& name)
{
    FILE* f = fopen(name.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

bool LocalFileExists(const std::string& name) { return FileExists(GetLocalFilePath(name)); }
bool CheckFileWritable(const std::string& filepath) { return false; }
bool CheckLocalFileWritable(const std::string& filepath) { return false; }

bool CloseFile(FileHandle* file) { return fclose(reinterpret_cast<FILE*>(file)) == 0; }
bool IsEndOfFile(FileHandle* file) { return feof(reinterpret_cast<FILE*>(file)) != 0; }
bool FileReadLine(char* str, int count, FileHandle* file) { return fgets(str, count, reinterpret_cast<FILE*>(file)) != nullptr; }
u64 FilePosition(FileHandle* file) { return ftell(reinterpret_cast<FILE*>(file)); }

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    int whence = SEEK_SET;
    if (origin == FileSeekOrigin::Current) whence = SEEK_CUR;
    else if (origin == FileSeekOrigin::End) whence = SEEK_END;
    return fseek(reinterpret_cast<FILE*>(file), offset, whence) == 0;
}

void FileRewind(FileHandle* file) { rewind(reinterpret_cast<FILE*>(file)); }
u64 FileRead(void* data, u64 size, u64 count, FileHandle* file) { return fread(data, size, count, reinterpret_cast<FILE*>(file)); }
bool FileFlush(FileHandle* file) { return fflush(reinterpret_cast<FILE*>(file)) == 0; }
u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file) { return fwrite(data, size, count, reinterpret_cast<FILE*>(file)); }

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vfprintf(reinterpret_cast<FILE*>(file), fmt, args);
    va_end(args);
    return n < 0 ? 0 : n;
}

u64 FileLength(FileHandle* file)
{
    FILE* f = reinterpret_cast<FILE*>(file);
    long pos = ftell(f);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, pos, SEEK_SET);
    return len;
}

void Log(LogLevel level, const char* fmt, ...)
{
    int needed = 3;
    switch (level)
    {
    case LogLevel::Error: needed = 0; break;
    case LogLevel::Warn: needed = 1; break;
    case LogLevel::Info: needed = 2; break;
    default: needed = 3; break;
    }
    if (HeadlessPlatform::LogVerbosity < needed)
        return;

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

struct HThread { std::thread T; };

Thread* Thread_Create(std::function<void()> func)
{
    auto* t = new HThread;
    t->T = std::thread(std::move(func));
    return reinterpret_cast<Thread*>(t);
}

void Thread_Free(Thread* thread)
{
    auto* t = reinterpret_cast<HThread*>(thread);
    if (t->T.joinable()) t->T.detach();
    delete t;
}

void Thread_Wait(Thread* thread)
{
    auto* t = reinterpret_cast<HThread*>(thread);
    if (t->T.joinable()) t->T.join();
}

struct HSemaphore
{
    std::mutex M;
    std::condition_variable CV;
    int Count = 0;
};

Semaphore* Semaphore_Create() { return reinterpret_cast<Semaphore*>(new HSemaphore); }
void Semaphore_Free(Semaphore* sema) { delete reinterpret_cast<HSemaphore*>(sema); }

void Semaphore_Reset(Semaphore* sema)
{
    auto* s = reinterpret_cast<HSemaphore*>(sema);
    std::lock_guard l(s->M);
    s->Count = 0;
}

void Semaphore_Wait(Semaphore* sema)
{
    auto* s = reinterpret_cast<HSemaphore*>(sema);
    std::unique_lock l(s->M);
    s->CV.wait(l, [s] { return s->Count > 0; });
    s->Count--;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    auto* s = reinterpret_cast<HSemaphore*>(sema);
    std::unique_lock l(s->M);
    if (timeout_ms == 0)
    {
        if (s->Count == 0) return false;
        s->Count--;
        return true;
    }
    if (!s->CV.wait_for(l, std::chrono::milliseconds(timeout_ms), [s] { return s->Count > 0; }))
        return false;
    s->Count--;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    auto* s = reinterpret_cast<HSemaphore*>(sema);
    {
        std::lock_guard l(s->M);
        s->Count += count;
    }
    s->CV.notify_all();
}

Mutex* Mutex_Create() { return reinterpret_cast<Mutex*>(new std::mutex); }
void Mutex_Free(Mutex* mutex) { delete reinterpret_cast<std::mutex*>(mutex); }
void Mutex_Lock(Mutex* mutex) { reinterpret_cast<std::mutex*>(mutex)->lock(); }
void Mutex_Unlock(Mutex* mutex) { reinterpret_cast<std::mutex*>(mutex)->unlock(); }
bool Mutex_TryLock(Mutex* mutex) { return reinterpret_cast<std::mutex*>(mutex)->try_lock(); }

void Sleep(u64 usecs) { std::this_thread::sleep_for(std::chrono::microseconds(usecs)); }

static const auto StartTime = std::chrono::steady_clock::now();
u64 GetMSCount()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - StartTime).count();
}
u64 GetUSCount()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - StartTime).count();
}

// netplay consoles never write to disk
void WriteNDSSave(const u8*, u32, u32, u32, void*) {}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

void MP_Begin(void* userdata) { if (auto* s = ConsoleSlot::From(userdata)) s->MPBegin(); }
void MP_End(void* userdata) { if (auto* s = ConsoleSlot::From(userdata)) s->MPEnd(); }

int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata)
{
    if (auto* s = ConsoleSlot::From(userdata)) return s->MPSendPacket(data, len, timestamp);
    return 0;
}

int MP_RecvPacket(u8* data, u64* timestamp, void* userdata)
{
    if (auto* s = ConsoleSlot::From(userdata)) return s->MPRecvPacket(data, timestamp);
    return 0;
}

int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata)
{
    if (auto* s = ConsoleSlot::From(userdata)) return s->MPSendCmd(data, len, timestamp);
    return 0;
}

int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void* userdata)
{
    if (auto* s = ConsoleSlot::From(userdata)) return s->MPSendReply(data, len, timestamp, aid);
    return 0;
}

int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata)
{
    if (auto* s = ConsoleSlot::From(userdata)) return s->MPSendAck(data, len, timestamp);
    return 0;
}

int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata)
{
    if (auto* s = ConsoleSlot::From(userdata)) return s->MPRecvHostPacket(data, timestamp);
    return 0;
}

u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata)
{
    if (auto* s = ConsoleSlot::From(userdata)) return s->MPRecvReplies(data, timestamp, aidmask);
    return 0;
}

int Net_SendPacket(u8* data, int len, void* userdata) { return 0; }
int Net_RecvPacket(u8* data, void* userdata) { return 0; }

void Camera_Start(int num, void* userdata) {}
void Camera_Stop(int num, void* userdata) {}
void Camera_CaptureFrame(int num, u32* frame, int width, int height, bool yuv, void* userdata) {}

void Mic_Start(void* userdata) {}
void Mic_Stop(void* userdata) {}
int Mic_ReadInput(s16* data, int maxlength, void* userdata)
{
    if (auto* s = ConsoleSlot::From(userdata)) return s->MicReadInput(data, maxlength);
    return 0;
}

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder* dec) {}
bool AAC_Configure(AACDecoder* dec, int frequency, int channels) { return false; }
bool AAC_DecodeFrame(AACDecoder* dec, const void* input, int inputlen, void* output, int outputlen) { return false; }

bool Addon_KeyDown(KeyType type, void* userdata) { return false; }
void Addon_RumbleStart(u32 len, void* userdata) {}
void Addon_RumbleStop(void* userdata) {}
float Addon_MotionQuery(MotionQueryType type, void* userdata) { return 0.0f; }

DynamicLibrary* DynamicLibrary_Load(const char* lib) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary* lib) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name) { return nullptr; }

}
