#ifndef GAMESERVER_H
#define GAMESERVER_H

#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>

using namespace muduo;
using namespace muduo::net;

class GameServer{
public:
    GameServer(EventLoop* loop,
            const InetAddress& listenAddr,
            const string& nameArg);


    void start();

private:
    void onConnection(const TcpConnectionPtr& conn);
    void onMessage(const TcpConnectionPtr& conn,
                Buffer* buf,
                Timestamp time);

    TcpServer _server;
    EventLoop *_loop;

};


#endif