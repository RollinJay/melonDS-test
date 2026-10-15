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
#include <optional>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QSplitter>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "NetplayOnlineDialog.h"
#include "EmuInstance.h"
#include "EmuThread.h"
#include "Config.h"
#include "NDS.h"
#include "NDSCart.h"
#include "P2PLink.h"

using namespace melonDS;
using melonDS::RollbackNet::P2PLink;

// match data op codes of the channel module (src/net/rollback/server/melonds_channel.lua)
static constexpr qint64 OP_HELLO = 0x4D440001;
static constexpr qint64 OP_STATUS = 0x4D440002;
static constexpr qint64 OP_CHAT = 0x4D440003;
static constexpr qint64 OP_CHALLENGE = 0x4D440004;
static constexpr qint64 OP_ANSWER = 0x4D440005;
// (0x4D440007, QUEUE: the channel's own quick match, which older versions use)
static constexpr qint64 OP_DONE = 0x4D440008;
static constexpr qint64 OP_STATE = 0x4D440064;
static constexpr qint64 OP_CHAT_MSG = 0x4D440065;
static constexpr qint64 OP_CHALLENGED = 0x4D440066;
static constexpr qint64 OP_CHALLENGE_UPDATE = 0x4D440067;
static constexpr qint64 OP_PAIRING = 0x4D440068;
static constexpr qint64 OP_PAIRING_END = 0x4D440069;
static constexpr qint64 OP_ERROR = 0x4D44006A;
static constexpr qint64 OP_P2P_SIGNAL = 0x494B5347;
// a player's hello inside a match that the matchmaker made: Nakama Matchmaker's
// launcher protocol (its docs/protocol.md), {player, name, identity, kind}
static constexpr qint64 OP_MATCH_HELLO = 0x4E504845;

static NetplayOnlineDialog* currentDlg = nullptr;

// a launcher's server and account for this run (--netplay-config)
static std::optional<NetplayLauncher::ServerProfile> serverProfile;

struct NetplayOnlineDialog::Settings
{
    NakamaClient::Server Server;
    QStringList Stun;
    QString Name;
    QString Region;
    QString DeviceID;
    int LocalPort = 7064;
    int InputDelay = -1;
    int MaxPrediction = 8;
    bool KeepSave = false;
};

NetplayOnlineDialog::Settings NetplayOnlineDialog::loadSettings()
{
    Config::Table& cfg = emuInstance->getGlobalConfig();
    Settings s;
    s.Server.Host = cfg.GetQString("Netplay.Server.Host");
    s.Server.Port = cfg.GetInt("Netplay.Server.Port");
    s.Server.Key = cfg.GetQString("Netplay.Server.Key");
    s.Server.SSL = cfg.GetBool("Netplay.Server.SSL");
    for (const QString& stun : cfg.GetQString("Netplay.STUN").split(',', Qt::SkipEmptyParts))
        s.Stun << stun.trimmed();
    s.Name = cfg.GetQString("Netplay.Name");
    if (s.Name.isEmpty())
        s.Name = emuInstance->getLocalConfig().GetQString("Firmware.Username");
    s.Region = cfg.GetQString("Netplay.Region");
    s.LocalPort = cfg.GetInt("Netplay.Port");
    s.InputDelay = cfg.GetInt("Netplay.InputDelay");
    s.MaxPrediction = cfg.GetInt("Netplay.MaxPrediction");
    s.KeepSave = cfg.GetBool("Netplay.KeepSave");
    s.DeviceID = cfg.GetQString("Netplay.DeviceID");

    if (serverProfile)
    {
        // the launcher's server and account: for this run, and saved nowhere
        s.Server = serverProfile->Server;
        if (!serverProfile->Username.isEmpty())
            s.Name = serverProfile->Username;
        if (!serverProfile->Stun.isEmpty())
            s.Stun = serverProfile->Stun;
        if (!serverProfile->DeviceID.isEmpty())
            s.DeviceID = serverProfile->DeviceID;
    }
    if (launcherMatch && matched)
    {
        // a launcher's match: its server and the launcher's name
        s.Server = launcherServer;
        s.Name = matched->Names.value(matched->Self, s.Name);
        if (!matched->Stun.isEmpty())
            s.Stun = matched->Stun;
    }
    return s;
}

QString NetplayOnlineDialog::ownDeviceID()
{
    // one id per install: the server's account for this computer
    Config::Table& cfg = emuInstance->getGlobalConfig();
    QString id = cfg.GetQString("Netplay.DeviceID");
    if (id.length() < 10)
    {
        id.clear();
        for (int i = 0; i < 4; i++)
            id += QString("%1").arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));
        cfg.SetQString("Netplay.DeviceID", id);
        Config::Save();
    }
    return id;
}

void NetplayOnlineDialog::useServerProfile(const NetplayLauncher::ServerProfile& profile)
{
    serverProfile = profile;
}

NetplayOnlineDialog* NetplayOnlineDialog::openDlg(QWidget* parent, EmuInstance* inst)
{
    if (!currentDlg)
        currentDlg = new NetplayOnlineDialog(parent, inst);
    currentDlg->show();
    currentDlg->raise();
    currentDlg->activateWindow();
    return currentDlg;
}

void NetplayOnlineDialog::closeDlg(EmuInstance* inst)
{
    // deleted now, not later: Qt then drops what the emulation thread posted to it
    if (currentDlg && currentDlg->emuInstance == inst)
        delete currentDlg;
}

