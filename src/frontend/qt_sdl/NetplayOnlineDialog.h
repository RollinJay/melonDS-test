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
#include <QWidget>

#include "NakamaClient.h"

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
// who is looking for a game, their chat, challenges and quick matches.
// When two players are paired, their computers connect directly and play
// a rollback session (melonDS::RollbackNet::OnlineSession, run by the
// emulation thread). Direct connections without a server are here too.
class NetplayOnlineDialog : public QWidget
{
    Q_OBJECT

public:
    // the one window (created on first use)
    static NetplayOnlineDialog* openDlg(QWidget* parent, EmuInstance* inst);

    explicit NetplayOnlineDialog(QWidget* parent, EmuInstance* inst);
    ~NetplayOnlineDialog() override;

    // A direct session without a server: the host waits on its UDP port, the
    // guest connects to address ("host:port"). delay: input delay in frames,
    // -1 from the round trip, -2 the settings'. quitAfter: quit melonDS when it ends.
    bool startDirect(bool host, int port, const QString& address, const QString& code, int delay, bool quitAfter);

private slots:
    void onConnectClicked();
    void onSettingsClicked();
    void onDirectClicked();
    void onConnectionChanged(bool connected, QString error);
    void onMatchJoined(QString matchID);
    void onMatchJoinFailed(QString matchID, QString error);
    void onMatchData(QString matchID, QString senderID, qint64 op, QByteArray data);
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
    void onSessionEnded(QString reason);

private:
    struct Settings;
    Settings loadSettings();

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

    EmuInstance* emuInstance;
    NakamaClient* client;

    // widgets
    QLabel* serverLabel;
    QPushButton* connectButton;
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
    bool quickMatching = false;
    int serverPing = -1;
    int sentPing = -1;

    // the session being played (or opening)
    std::shared_ptr<melonDS::RollbackNet::P2PLink> link;
    QString linkMatch;              // the match its signals go through
    int pairingSeq = -1;
    bool sessionActive = false;
    int delayOverride = -2;
    bool quitAfterSession = false;
};

#endif // NETPLAYONLINEDIALOG_H
