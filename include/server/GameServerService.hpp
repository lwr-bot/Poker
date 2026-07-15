#ifndef GameServerService_H
#define GameServerService_H

#include <mutex>
#include <muduo/net/TcpConnection.h>
#include <unordered_map>
#include <functional>
#include "usermodel.hpp"
#include "offlinemsgmodel.hpp"
#include "friendmodel.hpp"

using namespace muduo;
using namespace muduo::net;
using namespace std;


#include "json.hpp"
using json = nlohmann::json;


using MsgHandler = std::function<void(const TcpConnectionPtr&, json&, Timestamp)>;

class GameServerService{
public: 
    static GameServerService* getinstance();

    void login(const TcpConnectionPtr& conn, json& js, Timestamp time);

    void reg(const TcpConnectionPtr& conn, json& js, Timestamp time);

    void pointchat(const TcpConnectionPtr& conn, json& js, Timestamp time);

    void applyaddfriend(const TcpConnectionPtr& conn, json& js, Timestamp time);

    void removefriend(const TcpConnectionPtr& conn, json& js, Timestamp time);

    MsgHandler getHandler(int msgid);

    void usercloseexception(const TcpConnectionPtr& conn);

    void reset();
private:
    GameServerService();

    unordered_map<int, MsgHandler> _msgHandlerMap;

    unordered_map<int, TcpConnectionPtr> _userOnlineMap;

    mutex _userConnMutex;

    usermodel _userModel;

    offlinemsgmodel _offlineMsgModel;

    friendmodel _friendModel;
};

#endif