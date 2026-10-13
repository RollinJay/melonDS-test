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

// melonDS-netplay-tool: runs netplay machines without a window, to check that a
// game stays deterministic under rollback (synctest) and to measure what
// rollback costs on this computer (bench). See docs/NETPLAY.md.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "NDS.h"
#include "Platform.h"
#include "FreeBIOS.h"
#include "SPI_Firmware.h"
#include "NetplayMachine.h"
#include "NetplayInput.h"
#include "ToolCommon.h"

using namespace melonDS;
using namespace melonDS::RollbackNet;

namespace HeadlessPlatform { extern int LogVerbosity; }

namespace NetplayTool
{

static const char* kButtonNames[12] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};

bool ReadFile(const std::string& path, std::vector<u8>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return false; }
    out.resize(len);
    bool ok = len == 0 || fread(out.data(), len, 1, f) == 1;
    fclose(f);
    return ok;
}

bool InputScript::Load(const std::string& path, int numconsoles, std::string& err)
{
    std::ifstream in(path);
    if (!in)
    {
        err = "cannot open " + path;
        return false;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(in, line))
    {
        lineno++;
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        u32 frame;
        int console;
        std::string buttons;
        if (!(ls >> frame >> console >> buttons))
            continue;
        if (console < 0 || console >= numconsoles)
        {
            err = "line " + std::to_string(lineno) + ": no console " + std::to_string(console);
            return false;
        }
        PlayerInput in;
        if (buttons != "-")
        {
            std::istringstream bs(buttons);
            std::string name;
            while (std::getline(bs, name, ','))
            {
                for (auto& c : name) c = toupper(c);
                int bit = -1;
                for (int i = 0; i < 12; i++)
                    if (name == kButtonNames[i]) bit = i;
                if (bit < 0)
                {
                    err = "line " + std::to_string(lineno) + ": unknown button " + name;
                    return false;
                }
                in.Bits |= 1u << bit;
            }
        }
        std::string touch;
        int x, y;
        if (ls >> touch)
        {
            if (touch != "touch" || !(ls >> x >> y))
            {
                err = "line " + std::to_string(lineno) + ": expected 'touch X Y'";
                return false;
            }
            in.Bits |= PlayerInput::kTouching | (u32(x & 0xFF) << 16) | (u32(std::min(y, 191) & 0xFF) << 24);
        }
        Changes[frame][console] = in;
    }
    return true;
}

void InputScript::Get(u32 frame, std::vector<PlayerInput>& held)
{
    auto it = Changes.find(frame);
    if (it == Changes.end()) return;
    for (auto& [console, in] : it->second)
        held[console] = in;
}

void RandomInputs::Get(std::vector<PlayerInput>& held)
{
    std::uniform_int_distribution<int> pct(0, 99);
    for (auto& in : held)
    {
        if (pct(Rng) < 20)
            in.Bits = (Rng() & 0xFFF) & ~(0xC); // no SELECT/START: they tend to reset or pause games
        if (pct(Rng) < 15)
        {
            in.Bits &= PlayerInput::kButtonMask;
            if (pct(Rng) < 50)
                in.Bits |= PlayerInput::kTouching | (u32(Rng() & 0xFF) << 16) | (u32(Rng() % 192) << 24);
        }
    }
}

Firmware MakeFirmware(int index, const std::string& nickname)
{
    Firmware fw(0);
    auto& user = fw.GetEffectiveUserData();
    std::u16string name;
    for (char c : nickname.substr(0, 10)) name.push_back((char16_t)c);
    memset(user.Nickname, 0, sizeof(user.Nickname));
    memcpy(user.Nickname, name.data(), name.size() * 2);
    user.NameLength = (u16)name.size();
    user.FavoriteColor = index & 0xF;
    user.BirthdayMonth = 1;
    user.BirthdayDay = 1 + index;

    auto& header = fw.GetHeader();
    header.MacAddr = {0x00, 0x09, 0xBF, 0x4E, 0x50, (u8)(0x10 + index)};
    header.UpdateChecksum();
    fw.UpdateChecksums();
    return fw;
}

bool BuildConfig(const Options& opt, MachineConfig& cfg, std::string& err)
{
    if (!ReadFile(opt.ROMPath, cfg.ROM))
    {
        err = "cannot read ROM " + opt.ROMPath;
        return false;
    }
    auto slash = opt.ROMPath.find_last_of("/\\");
    cfg.ROMName = slash == std::string::npos ? opt.ROMPath : opt.ROMPath.substr(slash + 1);

    if (!opt.BIOS9Path.empty() || !opt.BIOS7Path.empty())
    {
        std::vector<u8> b9, b7;
        if (!ReadFile(opt.BIOS9Path, b9) || b9.size() != cfg.ARM9BIOS.size() ||
            !ReadFile(opt.BIOS7Path, b7) || b7.size() != cfg.ARM7BIOS.size())
        {
            err = "BIOS images must be 4 KB (ARM9) and 16 KB (ARM7)";
            return false;
        }
        memcpy(cfg.ARM9BIOS.data(), b9.data(), b9.size());
        memcpy(cfg.ARM7BIOS.data(), b7.data(), b7.size());
    }
    else
    {
        cfg.ARM9BIOS = FreeBIOSGetNtrArm9();
        cfg.ARM7BIOS = FreeBIOSGetNtrArm7();
    }

    for (int i = 0; i < opt.Consoles; i++)
    {
        MachineConfig::Console c;
        c.FirmwareImage = MakeFirmware(i, "Player" + std::to_string(i + 1));
        if (i < (int)opt.SavePaths.size() && !opt.SavePaths[i].empty())
        {
            if (!ReadFile(opt.SavePaths[i], c.Save))
            {
                err = "cannot read save " + opt.SavePaths[i];
                return false;
            }
        }
        cfg.Consoles.push_back(std::move(c));
    }
    return true;
}

static double NowMS()
{
    using namespace std::chrono;
    return duration_cast<duration<double, std::milli>>(steady_clock::now().time_since_epoch()).count();
}

// ---- savestate section diff, to say where two states differ ----

struct Section
{
    std::string Name;
    size_t Offset;
    size_t Length;
};

static std::vector<Section> SplitSavestate(const u8* data, size_t len)
{
    std::vector<Section> out;
    for (size_t off = 0x10; off + 16 <= len;)
    {
        u32 seclen = data[off+4] | (data[off+5] << 8) | (data[off+6] << 16) | ((u32)data[off+7] << 24);
        if (seclen < 16 || off + seclen > len) break;
        out.push_back({std::string((const char*)data + off, 4), off, seclen});
        off += seclen;
    }
    return out;
}

