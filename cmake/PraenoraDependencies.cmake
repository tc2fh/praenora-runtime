# praenora_find_dependencies()
#
# Makes the third-party dependencies available. It is a macro, so the variables and imported
# targets it creates stay in the caller's scope.
#
# - By default every dependency comes from find_package() in CONFIG mode, at the minimum versions
#   below, and nothing is downloaded.
# - With PRAENORA_FETCH_DEPENDENCIES=ON, FetchContent first tries the same find_package() call and
#   downloads only what is missing, at the pinned commits below.
# - jsoncons (the JSON Schema validator) is header-only, so it is compiled with the flags of the
#   build, the sanitizers' in particular. Fetched, it is not built as a project: only its sources
#   are downloaded and a target for its include directory is defined here, because its own
#   CMakeLists.txt has install rules that cannot be switched off (ADR-0017).
# - GoogleTest is needed only when PRAENORA_BUILD_TESTS is ON.
# - When the cache variable PRAENORA_RUNTIME_DEPS is defined, as the ci-* presets of
#   CMakePresets.json define it, from the environment variable of that name, it must be the
#   absolute path of a dependency bundle (a directory that holds praenora-runtime-deps.json), or
#   the configuration stops before any dependency is looked up: those presets' CMAKE_PREFIX_PATH,
#   LLVM_INSTALL_PREFIX and PRAENORA_THIRD_PARTY_LICENSE_DIRS name the bundle's directories. A
#   tree that does not define it is not checked.
#
# Afterwards these targets exist: nlohmann_json::nlohmann_json,
# jsoncons::jsoncons (IMPORTED when it is an installed package),
# yaml-cpp::yaml-cpp, the spdlog target named by PRAENORA_SPDLOG_TARGET and, with tests,
# GTest::gtest and GTest::gtest_main. One status line per dependency reports its version and where
# it came from. PRAENORA_FETCHED_DEPENDENCIES lists, by their FetchContent names, the dependencies
# that are built in this tree, which the runtime links statically (GoogleTest, which only the
# tests link, left out), and PRAENORA_FETCHED_SOURCE_DIR_<name> holds the source directory of
# each: cmake/PraenoraInstall.cmake installs their license files.
#
# praenora_copy_runtime_dlls(<target>) is also defined here.

include_guard(GLOBAL)

# Minimum versions accepted from find_package(). yaml-cpp 0.8 is the tested minimum; 0.7 is
# accepted too, because it exports only the unnamespaced yaml-cpp target, which the shim below
# turns into yaml-cpp::yaml-cpp.
set(PRAENORA_NLOHMANN_JSON_MIN_VERSION 3.11)
set(PRAENORA_JSONCONS_MIN_VERSION 1.10.0)
set(PRAENORA_YAML_CPP_MIN_VERSION 0.7)
set(PRAENORA_SPDLOG_MIN_VERSION 1.10)
set(PRAENORA_GTEST_MIN_VERSION 1.11)

# praenora_copy_runtime_dlls(<target>)
#
# On Windows, copies the DLLs that <target> loads at run time ($<TARGET_RUNTIME_DLLS>) next to it
# after every build, so tests and tools run without PATH changes. Does nothing when the list is
# empty and on platforms without DLLs.
function(praenora_copy_runtime_dlls target)
    if(WIN32)
        set(dlls "$<TARGET_RUNTIME_DLLS:${target}>")
        set(copy "${CMAKE_COMMAND};-E;copy_if_different;${dlls};$<TARGET_FILE_DIR:${target}>")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "$<$<BOOL:${dlls}>:${copy}>"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endif()
endfunction()

