add_executable(hello_prism hello_prism.cpp)
target_link_libraries(hello_prism PRIVATE prism vio::vio)

add_executable(task_api task_api.cpp)
target_link_libraries(task_api PRIVATE prism vio::vio)

add_executable(hello_h2c hello_h2c.cpp)
target_link_libraries(hello_h2c PRIVATE prism vio::vio)

add_executable(hello_websocket hello_websocket.cpp)
target_link_libraries(hello_websocket PRIVATE prism vio::vio)

add_executable(upload_stream upload_stream.cpp)
target_link_libraries(upload_stream PRIVATE prism vio::vio)

add_executable(per_thread_state per_thread_state.cpp)
target_link_libraries(per_thread_state PRIVATE prism vio::vio)

add_executable(hello_h2tls hello_h2tls.cpp)
target_link_libraries(hello_h2tls PRIVATE prism vio::vio)

add_executable(webapp webapp.cpp)
target_link_libraries(webapp PRIVATE prism vio::vio)
add_custom_command(TARGET webapp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/webroot $<TARGET_FILE_DIR:webapp>/webroot)

add_executable(react_server react-app/server.cpp)
target_link_libraries(react_server PRIVATE prism vio::vio)
