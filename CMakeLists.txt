cmake_minimum_required(VERSION 3.30)
find_program(CCACHE_PROGRAM ccache)
if (CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif ()

project(prism VERSION 0.0.1 DESCRIPTION "A lean REST service library built on vio and structify" LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

include(FetchContent)
FetchContent_Declare(cmake-dep
    GIT_REPOSITORY https://github.com/jorgen/cmake-dep.git
    GIT_TAG f61ffba
)
FetchContent_MakeAvailable(cmake-dep)
list(APPEND CMAKE_MODULE_PATH "${cmake-dep_SOURCE_DIR}/cmake")
include(CmDepMain)

include(GNUInstallDirs)
include(GenerateExportHeader)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (WIN32)
    add_compile_definitions(NOMINMAX=1 WIN32_LEAN_AND_MEAN=1 VC_EXTRALEAN=1)
endif ()
if (MSVC)
    string(REGEX REPLACE "/GR" "/GR-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/EHsc" "/EHs-c-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_HAS_EXCEPTIONS=0")
else ()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")
endif ()

option(PRISM_BUILD_TESTS "Build tests" ON)
option(PRISM_BUILD_EXAMPLES "Build examples" ON)
option(BUILD_SHARED_LIBS "Build shared libs" OFF)

option(PRISM_USE_SYSTEM_VIO "Use a pre-built vio via find_package instead of the bundled fetch" OFF)
option(PRISM_USE_SYSTEM_STRUCTIFY "Use a pre-built structify via find_package instead of the bundled fetch" OFF)
option(PRISM_USE_SYSTEM_DOCTEST "Use a pre-built doctest via find_package instead of the bundled fetch" OFF)
option(PRISM_USE_SYSTEM_LLHTTP "Use a pre-built llhttp via find_package instead of the bundled fetch" OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
find_package(Threads)

include(CMake/SetWarnings.cmake)

set(CMDEP_PACKAGES_FILE "${CMAKE_CURRENT_SOURCE_DIR}/CMake/3rdPartyPackages.cmake")
CmDepFetch()

include(CMake/Build3rdParty.cmake)
Build3rdParty()
include(CMake/AddSystemHeadersToTarget.cmake)
include(CMake/CopyDllForTarget.cmake)

add_subdirectory(src)

if (PRISM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (PRISM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()
