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

#ifndef NETPLAYTOOL_COMMON_H
#define NETPLAYTOOL_COMMON_H

#include <map>
#include <random>
#include <string>
#include <vector>

#include "types.h"
#include "SPI_Firmware.h"
#include "NetplayInput.h"
#include "NetplayMachine.h"

namespace NetplayTool
{

struct Dump
{
    int Console = 0;
    melonDS::u32 Address = 0;
    melonDS::u32 Length = 0;
};

struct Options
{
    std::string ROMPath;
    int Consoles = 1;
    int Local = 0;
    bool NoHeadless = false;
    int Frames = 3600;
    int Rollback = 4;
    int CheckEvery = 1;
    int CheckFrom = 0;
    int MaxReports = 3;
    std::string ScriptPath;
    int RandomSeed = -1;
    int RandomFrom = 0;
    int RandomUntil = -1;           // last frame with random inputs (-1: no end)
    bool RandomSame = false;        // every console gets console 0's random input
    int BenchFrom = 600;
    std::vector<std::string> SavePaths;
    std::string BIOS9Path, BIOS7Path;
    std::vector<Dump> Dumps;
    std::string TracePath;          // per-frame state hashes
    int SaveStateFrame = -1;        // write the state after this frame
    std::string SaveStatePath;
    std::string StartStatePath;     // start from this state (written by --save-state)
    std::vector<int> ShotFrames;    // play: pictures of the consoles after these frames
    int ShotEvery = 0;              // play: ... and every N frames
    std::string ShotDir = ".";
    bool Verbose = false;
    int Threads = 0;                // threads the machine uses (0: its default)
    int AsyncRender = -1;           // the shown console's 3D image on a thread of its own (-1: machine default)
    int Pattern = -1;               // bench: a rollback every N frames (0: none), as in a session
    int BadGuesses = 0;             // pair: percent of guessed inputs made at random

    // online
    int HostPort = 0;
    std::string JoinAddress;
    std::string Code = "test";
    std::string PlayerName;
    int Delay = -1;
    int MaxPrediction = 8;
    bool Fast = false;
};

// Inputs read from a text file: each line holds a console's input from a frame on.
class InputScript
{
public:
    bool Load(const std::string& path, int numconsoles, std::string& err);
    void Get(melonDS::u32 frame, std::vector<melonDS::RollbackNet::PlayerInput>& held);

private:
    std::map<melonDS::u32, std::map<int, melonDS::RollbackNet::PlayerInput>> Changes;
};

// Random button presses and touches, reproducible from a seed.
class RandomInputs
{
public:
    explicit RandomInputs(melonDS::u32 seed) : Rng(seed) {}
    void Get(std::vector<melonDS::RollbackNet::PlayerInput>& held);

private:
    std::mt19937 Rng;
};

bool ReadFile(const std::string& path, std::vector<melonDS::u8>& out);
// every console's input for a frame: the random inputs (from --random-from to
// --random-until), then the script's lines for that frame; `held` carries over
void NextInputs(const Options& opt, melonDS::u32 frame, InputScript& script, RandomInputs* rnd,
                std::vector<melonDS::RollbackNet::PlayerInput>& held);
melonDS::Firmware MakeFirmware(int index, const std::string& nickname);
bool BuildConfig(const Options& opt, melonDS::RollbackNet::MachineConfig& cfg, std::string& err);
void DiffStates(const std::vector<melonDS::u8>& a, const std::vector<melonDS::u8>& b, int numconsoles);
// a picture of the machine's consoles (both screens, side by side) in opt.ShotDir
void WriteShot(const Options& opt, melonDS::RollbackNet::NetplayMachine& machine, melonDS::u32 frame);
int Online(const Options& opt);

}

#endif
