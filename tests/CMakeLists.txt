include(GoogleTest)

add_subdirectory(support)
# Fixture modules, built into ${PRAENORA_TEST_MODULES_DIR}/<name>/ for the tests to load.
add_subdirectory(fixtures/modules)
# Fixture service providers, built into ${PRAENORA_TEST_PROVIDERS_DIR}/<name>/.
add_subdirectory(fixtures/providers)

# praenora_add_module() records where it generates each manifest, and where it copies a module's
# test vectors; stop early if it did not.
foreach(_praenora_property IN ITEMS PRAENORA_MODULE_MANIFEST PRAENORA_MODULE_DIR)
    get_target_property(_praenora_value fx_echo ${_praenora_property})
    if(NOT _praenora_value)
        message(FATAL_ERROR "praenora_add_module() did not set ${_praenora_property} on fx_echo")
    endif()
endforeach()
get_target_property(_praenora_value fx_nondeterministic PRAENORA_MODULE_TEST_VECTORS_PATH)
if(NOT _praenora_value MATCHES "/fx-nondeterministic/vectors\\.json$")
    message(FATAL_ERROR "praenora_add_module() set PRAENORA_MODULE_TEST_VECTORS_PATH of "
        "fx_nondeterministic to '${_praenora_value}', not the path of its copied vectors.json")
endif()

# Search-path layouts for the discovery tests: the manifest templates below
# tests/fixtures/manifests/layouts/ are generated into PRAENORA_TEST_LAYOUTS_DIR, keeping their
# directory structure. Each describes fx-echo; library.path is the relative path from the generated
# manifest to fx-echo's library. The variables of a template are the versions of the build
# (@PRAENORA_ABI_MAJOR@, ...), @PRAENORA_LAYOUT_LIBRARY@, the platform being built for
# (@PRAENORA_LAYOUT_OS@, @PRAENORA_LAYOUT_ARCH@) and another platform (@PRAENORA_LAYOUT_OTHER_OS@,
# @PRAENORA_LAYOUT_OTHER_ARCH@).
set(PRAENORA_TEST_LAYOUTS_DIR "${PROJECT_BINARY_DIR}/test-layouts")
get_property(_praenora_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_praenora_multi_config)
    string(APPEND PRAENORA_TEST_LAYOUTS_DIR "/$<CONFIG>")
endif()
set(PRAENORA_LAYOUT_OS "${PRAENORA_PLATFORM_OS}")
set(PRAENORA_LAYOUT_ARCH "${PRAENORA_PLATFORM_ARCH}")
if(PRAENORA_PLATFORM_OS STREQUAL "windows")
    set(PRAENORA_LAYOUT_OTHER_OS "linux")
else()
    set(PRAENORA_LAYOUT_OTHER_OS "windows")
endif()
set(PRAENORA_LAYOUT_OTHER_ARCH "x86_64")
set(_praenora_layouts_source "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/manifests/layouts")
file(GLOB_RECURSE _praenora_layout_templates CONFIGURE_DEPENDS
    RELATIVE "${_praenora_layouts_source}"
    "${_praenora_layouts_source}/*.pmm.json.in")
foreach(_praenora_template IN LISTS _praenora_layout_templates)
    string(REGEX REPLACE "\\.in$" "" _praenora_relative "${_praenora_template}")
    set(_praenora_output "${PRAENORA_TEST_LAYOUTS_DIR}/${_praenora_relative}")
    get_filename_component(_praenora_output_dir "${_praenora_output}" DIRECTORY)
    set(PRAENORA_LAYOUT_LIBRARY
        "$<PATH:RELATIVE_PATH,$<TARGET_FILE:fx_echo>,${_praenora_output_dir}>")
    set(_praenora_configured "${CMAKE_CURRENT_BINARY_DIR}/praenora-layouts/${_praenora_template}")
    configure_file("${_praenora_layouts_source}/${_praenora_template}" "${_praenora_configured}"
        @ONLY NEWLINE_STYLE UNIX)
    file(GENERATE OUTPUT "${_praenora_output}" INPUT "${_praenora_configured}"
        NEWLINE_STYLE UNIX)
