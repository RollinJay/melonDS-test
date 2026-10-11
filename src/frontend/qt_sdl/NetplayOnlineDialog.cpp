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

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
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
static constexpr qint64 OP_QUEUE = 0x4D440007;
static constexpr qint64 OP_DONE = 0x4D440008;
static constexpr qint64 OP_STATE = 0x4D440064;
static constexpr qint64 OP_CHAT_MSG = 0x4D440065;
static constexpr qint64 OP_CHALLENGED = 0x4D440066;
static constexpr qint64 OP_CHALLENGE_UPDATE = 0x4D440067;
static constexpr qint64 OP_PAIRING = 0x4D440068;
static constexpr qint64 OP_PAIRING_END = 0x4D440069;
static constexpr qint64 OP_ERROR = 0x4D44006A;
static constexpr qint64 OP_P2P_SIGNAL = 0x494B5347;

static NetplayOnlineDialog* currentDlg = nullptr;

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

    // one id per install: the server's account for this computer
    s.DeviceID = cfg.GetQString("Netplay.DeviceID");
    if (s.DeviceID.length() < 10)
    {
        QString id;
        for (int i = 0; i < 4; i++)
            id += QString("%1").arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));
        s.DeviceID = id;
        cfg.SetQString("Netplay.DeviceID", id);
        Config::Save();
    }
    return s;
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

NetplayOnlineDialog::NetplayOnlineDialog(QWidget* parent, EmuInstance* inst)
    : QWidget(parent, Qt::Window), emuInstance(inst)
{
    setWindowTitle("Play online (rollback)");
    resize(860, 520);

    client = new NakamaClient(this);

    // top: the server
    serverLabel = new QLabel("Not connected to a server.");
    connectButton = new QPushButton("Connect");
    QPushButton* settingsButton = new QPushButton("Settings...");
    QPushButton* directButton = new QPushButton("Direct connection...");
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
    QWidget* leftWidget = new QWidget();
    leftWidget->setLayout(left);

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
    QWidget* memberWidget = new QWidget();
    memberWidget->setLayout(memberSide);
    QSplitter* rightSplit = new QSplitter(Qt::Horizontal);
    rightSplit->addWidget(chatWidget);
    rightSplit->addWidget(memberWidget);
    rightSplit->setStretchFactor(0, 3);
    rightSplit->setStretchFactor(1, 2);

    QVBoxLayout* right = new QVBoxLayout();
    right->addWidget(channelTitle);
    right->addWidget(rightSplit, 1);
    QWidget* rightWidget = new QWidget();
    rightWidget->setLayout(right);

    QSplitter* split = new QSplitter(Qt::Horizontal);
    split->addWidget(leftWidget);
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
    connect(client, &NakamaClient::matchData, this, &NetplayOnlineDialog::onMatchData);
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
        leaveChannel();
        client->disconnectFromServer();
        return;
    }
    Settings s = loadSettings();
    if (s.Server.Host.isEmpty())
    {
        QMessageBox::information(this, "melonDS", "Enter the address of a Nakama server first (Settings...).\n\n"
                                 "Without a server, you can still play with Direct connection.");
        return;
    }
    serverLabel->setText("Connecting to " + s.Server.Host + "...");
    connectButton->setText("Cancel");
    client->connectToServer(s.Server, s.DeviceID, s.Name);
}