# _praenora_report_dependency(<label> <package> <content> <pinned-version> <target> [<extra>])
# <package> is the find_package() name, <content> the FetchContent name and <target> a target the
# dependency provides. An IMPORTED target was found with find_package(); any other was built in
# this tree. The target decides, not <content>_SOURCE_DIR: a project built in an earlier
# configuration leaves its project() call's <name>_SOURCE_DIR in the cache, often under the same
# name, after the dependency is found again.
function(_praenora_report_dependency label package content pinned_version target)
    get_target_property(imported ${target} IMPORTED)
    if(imported)
        set(version "${${package}_VERSION}")
        set(origin "found at ${${package}_DIR}")
    else()
        # FetchContent reads local sources instead of downloading them when
        # FETCHCONTENT_SOURCE_DIR_<NAME> names them; the version shown is still the pinned one,
        # which those sources are not checked against.
        string(TOLOWER "${content}" content_lower)
        string(TOUPPER "${content}" content_upper)
        set(version "${pinned_version}")
        if(NOT "${FETCHCONTENT_SOURCE_DIR_${content_upper}}" STREQUAL "")
            set(origin "local sources at ${${content_lower}_SOURCE_DIR}")
        else()
            set(origin "fetched into ${${content_lower}_SOURCE_DIR}")
        endif()
    endif()
    message(STATUS "Praenora: dependency ${label} ${version} (${origin}${ARGN})")
endfunction()

# _praenora_record_fetched(<content> <target>)
# Appends <content> to PRAENORA_FETCHED_DEPENDENCIES and sets PRAENORA_FETCHED_SOURCE_DIR_<content>
# to its source directory, in the caller's scope, when this configuration made <content> available
# with FetchContent (_praenora_fetched) and built it in this tree: <target>, which it provides, is
# not IMPORTED, as for _praenora_report_dependency().
function(_praenora_record_fetched content target)
    if(NOT content IN_LIST _praenora_fetched)
        return()
    endif()
    get_target_property(imported ${target} IMPORTED)
    if(imported)
        return()
    endif()
    FetchContent_GetProperties(${content} SOURCE_DIR source_dir)
    list(APPEND PRAENORA_FETCHED_DEPENDENCIES ${content})
    set(PRAENORA_FETCHED_DEPENDENCIES "${PRAENORA_FETCHED_DEPENDENCIES}" PARENT_SCOPE)
    set(PRAENORA_FETCHED_SOURCE_DIR_${content} "${source_dir}" PARENT_SCOPE)
endfunction()

