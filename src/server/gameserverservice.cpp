#include "GameServerService.hpp"
#include "public.hpp"

#include <mutex>
#include <string>
#include <muduo/base/Logging.h>
#include <iostream>
using namespace std;
using namespace muduo;


GameServerService* GameServerService::getinstance()
{
    static GameServerService service;
    return &service;
}

GameServerService::GameServerService()
{
    _msgHandlerMap.insert({LOGIN_MSG, std::bind(&GameServerService::login, this, placeholders::_1, placeholders::_2, placeholders::_3)});
    _msgHandlerMap.insert({REG_MSG, std::bind(&GameServerService::reg, this, placeholders::_1, placeholders::_2, placeholders::_3)});
    _msgHandlerMap.insert({POINT_CHAT_MSG, std::bind(&GameServerService::pointchat, this, placeholders::_1, placeholders::_2, placeholders::_3)});
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
   int id = js["id"];
   string password = js["password"];
   User user = _userModel.query(id);
   if(user.getId() == id && user.getPassword() == password){
        if(user.getState() == "online"){
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 2;
            response["errmsg"] = "该账号已经登录，请重新输入";
            conn->send(response.dump());
            return;
        }else{

            {
                lock_guard<mutex> lock(_userConnMutex);
                _userOnlineMap.insert({id, conn});
            }
            

            user.setState("online");
            _userModel.updateState(user);

            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 0;
            response["name"] = user.getName();
            conn->send(response.dump());

            vector<pair<int, string>> offlinemessages = _offlineMsgModel.query(id);
            if(!offlinemessages.empty()){
                for(const auto& [senderid, msg] : offlinemessages){
                    json response;
                    response["senderid"] = senderid;
                    response["msg"] = msg;
                    conn->send(response.dump());
                }
                _offlineMsgModel.remove(id);
            }

        }  
    }else{
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "用户名或密码错误";
        conn->send(response.dump());
    }
}


void GameServerService::reg(const TcpConnectionPtr& conn, json& js, Timestamp time)
{
    string name = js["name"];
    string password = js["password"];
    User user;
    user.setName(name);
    user.setPassword(password);

    bool state = _userModel.insert(user);
    if(state){
        json response;
        response["id"] = user.getId();
        response["errno"] = 0;
        response["msgid"] = REG_MSG_ACK;
        conn->send(response.dump());
    }else{
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "注册失败";
        conn->send(response.dump()); 
    }

}


void GameServerService::usercloseexception(const TcpConnectionPtr& conn)
{
    User user;
    {
        lock_guard<mutex> lock(_userConnMutex);
        for(auto it = _userOnlineMap.begin(); it != _userOnlineMap.end(); it++){
            if(it->second == conn){
                user.setId(it->first);
                _userOnlineMap.erase(it);
                break;
            }
        }
    }
    if(user.getId() != -1){
        user.setState("offline");
        _userModel.updateState(user);   
    }
}

void GameServerService::pointchat(const TcpConnectionPtr& conn, json& js, Timestamp time)
{
    int targetuserid = js["targetuserid"];
    int senderid = js["id"];
    {
        lock_guard<mutex> lock(_userConnMutex);
        auto it = _userOnlineMap.find(targetuserid);
        if(it != _userOnlineMap.end()){
            json response;
            response["senderid"] = js["id"];
            response["msg"] = js["msg"];
            it->second->send(response.dump());
        }else{
            _offlineMsgModel.insert(targetuserid, senderid, js["msg"]);
            json response;
            response["msgid"] = POINT_CHAT_MSG_ACK;
            response["errno"] = 1;
            response["errmsg"] = "对方不在线";
            conn->send(response.dump());
        }
    }

}
