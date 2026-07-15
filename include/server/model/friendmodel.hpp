#ifndef FRIEND_MODEL_HPP
#define FRIEND_MODEL_HPP

#include <vector>
#include "user.hpp"

class friendmodel{
public:
    void insertnewfriend(int userid, int targetuserid);

    void removefriend(int userid, int targetuserid);

    vector<int> queryfriend(int userid);

    int querystate(int userid, int targetuserid);

    void updatefriendstate(int userid, int targetuserid);
};

#endif
