#ifndef GameServerService_H
#define GameServerService_H

#include <muduo/net/TcpConnection.h>
#include <unordered_map>
#include <functional>
using namespace muduo;
using namespace muduo::net;
using namespace std;


#include "json.hpp"
using json = nlohmann::json;


using MsgHandler = std::function<void(const TcpConnectionPtr&, json&, Timestamp)>;

class GameServerService{
public: 
    static GameServerService* instance();

    void login(const TcpConnectionPtr& conn, json& js, Timestamp time);

    void reg(const TcpConnectionPtr& conn, json& js, Timestamp time);

    MsgHandler getHandler(int msgid);
private:

    GameServerService();

    unordered_map<int, MsgHandler> _msgHandlerMap;
};

#endif