endforeach()

# praenora_add_gtest(<name> LABEL <label> [WHITEBOX] [SANDBOX_SKIP] SOURCES <source>...
#                    [LINK <library>...])
# praenora_add_gtest(<name> NO_TESTS [WHITEBOX] SOURCES <source>... [LINK <library>...])
#
# Adds a GoogleTest executable whose tests are discovered when CTest runs (CTest names
# Suite.Case) and carry the CTest label <label>.
# - NO_TESTS: the executable is built, so its sources compile in every build, but none of its
#   tests is registered: the caller registers them (gtest_discover_tests()) when they can run,
#   adding ${_praenora_test_environment} if they load module libraries, and giving each property
#   one value (as for the official AsyncAPI schema below).
# - WHITEBOX: links the runtime's object library and dependencies directly, so tests can use
#   internal headers (praenora::detail). PRAENORA_RUNTIME_STATIC_DEFINE is defined because the
#   objects are linked into the executable rather than imported from the shared library.
# - SANDBOX_SKIP: its tests of the restricted sandbox profile skip, saying so, in a process that
#   runs in a sandbox already; CTest shows them as skipped (_praenora_sandbox_skip below).
# - Otherwise the executable links only the shared library Praenora::Runtime.
# Either way it links praenora_test_support, GTest::gtest_main and the LINK libraries. An
# executable never links both the object library and the shared library, so LINK cannot add the
# other one. On Windows the DLLs the executable needs are copied next to it.
# AddressSanitizer on macOS keeps the globals of an unloaded (dlclose'd) instrumented library
# registered, and their redzones poisoned in its shadow memory. A library loaded later at an
# overlapping address then trips false reports, and describing them reads the unloaded library's
# metadata, which crashes the sanitizer. Tests of such builds therefore keep module libraries
# loaded (PRAENORA_DEBUG_KEEP_LIBRARIES=1); every other build still unloads them.
set(_praenora_test_environment "")
if(APPLE AND "address" IN_LIST PRAENORA_SANITIZERS)
    set(_praenora_test_environment ENVIRONMENT "PRAENORA_DEBUG_KEEP_LIBRARIES=1")
endif()
# For executables with tests of the restricted sandbox profile (SANDBOX_SKIP): CTest shows as
# skipped GoogleTest's skips (its own expression) and, by the full start of its message, a test of
# the profile in a process that runs in a sandbox already, where macOS applies no second one
# (kNestedSandboxSkip, tests/support/sandbox_support.hpp).
set(_praenora_sandbox_skip SKIP_REGULAR_EXPRESSION
    "\\[  SKIPPED \\]|nested sandbox: this process runs in a sandbox already, and macOS applies")

function(praenora_add_gtest name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "WHITEBOX;NO_TESTS;SANDBOX_SKIP" "LABEL" "SOURCES;LINK")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "praenora_add_gtest(${name}): unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(arg_NO_TESTS AND arg_LABEL)
        message(FATAL_ERROR "praenora_add_gtest(${name}): NO_TESTS registers no test to label; "
            "the caller labels the tests it registers")
    endif()
    if(NOT arg_NO_TESTS AND NOT arg_LABEL)
        message(FATAL_ERROR "praenora_add_gtest(${name}): LABEL is required")
    endif()
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "praenora_add_gtest(${name}): SOURCES is required")
    endif()
    if(arg_WHITEBOX)
        set(forbidden Praenora::Runtime praenora_runtime)
    else()
        set(forbidden praenora_runtime_objects)
    endif()
    foreach(library IN LISTS arg_LINK)
        if(library IN_LIST forbidden)
            message(FATAL_ERROR "praenora_add_gtest(${name}): LINK must not contain ${library}; "
                "a test executable links either the runtime's object library (WHITEBOX) or the "
                "shared library Praenora::Runtime, never both.")
        endif()
    endforeach()

    add_executable(${name} ${arg_SOURCES})
    if(arg_WHITEBOX)
        target_link_libraries(${name} PRIVATE
            praenora_runtime_objects
            praenora_runtime_deps
            praenora_whitebox_support
            praenora_test_support)
        target_compile_definitions(${name} PRIVATE PRAENORA_RUNTIME_STATIC_DEFINE)
    else()
        target_link_libraries(${name} PRIVATE Praenora::Runtime praenora_test_support)
    endif()
    target_link_libraries(${name} PRIVATE ${arg_LINK} GTest::gtest_main)
    praenora_target_warnings(${name})
    praenora_copy_runtime_dlls(${name})

    set(skip "")
    if(arg_SANDBOX_SKIP)
        set(skip ${_praenora_sandbox_skip})
    endif()
    if(NOT arg_NO_TESTS)
        gtest_discover_tests(${name}
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS ${arg_LABEL} ${_praenora_test_environment} ${skip})
    endif()
