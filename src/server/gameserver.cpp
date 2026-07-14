#include "GameServer.hpp"
#include "json.hpp"
#include "GameServerService.hpp"

#include <functional>
#include <iostream>
#include <string>
using namespace placeholders;
using namespace std;
using json = nlohmann::json;

GameServer::GameServer(EventLoop *loop,
                       const InetAddress &listenAddr,
                       const string &nameArg) 
    : _server(loop, listenAddr, nameArg),_loop(loop)
{
    _server.setConnectionCallback(
        std::bind(&GameServer::onConnection, this, std::placeholders::_1));


    _server.setMessageCallback(
        std::bind(&GameServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));


    _server.setThreadNum(4);
}


void GameServer::start()
{
    _server.start();
}

void GameServer::onConnection(const TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        cout << "new connection [" << conn->name() << "] from " << conn->peerAddress().toIpPort() << endl;
    }
    else
    {
        GameServerService::getinstance()->usercloseexception(conn);
        conn->shutdown();
        cout << "connection [" << conn->name() << "] is down" << endl;
    }
}

void GameServer::onMessage(const TcpConnectionPtr &conn, Buffer *buffer, Timestamp time)
{
    string buf = buffer->retrieveAllAsString();

    json js = json::parse(buf);
    auto msghandler = GameServerService::getinstance()->getHandler(js["msgid"].get<int>());
    if (msghandler)
    {
        msghandler(conn, js, time);
    }


}