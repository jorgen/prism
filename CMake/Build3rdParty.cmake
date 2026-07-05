macro(Build3rdParty)
    # prism's own code is built with -fno-exceptions/-fno-rtti (see top-level
    # CMakeLists). Some third-party subdirectories (vio's deps, doctest) need
    # exceptions enabled to compile, so strip the flags here and restore them
    # after the third-party targets are added.
    set(_SAVED_CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    string(REPLACE "-fno-exceptions" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "-fno-rtti" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "/EHs-c-" "/EHsc" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "/GR-" "/GR" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "-D_HAS_EXCEPTIONS=0" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

    # vio: async I/O runtime (libuv + LibreSSL + coroutines). Built static, or
    # consumed pre-built via find_package when PRISM_USE_SYSTEM_VIO is set.
    if (PRISM_USE_SYSTEM_VIO)
        find_package(vio CONFIG REQUIRED)
    else ()
        set(VIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(VIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(VIO_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(VIO_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory(${vio_SOURCE_DIR} SYSTEM)
        unset(VIO_BUILD_TESTS CACHE)
        unset(VIO_BUILD_EXAMPLES CACHE)
        unset(VIO_BUILD_SHARED CACHE)
        unset(VIO_INSTALL CACHE)
        target_include_directories(vio PUBLIC ${vio_SOURCE_DIR}/src)
    endif ()

    # structify: header-only JSON<->struct. Provides structify::structify.
    if (PRISM_USE_SYSTEM_STRUCTIFY)
        find_package(structify CONFIG REQUIRED)
    else ()
        add_subdirectory(${structify_SOURCE_DIR} SYSTEM)
    endif ()

    # doctest: testing framework. Only needed for the test build. vio (when
    # bundled) may already create the `doctest` target, so guard on it.
    if (PRISM_BUILD_TESTS)
        if (PRISM_USE_SYSTEM_DOCTEST)
            find_package(doctest CONFIG REQUIRED)
        elseif (NOT TARGET doctest)
            set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
            add_subdirectory(${doctest_SOURCE_DIR} SYSTEM)
            unset(DOCTEST_WITH_TESTS CACHE)
        endif ()
    endif ()

    # llhttp: Node.js's HTTP/1.1 parser (pure C). Built static; the release
    # tarball ships pre-generated sources and an `llhttp::llhttp` alias.
    if (PRISM_USE_SYSTEM_LLHTTP)
        find_package(llhttp CONFIG REQUIRED)
    else ()
        set(LLHTTP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(LLHTTP_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
        add_subdirectory(${llhttp_SOURCE_DIR} SYSTEM)
        unset(LLHTTP_BUILD_SHARED_LIBS CACHE)
        unset(LLHTTP_BUILD_STATIC_LIBS CACHE)
    endif ()

    # Restore original flags for prism's own code.
    set(CMAKE_CXX_FLAGS ${_SAVED_CMAKE_CXX_FLAGS})
endmacro()
