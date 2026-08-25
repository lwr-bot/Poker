#include "protocol_service.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace poker::server {
namespace {

std::string playerStatus(domain::PlayerStatus status) {
    switch (status) {
    case domain::PlayerStatus::waiting: return "waiting";
    case domain::PlayerStatus::active: return "active";
    case domain::PlayerStatus::folded: return "folded";
    case domain::PlayerStatus::all_in: return "all_in";
    case domain::PlayerStatus::disconnected: return "disconnected";
    }
    return "unknown";
}

protocol::v1::Street street(domain::Street value) {
    switch (value) {
    case domain::Street::waiting: return protocol::v1::WAITING;
    case domain::Street::preflop: return protocol::v1::PREFLOP;
    case domain::Street::flop: return protocol::v1::FLOP;
    case domain::Street::turn: return protocol::v1::TURN;
    case domain::Street::river: return protocol::v1::RIVER;
    case domain::Street::showdown: return protocol::v1::SHOWDOWN;
    case domain::Street::settled: return protocol::v1::SETTLED;
    }
    return protocol::v1::STREET_UNSPECIFIED;
}

void writeCard(protocol::v1::Card* output, domain::Card card) {
    output->set_rank(domain::rankValue(card.rank));
    output->set_suit(static_cast<std::uint32_t>(card.suit));
}

void writeSnapshot(protocol::v1::TableSnapshot* output,
                   std::uint64_t table_id,
                   const domain::TableSnapshot& snapshot) {
    output->set_table_id(table_id);
    output->set_hand_id(snapshot.hand_id);
    output->set_server_sequence(snapshot.server_sequence);
    output->set_street(street(snapshot.street));
    output->set_dealer_user_id(snapshot.dealer.value_or(0));
    output->set_acting_user_id(snapshot.acting_player.value_or(0));
    output->set_current_bet(snapshot.current_bet);
    output->set_minimum_raise(snapshot.minimum_raise);
    output->set_pot(snapshot.pot);
    for (const auto card : snapshot.board) {
        writeCard(output->add_board(), card);
    }
    for (const auto& player : snapshot.players) {
        auto* target = output->add_players();
        target->set_user_id(player.id);
        target->set_seat(static_cast<std::uint32_t>(player.seat));
        target->set_stack(player.stack);
        target->set_street_commitment(player.street_commitment);
        target->set_hand_commitment(player.hand_commitment);
        target->set_status(playerStatus(player.status));
        target->set_ready(player.ready);
        target->set_connected(player.connected);
        for (const auto card : player.hole_cards) {
            writeCard(target->add_visible_hole_cards(), card);
        }
    }
}

bool validConfig(const domain::TableConfig& config) {
    return config.min_players >= 2 && config.max_players >= config.min_players
           && config.max_players <= 6 && config.small_blind > 0
           && config.big_blind >= config.small_blind * 2 && config.min_buy_in > 0
           && config.max_buy_in >= config.min_buy_in;
}

std::uint64_t persistentHandId(std::uint64_t table_id, std::uint64_t hand_number) {
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
    if (table_id == 0 || table_id > maximum || hand_number == 0 || hand_number > maximum) {
        return 0;
    }
    return (table_id << 32U) | hand_number;
}

std::uint32_t dealerSeat(const domain::TableSnapshot& snapshot) {
    if (!snapshot.dealer.has_value()) {
        return 0;
    }
    const auto dealer = std::find_if(snapshot.players.begin(), snapshot.players.end(),
                                     [&snapshot](const auto& player) {
                                         return player.id == *snapshot.dealer;
                                     });
    return dealer == snapshot.players.end() ? 0
                                             : static_cast<std::uint32_t>(dealer->seat);
}

storage::HandStartRecord handStart(std::uint64_t table_id,
                                   const domain::TableSnapshot& snapshot) {
    storage::HandStartRecord result;
    result.hand_id = persistentHandId(table_id, snapshot.hand_id);
    result.table_id = table_id;
    result.hand_number = snapshot.hand_id;
    result.dealer_seat = dealerSeat(snapshot);
    for (const auto& player : snapshot.players) {
        if (player.hole_cards.size() == 2) {
            result.players.push_back({player.id,
                                      static_cast<std::uint32_t>(player.seat),
                                      player.stack + player.hand_commitment,
                                      player.hole_cards});
        }
    }
    return result;
}

storage::HandSettlementRecord handSettlement(
    std::uint64_t table_id,
    const domain::TableSnapshot& snapshot,
    const std::vector<domain::PotAward>& awards) {
    storage::HandSettlementRecord result;
    result.hand_id = persistentHandId(table_id, snapshot.hand_id);
    result.table_id = table_id;
    result.hand_number = snapshot.hand_id;
    result.dealer_seat = dealerSeat(snapshot);
    result.board = snapshot.board;
    std::unordered_map<domain::PlayerId, domain::Chips> winnings;
    for (const auto& award : awards) {
        result.total_pot += award.pot_size;
        for (std::size_t index = 0; index < award.winners.size(); ++index) {
            winnings[award.winners[index]] += award.equal_share
                                              + (static_cast<domain::Chips>(index) < award.odd_chips
                                                     ? 1
                                                     : 0);
        }
    }
    for (const auto& player : snapshot.players) {
        if (player.hole_cards.size() != 2) {
            continue;
        }
        const auto won = winnings[player.id];
        result.players.push_back({player.id,
                                  static_cast<std::uint32_t>(player.seat),
                                  player.stack + player.hand_commitment - won,
                                  player.hand_commitment,
                                  won,
                                  player.stack,
                                  player.hole_cards,
                                  player.status == domain::PlayerStatus::folded});
    }
    return result;
}

}  // namespace

ProtocolService::ProtocolService(const config::ServerConfig& config,
                                 security::AuthService& auth,
                                 security::CryptoProvider& crypto,
                                 cluster::NodeRegistry& registry,
                                 cluster::LobbyRouter& router,
                                 application::RoomManager& rooms,
                                 application::BlockingExecutor& storage_executor,
                                 storage::GameStore& game_store,
                                 observability::MetricsRegistry& metrics,
                                 Push push,
                                 Schedule schedule)
    : config_(config),
      auth_(auth),
      crypto_(crypto),
      registry_(registry),
      router_(router),
      rooms_(rooms),
      storage_executor_(storage_executor),
      game_store_(game_store),
      metrics_(metrics),
      push_(std::move(push)),
      schedule_(std::move(schedule)) {}

ProtocolService::TableOperation::TableOperation(ProtocolService* owner,
                                                std::uint64_t table_id) noexcept
    : owner_(owner), table_id_(table_id) {}

ProtocolService::TableOperation::~TableOperation() {
    finish();
}

void ProtocolService::TableOperation::finish() noexcept {
    if (owner_ != nullptr) {
        auto* owner = owner_;
        owner_ = nullptr;
        owner->finishTableOperation(table_id_);
    }
}

std::shared_ptr<ProtocolService::TableOperation> ProtocolService::beginTableOperation(
    std::uint64_t table_id) {
    if (table_id == 0) {
        return {};
    }
    std::lock_guard<std::mutex> lock(table_operations_mutex_);
    if (!active_table_operations_.insert(table_id).second) {
        return {};
    }
    return std::make_shared<TableOperation>(this, table_id);
}

void ProtocolService::finishTableOperation(std::uint64_t table_id) noexcept {
    try {
        std::lock_guard<std::mutex> lock(table_operations_mutex_);
        active_table_operations_.erase(table_id);
    } catch (...) {
        // The guard destructor cannot report errors. Mutex failures are unrecoverable
        // process-level faults and must not turn stack unwinding into termination.
    }
}