NetplayOnlineDialog::NetplayOnlineDialog(QWidget* parent, EmuInstance* inst)
    : QWidget(parent, Qt::Window), emuInstance(inst)
{
    setWindowTitle("Play online (rollback)");
    resize(860, 520);

    client = new NakamaClient(this);

    // top: the server
    serverLabel = new QLabel("Not connected to a server.");
    connectButton = new QPushButton("Connect");
    settingsButton = new QPushButton("Settings...");
    directButton = new QPushButton("Direct connection...");
    QHBoxLayout* top = new QHBoxLayout();
    top->addWidget(serverLabel, 1);
    top->addWidget(connectButton);
    top->addWidget(settingsButton);
    top->addWidget(directButton);

    // left: the channels
    channelSearch = new QLineEdit();
    channelSearch->setPlaceholderText("Search games");
    channelList = new QListWidget();
    QPushButton* refreshButton = new QPushButton("Refresh");
    QPushButton* ownButton = new QPushButton("Channel of my game");
    QVBoxLayout* left = new QVBoxLayout();
    left->addWidget(new QLabel("Game channels"));
    left->addWidget(channelSearch);
    left->addWidget(channelList, 1);
    QHBoxLayout* leftButtons = new QHBoxLayout();
    leftButtons->addWidget(refreshButton);
    leftButtons->addWidget(ownButton);
    left->addLayout(leftButtons);
    channelsWidget = new QWidget();
    channelsWidget->setLayout(left);

    // right: the channel
    channelTitle = new QLabel("Join a channel: double-click a game, or open your game's channel.");
    QFont titleFont = channelTitle->font();
    titleFont.setBold(true);
    channelTitle->setFont(titleFont);
    chatView = new QTextBrowser();
    chatView->setOpenLinks(false);
    chatInput = new QLineEdit();
    chatInput->setPlaceholderText("Say something");
    QPushButton* sendButton = new QPushButton("Send");
    memberList = new QTreeWidget();
    memberList->setHeaderLabels({"Player", "Status", "Region", "Ping"});
    memberList->setRootIsDecorated(false);
    memberList->header()->setStretchLastSection(false);
    memberList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    lookingBox = new QCheckBox("Looking to play");
    challengeButton = new QPushButton("Challenge");
    quickButton = new QPushButton("Quick match");

    QHBoxLayout* chatLine = new QHBoxLayout();
    chatLine->addWidget(chatInput, 1);
    chatLine->addWidget(sendButton);
    QVBoxLayout* chatSide = new QVBoxLayout();
    chatSide->addWidget(chatView, 1);
    chatSide->addLayout(chatLine);
    QVBoxLayout* memberSide = new QVBoxLayout();
    memberSide->addWidget(memberList, 1);
    memberSide->addWidget(lookingBox);
    QHBoxLayout* memberButtons = new QHBoxLayout();
    memberButtons->addWidget(challengeButton);
    memberButtons->addWidget(quickButton);
    memberSide->addLayout(memberButtons);

    QWidget* chatWidget = new QWidget();
    chatWidget->setLayout(chatSide);
    membersWidget = new QWidget();
    membersWidget->setLayout(memberSide);
    QSplitter* rightSplit = new QSplitter(Qt::Horizontal);
    rightSplit->addWidget(chatWidget);
    rightSplit->addWidget(membersWidget);
    rightSplit->setStretchFactor(0, 3);
    rightSplit->setStretchFactor(1, 2);

    QVBoxLayout* right = new QVBoxLayout();
    right->addWidget(channelTitle);
    right->addWidget(rightSplit, 1);
    QWidget* rightWidget = new QWidget();
    rightWidget->setLayout(right);

    QSplitter* split = new QSplitter(Qt::Horizontal);
    split->addWidget(channelsWidget);
    split->addWidget(rightWidget);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);

    // bottom: the session
    sessionLabel = new QLabel("No game in progress.");
    leaveButton = new QPushButton("Leave the game");
    QHBoxLayout* bottom = new QHBoxLayout();
    bottom->addWidget(sessionLabel, 1);
    bottom->addWidget(leaveButton);

    QVBoxLayout* main = new QVBoxLayout(this);
    main->addLayout(top);
    main->addWidget(split, 1);
    main->addLayout(bottom);

    channelRefresh = new QTimer(this);
    channelRefresh->setInterval(15000);

    connect(connectButton, &QPushButton::clicked, this, &NetplayOnlineDialog::onConnectClicked);
    connect(settingsButton, &QPushButton::clicked, this, &NetplayOnlineDialog::onSettingsClicked);
    connect(directButton, &QPushButton::clicked, this, &NetplayOnlineDialog::onDirectClicked);
    connect(refreshButton, &QPushButton::clicked, this, &NetplayOnlineDialog::refreshChannels);
    connect(ownButton, &QPushButton::clicked, this, &NetplayOnlineDialog::joinOwnGameChannel);
    connect(channelList, &QListWidget::itemActivated, this, &NetplayOnlineDialog::onChannelActivated);
    connect(channelSearch, &QLineEdit::textChanged, this, &NetplayOnlineDialog::showChannels);
    connect(chatInput, &QLineEdit::returnPressed, this, &NetplayOnlineDialog::onSendChat);
    connect(sendButton, &QPushButton::clicked, this, &NetplayOnlineDialog::onSendChat);
    connect(lookingBox, &QCheckBox::toggled, this, &NetplayOnlineDialog::onLookingToggled);
    connect(challengeButton, &QPushButton::clicked, this, &NetplayOnlineDialog::onChallengeClicked);
    connect(memberList, &QTreeWidget::itemDoubleClicked, this, &NetplayOnlineDialog::onChallengeClicked);
    connect(memberList, &QTreeWidget::itemSelectionChanged, this, &NetplayOnlineDialog::updateButtons);
    connect(quickButton, &QPushButton::clicked, this, &NetplayOnlineDialog::onQuickMatchClicked);
    connect(leaveButton, &QPushButton::clicked, this, &NetplayOnlineDialog::onLeaveSessionClicked);
    connect(channelRefresh, &QTimer::timeout, this, &NetplayOnlineDialog::refreshChannels);

    connect(client, &NakamaClient::connectionChanged, this, &NetplayOnlineDialog::onConnectionChanged);
    connect(client, &NakamaClient::matchJoined, this, &NetplayOnlineDialog::onMatchJoined);
    connect(client, &NakamaClient::matchJoinFailed, this, &NetplayOnlineDialog::onMatchJoinFailed);
    connect(client, &NakamaClient::matchJoinedByToken, this, &NetplayOnlineDialog::onMatchJoinedByToken);
    connect(client, &NakamaClient::matchJoinByTokenFailed, this, &NetplayOnlineDialog::onMatchJoinByTokenFailed);
    connect(client, &NakamaClient::matchData, this, &NetplayOnlineDialog::onMatchData);
    connect(client, &NakamaClient::matchPresence, this, &NetplayOnlineDialog::onMatchPresence);
    connect(client, &NakamaClient::matchmakerMatched, this, &NetplayOnlineDialog::onMatchmakerMatched);
    connect(client, &NakamaClient::matchmakerFailed, this, &NetplayOnlineDialog::onMatchmakerFailed);
    connect(client, &NakamaClient::roundTripMeasured, this, &NetplayOnlineDialog::onRoundTrip);
    connect(client, &NakamaClient::serverError, this, [this](QString msg) { appendChat(QString(), "Server: " + msg, true); });

    EmuThread* et = emuInstance->getEmuThread();
    connect(et, &EmuThread::netplayStatus, this, &NetplayOnlineDialog::onSessionStatus);
    connect(et, &EmuThread::netplayChat, this, &NetplayOnlineDialog::onSessionChat);
    connect(et, &EmuThread::netplayGameStarted, this, &NetplayOnlineDialog::onSessionGameStarted);
    connect(et, &EmuThread::netplayEnded, this, &NetplayOnlineDialog::onSessionEnded);

    updateButtons();
}

NetplayOnlineDialog::~NetplayOnlineDialog()
{
    if (currentDlg == this)
        currentDlg = nullptr;
}

// ---- server ----------------------------------------------------------------------

void NetplayOnlineDialog::onConnectClicked()
{
    if (client->isConnected() || client->isConnecting())
    {
        stopQuickMatch();
        leaveChannel();
        client->disconnectFromServer();
        return;
    }
    Settings s = loadSettings();
    if (s.Server.Host.isEmpty())
    {
        if (startedOnline && quitAfterSession)
        {
            quitAfter("There is no server to connect to: enter one in the online settings, or give a server "
                      "profile with --netplay-config.", true);
            return;
        }
        QMessageBox::information(this, "melonDS", "Enter the address of a Nakama server first (Settings...).\n\n"
                                 "Without a server, you can still play with Direct connection.");
        return;
    }
    // a profile without a device id uses this install's account
    QString deviceID = s.DeviceID.length() >= 10 ? s.DeviceID : ownDeviceID();
    serverLabel->setText("Connecting to " + s.Server.Host + "...");
    connectButton->setText("Cancel");
    client->connectToServer(s.Server, deviceID, s.Name);
}

