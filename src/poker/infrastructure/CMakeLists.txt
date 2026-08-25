find_path(MYSQL_INCLUDE_DIR mysql/mysql.h REQUIRED)
find_library(MYSQL_CLIENT_LIBRARY NAMES mysqlclient mariadb REQUIRED)
find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
find_library(SODIUM_LIBRARY NAMES sodium REQUIRED)
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h REQUIRED)
find_library(HIREDIS_LIBRARY NAMES hiredis REQUIRED)

add_library(poker_infrastructure
    mysql_connection_pool.cpp
    mysql_account_store.cpp
    mysql_game_store.cpp
    sodium_crypto_provider.cpp
    hiredis_node_registry.cpp
)

target_include_directories(poker_infrastructure
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${MYSQL_INCLUDE_DIR}
        ${SODIUM_INCLUDE_DIR}
        ${HIREDIS_INCLUDE_DIR}
)

target_link_libraries(poker_infrastructure
    PUBLIC
        poker_config
        poker_security
        poker_cluster
    PRIVATE
        ${MYSQL_CLIENT_LIBRARY}
        ${SODIUM_LIBRARY}
        ${HIREDIS_LIBRARY}
        Threads::Threads
)

if(MSVC)
    target_compile_options(poker_infrastructure PRIVATE /W4 /permissive-)
else()
    target_compile_options(poker_infrastructure PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