void ProtocolService::handle(const Envelope& request,
                             const std::shared_ptr<ConnectionSession>& session,
                             Send send) {
    if (request.protocol_version() != 1 || request.request_id() == 0
        || request.request_id() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        fail(request, protocol::v1::INVALID_MESSAGE, "protocol version and request id are required", send);
        return;
    }

    if (request.message_type() == protocol::v1::REGISTER_REQUEST) {
        if (!request.has_register_request()) {
            fail(request, protocol::v1::INVALID_MESSAGE, "register body is missing", send);
            return;
        }
        const auto request_copy = request;
        const auto body = request.register_request();
        if (!storage_executor_.post([this, request_copy, body, send] {
                const auto registered = auth_.registerUser(body.username(), body.password());
                if (!registered) {
                    const auto code = registered.error == security::AuthError::username_taken
                                          ? protocol::v1::CONFLICT
                                          : registered.error == security::AuthError::storage_unavailable
                                                ? protocol::v1::SERVICE_UNAVAILABLE
                                                : protocol::v1::INVALID_MESSAGE;
                    fail(request_copy, code, registered.message, send);
                    return;
                }
                Envelope response;
                response.set_protocol_version(1);
                response.set_request_id(request_copy.request_id());
                response.set_message_type(protocol::v1::REGISTER_RESPONSE);
                response.mutable_register_response()->set_user_id(registered.value->id);
                send(std::move(response));
            })) {
            fail(request, protocol::v1::SERVICE_UNAVAILABLE, "storage queue is full", send);
        }
        return;
    }

    if (request.message_type() == protocol::v1::LOGIN_REQUEST) {
        if (!request.has_login_request()) {
            fail(request, protocol::v1::INVALID_MESSAGE, "login body is missing", send);
            return;
        }
        bool already_authenticated = false;
        bool authentication_in_progress = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            already_authenticated = session->user_id.has_value()
                                    && session->expires_at_unix_ms > nowUnixMs();
            if (!already_authenticated && session->user_id.has_value()) {
                session->user_id.reset();
                session->raw_token.clear();
                session->expires_at_unix_ms = 0;
                session->table_id.reset();
            }
            if (!already_authenticated) {
                authentication_in_progress = session->authentication_in_progress;
                session->authentication_in_progress = true;
            }
        }
        if (already_authenticated) {
            fail(request, protocol::v1::CONFLICT,
                 "this connection is already authenticated", send);
            return;
        }
        if (authentication_in_progress) {
            fail(request, protocol::v1::CONFLICT,
                 "another authentication request is already running", send);
            return;
        }
        const auto request_copy = request;
        const auto body = request.login_request();
        if (!storage_executor_.post([this, request_copy, body, session, send] {
                const auto logged_in = auth_.login(body.username(), body.password(), nowUnixMs());
                if (!logged_in) {
                    {
                        std::lock_guard<std::mutex> lock(session->mutex);
                        session->authentication_in_progress = false;
                    }
                    fail(request_copy,
                         logged_in.error == security::AuthError::storage_unavailable
                             ? protocol::v1::SERVICE_UNAVAILABLE
                             : protocol::v1::UNAUTHENTICATED,
                         logged_in.message,
                         send);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(session->mutex);
                    session->user_id = logged_in.value->user_id;
                    session->username = logged_in.value->username;
                    session->raw_token = logged_in.value->raw_token;
                    session->expires_at_unix_ms = logged_in.value->expires_at_unix_ms;
                    session->authentication_in_progress = false;
                }
                idempotency_.reset(logged_in.value->user_id);
                Envelope response;
                response.set_protocol_version(1);
                response.set_request_id(request_copy.request_id());
                response.set_message_type(protocol::v1::LOGIN_RESPONSE);
                auto* body_out = response.mutable_login_response();
                body_out->set_user_id(logged_in.value->user_id);
                body_out->set_session_token(logged_in.value->raw_token);
                body_out->set_wallet_chips(logged_in.value->wallet_balance);
                body_out->set_expires_at_unix_ms(logged_in.value->expires_at_unix_ms);
                send(std::move(response));
            })) {
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->authentication_in_progress = false;
            }
            fail(request, protocol::v1::SERVICE_UNAVAILABLE, "storage queue is full", send);
        }
        return;
    }

    if (request.message_type() == protocol::v1::AUTHENTICATE_SESSION_REQUEST) {
        if (!request.has_authenticate_session_request()) {
            fail(request, protocol::v1::INVALID_MESSAGE,
                 "authenticate-session body is missing", send);
            return;
        }
        bool already_authenticated = false;
        bool authentication_in_progress = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            already_authenticated = session->user_id.has_value()
                                    && session->expires_at_unix_ms > nowUnixMs();
            if (!already_authenticated && session->user_id.has_value()) {
                session->user_id.reset();
                session->raw_token.clear();
                session->expires_at_unix_ms = 0;
                session->table_id.reset();
            }
            if (!already_authenticated) {
                authentication_in_progress = session->authentication_in_progress;
                session->authentication_in_progress = true;
            }
        }
        if (already_authenticated) {
            fail(request, protocol::v1::CONFLICT,
                 "this connection is already authenticated", send);
            return;
        }
        if (authentication_in_progress) {
            fail(request, protocol::v1::CONFLICT,
                 "another authentication request is already running", send);
            return;
        }
        const auto request_copy = request;
        const auto raw_token = request.authenticate_session_request().session_token();
        if (!storage_executor_.post([this, request_copy, raw_token, session, send] {
                const auto authenticated = auth_.authenticate(raw_token, nowUnixMs());
                if (!authenticated) {
                    {
                        std::lock_guard<std::mutex> lock(session->mutex);
                        session->authentication_in_progress = false;
                    }
                    fail(request_copy,
                         authenticated.error == security::AuthError::storage_unavailable
                             ? protocol::v1::SERVICE_UNAVAILABLE
                             : protocol::v1::UNAUTHENTICATED,
                         authenticated.message,
                         send);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(session->mutex);
                    session->user_id = authenticated.value->user_id;
                    session->username = authenticated.value->username;
                    session->raw_token = raw_token;
                    session->expires_at_unix_ms = authenticated.value->expires_at_unix_ms;
                    session->authentication_in_progress = false;
                }
                idempotency_.reset(authenticated.value->user_id);
                Envelope response;
                response.set_protocol_version(1);
                response.set_request_id(request_copy.request_id());
                response.set_message_type(protocol::v1::AUTHENTICATE_SESSION_RESPONSE);
                auto* body = response.mutable_authenticate_session_response();
                body->set_user_id(authenticated.value->user_id);
                body->set_username(authenticated.value->username);
                body->set_wallet_chips(authenticated.value->wallet_balance);
                body->set_expires_at_unix_ms(authenticated.value->expires_at_unix_ms);
                send(std::move(response));
            })) {
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->authentication_in_progress = false;
            }
            fail(request, protocol::v1::SERVICE_UNAVAILABLE, "storage queue is full", send);
        }
        return;
    }

    if (request.message_type() == protocol::v1::HEARTBEAT) {
        Envelope response;
        response.set_protocol_version(1);
        response.set_request_id(request.request_id());
        response.set_message_type(protocol::v1::HEARTBEAT);
        auto* body = response.mutable_heartbeat();
        body->set_client_time_unix_ms(request.has_heartbeat()
                                          ? request.heartbeat().client_time_unix_ms()
                                          : 0);
        body->set_server_time_unix_ms(nowUnixMs());
        send(std::move(response));
        return;
    }

    if (request.message_type() == protocol::v1::RECONNECT_REQUEST) {
        if (!request.has_reconnect_request()) {
            fail(request, protocol::v1::INVALID_MESSAGE, "reconnect body is missing", send);
            return;
        }
        bool already_authenticated = false;
        bool authentication_in_progress = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            already_authenticated = session->user_id.has_value()
                                    && session->expires_at_unix_ms > nowUnixMs();
            if (!already_authenticated && session->user_id.has_value()) {
                session->user_id.reset();
                session->raw_token.clear();
                session->expires_at_unix_ms = 0;
                session->table_id.reset();
            }
            if (!already_authenticated) {
                authentication_in_progress = session->authentication_in_progress;
                session->authentication_in_progress = true;
            }
        }
        if (!already_authenticated) {
            if (authentication_in_progress) {
                fail(request, protocol::v1::CONFLICT,
                     "another authentication request is already running", send);
                return;
            }
            const auto request_copy = request;
            const auto raw_token = request.reconnect_request().session_token().empty()
                                       ? request.session_token()
                                       : request.reconnect_request().session_token();
            if (!storage_executor_.post([this, request_copy, raw_token, session, send] {
                    const auto authenticated = auth_.authenticate(raw_token, nowUnixMs());
                    if (!authenticated) {
                        {
                            std::lock_guard<std::mutex> lock(session->mutex);
                            session->authentication_in_progress = false;
                        }
                        fail(request_copy,
                             authenticated.error == security::AuthError::storage_unavailable
                                 ? protocol::v1::SERVICE_UNAVAILABLE
                                 : protocol::v1::UNAUTHENTICATED,
                             authenticated.message,
                             send);
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(session->mutex);
                        session->user_id = authenticated.value->user_id;
                        session->username = authenticated.value->username;
                        session->raw_token = raw_token;
                        session->expires_at_unix_ms = authenticated.value->expires_at_unix_ms;
                        session->authentication_in_progress = false;
                    }
                    idempotency_.reset(authenticated.value->user_id);
                    handle(request_copy, session, send);
                })) {
                {
                    std::lock_guard<std::mutex> lock(session->mutex);
                    session->authentication_in_progress = false;
                }
                fail(request, protocol::v1::SERVICE_UNAVAILABLE, "storage queue is full", send);
            }
            return;
        }
    }

    const auto authorized = authorize(request, session, send);
    if (!authorized.has_value()) {
        return;
    }
    const auto user_id = authorized->user_id;
    const auto request_copy = request;

    const auto type = request.message_type();
    const bool lobby_command = type == protocol::v1::CREATE_TABLE_REQUEST
                               || type == protocol::v1::LIST_TABLES_REQUEST
                               || type == protocol::v1::JOIN_TABLE_REQUEST;
    const bool game_command = type == protocol::v1::SIT_DOWN_REQUEST
                               || type == protocol::v1::READY_REQUEST
                               || type == protocol::v1::ACTION_REQUEST
                               || type == protocol::v1::SNAPSHOT_REQUEST
                               || type == protocol::v1::RECONNECT_REQUEST
                               || type == protocol::v1::LEAVE_TABLE_REQUEST;
    if ((config_.role == config::ServerRole::game && lobby_command)
        || (config_.role == config::ServerRole::lobby && game_command)) {
        fail(request, protocol::v1::FORBIDDEN,
             lobby_command ? "this command must be sent to a lobby node"
                           : "this command must be sent to the assigned game node",
             send, user_id);
        return;
    }

    switch (request.message_type()) {
    case protocol::v1::LOGOUT_REQUEST: {
        std::string token;
        std::optional<std::uint64_t> table_id;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            token = session->raw_token.empty() ? request.session_token() : session->raw_token;
            table_id = session->table_id;
        }
        if (!storage_executor_.post([this, token, table_id, user_id, request_copy, session, send] {
                const auto error = auth_.logout(token);
                if (error != security::AuthError::ok) {
                    fail(request_copy,
                         error == security::AuthError::storage_unavailable
                             ? protocol::v1::SERVICE_UNAVAILABLE
                             : protocol::v1::UNAUTHENTICATED,
                         security::toString(error),
                         send,
                         user_id);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(session->mutex);
                    session->user_id.reset();
                    session->raw_token.clear();
                    session->expires_at_unix_ms = 0;
                    session->table_id.reset();
                }
                idempotency_.reset(user_id);
                if (table_id.has_value()) {
                    rooms_.setConnected(
                        *table_id, user_id, false,
                        [this, table_id](domain::TableError table_error,
                                         domain::TableSnapshot snapshot) {
                            if (table_error == domain::TableError::ok) {
                                broadcastSnapshot(*table_id, snapshot);
                            }
                        });
                }
                acknowledge(user_id, request_copy, "logged out", send);
            })) {
            fail(request, protocol::v1::SERVICE_UNAVAILABLE, "storage queue is full", send, user_id);
        }
        return;
    }

    case protocol::v1::REFRESH_SESSION_REQUEST: {
        if (!request.has_refresh_session_request()
            || request.refresh_session_request().session_token().empty()) {
            fail(request, protocol::v1::INVALID_MESSAGE,
                 "refresh-session token is missing", send, user_id);
            return;
        }
        const auto token = request.refresh_session_request().session_token();
        bool token_matches = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            token_matches = session->raw_token == token;
        }
        if (!token_matches) {
            fail(request, protocol::v1::FORBIDDEN,
                 "refresh token does not belong to this connection", send, user_id);
            return;
        }
        if (!storage_executor_.post([this, token, user_id, request_copy, session, send] {
                const auto refreshed = auth_.refresh(token, nowUnixMs());
                if (!refreshed) {
                    fail(request_copy,
                         refreshed.error == security::AuthError::storage_unavailable
                             ? protocol::v1::SERVICE_UNAVAILABLE
                             : protocol::v1::UNAUTHENTICATED,
                         refreshed.message,
                         send,
                         user_id);
                    return;
                }
                bool identity_changed = false;
                {
                    std::lock_guard<std::mutex> lock(session->mutex);
                    identity_changed = session->user_id != std::optional<storage::UserId>{user_id}
                                       || session->raw_token != token;
                    if (!identity_changed) {
                        session->raw_token = refreshed.value->raw_token;
                        session->expires_at_unix_ms = refreshed.value->expires_at_unix_ms;
                    }
                }
                if (identity_changed) {
                    fail(request_copy, protocol::v1::CONFLICT,
                         "connection identity changed while refreshing", send, user_id);
                    return;
                }
                Envelope response;
                response.set_message_type(protocol::v1::REFRESH_SESSION_RESPONSE);
                auto* body = response.mutable_refresh_session_response();
                body->set_session_token(refreshed.value->raw_token);
                body->set_expires_at_unix_ms(refreshed.value->expires_at_unix_ms);
                reply(user_id, request_copy, std::move(response), send);
            })) {
            fail(request, protocol::v1::SERVICE_UNAVAILABLE,
                 "storage queue is full", send, user_id);
        }
        return;
    }

    case protocol::v1::CREATE_TABLE_REQUEST: {
        if (!request.has_create_table_request()) {
            fail(request, protocol::v1::INVALID_MESSAGE, "create-table body is missing", send, user_id);
            return;
        }
        const auto body = request.create_table_request();
        domain::TableConfig table_config;
        table_config.max_players = body.max_players();
        table_config.small_blind = body.small_blind();
        table_config.big_blind = body.big_blind();
        table_config.min_buy_in = body.min_buy_in();
        table_config.max_buy_in = body.max_buy_in();
        if (!validConfig(table_config) || body.name().size() > 64) {
            fail(request, protocol::v1::INVALID_MESSAGE, "invalid table configuration", send, user_id);
            return;
        }
        if (!storage_executor_.post([this, user_id, body, table_config, request_copy, send] {
                const auto allocated = registry_.allocateTableId();
                if (!allocated.has_value()) {
                    fail(request_copy, protocol::v1::SERVICE_UNAVAILABLE,
                         "could not allocate a global table id", send, user_id);
                    return;
                }
                const auto assignment = router_.assignNewTable(*allocated, user_id);
                if (!assignment.has_value()) {
                    fail(request_copy, protocol::v1::SERVICE_UNAVAILABLE,
                         "no healthy game node is available", send, user_id);
                    return;
                }
                const std::string name = body.name().empty()
                                             ? "Table " + std::to_string(*allocated)
                                             : body.name();
                const storage::TableRecord record{*allocated,
                                                  name,
                                                  assignment->node.node_id,
                                                  table_config,
                                                  user_id};
                const auto stored = game_store_.createTable(record);
                if (stored != storage::StorageError::ok) {
                    fail(request_copy, map(stored), "table metadata could not be stored", send, user_id);
                    return;
                }
                if (assignment->node.node_id == config_.node_id
                    && rooms_.createTable(*allocated, table_config) != domain::TableError::ok) {
                    fail(request_copy, protocol::v1::SERVICE_UNAVAILABLE,
                         "local room could not be created", send, user_id);
                    return;
                }
                Envelope response;
                response.set_message_type(protocol::v1::CREATE_TABLE_RESPONSE);
                auto* output = response.mutable_create_table_response();
                auto* summary = output->mutable_table();
                summary->set_table_id(*allocated);
                summary->set_name(name);
                summary->set_max_players(static_cast<std::uint32_t>(table_config.max_players));
                summary->set_small_blind(table_config.small_blind);
                summary->set_big_blind(table_config.big_blind);
                summary->set_game_node_id(assignment->node.node_id);
                output->set_node_endpoint(assignment->node.endpoint);
                output->set_join_ticket(assignment->join_ticket);
                reply(user_id, request_copy, std::move(response), send);
            })) {
            fail(request, protocol::v1::SERVICE_UNAVAILABLE, "storage queue is full", send, user_id);
        }
        return;
    }

    case protocol::v1::LIST_TABLES_REQUEST: {
        if (!request.has_list_tables_request() || !request.list_tables_request().page_token().empty()) {
            fail(request, protocol::v1::INVALID_MESSAGE,
                 "list-tables body is missing or page token is unsupported", send, user_id);
            return;
        }
        const auto requested = request.list_tables_request().page_size();
        const auto limit = requested == 0 ? std::size_t{50}
                                          : std::min<std::size_t>(requested, 100);
        if (!storage_executor_.post([this, user_id, limit, request_copy, send] {
                const auto tables = game_store_.listOpenTables(limit);
                if (!tables) {
                    fail(request_copy, map(tables.error), tables.message, send, user_id);
                    return;
                }
                Envelope response;
                response.set_message_type(protocol::v1::LIST_TABLES_RESPONSE);
                auto* output = response.mutable_list_tables_response();
                for (const auto& table : *tables.value) {
                    auto* summary = output->add_tables();
                    summary->set_table_id(table.table_id);
                    summary->set_name(table.name);
                    summary->set_max_players(static_cast<std::uint32_t>(table.config.max_players));
                    summary->set_small_blind(table.config.small_blind);
                    summary->set_big_blind(table.config.big_blind);
                    summary->set_game_node_id(table.node_id);
                }
                reply(user_id, request_copy, std::move(response), send);
            })) {
            fail(request, protocol::v1::SERVICE_UNAVAILABLE, "storage queue is full", send, user_id);
        }
        return;
    }

    case protocol::v1::JOIN_TABLE_REQUEST: {
        if (!request.has_join_table_request()) {
            fail(request, protocol::v1::INVALID_MESSAGE, "join-table body is missing", send, user_id);
            return;
        }
        const auto table_id = request.join_table_request().table_id();
        if (!storage_executor_.post([this, user_id, table_id, request_copy, send] {
                const auto table = game_store_.findTable(table_id);
                if (!table) {
                    fail(request_copy, map(table.error), table.message, send, user_id);
                    return;
                }
                const auto assignment = router_.routeExistingTable(
                    table_id, user_id, table.value->node_id);
                if (!assignment.has_value() || assignment->node.node_id != table.value->node_id) {
                    fail(request_copy, protocol::v1::SERVICE_UNAVAILABLE,
                         "table owner is unavailable", send, user_id);
                    return;
                }
                Envelope response;
                response.set_message_type(protocol::v1::JOIN_TABLE_RESPONSE);
                response.mutable_join_table_response()->set_node_endpoint(assignment->node.endpoint);
                response.mutable_join_table_response()->set_join_ticket(assignment->join_ticket);
                reply(user_id, request_copy, std::move(response), send);
            })) {
            fail(request, protocol::v1::SERVICE_UNAVAILABLE, "storage queue is full", send, user_id);
        }
        return;
    }

    case protocol::v1::SIT_DOWN_REQUEST: {
        if (!request.has_sit_down_request()) {
            fail(request, protocol::v1::INVALID_MESSAGE, "sit-down body is missing", send, user_id);
            return;
        }
        const auto body = request.sit_down_request();
        const auto operation = beginTableOperation(body.table_id());
        if (!operation) {
            fail(request, protocol::v1::CONFLICT,
                 "another table command is awaiting persistence", send, user_id);
            return;
        }
        if (!storage_executor_.post([this, user_id, body, request_copy, session, send,
                                     operation] {
                const auto ticket = registry_.consumeJoinTicket(
                    crypto_.hashSessionToken(body.join_ticket()));
                if (!ticket.has_value() || ticket->user_id != user_id
                    || ticket->table_id != body.table_id()) {
                    fail(request_copy, protocol::v1::FORBIDDEN,
                         "join ticket is invalid or already used", send, user_id);
                    return;
                }
                const auto table = game_store_.findTable(body.table_id());
                if (!table) {
                    fail(request_copy, map(table.error), table.message, send, user_id);
                    return;
                }
                if (table.value->node_id != config_.node_id) {
                    fail(request_copy, protocol::v1::FORBIDDEN,
                         "this game node does not own the table", send, user_id);
                    return;
                }
                if (!rooms_.hasTable(body.table_id())
                    && rooms_.createTable(body.table_id(), table.value->config)
                           != domain::TableError::ok) {
                    fail(request_copy, protocol::v1::SERVICE_UNAVAILABLE,
                         "room could not be activated", send, user_id);
                    return;
                }
                const auto buy_key = "buy:" + std::to_string(body.table_id()) + ":"
                                     + std::to_string(request_copy.request_id());
                const auto reserved = game_store_.reserveBuyIn(body.table_id(),
                                                               user_id,
                                                               body.seat(),
                                                               body.buy_in(),
                                                               buy_key);
                if (!reserved) {
                    fail(request_copy, map(reserved.error), reserved.message, send, user_id);
                    return;
                }
                rooms_.seatPlayer(
                    body.table_id(), user_id, body.seat(), reserved.value->table_stack,
                    [this, user_id, body, request_copy, session, send,
                     operation](domain::TableError error) {
                        if (error != domain::TableError::ok) {
                            const auto rollback_key = "rollback:" + std::to_string(body.table_id())
                                                      + ":" + std::to_string(request_copy.request_id());
                            const auto rollback_queued = storage_executor_.post(
                                [this, table_id = body.table_id(), user_id,
                                 rollback_key, operation] {
                                static_cast<void>(operation);
                                const auto rolled_back = game_store_.cashOut(
                                    table_id, user_id, rollback_key);
                                if (!rolled_back) {
                                    domain::TableSnapshot audit;
                                    domain::PlayerView affected;
                                    affected.id = user_id;
                                    audit.players.push_back(std::move(affected));
                                    abortTableAndNotify(
                                        table_id, audit,
                                        "reserved buy-in could not be rolled back", true);
                                }
                            });
                            if (!rollback_queued) {
                                domain::TableSnapshot audit;
                                domain::PlayerView affected;
                                affected.id = user_id;
                                audit.players.push_back(std::move(affected));
                                abortTableAndNotify(body.table_id(), audit,
                                                   "buy-in rollback queue is full", false);
                                fail(request_copy, protocol::v1::SERVICE_UNAVAILABLE,
                                     "buy-in rollback was deferred and the table was closed",
                                     send, user_id);
                                return;
                            }
                            fail(request_copy, map(error), domain::toString(error), send, user_id);
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lock(session->mutex);
                            session->table_id = body.table_id();
                        }
                        rooms_.snapshot(
                            body.table_id(), user_id,
                            [this, user_id, body, request_copy, send, operation](
                                domain::TableError snapshot_error,
                                domain::TableSnapshot snapshot) {
                                static_cast<void>(operation);
                                if (snapshot_error != domain::TableError::ok) {
                                    fail(request_copy, map(snapshot_error),
                                         domain::toString(snapshot_error), send, user_id);
                                    return;
                                }
                                reply(user_id, request_copy,
                                      snapshotEnvelope(request_copy, snapshot), send);
                                broadcastSnapshot(body.table_id(), snapshot);
                            });
                    });
            })) {
            fail(request, protocol::v1::SERVICE_UNAVAILABLE, "storage queue is full", send, user_id);
        }
        return;
    }

    case protocol::v1::READY_REQUEST: {
        if (!request.has_ready_request()) {
            fail(request, protocol::v1::INVALID_MESSAGE, "ready body is missing", send, user_id);
            return;
        }
        const auto body = request.ready_request();
        const auto operation = beginTableOperation(body.table_id());
        if (!operation) {
            fail(request, protocol::v1::CONFLICT,
                 "another table command is awaiting persistence", send, user_id);
            return;
        }
        rooms_.setReady(body.table_id(), user_id, body.ready(),
                        [this, user_id, body, request_copy, send,
                         operation](domain::TableError error) {
            if (error != domain::TableError::ok) {
                fail(request_copy, map(error), domain::toString(error), send, user_id);
                return;
            }
            if (!body.ready()) {
                operation->finish();
                replyWithCurrentSnapshot(body.table_id(), user_id, request_copy, send, false);
                return;
            }
            rooms_.startHand(body.table_id(),
                             [this, user_id, body, request_copy, send,
                              operation](domain::TableError start_error) {
                if (start_error != domain::TableError::ok
                    && start_error != domain::TableError::not_enough_players
                    && start_error != domain::TableError::hand_in_progress) {
                    fail(request_copy, map(start_error), domain::toString(start_error), send, user_id);
                    return;
                }
                if (start_error != domain::TableError::ok) {
                    operation->finish();
                    replyWithCurrentSnapshot(body.table_id(), user_id, request_copy, send, true);
                    return;
                }
                rooms_.auditSnapshot(
                    body.table_id(),
                    [this, user_id, body, request_copy, send,
                     operation](domain::TableError audit_error,
                                domain::TableSnapshot audit) {
                        if (audit_error != domain::TableError::ok) {
                            rooms_.abortHand(body.table_id(), {});
                            fail(request_copy, map(audit_error), domain::toString(audit_error),
                                 send, user_id);
                            return;
                        }
                        const auto start = handStart(body.table_id(), audit);
                        if (!storage_executor_.post(
                                [this, user_id, body, request_copy, send, start,
                                 operation] {
                                    static_cast<void>(operation);
                                    const auto stored = game_store_.beginHand(start);
                                    if (stored != storage::StorageError::ok
                                        && stored != storage::StorageError::duplicate) {
                                        rooms_.abortHand(body.table_id(), {});
                                        fail(request_copy, map(stored),
                                             "hand start could not be persisted", send, user_id);
                                        return;
                                    }
                                    operation->finish();
                                    replyWithCurrentSnapshot(body.table_id(), user_id,
                                                             request_copy, send, true);
                                })) {
                            rooms_.abortHand(body.table_id(), {});
                            fail(request_copy, protocol::v1::SERVICE_UNAVAILABLE,
                                 "storage queue is full", send, user_id);
                        }
                    });
            });
        });
        return;
    }

    case protocol::v1::ACTION_REQUEST: {
        if (!request.has_action_request()
            || request.action_request().action() == protocol::v1::ACTION_TYPE_UNSPECIFIED) {
            fail(request, protocol::v1::INVALID_MESSAGE, "action body is missing or invalid", send, user_id);
            return;
        }
        const auto body = request.action_request();
        const auto operation = beginTableOperation(body.table_id());
        if (!operation) {
            fail(request, protocol::v1::CONFLICT,
                 "another table command is awaiting persistence", send, user_id);
            return;
        }
        rooms_.act(body.table_id(),
                   body.hand_id(),
                   {user_id, map(body.action()), body.target_street_commitment()},
                   [this, user_id, body, request_copy, send, operation](
                       domain::ActionResult result,
                       domain::TableSnapshot snapshot,
                       domain::TableSnapshot audit) {
            if (!result) {
                metrics_.actionRejected();
                fail(request_copy, map(result.error), result.message, send, user_id);
                return;
            }
            metrics_.actionAccepted();
            storage::HandActionRecord action;
            action.hand_id = persistentHandId(body.table_id(), audit.hand_id);
            action.sequence = result.server_sequence;
            action.request_id = request_copy.request_id();
            action.user_id = user_id;
            action.street = result.action_street;
            action.action = map(body.action());
            action.target_amount = body.target_street_commitment();
            const auto settlement = audit.street == domain::Street::settled
                                        ? std::optional<storage::HandSettlementRecord>{
                                              handSettlement(body.table_id(), audit,
                                                             result.awards)}
                                        : std::nullopt;
            if (!storage_executor_.post(
                    [this, user_id, body, request_copy, send, snapshot,
                     audit, action, settlement, operation] {
                        static_cast<void>(operation);
                        const auto action_stored = game_store_.appendAction(action);
                        if (action_stored != storage::StorageError::ok
                            && action_stored != storage::StorageError::duplicate) {
                            metrics_.storageFailure();
                            abortTableAndNotify(
                                body.table_id(), audit,
                                "action history could not be persisted", true);
                            fail(request_copy, protocol::v1::SERVICE_UNAVAILABLE,
                                 "action persistence failed; the table was safely aborted",
                                 send, user_id);
                            return;
                        }
                        if (settlement.has_value()) {
                            const auto settled = game_store_.settleHand(*settlement);
                            if (settled != storage::StorageError::ok
                                && settled != storage::StorageError::duplicate) {
                                metrics_.storageFailure();
                                abortTableAndNotify(
                                    body.table_id(), audit,
                                    "settlement could not be committed", true);
                                fail(request_copy, protocol::v1::SERVICE_UNAVAILABLE,
                                     "settlement failed; the table was safely aborted",
                                     send, user_id);
                                return;
                            }
                        }
                        scheduleTimeout(body.table_id(), snapshot);
                        operation->finish();
                        reply(user_id, request_copy,
                            snapshotEnvelope(request_copy, snapshot), send);
                        broadcastSnapshot(body.table_id(), snapshot);
                    })) {
                abortTableAndNotify(body.table_id(), audit,
                                   "storage queue is full", false);
                fail(request_copy, protocol::v1::SERVICE_UNAVAILABLE,
                     "storage queue is full; the table was safely aborted",
                     send, user_id);
            }
        });
        return;
    }

    case protocol::v1::SNAPSHOT_REQUEST: {
        if (!request.has_snapshot_request()) {
            fail(request, protocol::v1::INVALID_MESSAGE, "snapshot body is missing", send, user_id);
            return;
        }
        const auto table_id = request.snapshot_request().table_id();
        rooms_.snapshot(table_id, user_id,
                        [this, user_id, request_copy, send](domain::TableError error,
                                                            domain::TableSnapshot snapshot) {
            if (error != domain::TableError::ok) {
                fail(request_copy, map(error), domain::toString(error), send, user_id);
                return;
            }
            reply(user_id, request_copy, snapshotEnvelope(request_copy, snapshot), send);
        });
        return;
    }

    case protocol::v1::RECONNECT_REQUEST: {
        if (!request.has_reconnect_request()) {
            fail(request, protocol::v1::INVALID_MESSAGE, "reconnect body is missing", send, user_id);
            return;
        }
        const auto table_id = request.reconnect_request().table_id();
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->table_id = table_id;
        }
        rooms_.setConnected(table_id, user_id, true,
                            [this, user_id, table_id, request_copy, send](domain::TableError error,
                                                                         domain::TableSnapshot snapshot) {
            if (error != domain::TableError::ok) {
                fail(request_copy, map(error), domain::toString(error), send, user_id);
                return;
            }
            reply(user_id, request_copy, snapshotEnvelope(request_copy, snapshot), send);
            broadcastSnapshot(table_id, snapshot);
            scheduleTimeout(table_id, snapshot);
        });
        return;
    }

    case protocol::v1::LEAVE_TABLE_REQUEST: {
        if (!request.has_leave_table_request()) {
            fail(request, protocol::v1::INVALID_MESSAGE, "leave-table body is missing", send, user_id);
            return;
        }
        const auto table_id = request.leave_table_request().table_id();
        const auto operation = beginTableOperation(table_id);
        if (!operation) {
            fail(request, protocol::v1::CONFLICT,
                 "another table command is awaiting persistence", send, user_id);
            return;
        }
        rooms_.setReady(table_id, user_id, false,
                        [this, table_id, user_id, request_copy, session, send,
                         operation](domain::TableError error) {
            if (error != domain::TableError::ok) {
                fail(request_copy, map(error), domain::toString(error), send, user_id);
                return;
            }
            const auto cash_key = "cash:" + std::to_string(table_id) + ":"
                                  + std::to_string(request_copy.request_id());
            if (!storage_executor_.post(
                    [this, table_id, user_id, cash_key, request_copy, session, send,
                     operation] {
                        const auto cashed_out = game_store_.cashOut(table_id, user_id, cash_key);
                        if (!cashed_out) {
                            fail(request_copy, map(cashed_out.error), cashed_out.message,
                                 send, user_id);
                            return;
                        }
                        rooms_.removePlayer(
                            table_id, user_id,
                            [this, table_id, user_id, request_copy, session,
                             send, operation](domain::TableError remove_error) {
                                static_cast<void>(operation);
                                if (remove_error != domain::TableError::ok) {
                                    fail(request_copy, map(remove_error),
                                         domain::toString(remove_error), send, user_id);
                                    return;
                                }
                                {
                                    std::lock_guard<std::mutex> lock(session->mutex);
                                    session->table_id.reset();
                                }
                                acknowledge(user_id, request_copy, "left table", send);
                                rooms_.snapshot(
                                    table_id, std::nullopt,
                                    [this, table_id](domain::TableError snapshot_error,
                                                     domain::TableSnapshot snapshot) {
                                        if (snapshot_error == domain::TableError::ok) {
                                            broadcastSnapshot(table_id, snapshot);
                                        }
                                    });
                            });
                    })) {
                fail(request_copy, protocol::v1::SERVICE_UNAVAILABLE,
                     "storage queue is full", send, user_id);
            }
        });
        return;
    }

    default:
        fail(request, protocol::v1::INVALID_MESSAGE, "unsupported message type", send, user_id);
        return;
    }
}

