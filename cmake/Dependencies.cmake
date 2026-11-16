# Dependency discovery.
#
# Works with either:
#   * system packages (apt/dnf/brew): found through the normal CMake search path
#   * vcpkg: configure with the "vcpkg" preset (toolchain file), which makes the
#     very same find_package() calls resolve against vcpkg_installed/.
# vcpkg.json is the canonical dependency manifest; nothing here depends on it
# being used locally.
#
# Every required dependency fails the configure step loudly if missing.

find_package(Threads REQUIRED)
find_package(spdlog 1.9 CONFIG REQUIRED)
find_package(nlohmann_json 3.10 CONFIG REQUIRED)
find_package(SQLite3 3.37 REQUIRED)

# Needed from Phase 2 on (network/transport/security). Resolved now so a missing
# dependency is reported at the start rather than mid-project.
find_package(Boost 1.80 CONFIG REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)

if(LANSYNC_BUILD_DESKTOP)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
    qt_standard_project_setup(REQUIRES 6.5)
endif()

if(LANSYNC_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

message(STATUS "LanSync dependencies:")
message(STATUS "  spdlog        ${spdlog_VERSION}")
message(STATUS "  nlohmann_json ${nlohmann_json_VERSION}")
message(STATUS "  SQLite3       ${SQLite3_VERSION}")
message(STATUS "  Boost         ${Boost_VERSION}")
message(STATUS "  OpenSSL       ${OPENSSL_VERSION}")
if(LANSYNC_BUILD_DESKTOP)
    message(STATUS "  Qt6           ${Qt6_VERSION}")
endif()
if(LANSYNC_BUILD_TESTS)
    message(STATUS "  GTest         ${GTest_VERSION}")
endif()