void DiffStates(const std::vector<u8>& a, const std::vector<u8>& b, int numconsoles)
{
    // walks both machine states console by console, comparing savestate sections by name
    auto u32at = [](const std::vector<u8>& v, size_t p) -> u32 {
        return v[p] | (v[p+1] << 8) | (v[p+2] << 16) | ((u32)v[p+3] << 24);
    };
    size_t pa = 13, pb = 13;
    for (int c = 0; c < numconsoles; c++)
    {
        if (pa + 12 > a.size() || pb + 12 > b.size()) return;
        if (u32at(a, pa) != u32at(b, pb))
            printf("    console %d: key input %08X vs %08X\n", c, u32at(a, pa), u32at(b, pb));
        if (u32at(a, pa + 4) != u32at(b, pb + 4))
            printf("    console %d: last input %08X vs %08X\n", c, u32at(a, pa + 4), u32at(b, pb + 4));
        u32 la = u32at(a, pa + 8), lb = u32at(b, pb + 8);
        pa += 12; pb += 12;
        if (pa + la > a.size() || pb + lb > b.size()) return;
        auto sa = SplitSavestate(a.data() + pa, la);
        auto sb = SplitSavestate(b.data() + pb, lb);
        for (size_t k = 0; k < sa.size() && k < sb.size(); k++)
        {
            const Section& x = sa[k];
            const Section& y = sb[k];
            const u8* da = a.data() + pa + x.Offset + 16;
            const u8* db = b.data() + pb + y.Offset + 16;
            size_t lenx = x.Length - 16, leny = y.Length - 16;
            if (x.Name != y.Name)
            {
                printf("    console %d: section %zu is %s vs %s\n", c, k, x.Name.c_str(), y.Name.c_str());
                break;
            }
            if (lenx == leny && memcmp(da, db, lenx) == 0) continue;
            size_t common = std::min(lenx, leny);
            size_t first = 0;
            while (first < common && da[first] == db[first]) first++;
            size_t count = 0;
            for (size_t m = 0; m < common; m++) if (da[m] != db[m]) count++;
            if (first == common)
                printf("    console %d: section %s differs in length: %zu vs %zu%s\n", c, x.Name.c_str(), lenx, leny,
                       x.Name == "GPUR" ? " (renderer leftovers, not compared across computers)" : "");
            else
                printf("    console %d: section %s differs: %zu bytes, first at +0x%zX (%02X vs %02X)%s\n",
                       c, x.Name.c_str(), count, first, da[first], db[first],
                       x.Name == "GPUR" ? " (renderer leftovers, not compared across computers)" : "");
        }
        pa += la; pb += lb;
    }
    if (pa < a.size() && pb < b.size() &&
        (a.size() - pa != b.size() - pb || memcmp(a.data() + pa, b.data() + pb, a.size() - pa) != 0))
        printf("    the wireless bus state differs\n");
}

// ---- commands ----

void NextInputs(const Options& opt, u32 frame, InputScript& script, RandomInputs* rnd, std::vector<PlayerInput>& held)
{
    if (rnd && (s64)frame >= opt.RandomFrom && (opt.RandomUntil < 0 || (s64)frame <= opt.RandomUntil))
    {
        rnd->Get(held);
        if (opt.RandomSame)
            for (auto& in : held) in = held[0];
    }
    script.Get(frame, held);
}

// --start-state: continue from a state written by --save-state. Frame numbers
// (scripts, --frames, --check-from, ...) go on from the state's frame, and the
// inputs the script holds at that frame are set up again; random inputs start
// over.
static bool StartFrom(const Options& opt, NetplayMachine& machine, InputScript& script,
                      std::vector<PlayerInput>& held, u32& first)
{
    first = 0;
    if (opt.StartStatePath.empty())
        return true;
    std::vector<u8> data;
    if (!ReadFile(opt.StartStatePath, data) || !machine.LoadState(data.data(), data.size()))
    {
        fprintf(stderr, "cannot load the state %s (written by this build, with the same ROM and --consoles?)\n",
                opt.StartStatePath.c_str());
        return false;
    }
    first = machine.FrameCount();
    for (u32 f = 0; f < first; f++)
        script.Get(f, held);
    return true;
}

static void WriteStateFile(NetplayMachine& machine, const std::string& path)
{
    std::vector<u8> data;
    machine.SaveState(data);
    FILE* f = fopen(path.c_str(), "wb");
    if (f)
    {
        fwrite(data.data(), data.size(), 1, f);
        fclose(f);
    }
    else
        fprintf(stderr, "cannot write %s\n", path.c_str());
}

