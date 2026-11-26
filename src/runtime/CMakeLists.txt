# The runtime is compiled once, into the OBJECT library praenora_runtime_objects. Its objects are
# linked into the shared library praenora_runtime (Praenora::Runtime), which exports only the
# public API, and directly into the white-box test executables.

include(GenerateExportHeader)

# Third-party dependencies of the runtime. Linked PRIVATE everywhere: public headers use only the
# standard library and Praenora types.
# System libraries come with them: threads for the module workers and, where it is separate, the
# dynamic loader library.
find_package(Threads REQUIRED)
add_library(praenora_runtime_deps INTERFACE)
target_link_libraries(praenora_runtime_deps INTERFACE
    jsoncons::jsoncons
    nlohmann_json::nlohmann_json
    yaml-cpp::yaml-cpp
    ${PRAENORA_SPDLOG_TARGET}
    Threads::Threads
    ${CMAKE_DL_LIBS})

add_library(praenora_runtime_objects OBJECT
    api/graph_export.cpp
    api/module_check.cpp
    api/module_check_cases.cpp
    api/runtime.cpp
    api/validation.cpp
    api/version.cpp
    api/workflow.cpp
    catalog/catalog.cpp
    compat/compat.cpp
    config/workflow_config.cpp
    discovery/manifest.cpp
    discovery/manifest_reader.cpp
    discovery/module_index.cpp
    discovery/search_paths.cpp
    engine/engine.cpp
    engine/envelope.cpp
    engine/result_check.cpp
    graph/graph.cpp
    instance/module_instance.cpp
    instance/services.cpp
    instance/worker.cpp
    isolation/channel.cpp
    isolation/child_module.cpp
    isolation/compiled_model_store.cpp
    isolation/sandbox.cpp
    io/csv.cpp
    io/input_source.cpp
    io/json_document_source.cpp
    io/observation_csv_source.cpp
    io/output_recorder.cpp
    io/run_record.cpp
    io/trace_writer.cpp
    loader/library_file.cpp
    loader/library_gate.cpp
    loader/loaded_module.cpp
    core/duration.cpp
    core/error.cpp
    core/paths.cpp
    core/seed.cpp
    core/semver.cpp
    core/sha256.cpp
    core/status_names.cpp
    core/time.cpp
    document/json_util.cpp
    document/resources.cpp
    document/schema.cpp
    document/schema_registry.cpp
    document/yaml_loader.cpp
    observability/logging.cpp
    observability/metrics.cpp
    observability/path_anchors.cpp
    observability/status_file.cpp
    observability/status_json.cpp
    provider/provider_index.cpp
    provider/provider_loader.cpp
    provider/provider_manifest.cpp)
# Operating-system specific code: platform/windows on Windows, platform/posix elsewhere.
if(WIN32)
    target_sources(praenora_runtime_objects PRIVATE
        platform/windows/child_process_windows.cpp
        platform/windows/platform_windows.cpp
        platform/windows/shared_library_windows.cpp)
else()
    target_sources(praenora_runtime_objects PRIVATE
        platform/posix/child_process_posix.cpp
        platform/posix/platform_posix.cpp
        platform/posix/shared_library_posix.cpp)
