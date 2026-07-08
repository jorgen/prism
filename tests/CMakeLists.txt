add_library(doctest_main STATIC doctest-main.cpp)
target_link_libraries(doctest_main PUBLIC doctest::doctest)
target_compile_definitions(doctest_main PUBLIC DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

add_executable(prism_tests
        json_tests.cpp
        logging_tests.cpp
        http_tests.cpp
        typed_tests.cpp
        content_tests.cpp
        router_tests.cpp
        http1_tests.cpp
        server_tests.cpp
        http2_frame_tests.cpp
        http2_hpack_tests.cpp
        http2_connection_tests.cpp
        http2_server_tests.cpp
        streaming_tests.cpp
        thread_state_tests.cpp
        static_files_tests.cpp
)
target_link_libraries(prism_tests PRIVATE prism doctest_main doctest::doctest)

add_test(NAME prism_tests COMMAND prism_tests)
