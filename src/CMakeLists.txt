set(public_headers
        prism/prism.h
        prism/status.h
        prism/error.h
        prism/http.h
        prism/json.h
        prism/logging.h
        prism/params.h
        prism/router.h
        prism/app.h
        prism/server_options.h
)
set(sources
        prism/http.cpp
        prism/logging.cpp
        prism/router.cpp
        prism/app.cpp
        prism/version.cpp
        prism/detail/http1.cpp
        prism/detail/server.cpp
)

add_library(prism ${public_headers} ${sources})
add_library(prism::prism ALIAS prism)

generate_export_header(prism
        BASE_NAME prism
        EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/prism/prism_export.h
)

target_compile_features(prism PUBLIC cxx_std_23)
target_compile_definitions(prism PRIVATE PRISM_VERSION="${PROJECT_VERSION}")

target_link_libraries(prism PUBLIC vio structify::structify PRIVATE llhttp::llhttp)

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
