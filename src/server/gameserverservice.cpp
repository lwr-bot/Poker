#include "GameServerService.hpp"
#include "public.hpp"
#include <string>
#include <muduo/base/Logging.h>
#include <iostream>
using namespace std;
using namespace muduo;


GameServerService* GameServerService::instance()
{
    static GameServerService service;
    return &service;
}

GameServerService::GameServerService()
{
    _msgHandlerMap.insert({LOGIN_MSG, std::bind(&GameServerService::login, this, placeholders::_1, placeholders::_2, placeholders::_3)});
    _msgHandlerMap.insert({REG_MSG, std::bind(&GameServerService::reg, this, placeholders::_1, placeholders::_2, placeholders::_3)});

}

MsgHandler GameServerService::getHandler(int msgid)
{
    auto it = _msgHandlerMap.find(msgid);
    if(it == _msgHandlerMap.end()){
        LOG_ERROR << "msgid: " << msgid << " can not find handler!";
        string errMsg = "msgid: " + to_string(msgid) + " can not find handler!";
        throw std::runtime_error(errMsg);
        return nullptr;
    }else{
        return it->second;
    }
}

void GameServerService::login(const TcpConnectionPtr& conn, json& js, Timestamp time)
{
    cout << "login service" << endl;
}

void GameServerService::reg(const TcpConnectionPtr& conn, json& js, Timestamp time)
{
    cout << "reg service" << endl;
}