void ProtocolService::onDisconnect(const std::shared_ptr<ConnectionSession>& session) {
    std::optional<storage::UserId> user_id;
    std::optional<std::uint64_t> table_id;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        user_id = session->user_id;
        table_id = session->table_id;
    }
    if (!user_id.has_value() || !table_id.has_value()) {
        return;
    }
    rooms_.setConnected(*table_id, *user_id, false,
                        [this, table_id](domain::TableError error, domain::TableSnapshot snapshot) {
        if (error == domain::TableError::ok) {
            broadcastSnapshot(*table_id, snapshot);
            scheduleTimeout(*table_id, snapshot);
        }
    });
}

std::optional<ProtocolService::Authorized> ProtocolService::authorize(
    const Envelope& request,
    const std::shared_ptr<ConnectionSession>& session,
    const Send& send) {
    std::optional<storage::UserId> user_id;
    std::optional<std::uint64_t> expired_table_id;
    bool expired = false;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        user_id = session->user_id;
        expired = user_id.has_value() && session->expires_at_unix_ms <= nowUnixMs();
        if (expired) {
            expired_table_id = session->table_id;
            session->user_id.reset();
            session->raw_token.clear();
            session->expires_at_unix_ms = 0;
            session->table_id.reset();
        }
    }
    if (expired && expired_table_id.has_value()) {
        rooms_.setConnected(
            *expired_table_id, *user_id, false,
            [this, table_id = *expired_table_id](domain::TableError error,
                                                 domain::TableSnapshot snapshot) {
                if (error == domain::TableError::ok) {
                    broadcastSnapshot(table_id, snapshot);
                }
            });
    }
    if (expired) {
        user_id.reset();
    }
    bool newly_authenticated = false;
    if (!user_id.has_value()) {
        fail(request,
             protocol::v1::UNAUTHENTICATED,
             expired ? "session expired; login again"
                     : "login or reconnect is required before this command",
             send);
        return std::nullopt;
    }
    if (!beginRequest(*user_id, request, send)) {
        return std::nullopt;
    }
    return Authorized{*user_id, newly_authenticated};
}

