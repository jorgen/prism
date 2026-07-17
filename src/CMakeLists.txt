set(public_headers
        prism/prism.h
        prism/status.h
        prism/error.h
        prism/http.h
        prism/json.h
        prism/codec.h
        prism/content.h
        prism/render.h
        prism/logging.h
        prism/params.h
        prism/router.h
        prism/app.h
        prism/websocket.h
        prism/reverse_proxy.h
        prism/server_options.h
        prism/static_files.h
        prism/acme/jose.h
        prism/acme/csr.h
        prism/acme/client.h
        prism/acme/manager.h
)
set(sources
        prism/acme/jose.cpp
        prism/acme/csr.cpp
        prism/acme/client.cpp
        prism/acme/manager.cpp
        prism/http.cpp
        prism/content.cpp
        prism/logging.cpp
        prism/router.cpp
        prism/app.cpp
        prism/reverse_proxy.cpp
        prism/static_files.cpp
        prism/version.cpp
        prism/detail/http1.cpp
        prism/detail/server.cpp
        prism/detail/http2/frame.cpp
        prism/detail/http2/hpack.cpp
        prism/detail/http2/connection.cpp
        prism/detail/http2/server.cpp
        prism/detail/websocket/frame.cpp
        prism/detail/websocket/handshake.cpp
        prism/detail/websocket/client.cpp
)

add_library(prism ${public_headers} ${sources})
add_library(prism::prism ALIAS prism)

generate_export_header(prism
        BASE_NAME prism
        EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/prism/prism_export.h
)

target_compile_features(prism PUBLIC cxx_std_23)
target_compile_definitions(prism PRIVATE PRISM_VERSION="${PROJECT_VERSION}")

target_link_libraries(prism PUBLIC vio::vio structify::structify PRIVATE llhttp::llhttp)

target_include_directories(prism
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(prism PROPERTIES
        PUBLIC_HEADER "${public_headers}"
)

setWarningFlagsForTarget(prism)