void NetplayOnlineDialog::onConnectionChanged(bool connected, QString error)
{
    if (connected)
    {
        serverLabel->setText("Connected to " + loadSettings().Server.Host + " as " + loadSettings().Name + ".");
        connectButton->setText("Disconnect");
        refreshChannels();
        channelRefresh->start();
        QString game, title, why;
        if (loadedGame(game, title, why))
            joinChannel(game, title);
    }
    else
    {
        serverLabel->setText(error.isEmpty() ? QString("Not connected to a server.") : "Disconnected: " + error);
        connectButton->setText("Connect");
        channelRefresh->stop();
        channelMatch.clear();
        channelGame.clear();
        channelTitleText.clear();
        pendingChannelMatch.clear();
        members = QJsonArray();
        memberList->clear();
        channelTitle->setText("Not in a channel.");
        quickMatching = false;
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

    QLineEdit* host = new QLineEdit(cfg.GetQString("Netplay.Server.Host"));
    host->setPlaceholderText("nakama.example.net");
    QSpinBox* port = new QSpinBox();
    port->setRange(1, 65535);
    port->setValue(cfg.GetInt("Netplay.Server.Port"));
    QLineEdit* key = new QLineEdit(cfg.GetQString("Netplay.Server.Key"));
    QCheckBox* ssl = new QCheckBox("Use TLS (https/wss)");
    ssl->setChecked(cfg.GetBool("Netplay.Server.SSL"));
    QLineEdit* name = new QLineEdit(cfg.GetQString("Netplay.Name"));
    name->setMaxLength(24);
    name->setPlaceholderText("your firmware nickname");
    QLineEdit* region = new QLineEdit(cfg.GetQString("Netplay.Region"));
    region->setMaxLength(8);
    region->setPlaceholderText("e.g. EU, US-W");
    QLineEdit* stun = new QLineEdit(cfg.GetQString("Netplay.STUN"));
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

    QLabel* note = new QLabel("The host's input delay applies to both players. A lower rollback limit makes a slow\n"
                              "computer wait instead of falling behind. The UDP port only matters to players who\n"
                              "forward it, or who host direct connections.");
    note->setWordWrap(true);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    cfg.SetQString("Netplay.Server.Host", host->text().trimmed());
    cfg.SetInt("Netplay.Server.Port", port->value());
    cfg.SetQString("Netplay.Server.Key", key->text().trimmed());
    cfg.SetBool("Netplay.Server.SSL", ssl->isChecked());
    cfg.SetQString("Netplay.Name", name->text().trimmed());
    cfg.SetQString("Netplay.Region", region->text().trimmed());
    cfg.SetQString("Netplay.STUN", stun->text().trimmed());
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
            self->appendChat(QString(), "Could not list the channels: " + error, true);
            return;
        }
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
    if (!client->isConnected() || game == channelGame)
        return;
    if (sessionActive)
    {
        appendChat(QString(), "Finish the game in progress before changing channels.", true);
        return;
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
    if (matchID != pendingChannelMatch)
        return;
    channelMatch = matchID;
    pendingChannelMatch.clear();
    chatView->clear();
    channelTitle->setText(channelTitleText);
    Settings s = loadSettings();
    sentPing = serverPing;
    sendChannel(OP_HELLO, QJsonObject {{"name", s.Name}, {"region", s.Region}, {"ping", serverPing}});
    if (lookingBox->isChecked())
        sendStatus();
    updateButtons();
}

void NetplayOnlineDialog::onMatchJoinFailed(QString matchID, QString error)
{
    if (matchID != pendingChannelMatch)
        return;
    pendingChannelMatch.clear();
    channelGame.clear();
    channelTitle->setText("Could not join the channel: " + error);
}

void NetplayOnlineDialog::leaveChannel()
{
    if (!channelMatch.isEmpty())
        client->leaveMatch(channelMatch);
    channelMatch.clear();
    channelGame.clear();
    channelTitleText.clear();
    members = QJsonArray();
    memberList->clear();
    quickMatching = false;
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
    QString status = quickMatching || lookingBox->isChecked() ? "looking" : "idle";
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
    QTreeWidgetItem* cur = memberList->currentItem();
    bool other = cur && cur->data(0, Qt::UserRole).toString() != client->userID() && cur->text(1) != "Playing";
    challengeButton->setEnabled(inChannel && other && !sessionActive);
    quickButton->setEnabled(inChannel && !sessionActive);
    quickButton->setText(quickMatching ? "Stop searching" : "Quick match");
    lookingBox->setEnabled(inChannel && !sessionActive);
    leaveButton->setEnabled(sessionActive);
}

void NetplayOnlineDialog::onSendChat()
{
    QString text = chatInput->text().trimmed();
    if (text.isEmpty()) return;
    chatInput->clear();
    if (!channelMatch.isEmpty())
        sendChannel(OP_CHAT, QJsonObject {{"text", text}});
    else if (sessionActive)
        emuInstance->getEmuThread()->netplaySendChat(text);     // a direct session's chat
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
    if (!quickMatching)
    {
        QString game, title, why;
        if (!loadedGame(game, title, why) || game != channelGame)
        {
            QMessageBox::information(this, "melonDS", "To play in this channel, open " + channelTitleText + " first.");
            return;
        }
    }
    quickMatching = !quickMatching;
    sendChannel(OP_QUEUE, QJsonObject {{"on", quickMatching}});
    if (quickMatching)
        appendChat(QString(), "Looking for an opponent...", true);
    updateButtons();
}

void NetplayOnlineDialog::onMatchData(QString matchID, QString senderID, qint64 op, QByteArray data)
{
    if (op == OP_P2P_SIGNAL)
    {
        if (link && matchID == linkMatch)
            link->HandleSignal(data.toStdString());
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

    quickMatching = false;
    QString game, title, why;
    if (sessionActive || link || !loadedGame(game, title, why) || game != channelGame)
    {
        appendChat(QString(), sessionActive ? "Already in a game." : "Open " + channelTitleText + " to play it.", true);
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
        emuInstance->getEmuThread()->netplayLeave();
}

void NetplayOnlineDialog::onSessionStatus(QString text)
{
    sessionLabel->setText(text);
}

void NetplayOnlineDialog::onSessionChat(QString name, QString text, bool notice)
{
    // a channel's players see the channel's chat; a direct session's here
    if (channelMatch.isEmpty() || notice)
        appendChat(name, text, notice);
}

void NetplayOnlineDialog::onSessionGameStarted()
{
    sessionLabel->setText("Playing.");
}

void NetplayOnlineDialog::onSessionEnded(QString reason)
{
    appendChat(QString(), reason, true);
    sessionLabel->setText("No game in progress. " + reason);
    if (pairingSeq >= 0)
        sendChannel(OP_DONE, QJsonObject {{"seq", pairingSeq}, {"reason", reason.left(80)}});
    pairingSeq = -1;
    link.reset();
    linkMatch.clear();
    sessionActive = false;
    if (!channelMatch.isEmpty())
        sendStatus();
    updateButtons();

    if (quitAfterSession)
    {
        printf("netplay: %s\n", reason.toUtf8().constData());
        QTimer::singleShot(500, qApp, &QApplication::quit);
    }
}

void NetplayOnlineDialog::onDirectClicked()
{
    if (sessionActive)
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

bool NetplayOnlineDialog::startDirect(bool host, int port, const QString& address, const QString& code,
                                      int delay, bool quitAfter)
{
    if (sessionActive)
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
            QTimer::singleShot(0, qApp, &QApplication::quit);
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