bool ProtocolService::beginRequest(storage::UserId user_id,
                                   const Envelope& request,
                                   const Send& send) {
    const auto decision = idempotency_.begin(user_id,
                                             request.request_id(),
                                             request.client_sequence());
    if (decision.status == application::RequestStatus::accepted) {
        return true;
    }
    if (decision.status == application::RequestStatus::duplicate
        && decision.cached_response.has_value()) {
        Envelope cached;
        if (cached.ParseFromArray(decision.cached_response->data(),
                                  static_cast<int>(decision.cached_response->size()))) {
            send(std::move(cached));
            return false;
        }
    }
    const auto message = decision.status == application::RequestStatus::in_flight
                             ? "request is already being processed"
                             : decision.status == application::RequestStatus::capacity_exceeded
                                   ? "too many requests are still awaiting cached responses"
                                   : "client sequence is stale or contains a gap";
    fail(request,
         decision.status == application::RequestStatus::capacity_exceeded
             ? protocol::v1::RATE_LIMITED
             : decision.status == application::RequestStatus::in_flight
                   ? protocol::v1::CONFLICT
                   : protocol::v1::OUT_OF_SEQUENCE,
         message,
         send);
    return false;
}

void ProtocolService::reply(storage::UserId user_id,
                            const Envelope& request,
                            Envelope response,
                            const Send& send) {
    response.set_protocol_version(1);
    response.set_request_id(request.request_id());
    std::string serialized;
    response.SerializeToString(&serialized);
    idempotency_.complete(user_id,
                          request.request_id(),
                          std::vector<std::uint8_t>(serialized.begin(), serialized.end()));
    send(std::move(response));
}