void NetplayOnlineDialog::onConnectionChanged(bool connected, QString error)
{
    if (connected)
    {
        serverConnected = true;
        Settings s = loadSettings();
        connectButton->setText("Disconnect");
        channelListError.clear();
        if (launcherMatch)
        {
            serverLabel->setText("Connected to " + s.Server.Host + " as " + s.Name + ", for the match that Nakama Matchmaker found.");
            // the launcher's session must be the account of this computer's player
            if (matched && client->userID() != matched->UserIDs.value(matched->Self))
            {
                failMatched(QString("The launcher's session is not the account of player %1 of its match.")
                                .arg(matched->Self + 1));
                updateButtons();
                return;
            }
            if (matched && !matched->Joining && matched->JoinedID.isEmpty())
                joinMatched();
            updateButtons();
            return;
        }
        QString via = serverProfile ? " (the launcher's server profile)" : QString();
        serverLabel->setText("Connected to " + s.Server.Host + " as " + s.Name + via + ".");
        refreshChannels();
        channelRefresh->start();
        QString game, title, why;
        if (loadedGame(game, title, why))
            joinChannel(game, title);
        if (quickAtStart)
        {
            quickAtStart = false;
            QString quickWhy;
            if (!startQuickMatch(quickWhy))
            {
                appendChat(QString(), quickWhy, true);
                if (quitAfterSession)
                    quitAfter("Could not start the quick match: " + quickWhy, true);
            }
        }
    }
    else
    {
        // the server drops the matchmaker ticket with the connection
        quickMatching = false;
        QString lost = serverConnected ? QString("Disconnected from the server") : QString("Could not connect to the server");
        lost += error.isEmpty() ? QString(".") : ": " + error;
        serverConnected = false;
        // a matched game that has not started cannot go on without the server
        if (matched && !sessionActive)
            failMatched(lost);
        serverLabel->setText(error.isEmpty() ? QString("Not connected to a server.") : lost);
        connectButton->setText("Connect");
        channelRefresh->stop();
        channelMatch.clear();
        channelGame.clear();
        channelTitleText.clear();
        pendingChannelMatch.clear();
        members = QJsonArray();
        memberList->clear();
        if (!launcherMatch)
            channelTitle->setText("Not in a channel.");
        // started from the command line to play: without the server, nothing more comes
        if (startedOnline && quitAfterSession && !sessionActive && !error.isEmpty())
            quitAfter(lost, true);
    }
    updateButtons();
}

void NetplayOnlineDialog::onRoundTrip(int ms)
{
    serverPing = ms;
    // tell the channel when it changed noticeably
    if (!channelMatch.isEmpty() && (sentPing < 0 || std::abs(ms - sentPing) >= 20))
        sendStatus();
}

void NetplayOnlineDialog::onSettingsClicked()
{
    Config::Table& cfg = emuInstance->getGlobalConfig();

    QDialog dlg(this);
    dlg.setWindowTitle("Online settings");
    QFormLayout* form = new QFormLayout();

    // with a launcher's server profile, the fields it sets show the profile's
    // values, read-only: this run uses them instead of the settings
    const bool fromProfile = serverProfile.has_value();
    const bool nameFromProfile = fromProfile && !serverProfile->Username.isEmpty();
    const bool stunFromProfile = fromProfile && !serverProfile->Stun.isEmpty();
    QLineEdit* host = new QLineEdit(fromProfile ? serverProfile->Server.Host : cfg.GetQString("Netplay.Server.Host"));
    host->setPlaceholderText("nakama.example.net");
    QSpinBox* port = new QSpinBox();
    port->setRange(1, 65535);
    port->setValue(fromProfile ? serverProfile->Server.Port : cfg.GetInt("Netplay.Server.Port"));
    QLineEdit* key = new QLineEdit(fromProfile ? serverProfile->Server.Key : cfg.GetQString("Netplay.Server.Key"));
    QCheckBox* ssl = new QCheckBox("Use TLS (https/wss)");
    ssl->setChecked(fromProfile ? serverProfile->Server.SSL : cfg.GetBool("Netplay.Server.SSL"));
    QLineEdit* name = new QLineEdit(nameFromProfile ? serverProfile->Username : cfg.GetQString("Netplay.Name"));
    name->setMaxLength(24);
    name->setPlaceholderText("your firmware nickname");
    QLineEdit* region = new QLineEdit(cfg.GetQString("Netplay.Region"));
    region->setMaxLength(8);
    region->setPlaceholderText("e.g. EU, US-W");
    QLineEdit* stun = new QLineEdit(stunFromProfile ? serverProfile->Stun.join(",") : cfg.GetQString("Netplay.STUN"));
    if (fromProfile)
    {
        QList<QWidget*> fixed {host, port, key, ssl};
        if (nameFromProfile)
            fixed << name;
        if (stunFromProfile)
            fixed << stun;
        for (QWidget* w : fixed)
        {
            w->setEnabled(false);
            w->setToolTip("From the launcher's server profile, for this run.");
        }
    }
    QSpinBox* udp = new QSpinBox();
    udp->setRange(1024, 65535);
    udp->setValue(cfg.GetInt("Netplay.Port"));
    QComboBox* delay = new QComboBox();
    delay->addItem("Automatic (from the ping)", -1);
    for (int i = 0; i <= 10; i++)
        delay->addItem(QString::number(i) + (i == 1 ? " frame" : " frames"), i);
    delay->setCurrentIndex(std::max(0, delay->findData(cfg.GetInt("Netplay.InputDelay"))));
    QSpinBox* prediction = new QSpinBox();
    prediction->setRange(1, 8);
    prediction->setValue(cfg.GetInt("Netplay.MaxPrediction"));
    QCheckBox* keepSave = new QCheckBox("Keep what the game saves during online play");
    keepSave->setChecked(cfg.GetBool("Netplay.KeepSave"));
    keepSave->setToolTip("When a session ends, your save file gets the save your console ended with.\n"
                         "Off: online play never changes your save file.");

    form->addRow("Server address:", host);
    form->addRow("Server port:", port);
    form->addRow("Server key:", key);
    form->addRow("", ssl);
    form->addRow("Online name:", name);
    form->addRow("Region:", region);
    form->addRow("STUN servers:", stun);
    form->addRow("UDP port:", udp);
    form->addRow("Input delay:", delay);
    form->addRow("Rollback limit (frames):", prediction);
    form->addRow("", keepSave);

    QLabel* note = new QLabel("The host's input delay applies to both players. A lower rollback limit makes a slow "
                              "computer wait instead of falling behind. The UDP port only matters to players who "
                              "forward it, or who host direct connections.");
    note->setWordWrap(true);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    if (fromProfile)
    {
        QStringList given {"the server"};
        if (!serverProfile->DeviceID.isEmpty())
            given << "the account";
        if (nameFromProfile)
            given << "the online name";
        if (stunFromProfile)
            given << "the STUN servers";
        QString list = given.size() == 1 ? given.first()
                     : given.mid(0, given.size() - 1).join(", ") + " and " + given.last();
        list[0] = list[0].toUpper();
        QString profileName = serverProfile->Name.isEmpty() ? serverProfile->ID : serverProfile->Name;
        QLabel* fromLauncher = new QLabel(list + (given.size() == 1 ? " comes" : " come") + " from the launcher's "
                                          "server profile \"" + profileName + "\" for as long as melonDS runs; your "
                                          "own settings for them are kept for later.");
        fromLauncher->setTextFormat(Qt::PlainText);
        fromLauncher->setWordWrap(true);
        layout->addWidget(fromLauncher);
    }
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    // (what the profile gives stays out of the settings)
    if (!fromProfile)
    {
        cfg.SetQString("Netplay.Server.Host", host->text().trimmed());
        cfg.SetInt("Netplay.Server.Port", port->value());
        cfg.SetQString("Netplay.Server.Key", key->text().trimmed());
        cfg.SetBool("Netplay.Server.SSL", ssl->isChecked());
    }
    if (!nameFromProfile)
        cfg.SetQString("Netplay.Name", name->text().trimmed());
    if (!stunFromProfile)
        cfg.SetQString("Netplay.STUN", stun->text().trimmed());
    cfg.SetQString("Netplay.Region", region->text().trimmed());
    cfg.SetInt("Netplay.Port", udp->value());
    cfg.SetInt("Netplay.InputDelay", delay->currentData().toInt());
    cfg.SetInt("Netplay.MaxPrediction", prediction->value());
    cfg.SetBool("Netplay.KeepSave", keepSave->isChecked());
    Config::Save();

    if (!channelMatch.isEmpty())
        sendStatus();
}