endfunction()

praenora_add_gtest(praenora_unit_tests LABEL unit WHITEBOX
    SOURCES
        unit/version_test.cpp
        unit/sdk_abi_test.cpp
        unit/sdk_abi_c99_check.c
        unit/provider_abi_test.cpp
        unit/provider_abi_c99_check.c
        unit/simulation_abi_test.cpp
        unit/simulation_c99_check.c
        unit/model_conversion_abi_test.cpp
        unit/model_conversion_c99_check.c
        unit/antimony_source_checks_test.cpp
        ../src/providers/antimony/source_checks.hpp
        ../src/providers/antimony/source_checks.cpp
        unit/sdk_adapter_test.cpp
        unit/sdk_testkit_test.cpp
        unit/sdk_windows_macros_test.cpp
        unit/error_test.cpp
        unit/core_test.cpp
        unit/document_test.cpp
        unit/schema_test.cpp
        unit/logging_test.cpp
        unit/config_test.cpp
        unit/csv_test.cpp
        unit/input_source_test.cpp
        unit/fingerprint_test.cpp
        unit/metrics_test.cpp
        unit/status_file_test.cpp
        unit/locale_test.cpp
        unit/runtime_context_test.cpp
        unit/run_record_test.cpp
        unit/path_anchors_test.cpp
        unit/library_gate_test.cpp
        unit/channel_test.cpp
        unit/compiled_model_store_test.cpp
        unit/output_sink_test.cpp
        unit/sandbox_test.cpp
    LINK Praenora::ModuleSDK)
