add_executable(hello_prism hello_prism.cpp)
target_link_libraries(hello_prism PRIVATE prism vio)

add_executable(task_api task_api.cpp)
target_link_libraries(task_api PRIVATE prism vio)

add_executable(hello_h2c hello_h2c.cpp)
target_link_libraries(hello_h2c PRIVATE prism vio)

add_executable(hello_h2tls hello_h2tls.cpp)
target_link_libraries(hello_h2tls PRIVATE prism vio)