// ---- channels ----------------------------------------------------------------------

void NetplayOnlineDialog::refreshChannels()
{
    if (!client->isConnected())
        return;
    QPointer<NetplayOnlineDialog> self(this);
    client->callRPC("melonds_channels", QJsonObject(), [self](bool ok, const QJsonObject& reply, const QString& error)
    {
        if (!self) return;
        if (!ok)
        {
            // a server without the channel module: quick match still works
            if (error != self->channelListError)
                self->appendChat(QString(), "Could not list the channels: " + error, true);
            self->channelListError = error;
            return;
        }
        self->channelListError.clear();
        self->channels = reply.value("channels").toArray();
        self->showChannels();
    });
}

void NetplayOnlineDialog::showChannels()
{
    QString filter = channelSearch->text().trimmed();
    QString selected = channelList->currentItem() ? channelList->currentItem()->data(Qt::UserRole).toString() : QString();
    channelList->clear();
    for (const QJsonValue& v : channels)
    {
        QJsonObject c = v.toObject();
        QString title = c.value("title").toString();
        if (!filter.isEmpty() && !title.contains(filter, Qt::CaseInsensitive))
            continue;
        int count = c.value("members").toInt();
        int playing = c.value("playing").toInt();
        QString text = QString("%1   (%2 online%3)").arg(title).arg(count)
                           .arg(playing ? QString(", %1 playing").arg(playing) : QString());
        QListWidgetItem* item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, c.value("game").toString());
        item->setData(Qt::UserRole + 1, title);
        channelList->addItem(item);
        if (c.value("game").toString() == selected)
            channelList->setCurrentItem(item);
    }
}

void NetplayOnlineDialog::onChannelActivated()
{
    QListWidgetItem* item = channelList->currentItem();
    if (!item) return;
    joinChannel(item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toString());
}

bool NetplayOnlineDialog::loadedGame(QString& game, QString& title, QString& why)
{
    NDS* nds = emuInstance->getNDS();
    if (!nds || !emuInstance->getEmuThread()->emuIsActive() || !nds->NDSCartSlot.GetCart() ||
        emuInstance->lastROMPath.isEmpty())
    {
        why = "Open the game you want to play first (File > Open ROM...).";
        return false;
    }
    if (nds->ConsoleType != 0)
    {
        why = "Online play runs DS games in DS mode: switch the console type to DS (Config > Emu settings).";
        return false;
    }
    auto* cart = nds->NDSCartSlot.GetCart();
    const NDSHeader& header = cart->GetHeader();

    QString code = QString::fromLatin1(header.GameCode, 4);
    bool retail = true;
    for (QChar c : code)
        retail &= c.isLetterOrNumber() && c.unicode() < 128;
    // homebrew rarely has a game code of its own: its channel goes by its binaries
    game = retail ? "NTR-" + code : QString("HB-%1").arg(cart->Checksum(), 8, 16, QChar('0')).toUpper();

    title.clear();
    if (const NDSBanner* banner = cart->Banner())
    {
        QString t = QString::fromUtf16(banner->EnglishTitle, 128);
        t.truncate(t.indexOf(QChar(0)) >= 0 ? t.indexOf(QChar(0)) : t.size());
        title = t.split('\n').value(0).trimmed();
    }
    if (title.isEmpty())
        title = QString::fromLatin1(header.GameTitle, 12).trimmed().remove(QChar(0));
    if (title.isEmpty())
        title = game;
    return true;
}

void NetplayOnlineDialog::joinOwnGameChannel()
{
    QString game, title, why;
    if (!loadedGame(game, title, why))
    {
        QMessageBox::information(this, "melonDS", why);
        return;
    }
    if (!client->isConnected())
    {
        QMessageBox::information(this, "melonDS", "Connect to a server first.");
        return;
    }
    joinChannel(game, title);
}

void NetplayOnlineDialog::joinChannel(const QString& game, const QString& title)
{
    if (!client->isConnected() || game == channelGame || launcherMatch)
        return;
    if (sessionActive || matched)
    {
        appendChat(QString(), "Finish the game in progress before changing channels.", true);
        return;
    }
    // looking for an opponent for another game: not in this channel
    if (quickMatching && game != quickGame)
    {
        stopQuickMatch();
        appendChat(QString(), "Quick match cancelled.", true);
    }
    leaveChannel();
    channelTitle->setText("Joining " + title + "...");
    QPointer<NetplayOnlineDialog> self(this);
    client->callRPC("melonds_channel_join", QJsonObject {{"game", game}, {"title", title}},
                    [self, game, title](bool ok, const QJsonObject& reply, const QString& error)
    {
        if (!self) return;
        if (!ok)
        {
            self->channelTitle->setText("Could not join " + title + ": " + error);
            return;
        }
        self->pendingChannelMatch = reply.value("match_id").toString();
        self->channelGame = game;
        self->channelTitleText = title;
        self->client->joinMatch(self->pendingChannelMatch);
    });
}

void NetplayOnlineDialog::onMatchJoined(QString matchID)
{
    if (matchID.isEmpty())
        return;
    if (matchID == pendingChannelMatch)
    {
        channelMatch = matchID;
        pendingChannelMatch.clear();
        chatView->clear();
        channelTitle->setText(channelTitleText);
        Settings s = loadSettings();
        sentPing = serverPing;
        sendChannel(OP_HELLO, QJsonObject {{"name", s.Name}, {"region", s.Region}, {"ping", serverPing}});
        if (lookingBox->isChecked() || quickMatching || matched || sessionActive)
            sendStatus();
        updateButtons();
        return;
    }
    if (matched && matched->Joining && matched->MatchToken.isEmpty() && matchID == matched->MatchID)
    {
        onMatchedJoined(matchID);
        return;
    }
    if (abandonedJoins.remove(matchID))
        client->leaveMatch(matchID);
}

void NetplayOnlineDialog::onMatchJoinFailed(QString matchID, QString error)
{
    if (!matchID.isEmpty() && matchID == pendingChannelMatch)
    {
        pendingChannelMatch.clear();
        channelGame.clear();
        channelTitle->setText("Could not join the channel: " + error);
        return;
    }
    if (matched && matched->Joining && matched->MatchToken.isEmpty() && matchID == matched->MatchID)
    {
        matched->Joining = false;
        failMatched("Could not join the match: " + (error.isEmpty() ? QString("the server refused") : error));
        return;
    }
    abandonedJoins.remove(matchID);
}

void NetplayOnlineDialog::onMatchJoinedByToken(QString matchToken, QString matchID)
{
    if (matched && matched->Joining && !matched->MatchToken.isEmpty() && matchToken == matched->MatchToken)
    {
        onMatchedJoined(matchID);
        return;
    }
    if (abandonedJoins.remove(matchToken))
        client->leaveMatch(matchID);
}

