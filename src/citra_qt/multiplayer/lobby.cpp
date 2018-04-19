// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QInputDialog>
#include <QList>
#include <QtConcurrent/QtConcurrentRun>
#include "citra_qt/game_list_p.h"
#include "citra_qt/main.h"
#include "citra_qt/multiplayer/client_room.h"
#include "citra_qt/multiplayer/lobby.h"
#include "citra_qt/multiplayer/lobby_p.h"
#include "citra_qt/multiplayer/message.h"
#include "citra_qt/multiplayer/state.h"
#include "citra_qt/multiplayer/validation.h"
#include "citra_qt/ui_settings.h"
#include "common/logging/log.h"
#include "core/settings.h"
#include "network/network.h"

Lobby::Lobby(QWidget* parent, QStandardItemModel* list,
             std::shared_ptr<Core::AnnounceMultiplayerSession> session)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowSystemMenuHint),
      ui(std::make_unique<Ui::Lobby>()), announce_multiplayer_session(session), game_list(list) {
    ui->setupUi(this);

    // setup the watcher for background connections
    watcher = new QFutureWatcher<void>;
    connect(watcher, &QFutureWatcher<void>::finished, this, &Lobby::OnConnection);

    model = new QStandardItemModel(ui->room_list);
    proxy = new LobbyFilterProxyModel(this, game_list);
    proxy->setSourceModel(model);
    proxy->setDynamicSortFilter(true);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy->setSortLocaleAware(true);
    ui->room_list->setModel(proxy);
    ui->room_list->header()->setSectionResizeMode(QHeaderView::Interactive);
    ui->room_list->header()->stretchLastSection();
    ui->room_list->setAlternatingRowColors(true);
    ui->room_list->setSelectionMode(QHeaderView::SingleSelection);
    ui->room_list->setSelectionBehavior(QHeaderView::SelectRows);
    ui->room_list->setVerticalScrollMode(QHeaderView::ScrollPerPixel);
    ui->room_list->setHorizontalScrollMode(QHeaderView::ScrollPerPixel);
    ui->room_list->setSortingEnabled(true);
    ui->room_list->setEditTriggers(QHeaderView::NoEditTriggers);
    ui->room_list->setExpandsOnDoubleClick(false);
    ui->room_list->setContextMenuPolicy(Qt::CustomContextMenu);

    ui->nickname->setValidator(validation.GetNickname());
    ui->nickname->setText(UISettings::values.nickname);

    // UI Buttons
    MultiplayerState* p = reinterpret_cast<MultiplayerState*>(parent);
    connect(ui->refresh_list, &QPushButton::pressed, this, &Lobby::RefreshLobby);
    connect(ui->games_owned, &QCheckBox::stateChanged, proxy,
            &LobbyFilterProxyModel::SetFilterOwned);
    connect(ui->hide_full, &QCheckBox::stateChanged, proxy, &LobbyFilterProxyModel::SetFilterFull);
    connect(ui->search, &QLineEdit::textChanged, proxy, &LobbyFilterProxyModel::SetFilterSearch);
    connect(ui->room_list, &QTreeView::doubleClicked, this, &Lobby::OnJoinRoom);
    connect(ui->room_list, &QTreeView::clicked, this, &Lobby::OnExpandRoom);

    // Actions
    connect(this, &Lobby::LobbyRefreshed, this, &Lobby::OnRefreshLobby);
    // TODO(jroweboy): change this slot to OnConnected?
    connect(this, &Lobby::Connected, p, &MultiplayerState::OnOpenNetworkRoom);

    // manually start a refresh when the window is opening
    // TODO(jroweboy): if this refresh is slow for people with bad internet, then don't do it as
    // part of the constructor, but offload the refresh until after the window shown. perhaps emit a
    // refreshroomlist signal from places that open the lobby
    RefreshLobby();
}

QString Lobby::PasswordPrompt() {
    bool ok;
    const QString text =
        QInputDialog::getText(this, tr("Password Required to Join"), tr("Password:"),
                              QLineEdit::Normal, tr("Password"), &ok);
    return ok ? text : QString();
}

void Lobby::OnExpandRoom(const QModelIndex& index) {
    QModelIndex member_index = proxy->index(index.row(), Column::MEMBER);
    auto member_list = proxy->data(member_index, LobbyItemMemberList::MemberListRole).toList();
}

