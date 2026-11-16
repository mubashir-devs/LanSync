cmake_minimum_required(VERSION 3.25)

project(LanSync
    VERSION 0.1.0
    DESCRIPTION "Secure LAN file synchronization"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

option(LANSYNC_BUILD_DESKTOP "Build the Qt/QML desktop application" ON)
option(LANSYNC_BUILD_CLI "Build the headless CLI" ON)
option(LANSYNC_BUILD_TESTS "Build unit/integration tests" ON)
option(LANSYNC_ENABLE_SANITIZERS "Enable ASan + UBSan" OFF)
option(LANSYNC_ENABLE_TSAN "Enable ThreadSanitizer (exclusive with ASan)" OFF)
option(LANSYNC_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

include(cmake/Dependencies.cmake)
include(cmake/CompilerWarnings.cmake)
include(cmake/Migrations.cmake)

add_subdirectory(src)

if(LANSYNC_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()
if(LANSYNC_BUILD_DESKTOP)
    add_subdirectory(apps/desktop)
endif()
if(LANSYNC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(cmake/Packaging.cmake)   # after the targets it installs