void NetplayOnlineDialog::onMatchJoinByTokenFailed(QString matchToken, QString error)
{
    if (matched && matched->Joining && !matched->MatchToken.isEmpty() && matchToken == matched->MatchToken)
    {
        matched->Joining = false;
        failMatched("Could not join the match: " + error);
        return;
    }
    abandonedJoins.remove(matchToken);
}

void NetplayOnlineDialog::leaveChannel()
{
    if (!channelMatch.isEmpty() && client->isConnected())
        client->leaveMatch(channelMatch);
    channelMatch.clear();
    channelGame.clear();
    channelTitleText.clear();
    members = QJsonArray();
    memberList->clear();
    // (a quick match goes on: Nakama's matchmaker does not need the channel)
    updateButtons();
}

void NetplayOnlineDialog::sendChannel(qint64 op, const QJsonObject& payload)
{
    if (channelMatch.isEmpty()) return;
    client->sendMatchData(channelMatch, op, QJsonDocument(payload).toJson(QJsonDocument::Compact));
}

void NetplayOnlineDialog::sendStatus()
{
    Settings s = loadSettings();
    sentPing = serverPing;
    // playing: a game the matchmaker made (the channel knows its own pairs);
    // channel modules from before "playing" ignore it
    QString status = (sessionActive || matched) ? "playing"
                   : (quickMatching || lookingBox->isChecked()) ? "looking" : "idle";
    sendChannel(OP_STATUS, QJsonObject {{"status", status}, {"ping", serverPing}, {"region", s.Region}});
}

void NetplayOnlineDialog::appendChat(const QString& name, const QString& text, bool notice)
{
    if (notice)
        chatView->append("<i>" + text.toHtmlEscaped() + "</i>");
    else
        chatView->append("<b>" + name.toHtmlEscaped() + ":</b> " + text.toHtmlEscaped());
}

void NetplayOnlineDialog::setMembers(const QJsonArray& list)
{
    members = list;
    QString selected;
    if (QTreeWidgetItem* cur = memberList->currentItem())
        selected = cur->data(0, Qt::UserRole).toString();
    memberList->clear();
    int looking = 0;
    // those looking for a game first, then those who are free, then the rest
    auto rank = [](const QString& status) {
        return status == "looking" ? 0 : status == "idle" ? 1 : status == "away" ? 2 : 3;
    };
    QList<QJsonObject> sorted;
    for (const QJsonValue& v : list)
        sorted.append(v.toObject());
    std::stable_sort(sorted.begin(), sorted.end(), [&](const QJsonObject& a, const QJsonObject& b) {
        int ra = rank(a.value("status").toString()), rb = rank(b.value("status").toString());
        if (ra != rb) return ra < rb;
        return a.value("name").toString().compare(b.value("name").toString(), Qt::CaseInsensitive) < 0;
    });
    for (const QJsonObject& m : sorted)
    {
        QString status = m.value("status").toString();
        if (status == "looking") looking++;
        int ping = m.value("ping").toInt(-1);
        QString statusText = status == "looking" ? "Looking to play" : status == "playing" ? "Playing" :
                             status == "away" ? "Away" : "";
        QTreeWidgetItem* item = new QTreeWidgetItem({m.value("name").toString(), statusText,
                                                     m.value("region").toString(),
                                                     ping >= 0 ? QString("%1 ms").arg(ping) : QString()});
        QString uid = m.value("user_id").toString();
        item->setData(0, Qt::UserRole, uid);
        if (uid == client->userID())
        {
            QFont f = item->font(0);
            f.setItalic(true);
            item->setFont(0, f);
        }
        memberList->addTopLevelItem(item);
        if (uid == selected)
            memberList->setCurrentItem(item);
    }
    channelTitle->setText(QString("%1   (%2 here, %3 looking to play)").arg(channelTitleText).arg(list.size()).arg(looking));
    updateButtons();
}

void NetplayOnlineDialog::updateButtons()
{
    bool inChannel = !channelMatch.isEmpty();
    bool busy = sessionActive || matched;
    QTreeWidgetItem* cur = memberList->currentItem();
    bool other = cur && cur->data(0, Qt::UserRole).toString() != client->userID() && cur->text(1) != "Playing";
    challengeButton->setEnabled(inChannel && other && !busy);
    // the matchmaker needs no channel: a server without the channel module has quick match too
    quickButton->setEnabled(client->isConnected() && (quickMatching || !busy));
    quickButton->setText(quickMatching ? "Cancel quick match" : "Quick match");
    lookingBox->setEnabled(inChannel && !busy);
    leaveButton->setEnabled(busy);
}

void NetplayOnlineDialog::onSendChat()
{
    QString text = chatInput->text().trimmed();
    if (text.isEmpty()) return;
    chatInput->clear();
    if (matched && sessionActive)
        emuInstance->getEmuThread()->netplaySendChat(text);     // the opponent may be in no channel
    else if (!channelMatch.isEmpty())
        sendChannel(OP_CHAT, QJsonObject {{"text", text}});
    else if (sessionActive)
        emuInstance->getEmuThread()->netplaySendChat(text);     // a direct session's chat
    else if (matched)
        appendChat(QString(), "The chat with the other player opens with the game.", true);
    else
        appendChat(QString(), "Join a channel to chat.", true);
}

void NetplayOnlineDialog::onLookingToggled(bool)
{
    sendStatus();
}

void NetplayOnlineDialog::onChallengeClicked()
{
    QTreeWidgetItem* cur = memberList->currentItem();
    if (!cur || sessionActive) return;
    QString uid = cur->data(0, Qt::UserRole).toString();
    if (uid == client->userID()) return;
    QString game, title, why;
    if (!loadedGame(game, title, why) || game != channelGame)
    {
        QMessageBox::information(this, "melonDS", "To play in this channel, open " + channelTitleText + " first.");
        return;
    }
    sendChannel(OP_CHALLENGE, QJsonObject {{"to", uid}});
}

void NetplayOnlineDialog::onQuickMatchClicked()
{
    if (quickMatching)
    {
        stopQuickMatch();
        appendChat(QString(), "Quick match cancelled.", true);
        return;
    }
    QString why;
    if (!startQuickMatch(why))
        QMessageBox::information(this, "melonDS", why);
}

// ---- quick match ---------------------------------------------------------------------
//
// Nakama's matchmaker pairs two players whose tickets ask for the same game
// identity, with exactly the properties and query of Nakama Matchmaker's
// launcher (its LauncherCore::findMatch), so that players in the launcher and
// players in this window find each other. The pair then plays as a match the
// launcher found would: see startMatched.

QString NetplayOnlineDialog::loadedIdentity(QString& error)
{
    const QStringList files = emuInstance->lastROMPath;
    if (files.isEmpty())
    {
        error = "no game is loaded";
        return QString();
    }
    QFileInfo info(files.first());
    QString key = files.join('\n') + '\n' + QString::number(info.size()) + '\n' +
                  QString::number(info.lastModified().toMSecsSinceEpoch());
    if (key == identityKey && !identityValue.isEmpty())
        return identityValue;
    QString identity = NetplayLauncher::IdentityOfFiles(files, error);
    if (!identity.isEmpty())
    {
        identityKey = key;
        identityValue = identity;
    }
    return identity;
}

