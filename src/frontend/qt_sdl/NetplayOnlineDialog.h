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

#ifndef NETPLAYONLINEDIALOG_H
#define NETPLAYONLINEDIALOG_H

#include <memory>

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include "NakamaClient.h"
#include "NetplayLauncher.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextBrowser;
class QTreeWidget;
class QTimer;
class EmuInstance;

namespace melonDS::RollbackNet
{
class P2PLink;
}

// The online window: game channels on a Nakama server, in the manner of
// Fightcade. A channel gathers the players of one game: who is there and
// who is looking for a game, their chat and challenges. Quick match finds an
// opponent with Nakama's matchmaker, with the same ticket as Nakama
// Matchmaker's launcher, so players of both meet; a match that the launcher
// found is played here too (NETPLAY_CONNECT). When two players are paired,
// their computers connect directly and play a rollback session
// (melonDS::RollbackNet::OnlineSession, run by the emulation thread). Direct
// connections without a server are here too.
class NetplayOnlineDialog : public QWidget
{
    Q_OBJECT

public:
    // the one window (created on first use)
    static NetplayOnlineDialog* openDlg(QWidget* parent, EmuInstance* inst);
    // Deletes the window if it plays on this instance, which is going away: the
    // window uses the instance and its emulation thread, so it must not outlive
    // them (nor get the end of a session that the thread posts as it stops).
    static void closeDlg(EmuInstance* inst);

    explicit NetplayOnlineDialog(QWidget* parent, EmuInstance* inst);
    ~NetplayOnlineDialog() override;

    // A direct session without a server: the host waits on its UDP port, the
    // guest connects to address ("host:port"). delay: input delay in frames,
    // -1 from the round trip, -2 the settings'. quitAfter: quit melonDS when it ends.
    bool startDirect(bool host, int port, const QString& address, const QString& code, int delay, bool quitAfter);

    // A server and account from a launcher (--netplay-config), used instead of
    // the online settings' server, name, STUN servers and device id for the
    // rest of this run, and saved nowhere. Call it before the window exists.
    static void useServerProfile(const NetplayLauncher::ServerProfile& profile);
    // Connects at start (--netplay-online); with quick, then starts a quick
    // match (--netplay-quick). quitAfter: quit melonDS when the session ends,
    // or when connecting or the quick match fails before one starts.
    void connectAtStart(bool quick, bool quitAfter);
    // Plays the match that a launcher found (NETPLAY_CONNECT), whose ROM is
    // loaded: signs in with the launcher's session, joins the match, connects
    // to the other player, plays, and quits melonDS with exit status 0 when a
    // player left, 1 when it failed.
    void startFromLauncher(const NetplayLauncher::ConnectionFile& match);

private slots:
    void onConnectClicked();
    void onSettingsClicked();
    void onDirectClicked();
    void onConnectionChanged(bool connected, QString error);
    void onMatchJoined(QString matchID);
    void onMatchJoinFailed(QString matchID, QString error);
    void onMatchJoinedByToken(QString matchToken, QString matchID);
    void onMatchJoinByTokenFailed(QString matchToken, QString error);
    void onMatchData(QString matchID, QString senderID, qint64 op, QByteArray data);
    void onMatchPresence(QString matchID, QStringList joined, QStringList left);
    void onMatchmakerMatched(NakamaMatched matched);
    void onMatchmakerFailed(QString error);
    void onRoundTrip(int ms);

    void refreshChannels();
    void onChannelActivated();
    void joinOwnGameChannel();
    void onSendChat();
    void onLookingToggled(bool on);
    void onChallengeClicked();
    void onQuickMatchClicked();
    void onLeaveSessionClicked();

    void onSessionStatus(QString text);
    void onSessionChat(QString name, QString text, bool notice);
    void onSessionGameStarted();
    void onSessionEnded(QString reason, bool failed);

private:
    struct Settings;
    Settings loadSettings();
    // this install's account id on servers (created and saved the first time)
    QString ownDeviceID();

