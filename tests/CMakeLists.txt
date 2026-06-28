add_library(doctest_main STATIC doctest-main.cpp)
target_link_libraries(doctest_main PUBLIC doctest)
target_compile_definitions(doctest_main PUBLIC DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

add_executable(prism_tests
        json_tests.cpp
        logging_tests.cpp
        router_tests.cpp
        http1_tests.cpp
        server_tests.cpp
)
target_link_libraries(prism_tests PRIVATE prism doctest_main doctest)

add_test(NAME prism_tests COMMAND prism_tests)
