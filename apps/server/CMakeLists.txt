if(NOT Protobuf_FOUND)
    message(FATAL_ERROR "POKER_BUILD_SERVER requires Protobuf and protoc")
endif()

find_path(MUDUO_INCLUDE_DIR muduo/net/TcpServer.h REQUIRED)
find_library(MUDUO_NET_LIBRARY NAMES muduo_net REQUIRED)
find_library(MUDUO_BASE_LIBRARY NAMES muduo_base REQUIRED)
find_library(MUDUO_HTTP_LIBRARY NAMES muduo_http REQUIRED)

add_executable(poker_server
    main.cpp
    health_http_server.cpp
    poker_tcp_server.cpp
    protocol_service.cpp
)

target_include_directories(poker_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MUDUO_INCLUDE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/../..
)

target_link_libraries(poker_server
    PRIVATE
        poker_domain
        poker_net
        poker_config
        poker_security
        poker_application
        poker_cluster
        poker_infrastructure
        poker_observability
        poker_protocol
        ${MUDUO_NET_LIBRARY}
        ${MUDUO_HTTP_LIBRARY}
        ${MUDUO_BASE_LIBRARY}
        Threads::Threads
)

if(MSVC)
    target_compile_options(poker_server PRIVATE /W4 /permissive-)
else()
    target_compile_options(poker_server PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
