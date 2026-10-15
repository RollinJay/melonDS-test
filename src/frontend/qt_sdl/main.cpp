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

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include <optional>
#include <string>

#include <QApplication>
#include <QStyle>
#include <QMessageBox>
#include <QMenuBar>
#include <QFileDialog>
#include <QInputDialog>
#include <QPainter>
#include <QKeyEvent>
#include <QMimeData>
#include <QVector>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#ifndef _WIN32
#include <QGuiApplication>
#include <QSocketNotifier>
#include <unistd.h>
#include <sys/socket.h>
#include <csignal>
#endif

#include <SDL2/SDL.h>

#include "OpenGLSupport.h"
#include "duckstation/gl/context.h"

#include "main.h"
#include "version.h"

#include "Config.h"

#include "EmuInstance.h"
#include "ArchiveUtil.h"
#include "CameraManager.h"
#include "MPInterface.h"
#include "Net.h"

#include "CLI.h"
#include "NetplayLauncher.h"
#include "NetplayOnlineDialog.h"

#include "Net_PCap.h"
#include "Net_Slirp.h"

using namespace melonDS;

QString* systemThemeName;


QString emuDirectory;

const int kMaxEmuInstances = 16;
EmuInstance* emuInstances[kMaxEmuInstances];

CameraManager* camManager[2];
bool camStarted[2];

std::optional<LibPCap> pcap;
Net net;


QElapsedTimer sysTimer;


void NetInit()
{
    Config::Table cfg = Config::GetGlobalTable();
    if (cfg.GetBool("LAN.DirectMode"))
    {
        if (!pcap)
            pcap = LibPCap::New();

        if (pcap)
        {
            std::string devicename = cfg.GetString("LAN.Device");
            std::unique_ptr<Net_PCap> netPcap = pcap->Open(devicename, [](const u8* data, int len) {
                net.RXEnqueue(data, len);
            });

            if (netPcap)
            {
                net.SetDriver(std::move(netPcap));
            }
        }
    }
    else
    {
        net.SetDriver(std::make_unique<Net_Slirp>([](const u8* data, int len) {
            net.RXEnqueue(data, len);
        }));
    }
}


bool createEmuInstance()
{
    int id = -1;
    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        if (!emuInstances[i])
        {
            id = i;
            break;
        }
    }

    if (id == -1)
        return false;

    auto inst = new EmuInstance(id);
    emuInstances[id] = inst;

    return true;
}

void deleteEmuInstance(int id)
{
    auto inst = emuInstances[id];
    if (!inst) return;

    delete inst;
    emuInstances[id] = nullptr;
}

void deleteAllEmuInstances(int first)
{
    for (int i = first; i < kMaxEmuInstances; i++)
        deleteEmuInstance(i);
}

int numEmuInstances()
{
    int ret = 0;

    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        if (emuInstances[i])
            ret++;
    }

    return ret;
}


void broadcastInstanceCommand(int cmd, QVariant& param, int sourceinst)
{
    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        if (i == sourceinst) continue;
        if (!emuInstances[i]) continue;

        emuInstances[i]->handleCommand(cmd, param);
    }
}


void pathInit()
{
    // First, check for the portable directory next to the executable.
    QString appdirpath = QCoreApplication::applicationDirPath();
    QString portablepath = appdirpath + QDir::separator() + "portable";

#if defined(__APPLE__)
    // On Apple platforms we may need to navigate outside an app bundle.
    // The executable directory would be "melonDS.app/Contents/MacOS", so we need to go a total of three steps up.
    QDir bundledir(appdirpath);
    if (bundledir.cd("..") && bundledir.cd("..") && bundledir.dirName().endsWith(".app") && bundledir.cd(".."))
    {
        portablepath = bundledir.absolutePath() + QDir::separator() + "portable";
    }
#endif

    QDir portabledir(portablepath);
    if (portabledir.exists())
    {
        emuDirectory = portabledir.absolutePath();
    }
    else
    {
        // If no overrides are specified, use the default path.
#if defined(__WIN32__) && defined(WIN32_PORTABLE)
        emuDirectory = appdirpath;
#else
        QString confdir;
        QDir config(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation));
        config.mkdir("melonDS");
        confdir = config.absolutePath() + QDir::separator() + "melonDS";
        emuDirectory = confdir;
#endif
    }
}


void setMPInterface(MPInterfaceType type)
{
    // switch to the requested MP interface
    MPInterface::Set(type);

    // set receive timeout
    // TODO: different settings per interface?
    MPInterface::Get().SetRecvTimeout(Config::GetGlobalTable().GetInt("MP.RecvTimeout"));

    // update UI appropriately
    // TODO: decide how to deal with multi-window when it becomes a thing
    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        EmuInstance* inst = emuInstances[i];
        if (!inst) continue;

        MainWindow* win = inst->getMainWindow();
        if (win) win->updateMPInterface(type);
    }
}