endif()
target_include_directories(praenora_runtime_objects PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
# The objects end up in the shared library, so they are compiled as its sources would be: with
# its export symbol defined, position-independent and with hidden visibility.
target_compile_definitions(praenora_runtime_objects PRIVATE praenora_runtime_EXPORTS)
target_link_libraries(praenora_runtime_objects PRIVATE praenora_runtime_deps)
set_target_properties(praenora_runtime_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
praenora_target_warnings(praenora_runtime_objects)
# jsoncons's templates give schema.cpp more sections than a Debug object file of MSVC can hold.
if(MSVC)
    set_source_files_properties(document/schema.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
endif()

# The schemas and the protocol catalog are embedded in the runtime (document/resources.hpp):
# spec/schemas/*.json under schemas/ and spec/catalog/**/*.json under catalog/. The globs are
# re-checked at build time, so added or removed files take effect without a manual reconfigure.
file(GLOB _praenora_schema_files CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/spec/schemas/*.json")
file(GLOB_RECURSE _praenora_catalog_files CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/spec/catalog/*.json")
praenora_embed_resources(TARGET praenora_runtime_objects NAME praenora_runtime
    BASE_DIR "${PROJECT_SOURCE_DIR}/spec"
    FILES ${_praenora_schema_files} ${_praenora_catalog_files})

# Build facts reported by praenora::version_info() (api/version.cpp). They are target-level
# definitions because some generators (Xcode) cannot vary per-source settings by configuration.
# The document formats, one version per kind (PRAENORA_FORMAT_VERSIONS), reach version.cpp as the
# generated table format_versions.inc, one line {"<kind>", "<version>"}, per kind; file(CONFIGURE)
# rewrites it only when it changes.
set(_praenora_format_table "// Generated from PRAENORA_FORMAT_VERSIONS.\n")
foreach(_praenora_format IN LISTS PRAENORA_FORMAT_VERSIONS)
    string(REGEX REPLACE "^([^=]+)=(.+)$" "{\"\\1\", \"\\2\"},\n" _praenora_line
        "${_praenora_format}")
    string(APPEND _praenora_format_table "${_praenora_line}")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/format_versions.inc"
    CONTENT "${_praenora_format_table}" NEWLINE_STYLE UNIX)
target_include_directories(praenora_runtime_objects PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(praenora_runtime_objects PRIVATE
    "PRAENORA_BUILD_RUNTIME_VERSION=\"${PRAENORA_RUNTIME_VERSION}\""
    "PRAENORA_BUILD_SDK_VERSION=\"${PRAENORA_SDK_VERSION}\""
    "PRAENORA_BUILD_PMP_VERSION=\"${PRAENORA_PMP_VERSION}\""
    "PRAENORA_BUILD_ABI_MAJOR=${PRAENORA_ABI_MAJOR}u"
    "PRAENORA_BUILD_ABI_MINOR=${PRAENORA_ABI_MINOR}u"
    "PRAENORA_BUILD_PLATFORM=\"${PRAENORA_PLATFORM}\""
    "PRAENORA_BUILD_COMPILER=\"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\""
    "PRAENORA_BUILD_TYPE=\"$<CONFIG>\"")

# The provider interface (provider/provider_abi.h, which provider code includes as
# "provider_abi.h") for the service providers built in this repository with
# praenora_add_provider() (cmake/PraenoraProvider.cmake). Internal: never installed.
add_library(praenora_provider_interface INTERFACE)
target_include_directories(praenora_provider_interface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/provider")
target_link_libraries(praenora_provider_interface INTERFACE praenora_module_sdk)

add_library(praenora_runtime SHARED)
add_library(Praenora::Runtime ALIAS praenora_runtime)
target_link_libraries(praenora_runtime PRIVATE praenora_runtime_objects praenora_runtime_deps)
target_include_directories(praenora_runtime PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_features(praenora_runtime PUBLIC cxx_std_17)
set_target_properties(praenora_runtime PROPERTIES
    OUTPUT_NAME praenora_runtime
    EXPORT_NAME Runtime
    VERSION ${PRAENORA_RUNTIME_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
# Exports of the shared library: symbols of the praenora namespace only (with the type information
# and vtables of its classes). The runtime's own code is compiled with hidden visibility anyway;
# these link options also keep the symbols of statically linked dependencies (such as yaml-cpp,
# built with default visibility) internal, so they cannot clash with another copy in the host
# application. jsoncons and nlohmann/json are header-only and compiled into the runtime's own
# objects, so they are hidden by the setting above.
# - macOS: exported-symbol patterns (verified).
# - Other ELF platforms: a version script plus --exclude-libs. The functions and variables are
#   matched by their demangled names; the type information, its names and the vtables by their
#   mangled names, as on macOS, because a version script matches a quoted demangled name (such as
#   "typeinfo for praenora::Error") literally, never as a pattern, and an unquoted one cannot hold
#   the spaces of "typeinfo for".
# - Windows: only dllexport'ed symbols are exported in the first place.
if(APPLE)
    target_link_options(praenora_runtime PRIVATE
        "LINKER:-exported_symbol,__ZN8praenora*"
        "LINKER:-exported_symbol,__ZNK8praenora*"
        "LINKER:-exported_symbol,__ZTIN8praenora*"
        "LINKER:-exported_symbol,__ZTSN8praenora*"
        "LINKER:-exported_symbol,__ZTVN8praenora*")
elseif(NOT WIN32)
    set(_praenora_version_script "${CMAKE_CURRENT_BINARY_DIR}/praenora_runtime.map")
    file(CONFIGURE OUTPUT "${_praenora_version_script}" @ONLY CONTENT [=[{
  global:
    extern "C++" {
      praenora::*;
    };
    _ZTIN8praenora*;
    _ZTSN8praenora*;
    _ZTVN8praenora*;
  local:
    *;
};
]=])
    target_link_options(praenora_runtime PRIVATE
        "LINKER:--version-script=${_praenora_version_script}"
        "LINKER:--exclude-libs,ALL")
    set_property(TARGET praenora_runtime APPEND PROPERTY LINK_DEPENDS "${_praenora_version_script}")
endif()

# Generated include/praenora/runtime/export.h: PRAENORA_RUNTIME_API marks the public API.
# PRAENORA_RUNTIME_STATIC_DEFINE empties it for code that links the objects directly.
generate_export_header(praenora_runtime
    BASE_NAME PRAENORA_RUNTIME
    EXPORT_MACRO_NAME PRAENORA_RUNTIME_API
    EXPORT_FILE_NAME "${PROJECT_BINARY_DIR}/include/praenora/runtime/export.h"
    STATIC_DEFINE PRAENORA_RUNTIME_STATIC_DEFINE)

# On Windows, the DLLs of the third-party libraries go beside the runtime's DLL in <build>/bin.
# Executables that link only the runtime (the praenora tool, black-box tests) need them there:
# their own $<TARGET_RUNTIME_DLLS> lists the runtime's DLL but not the libraries it links
# privately.
praenora_copy_runtime_dlls(praenora_runtime)

# Installed: the library (export set PraenoraRuntimeTargets, cmake/PraenoraInstall.cmake; on
# Windows the DLL goes to the bin directory, with the DLLs of its third-party libraries, and its
# import library to the lib directory) and the public headers, the generated export.h included.
# The installed library keeps CMake's default install name on macOS,
# @rpath/libpraenora_runtime.<major>.<minor>.dylib, and has no RPATH of its own: the third-party
# libraries it links are found by their own install names (macOS) or the loader's search path
# (Linux).
if(PRAENORA_INSTALL_RULES)
    install(TARGETS praenora_runtime EXPORT PraenoraRuntimeTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    if(WIN32)
        # Nothing is installed when every third-party library is static.
        install(FILES "$<TARGET_RUNTIME_DLLS:praenora_runtime>"
            DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/praenora/runtime"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/praenora"
        FILES_MATCHING PATTERN "*.hpp")
    install(FILES "${PROJECT_BINARY_DIR}/include/praenora/runtime/export.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/praenora/runtime")
endif()