void ProtocolService::fail(const Envelope& request,
                           protocol::v1::ErrorCode code,
                           std::string message,
                           const Send& send,
                           std::optional<storage::UserId> user_id) {
    metrics_.rejectedRequest();
    if (code == protocol::v1::SERVICE_UNAVAILABLE) {
        metrics_.storageFailure();
    }
    Envelope response;
    response.set_protocol_version(1);
    response.set_request_id(request.request_id());
    response.set_message_type(protocol::v1::ERROR_RESPONSE);
    auto* error = response.mutable_error_response();
    error->set_code(code);
    error->set_message(std::move(message));
    error->set_related_request_id(request.request_id());
    if (user_id.has_value()) {
        reply(*user_id, request, std::move(response), send);
    } else {
        send(std::move(response));
    }
}

void ProtocolService::acknowledge(storage::UserId user_id,
                                  const Envelope& request,
                                  std::string message,
                                  const Send& send) {
    Envelope response;
    response.set_message_type(protocol::v1::COMMAND_ACK);
    response.mutable_command_ack()->set_accepted(true);
    response.mutable_command_ack()->set_message(std::move(message));
    reply(user_id, request, std::move(response), send);
}

ProtocolService::Envelope ProtocolService::snapshotEnvelope(
    const Envelope& request,
    const domain::TableSnapshot& snapshot) const {
    Envelope response;
    response.set_protocol_version(1);
    response.set_request_id(request.request_id());
    response.set_message_type(protocol::v1::TABLE_SNAPSHOT);
    response.set_table_id(request.table_id());
    response.set_hand_id(snapshot.hand_id);
    response.set_server_sequence(snapshot.server_sequence);
    const auto table_id = request.table_id() != 0
                              ? request.table_id()
                              : request.has_action_request()
                                    ? request.action_request().table_id()
                                    : request.has_ready_request()
                                          ? request.ready_request().table_id()
                                          : request.has_sit_down_request()
                                                ? request.sit_down_request().table_id()
                                                : request.has_snapshot_request()
                                                      ? request.snapshot_request().table_id()
                                                      : request.has_reconnect_request()
                                                            ? request.reconnect_request().table_id()
                                                            : 0;
    response.set_table_id(table_id);
    writeSnapshot(response.mutable_table_snapshot(), table_id, snapshot);
    return response;
}