bool NetplayOnlineDialog::startQuickMatch(QString& why)
{
    if (!client->isConnected())
    {
        why = "Connect to a server first.";
        return false;
    }
    if (sessionActive || matched)
    {
        why = "A game is already in progress.";
        return false;
    }
    QString game, title;
    if (!loadedGame(game, title, why))
        return false;
    if (!channelMatch.isEmpty() && game != channelGame)
    {
        why = "To play in this channel, open " + channelTitleText + " first.";
        return false;
    }
    QString error;
    QString identity = loadedIdentity(error);
    if (identity.isEmpty())
    {
        why = "Cannot look for an opponent: " + error + ".";
        return false;
    }

    QJsonObject properties {
        {"game", identity},
        {"kind", "melonds"},
        {"mode", "unranked"},
        {"players", "2"},
    };
    QString query = QString("+properties.game:%1 +properties.kind:melonds +properties.mode:unranked +properties.players:2")
                        .arg(identity);
    client->addMatchmaker(query, 2, 2, properties);
    quickMatching = true;
    quickIdentity = identity;
    quickGame = game;
    appendChat(QString(), "Looking for an opponent for " + title + "...", true);
    sendStatus();
    updateButtons();
    return true;
}

void NetplayOnlineDialog::stopQuickMatch()
{
    if (!quickMatching)
        return;
    client->removeMatchmaker();
    quickMatching = false;
    sendStatus();
    updateButtons();
}

void NetplayOnlineDialog::onMatchmakerFailed(QString error)
{
    if (!quickMatching)
        return;
    quickMatching = false;
    QString why = "The quick match failed: " + error;
    appendChat(QString(), why, true);
    sendStatus();
    updateButtons();
    if (startedOnline && quitAfterSession && !sessionActive)
        quitAfter(why, true);
}

void NetplayOnlineDialog::onMatchmakerMatched(NakamaMatched m)
{
    if (!quickMatching)
        return;     // cancelled meanwhile
    quickMatching = false;
    if (sessionActive || matched || link)
    {
        appendChat(QString(), "The matchmaker found an opponent, but a game is already in progress.", true);
        sendStatus();
        updateButtons();
        return;
    }
    if (m.UserIDs.size() != 2 || m.Self < 0 || (m.MatchID.isEmpty() && m.Token.isEmpty()))
    {
        QString why = "The matchmaker's answer is not a two-player match.";
        appendChat(QString(), why, true);
        sendStatus();
        updateButtons();
        if (startedOnline && quitAfterSession)
            quitAfter(why, true);
        return;
    }

    MatchedGame g;
    g.MatchID = m.MatchID;
    g.MatchToken = m.MatchID.isEmpty() ? m.Token : QString();
    g.UserIDs = m.UserIDs;
    g.Names = m.Names;
    g.Self = m.Self;
    g.Names[g.Self] = loadSettings().Name;
    // the game loaded now (another one since the ticket fails the hello at once,
    // on both sides, rather than when the consoles compare their ROMs)
    QString error;
    g.Identity = loadedIdentity(error);
    if (g.Identity.isEmpty())
        g.Identity = quickIdentity;
    startMatched(g);
}

// ---- matched games ---------------------------------------------------------------------
//
// A match that Nakama's matchmaker made, here or in a launcher, is played as
// Nakama Matchmaker's docs/launch.md describes: both players join the match
// (by id, or by token for a match that Nakama relays), send their hello with
// their game's identity, and exchange the P2P signals of the direct
// connection through the match. The matchmaker's first player hosts.

void NetplayOnlineDialog::startMatched(const MatchedGame& game)
{
    matched = std::make_unique<MatchedGame>(game);
    QString peerName = matched->Names.value(1 - matched->Self);
    appendChat(QString(), "Found an opponent: " + peerName + ". Joining the match...", true);
    sessionLabel->setText("Joining the match with " + peerName + "...");
    sendStatus();
    updateButtons();
    if (client->isConnected())
        joinMatched();
}

void NetplayOnlineDialog::joinMatched()
{
    matched->Joining = true;
    if (!matched->MatchID.isEmpty())
        client->joinMatch(matched->MatchID);
    else
        client->joinMatchByToken(matched->MatchToken);
}

void NetplayOnlineDialog::onMatchedJoined(const QString& matchID)
{
    MatchedGame& m = *matched;
    m.Joining = false;
    m.JoinedID = matchID;
    const int peer = 1 - m.Self;
    printf("netplay: joined the match %s (by %s) as player %d, against %s\n", matchID.toUtf8().constData(),
           m.MatchToken.isEmpty() ? "id" : "token", m.Self + 1, m.Names[peer].toUtf8().constData());
    fflush(stdout);

    sendMatchedHello();

    Settings s = loadSettings();
    P2PLink::Options opt;
    opt.LocalPort = (u16)(m.Port > 0 ? m.Port : s.LocalPort);
    for (const QString& stun : (m.Stun.isEmpty() ? s.Stun : m.Stun))
        opt.StunServers.push_back(stun.toStdString());
    opt.ConnectTimeoutMs = 20000;

    // the link's thread hands signals over to this thread, which owns the server connection
    QPointer<NetplayOnlineDialog> self(this);
    auto sendSignal = [self, matchID](const std::string& json)
    {
        QByteArray data = QByteArray::fromStdString(json);
        QMetaObject::invokeMethod(qApp, [self, matchID, data]
        {
            if (self && self->client->isConnected())
                self->client->sendMatchData(matchID, OP_P2P_SIGNAL, data);
        }, Qt::QueuedConnection);
    };

    auto newLink = std::make_shared<P2PLink>();
    std::string err;
    if (!newLink->StartSignaled(opt, matchID.toStdString(), m.UserIDs[m.Self].toStdString(),
                                m.UserIDs[peer].toStdString(), sendSignal, err))
    {
        failMatched("Could not start the connection: " + QString::fromStdString(err));
        return;
    }
    link = newLink;
    linkMatch = matchID;
    pairingSeq = -1;
    delayOverride = m.InputDelay;
    appendChat(QString(), "Playing against " + m.Names[peer] + (m.Self == 0 ? " (you host)." : "."), true);
    startSession(newLink, m.Self == 0);
}

void NetplayOnlineDialog::sendMatchedHello()
{
    const MatchedGame& m = *matched;
    client->sendMatchData(m.JoinedID, OP_MATCH_HELLO, QJsonDocument(QJsonObject {
        {"player", m.Self},
        {"name", m.Names[m.Self]},
        {"identity", m.Identity},
        {"kind", "melonds"},
    }).toJson(QJsonDocument::Compact));
}

void NetplayOnlineDialog::failMatched(const QString& reason)
{
    if (!matched)
        return;
    if (sessionActive)
    {
        // onSessionEnded finishes the match
        emuInstance->getEmuThread()->netplayLeave(reason, true);
        return;
    }
    appendChat(QString(), reason, true);
    sessionLabel->setText("No game in progress. " + reason);
    finishMatched();
    if (quitAfterSession)
        quitAfter(reason, true);
}

void NetplayOnlineDialog::finishMatched()
{
    if (!matched)
        return;
    if (client->isConnected())
    {
        if (!matched->JoinedID.isEmpty())
            client->leaveMatch(matched->JoinedID);
        else if (matched->Joining)
            abandonedJoins.insert(matched->MatchToken.isEmpty() ? matched->MatchID : matched->MatchToken);
    }
    matched.reset();
    sendStatus();
    updateButtons();
}

void NetplayOnlineDialog::quitAfter(const QString& reason, bool failed)
{
    if (quitting)
        return;
    quitting = true;
    fprintf(failed ? stderr : stdout, "netplay: %s\n", reason.toUtf8().constData());
    fflush(failed ? stderr : stdout);
    // a failure stays on screen a moment
    QTimer::singleShot(failed ? 3000 : 500, qApp, [failed] { qApp->exit(failed ? 1 : 0); });
}

