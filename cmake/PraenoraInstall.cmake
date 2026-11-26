# The install rules of the package that belong to no single directory: the CMake package
# configuration that find_package(Praenora) reads, the export sets of Praenora::Runtime and
# Praenora::ModuleSDK, the module helper PraenoraModule.cmake, the schemas and the catalog, and the
# license files.
# The top-level CMakeLists.txt includes this file after every target is defined, when
# PRAENORA_INSTALL_RULES is ON (never in a wheel build; cmake/PraenoraOptions.cmake). The targets,
# the headers and the example modules are installed by their own directories (src/runtime,
# src/sdk, src/cli, examples).
#
# The installation, relative to its prefix (GNUInstallDirs directories; on Windows the DLLs go to
# the bin directory and the import library to the lib directory):
#   ${CMAKE_INSTALL_BINDIR}/      praenora (Windows: and the DLLs it loads)
#   ${CMAKE_INSTALL_LIBDIR}/      the runtime library (soname links on Linux, install name
#                                 @rpath/libpraenora_runtime.<major>.<minor>.dylib on macOS)
#   ${CMAKE_INSTALL_INCLUDEDIR}/praenora/{runtime,sdk}/   the public headers, generated ones
#                                 (export.h, version.h) included
#   ${CMAKE_INSTALL_LIBDIR}/cmake/Praenora/   PraenoraConfig.cmake, PraenoraConfigVersion.cmake,
#                                 PraenoraRuntimeTargets*.cmake, PraenoraModuleSDKTargets*.cmake
#                                 and PraenoraModule.cmake
#   ${CMAKE_INSTALL_DATADIR}/praenora/{schemas,catalog}/   the documents embedded in the runtime
#   ${CMAKE_INSTALL_DATADIR}/praenora/licenses/   license files: praenora-runtime/LICENSE, this
#                                 project's; <name>/ for each dependency built in this tree
#                                 (PRAENORA_FETCHED_DEPENDENCIES), its license files under their
#                                 paths in its sources; and the contents of each directory of
#                                 PRAENORA_THIRD_PARTY_LICENSE_DIRS
#   ${CMAKE_INSTALL_LIBDIR}/praenora/modules/<name>/   the example modules: install component
#                                 Examples, part of a default installation only with
#                                 PRAENORA_INSTALL_EXAMPLES; that is the installation's module
#                                 directory, the last place the runtime looks for modules
# Every installed CMake file names locations relative to the installation, so it can be moved.

include_guard(GLOBAL)
include(CMakePackageConfigHelpers)

set(_praenora_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/Praenora")

install(EXPORT PraenoraRuntimeTargets
    NAMESPACE Praenora::
    DESTINATION "${_praenora_package_dir}")
install(EXPORT PraenoraModuleSDKTargets
    NAMESPACE Praenora::
    DESTINATION "${_praenora_package_dir}")

# No check_required_components(): PraenoraConfig.cmake.in says why it sets Praenora_FOUND itself.
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/PraenoraConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/cmake/PraenoraConfig.cmake"
    INSTALL_DESTINATION "${_praenora_package_dir}"
    NO_CHECK_REQUIRED_COMPONENTS_MACRO)
# While the version is 0.y, a change of y may break compatibility.
write_basic_package_version_file("${PROJECT_BINARY_DIR}/cmake/PraenoraConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion)
install(FILES
    "${PROJECT_BINARY_DIR}/cmake/PraenoraConfig.cmake"
    "${PROJECT_BINARY_DIR}/cmake/PraenoraConfigVersion.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/PraenoraModule.cmake"
    DESTINATION "${_praenora_package_dir}")

# The schemas and the catalog, as embedded in the runtime (cmake/PraenoraEmbedResources.cmake).
foreach(_praenora_documents IN ITEMS schemas catalog)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/spec/${_praenora_documents}/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/praenora/${_praenora_documents}"
        FILES_MATCHING PATTERN "*.json")
endforeach()

# The license files: this project's, under the MIT license (LICENSE).
set(_praenora_licenses_dir "${CMAKE_INSTALL_DATADIR}/praenora/licenses")
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
    DESTINATION "${_praenora_licenses_dir}/praenora-runtime")
