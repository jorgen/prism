add_executable(hello_prism hello_prism.cpp)
target_link_libraries(hello_prism PRIVATE prism vio)

add_executable(task_api task_api.cpp)
target_link_libraries(task_api PRIVATE prism vio)
