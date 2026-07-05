add_executable(hello_prism hello_prism.cpp)
target_link_libraries(hello_prism PRIVATE prism vio)

add_executable(task_api task_api.cpp)
target_link_libraries(task_api PRIVATE prism vio)

add_executable(hello_h2c hello_h2c.cpp)
target_link_libraries(hello_h2c PRIVATE prism vio)

add_executable(hello_h2tls hello_h2tls.cpp)
target_link_libraries(hello_h2tls PRIVATE prism vio)

add_executable(webapp webapp.cpp)
target_link_libraries(webapp PRIVATE prism vio)
add_custom_command(TARGET webapp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/webroot $<TARGET_FILE_DIR:webapp>/webroot)

add_executable(react_server react-app/server.cpp)
target_link_libraries(react_server PRIVATE prism vio)