void NetplayOnlineDialog::showChannelWidgets(bool show)
{
    channelsWidget->setVisible(show);
    membersWidget->setVisible(show);
    connectButton->setVisible(show);
    settingsButton->setVisible(show);
    directButton->setVisible(show);
}

void NetplayOnlineDialog::onMatchPresence(QString matchID, QStringList joined, QStringList left)
{
    if (!matched || matchID != matched->JoinedID || matched->GameStarted)
        return;
    const int peer = 1 - matched->Self;
    if (left.contains(matched->UserIDs[peer]))
        failMatched(matched->Names[peer] + " left the match before the game started.");
    else if (joined.contains(matched->UserIDs[peer]))
        sendMatchedHello();     // a match does not keep what was sent before someone joined
}

void NetplayOnlineDialog::onMatchData(QString matchID, QString senderID, qint64 op, QByteArray data)
{
    if (op == OP_P2P_SIGNAL)
    {
        if (link && matchID == linkMatch)
            link->HandleSignal(data.toStdString());
        return;
    }
    if (op == OP_MATCH_HELLO)
    {
        if (!matched || matchID != matched->JoinedID)
            return;
        const int peer = 1 - matched->Self;
        if (senderID != matched->UserIDs[peer])
            return;
        QJsonObject hello = QJsonDocument::fromJson(data).object();
        QString name = hello.value("name").toString().trimmed().left(32);
        if (!name.isEmpty())
            matched->Names[peer] = name;
        QString kind = hello.value("kind").toString();
        if (hello.value("identity").toString() != matched->Identity || (!kind.isEmpty() && kind != "melonds"))
            failMatched("The other player's game differs from yours (another ROM, or a melonDS that speaks another "
                        "online protocol): you cannot play each other.");
        return;
    }
    if (matchID != channelMatch)
        return;

    QJsonObject msg = QJsonDocument::fromJson(data).object();
    switch (op)
    {
    case OP_STATE:
        setMembers(msg.value("members").toArray());
        break;

    case OP_CHAT_MSG:
        if (msg.contains("notice"))
            appendChat(QString(), msg.value("notice").toString(), true);
        else
            appendChat(msg.value("name").toString(), msg.value("text").toString(), false);
        break;

    case OP_CHALLENGED:
        {
            QString from = msg.value("from").toString();
            QString name = msg.value("name").toString();
            if (sessionActive || matched)
            {
                // the channel does not know about games the matchmaker made
                sendChannel(OP_ANSWER, QJsonObject {{"from", from}, {"accept", false}});
                appendChat(QString(), name + " challenged you during your game (declined).", true);
                break;
            }
            appendChat(QString(), name + " challenges you.", true);
            QMessageBox* box = new QMessageBox(QMessageBox::Question, "melonDS",
                                               name + " wants to play " + channelTitleText + " with you.",
                                               QMessageBox::NoButton, this);
            QPushButton* accept = box->addButton("Accept", QMessageBox::AcceptRole);
            box->addButton("Decline", QMessageBox::RejectRole);
            box->setAttribute(Qt::WA_DeleteOnClose);
            QPointer<NetplayOnlineDialog> self(this);
            QString match = channelMatch;
            connect(box, &QMessageBox::finished, this, [self, box, accept, from, match](int)
            {
                if (!self || self->channelMatch != match) return;
                bool yes = box->clickedButton() == accept;
                if (yes)
                {
                    QString game, title, why;
                    if (!self->loadedGame(game, title, why) || game != self->channelGame)
                    {
                        self->appendChat(QString(), "Open " + self->channelTitleText + " first to play it.", true);
                        yes = false;
                    }
                }
                self->sendChannel(OP_ANSWER, QJsonObject {{"from", from}, {"accept", yes}});
            });
            box->show();
        }
        break;

    case OP_CHALLENGE_UPDATE:
        {
            QString name = msg.value("name").toString();
            QString state = msg.value("state").toString();
            if (state == "sent") appendChat(QString(), "Challenge sent to " + name + ".", true);
            else if (state == "declined") appendChat(QString(), name + " declined.", true);
            else if (state == "cancelled") appendChat(QString(), "The challenge with " + name + " was withdrawn.", true);
            else if (state == "expired") appendChat(QString(), "The challenge with " + name + " expired.", true);
            else if (state == "busy") appendChat(QString(), name + " is busy.", true);
        }
        break;

    case OP_PAIRING:
        startPairing(msg);
        break;

    case OP_PAIRING_END:
        // the other player gave up on (or finished) our pairing
        if (sessionActive && msg.value("seq").toInt() == pairingSeq)
        {
            pairingSeq = -1;
            emuInstance->getEmuThread()->netplayLeave();
        }
        break;

    case OP_ERROR:
        appendChat(QString(), msg.value("text").toString(), true);
        break;

    default:
        break;
    }
}

// ---- sessions ------------------------------------------------------------------------

void NetplayOnlineDialog::startPairing(const QJsonObject& pairing)
{
    int seq = pairing.value("seq").toInt();
    QString host = pairing.value("host").toString();
    QString guest = pairing.value("guest").toString();
    QString me = client->userID();
    if (host != me && guest != me)
        return;
    bool isHost = host == me;
    QString peer = isHost ? guest : host;
    QString peerName = pairing.value(isHost ? "guest_name" : "host_name").toString();

    // the channel's pairing takes over from a quick match
    stopQuickMatch();
    QString game, title, why;
    bool busy = sessionActive || link || matched;
    if (busy || !loadedGame(game, title, why) || game != channelGame)
    {
        appendChat(QString(), busy ? "Already in a game." : "Open " + channelTitleText + " to play it.", true);
        sendChannel(OP_DONE, QJsonObject {{"seq", seq}, {"reason", "unavailable"}});
        updateButtons();
        return;
    }

    Settings s = loadSettings();
    P2PLink::Options opt;
    opt.LocalPort = (u16)s.LocalPort;
    for (const QString& stun : s.Stun)
        opt.StunServers.push_back(stun.toStdString());
    opt.ConnectTimeoutMs = 20000;

    // the link's thread hands signals over to this thread, which owns the server connection
    QPointer<NetplayOnlineDialog> self(this);
    QString match = channelMatch;
    auto sendSignal = [self, match](const std::string& json)
    {
        QByteArray data = QByteArray::fromStdString(json);
        QMetaObject::invokeMethod(qApp, [self, match, data]
        {
            if (self && self->client->isConnected())
                self->client->sendMatchData(match, OP_P2P_SIGNAL, data);
        }, Qt::QueuedConnection);
    };

    auto newLink = std::make_shared<P2PLink>();
    std::string err;
    if (!newLink->StartSignaled(opt, match.toStdString(), me.toStdString(), peer.toStdString(), sendSignal, err))
    {
        appendChat(QString(), "Could not start the connection: " + QString::fromStdString(err), true);
        sendChannel(OP_DONE, QJsonObject {{"seq", seq}, {"reason", "p2p"}});
        return;
    }
    link = newLink;
    linkMatch = match;
    pairingSeq = seq;
    appendChat(QString(), "Playing against " + peerName + (isHost ? " (you host)." : "."), true);
    startSession(newLink, isHost);
}

