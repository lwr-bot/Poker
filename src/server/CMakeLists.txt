aux_source_directory(. SRC_LIST)
aux_source_directory(./db DB_LIST)

add_executable(GameServer ${SRC_LIST} ${DB_LIST})

link_directories(
    /home/iurnewgnail/project/build/release-install-cpp11/lib
)

target_link_libraries(GameServer 
/home/iurnewgnail/project/build/release-install-cpp11/lib/libmuduo_net.a
/home/iurnewgnail/project/build/release-install-cpp11/lib/libmuduo_base.a
mysqlclient
pthread)