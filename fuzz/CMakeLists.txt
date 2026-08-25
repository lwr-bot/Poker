if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "POKER_BUILD_FUZZERS requires Clang/libFuzzer")
endif()

add_executable(protocol_fuzzer protocol_fuzzer.cpp)
target_include_directories(protocol_fuzzer PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR})
target_link_libraries(protocol_fuzzer PRIVATE poker_net poker_protocol)
target_compile_options(protocol_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
target_link_options(protocol_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