ProtocolService::Envelope ProtocolService::pushSnapshotEnvelope(
    std::uint64_t table_id,
    const domain::TableSnapshot& snapshot) const {
    Envelope response;
    response.set_protocol_version(1);
    response.set_message_type(protocol::v1::TABLE_SNAPSHOT);
    response.set_table_id(table_id);
    response.set_hand_id(snapshot.hand_id);
    response.set_server_sequence(snapshot.server_sequence);
    writeSnapshot(response.mutable_table_snapshot(), table_id, snapshot);
    return response;
}

void ProtocolService::replyWithCurrentSnapshot(std::uint64_t table_id,
                                               storage::UserId user_id,
                                               const Envelope& request,
                                               const Send& send,
                                               bool arm_timeout) {
    rooms_.snapshot(table_id, user_id,
                    [this, table_id, user_id, request, send, arm_timeout](domain::TableError error,
                                                                         domain::TableSnapshot snapshot) {
        if (error != domain::TableError::ok) {
            fail(request, map(error), domain::toString(error), send, user_id);
            return;
        }
        reply(user_id, request, snapshotEnvelope(request, snapshot), send);
        broadcastSnapshot(table_id, snapshot);
        if (arm_timeout) {
            scheduleTimeout(table_id, snapshot);
        }
    });
}

