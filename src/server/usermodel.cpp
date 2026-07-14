#include "usermodel.hpp"
#include "db.hpp"
#include <string>
#include <iostream>
using namespace std;


bool usermodel::insert(User &user){
    // 1.组装sql语句
    char sql[1024] = {0};
    sprintf(sql, "insert into user(name, password, state) values('%s', '%s', '%s')", user.getName().c_str(), user.getPassword().c_str(), user.getState().c_str());

    MySQL mysql;
    if(mysql.connect()){
        if(mysql.update(sql)){
            // 获取插入成功的用户数据生成的主键id
            user.setId(mysql_insert_id(mysql.getConnection()));
            return true;
        }
    }
    return false;
}

User usermodel::query(int id){
    char sql[1024] = {0};
    sprintf(sql, "select * from user where id = %d", id);

    MySQL mysql;
    if(mysql.connect()){
        MYSQL_RES *res = mysql.query(sql);
        if(res != nullptr){
            MYSQL_ROW row = mysql_fetch_row(res);
            if(row != nullptr){
                User user(atoi(row[0]), row[1], row[2], row[3]);
                mysql_free_result(res);
                return user;
            }
        }
    }
    return User();
}

bool usermodel::updateState(User user){
    char sql[1024] = {0};
    sprintf(sql, "update user set state = '%s' where id = %d", user.getState().c_str(), user.getId());

    MySQL mysql;
    if(mysql.connect()){
        if(mysql.update(sql)){
            return true;
        }
    }
    return false;
}