void NetplayOnlineDialog::startSession(std::shared_ptr<P2PLink> newLink, bool host)
{
    // a game starting ends the search for another
    stopQuickMatch();
    Settings s = loadSettings();
    auto req = std::make_shared<NetplayStartRequest>();
    req->Link = newLink;
    req->Host = host;
    req->InputDelay = delayOverride >= -1 ? delayOverride : s.InputDelay;
    delayOverride = -2;
    req->MaxPrediction = s.MaxPrediction;
    req->KeepSave = s.KeepSave;
    req->PlayerName = s.Name.toStdString();

    sessionActive = true;
    sessionLabel->setText("Connecting to the other player...");
    updateButtons();
    emuInstance->getEmuThread()->netplayStart(req);
}

void NetplayOnlineDialog::onLeaveSessionClicked()
{
    if (sessionActive)
    {
        emuInstance->getEmuThread()->netplayLeave();
        return;
    }
    if (matched)
    {
        // still joining the match: the other player sees this one leave it
        QString reason = "You left the match.";
        appendChat(QString(), reason, true);
        sessionLabel->setText("No game in progress. " + reason);
        finishMatched();
        if (quitAfterSession)
            quitAfter(reason, false);
    }
}

void NetplayOnlineDialog::onSessionStatus(QString text)
{
    sessionLabel->setText(text);
}

void NetplayOnlineDialog::onSessionChat(QString name, QString text, bool notice)
{
    // a channel's pair chats in the channel; a direct session and a matched
    // game (whose opponent may be in no channel) here
    if (channelMatch.isEmpty() || notice || matched)
        appendChat(name, text, notice);
}

void NetplayOnlineDialog::onSessionGameStarted()
{
    sessionLabel->setText("Playing.");
    if (matched)
        matched->GameStarted = true;
}

void NetplayOnlineDialog::onSessionEnded(QString reason, bool failed)
{
    appendChat(QString(), reason, true);
    sessionLabel->setText("No game in progress. " + reason);
    if (pairingSeq >= 0)
        sendChannel(OP_DONE, QJsonObject {{"seq", pairingSeq}, {"reason", reason.left(80)}});
    pairingSeq = -1;
    link.reset();
    linkMatch.clear();
    sessionActive = false;
    finishMatched();
    if (!channelMatch.isEmpty())
        sendStatus();
    updateButtons();

    if (quitAfterSession)
        quitAfter(reason, failed);
}

void NetplayOnlineDialog::onDirectClicked()
{
    if (sessionActive || matched)
    {
        QMessageBox::information(this, "melonDS", "A game is already in progress.");
        return;
    }
    QString game, title, why;
    if (!loadedGame(game, title, why))
    {
        QMessageBox::information(this, "melonDS", why);
        return;
    }

    Config::Table& cfg = emuInstance->getGlobalConfig();
    QDialog dlg(this);
    dlg.setWindowTitle("Direct connection");
    QRadioButton* hostRadio = new QRadioButton("Host: wait for the other player on my UDP port");
    QRadioButton* joinRadio = new QRadioButton("Join the host at this address:");
    hostRadio->setChecked(true);
    QSpinBox* port = new QSpinBox();
    port->setRange(1024, 65535);
    port->setValue(cfg.GetInt("Netplay.Port"));
    QLineEdit* address = new QLineEdit(cfg.GetQString("Netplay.LastHost"));
    address->setPlaceholderText("host:port, e.g. 203.0.113.5:7064");
    QLineEdit* code = new QLineEdit(cfg.GetQString("Netplay.DirectCode"));
    code->setPlaceholderText("any word both players enter");
    QLabel* note = new QLabel("Both players need the same game (" + title + "). The host's UDP port must be reachable:\n"
                              "forwarded on its router, or both on the same network. Through a server, no port\n"
                              "needs forwarding.");
    note->setWordWrap(true);

    QFormLayout* form = new QFormLayout();
    form->addRow(hostRadio);
    form->addRow("UDP port:", port);
    form->addRow(joinRadio);
    form->addRow("Host address:", address);
    form->addRow("Session code:", code);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    bool host = hostRadio->isChecked();
    cfg.SetInt("Netplay.Port", port->value());
    cfg.SetQString("Netplay.LastHost", address->text().trimmed());
    cfg.SetQString("Netplay.DirectCode", code->text().trimmed());
    Config::Save();

    startDirect(host, port->value(), address->text().trimmed(), code->text().trimmed(), -2, false);
}

void NetplayOnlineDialog::connectAtStart(bool quick, bool quitAfterEnd)
{
    startedOnline = true;
    quickAtStart = quick;
    quitAfterSession = quitAfterEnd;
    if (!client->isConnected() && !client->isConnecting())
        onConnectClicked();
}

void NetplayOnlineDialog::startFromLauncher(const NetplayLauncher::ConnectionFile& file)
{
    launcherMatch = true;
    quitAfterSession = true;
    launcherServer = file.Server;

    MatchedGame g;
    g.MatchID = file.MatchID;
    g.MatchToken = file.MatchID.isEmpty() ? file.MatchToken : QString();
    g.UserIDs = file.UserIDs;
    g.Names = file.Names;
    g.Self = file.Self;
    g.Identity = file.Identity;
    g.Stun = file.Stun;
    g.Port = file.Port;
    g.InputDelay = file.InputDelay >= 0 ? file.InputDelay : -2;
    g.FromLauncher = true;

    // no channels: only this match
    showChannelWidgets(false);
    channelTitle->setText("The match that Nakama Matchmaker found: " + g.Names[0] + " vs " + g.Names[1]);
    serverLabel->setText("Connecting to " + file.Server.Host + " for the match...");
    matched = std::make_unique<MatchedGame>(g);
    sessionLabel->setText("Joining the match with " + g.Names[1 - g.Self] + "...");
    updateButtons();
    // the launcher's account: the same user the matchmaker matched
    client->useSession(file.Server, file.Token, file.RefreshToken);
}

bool NetplayOnlineDialog::startDirect(bool host, int port, const QString& address, const QString& code,
                                      int delay, bool quitAfter)
{
    if (sessionActive || matched)
        return false;
    QString sessionCode = code.isEmpty() ? emuInstance->getGlobalConfig().GetQString("Netplay.DirectCode") : code;

    P2PLink::Options opt;
    opt.LocalPort = (u16)port;
    auto newLink = std::make_shared<P2PLink>();
    std::string err;
    bool ok;
    if (host && (port < 1 || port > 65535))
    {
        ok = false;
        err = "Give the UDP port to host on (1 to 65535).";
    }
    else if (host)
    {
        opt.ConnectTimeoutMs = 0;
        ok = newLink->StartHost(opt, sessionCode.toStdString(), err);
    }
    else
    {
        opt.LocalPort = 0;
        opt.ConnectTimeoutMs = 20000;
        ok = newLink->StartGuest(opt, address.toStdString(), sessionCode.toStdString(), err);
    }
    if (!ok)
    {
        appendChat(QString(), QString::fromStdString(err), true);
        if (quitAfter)
        {
            quitAfterSession = true;
            this->quitAfter(QString::fromStdString(err), true);   // (the parameter hides the method)
        }
        else
            QMessageBox::warning(this, "melonDS", QString::fromStdString(err));
        return false;
    }
    link = newLink;
    linkMatch.clear();
    pairingSeq = -1;
    delayOverride = delay;
    quitAfterSession = quitAfter;
    appendChat(QString(), host ? QString("Waiting for the other player on UDP port %1...").arg(port)
                               : "Connecting to " + address + "...", true);
    startSession(newLink, host);
    return true;
}