void ProtocolService::broadcastSnapshot(std::uint64_t table_id,
                                        const domain::TableSnapshot& source_snapshot) {
    for (const auto& player : source_snapshot.players) {
        rooms_.snapshot(table_id, player.id,
                        [this, table_id, user_id = player.id](domain::TableError error,
                                                              domain::TableSnapshot snapshot) {
            if (error == domain::TableError::ok && push_) {
                push_(user_id, pushSnapshotEnvelope(table_id, snapshot));
            }
        });
    }
}

void ProtocolService::pushTableEvent(std::uint64_t table_id,
                                     std::uint64_t hand_id,
                                     std::uint64_t server_sequence,
                                     const std::vector<storage::UserId>& users,
                                     std::string event_type,
                                     std::string message) {
    if (!push_) {
        return;
    }
    Envelope envelope;
    envelope.set_protocol_version(1);
    envelope.set_message_type(protocol::v1::TABLE_EVENT);
    auto* event = envelope.mutable_table_event();
    event->set_table_id(table_id);
    event->set_hand_id(hand_id);
    event->set_server_sequence(server_sequence);
    event->set_event_type(std::move(event_type));
    event->set_event_payload(std::move(message));
    for (const auto user_id : users) {
        push_(user_id, envelope);
    }
}

void ProtocolService::abortTableAndNotify(std::uint64_t table_id,
                                          const domain::TableSnapshot& audit,
                                          std::string reason,
                                          bool attempt_refund) {
    std::vector<storage::UserId> users;
    users.reserve(audit.players.size());
    for (const auto& player : audit.players) {
        users.push_back(player.id);
    }
    rooms_.closeTable(table_id);

    const auto refunded = attempt_refund
                              ? game_store_.abortTableAndRefund(table_id)
                              : storage::StorageError::unavailable;
    if (refunded == storage::StorageError::ok) {
        pushTableEvent(table_id, audit.hand_id, audit.server_sequence, users,
                       "table_aborted_refunded", reason + "; all table stacks were refunded");
        return;
    }

    metrics_.storageFailure();
    pushTableEvent(table_id, audit.hand_id, audit.server_sequence, users,
                   "table_aborted_refund_pending", reason + "; stack refund is pending");
    retryRefund(table_id, audit.hand_id, audit.server_sequence, std::move(users), 1);
}

