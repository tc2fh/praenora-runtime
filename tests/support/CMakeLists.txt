# praenora_test_support: helpers for every test executable (test_support.hpp; json_compare.hpp,
# JSON comparison with a tolerance; scoped_environment.hpp, environment variables set for a scope;
# the header-only temp_dir.hpp, temporary directories and a guard that gives a path its
# permissions back; module_fixtures.hpp for tests that use the fixture modules,
# provider_fixtures.hpp for tests that use the fixture providers and
# directory_checks.hpp for tests of the directories that the files of a run name). It uses
# only the standard library, nlohmann/json and GoogleTest, so black-box tests (linked to the
# shared library) can use it too.
# Its compile definitions give tests the locations of sources, fixtures and built modules.
add_library(praenora_test_support STATIC
    test_support.hpp
    test_support.cpp
    json_compare.hpp
    json_compare.cpp
    temp_dir.hpp
    module_fixtures.hpp
    provider_fixtures.hpp
    directory_checks.hpp
    scoped_environment.hpp
    scoped_environment.cpp)
target_include_directories(praenora_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# json_compare.cpp forwards to the runtime's header-only comparison (core/json_near.hpp), the one
# implementation of its rule; the include directory stays private, so the tests that use this
# library see no internal header of the runtime.
target_include_directories(praenora_test_support PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(praenora_test_support PUBLIC GTest::gtest nlohmann_json::nlohmann_json)
target_compile_definitions(praenora_test_support PUBLIC
    "PRAENORA_TEST_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
    "PRAENORA_TEST_BINARY_DIR=\"${PROJECT_BINARY_DIR}\""
    "PRAENORA_TEST_FIXTURES_DIR=\"${PROJECT_SOURCE_DIR}/tests/fixtures\""
    "PRAENORA_TEST_CATALOG_DIR=\"${PROJECT_SOURCE_DIR}/tests/fixtures/catalog\""
    "PRAENORA_TEST_MODULES_DIR=\"${PRAENORA_TEST_MODULES_DIR}\""
    "PRAENORA_TEST_PROVIDERS_DIR=\"${PRAENORA_TEST_PROVIDERS_DIR}\""
    "PRAENORA_EXAMPLE_MODULES_DIR=\"${PRAENORA_EXAMPLE_MODULES_DIR}\"")
praenora_target_warnings(praenora_test_support)

# praenora_whitebox_support: helpers for white-box tests, which see the runtime's internal headers
# and dependencies (LogCapture; the header-only instance_fixtures.hpp for tests that drive module
# instances, validation_fixtures.hpp for tests of the validation sequence and descriptor_fixtures.hpp
# for those of the target descriptor). It never links the
# object library itself; praenora_add_gtest(WHITEBOX) does, together with this library.
add_library(praenora_whitebox_support STATIC
    log_capture.hpp
    log_capture.cpp
    instance_fixtures.hpp
    validation_fixtures.hpp
    descriptor_fixtures.hpp)
target_include_directories(praenora_whitebox_support PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "$<TARGET_PROPERTY:praenora_runtime_objects,INTERFACE_INCLUDE_DIRECTORIES>")
target_link_libraries(praenora_whitebox_support PUBLIC praenora_runtime_deps)
# Its code is linked next to the runtime's objects, not against the shared library.
target_compile_definitions(praenora_whitebox_support PUBLIC PRAENORA_RUNTIME_STATIC_DEFINE)
praenora_target_warnings(praenora_whitebox_support)
