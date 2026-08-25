#ifndef POKER_CLIENT_NETWORK_CLIENT_HPP
#define POKER_CLIENT_NETWORK_CLIENT_HPP

#include "poker/net/length_field_codec.hpp"
#include "poker.pb.h"

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantList>

#include <cstdint>
#include <string>

namespace poker::client {

class NetworkClient final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY authenticatedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(qulonglong userId READ userId NOTIFY identityChanged)
    Q_PROPERTY(qint64 walletChips READ walletChips NOTIFY identityChanged)
    Q_PROPERTY(QVariantList tables READ tables NOTIFY tablesChanged)
    Q_PROPERTY(QVariantList players READ players NOTIFY tableChanged)
    Q_PROPERTY(QVariantList board READ board NOTIFY tableChanged)
    Q_PROPERTY(qulonglong tableId READ tableId NOTIFY tableChanged)
    Q_PROPERTY(qulonglong handId READ handId NOTIFY tableChanged)
    Q_PROPERTY(qulonglong actingUserId READ actingUserId NOTIFY tableChanged)
    Q_PROPERTY(QString street READ street NOTIFY tableChanged)
    Q_PROPERTY(qint64 pot READ pot NOTIFY tableChanged)
    Q_PROPERTY(qint64 currentBet READ currentBet NOTIFY tableChanged)
    Q_PROPERTY(qint64 minimumRaise READ minimumRaise NOTIFY tableChanged)

public:
    explicit NetworkClient(QObject* parent = nullptr);

    bool connected() const noexcept;
    bool authenticated() const noexcept;
    QString status() const;
    qulonglong userId() const noexcept;
    qint64 walletChips() const noexcept;
    QVariantList tables() const;
    QVariantList players() const;
    QVariantList board() const;
    qulonglong tableId() const noexcept;
    qulonglong handId() const noexcept;
    qulonglong actingUserId() const noexcept;
    QString street() const;
    qint64 pot() const noexcept;
    qint64 currentBet() const noexcept;
    qint64 minimumRaise() const noexcept;

    Q_INVOKABLE void connectToLobby(const QString& host, quint16 port);
    Q_INVOKABLE void registerAccount(const QString& username, const QString& password);
    Q_INVOKABLE void login(const QString& username, const QString& password);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void refreshTables();
    Q_INVOKABLE void createTable(const QString& name,
                                 int maxPlayers,
                                 qint64 smallBlind,
                                 qint64 bigBlind,
                                 qint64 minimumBuyIn,
                                 qint64 maximumBuyIn);
    Q_INVOKABLE void joinTable(qulonglong tableId);
    Q_INVOKABLE void sitDown(int seat, qint64 buyIn);
    Q_INVOKABLE void setReady(bool ready);
    Q_INVOKABLE void act(const QString& action, qint64 targetCommitment = 0);
    Q_INVOKABLE void leaveTable();

signals:
    void connectedChanged();
    void authenticatedChanged();
    void statusChanged();
    void identityChanged();
    void tablesChanged();
    void tableChanged();

private:
    using Envelope = protocol::v1::Envelope;

    Envelope envelope(protocol::v1::MessageType type, bool sequenced = true);
    void send(Envelope message);
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);
    void process(const Envelope& message);
    void processSnapshot(const protocol::v1::TableSnapshot& snapshot);
    void routeToGame(const QString& endpoint, QString ticket, qulonglong tableId);
    void scheduleReconnect();
    void setStatus(QString value);
    static QString cardText(const protocol::v1::Card& card);
    static QString streetText(protocol::v1::Street street);

    QTcpSocket socket_;
    QTimer reconnect_timer_;
    QTimer heartbeat_timer_;
    net::LengthFieldCodec codec_;
    QString status_{"Disconnected"};
    QString lobby_host_{"127.0.0.1"};
    quint16 lobby_port_{6000};
    QString game_host_;
    quint16 game_port_{0};
    QString session_token_;
    QString pending_ticket_;
    bool routing_to_game_{false};
    bool authenticate_on_connect_{false};
    bool authenticated_{false};
    bool seated_{false};
    bool intentional_disconnect_{false};
    bool reconnect_after_auth_{false};
    int reconnect_attempt_{0};
    std::uint64_t next_request_id_{1};
    std::uint64_t client_sequence_{0};
    std::uint64_t lobby_sequence_{0};
    std::uint64_t game_sequence_{0};
    std::uint64_t user_id_{0};
    std::int64_t wallet_chips_{0};
    QVariantList tables_;
    QVariantList players_;
    QVariantList board_;
    std::uint64_t table_id_{0};
    std::uint64_t hand_id_{0};
    std::uint64_t server_sequence_{0};
    std::uint64_t acting_user_id_{0};
    QString street_{"Waiting"};
    std::int64_t pot_{0};
    std::int64_t current_bet_{0};
    std::int64_t minimum_raise_{0};
};

}  // namespace poker::client

#endif
