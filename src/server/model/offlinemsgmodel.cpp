#include "offlinemsgmodel.hpp"
#include "db.hpp"
#include <string>
#include <iostream>
#include <vector>
using namespace std;


bool offlinemsgmodel::insert(int targetuserid,int senderid, string msg) {
    char sql[1024] = {0};
    sprintf(sql, "insert into offlinemsg(targetuserid, senderid, message) values(%d, %d, '%s')", targetuserid, senderid, msg.c_str());

    MySQL mysql;
    if(mysql.connect()){
        mysql.update(sql);
        return true;
    }
    return false;
}

vector<pair<int, string>> offlinemsgmodel::query(int targetuserid) {
    char sql[1024] = {0};
    sprintf(sql, "select senderid, message from offlinemsg where targetuserid = %d", targetuserid);
    MySQL mysql;
    vector<pair<int, string>> messages;
    if(mysql.connect()){
        MYSQL_RES *res = mysql.query(sql);
        MYSQL_ROW row;
        while((row = mysql_fetch_row(res)) != nullptr){
            messages.push_back({stoi(row[0]), row[1]}); 
        }
        mysql_free_result(res);
    }
    return messages;
}

bool offlinemsgmodel::remove(int targetuserid) {
    char sql[1024] = {0};
    sprintf(sql, "delete from offlinemsg where targetuserid = %d", targetuserid);
    MySQL mysql;
    if(mysql.connect()){
        mysql.update(sql);
        return true;
    }
    return false;
}