void Lobby::OnJoinRoom(const QModelIndex& source) {
    QModelIndex index = source;
    // If the user double clicks on a child row (aka the player list) then use the parent instead
    if (source.parent() != QModelIndex()) {
        index = source.parent();
    }
    if (!ui->nickname->hasAcceptableInput()) {
        NetworkMessage::ShowError(NetworkMessage::USERNAME_NOT_VALID);
        return;
    }
    if (const auto member = Network::GetRoomMember().lock()) {
        if (member->IsConnected()) {
            if (!NetworkMessage::WarnDisconnect()) {
                return;
            }
        }
    }

    // Get a password to pass if the room is password protected
    QModelIndex password_index = proxy->index(index.row(), Column::ROOM_NAME);
    bool has_password = proxy->data(password_index, LobbyItemName::PasswordRole).toBool();
    const std::string password = has_password ? PasswordPrompt().toStdString() : "";
    if (has_password && password.empty()) {
        return;
    }

    // attempt to connect in a different thread
    QFuture<void> f = QtConcurrent::run([&, password] {
        if (auto room_member = Network::GetRoomMember().lock()) {
            QModelIndex connection_index = proxy->index(index.row(), Column::HOST);
            const std::string nickname = ui->nickname->text().toStdString();
            const std::string ip =
                proxy->data(connection_index, LobbyItemHost::HostIPRole).toString().toStdString();
            int port = proxy->data(connection_index, LobbyItemHost::HostPortRole).toInt();
            room_member->Join(nickname, ip.c_str(), port, 0, Network::NoPreferredMac, password);
        }
    });
    watcher->setFuture(f);
    // and disable widgets and display a connecting while we wait
    QModelIndex connection_index = proxy->index(index.row(), Column::HOST);

    // Save settings
    UISettings::values.nickname = ui->nickname->text();
    UISettings::values.ip = proxy->data(connection_index, LobbyItemHost::HostIPRole).toString();
    UISettings::values.port = proxy->data(connection_index, LobbyItemHost::HostPortRole).toString();
    Settings::Apply();
}

void Lobby::ResetModel() {
    model->clear();
    model->insertColumns(0, Column::TOTAL);
    model->setHeaderData(Column::EXPAND, Qt::Horizontal, "", Qt::DisplayRole);
    model->setHeaderData(Column::ROOM_NAME, Qt::Horizontal, tr("Room Name"), Qt::DisplayRole);
    model->setHeaderData(Column::GAME_NAME, Qt::Horizontal, tr("Preferred Game"), Qt::DisplayRole);
    model->setHeaderData(Column::HOST, Qt::Horizontal, tr("Host"), Qt::DisplayRole);
    model->setHeaderData(Column::MEMBER, Qt::Horizontal, tr("Players"), Qt::DisplayRole);
}

void Lobby::RefreshLobby() {
    if (auto session = announce_multiplayer_session.lock()) {
        ResetModel();
        room_list_future = session->GetRoomList([&]() { emit LobbyRefreshed(); });
        ui->refresh_list->setEnabled(false);
        ui->refresh_list->setText(tr("Refreshing"));
    } else {
        // TODO(jroweboy): Display an error box about announce couldn't be started
    }
}

void Lobby::OnRefreshLobby() {
    AnnounceMultiplayerRoom::RoomList new_room_list = room_list_future.get();
    for (auto room : new_room_list) {
        // find the icon for the game if this person owns that game.
        QPixmap smdh_icon;
        for (int r = 0; r < game_list->rowCount(); ++r) {
            auto index = game_list->index(r, 0);
            auto game_id = game_list->data(index, GameListItemPath::ProgramIdRole).toULongLong();
            if (game_id != 0 && room.preferred_game_id == game_id) {
                smdh_icon = game_list->data(index, Qt::DecorationRole).value<QPixmap>();
            }
        }

        QList<QVariant> members;
        for (auto member : room.members) {
            QVariant var;
            var.setValue(LobbyMember{QString::fromStdString(member.name), member.game_id,
                                     QString::fromStdString(member.game_name)});
            members.append(var);
        }

        auto first_item = new LobbyItem();
        auto row = QList<QStandardItem*>({
            first_item,
            new LobbyItemName(room.has_password, QString::fromStdString(room.name)),
            new LobbyItemGame(room.preferred_game_id, QString::fromStdString(room.preferred_game),
                              smdh_icon),
            new LobbyItemHost(QString::fromStdString(room.owner), QString::fromStdString(room.ip),
                              room.port),
            new LobbyItemMemberList(members, room.max_player),
        });
        model->appendRow(row);
        // To make the rows expandable, add the member data as a child of the first column of the
        // rows with people in them and have qt set them to colspan after the model is finished
        // resetting
        if (room.members.size() > 0) {
            first_item->appendRow(new LobbyItemExpandedMemberList(members));
        }
    }

    // Reenable the refresh button and resize the columns
    ui->refresh_list->setEnabled(true);
    ui->refresh_list->setText(tr("Refresh List"));
    ui->room_list->header()->stretchLastSection();
    for (int i = 0; i < Column::TOTAL - 1; ++i) {
        ui->room_list->resizeColumnToContents(i);
    }

    // Set the member list child items to span all columns
    for (int i = 0; i < proxy->rowCount(); i++) {
        auto parent = model->item(i, 0);
        if (parent->hasChildren()) {
            ui->room_list->setFirstColumnSpanned(0, proxy->index(i, 0), true);
        }
    }
}

