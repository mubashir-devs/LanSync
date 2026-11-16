include(GoogleTest)

add_executable(lansync_unit_tests
    unit/test_result.cpp
    unit/test_database.cpp
    unit/test_migrator.cpp
    unit/test_logging.cpp
    unit/test_application.cpp
    unit/test_crypto.cpp
    unit/test_protocol.cpp
    unit/test_identity.cpp
    unit/test_discovery.cpp
    unit/test_transport.cpp
    unit/test_repositories.cpp
    unit/test_settings_pairing.cpp
    unit/test_path_safety.cpp
    unit/test_scanner.cpp
    unit/test_version_vector.cpp
    unit/test_sync_logic.cpp
    unit/test_transfer_versioning.cpp
    integration/test_pairing.cpp
    integration/test_watcher.cpp
    integration/test_sync_e2e.cpp)
target_link_libraries(lansync_unit_tests PRIVATE
    lansync_app lansync_core lansync_network lansync_options GTest::gtest GTest::gmock GTest::gtest_main)
target_include_directories(lansync_unit_tests PRIVATE unit)
gtest_discover_tests(lansync_unit_tests)

# Multi-process system test: two lansync-cli daemons sync real files over loopback.
if(LANSYNC_BUILD_CLI AND NOT WIN32)
    add_test(NAME system_two_processes
             COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/system_test.sh $<TARGET_FILE:lansync-cli>)
    set_tests_properties(system_two_processes PROPERTIES TIMEOUT 300 LABELS "system")
endif()
