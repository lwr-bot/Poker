find_package(Qt6 6.4 REQUIRED COMPONENTS Quick Network)

qt_standard_project_setup(REQUIRES 6.4)

qt_add_executable(poker_client
    main.cpp
    network_client.cpp
    network_client.hpp
)

qt_add_qml_module(poker_client
    URI Poker.Client
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_include_directories(poker_client
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/../..
)

target_link_libraries(poker_client
    PRIVATE
        Qt6::Quick
        Qt6::Network
        poker_net
        poker_protocol
)

if(MSVC)
    target_compile_options(poker_client PRIVATE /W4 /permissive-)
else()
    target_compile_options(poker_client PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