LobbyFilterProxyModel::LobbyFilterProxyModel(QWidget* parent, QStandardItemModel* list)
    : QSortFilterProxyModel(parent), game_list(list) {}

bool LobbyFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    // Prioritize filters by fastest to compute

    // pass over any child rows (aka row that shows the players in the room)
    if (sourceParent != QModelIndex()) {
        return true;
    }

    // filter by filled rooms
    if (filter_full) {
        QModelIndex member_list = sourceModel()->index(sourceRow, Column::MEMBER, sourceParent);
        int player_count =
            sourceModel()->data(member_list, LobbyItemMemberList::MemberListRole).toList().size();
        int max_players =
            sourceModel()->data(member_list, LobbyItemMemberList::MaxPlayerRole).toInt();
        if (player_count >= max_players) {
            return false;
        }
    }

    // filter by search parameters
    if (!filter_search.isEmpty()) {
        QModelIndex game_name = sourceModel()->index(sourceRow, Column::GAME_NAME, sourceParent);
        QModelIndex room_name = sourceModel()->index(sourceRow, Column::ROOM_NAME, sourceParent);
        QModelIndex host_name = sourceModel()->index(sourceRow, Column::HOST, sourceParent);
        bool preferred_game_match = sourceModel()
                                        ->data(game_name, LobbyItemGame::GameNameRole)
                                        .toString()
                                        .contains(filter_search, filterCaseSensitivity());
        bool room_name_match = sourceModel()
                                   ->data(room_name, LobbyItemName::NameRole)
                                   .toString()
                                   .contains(filter_search, filterCaseSensitivity());
        bool username_match = sourceModel()
                                  ->data(host_name, LobbyItemHost::HostUsernameRole)
                                  .toString()
                                  .contains(filter_search, filterCaseSensitivity());
        if (!preferred_game_match && !room_name_match && !username_match) {
            return false;
        }
    }

    // filter by game owned
    if (filter_owned) {
        QModelIndex game_name = sourceModel()->index(sourceRow, Column::GAME_NAME, sourceParent);
        QList<QModelIndex> owned_games;
        for (int r = 0; r < game_list->rowCount(); ++r) {
            owned_games.append(QModelIndex(game_list->index(r, 0)));
        }
        auto current_id = sourceModel()->data(game_name, LobbyItemGame::TitleIDRole).toLongLong();
        if (current_id == 0) {
            // TODO(jroweboy): homebrew often doesn't have a game id and this hides them
            return false;
        }
        bool owned = false;
        for (const auto& game : owned_games) {
            auto game_id = game_list->data(game, GameListItemPath::ProgramIdRole).toLongLong();
            if (current_id == game_id) {
                owned = true;
            }
        }
        if (!owned) {
            return false;
        }
    }

    return true;
}

void LobbyFilterProxyModel::sort(int column, Qt::SortOrder order) {
    sourceModel()->sort(column, order);
}

void LobbyFilterProxyModel::SetFilterOwned(bool filter) {
    filter_owned = filter;
    invalidate();
}

void LobbyFilterProxyModel::SetFilterFull(bool filter) {
    filter_full = filter;
    invalidate();
}

void LobbyFilterProxyModel::SetFilterSearch(const QString& filter) {
    filter_search = filter;
    invalidate();
}

void Lobby::OnConnection() {
    if (auto room_member = Network::GetRoomMember().lock()) {
        switch (room_member->GetState()) {
        case Network::RoomMember::State::CouldNotConnect:
            ShowError(NetworkMessage::UNABLE_TO_CONNECT);
            break;
        case Network::RoomMember::State::NameCollision:
            ShowError(NetworkMessage::USERNAME_IN_USE);
            break;
        case Network::RoomMember::State::Error:
            ShowError(NetworkMessage::UNABLE_TO_CONNECT);
            break;
        case Network::RoomMember::State::Joining:
            auto parent = static_cast<MultiplayerState*>(parentWidget());
            parent->OnOpenNetworkRoom();
            close();
            break;
        }
    }
}
