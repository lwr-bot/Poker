#include "GameServer.hpp"
#include "GameServerService.hpp"
#include <iostream>
#include <signal.h>
using namespace std;




void resetHandler(int sig){
    GameServerService::getinstance()->reset();
    exit(0);
}



int main(){

    signal(SIGINT, resetHandler);

    EventLoop loop;
    InetAddress listenAddr("127.0.0.1",6000);
    GameServer server(&loop, listenAddr, "GameServer");


    server.start();
    loop.loop();    

    return 0;
}