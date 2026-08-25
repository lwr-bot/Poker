add_executable(poker_tests
    test_main.cpp
)

target_link_libraries(poker_tests
    PRIVATE
        poker_domain
        poker_net
        poker_application
        poker_security
        poker_storage_memory
        poker_cluster
        poker_observability
)

if(MSVC)
    target_compile_options(poker_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(poker_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_test(NAME poker_tests COMMAND poker_tests)
