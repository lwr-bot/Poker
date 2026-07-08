#include "GameServer.hpp"
#include <iostream>
using namespace std;

int main(){
    EventLoop loop;
    InetAddress listenAddr("127.0.0.1",6000);
    GameServer server(&loop, listenAddr, "GameServer");


    server.start();
    loop.loop();    

    return 0;
}