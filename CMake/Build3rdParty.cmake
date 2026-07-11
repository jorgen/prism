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

    # vio: async I/O runtime (libuv + LibreSSL + coroutines). Built static (with
    # its src/ dir re-exposed as a PUBLIC include, since vio marks headers PRIVATE
    # upstream), or consumed pre-built via find_package when PRISM_USE_SYSTEM_VIO is set.
    # SKIP_IF_TARGET lets a parent project that already added vio (e.g. photon
    # consuming prism) win, so vio is add_subdirectory'd exactly once.
    CmDepAddPackage(vio CONFIG PUBLIC_INCLUDE src SKIP_IF_TARGET vio
        OPTIONS VIO_BUILD_TESTS=OFF VIO_BUILD_EXAMPLES=OFF VIO_BUILD_SHARED=OFF VIO_INSTALL=OFF)

    # structify: header-only JSON<->struct. Provides structify::structify.
    CmDepAddPackage(structify CONFIG SKIP_IF_TARGET structify)

    # doctest: testing framework. Only needed for the test build. vio (when
    # bundled) may already create the `doctest` target, so guard on it.
    if (PRISM_BUILD_TESTS)
        CmDepAddPackage(doctest CONFIG SKIP_IF_TARGET doctest OPTIONS DOCTEST_WITH_TESTS=OFF)
    endif ()

    # llhttp: Node.js's HTTP/1.1 parser (pure C). Built static; the release
    # tarball ships pre-generated sources and an `llhttp::llhttp` alias.
    CmDepAddPackage(llhttp CONFIG OPTIONS LLHTTP_BUILD_SHARED_LIBS=OFF LLHTTP_BUILD_STATIC_LIBS=ON)

    # Restore original flags for prism's own code.
    set(CMAKE_CXX_FLAGS ${_SAVED_CMAKE_CXX_FLAGS})
endmacro()