    void showChannels();
    void joinChannel(const QString& game, const QString& title);
    void leaveChannel();
    void sendChannel(qint64 op, const QJsonObject& payload);
    void sendStatus();
    void appendChat(const QString& name, const QString& text, bool notice);
    void setMembers(const QJsonArray& members);
    void updateButtons();

    // the loaded game's channel id and title; false (and why) if none is loaded
    bool loadedGame(QString& game, QString& title, QString& why);

    void startPairing(const QJsonObject& pairing);
    void startSession(std::shared_ptr<melonDS::RollbackNet::P2PLink> link, bool host);

    // quick match through Nakama's matchmaker; false (and why) if it cannot start
    bool startQuickMatch(QString& why);
    void stopQuickMatch();
    // the identity of the loaded game (see NetplayLauncher), cached
    QString loadedIdentity(QString& error);

    // A game in a match that Nakama's matchmaker made: here (quick match) or
    // in a launcher (NETPLAY_CONNECT).
    struct MatchedGame
    {
        QString MatchID;            // an authoritative match...
        QString MatchToken;         // ...or the token of a relayed one
        QString JoinedID;           // the match, once joined
        QStringList UserIDs, Names; // the matchmaker's order: player 1 (the host) first
        int Self = -1;
        QString Identity;           // this player's game, as the hello says
        QStringList Stun;           // empty: the settings'
        int Port = 0;               // UDP port; 0: the settings'
        int InputDelay = -2;        // -2: the settings'
        bool FromLauncher = false;
        bool Joining = false;
        bool GameStarted = false;
    };
    void startMatched(const MatchedGame& game);
    void joinMatched();
    void onMatchedJoined(const QString& matchID);
    // the launcher protocol's hello: this player's index, name and game identity
    void sendMatchedHello();
    // the matched game cannot be played: tell the player, and end it
    void failMatched(const QString& reason);
    void finishMatched();
    // quits melonDS (quitAfterSession), with exit status 1 if failed
    void quitAfter(const QString& reason, bool failed);
    void showChannelWidgets(bool show);

    EmuInstance* emuInstance;
    NakamaClient* client;

    // widgets
    QLabel* serverLabel;
    QPushButton* connectButton;
    QPushButton* settingsButton;
    QPushButton* directButton;
    QWidget* channelsWidget;
    QWidget* membersWidget;
    QLineEdit* channelSearch;
    QListWidget* channelList;
    QLabel* channelTitle;
    QTextBrowser* chatView;
    QLineEdit* chatInput;
    QTreeWidget* memberList;
    QCheckBox* lookingBox;
    QPushButton* challengeButton;
    QPushButton* quickButton;
    QLabel* sessionLabel;
    QPushButton* leaveButton;
    QTimer* channelRefresh;

    // the channel this window is in
    QString channelMatch;
    QString channelGame;
    QString channelTitleText;
    QString pendingChannelMatch;
    QJsonArray channels;
    QJsonArray members;
    QString channelListError;       // shown once, not at every refresh
    bool serverConnected = false;   // (connecting does not count)
    int serverPing = -1;
    int sentPing = -1;

    // quick match
    bool quickMatching = false;
    bool quickAtStart = false;      // once connected (--netplay-quick)
    bool startedOnline = false;     // --netplay-online
    QString quickIdentity;
    QString quickGame;              // the channel id of the game it is for
    QString identityKey, identityValue;     // loadedIdentity's cache

    // a matched game (quick match, or a launcher's)
    std::unique_ptr<MatchedGame> matched;
    bool launcherMatch = false;
    NakamaClient::Server launcherServer;
    QSet<QString> abandonedJoins;   // match ids and tokens of joins given up on
    bool quitting = false;

    // the session being played (or opening)
    std::shared_ptr<melonDS::RollbackNet::P2PLink> link;
    QString linkMatch;              // the match its signals go through
    int pairingSeq = -1;
    bool sessionActive = false;
    int delayOverride = -2;
    bool quitAfterSession = false;
};

#endif // NETPLAYONLINEDIALOG_H
