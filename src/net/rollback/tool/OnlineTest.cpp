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

// melonDS-netplay-tool online: plays a whole online session (direct connection,
// setup, GGPO, desync checks) between two processes, without a window.

#include <chrono>
#include <cstdio>
#include <thread>

#include "ToolCommon.h"
#include "OnlineSession.h"
#include "P2PLink.h"

using namespace melonDS;
using namespace melonDS::RollbackNet;

namespace NetplayTool
{

static double Now()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

int Online(const Options& opt)
{
    bool host = opt.HostPort != 0;
    if (host == !opt.JoinAddress.empty())
    {
        fprintf(stderr, "online: give either --host PORT or --join HOST:PORT\n");
        return 2;
    }

    LocalSetup local;
    if (!ReadFile(opt.ROMPath, local.ROM))
    {
        fprintf(stderr, "cannot read ROM %s\n", opt.ROMPath.c_str());
        return 2;
    }
    auto slash = opt.ROMPath.find_last_of("/\\");
    local.ROMName = slash == std::string::npos ? opt.ROMPath : opt.ROMPath.substr(slash + 1);
    local.Profile.Name = opt.PlayerName.empty() ? (host ? "Host" : "Guest") : opt.PlayerName;
    if (!opt.SavePaths.empty() && !ReadFile(opt.SavePaths[0], local.Save))
    {
        fprintf(stderr, "cannot read save %s\n", opt.SavePaths[0].c_str());
        return 2;
    }
    local.InputDelay = opt.Delay;
    local.Threads = opt.Threads;
    local.MaxPrediction = opt.MaxPrediction;

    auto link = std::make_shared<P2PLink>();
    P2PLink::Options lo;
    std::string err;
    bool started;
    if (host)
    {
        lo.LocalPort = (u16)opt.HostPort;
        lo.ConnectTimeoutMs = 0;
        started = link->StartHost(lo, opt.Code, err);
    }
    else
    {
        lo.ConnectTimeoutMs = 20000;
        started = link->StartGuest(lo, opt.JoinAddress, opt.Code, err);
    }
    if (!started)
    {
        fprintf(stderr, "online: %s\n", err.c_str());
        return 2;
    }
    printf("%s on port %u\n", host ? "hosting" : "joining", link->LocalPort());

    OnlineSession session(link, host, std::move(local));

    // setup
    std::string lastStatus;
    double setupStart = Now();
    while (session.GetPhase() != OnlineSession::Phase::Running && session.GetPhase() != OnlineSession::Phase::Ended)
    {
        session.Poll();
        std::string st = session.StatusText();
        if (st != lastStatus)
        {
            printf("[%7.0f ms] %s\n", Now() - setupStart, st.c_str());
            fflush(stdout);
            lastStatus = st;
        }
        if (Now() - setupStart > 120000)
        {
            session.Leave("setup took too long");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (session.GetPhase() == OnlineSession::Phase::Ended)
    {
        printf("ended during setup: %s\n", session.EndReason().c_str());
        return 1;
    }
    printf("[%7.0f ms] running as player %d (%s vs %s), input delay %d\n", Now() - setupStart,
           session.LocalPlayer() + 1, session.PlayerName(0).c_str(), session.PlayerName(1).c_str(),
           session.InputDelay());
    fflush(stdout);

    // play: this player's inputs, from the script's lines for its console and/or random
    InputScript script;
    if (!opt.ScriptPath.empty() && !script.Load(opt.ScriptPath, 2, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        session.Leave("bad script");
        return 2;
    }
    std::unique_ptr<RandomInputs> rnd;
    if (opt.RandomSeed >= 0)
        rnd = std::make_unique<RandomInputs>((u32)opt.RandomSeed);
    const int me = session.LocalPlayer();

    // Every console's inputs, worked out up front as synctest and play do, so
    // that the machine gets the same inputs as there: an input given at frame f
    // takes effect at f + input delay, so this player gives at f its input for
    // that frame.
    const int delay = std::max(0, session.InputDelay());
    std::vector<PlayerInput> plan((size_t)opt.Frames + delay + 1);
    {
        std::vector<PlayerInput> held(2);
        for (size_t f = 0; f < plan.size(); f++)
        {
            NextInputs(opt, (u32)f, script, rnd.get(), held);
            plan[f] = held[me];
        }
    }

    const double frameMs = 1000.0 / 59.8261;
    double next = Now();
    double playStart = next;
    int frames = 0, waits = 0;
    double worstFrame = 0;
    while (frames < opt.Frames && session.GetPhase() == OnlineSession::Phase::Running)
    {
        session.Poll();
        PlayerInput in = plan[(size_t)frames + delay];

        double t0 = Now();
        auto res = session.RunFrame(in);
        double t1 = Now();

        if (res == RollbackSession::FrameResult::Ran)
        {
            frames++;
            for (int f : opt.ShotFrames)
            {
                // after frame f (counted from 0, as play does); the shown
                // console only: the others run headless
                if (f + 1 == frames && session.Machine())
                    WriteShot(opt, *session.Machine(), (u32)f);
            }
            worstFrame = std::max(worstFrame, t1 - t0);
            if (opt.Verbose && t1 - t0 > 30)
                printf("frame %d took %.1f ms\n", frames, t1 - t0);
            if (opt.Verbose && frames % 600 == 0)
            {
                SessionStats s = session.Stats();
                printf("frame %d: ping %d ms, rollbacks %llu (max %d frames), checks %d\n", frames, s.Ping,
                       (unsigned long long)s.Rollbacks, s.MaxRollbackDepth, s.ChecksPassed);
                fflush(stdout);
            }
            next += frameMs * (1.0 + session.TakeFrameWait());
        }
        else if (res == RollbackSession::FrameResult::Waiting)
        {
            waits++;
            next = std::max(next, Now() + 1.0);
        }
        else
            break;

        if (!opt.Fast)
        {
            double now = Now();
            if (next > now)
                std::this_thread::sleep_for(std::chrono::microseconds((long long)((next - now) * 1000)));
            else if (now - next > 100)
                next = now;     // fell behind (a slow frame): do not race to catch up
        }
    }

    double elapsed = Now() - playStart;
    SessionStats s = session.Stats();

    // let the other side reach the same frame before leaving
    if (session.GetPhase() == OnlineSession::Phase::Running)
    {
        double until = Now() + 3000;
        while (Now() < until && session.GetPhase() == OnlineSession::Phase::Running)
        {
            session.Poll();
            // waits at the prediction limit; keeps GGPO polling
            session.RunFrame(plan[std::min(plan.size() - 1, (size_t)frames + delay)]);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        s = session.Stats();
        if (session.GetPhase() == OnlineSession::Phase::Running)
            session.Leave("test finished");
    }

    printf("online: %d frames in %.1f s (%.1f fps), %d waits; ping %d ms; rollbacks %llu, %llu frames simulated again "
           "(deepest %d); slowest frame %.1f ms; desync checks passed %d (last at frame %u)\n",
           frames, elapsed / 1000.0, frames * 1000.0 / std::max(elapsed, 1.0), waits, s.Ping,
           (unsigned long long)s.Rollbacks, (unsigned long long)s.FramesResimulated, s.MaxRollbackDepth,
           worstFrame, s.ChecksPassed, s.LastCheckedFrame);
    printf("end: %s\n", session.EndReason().c_str());

    bool desync = session.EndReason().find("out of sync") != std::string::npos;
    return (frames >= opt.Frames && !desync) ? 0 : 1;
}

}