// A PNG without compression (stored deflate blocks), so that no zlib is needed.
static u32 PngCrc(const u8* data, size_t len, u32 crc)
{
    static u32 table[256];
    static bool init = false;
    if (!init)
    {
        for (u32 n = 0; n < 256; n++)
        {
            u32 c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        init = true;
    }
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static bool WritePNG(const std::string& path, const std::vector<u8>& rgb, u32 width, u32 height)
{
    std::vector<u8> raw;
    raw.reserve((width * 3 + 1) * height);
    for (u32 y = 0; y < height; y++)
    {
        raw.push_back(0);   // no filter
        raw.insert(raw.end(), rgb.begin() + (size_t)y * width * 3, rgb.begin() + (size_t)(y + 1) * width * 3);
    }

    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (u8 v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < raw.size() || pos == 0;)
    {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(n & 0xFF); z.push_back(n >> 8);
        z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    u32 adler = (b << 16) | a;
    for (int i = 3; i >= 0; i--) z.push_back((adler >> (i * 8)) & 0xFF);

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    auto be32 = [](u8* p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; };
    auto chunk = [&](const char* type, const std::vector<u8>& data)
    {
        u8 len[4]; be32(len, (u32)data.size());
        fwrite(len, 4, 1, f);
        std::vector<u8> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        fwrite(td.data(), td.size(), 1, f);
        u8 crc[4]; be32(crc, PngCrc(td.data(), td.size(), 0));
        fwrite(crc, 4, 1, f);
    };
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 8, 1, f);
    std::vector<u8> ihdr(13);
    be32(&ihdr[0], width); be32(&ihdr[4], height);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
    return true;
}

// both screens of every console, side by side (console 0 on the left)
void WriteShot(const Options& opt, NetplayMachine& machine, u32 frame)
{
    const int n = machine.NumConsoles();
    const u32 width = 256 * n + 4 * (n - 1), height = 384;
    std::vector<u8> rgb((size_t)width * height * 3, 0x40);
    for (int c = 0; c < n; c++)
    {
        void* top = nullptr;
        void* bottom = nullptr;
        if (!machine.Console(c)->GPU.GetFramebuffers(&top, &bottom) || !top || !bottom)
            continue;
        for (u32 y = 0; y < height; y++)
        {
            const u32* src = (y < 192) ? (const u32*)top + y * 256 : (const u32*)bottom + (y - 192) * 256;
            u8* dst = &rgb[((size_t)y * width + c * 260) * 3];
            for (u32 x = 0; x < 256; x++)
            {
                u32 px = src[x];
                dst[x * 3 + 0] = (px >> 16) & 0xFF;
                dst[x * 3 + 1] = (px >> 8) & 0xFF;
                dst[x * 3 + 2] = px & 0xFF;
            }
        }
    }
    char name[64];
    snprintf(name, sizeof(name), "/shot-%06u.png", frame);
    std::string path = opt.ShotDir + name;
    if (!WritePNG(path, rgb, width, height))
        fprintf(stderr, "cannot write %s\n", path.c_str());
}

// (debugging) the first round of frame --check-from whose state differs between
// running on from the start state and running it again after loading the state
// before that frame: what a savestate leaves out shows up there
int Locate(const Options& opt)
{
    const u32 target = (u32)opt.CheckFrom;
    auto build = [&](NetplayMachine*& out, std::unique_ptr<NetplayMachine>& holder, InputScript& script,
                     std::unique_ptr<RandomInputs>& rnd, std::vector<PlayerInput>& held, u32& first) -> bool {
        MachineConfig cfg;
        std::string err;
        if (!BuildConfig(opt, cfg, err)) { fprintf(stderr, "%s\n", err.c_str()); return false; }
        if (!opt.ScriptPath.empty() && !script.Load(opt.ScriptPath, opt.Consoles, err)) { fprintf(stderr, "%s\n", err.c_str()); return false; }
        if (opt.RandomSeed >= 0) rnd = std::make_unique<RandomInputs>((u32)opt.RandomSeed);
        holder = std::make_unique<NetplayMachine>(std::move(cfg));
        if (!holder->IsValid()) { fprintf(stderr, "%s\n", holder->Error().c_str()); return false; }
        holder->SetLocalConsole(opt.Local);
        holder->SetThreadCount(opt.Threads);
        holder->SetAsyncRender(opt.AsyncRender);
        holder->AlwaysSlice = true;
        held.assign(opt.Consoles, PlayerInput{});
        if (!StartFrom(opt, *holder, script, held, first)) return false;
        out = holder.get();
        return true;
    };

    NetplayMachine* m = nullptr;
    std::unique_ptr<NetplayMachine> holder;
    InputScript script;
    std::unique_ptr<RandomInputs> rnd;
    std::vector<PlayerInput> held;
    u32 first;
    if (!build(m, holder, script, rnd, held, first)) return 2;
    if (target < first) { fprintf(stderr, "--check-from must be at or after the start state's frame\n"); return 2; }

    for (u32 f = first; f < target; f++)
    {
        NextInputs(opt, f, script, rnd.get(), held);
        m->RunFrame(held.data(), true);
    }
    NextInputs(opt, target, script, rnd.get(), held);
    std::vector<PlayerInput> in = held;
    MachineState before;
    m->SaveState(before);

    // run on: the hash after every round
    std::vector<u64> hashes;
    std::vector<u8> tmp;
    m->RoundHook = [&](int) { m->SaveState(tmp); hashes.push_back(NetplayMachine::HashState(tmp.data(), tmp.size())); };
    m->RunFrame(in.data(), true);
    m->RoundHook = nullptr;
    std::vector<u8> endA;
    m->SaveState(endA);

    // again, from the state before the frame
    m->LoadState(before);
    int bad = -1;
    std::vector<u8> stateB;
    m->RoundHook = [&](int r) {
        if (bad >= 0) return;
        m->SaveState(tmp);
        u64 h = NetplayMachine::HashState(tmp.data(), tmp.size());
        if (r >= (int)hashes.size() || h != hashes[r]) { bad = r; stateB = tmp; }
    };
    m->RunFrame(in.data(), true);
    m->RoundHook = nullptr;
    std::vector<u8> endB;
    m->SaveState(endB);

    printf("locate: frame %u, %zu rounds; end states %s\n", target, hashes.size(),
           NetplayMachine::HashState(endA.data(), endA.size()) == NetplayMachine::HashState(endB.data(), endB.size()) ? "match" : "differ");
    if (bad < 0) { printf("  every round matched\n"); return 0; }

    // the first run's state at that round: run it once more the same way
    NetplayMachine* m2 = nullptr;
    std::unique_ptr<NetplayMachine> holder2;
    InputScript script2;
    std::unique_ptr<RandomInputs> rnd2;
    std::vector<PlayerInput> held2;
    u32 first2;
    if (!build(m2, holder2, script2, rnd2, held2, first2)) return 2;
    for (u32 f = first2; f < target; f++)
    {
        NextInputs(opt, f, script2, rnd2.get(), held2);
        m2->RunFrame(held2.data(), true);
    }
    std::vector<u8> stateA;
    m2->RoundHook = [&](int r) { m2->SaveState(tmp); if (r == bad) stateA = tmp; };
    m2->RunFrame(in.data(), true);
    printf("  first difference after round %d:\n", bad);
    DiffStates(stateA, stateB, opt.Consoles);
    if (!opt.SaveStatePath.empty())
    {
        auto write = [](const std::string& path, const std::vector<u8>& d) {
            FILE* f = fopen(path.c_str(), "wb"); if (f) { fwrite(d.data(), d.size(), 1, f); fclose(f); } };
        write(opt.SaveStatePath + ".a", stateA);
        write(opt.SaveStatePath + ".b", stateB);
        printf("  states written to %s.a (ran on) and %s.b (loaded)\n", opt.SaveStatePath.c_str(), opt.SaveStatePath.c_str());
    }
    return 1;
}

// Runs the game as a session would, without rolling back, drawing every
// console: for writing input scripts (pictures of the screens) and states to
// start the other commands from.
int Play(const Options& opt)
{
    MachineConfig cfg;
    std::string err;
    if (!BuildConfig(opt, cfg, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    InputScript script;
    if (!opt.ScriptPath.empty() && !script.Load(opt.ScriptPath, opt.Consoles, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    std::unique_ptr<RandomInputs> rnd;
    if (opt.RandomSeed >= 0) rnd = std::make_unique<RandomInputs>((u32)opt.RandomSeed);

    NetplayMachine machine(std::move(cfg));
    if (!machine.IsValid())
    {
        fprintf(stderr, "%s\n", machine.Error().c_str());
        return 2;
    }
    machine.SetLocalConsole(opt.Local);
    machine.SetThreadCount(opt.Threads);
    machine.SetAsyncRender(opt.AsyncRender);
    machine.SetHeadlessEnabled(false);

    std::vector<PlayerInput> held(opt.Consoles);
    u32 first;
    if (!StartFrom(opt, machine, script, held, first))
        return 2;

    std::set<int> shots(opt.ShotFrames.begin(), opt.ShotFrames.end());
    double tstart = NowMS();
    for (u32 f = first; f < first + (u32)opt.Frames; f++)
    {
        NextInputs(opt, f, script, rnd.get(), held);
        machine.RunFrame(held.data());
        if (shots.count((int)f) || (opt.ShotEvery > 0 && (f % opt.ShotEvery) == 0))
            WriteShot(opt, machine, f);
        if ((int)f == opt.SaveStateFrame)
            WriteStateFile(machine, opt.SaveStatePath);
    }
    double elapsed = NowMS() - tstart;

    std::vector<u8> state;
    machine.SaveState(state);
    printf("play: frames %u-%u, %d console(s), %.2f ms per frame; state hash %016llX\n",
           first, first + (u32)opt.Frames - 1, opt.Consoles, elapsed / std::max(1, opt.Frames),
           (unsigned long long)NetplayMachine::HashState(state.data(), state.size()));
    return 0;
}

int SyncTest(const Options& opt)
{
    MachineConfig cfg;
    std::string err;
    if (!BuildConfig(opt, cfg, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }

    InputScript script;
    if (!opt.ScriptPath.empty() && !script.Load(opt.ScriptPath, opt.Consoles, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    std::unique_ptr<RandomInputs> rnd;
    if (opt.RandomSeed >= 0) rnd = std::make_unique<RandomInputs>((u32)opt.RandomSeed);

    NetplayMachine machine(std::move(cfg));
    if (!machine.IsValid())
    {
        fprintf(stderr, "%s\n", machine.Error().c_str());
        return 2;
    }
    machine.SetLocalConsole(opt.Local);
    machine.SetThreadCount(opt.Threads);
    machine.SetAsyncRender(opt.AsyncRender);

    const int n = opt.Consoles;
    const u32 dist = std::max(1, opt.Rollback);
    std::vector<PlayerInput> held(n);
    u32 first;
    if (!StartFrom(opt, machine, script, held, first))
        return 2;

    // states[f % ring] = state before frame f; inputs likewise
    const u32 ring = dist + 2;
    std::vector<MachineState> states(ring);
    std::vector<std::vector<PlayerInput>> inputs(ring, std::vector<PlayerInput>(n));
    MachineState now, again;

    int failures = 0;
    double tstart = NowMS();

    FILE* trace = opt.TracePath.empty() ? nullptr : fopen(opt.TracePath.c_str(), "w");
    MachineState traced;

    for (u32 f = first; f < first + (u32)opt.Frames; f++)
    {
        NextInputs(opt, f, script, rnd.get(), held);
        inputs[f % ring] = held;

        machine.SaveState(states[f % ring]);
        machine.RunFrame(held.data());

        // the shown console's picture, as shown after the rollback before it
        for (int s : opt.ShotFrames)
            if (s == (int)f)
                WriteShot(opt, machine, f);

        if (trace || (int)f == opt.SaveStateFrame)
        {
            machine.SaveState(traced);
            if (trace)
                fprintf(trace, "%u %016llX\n", f, (unsigned long long)NetplayMachine::HashState(traced));
            if ((int)f == opt.SaveStateFrame)
            {
                FILE* sf = fopen(opt.SaveStatePath.c_str(), "wb");
                std::vector<u8> flat;
                traced.Serialize(flat);
                if (sf) { fwrite(flat.data(), flat.size(), 1, sf); fclose(sf); }
            }
        }

        bool check = (f + 1 >= first + dist) && (f >= (u32)opt.CheckFrom) && ((f % opt.CheckEvery) == 0);
        if (!check) continue;

        machine.SaveState(now);
        u64 hnow = NetplayMachine::HashState(now);

        u32 from = f + 1 - dist;
        if (!machine.LoadState(states[from % ring]))
        {
            printf("frame %u: could not load the state of frame %u\n", f, from);
            return 1;
        }
        for (u32 g = from; g <= f; g++)
            machine.RunFrame(inputs[g % ring].data(), true, g == f);

        machine.SaveState(again);
        u64 hagain = NetplayMachine::HashState(again);
        if (hagain != hnow)
        {
            failures++;
            if (failures <= opt.MaxReports)
            {
                printf("DESYNC at frame %u (rolled back %u frames): %016llX vs %016llX\n",
                       f, dist, (unsigned long long)hnow, (unsigned long long)hagain);
                std::vector<u8> a, b;
                now.Serialize(a);
                again.Serialize(b);
                DiffStates(a, b, n);
            }
            // continue from the original simulation
            machine.LoadState(now);
        }

        if (opt.Verbose && (f % 600) == 0)
            printf("frame %u ok\n", f);
    }

    if (trace) fclose(trace);

    double elapsed = NowMS() - tstart;
    printf("synctest: %d frames, %d console(s), rollback %u every %d frame(s): %s (%.1f s)\n",
           opt.Frames, n, dist, opt.CheckEvery, failures ? "FAILED" : "no desync", elapsed / 1000.0);
    if (failures)
        printf("  %d of the checks failed\n", failures);

    for (auto& d : opt.Dumps)
    {
        if (d.Console >= n) continue;
        NDS* nds = machine.Console(d.Console);
        printf("console %d %08X:", d.Console, d.Address);
        for (u32 i = 0; i < d.Length; i++)
            printf(" %02X", nds->ARM9Read8(d.Address + i));
        printf("\n");
    }
    printf("state hash %016llX\n", (unsigned long long)[&] {
        std::vector<u8> s; machine.SaveState(s); return NetplayMachine::HashState(s.data(), s.size()); }());

    return failures ? 1 : 0;
}

// Two machines built in one process from the same configuration must start
// identical (an online session compares their hashes): anything left
// uninitialized shows up here as a difference, since the second machine gets
// memory the first one used.
int Twins(const Options& opt)
{
    auto build = [&]() -> std::unique_ptr<NetplayMachine> {
        MachineConfig cfg;
        std::string err;
        if (!BuildConfig(opt, cfg, err))
        {
            fprintf(stderr, "%s\n", err.c_str());
            return nullptr;
        }
        return std::make_unique<NetplayMachine>(std::move(cfg));
    };
    InputScript script;
    std::string err;
    if (!opt.ScriptPath.empty() && !script.Load(opt.ScriptPath, opt.Consoles, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }

    // the first machine runs alone, keeping a hash of every 60th frame
    const int frames = std::min(opt.Frames, 1200);
    std::vector<std::vector<u8>> statesA;
    {
        auto a = build();
        if (!a || !a->IsValid()) return 2;
        auto rnd = opt.RandomSeed >= 0 ? std::make_unique<RandomInputs>((u32)opt.RandomSeed) : nullptr;
        std::vector<PlayerInput> held(opt.Consoles);
        for (int f = 0; f <= frames; f++)
        {
            if (f % 60 == 0)
            {
                statesA.emplace_back();
                a->SaveState(statesA.back());
            }
            if (f == frames) break;
            NextInputs(opt, f, script, rnd.get(), held);
            a->RunFrame(held.data());
        }
    }

    // fill the memory the second machine may get with something else than zeroes
    {
        std::vector<std::unique_ptr<u8[]>> junk;
        for (int i = 0; i < 64; i++)
        {
            junk.emplace_back(new u8[1 << 20]);
            memset(junk.back().get(), 0xA5 ^ i, 1 << 20);
        }
    }

    auto b = build();
    if (!b || !b->IsValid()) return 2;
    auto rnd = opt.RandomSeed >= 0 ? std::make_unique<RandomInputs>((u32)opt.RandomSeed) : nullptr;
    std::vector<PlayerInput> held(opt.Consoles);
    int differ = 0;
    for (int f = 0; f <= frames; f++)
    {
        if (f % 60 == 0)
        {
            std::vector<u8> sb;
            b->SaveState(sb);
            const std::vector<u8>& sa = statesA[f / 60];
            u64 ha = NetplayMachine::HashState(sa.data(), sa.size());
            u64 hb = NetplayMachine::HashState(sb.data(), sb.size());
            if (ha != hb)
            {
                if (differ++ < opt.MaxReports)
                {
                    printf("frame %d: %016llX vs %016llX\n", f, (unsigned long long)ha, (unsigned long long)hb);
                    DiffStates(sa, sb, opt.Consoles);
                }
            }
        }
        if (f == frames) break;
        NextInputs(opt, f, script, rnd.get(), held);
        b->RunFrame(held.data());
    }
    printf("twins: %d frames, %d console(s): %s\n", frames, opt.Consoles, differ ? "DIFFERENT" : "identical");
    return differ ? 1 : 0;
}

// Two computers of a session in one process, as GGPO runs them: each runs the
// whole machine and shows its own player's console (players 1 and 2); it gets
// the other players' inputs 0 to --rollback frames late (at random), runs ahead
// with the last input it got from them, and when an input arrives that differs
// from that guess, rolls back to the frame it was for. The frames neither can
// roll back any more are compared between the two. Unlike `online`, a run can
// be repeated exactly (--random seeds the delays too).
int Pair(const Options& opt)
{
    if (opt.Consoles < 2)
    {
        fprintf(stderr, "pair needs --consoles 2 or more\n");
        return 2;
    }
    const int n = opt.Consoles;
    const int depth = std::clamp(opt.Rollback, 1, 8);
    const u32 ring = (u32)depth + 3;

    struct Side
    {
        std::unique_ptr<NetplayMachine> M;
        int Player = 0;
        std::mt19937 Rng;
        std::vector<MachineState> States;               // [f % ring]: before frame f
        std::vector<std::vector<PlayerInput>> Used;     // [f % ring]: what frame f last ran with
        s64 Known = -1;                                 // the others' inputs are known up to here
        u64 Rollbacks = 0, Resimulated = 0;
        int Deepest = 0;
    };
    Side sides[2];
    std::vector<PlayerInput> initial(n);
    std::vector<std::vector<PlayerInput>> truth(ring, std::vector<PlayerInput>(n));
    InputScript script;
    std::string err;
    if (!opt.ScriptPath.empty() && !script.Load(opt.ScriptPath, n, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    u32 first = 0;
    for (int s = 0; s < 2; s++)
    {
        MachineConfig cfg;
        if (!BuildConfig(opt, cfg, err))
        {
            fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
        Side& side = sides[s];
        side.M = std::make_unique<NetplayMachine>(std::move(cfg));
        if (!side.M->IsValid())
        {
            fprintf(stderr, "%s\n", side.M->Error().c_str());
            return 2;
        }
        side.Player = s;
        side.M->SetLocalConsole(s);
        side.M->SetThreadCount(opt.Threads);
        side.M->SetAsyncRender(opt.AsyncRender);
        side.Rng.seed((u32)std::max(0, opt.RandomSeed) * 2 + 1 + s);
        side.States.resize(ring);
        side.Used.assign(ring, std::vector<PlayerInput>(n));
        // the inputs held when the start state was written (the same for both)
        std::vector<PlayerInput> held(n);
        if (!StartFrom(opt, *side.M, script, held, first))
            return 2;
        initial = held;
        side.Known = (s64)first - 1;
    }
    std::unique_ptr<RandomInputs> rnd;
    if (opt.RandomSeed >= 0) rnd = std::make_unique<RandomInputs>((u32)opt.RandomSeed);
    std::vector<PlayerInput> held = initial;

    // what a side runs frame g with: its own player's input, and the others' as
    // far as they arrived, else a guess: the last one that did (as GGPO
    // guesses), or with --bad-guesses, sometimes a random one, so that the
    // frames rolled back differ more from the right ones
    std::uniform_int_distribution<int> pct(0, 99);
    auto inputsFor = [&](Side& side, u32 g) {
        std::vector<PlayerInput> in(n);
        for (int c = 0; c < n; c++)
        {
            if (c == side.Player || (s64)g <= side.Known)
                in[c] = truth[g % ring][c];
            else if (opt.BadGuesses > 0 && pct(side.Rng) < opt.BadGuesses)
                in[c].Bits = side.Rng() & (0xFFFF0000 | PlayerInput::kTouching | PlayerInput::kButtonMask);
            else
                in[c] = side.Known >= (s64)first ? truth[side.Known % ring][c] : initial[c];
        }
        return in;
    };

    // the other players' inputs up to frame `known` arrive at side `side`, which
    // is about to run frame f: it rolls back to the first one it guessed wrong
    auto arrive = [&](Side& side, u32 f, s64 known) {
        s64 wrong = -1;
        for (s64 g = side.Known + 1; g <= known && g < (s64)f && wrong < 0; g++)
        {
            for (int c = 0; c < n; c++)
                if (c != side.Player && side.Used[g % ring][c].Bits != truth[g % ring][c].Bits)
                    wrong = g;
        }
        side.Known = std::max(side.Known, known);
        if (wrong < 0)
            return;

        side.M->LoadState(side.States[wrong % ring]);
        for (u32 g = (u32)wrong; g < f; g++)
        {
            std::vector<PlayerInput> in = inputsFor(side, g);
            side.M->SaveState(side.States[g % ring]);
            side.M->RunFrame(in.data(), true, g + 1 == f);
            side.Used[g % ring] = in;
        }
        side.Rollbacks++;
        side.Resimulated += f - (u32)wrong;
        side.Deepest = std::max(side.Deepest, (int)(f - (u32)wrong));
    };

    double tstart = NowMS();
    int compared = 0;
    const u32 end = first + (u32)opt.Frames;
    for (u32 f = first; f < end; f++)
    {
        NextInputs(opt, f, script, rnd.get(), held);
        truth[f % ring] = held;

        for (Side& side : sides)
        {
            // the other players' inputs arrive, up to `depth` frames late
            std::uniform_int_distribution<int> lag(0, depth);
            arrive(side, f, (s64)f - lag(side.Rng));

            std::vector<PlayerInput> in = inputsFor(side, f);
            side.M->SaveState(side.States[f % ring]);
            side.M->RunFrame(in.data(), false);
            side.Used[f % ring] = in;
        }

        // the state before frame g is final on both sides: every input before it is known
        s64 g = (s64)f - depth;
        if (g >= (s64)first && (g % opt.CheckEvery) == 0)
        {
            u64 h0 = NetplayMachine::HashState(sides[0].States[g % ring]);
            u64 h1 = NetplayMachine::HashState(sides[1].States[g % ring]);
            compared++;
            if (h0 != h1)
            {
                printf("pair: the two sides differ before frame %lld: %016llX vs %016llX\n", (long long)g,
                       (unsigned long long)h0, (unsigned long long)h1);
                std::vector<u8> a, b;
                sides[0].States[g % ring].Serialize(a);
                sides[1].States[g % ring].Serialize(b);
                DiffStates(a, b, n);
                return 1;
            }
        }
        if (opt.Verbose && (f % 600) == 0)
            printf("frame %u ok\n", f);
    }

    // the last inputs arrive: both sides end where a run without rollbacks
    // ends (synctest prints the same hash for the same frames and inputs)
    u64 final[2];
    for (int s = 0; s < 2; s++)
    {
        arrive(sides[s], end, (s64)end - 1);
        MachineState st;
        sides[s].M->SaveState(st);
        final[s] = NetplayMachine::HashState(st);
    }
    if (final[0] != final[1])
    {
        printf("pair: the two sides end different: %016llX vs %016llX\n", (unsigned long long)final[0],
               (unsigned long long)final[1]);
        return 1;
    }

    printf("pair: %d frames, %d console(s), inputs up to %d frames late: identical (%d comparisons, %.1f s)\n",
           opt.Frames, n, depth, compared, (NowMS() - tstart) / 1000.0);
    for (int s = 0; s < 2; s++)
        printf("  side %d: %llu rollbacks, %llu frames simulated again, deepest %d\n", s + 1,
               (unsigned long long)sides[s].Rollbacks, (unsigned long long)sides[s].Resimulated, sides[s].Deepest);
    printf("state hash %016llX\n", (unsigned long long)final[0]);
    return 0;
}

// Frames as a session runs them (GGPO): every frame saved, then run shown; every
// --pattern frames, a rollback of --rollback frames before it (load, then the
// frames again, saved, headless). Prints how long the ticks take.
static int SessionBench(const Options& opt, NetplayMachine& machine, InputScript& script,
                        RandomInputs* rnd, std::vector<PlayerInput>& held, u32 first)
{
    const u32 end = first + (u32)opt.Frames;
    const u32 from = std::max(first, (u32)opt.BenchFrom);
    for (u32 f = first; f < from && f < end; f++)
    {
        NextInputs(opt, f, script, rnd, held);
        machine.RunFrame(held.data());
    }

    const u32 depth = std::max(1, opt.Rollback);
    const u32 ring = depth + 2;
    std::vector<MachineState> states(ring);
    std::vector<std::vector<PlayerInput>> inputs(ring);
    std::vector<double> ticks;
    double budget = 1000.0 / 59.8261;
    // where the time of the ticks goes: [0] ticks without a rollback, [1] with one
    double tload = 0, tresim = 0, tresimsave = 0, tsave[2] = {}, tframe[2] = {};
    int count[2] = {};
    for (u32 f = from; f < end; f++)
    {
        NextInputs(opt, f, script, rnd, held);
        double t0 = NowMS();
        const int rb = (opt.Pattern > 0 && (f - from) >= depth && ((f - from) % opt.Pattern) == 0) ? 1 : 0;
        if (rb)
        {
            // the inputs of the last `depth` frames turned out as predicted: the
            // cost is the same as for a misprediction
            machine.LoadState(states[(f - depth) % ring]);
            double tl = NowMS();
            tload += tl - t0;
            for (u32 g = f - depth; g < f; g++)
            {
                double ts = NowMS();
                machine.SaveState(states[g % ring]);
                double tr = NowMS();
                machine.RunFrame(inputs[g % ring].data(), true, g + 1 == f);
                tresimsave += tr - ts;
                tresim += NowMS() - tr;
            }
        }
        double ts = NowMS();
        machine.SaveState(states[f % ring]);
        inputs[f % ring] = held;
        double tf = NowMS();
        machine.RunFrame(held.data(), false);
        double te = NowMS();
        tsave[rb] += tf - ts;
        tframe[rb] += te - tf;
        count[rb]++;
        ticks.push_back(te - t0);
    }

    std::vector<double> sorted = ticks;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    int over = 0;
    for (double t : ticks) { sum += t; if (t > budget) over++; }
    printf("session bench: %d console(s), %d threads, %zu ticks, rollback of %u frames every %d frames\n",
           machine.NumConsoles(), machine.ThreadCount(), ticks.size(), depth, opt.Pattern);
    printf("  tick: average %.2f ms, median %.2f ms, 95%% %.2f ms, slowest %.2f ms; %d of %zu over %.2f ms\n",
           sum / std::max<size_t>(1, ticks.size()), sorted[sorted.size() / 2], sorted[sorted.size() * 95 / 100],
           sorted.back(), over, ticks.size(), budget);
    if (count[0])
        printf("  without a rollback (%d): save %.2f ms, frame %.2f ms\n", count[0], tsave[0] / count[0],
               tframe[0] / count[0]);
    if (count[1])
        printf("  with a rollback (%d): load %.2f ms, %u frame(s) again %.2f ms and their saves %.2f ms, "
               "save %.2f ms, frame %.2f ms\n", count[1], tload / count[1], depth, tresim / count[1],
               tresimsave / count[1], tsave[1] / count[1], tframe[1] / count[1]);
    return 0;
}

int Bench(const Options& opt)
{
    MachineConfig cfg;
    std::string err;
    if (!BuildConfig(opt, cfg, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    InputScript script;
    if (!opt.ScriptPath.empty() && !script.Load(opt.ScriptPath, opt.Consoles, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    std::unique_ptr<RandomInputs> rnd;
    if (opt.RandomSeed >= 0) rnd = std::make_unique<RandomInputs>((u32)opt.RandomSeed);

    NetplayMachine machine(std::move(cfg));
    if (!machine.IsValid())
    {
        fprintf(stderr, "%s\n", machine.Error().c_str());
        return 2;
    }
    machine.SetLocalConsole(opt.Local);
    machine.SetThreadCount(opt.Threads);
    machine.SetAsyncRender(opt.AsyncRender);
    machine.SetHeadlessEnabled(!opt.NoHeadless);

    const int n = opt.Consoles;
    std::vector<PlayerInput> held(n);
    MachineState state;
    u32 first;
    if (!StartFrom(opt, machine, script, held, first))
        return 2;
    if (opt.Pattern >= 0)
        return SessionBench(opt, machine, script, rnd.get(), held, first);

    const u32 end = first + (u32)opt.Frames;
    const u32 from = std::max(first, (u32)opt.BenchFrom);

    // warm up to the frame the measurement starts at
    for (u32 f = first; f < from && f < end; f++)
    {
        NextInputs(opt, f, script, rnd.get(), held);
        machine.RunFrame(held.data());
    }

    // Each measured frame runs three times from the same state: as shown, as a
    // frame of a rollback (every console headless), and as the last frame of a
    // rollback (which renders the shown console's next 3D image). All three give
    // the same state; the last leaves the machine as the next shown frame finds it
    // in a session.
    double tshown = 0, tresim = 0, tlast = 0, tsave = 0, tload = 0;
    int count = 0;
    for (u32 f = from; f < end; f++)
    {
        NextInputs(opt, f, script, rnd.get(), held);

        double t0 = NowMS();
        machine.SaveState(state);
        double t1 = NowMS();
        machine.RunFrame(held.data(), false);
        double t2 = NowMS();
        machine.LoadState(state);
        double t3 = NowMS();
        machine.RunFrame(held.data(), true);
        double t4 = NowMS();
        machine.LoadState(state);
        double t5 = NowMS();
        machine.RunFrame(held.data(), true, true);
        double t6 = NowMS();

        tsave += t1 - t0;
        tshown += t2 - t1;
        tload += (t3 - t2) + (t5 - t4);
        tresim += t4 - t3;
        tlast += t6 - t5;
        count++;
    }

    if (!count)
    {
        printf("bench: no frames measured\n");
        return 2;
    }

    double shown = tshown / count, resim = tresim / count, last = tlast / count;
    double save = tsave / count, load = tload / (2 * count);
    // GGPO saves every frame it runs. A rollback of D frames loads a state, runs
    // D frames again (each saved; the last one renders) and then the new frame.
    double budget = 1000.0 / 59.8261;
    double normal = save + shown;
    auto tick = [&](int d) { return load + (d - 1) * (resim + save) + (last + save) + normal; };
    int depth = 0;
    while (depth < 8 && tick(depth + 1) <= budget) depth++;

    printf("bench: %d console(s), %d frames measured, %s\n", n, count,
           opt.NoHeadless ? "every console drawn" : "only the local console drawn");
    size_t statesize = state.Bus.size();
    for (const auto& c : state.Consoles) statesize += c.Length;
    printf("  threads           %d\n", machine.ThreadCount());
    printf("  async 3D render   %s\n", machine.UsesAsyncRender() ? "yes" : "no");
    printf("  state size        %.2f MB\n", statesize / 1048576.0);
    printf("  frame (shown)     %.2f ms\n", shown);
    printf("  frame (resim)     %.2f ms\n", resim);
    printf("  frame (resim, last of a rollback) %.2f ms\n", last);
    printf("  save state        %.2f ms\n", save);
    printf("  load state        %.2f ms\n", load);
    printf("  frame without rollback: %.2f ms of %.2f ms (%.0f%%)\n", normal, budget, 100.0 * normal / budget);
    printf("  frame with a rollback of 1 frame: %.2f ms; each frame more: %.2f ms\n", tick(1), resim + save);
    printf("  deepest rollback that still fits in one frame: %d\n", depth);
    return 0;
}

}

static void Usage()
{
    printf(
        "melonDS-netplay-tool: checks and measures rollback netplay without a window\n"
        "\n"
        "  melonDS-netplay-tool synctest ROM [options]\n"
        "      Runs the game, and keeps rolling back and simulating frames again,\n"
        "      checking that the result is the same every time (the game is\n"
        "      deterministic).\n"
        "  melonDS-netplay-tool twins ROM [options]\n"
        "      Builds two machines in one process and compares them every 60 frames\n"
        "      (catches state left uninitialized).\n"
        "  melonDS-netplay-tool pair ROM --consoles N [options]\n"
        "      Two computers of a session in one process: each shows its own\n"
        "      player's console, gets the other's inputs 0 to --rollback frames late,\n"
        "      guesses and rolls back as GGPO does; their machines are compared\n"
        "      once no rollback can change them. Repeatable, unlike online.\n"
        "  melonDS-netplay-tool locate ROM --check-from F [options]\n"
        "      Runs frame F on from the start, then again after loading the state\n"
        "      before it, and names the first round and savestate sections where the\n"
        "      two differ: what a savestate leaves out.\n"
        "  melonDS-netplay-tool bench ROM [options]\n"
        "      Measures the time a frame, a savestate save and a load take; with\n"
        "      --pattern, how long a session's frames take.\n"
        "  melonDS-netplay-tool play ROM [options]\n"
        "      Runs the game without rollback, drawing every console: for writing\n"
        "      input scripts (--shot) and states to start from (--save-state).\n"
        "  melonDS-netplay-tool online ROM (--host PORT | --join HOST:PORT) [options]\n"
        "      Plays an online session against another melonDS-netplay-tool\n"
        "      (direct connection), with random inputs: checks the whole path\n"
        "      (connection, setup, GGPO, desync checks). GGPO_NETWORK_DELAY=MS\n"
        "      adds latency to GGPO's packets.\n"
        "\n"
        "options:\n"
        "  --consoles N        consoles (players) in the machine (default 1)\n"
        "  --frames N          frames to run (default 3600)\n"
        "  --rollback N        synctest: frames rolled back each check (default 4);\n"
        "                      pair: most frames an input arrives late (1 to 8)\n"
        "  --check-every N     synctest: check every N frames (default 1)\n"
        "  --check-from F      synctest: first frame checked (default 0)\n"
        "  --max-reports N     synctest: desyncs described in detail (default 3)\n"
        "  --script FILE       inputs: lines 'FRAME CONSOLE BUTTONS [touch X Y]',\n"
        "                      BUTTONS like A,B,R or - for none; held until changed\n"
        "  --random SEED       random inputs (in addition to the script)\n"
        "  --random-from F     start random inputs at frame F (default 0)\n"
        "  --random-until F    last frame with random inputs\n"
        "  --random-same       every console gets the same random inputs\n"
        "  --bench-from F      bench: frame the measurement starts at (default 600)\n"
        "  --local N           console shown on this computer; the others run\n"
        "                      headless (default 0, -1 for none)\n"
        "  --no-headless       bench: draw every console (to compare)\n"
        "  --pattern N         bench: run frames as a session does, with a rollback\n"
        "                      of --rollback frames every N frames (0: none)\n"
        "  --bad-guesses PCT   pair: guess this percentage of the late inputs at\n"
        "                      random (any buttons, touches), not as GGPO does\n"
        "  --save FILE         save memory of the next console (repeat per console)\n"
        "  --bios9 FILE --bios7 FILE   DS BIOS images (default: FreeBIOS)\n"
        "  --dump C:ADDR:LEN   synctest: print LEN bytes at ADDR of console C at the end\n"
        "  --threads N         threads the machine runs its consoles on (default: one\n"
        "                      per console, up to the cores; the results are the same)\n"
        "  --sync-render, --async-render   render the shown console's 3D image on the\n"
        "                      emulation thread, or on one of its own (default: on\n"
        "                      its own when a core is left over by --threads)\n"
        "  --save-state F:FILE write the machine state after frame F\n"
        "  --start-state FILE  start from a state written by --save-state (same build,\n"
        "                      ROM and --consoles); frame numbers go on from it\n"
        "  --shot F            play, synctest, online: picture of the consoles after\n"
        "                      frame F (repeat); outside play, only the shown\n"
        "                      console is drawn\n"
        "  --shot-every N      play: ... after every N-th frame\n"
        "  --shots DIR         where the pictures go (default .)\n"
        "  --verbose           progress and core warnings\n"
        "  online: --code TEXT (session code), --name NAME, --delay FRAMES (host;\n"
        "          default from the round trip), --prediction N (1-8), --fast (no\n"
        "          60 fps pacing); --save FILE is this player's save\n");
}

int main(int argc, char** argv)
{
    using namespace NetplayTool;
    if (argc < 3)
    {
        Usage();
        return 2;
    }

    Options opt;
    std::string cmd = argv[1];
    opt.ROMPath = argv[2];
    for (int i = 3; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); }
            return argv[++i];
        };
        if (a == "--consoles") opt.Consoles = std::stoi(next());
        else if (a == "--frames") opt.Frames = std::stoi(next());
        else if (a == "--rollback") opt.Rollback = std::stoi(next());
        else if (a == "--check-every") opt.CheckEvery = std::max(1, std::stoi(next()));
        else if (a == "--check-from") opt.CheckFrom = std::stoi(next());
        else if (a == "--max-reports") opt.MaxReports = std::stoi(next());
        else if (a == "--script") opt.ScriptPath = next();
        else if (a == "--random") opt.RandomSeed = std::stoi(next());
        else if (a == "--random-from") opt.RandomFrom = std::stoi(next());
        else if (a == "--random-same") opt.RandomSame = true;
        else if (a == "--random-until") opt.RandomUntil = std::stoi(next());
        else if (a == "--bench-from") opt.BenchFrom = std::stoi(next());
        else if (a == "--save") opt.SavePaths.push_back(next());
        else if (a == "--bios9") opt.BIOS9Path = next();
        else if (a == "--bios7") opt.BIOS7Path = next();
        else if (a == "--dump")
        {
            Dump d;
            if (sscanf(next().c_str(), "%d:%x:%u", &d.Console, &d.Address, &d.Length) != 3)
            {
                fprintf(stderr, "--dump expects CONSOLE:HEXADDR:LENGTH\n");
                return 2;
            }
            opt.Dumps.push_back(d);
        }
        else if (a == "--trace") opt.TracePath = next();
        else if (a == "--local") opt.Local = std::stoi(next());
        else if (a == "--no-headless") opt.NoHeadless = true;
        else if (a == "--save-state")
        {
            std::string v = next();
            auto colon = v.find(':');
            if (colon == std::string::npos) { fprintf(stderr, "--save-state expects FRAME:FILE\n"); return 2; }
            opt.SaveStateFrame = std::stoi(v.substr(0, colon));
            opt.SaveStatePath = v.substr(colon + 1);
        }
        else if (a == "--diff")
        {
            // compare two state files written by --save-state
            std::string fa = next(), fb = next();
            std::vector<u8> sa, sb;
            if (!ReadFile(fa, sa) || !ReadFile(fb, sb)) { fprintf(stderr, "cannot read states\n"); return 2; }
            DiffStates(sa, sb, opt.Consoles);
            return 0;
        }
        else if (a == "--start-state") opt.StartStatePath = next();
        else if (a == "--threads") opt.Threads = std::stoi(next());
        else if (a == "--sync-render") opt.AsyncRender = 0;
        else if (a == "--async-render") opt.AsyncRender = 1;
        else if (a == "--pattern") opt.Pattern = std::stoi(next());
        else if (a == "--bad-guesses") opt.BadGuesses = std::stoi(next());
        else if (a == "--shot") opt.ShotFrames.push_back(std::stoi(next()));
        else if (a == "--shot-every") opt.ShotEvery = std::stoi(next());
        else if (a == "--shots") opt.ShotDir = next();
        else if (a == "--verbose") { opt.Verbose = true; HeadlessPlatform::LogVerbosity = 1; }
        else if (a == "--host") opt.HostPort = std::stoi(next());
        else if (a == "--join") opt.JoinAddress = next();
        else if (a == "--code") opt.Code = next();
        else if (a == "--name") opt.PlayerName = next();
        else if (a == "--delay") opt.Delay = std::stoi(next());
        else if (a == "--prediction") opt.MaxPrediction = std::stoi(next());
        else if (a == "--fast") opt.Fast = true;
        else
        {
            fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (!opt.Verbose) HeadlessPlatform::LogVerbosity = 0;
    if (opt.Consoles < 1 || opt.Consoles > 4)
    {
        fprintf(stderr, "--consoles must be 1 to 4\n");
        return 2;
    }

    if (cmd == "synctest") return SyncTest(opt);
    if (cmd == "bench") return Bench(opt);
    if (cmd == "online") return Online(opt);
    if (cmd == "twins") return Twins(opt);
    if (cmd == "pair") return Pair(opt);
    if (cmd == "play") return Play(opt);
    if (cmd == "locate") return Locate(opt);
    Usage();
    return 2;
}