void ProtocolService::retryRefund(std::uint64_t table_id,
                                  std::uint64_t hand_id,
                                  std::uint64_t server_sequence,
                                  std::vector<storage::UserId> users,
                                  std::size_t attempt) {
    constexpr std::size_t max_attempts = 8;
    if (!schedule_ || attempt > max_attempts) {
        pushTableEvent(table_id, hand_id, server_sequence, users,
                       "table_refund_requires_reconciliation",
                       "automatic stack refund retries were exhausted; operator reconciliation is required");
        return;
    }
    const auto exponent = std::min<std::size_t>(attempt - 1, 5);
    const auto delay = std::chrono::seconds(std::size_t{1} << exponent);
    schedule_(std::chrono::duration_cast<std::chrono::milliseconds>(delay),
              [this, table_id, hand_id, server_sequence,
               users = std::move(users), attempt]() mutable {
        auto fallback_users = users;
        const auto queued = storage_executor_.post(
            [this, table_id, hand_id, server_sequence,
             users = std::move(users), attempt]() mutable {
                const auto result = game_store_.abortTableAndRefund(table_id);
                if (result == storage::StorageError::ok) {
                    pushTableEvent(table_id, hand_id, server_sequence, users,
                                   "table_refund_completed",
                                   "the deferred table-stack refund completed");
                    return;
                }
                metrics_.storageFailure();
                retryRefund(table_id, hand_id, server_sequence,
                            std::move(users), attempt + 1);
            });
        if (!queued) {
            retryRefund(table_id, hand_id, server_sequence,
                        std::move(fallback_users), attempt + 1);
        }
    });
}

void ProtocolService::scheduleTimeout(
    std::uint64_t table_id,
    const domain::TableSnapshot& snapshot) {
    if (!schedule_) {
        return;
    }

    const auto expected_sequence = snapshot.server_sequence;
    {
        std::lock_guard<std::mutex> lock(table_operations_mutex_);
        auto [position, inserted] =
            latest_timeout_sequences_.try_emplace(table_id, expected_sequence);

        if (!inserted && expected_sequence < position->second) {
            return;
        }

        position->second = expected_sequence;
    }

    if (!snapshot.acting_player.has_value()) {
        return;
    }

    const auto acting_player = *snapshot.acting_player;
    const auto actor = std::find_if(
        snapshot.players.begin(),
        snapshot.players.end(),
        [acting_player](const auto& player) {
            return player.id == acting_player;
        });

    const auto timeout_action =
        actor != snapshot.players.end()
                && actor->street_commitment == snapshot.current_bet
            ? domain::ActionType::check
            : domain::ActionType::fold;

    schedule_(
        std::chrono::milliseconds(config_.action_timeout_ms),
        [this,
         table_id,
         acting_player,
         expected_sequence,
         timeout_action] {
            {
                std::lock_guard<std::mutex> lock(table_operations_mutex_);
                const auto latest =
                    latest_timeout_sequences_.find(table_id);

                if (latest == latest_timeout_sequences_.end()
                    || latest->second != expected_sequence) {
                    return;
                }
            }

            rooms_.auditSnapshot(
                table_id,
                [this,
                 table_id,
                 acting_player,
                 expected_sequence,
                 timeout_action](
                    domain::TableError snapshot_error,
                    domain::TableSnapshot current) {
                    if (snapshot_error != domain::TableError::ok
                        || current.server_sequence != expected_sequence
                        || !current.acting_player.has_value()
                        || *current.acting_player != acting_player) {
                        return;
                    }

                    const auto operation =
                        beginTableOperation(table_id);

                    if (!operation) {
                        return;
                    }

                    rooms_.onActionTimeout(
                        table_id,
                        acting_player,
                        expected_sequence,
                        [this,
                         table_id,
                         acting_player,
                         expected_sequence,
                         timeout_action,
                         operation](
                            domain::ActionResult result,
                            domain::TableSnapshot after,
                            domain::TableSnapshot audit) {
                            if (!result) {
                                return;
                            }

                            storage::HandActionRecord action;
                            action.hand_id =
                                persistentHandId(table_id, audit.hand_id);
                            action.sequence = result.server_sequence;
                            action.request_id =
                                (std::uint64_t{1} << 63U)
                                | expected_sequence;
                            action.user_id = acting_player;
                            action.street = result.action_street;
                            action.action = timeout_action;

                            const auto settlement =
                                audit.street == domain::Street::settled
                                    ? std::optional<
                                          storage::HandSettlementRecord>{
                                          handSettlement(
                                              table_id,
                                              audit,
                                              result.awards)}
                                    : std::nullopt;

                            const auto queued =
                                storage_executor_.post(
                                    [this,
                                     table_id,
                                     action,
                                     settlement,
                                     after,
                                     audit,
                                     operation] {
                                        const auto action_stored =
                                            game_store_.appendAction(action);

                                        if (action_stored
                                                != storage::StorageError::ok
                                            && action_stored
                                                != storage::StorageError::
                                                       duplicate) {
                                            metrics_.storageFailure();
                                            abortTableAndNotify(
                                                table_id,
                                                audit,
                                                "timeout action history "
                                                "could not be persisted",
                                                true);
                                            return;
                                        }

                                        if (settlement.has_value()) {
                                            const auto settled =
                                                game_store_.settleHand(
                                                    *settlement);

                                            if (settled
                                                    != storage::StorageError::ok
                                                && settled
                                                    != storage::StorageError::
                                                           duplicate) {
                                                metrics_.storageFailure();
                                                abortTableAndNotify(
                                                    table_id,
                                                    audit,
                                                    "timeout settlement "
                                                    "could not be committed",
                                                    true);
                                                return;
                                            }
                                        }

                                        scheduleTimeout(table_id, after);
                                        operation->finish();
                                        broadcastSnapshot(table_id, after);
                                    });

                            if (!queued) {
                                abortTableAndNotify(
                                    table_id,
                                    audit,
                                    "storage queue is full after "
                                    "timeout action",
                                    false);
                                operation->finish();
                            }
                        });
                });
        });
}

std::int64_t ProtocolService::nowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

protocol::v1::ErrorCode ProtocolService::map(domain::TableError error) {
    switch (error) {
    case domain::TableError::ok: return protocol::v1::ERROR_CODE_OK;
    case domain::TableError::player_not_found: return protocol::v1::NOT_FOUND;
    case domain::TableError::seat_occupied:
    case domain::TableError::player_already_seated:
    case domain::TableError::hand_in_progress: return protocol::v1::CONFLICT;
    case domain::TableError::service_unavailable: return protocol::v1::SERVICE_UNAVAILABLE;
    default: return protocol::v1::TABLE_RULE_VIOLATION;
    }
}

protocol::v1::ErrorCode ProtocolService::map(storage::StorageError error) {
    switch (error) {
    case storage::StorageError::ok: return protocol::v1::ERROR_CODE_OK;
    case storage::StorageError::not_found: return protocol::v1::NOT_FOUND;
    case storage::StorageError::duplicate:
    case storage::StorageError::conflict: return protocol::v1::CONFLICT;
    case storage::StorageError::insufficient_funds: return protocol::v1::TABLE_RULE_VIOLATION;
    case storage::StorageError::invalid_data: return protocol::v1::INVALID_MESSAGE;
    case storage::StorageError::unavailable: return protocol::v1::SERVICE_UNAVAILABLE;
    }
    return protocol::v1::INTERNAL_ERROR;
}

domain::ActionType ProtocolService::map(protocol::v1::ActionType action) {
    switch (action) {
    case protocol::v1::FOLD: return domain::ActionType::fold;
    case protocol::v1::CHECK: return domain::ActionType::check;
    case protocol::v1::CALL: return domain::ActionType::call;
    case protocol::v1::BET: return domain::ActionType::bet;
    case protocol::v1::RAISE: return domain::ActionType::raise;
    case protocol::v1::ALL_IN: return domain::ActionType::all_in;
    default: return domain::ActionType::fold;
    }
}

}  // namespace poker::server
