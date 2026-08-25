cmake_minimum_required(VERSION 3.16)

project(PokerServer VERSION 0.1.0 LANGUAGES CXX)

option(POKER_BUILD_TESTS "Build the self-contained test suite" ON)
option(POKER_BUILD_SERVER "Build the Muduo server adapters" OFF)
option(POKER_BUILD_CLIENT "Build the Qt 6 Quick desktop client" OFF)
option(POKER_BUILD_FUZZERS "Build libFuzzer protocol targets" OFF)
option(POKER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(POKER_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(POKER_ENABLE_COVERAGE "Enable GCC coverage instrumentation" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(POKER_ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "POKER_ENABLE_COVERAGE currently requires GCC")
    endif()
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

if(POKER_ENABLE_SANITIZERS AND POKER_ENABLE_TSAN)
    message(FATAL_ERROR "Address/Undefined sanitizers and ThreadSanitizer require separate builds")
endif()

if(POKER_ENABLE_SANITIZERS AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(POKER_ENABLE_TSAN AND NOT MSVC)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

add_library(poker_domain
    src/poker/domain/card.cpp
    src/poker/domain/deck.cpp
    src/poker/domain/hand_evaluator.cpp
    src/poker/domain/table.cpp
)

target_include_directories(poker_domain
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(poker_domain PRIVATE /W4 /permissive-)
else()
    target_compile_options(poker_domain PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_library(poker_net
    src/poker/net/length_field_codec.cpp
    src/poker/net/token_bucket.cpp
)
target_include_directories(poker_net PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

add_library(poker_config
    src/poker/config/server_config.cpp
)
target_include_directories(poker_config PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

add_library(poker_observability
    src/poker/observability/metrics.cpp
)
target_include_directories(poker_observability PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

add_library(poker_security
    src/poker/security/auth_service.cpp
)
target_include_directories(poker_security PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

add_library(poker_storage_memory
    src/poker/storage/in_memory_account_store.cpp
    src/poker/storage/in_memory_game_store.cpp
)
target_include_directories(poker_storage_memory PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

add_library(poker_cluster
    src/poker/cluster/in_memory_node_registry.cpp
    src/poker/cluster/lobby_router.cpp
    src/poker/cluster/node_failure_reaper.cpp
)
target_include_directories(poker_cluster PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(poker_cluster PUBLIC poker_security)

find_package(Threads REQUIRED)
add_library(poker_application
    src/poker/application/blocking_executor.cpp
    src/poker/application/idempotency_cache.cpp
    src/poker/application/room_executor.cpp
    src/poker/application/room_manager.cpp
)
target_include_directories(poker_application PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(poker_application PUBLIC poker_domain Threads::Threads)

foreach(target poker_net poker_config poker_observability poker_security poker_storage_memory poker_cluster poker_application)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endforeach()

find_package(Protobuf QUIET)
if(Protobuf_FOUND)
    protobuf_generate_cpp(POKER_PROTO_SOURCES POKER_PROTO_HEADERS protocol/poker.proto)
    add_library(poker_protocol ${POKER_PROTO_SOURCES} ${POKER_PROTO_HEADERS})
    target_include_directories(poker_protocol PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(poker_protocol PUBLIC protobuf::libprotobuf)
endif()

if(POKER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(POKER_BUILD_SERVER)
    add_subdirectory(src/poker/infrastructure)
    add_subdirectory(apps/server)
endif()

if(POKER_BUILD_CLIENT)
    if(NOT Protobuf_FOUND)
        message(FATAL_ERROR "POKER_BUILD_CLIENT requires Protobuf and protoc")
    endif()
    add_subdirectory(apps/client)
endif()

if(POKER_BUILD_FUZZERS)
    if(NOT Protobuf_FOUND)
        message(FATAL_ERROR "POKER_BUILD_FUZZERS requires Protobuf and protoc")
    endif()
    add_subdirectory(fuzz)
endif()
