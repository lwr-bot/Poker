#include "health_http_server.hpp"
#include "poker_tcp_server.hpp"
#include "protocol_service.hpp"

#include "poker/application/room_manager.hpp"
#include "poker/application/blocking_executor.hpp"
#include "poker/cluster/hiredis_node_registry.hpp"
#include "poker/cluster/lobby_router.hpp"
#include "poker/cluster/node_failure_reaper.hpp"
#include "poker/config/server_config.hpp"
#include "poker/observability/metrics.hpp"
#include "poker/security/auth_service.hpp"
#include "poker/security/sodium_crypto_provider.hpp"
#include "poker/storage/mysql_account_store.hpp"
#include "poker/storage/mysql_connection_pool.hpp"
#include "poker/storage/mysql_game_store.hpp"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

int main() {
    const auto loaded = poker::config::loadFromEnvironment();
    if (!loaded) {
        for (const auto& error : loaded.errors) {
            std::cerr << "configuration error: " << error << '\n';
        }
        return 2;
    }
    const auto config = *loaded.value;

    try {
        poker::storage::MySqlConnectionPool mysql_pool(config.mysql);
        poker::storage::MySqlAccountStore accounts(mysql_pool);
        poker::storage::MySqlGameStore games(mysql_pool);
        poker::security::SodiumCryptoProvider crypto;
        poker::security::AuthService auth(accounts,
                                          crypto,
                                          config.session_ttl_seconds,
                                          100'000);
        poker::cluster::HiredisNodeRegistry registry(config.redis);
        if (config.role != poker::config::ServerRole::lobby) {
            const auto nodes = registry.healthyNodes();
            const auto duplicate_node = std::find_if(
                nodes.begin(), nodes.end(), [&config](const auto& node) {
                    return node.node_id == config.node_id;
                });
            if (duplicate_node != nodes.end()) {
                throw std::runtime_error(
                    "POKER_NODE_ID is already registered; wait for its heartbeat TTL or choose a unique id");
            }
            const auto stale_tables = games.listOpenTables(10'000);
            if (!stale_tables) {
                throw std::runtime_error("could not inspect stale tables during node startup");
            }
            for (const auto& table : *stale_tables.value) {
                if (table.node_id != config.node_id) {
                    continue;
                }
                if (games.abortTableAndRefund(table.table_id)
                    != poker::storage::StorageError::ok) {
                    throw std::runtime_error(
                        "could not refund stale table " + std::to_string(table.table_id));
                }
                LOG_WARN << "refunded stale table during node startup table=" << table.table_id;
            }
        }
        poker::cluster::LobbyRouter router(registry, crypto);
        poker::cluster::NodeFailureReaper failure_reaper(
            registry,
            games,
            std::chrono::milliseconds(config.node_failure_grace_ms));
        poker::application::RoomManager rooms(config.logic_shards);
        poker::application::BlockingExecutor storage_executor(config.mysql.pool_size);
        poker::application::BlockingExecutor coordination_executor(1, 1);
        poker::observability::MetricsRegistry metrics;
        std::atomic<bool> coordination_healthy{false};
        std::atomic<bool> database_healthy{false};

        muduo::net::EventLoop loop;
        poker::server::PokerTcpServer* tcp_server = nullptr;
        poker::server::ProtocolService service(
            config,
            auth,
            crypto,
            registry,
            router,
            rooms,
            storage_executor,
            games,
            metrics,
            [&tcp_server](auto user_id, auto envelope) {
                if (tcp_server != nullptr) {
                    tcp_server->push(user_id, std::move(envelope));
                }
            },
            [&loop](std::chrono::milliseconds delay, std::function<void()> task) {
                loop.runAfter(static_cast<double>(delay.count()) / 1000.0, std::move(task));
            });

        const muduo::net::InetAddress address(config.listen_host, config.listen_port);
        poker::server::PokerTcpServer server(&loop, address, config, service, metrics);
        tcp_server = &server;

        const auto heartbeat = [&registry, &config, &rooms, &metrics,
                                &coordination_healthy, &database_healthy,
                                &coordination_executor, &mysql_pool, &games] {
            if (!coordination_executor.post([&registry, &config, &rooms, &metrics,
                                        &coordination_healthy, &database_healthy,
                                        &mysql_pool, &games] {
                    bool mysql_healthy = mysql_pool.ping();
                    const auto table_ids = rooms.tableIds();
                    std::size_t table_count = table_ids.size();
                    if (mysql_healthy && config.role != poker::config::ServerRole::lobby) {
                        const auto owned_tables = games.countOpenTables(config.node_id);
                        if (owned_tables) {
                            table_count = *owned_tables.value;
                        } else {
                            mysql_healthy = false;
                        }
                    }
                    database_healthy.store(mysql_healthy, std::memory_order_relaxed);
                    if (!mysql_healthy) {
                        LOG_WARN << "mysql health ping failed";
                    }
                    metrics.setTables(table_count);
                    bool healthy = mysql_healthy
                                   && (config.role == poker::config::ServerRole::lobby
                                           ? registry.ping()
                                           : registry.heartbeat({config.node_id,
                                                                 config.public_endpoint,
                                                                 table_count,
                                                                 metrics.activeConnections()},
                                                                6));
                    if (healthy && config.role != poker::config::ServerRole::lobby) {
                        for (const auto table_id : table_ids) {
                            healthy = registry.assignRoom(table_id, config.node_id, 30) && healthy;
                        }
                    }
                    coordination_healthy.store(healthy, std::memory_order_relaxed);
                    if (!healthy) {
                        LOG_WARN << "redis coordination heartbeat failed node=" << config.node_id;
                    }
            })) {
                coordination_healthy.store(false, std::memory_order_relaxed);
                database_healthy.store(false, std::memory_order_relaxed);
            }
        };
        heartbeat();
        loop.runEvery(2.0, heartbeat);
        if (config.role == poker::config::ServerRole::lobby) {
            loop.runEvery(5.0, [&coordination_executor, &failure_reaper, &metrics] {
                if (!coordination_executor.post([&failure_reaper, &metrics] {
                        const auto result = failure_reaper.sweep();
                        for (const auto table_id : result.refunded_tables) {
                            LOG_WARN << "refunded table after owner heartbeat expired table=" << table_id;
                        }
                        if (!result.failed_tables.empty()) {
                            metrics.storageFailure();
                            LOG_ERROR << "failed to refund " << result.failed_tables.size()
                                      << " table(s) after node failure";
                        }
                    })) {
                    metrics.storageFailure();
                }
            });
        }

        LOG_INFO << "starting poker server role=" << poker::config::toString(config.role)
                 << " node=" << config.node_id
                 << " listen=" << config.listen_host << ':' << config.listen_port;
        const muduo::net::InetAddress metrics_address(config.metrics_host, config.metrics_port);
        poker::server::HealthHttpServer health_server(
            &loop,
            metrics_address,
            metrics,
            [&database_healthy, &coordination_healthy] {
                return database_healthy.load(std::memory_order_relaxed)
                       && coordination_healthy.load(std::memory_order_relaxed);
            });
        health_server.start();
        server.start();
        loop.loop();
        coordination_executor.stop();
        rooms.stop();
        storage_executor.stop();
    } catch (const std::exception& error) {
        std::cerr << "fatal server error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