macro(praenora_find_dependencies)
    # The dependency bundle of the ci-* presets, checked before anything is looked up in it.
    if(DEFINED CACHE{PRAENORA_RUNTIME_DEPS})
        if(NOT IS_ABSOLUTE "${PRAENORA_RUNTIME_DEPS}" OR
           NOT EXISTS "${PRAENORA_RUNTIME_DEPS}/praenora-runtime-deps.json")
            message(FATAL_ERROR "PRAENORA_RUNTIME_DEPS ('${PRAENORA_RUNTIME_DEPS}') is not a "
                "dependency bundle: set the environment variable PRAENORA_RUNTIME_DEPS to a "
                "bundle directory, for example with cmake -P "
                "cmake/praenora_fetch_runtime_deps.cmake (docs/developer-guide.md).")
        endif()
    endif()

    # Without PRAENORA_FETCH_DEPENDENCIES every dependency comes from find_package().
    if(NOT PRAENORA_FETCH_DEPENDENCIES)
        find_package(nlohmann_json ${PRAENORA_NLOHMANN_JSON_MIN_VERSION} CONFIG REQUIRED)
        find_package(jsoncons ${PRAENORA_JSONCONS_MIN_VERSION} EXACT CONFIG REQUIRED)
        find_package(yaml-cpp ${PRAENORA_YAML_CPP_MIN_VERSION} CONFIG REQUIRED)
        find_package(spdlog ${PRAENORA_SPDLOG_MIN_VERSION} CONFIG REQUIRED)
        if(PRAENORA_BUILD_TESTS)
            find_package(GTest ${PRAENORA_GTEST_MIN_VERSION} CONFIG REQUIRED)
        endif()
        set(_praenora_gtest_package GTest)
    endif()

    # The FetchContent names made available below (none without the option).
    set(_praenora_fetched "")
    if(PRAENORA_FETCH_DEPENDENCIES)
        include(FetchContent)

        # Static dependencies are linked into the shared runtime, so fetched sub-projects build
        # position-independent code. The previous setting is restored afterwards.
        if(DEFINED CMAKE_POSITION_INDEPENDENT_CODE)
            set(_praenora_saved_pic "${CMAKE_POSITION_INDEPENDENT_CODE}")
        endif()
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)

        # Sub-project options. CMP0077 lets option() calls in older sub-projects honor them.
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        set(INSTALL_GTEST OFF)
        set(gtest_force_shared_crt ON)
        set(YAML_CPP_BUILD_TOOLS OFF)
        # No install rules of the fetched projects: they are header-only or static libraries,
        # used privately by the shared runtime, so neither cmake --install nor a wheel may pick
        # up their headers, libraries or CMake files. The option names are the upstream projects'
        # own (nlohmann/json, yaml-cpp, spdlog); they could not be checked without downloading
        # the projects. jsoncons is not built as a project (see its declaration below).
        set(JSON_Install OFF)
        set(YAML_CPP_INSTALL OFF)
        set(SPDLOG_INSTALL OFF)

        # CMake 3.25 and newer treat the fetched headers as system headers.
        set(_praenora_fetch_system "")
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
            set(_praenora_fetch_system SYSTEM)
        endif()

        # jsoncons is header-only. SOURCE_SUBDIR names a directory without a CMakeLists.txt, so
        # FetchContent_MakeAvailable() downloads the sources and does not add jsoncons' project,
        # whose install rules (the headers and a CMake package) cannot be switched off and which
        # builds its tests unless JSONCONS_BUILD_TESTS is OFF: neither cmake --install nor a wheel
        # may carry the headers (ADR-0017). The target is defined after the download.
        FetchContent_Declare(jsoncons
            GIT_REPOSITORY https://github.com/danielaparker/jsoncons.git
            GIT_TAG 9e57e421285422d7aeeb5aa7dc839f6a19121ee6 # v1.10.0
            SOURCE_SUBDIR include
            ${_praenora_fetch_system}
            FIND_PACKAGE_ARGS ${PRAENORA_JSONCONS_MIN_VERSION} EXACT CONFIG)
        if(PRAENORA_FETCH_DEPENDENCIES)
            FetchContent_Declare(nlohmann_json
                GIT_REPOSITORY https://github.com/nlohmann/json.git
                GIT_TAG 55f93686c01528224f448c19128836e7df245f72 # v3.12.0
                ${_praenora_fetch_system}
                FIND_PACKAGE_ARGS ${PRAENORA_NLOHMANN_JSON_MIN_VERSION} CONFIG)
            FetchContent_Declare(yaml-cpp
                GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
                GIT_TAG 56e3bb550c91fd7005566f19c079cb7a503223cf # yaml-cpp-0.9.0
                ${_praenora_fetch_system}
                FIND_PACKAGE_ARGS ${PRAENORA_YAML_CPP_MIN_VERSION} CONFIG)
            FetchContent_Declare(spdlog
                GIT_REPOSITORY https://github.com/gabime/spdlog.git
                GIT_TAG 79524ddd08a4ec981b7fea76afd08ee05f83755d # v1.17.0
                ${_praenora_fetch_system}
                FIND_PACKAGE_ARGS ${PRAENORA_SPDLOG_MIN_VERSION} CONFIG)
            # The content is named googletest, not GTest: GoogleTest's own gtest project would
            # otherwise clash with the gtest_SOURCE_DIR variable that FetchContent sets.
            FetchContent_Declare(googletest
                GIT_REPOSITORY https://github.com/google/googletest.git
                GIT_TAG 063de7e9578f82b369302001269680b4b1553359 # v1.18.0
                ${_praenora_fetch_system}
                FIND_PACKAGE_ARGS ${PRAENORA_GTEST_MIN_VERSION} CONFIG NAMES GTest)

            set(_praenora_fetched nlohmann_json jsoncons yaml-cpp spdlog)
            if(PRAENORA_BUILD_TESTS)
                list(APPEND _praenora_fetched googletest)
            endif()
            set(_praenora_gtest_package googletest)
        endif()
        # The fetched libraries are static, whatever a super-build's BUILD_SHARED_LIBS says: a
        # fetched shared library would be neither installed (the rules above are off) nor found
        # by an installed runtime. Each project's own shared-library option is set OFF (the names
        # are the upstream projects'; they could not be checked without downloading them), and
        # BUILD_SHARED_LIBS too, which some of them also follow, as a normal variable for these
        # projects only: it is unset right after them. This project never reads it.
        set(YAML_BUILD_SHARED_LIBS OFF)
        set(SPDLOG_BUILD_SHARED OFF)
        set(BUILD_SHARED_LIBS OFF)
        FetchContent_MakeAvailable(${_praenora_fetched})
        unset(BUILD_SHARED_LIBS)

        if(DEFINED _praenora_saved_pic)
            set(CMAKE_POSITION_INDEPENDENT_CODE "${_praenora_saved_pic}")
            unset(_praenora_saved_pic)
        else()
            unset(CMAKE_POSITION_INDEPENDENT_CODE)
        endif()
    endif()

    # Target shims for packages that export only the older, unnamespaced target names.
    if(NOT TARGET yaml-cpp::yaml-cpp)
        if(NOT TARGET yaml-cpp)
            message(FATAL_ERROR "yaml-cpp provides neither the yaml-cpp::yaml-cpp nor the "
                "yaml-cpp target.")
        endif()
        add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
    endif()
    if(DEFINED yaml-cpp_VERSION)
        if("${yaml-cpp_VERSION}" VERSION_LESS 0.8)
            message(STATUS "Praenora: yaml-cpp ${yaml-cpp_VERSION} is older than the tested "
                "minimum 0.8; it is used through the yaml-cpp target shim")
        endif()
    endif()
    # jsoncons fetched without find_package(): an interface target for its include directory.
    if(NOT TARGET jsoncons::jsoncons)
        if(NOT DEFINED jsoncons_SOURCE_DIR)
            message(FATAL_ERROR "jsoncons provides no jsoncons::jsoncons target and was not "
                "fetched.")
        endif()
        add_library(jsoncons INTERFACE)
        target_include_directories(jsoncons SYSTEM INTERFACE "${jsoncons_SOURCE_DIR}/include")
        add_library(jsoncons::jsoncons ALIAS jsoncons)
    endif()
    # spdlog: the compiled library when available, otherwise the header-only target.
    if(TARGET spdlog::spdlog)
        set(PRAENORA_SPDLOG_TARGET spdlog::spdlog)
    elseif(TARGET spdlog::spdlog_header_only)
        set(PRAENORA_SPDLOG_TARGET spdlog::spdlog_header_only)
    else()
        message(FATAL_ERROR "spdlog provides neither the spdlog::spdlog nor the "
            "spdlog::spdlog_header_only target.")
    endif()

    # The dependencies built in this tree, whose license files the installation holds.
    set(PRAENORA_FETCHED_DEPENDENCIES "")
    _praenora_record_fetched(nlohmann_json nlohmann_json::nlohmann_json)
    _praenora_record_fetched(jsoncons jsoncons::jsoncons)
    _praenora_record_fetched(yaml-cpp yaml-cpp::yaml-cpp)
    _praenora_record_fetched(spdlog ${PRAENORA_SPDLOG_TARGET})
    unset(_praenora_fetched)

    _praenora_report_dependency(nlohmann_json nlohmann_json nlohmann_json 3.12.0
        nlohmann_json::nlohmann_json)
    _praenora_report_dependency(jsoncons jsoncons jsoncons 1.10.0 jsoncons::jsoncons)
    _praenora_report_dependency(yaml-cpp yaml-cpp yaml-cpp 0.9.0 yaml-cpp::yaml-cpp)
    _praenora_report_dependency(spdlog spdlog spdlog 1.17.0 ${PRAENORA_SPDLOG_TARGET}
        "; target ${PRAENORA_SPDLOG_TARGET}")
    if(PRAENORA_BUILD_TESTS)
        _praenora_report_dependency(GTest ${_praenora_gtest_package} googletest 1.18.0 GTest::gtest)
    else()
        message(STATUS "Praenora: dependency GTest not needed (PRAENORA_BUILD_TESTS is OFF)")
    endif()
    unset(_praenora_gtest_package)
endmacro()
