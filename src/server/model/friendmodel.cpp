#include "friendmodel.hpp"
#include "db.hpp"
#include <string>
using namespace std;


void friendmodel::insertnewfriend(int userid, int targetuserid){
    char sql[1024] = {0};
    sprintf(sql, "insert into friend(id, friendid, state) values(%d, %d, 1)", userid, targetuserid);

    MySQL mysql;
    if(mysql.connect()){
        mysql.update(sql);
    }
}

void friendmodel::removefriend(int userid, int targetuserid){
    char sql[1024] = {0};
    sprintf(sql, "delete from friend where id = %d and friendid = %d", userid, targetuserid);

    MySQL mysql;
    if(mysql.connect()){
        mysql.update(sql);
    }
}

vector<int> friendmodel::queryfriend(int userid){
    char sql[1024] = {0};
    sprintf(sql, "select friendid from friend where id = %d", userid);

    MySQL mysql;
    vector<int> friendids;
    if(mysql.connect()){
        MYSQL_RES *res = mysql.query(sql);
        if(res != nullptr){
            MYSQL_ROW row;
            while((row = mysql_fetch_row(res)) != nullptr){
                int friendid = atoi(row[0]);
                friendids.push_back(friendid);
            }
            mysql_free_result(res);
        }
    }
    return friendids;
}


int friendmodel::querystate(int userid, int targetuserid)
{
    char sql[1024] = {0};
    sprintf(sql, "select state from friend where id = %d and friendid = %d", userid, targetuserid);

    MySQL mysql;
    if(mysql.connect()){
        MYSQL_RES *res = mysql.query(sql);
        if(res != nullptr){
            MYSQL_ROW row = mysql_fetch_row(res);
            if(row != nullptr){
                int state = atoi(row[0]);
                mysql_free_result(res);
                return state;
            }
        }
    }
    return 0;
}


void friendmodel::updatefriendstate(int userid, int targetuserid)
{
    char sql[1024] = {0};
    sprintf(sql, "update friend set state = 2 where id = %d and friendid = %d", userid, targetuserid);

    MySQL mysql;
    if(mysql.connect()){
        mysql.update(sql);
    }
}