MelonApplication::MelonApplication(int& argc, char** argv)
    : QApplication(argc, argv)
{
#if !defined(Q_OS_APPLE)
    setWindowIcon(QIcon(":/melon-icon"));
    #if defined(Q_OS_UNIX)
        setDesktopFileName(QString("net.kuribo64.melonDS"));
    #endif
#endif
}

// TODO: ROM loading should be moved to EmuInstance
// especially so the preloading below and in main() can be done in a nicer fashion

bool MelonApplication::event(QEvent *event)
{
    if (event->type() == QEvent::FileOpen)
    {
        EmuInstance* inst = emuInstances[0];
        MainWindow* win = inst->getMainWindow();
        QFileOpenEvent *openEvent = static_cast<QFileOpenEvent*>(event);

        const QStringList file = win->splitArchivePath(openEvent->file(), true);
        win->preloadROMs(file, {}, true);
    }

    return QApplication::event(event);
}

#ifndef _WIN32
// SIGINT and SIGTERM quit melonDS as closing its window does. The signal can
// come while the UI thread is in the middle of anything (drawing a screen,
// ...), and quitting deletes the emulator instances, so the handler only
// raises a flag, which the UI thread reads at a safe point (see main).
static volatile std::sig_atomic_t quitSignalled = 0;
// QApplication::quit() does nothing before main's event loop runs
static bool mainLoopRunning = false;

static void signalHandler(int signal)
{
    std::signal(signal, SIG_DFL);
    quitSignalled = 1;
}
#endif

// --netplay-identity <rom>: prints the ROM's online identity (see
// NetplayLauncher) on the last line of standard output and returns 0, or an
// error on standard error and 1. Runs before Qt's window system starts, so that
// it works without a display: launchers run it to match players.
static int printNetplayIdentity(const char* rom)
{
    if (!rom || !*rom)
    {
        fprintf(stderr, "melonDS: --netplay-identity needs a ROM\n");
        return 1;
    }
#ifdef _WIN32
    QString path = QString::fromUtf8(rom);      // SDL's WinMain passes UTF-8
#else
    QString path = QString::fromLocal8Bit(rom);
#endif
    QString error;
    QString identity = NetplayLauncher::IdentityOfPath(path, error);
    if (identity.isEmpty())
    {
        fprintf(stderr, "melonDS: %s\n", error.toUtf8().constData());
        return 1;
    }
    printf("%s\n", identity.toLatin1().constData());
    fflush(stdout);
    return 0;
}

// the same ROM: the same file (or archive, and the same file in it)
static bool sameROMFiles(const QStringList& a, const QStringList& b)
{
    if (a.isEmpty() || a.size() != b.size())
        return false;
    QString first = QFileInfo(a.first()).canonicalFilePath();
    return !first.isEmpty() && first == QFileInfo(b.first()).canonicalFilePath() && a.mid(1) == b.mid(1);
}

int main(int argc, char** argv)
{
    sysTimer.start();
    srand(time(nullptr));

    for (int i = 0; i < kMaxEmuInstances; i++)
        emuInstances[i] = nullptr;

    qputenv("QT_SCALE_FACTOR", "1");

#if defined(_WIN32)
#if QT_VERSION_MAJOR == 6
    // Allow using the system dark theme palette on Windows
    qputenv("QT_QPA_PLATFORM", "windows:darkmode=2");
#endif

    // argc and argv are passed as UTF8 by SDL's WinMain function
    // QT checks for the original value in local encoding though
    // to see whether it is unmodified to activate its hack that
    // retrieves the unicode value via CommandLineToArgvW.
    argc = __argc;
    argv = __argv;

    // Check whether we are already attached to an output stream.
    HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!outputHandle || (outputHandle == INVALID_HANDLE_VALUE))
    {
        // If started from terminal, attach and output logs to it.
        if (AttachConsole(ATTACH_PARENT_PROCESS))
        {
            freopen("CONOUT$", "a", stdout);
            freopen("CONOUT$", "a", stderr);
        }
        else
        {
            // Otherwise, discard log output.
            freopen("NUL:", "w", stdout);
            freopen("NUL:", "w", stderr);
        }
    }