# A dependency built in this tree is linked statically into the runtime, so the installation holds
# its license files: those at the top of its sources whose names begin with LICENSE or COPYING, in
# any case, the NOTICE files beside them (which the Apache License asks to pass on), and spdlog's
# copy of the license of the fmt library it bundles, if it has one; each under its path in the
# sources. A dependency without a LICENSE or COPYING file stops the configuration.
foreach(_praenora_dependency IN LISTS PRAENORA_FETCHED_DEPENDENCIES)
    set(_praenora_source "${PRAENORA_FETCHED_SOURCE_DIR_${_praenora_dependency}}")
    file(GLOB _praenora_top_files LIST_DIRECTORIES false RELATIVE "${_praenora_source}"
        "${_praenora_source}/*")
    set(_praenora_license_files ${_praenora_top_files})
    list(FILTER _praenora_license_files INCLUDE
        REGEX "^([Ll][Ii][Cc][Ee][Nn][Ss][Ee]|[Cc][Oo][Pp][Yy][Ii][Nn][Gg])")
    if(NOT _praenora_license_files)
        message(FATAL_ERROR "The sources of ${_praenora_dependency}, which is built in this tree, "
            "hold no license file (LICENSE* or COPYING*) in ${_praenora_source}.")
    endif()
    set(_praenora_notice_files ${_praenora_top_files})
    list(FILTER _praenora_notice_files INCLUDE REGEX "^[Nn][Oo][Tt][Ii][Cc][Ee]")
    list(APPEND _praenora_license_files ${_praenora_notice_files})
    if(_praenora_dependency STREQUAL "jsoncons")
        # Headers that carry the license text of code they include (MIT: a5hash by Aleksey Vaneev,
        # Grisu3 by Florian Loitsch; Apache 2.0: MongoDB's BSON headers, which the JSON Schema
        # code does not include). jsoncons' LICENSE covers its own code only.
        list(APPEND _praenora_license_files
            include/jsoncons/nonstd/a5hash.hpp
            include/jsoncons/utility/detail/grisu3.hpp
            include/jsoncons_ext/bson/bson_decimal128.hpp
            include/jsoncons_ext/bson/bson_oid.hpp)
    endif()
    if(_praenora_dependency STREQUAL "spdlog")
        file(GLOB _praenora_fmt_license LIST_DIRECTORIES false RELATIVE "${_praenora_source}"
            "${_praenora_source}/include/spdlog/fmt/bundled/*")
        list(FILTER _praenora_fmt_license INCLUDE REGEX "[Ll][Ii][Cc][Ee][Nn][Ss][Ee][^/]*$")
        list(APPEND _praenora_license_files ${_praenora_fmt_license})
    endif()
    foreach(_praenora_license_file IN LISTS _praenora_license_files)
        get_filename_component(_praenora_license_subdir "${_praenora_license_file}" DIRECTORY)
        set(_praenora_license_destination "${_praenora_licenses_dir}/${_praenora_dependency}")
        if(NOT _praenora_license_subdir STREQUAL "")
            string(APPEND _praenora_license_destination "/${_praenora_license_subdir}")
        endif()
        install(FILES "${_praenora_source}/${_praenora_license_file}"
            DESTINATION "${_praenora_license_destination}")
    endforeach()
endforeach()
# The contents of each directory of PRAENORA_THIRD_PARTY_LICENSE_DIRS (cmake/PraenoraOptions.cmake),
# such as the licenses/ directory of a dependency bundle.
foreach(_praenora_license_entry IN LISTS PRAENORA_THIRD_PARTY_LICENSE_DIRS)
    if(_praenora_license_entry STREQUAL "")
        continue()
    endif()
    cmake_path(SET _praenora_license_entry_dir NORMALIZE "${_praenora_license_entry}")
    string(REGEX REPLACE "(.)/+$" "\\1" _praenora_license_entry_dir
        "${_praenora_license_entry_dir}")
    if(NOT IS_ABSOLUTE "${_praenora_license_entry_dir}"
       OR NOT IS_DIRECTORY "${_praenora_license_entry_dir}")
        message(FATAL_ERROR "PRAENORA_THIRD_PARTY_LICENSE_DIRS names '${_praenora_license_entry}', "
            "which is not the absolute path of a directory.")
    endif()
    install(DIRECTORY "${_praenora_license_entry_dir}/" DESTINATION "${_praenora_licenses_dir}")
endforeach()
