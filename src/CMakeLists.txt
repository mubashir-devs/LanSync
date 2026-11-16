configure_file(core/common/version.hpp.in
               ${CMAKE_BINARY_DIR}/generated/core/common/version.hpp @ONLY)

# Platform-specific code lives behind platform/platform.hpp.
if(WIN32)
    set(LANSYNC_PLATFORM_SRC platform/windows/platform_windows.cpp)
elseif(APPLE)
    set(LANSYNC_PLATFORM_SRC platform/macos/platform_macos.cpp)
else()
    set(LANSYNC_PLATFORM_SRC platform/linux/platform_linux.cpp)
endif()
if(NOT WIN32)
    list(APPEND LANSYNC_PLATFORM_SRC platform/posix/net_interfaces_posix.cpp)
endif()

# --- common: errors, logging, clock, platform abstraction
add_library(lansync_common STATIC
    core/common/logging.cpp
    core/common/crypto.cpp
    core/common/path_safety.cpp
    core/common/worker_pool.cpp
    ${LANSYNC_PLATFORM_SRC})
target_include_directories(lansync_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(lansync_common
    PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json Threads::Threads OpenSSL::Crypto
    PRIVATE lansync_options)

# --- network: protocol, identity, TLS transport, discovery
add_library(lansync_network STATIC
    network/protocol/message.cpp
    network/protocol/framing.cpp
    network/security/identity.cpp
    network/transport/connection.cpp
    network/transport/tls_server.cpp
    network/discovery/discovery.cpp)
target_link_libraries(lansync_network
    PUBLIC lansync_common Boost::headers OpenSSL::SSL OpenSSL::Crypto
    PRIVATE lansync_options)

# --- storage: SQLite wrapper + migrations
add_library(lansync_storage STATIC
    storage/database/database.cpp
    storage/database/migrator.cpp
    storage/database/sql.cpp
    storage/repositories/repositories.cpp
    ${LANSYNC_MIGRATIONS_CPP})
set_source_files_properties(${LANSYNC_MIGRATIONS_CPP} PROPERTIES GENERATED TRUE)
target_link_libraries(lansync_storage
    PUBLIC lansync_common SQLite::SQLite3
    PRIVATE lansync_options)

# --- filesystem: scanner + native watcher (platform selected here, nowhere else)
if(WIN32)
    set(LANSYNC_WATCHER_SRC platform/windows/watcher_windows.cpp)
elseif(APPLE)
    set(LANSYNC_WATCHER_SRC platform/macos/watcher_macos.cpp)
else()
    set(LANSYNC_WATCHER_SRC platform/linux/inotify_watcher.cpp)
endif()
add_library(lansync_fs STATIC
    filesystem/scanner/scanner.cpp
    ${LANSYNC_WATCHER_SRC})
target_link_libraries(lansync_fs PUBLIC lansync_common PRIVATE lansync_options)

# --- core: device management, sync, transfer, conflict, versioning (no Qt)
add_library(lansync_core STATIC
    core/application/settings.cpp
    core/device/pairing_state.cpp
    core/device/peer_session.cpp
    core/device/device_manager.cpp
    core/sync/version_vector.cpp
    core/sync/local_index.cpp
    core/sync/sync_state.cpp
    core/sync/sync_manager.cpp
    core/sync/sync_transfers.cpp
    core/sync/sync_apply.cpp
    core/sync/diff.cpp
    core/conflict/conflict.cpp
    core/versioning/version_store.cpp
    core/transfer/chunk_writer.cpp)
target_link_libraries(lansync_core
    PUBLIC lansync_storage lansync_network lansync_fs lansync_common
    PRIVATE lansync_options)

# --- application: composition root (no Qt, no UI)
add_library(lansync_app STATIC
    core/application/application.cpp
    core/application/engine.cpp)
target_link_libraries(lansync_app
    PUBLIC lansync_core lansync_storage lansync_network lansync_common
    PRIVATE lansync_options)