# sdk_abi_c99_check.c, simulation_c99_check.c, model_conversion_c99_check.c and
# provider_abi_c99_check.c prove that the SDK's C headers (those of the simulation and model
# conversion services included) and the provider interface header are valid C99.
set_target_properties(praenora_unit_tests PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
# The antimony provider's rules that read a source's text alone need no engine: their source file
# is compiled into the unit tests of every build (unit/antimony_source_checks_test.cpp).
target_include_directories(praenora_unit_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/providers/antimony")
# The unit tests embed two fixture schemas of their own (praenora_test_find_resource()), beside the
# runtime's embedded resources.
# The fingerprint tests resolve and hash the fixture modules and providers (without loading them).
add_dependencies(praenora_unit_tests praenora_test_modules praenora_test_providers)
praenora_embed_resources(TARGET praenora_unit_tests NAME praenora_test
    BASE_DIR "${PROJECT_SOURCE_DIR}/tests/fixtures/documents"
    FILES
        schema-registry/sample.0.1.schema.json
        schema-registry/sample.0.2.schema.json)

praenora_add_gtest(praenora_protocol_tests LABEL protocol WHITEBOX
    SOURCES
        protocol/schemas_test.cpp
        protocol/catalog_test.cpp
        protocol/graph_test.cpp
        protocol/graph_export_test.cpp)
# The committed AsyncAPI documents against the official AsyncAPI 3.0.0 JSON Schema
# (protocol/asyncapi_official_schema_test.cpp), in an executable of their own that every build
# compiles. Its tests are registered, with the label protocol, only when PRAENORA_ASYNCAPI_SCHEMA
# names a local copy of that schema, which the build never downloads; CTest then gives them the
# copy's path in the environment variable PRAENORA_TEST_ASYNCAPI_SCHEMA. They load no module
# library, so they need no PRAENORA_DEBUG_KEEP_LIBRARIES.
praenora_add_gtest(praenora_asyncapi_official_schema_tests NO_TESTS WHITEBOX
    SOURCES protocol/asyncapi_official_schema_test.cpp)
if(NOT PRAENORA_ASYNCAPI_SCHEMA STREQUAL "")
    # cmake_path() reads the value as one path (file(TO_CMAKE_PATH) reads it as a search path and
    # splits it at ':' on Linux and macOS); on Windows it turns backslashes into '/'. A ';', '['
    # or ']' would split, or stop splitting, the CMake lists below, and gtest_discover_tests()
    # writes the values into brackets ([[...]]).
    cmake_path(SET _praenora_asyncapi_schema NORMALIZE "${PRAENORA_ASYNCAPI_SCHEMA}")
    if(NOT IS_ABSOLUTE "${_praenora_asyncapi_schema}" OR NOT EXISTS "${_praenora_asyncapi_schema}"
            OR IS_DIRECTORY "${_praenora_asyncapi_schema}"
            OR _praenora_asyncapi_schema MATCHES "[];[]")
        message(FATAL_ERROR "PRAENORA_ASYNCAPI_SCHEMA must be the absolute path, without ';', '[' "
            "or ']', of a copy of the official AsyncAPI 3.0.0 JSON Schema (docs/testing.md); it "
            "is '${PRAENORA_ASYNCAPI_SCHEMA}'")
    endif()
    # Each property holds one value: gtest_discover_tests() before CMake 3.31 splits a property's
    # list value into separate arguments.
    set(_praenora_asyncapi_properties LABELS protocol
        ENVIRONMENT "PRAENORA_TEST_ASYNCAPI_SCHEMA=${_praenora_asyncapi_schema}")
    gtest_discover_tests(praenora_asyncapi_official_schema_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES ${_praenora_asyncapi_properties})
endif()
# The committed AsyncAPI documents through the official AsyncAPI parser, @asyncapi/parser, which
# Node.js runs with protocol/asyncapi_parser_check.cjs. Its two checks are registered, with the
# label protocol, only when PRAENORA_ASYNCAPI_PARSER names a directory into which that parser is
# installed (it holds node_modules/@asyncapi/parser), which the build never downloads:
# asyncapi.parser.accepts_committed_documents (each document parses, with only the diagnostics the
# script records) and asyncapi.parser.refuses_broken_copies. Both first check the parser's version.
# The path reaches Node.js as one argument, but a ';' is refused: CMake reads a PATH value given
# on the command line without a type as a list, and makes each element absolute, so such a value
# reaches this file broken. Node.js is looked for where find_program() looks, the bin directories
# of CMAKE_PREFIX_PATH before the PATH. The caller's NODE_OPTIONS and NODE_PATH cannot change what
# Node.js loads.
if(NOT PRAENORA_ASYNCAPI_PARSER STREQUAL "")
    cmake_path(SET _praenora_asyncapi_parser NORMALIZE "${PRAENORA_ASYNCAPI_PARSER}")
    if(NOT IS_ABSOLUTE "${_praenora_asyncapi_parser}" OR _praenora_asyncapi_parser MATCHES ";"
            OR NOT EXISTS
            "${_praenora_asyncapi_parser}/node_modules/@asyncapi/parser/package.json")
        message(FATAL_ERROR "PRAENORA_ASYNCAPI_PARSER must be the absolute path, without ';', of "
            "a directory into which @asyncapi/parser is installed, one that holds "
            "node_modules/@asyncapi/parser (docs/testing.md); it is '${PRAENORA_ASYNCAPI_PARSER}'")
    endif()
    find_program(PRAENORA_NODE_EXECUTABLE NAMES node nodejs
        DOC "With PRAENORA_ASYNCAPI_PARSER: the Node.js that runs the official AsyncAPI parser")
    if(PRAENORA_NODE_EXECUTABLE MATCHES "-NOTFOUND$")
        message(FATAL_ERROR "PRAENORA_ASYNCAPI_PARSER needs Node.js, and no node was found: "
            "name it with PRAENORA_NODE_EXECUTABLE (docs/testing.md)")
    elseif(NOT EXISTS "${PRAENORA_NODE_EXECUTABLE}" OR IS_DIRECTORY "${PRAENORA_NODE_EXECUTABLE}")
        message(FATAL_ERROR "PRAENORA_ASYNCAPI_PARSER needs Node.js, and PRAENORA_NODE_EXECUTABLE "
            "is '${PRAENORA_NODE_EXECUTABLE}', which names no file: name Node.js there, or unset "
            "it (-UPRAENORA_NODE_EXECUTABLE) to search again (docs/testing.md)")
    endif()
    foreach(_praenora_check IN ITEMS accepts_committed_documents refuses_broken_copies)
        add_test(NAME asyncapi.parser.${_praenora_check}
            COMMAND "${PRAENORA_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/protocol/asyncapi_parser_check.cjs"
                ${_praenora_check} "${_praenora_asyncapi_parser}"
                "${PROJECT_SOURCE_DIR}/spec/asyncapi")
        set_tests_properties(asyncapi.parser.${_praenora_check} PROPERTIES
            LABELS protocol
            TIMEOUT 120
            ENVIRONMENT_MODIFICATION "NODE_OPTIONS=unset:;NODE_PATH=unset:")
    endforeach()
endif()

praenora_add_gtest(praenora_negative_tests LABEL negative WHITEBOX
    SOURCES
        negative/graph_negative_test.cpp
        negative/manifest_negative_test.cpp
        negative/provider_manifest_negative_test.cpp
        negative/compat_test.cpp
        negative/config_negative_test.cpp
        negative/validation_test.cpp
        negative/service_validation_test.cpp
        negative/isolation_validation_test.cpp)
# The validation tests resolve and check the fixture modules and providers (without loading them);
# the provider load failures load fixture providers that break the provider interface.
add_dependencies(praenora_negative_tests praenora_test_modules praenora_test_providers)

# Contract tests: manifests and discovery, the loader, module instances, the engine and the
# contract checks, through the real loader and the fixture modules; and the service checks of the
# simulation service (contract/simulation_service_checks.hpp) and of the model conversion service
# (contract/model_conversion_service_checks.hpp), one instantiation per provider.
praenora_add_gtest(praenora_contract_tests LABEL contract WHITEBOX
    SOURCES
        contract/discovery_test.cpp
        contract/loader_test.cpp
        contract/provider_discovery_test.cpp
        contract/provider_loader_test.cpp
        contract/instance_contract_test.cpp
        contract/engine_test.cpp
        contract/service_run_test.cpp
        contract/module_check_test.cpp
        contract/simulation_service_checks.hpp
        contract/simulation_service_checks.cpp
        contract/simulation_service_test.cpp
        contract/model_conversion_service_checks.hpp
        contract/model_conversion_service_checks.cpp
        contract/model_conversion_service_test.cpp)
target_compile_definitions(praenora_contract_tests PRIVATE
    "PRAENORA_TEST_LAYOUTS_DIR=\"${PRAENORA_TEST_LAYOUTS_DIR}\"")
add_dependencies(praenora_contract_tests praenora_test_modules praenora_test_providers)
# The contract checks are also run against the example modules, when they are built.
if(PRAENORA_BUILD_EXAMPLES)
    target_compile_definitions(praenora_contract_tests PRIVATE PRAENORA_TEST_WITH_EXAMPLES)
    add_dependencies(praenora_contract_tests praenora_example_modules)
endif()
# With the roadrunner provider (PRAENORA_WITH_ROADRUNNER): the service checks against it, with two
# SBML models (contract/simulation_service_roadrunner_test.cpp), and what it does beyond them
# (contract/roadrunner_provider_test.cpp). PRAENORA_TEST_ROADRUNNER_MANIFEST names its manifest.
if(PRAENORA_WITH_ROADRUNNER)
    target_sources(praenora_contract_tests PRIVATE
        contract/simulation_service_roadrunner_test.cpp
        contract/roadrunner_provider_test.cpp)
    set(_praenora_roadrunner_manifest
        "$<GENEX_EVAL:$<TARGET_PROPERTY:praenora_provider_roadrunner,PRAENORA_PROVIDER_MANIFEST>>")
    target_compile_definitions(praenora_contract_tests PRIVATE
        "PRAENORA_TEST_ROADRUNNER_MANIFEST=\"${_praenora_roadrunner_manifest}\"")
    add_dependencies(praenora_contract_tests praenora_provider_roadrunner)
    # The provider's engine code itself, compiled into a test of its own with the provider's
    # definitions (contract/roadrunner_engine_test.cpp): models loaded from their saved states, as
    # the shared compiled models keep them, are the models compiled from their texts.
    praenora_add_gtest(praenora_roadrunner_engine_tests LABEL contract
        SOURCES
            contract/roadrunner_engine_test.cpp
            ../src/providers/roadrunner/engine.cpp
            ../src/providers/roadrunner/documents.cpp
        LINK praenora_provider_interface praenora_roadrunner_engine nlohmann_json::nlohmann_json)
    target_include_directories(praenora_roadrunner_engine_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src/providers/roadrunner")
    target_compile_definitions(praenora_roadrunner_engine_tests PRIVATE
        "$<TARGET_PROPERTY:praenora_provider_roadrunner,COMPILE_DEFINITIONS>")
endif()
# With the antimony provider (PRAENORA_WITH_ANTIMONY): the service checks of the model conversion
# service against it, with the models of fixtures/models/antimony and fixtures/models/sbml
# (contract/model_conversion_service_antimony_test.cpp), and what it does beyond them
# (contract/antimony_provider_test.cpp). PRAENORA_TEST_ANTIMONY_MANIFEST names its manifest; the
# versions are those the build's probe had the libraries report (cmake/PraenoraAntimony.cmake).
if(PRAENORA_WITH_ANTIMONY)
    target_sources(praenora_contract_tests PRIVATE
        contract/model_conversion_service_antimony_test.cpp
        contract/antimony_provider_test.cpp)
    set(_praenora_antimony_manifest
        "$<GENEX_EVAL:$<TARGET_PROPERTY:praenora_provider_antimony,PRAENORA_PROVIDER_MANIFEST>>")
    target_compile_definitions(praenora_contract_tests PRIVATE
        "PRAENORA_TEST_ANTIMONY_MANIFEST=\"${_praenora_antimony_manifest}\""
        "PRAENORA_TEST_ANTIMONY_VERSION=\"${PRAENORA_ANTIMONY_VERSION}\""
        "PRAENORA_TEST_ANTIMONY_LIBSBML_VERSION=\"${PRAENORA_ANTIMONY_LIBSBML_VERSION}\""
        "PRAENORA_TEST_SBMLNETWORK_VERSION=\"${PRAENORA_SBMLNETWORK_VERSION}\"")
    add_dependencies(praenora_contract_tests praenora_provider_antimony)
endif()

# Lifecycle tests: call order, cleanup, timeouts and cancellation of module instances, calls into
# one library from instances of several loads of it, and the runtime services an instance is
# served.
praenora_add_gtest(praenora_lifecycle_tests LABEL lifecycle WHITEBOX
    SOURCES
        lifecycle/instance_test.cpp
        lifecycle/library_sharing_test.cpp
        lifecycle/service_visibility_test.cpp)
add_dependencies(praenora_lifecycle_tests praenora_test_modules praenora_test_providers)

# Isolation tests (label isolation): child processes, the module host and module instances in it
# (tests/isolation).
add_subdirectory(isolation)

# Integration tests: the public API through the shared library only, with the fixture modules and
# the test catalog (integration/api_test.cpp), steps in module hosts
# (integration/isolation_test.cpp) and, when the examples are built, the example workflow end to
# end (integration/example_test.cpp), in module hosts too (integration/isolation_example_test.cpp).
set(_praenora_integration_sources integration/api_test.cpp integration/isolation_test.cpp)
if(PRAENORA_BUILD_EXAMPLES)
    list(APPEND _praenora_integration_sources integration/example_test.cpp
        integration/isolation_example_test.cpp)
endif()
praenora_add_gtest(praenora_integration_tests LABEL integration SANDBOX_SKIP
    SOURCES ${_praenora_integration_sources})
# The build's document formats, "<kind>=<version>" pairs separated by ','.
list(JOIN PRAENORA_FORMAT_VERSIONS "," _praenora_format_versions)
target_compile_definitions(praenora_integration_tests PRIVATE
    "PRAENORA_TEST_RUNTIME_VERSION=\"${PRAENORA_RUNTIME_VERSION}\""
    "PRAENORA_TEST_FORMAT_VERSIONS=\"${_praenora_format_versions}\""
    "PRAENORA_TEST_PLATFORM=\"${PRAENORA_PLATFORM}\""
    "PRAENORA_TEST_BUILD_TYPE=\"$<CONFIG>\""
    "PRAENORA_EXAMPLE_WORKFLOW_DIR=\"${PROJECT_SOURCE_DIR}/examples/workflows/estimate-predict\""
    "PRAENORA_TEST_GOLDEN_DIR=\"${PROJECT_SOURCE_DIR}/tests/fixtures/golden\"")
add_dependencies(praenora_integration_tests praenora_test_modules praenora_test_providers)
if(PRAENORA_BUILD_EXAMPLES)
    add_dependencies(praenora_integration_tests praenora_example_modules)
endif()
# The module host, which the runtime starts for the steps in process mode.
if(TARGET praenora_module_host)
    add_dependencies(praenora_integration_tests praenora_module_host)
endif()
# With the roadrunner provider, the build tree's runtime finds it without a provider path; so it
# does the antimony provider.
if(PRAENORA_WITH_ROADRUNNER)
    target_compile_definitions(praenora_integration_tests PRIVATE PRAENORA_TEST_WITH_ROADRUNNER)
    add_dependencies(praenora_integration_tests praenora_provider_roadrunner)
endif()
if(PRAENORA_WITH_ANTIMONY)
    target_compile_definitions(praenora_integration_tests PRIVATE PRAENORA_TEST_WITH_ANTIMONY)
    add_dependencies(praenora_integration_tests praenora_provider_antimony)
endif()

# CLI tests (label cli): the praenora tool, run through tests/cli/run_cli.cmake, and that driver
# itself.
if(TARGET praenora_cli)
    add_subdirectory(cli)
endif()

# Python tests (label python): the praenora.runtime package and its extension, run by the
# interpreter (tests/python).
if(TARGET praenora_python_bindings)
    add_subdirectory(python)
endif()

# Install test (label install): the installation, used from outside the build (tests/install).
# Only a build with install rules has it.
if(PRAENORA_INSTALL_RULES)
    add_subdirectory(install)
endif()

# Provider libraries as built files (label contract): their exported symbols, and the same results
# in two processes (tests/providers).
add_subdirectory(providers)

# Performance probe (label perf): praenora_perf and its two report-only runs (tests/perf).
add_subdirectory(perf)

# Documentation test (label docs): the documentation checked against the source tree (tests/docs).
add_subdirectory(docs)

# Tool tests (label tools): tools/migrate_schemas_to_2020_12.py and the schemas it wrote
# (tests/tools).
add_subdirectory(tools)

# CI tooling tests (label ci): the script that fetches a pinned dependency bundle, run on fake
# archives from file:// URLs, and the workflow checked against the pin and the CI presets
# (tests/ci).
add_subdirectory(ci)
