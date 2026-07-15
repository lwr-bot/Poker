#ifndef OFFLINEMSGMODEL_HPP
#define OFFLINEMSGMODEL_HPP
#include <string>
#include <vector>
using namespace std;


class offlinemsgmodel{
    public:
        // 存储离线消息
        bool insert(int targetuserid,int senderid, std::string msg);

        // 查询用户的离线消息
        vector<pair<int, string>> query(int targetuserid);

        // 删除用户的离线消息
        bool remove(int targetuserid);
};


#endif