#else
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#endif

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--"))
            break;
        if (!strcmp(argv[i], "--netplay-identity"))
            return printNetplayIdentity(i + 1 < argc ? argv[i + 1] : nullptr);
        if (!strncmp(argv[i], "--netplay-identity=", 19))
            return printNetplayIdentity(argv[i] + 19);
    }

    printf("melonDS " MELONDS_VERSION "\n");
    printf(MELONDS_URL "\n");

    // easter egg - not worth checking other cases for something so dumb
    if (argc != 0 && (!strcasecmp(argv[0], "derpDS") || !strcasecmp(argv[0], "./derpDS")))
        printf("did you just call me a derp???\n");

    MelonApplication melon(argc, argv);
    pathInit();

#ifndef _WIN32
    QTimer* signalCheck = new QTimer(&melon);
    QObject::connect(signalCheck, &QTimer::timeout, &melon, []
    {
        if (!quitSignalled)
            return;
        if (mainLoopRunning)
        {
            quitSignalled = 0;
            qApp->quit();
        }
        // a message box shown at start runs its own loop: the flag stays up,
        // the box is closed as quit() closes boxes, and main quits before its
        // loop
        else if (QWidget* box = QApplication::activeModalWidget())
            box->close();
    });
    signalCheck->start(100);
#endif

    // a match that a launcher found: its file holds the launcher's session, so
    // it is read and deleted first thing, whatever happens next
    NetplayLauncher::ConnectionFile launcherMatch;
    QString launcherError;
    const bool fromLauncher = NetplayLauncher::TakeConnectionFile(launcherMatch, launcherError);

    CLI::CommandLineOptions* options = CLI::ManageArgs(melon);

    QStringList launcherROM;
    if (!launcherError.isEmpty())
    {
        fprintf(stderr, "netplay: %s\n", launcherError.toUtf8().constData());
        return 1;
    }
    if (fromLauncher)
    {
        if (options->netplayHost.has_value() || options->netplayJoin.has_value() || options->netplayOnline)
        {
            fprintf(stderr, "netplay: a launcher's match (NETPLAY_CONNECT) cannot be played together with "
                            "--netplay-host, --netplay-join, --netplay-online or --netplay-quick\n");
            return 1;
        }
        // the ROM the launcher matched on, which must still be the same game
        QString error;
        launcherROM = MainWindow::romFilesWithoutAsking(launcherMatch.ROMPath, error);
        if (launcherROM.isEmpty())
        {
            fprintf(stderr, "netplay: the launcher's match names a ROM melonDS cannot use: %s\n", error.toUtf8().constData());
            return 1;
        }
        if (options->dsRomPath.has_value())
        {
            QStringList given = options->dsRomArchivePath.has_value()
                ? QStringList {*options->dsRomPath, *options->dsRomArchivePath}
                : MainWindow::romFilesWithoutAsking(*options->dsRomPath, error);
            if (!sameROMFiles(given, launcherROM))
            {
                fprintf(stderr, "netplay: the ROM on the command line (%s) is not the one of the launcher's match (%s)\n",
                        options->dsRomPath->toUtf8().constData(), launcherMatch.ROMPath.toUtf8().constData());
                return 1;
            }
        }
        QString identity = NetplayLauncher::IdentityOfFiles(launcherROM, error);
        if (identity.isEmpty())
        {
            fprintf(stderr, "netplay: %s\n", error.toUtf8().constData());
            return 1;
        }
        if (identity != launcherMatch.Identity)
        {
            fprintf(stderr, "netplay: %s is not the game the launcher matched on (its identity differs: the ROM changed "
                            "since, or the launcher asked another melonDS)\n", launcherMatch.ROMPath.toUtf8().constData());
            return 1;
        }
        // played like a ROM given on the command line
        if (options->bootAuto)
            options->boot = true;
    }
    if (options->netplayConfig.has_value())
    {
        NetplayLauncher::ServerProfile profile;
        QString error;
        if (!NetplayLauncher::ReadServerProfile(*options->netplayConfig, options->netplayServer.value_or(QString()),
                                                profile, error))
        {
            fprintf(stderr, "melonDS: --netplay-config: %s\n", error.toUtf8().constData());
            return 1;
        }
        NetplayOnlineDialog::useServerProfile(profile);
    }

    // http://stackoverflow.com/questions/14543333/joystick-wont-work-using-sdl
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    SDL_SetHint(SDL_HINT_APP_NAME, "melonDS");

    if (SDL_Init(SDL_INIT_HAPTIC) < 0)
    {
        printf("SDL couldn't init rumble\n");
    }
    if (SDL_Init(SDL_INIT_JOYSTICK) < 0)
    {
        printf("SDL couldn't init joystick\n");
    }
    if (SDL_Init(SDL_INIT_SENSOR) < 0)
    {
        printf("SDL couldn't init motion sensors\n");
    }
    if (SDL_Init(SDL_INIT_AUDIO) < 0)
    {
        const char* err = SDL_GetError();
        QString errorStr = "Failed to initialize SDL. This could indicate an issue with your audio driver.\n\nThe error was: ";
        errorStr += err;

        QMessageBox::critical(nullptr, "melonDS", errorStr);
        return 1;
    }

    SDL_JoystickEventState(SDL_ENABLE);

    SDL_InitSubSystem(SDL_INIT_VIDEO);
    SDL_EnableScreenSaver(); SDL_DisableScreenSaver();

    if (!Config::Load())
        QMessageBox::critical(nullptr,
                              "melonDS",
                              "Unable to write to config.\nPlease check the write permissions of the folder you placed melonDS in.");

    camStarted[0] = false;
    camStarted[1] = false;
    camManager[0] = new CameraManager(0, 640, 480, true);
    camManager[1] = new CameraManager(1, 640, 480, true);

    systemThemeName = new QString(QApplication::style()->objectName());

    {
        Config::Table cfg = Config::GetGlobalTable();
        QString uitheme = cfg.GetQString("UITheme");
        if (!uitheme.isEmpty())
        {
            QApplication::setStyle(uitheme);
        }
    }

    // fix for Wayland OpenGL glitches
    QGuiApplication::setAttribute(Qt::AA_NativeWindows, false);
    QGuiApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);

    // default MP interface type is local MP
    // this will be changed if a LAN or netplay session is initiated
    setMPInterface(MPInterface_Local);

    NetInit();

    createEmuInstance();

    bool startFailed = false;
    {
        MainWindow* win = emuInstances[0]->getMainWindow();
        bool memberSyntaxUsed = false;
        const auto prepareRomPath = [&](const std::optional<QString> &romPath,
                                        const std::optional<QString> &romArchivePath) -> QStringList
        {
            if (!romPath.has_value())
                return {};

            if (romArchivePath.has_value())
                return {*romPath, *romArchivePath};

            const QStringList path = win->splitArchivePath(*romPath, true);
            if (path.size() > 1) memberSyntaxUsed = true;
            return path;
        };

        const QStringList dsfile = fromLauncher ? launcherROM : prepareRomPath(options->dsRomPath, options->dsRomArchivePath);
        const QStringList gbafile = prepareRomPath(options->gbaRomPath, options->gbaRomArchivePath);

        if (memberSyntaxUsed) printf("Warning: use the a.zip|b.nds format at your own risk!\n");

        bool loaded = win->preloadROMs(dsfile, gbafile, options->boot);

        if (options->fullscreen)
            win->toggleFullscreen();

        // online play started from the command line: without its game, it ends here
        const bool netplayNeedsROM = fromLauncher || options->netplayHost.has_value() ||
                                     options->netplayJoin.has_value() || options->netplayQuick;
        if (netplayNeedsROM && !loaded)
        {
            fprintf(stderr, "netplay: %s\n", fromLauncher ? "could not load the ROM of the launcher's match"
                                                          : "give the ROM to play on the command line");
            startFailed = true;
        }
        else if (fromLauncher)
        {
            NetplayOnlineDialog* dlg = NetplayOnlineDialog::openDlg(win, win->getEmuInstance());
            dlg->startFromLauncher(launcherMatch);
        }
        else if (options->netplayHost.has_value() || options->netplayJoin.has_value())
        {
            NetplayOnlineDialog* dlg = NetplayOnlineDialog::openDlg(win, win->getEmuInstance());
            dlg->startDirect(options->netplayHost.has_value(),
                             options->netplayHost.value_or(0),
                             options->netplayJoin.value_or(QString()),
                             options->netplayCode.value_or(QString()),
                             options->netplayDelay.value_or(-2),
                             options->netplayExit);
        }
        else if (options->netplayOnline)
        {
            NetplayOnlineDialog* dlg = NetplayOnlineDialog::openDlg(win, win->getEmuInstance());
            dlg->connectAtStart(options->netplayQuick, options->netplayExit);
        }
    }

#ifndef _WIN32
    // SIGINT or SIGTERM during the start: quit as if the window were closed
    // right away
    const bool quitAtStart = quitSignalled;
    mainLoopRunning = true;
#else
    const bool quitAtStart = false;
#endif
    int ret = startFailed ? 1 : quitAtStart ? 0 : melon.exec();

    delete options;

    // if we get here, all the existing emu instances should have been deleted already
    // but with this we make extra sure they are all deleted
    deleteAllEmuInstances();

    delete camManager[0];
    delete camManager[1];

    Config::Save();

    SDL_Quit();
    return ret;
}
