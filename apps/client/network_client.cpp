#include "network_client.hpp"

#include <QDateTime>
#include <QVariantMap>

#include <algorithm>
#include <exception>
#include <utility>

namespace poker::client {

NetworkClient::NetworkClient(QObject* parent) : QObject(parent) {
    reconnect_timer_.setSingleShot(true);
    heartbeat_timer_.setInterval(10'000);
    connect(&socket_, &QTcpSocket::connected, this, &NetworkClient::onConnected);
    connect(&socket_, &QTcpSocket::disconnected, this, &NetworkClient::onDisconnected);
    connect(&socket_, &QTcpSocket::readyRead, this, &NetworkClient::onReadyRead);
    connect(&socket_, &QTcpSocket::errorOccurred, this, &NetworkClient::onSocketError);
    connect(&reconnect_timer_, &QTimer::timeout, this, [this] {
        if (!routing_to_game_ || game_host_.isEmpty() || game_port_ == 0) {
            return;
        }
        codec_.reset();
        authenticate_on_connect_ = true;
        setStatus(QStringLiteral("Reconnecting to game node…"));
        socket_.connectToHost(game_host_, game_port_);
    });
    connect(&heartbeat_timer_, &QTimer::timeout, this, [this] {
        if (!connected()) {
            return;
        }
        auto heartbeat = envelope(protocol::v1::HEARTBEAT, false);
        heartbeat.mutable_heartbeat()->set_client_time_unix_ms(
            QDateTime::currentMSecsSinceEpoch());
        send(std::move(heartbeat));
    });
}

bool NetworkClient::connected() const noexcept {
    return socket_.state() == QAbstractSocket::ConnectedState;
}

bool NetworkClient::authenticated() const noexcept { return authenticated_; }
QString NetworkClient::status() const { return status_; }
qulonglong NetworkClient::userId() const noexcept { return user_id_; }
qint64 NetworkClient::walletChips() const noexcept { return wallet_chips_; }
QVariantList NetworkClient::tables() const { return tables_; }
QVariantList NetworkClient::players() const { return players_; }
QVariantList NetworkClient::board() const { return board_; }
qulonglong NetworkClient::tableId() const noexcept { return table_id_; }
qulonglong NetworkClient::handId() const noexcept { return hand_id_; }
qulonglong NetworkClient::actingUserId() const noexcept { return acting_user_id_; }
QString NetworkClient::street() const { return street_; }
qint64 NetworkClient::pot() const noexcept { return pot_; }
qint64 NetworkClient::currentBet() const noexcept { return current_bet_; }
qint64 NetworkClient::minimumRaise() const noexcept { return minimum_raise_; }

void NetworkClient::connectToLobby(const QString& host, quint16 port) {
    reconnect_timer_.stop();
    reconnect_attempt_ = 0;
    reconnect_after_auth_ = false;
    lobby_host_ = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    lobby_port_ = port;
    if (routing_to_game_) {
        game_sequence_ = client_sequence_;
    }
    routing_to_game_ = false;
    client_sequence_ = lobby_sequence_;
    authenticate_on_connect_ = !session_token_.isEmpty();
    authenticated_ = false;
    emit authenticatedChanged();
    codec_.reset();
    intentional_disconnect_ = socket_.state() != QAbstractSocket::UnconnectedState;
    socket_.abort();
    setStatus(QStringLiteral("Connecting to lobby…"));
    socket_.connectToHost(lobby_host_, lobby_port_);
}

void NetworkClient::registerAccount(const QString& username, const QString& password) {
    auto request = envelope(protocol::v1::REGISTER_REQUEST, false);
    request.mutable_register_request()->set_username(username.toStdString());
    request.mutable_register_request()->set_password(password.toStdString());
    send(std::move(request));
}

void NetworkClient::login(const QString& username, const QString& password) {
    auto request = envelope(protocol::v1::LOGIN_REQUEST, false);
    request.mutable_login_request()->set_username(username.toStdString());
    request.mutable_login_request()->set_password(password.toStdString());
    send(std::move(request));
}

void NetworkClient::logout() {
    auto request = envelope(protocol::v1::LOGOUT_REQUEST);
    request.mutable_logout_request();
    send(std::move(request));
}

void NetworkClient::refreshTables() {
    auto request = envelope(protocol::v1::LIST_TABLES_REQUEST);
    request.mutable_list_tables_request()->set_page_size(100);
    send(std::move(request));
}

void NetworkClient::createTable(const QString& name,
                                int maxPlayers,
                                qint64 smallBlind,
                                qint64 bigBlind,
                                qint64 minimumBuyIn,
                                qint64 maximumBuyIn) {
    auto request = envelope(protocol::v1::CREATE_TABLE_REQUEST);
    auto* body = request.mutable_create_table_request();
    body->set_name(name.toStdString());
    body->set_max_players(static_cast<std::uint32_t>(std::clamp(maxPlayers, 2, 6)));
    body->set_small_blind(smallBlind);
    body->set_big_blind(bigBlind);
    body->set_min_buy_in(minimumBuyIn);
    body->set_max_buy_in(maximumBuyIn);
    send(std::move(request));
}

void NetworkClient::joinTable(qulonglong tableId) {
    table_id_ = tableId;
    emit tableChanged();
    auto request = envelope(protocol::v1::JOIN_TABLE_REQUEST);
    request.mutable_join_table_request()->set_table_id(tableId);
    send(std::move(request));
}

void NetworkClient::sitDown(int seat, qint64 buyIn) {
    auto request = envelope(protocol::v1::SIT_DOWN_REQUEST);
    auto* body = request.mutable_sit_down_request();
    body->set_table_id(table_id_);
    body->set_seat(static_cast<std::uint32_t>(std::max(0, seat)));
    body->set_buy_in(buyIn);
    body->set_join_ticket(pending_ticket_.toStdString());
    send(std::move(request));
}

void NetworkClient::setReady(bool ready) {
    auto request = envelope(protocol::v1::READY_REQUEST);
    request.mutable_ready_request()->set_table_id(table_id_);
    request.mutable_ready_request()->set_ready(ready);
    send(std::move(request));
}

void NetworkClient::act(const QString& action, qint64 targetCommitment) {
    auto request = envelope(protocol::v1::ACTION_REQUEST);
    auto* body = request.mutable_action_request();
    body->set_table_id(table_id_);
    body->set_hand_id(hand_id_);
    body->set_target_street_commitment(targetCommitment);
    const auto normalized = action.trimmed().toLower();
    if (normalized == "fold") body->set_action(protocol::v1::FOLD);
    else if (normalized == "check") body->set_action(protocol::v1::CHECK);
    else if (normalized == "call") body->set_action(protocol::v1::CALL);
    else if (normalized == "bet") body->set_action(protocol::v1::BET);
    else if (normalized == "raise") body->set_action(protocol::v1::RAISE);
    else if (normalized == "all_in") body->set_action(protocol::v1::ALL_IN);
    else {
        setStatus(QStringLiteral("Unknown action"));
        return;
    }
    send(std::move(request));
}

void NetworkClient::leaveTable() {
    auto request = envelope(protocol::v1::LEAVE_TABLE_REQUEST);
    request.mutable_leave_table_request()->set_table_id(table_id_);
    send(std::move(request));
}

NetworkClient::Envelope NetworkClient::envelope(protocol::v1::MessageType type,
                                                 bool sequenced) {
    Envelope result;
    result.set_protocol_version(1);
    result.set_request_id(next_request_id_++);
    result.set_message_type(type);
    if (sequenced) {
        result.set_client_sequence(++client_sequence_);
        if (routing_to_game_) {
            game_sequence_ = client_sequence_;
        } else {
            lobby_sequence_ = client_sequence_;
        }
    }
    return result;
}

void NetworkClient::send(Envelope message) {
    if (!connected()) {
        setStatus(QStringLiteral("Not connected"));
        return;
    }
    std::string payload;
    if (!message.SerializeToString(&payload)) {
        setStatus(QStringLiteral("Could not encode request"));
        return;
    }
    try {
        const auto frame = net::LengthFieldCodec::encode(payload);
        socket_.write(reinterpret_cast<const char*>(frame.data()),
                      static_cast<qint64>(frame.size()));
    } catch (const std::exception& error) {
        setStatus(QString::fromUtf8(error.what()));
    }
}

void NetworkClient::onConnected() {
    reconnect_timer_.stop();
    heartbeat_timer_.start();
    emit connectedChanged();
    setStatus(routing_to_game_ ? QStringLiteral("Connected to game node")
                               : QStringLiteral("Connected to lobby"));
    if (authenticate_on_connect_ && !session_token_.isEmpty()) {
        auto request = envelope(protocol::v1::AUTHENTICATE_SESSION_REQUEST, false);
        request.mutable_authenticate_session_request()->set_session_token(
            session_token_.toStdString());
        send(std::move(request));
    }
}

void NetworkClient::onDisconnected() {
    heartbeat_timer_.stop();
    authenticated_ = false;
    emit connectedChanged();
    emit authenticatedChanged();
    if (intentional_disconnect_) {
        intentional_disconnect_ = false;
        return;
    }
    if (routing_to_game_ && !session_token_.isEmpty() && table_id_ != 0) {
        reconnect_after_auth_ = seated_;
        scheduleReconnect();
        return;
    }
    setStatus(QStringLiteral("Disconnected"));
}

void NetworkClient::onReadyRead() {
    const auto bytes = socket_.readAll();
    const auto decoded = codec_.feed(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                     static_cast<std::size_t>(bytes.size()));
    if (!decoded) {
        setStatus(QStringLiteral("Protocol framing error"));
        socket_.abort();
        return;
    }
    for (const auto& frame : decoded.frames) {
        Envelope message;
        if (!message.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
            setStatus(QStringLiteral("Invalid server message"));
            continue;
        }
        process(message);
    }
}

void NetworkClient::onSocketError(QAbstractSocket::SocketError error) {
    static_cast<void>(error);
    setStatus(socket_.errorString());
    if (!intentional_disconnect_ && socket_.state() == QAbstractSocket::UnconnectedState
        && routing_to_game_ && !session_token_.isEmpty() && table_id_ != 0) {
        scheduleReconnect();
    }
}

void NetworkClient::process(const Envelope& message) {
    switch (message.message_type()) {
    case protocol::v1::REGISTER_RESPONSE:
        setStatus(QStringLiteral("Account created. Sign in now."));
        return;
    case protocol::v1::LOGIN_RESPONSE: {
        const auto& body = message.login_response();
        user_id_ = body.user_id();
        wallet_chips_ = body.wallet_chips();
        session_token_ = QString::fromStdString(body.session_token());
        client_sequence_ = 0;
        lobby_sequence_ = 0;
        authenticated_ = true;
        emit authenticatedChanged();
        emit identityChanged();
        setStatus(QStringLiteral("Signed in"));
        refreshTables();
        return;
    }
    case protocol::v1::AUTHENTICATE_SESSION_RESPONSE: {
        const auto& body = message.authenticate_session_response();
        user_id_ = body.user_id();
        wallet_chips_ = body.wallet_chips();
        authenticated_ = true;
        authenticate_on_connect_ = false;
        reconnect_attempt_ = 0;
        client_sequence_ = 0;
        if (routing_to_game_) {
            game_sequence_ = 0;
        } else {
            lobby_sequence_ = 0;
        }
        emit authenticatedChanged();
        emit identityChanged();
        setStatus(routing_to_game_ ? QStringLiteral("Game node authenticated")
                                   : QStringLiteral("Lobby session restored"));
        if (routing_to_game_ && reconnect_after_auth_) {
            auto reconnect = envelope(protocol::v1::RECONNECT_REQUEST);
            auto* reconnect_body = reconnect.mutable_reconnect_request();
            reconnect_body->set_table_id(table_id_);
            reconnect_body->set_session_token(session_token_.toStdString());
            reconnect_body->set_last_server_sequence(server_sequence_);
            reconnect_after_auth_ = false;
            send(std::move(reconnect));
        } else if (!routing_to_game_) {
            refreshTables();
        }
        return;
    }
    case protocol::v1::REFRESH_SESSION_RESPONSE:
        session_token_ = QString::fromStdString(
            message.refresh_session_response().session_token());
        setStatus(QStringLiteral("Session refreshed"));
        return;
    case protocol::v1::LIST_TABLES_RESPONSE: {
        tables_.clear();
        for (const auto& table : message.list_tables_response().tables()) {
            QVariantMap item;
            item["tableId"] = QVariant::fromValue<qulonglong>(table.table_id());
            item["name"] = QString::fromStdString(table.name());
            item["players"] = table.seated_players();
            item["maxPlayers"] = table.max_players();
            item["blinds"] = QStringLiteral("%1 / %2")
                                 .arg(table.small_blind()).arg(table.big_blind());
            item["node"] = QString::fromStdString(table.game_node_id());
            tables_.push_back(item);
        }
        emit tablesChanged();
        setStatus(QStringLiteral("Lobby refreshed"));
        return;
    }
    case protocol::v1::CREATE_TABLE_RESPONSE: {
        const auto& body = message.create_table_response();
        routeToGame(QString::fromStdString(body.node_endpoint()),
                    QString::fromStdString(body.join_ticket()),
                    body.table().table_id());
        return;
    }
    case protocol::v1::JOIN_TABLE_RESPONSE: {
        const auto& body = message.join_table_response();
        routeToGame(QString::fromStdString(body.node_endpoint()),
                    QString::fromStdString(body.join_ticket()), table_id_);
        return;
    }
    case protocol::v1::TABLE_SNAPSHOT:
        processSnapshot(message.table_snapshot());
        return;
    case protocol::v1::COMMAND_ACK:
        setStatus(QString::fromStdString(message.command_ack().message()));
        if (message.command_ack().message() == "logged out") {
            const bool was_on_game_node = routing_to_game_;
            reconnect_timer_.stop();
            session_token_.clear();
            authenticate_on_connect_ = false;
            reconnect_after_auth_ = false;
            authenticated_ = false;
            seated_ = false;
            table_id_ = 0;
            hand_id_ = 0;
            server_sequence_ = 0;
            acting_user_id_ = 0;
            user_id_ = 0;
            wallet_chips_ = 0;
            client_sequence_ = 0;
            lobby_sequence_ = 0;
            game_sequence_ = 0;
            players_.clear();
            board_.clear();
            emit authenticatedChanged();
            emit identityChanged();
            emit tableChanged();
            if (was_on_game_node) {
                connectToLobby(lobby_host_, lobby_port_);
            }
        } else if (message.command_ack().message() == "left table") {
            seated_ = false;
            table_id_ = 0;
            hand_id_ = 0;
            players_.clear();
            board_.clear();
            emit tableChanged();
            connectToLobby(lobby_host_, lobby_port_);
        }
        return;
    case protocol::v1::ERROR_RESPONSE: {
        const auto& error = message.error_response();
        const auto error_text = QString::fromStdString(error.message());
        if (error.code() == protocol::v1::UNAUTHENTICATED) {
            const bool was_on_game_node = routing_to_game_;
            reconnect_timer_.stop();
            heartbeat_timer_.stop();
            session_token_.clear();
            authenticate_on_connect_ = false;
            reconnect_after_auth_ = false;
            authenticated_ = false;
            seated_ = false;
            table_id_ = 0;
            hand_id_ = 0;
            server_sequence_ = 0;
            acting_user_id_ = 0;
            user_id_ = 0;
            wallet_chips_ = 0;
            client_sequence_ = 0;
            lobby_sequence_ = 0;
            game_sequence_ = 0;
            players_.clear();
            board_.clear();
            emit authenticatedChanged();
            emit identityChanged();
            emit tableChanged();
            if (was_on_game_node) {
                connectToLobby(lobby_host_, lobby_port_);
            }
        }
        setStatus(QStringLiteral("Error: %1").arg(error_text));
        return;
    }
    case protocol::v1::TABLE_EVENT: {
        const auto& event = message.table_event();
        setStatus(QString::fromStdString(event.event_payload()));
        if (event.event_type() == "session_replaced") {
            reconnect_timer_.stop();
            heartbeat_timer_.stop();
            session_token_.clear();
            routing_to_game_ = false;
            authenticate_on_connect_ = false;
            reconnect_after_auth_ = false;
            authenticated_ = false;
            seated_ = false;
            table_id_ = 0;
            hand_id_ = 0;
            server_sequence_ = 0;
            acting_user_id_ = 0;
            user_id_ = 0;
            wallet_chips_ = 0;
            client_sequence_ = 0;
            lobby_sequence_ = 0;
            game_sequence_ = 0;
            players_.clear();
            board_.clear();
            emit authenticatedChanged();
            emit identityChanged();
            emit tableChanged();
            intentional_disconnect_ = socket_.state() != QAbstractSocket::UnconnectedState;
            socket_.disconnectFromHost();
        } else if (event.event_type() == "table_aborted_refunded"
            || event.event_type() == "table_aborted_refund_pending") {
            seated_ = false;
            table_id_ = 0;
            hand_id_ = 0;
            acting_user_id_ = 0;
            players_.clear();
            board_.clear();
            emit tableChanged();
            connectToLobby(lobby_host_, lobby_port_);
        }
        return;
    }
    case protocol::v1::HEARTBEAT:
        return;
    default:
        setStatus(QStringLiteral("Unsupported server response"));
        return;
    }
}

void NetworkClient::processSnapshot(const protocol::v1::TableSnapshot& snapshot) {
    table_id_ = snapshot.table_id();
    hand_id_ = snapshot.hand_id();
    server_sequence_ = snapshot.server_sequence();
    acting_user_id_ = snapshot.acting_user_id();
    street_ = streetText(snapshot.street());
    pot_ = snapshot.pot();
    current_bet_ = snapshot.current_bet();
    minimum_raise_ = snapshot.minimum_raise();
    board_.clear();
    for (const auto& card : snapshot.board()) {
        board_.push_back(cardText(card));
    }
    players_.clear();
    seated_ = false;
    for (const auto& player : snapshot.players()) {
        QVariantMap item;
        item["userId"] = QVariant::fromValue<qulonglong>(player.user_id());
        item["seat"] = player.seat();
        item["stack"] = QVariant::fromValue<qint64>(player.stack());
        item["commitment"] = QVariant::fromValue<qint64>(player.street_commitment());
        item["status"] = QString::fromStdString(player.status());
        item["ready"] = player.ready();
        item["connected"] = player.connected();
        QStringList cards;
        for (const auto& card : player.visible_hole_cards()) {
            cards.push_back(cardText(card));
        }
        item["cards"] = cards.join(' ');
        players_.push_back(item);
        if (player.user_id() == user_id_) {
            seated_ = true;
        }
    }
    emit tableChanged();
    setStatus(acting_user_id_ == user_id_ ? QStringLiteral("Your turn")
                                          : QStringLiteral("Table synchronized"));
}

void NetworkClient::routeToGame(const QString& endpoint,
                                QString ticket,
                                qulonglong tableId) {
    const auto separator = endpoint.lastIndexOf(':');
    if (separator <= 0) {
        setStatus(QStringLiteral("Invalid game-node endpoint"));
        return;
    }
    bool port_ok = false;
    const auto port = endpoint.mid(separator + 1).toUShort(&port_ok);
    if (!port_ok) {
        setStatus(QStringLiteral("Invalid game-node port"));
        return;
    }
    lobby_sequence_ = client_sequence_;
    client_sequence_ = game_sequence_;
    table_id_ = tableId;
    pending_ticket_ = std::move(ticket);
    game_host_ = endpoint.left(separator);
    game_port_ = port;
    routing_to_game_ = true;
    reconnect_timer_.stop();
    reconnect_attempt_ = 0;
    reconnect_after_auth_ = false;
    authenticate_on_connect_ = true;
    authenticated_ = false;
    emit authenticatedChanged();
    emit tableChanged();
    codec_.reset();
    intentional_disconnect_ = socket_.state() != QAbstractSocket::UnconnectedState;
    socket_.abort();
    setStatus(QStringLiteral("Routing to assigned game node…"));
    socket_.connectToHost(game_host_, game_port_);
}

void NetworkClient::scheduleReconnect() {
    if (reconnect_timer_.isActive()) {
        return;
    }
    const auto shift = std::min(reconnect_attempt_, 4);
    const auto delay_ms = std::min(10'000, 500 * (1 << shift));
    ++reconnect_attempt_;
    setStatus(QStringLiteral("Connection lost; retrying in %1 ms").arg(delay_ms));
    reconnect_timer_.start(delay_ms);
}

void NetworkClient::setStatus(QString value) {
    if (status_ == value) {
        return;
    }
    status_ = std::move(value);
    emit statusChanged();
}

QString NetworkClient::cardText(const protocol::v1::Card& card) {
    static const QStringList ranks{"?", "?", "2", "3", "4", "5", "6", "7",
                                   "8", "9", "T", "J", "Q", "K", "A"};
    static const QStringList suits{"♣", "♦", "♥", "♠"};
    const auto rank = card.rank() < static_cast<std::uint32_t>(ranks.size())
                          ? ranks[static_cast<int>(card.rank())]
                          : QStringLiteral("?");
    const auto suit = card.suit() < static_cast<std::uint32_t>(suits.size())
                          ? suits[static_cast<int>(card.suit())]
                          : QStringLiteral("?");
    return rank + suit;
}

QString NetworkClient::streetText(protocol::v1::Street value) {
    switch (value) {
    case protocol::v1::WAITING: return QStringLiteral("Waiting");
    case protocol::v1::PREFLOP: return QStringLiteral("Preflop");
    case protocol::v1::FLOP: return QStringLiteral("Flop");
    case protocol::v1::TURN: return QStringLiteral("Turn");
    case protocol::v1::RIVER: return QStringLiteral("River");
    case protocol::v1::SHOWDOWN: return QStringLiteral("Showdown");
    case protocol::v1::SETTLED: return QStringLiteral("Settled");
    default: return QStringLiteral("Unknown");
    }
}

}  // namespace poker::client
