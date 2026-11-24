# Installs a build of praenora-runtime and uses the installation from the outside: the driver of
# the install test (tests/install/CMakeLists.txt). Script mode, cmake -P, with these variables
# (lists are ;-lists; the settings may be empty):
#
#   BUILD_DIR, SOURCE_DIR       the build tree to install and its source tree
#   WORK_DIR                    scratch directory, emptied first: the installation goes to
#                               <WORK_DIR>/prefix, the consumers build in <WORK_DIR>/<consumer>
#   TEST_DIR                    tests/install, which holds the consumer projects
#   CATALOG_DIR                 the test catalog, which defines test.echo for consumer-echo
#   GENERATOR, GENERATOR_PLATFORM, GENERATOR_TOOLSET, MULTI_CONFIG, MAKE_PROGRAM
#                               the build's generator; MULTI_CONFIG is true for a multi-config one
#   C_COMPILER, CXX_COMPILER    the build's compilers
#   BUILD_TYPE                  the configuration under test
#   PREFIX_PATH                 the build's dependency prefixes (its CMAKE_PREFIX_PATH)
#   C_FLAGS, CXX_FLAGS, EXE_LINKER_FLAGS, SHARED_LINKER_FLAGS, MODULE_LINKER_FLAGS
#                               the consumers' CMAKE_<LANG>_FLAGS and CMAKE_<KIND>_LINKER_FLAGS:
#                               the build's, with its sanitizer options
#   OSX_ARCHITECTURES, OSX_DEPLOYMENT_TARGET   the build's macOS settings
#   BINDIR, LIBDIR, INCLUDEDIR, DATADIR        the build's GNUInstallDirs directories
#   WITH_CLI, WITH_EXAMPLES     the build has the praenora tool, the example modules
#   INSTALL_EXAMPLES            a default installation includes the example modules
#   PROVIDERS                   the service providers a default installation includes (the
#                               roadrunner provider, in a build with PRAENORA_WITH_ROADRUNNER,
#                               and the antimony provider, with PRAENORA_WITH_ANTIMONY)
#   LICENSE_DEPENDENCIES        the dependencies built in the tree (PRAENORA_FETCHED_DEPENDENCIES,
#                               cmake/PraenoraDependencies.cmake), whose license files the
#                               installation holds in licenses/<name>/
#   LICENSE_DIRS                PRAENORA_THIRD_PARTY_LICENSE_DIRS: the directories whose contents
#                               the installation holds in licenses/
#
# Steps, in order; the first failure ends the test, showing what ran and what it printed:
#   1. cmake --install <BUILD_DIR> --prefix <WORK_DIR>/prefix. Each kind of installed file must
#      be there, and nothing else of its kind: the tool, the runtime library, the headers (the
#      generated ones included; no template and no internal header), the package configuration,
#      the schemas and the catalog. The license files, below
#      <prefix>/<DATADIR>/praenora/licenses: this project's LICENSE at praenora-runtime/LICENSE,
#      byte for byte; at least one file in <name>/ for each of LICENSE_DEPENDENCIES; and every file
#      below each of LICENSE_DIRS at its path there, byte for byte. No file of the Python package
#      is installed, and no example module unless INSTALL_EXAMPLES. With example modules built,
#      cmake --install ... --component Examples then installs them, with their manifests. The
#      installation's provider directory holds each of PROVIDERS, its manifest and library side by
#      side, and nothing else (no directory at all without PROVIDERS). On POSIX hosts the module
#      host, <prefix>/libexec/praenora/praenora-module-host (beside <LIBDIR>:
#      <LIBDIR>/praenora/../..), is installed and executable.
#   2. consumer-module and consumer-app are configured, with uninitialized variables reported
#      (one read in the installed PraenoraConfig.cmake or PraenoraModule.cmake fails; each
#      configuration's output is kept in <WORK_DIR>/<consumer>-configure.log), and built, each
#      against <prefix>;<PREFIX_PATH>; each must have found Praenora in <prefix>. A project that
#      requires an unknown component must fail to configure, with a reason that names it, also
#      when the component is named like a CMake false constant (Off, Frobnicator-NOTFOUND); one
#      that names an unknown optional component must find Praenora, with the components it
#      requires and no other, and one that names no component must get both.
#      consumer-module then installs its module into <WORK_DIR>/module-prefix through the
#      installed praenora_add_module() (INSTALL_DESTINATION): manifest and library side by side.
#   3. consumer-app runs with the installed module's directory and CATALOG_DIR: exit 0, and
#      consumer-echo's describe document on standard output.
#   4. No file below <prefix>/<LIBDIR>/cmake/Praenora and no installed *.pmm.json (the
#      installation's and consumer-module's) or *.provider.json mentions SOURCE_DIR or
#      BUILD_DIR.
#   5. <prefix>/<BINDIR>/praenora version --json: exit 0 and a successful JSON document; with
#      PROVIDERS, praenora providers list --json, without --provider-path, lists each of them,
#      usable, from the installation's provider directory. On POSIX hosts the installed tool runs
#      the test catalog's test.single with consumer-echo (step 2's) in a module host
#      (--isolation process): the run completes, the installation's module host started (its
#      module_host.started record, at --log-level debug), and the run record says the step ran
#      in process mode.
#   6. With example modules: <prefix>/<BINDIR>/praenora modules list --json, without
#      --module-path, lists them from the installation's module directory, and
#      praenora modules check passes for each installed manifest (the module loads from the
#      installation and passes its contract checks with its installed test vectors).
#   7. The installation is moved to another directory, whose name holds a space and a non-ASCII
#      character; there it still holds the license files of step 1, and from there,
#      steps 5 and 6 succeed again, with the modules found in their new place.
# Without the tool or the example modules, the steps that need them are skipped, with a note.
# Every command runs without the PRAENORA_* variables that the runtime reads (except
# PRAENORA_DEBUG_KEEP_LIBRARIES), and a sanitizer report in its output fails the test.
cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../support/run_command.cmake")

foreach(variable IN ITEMS BUILD_DIR SOURCE_DIR WORK_DIR TEST_DIR CATALOG_DIR GENERATOR BINDIR
                          LIBDIR INCLUDEDIR DATADIR)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "run_install_test.cmake: ${variable} is required")
    endif()
endforeach()
foreach(variable IN ITEMS GENERATOR_PLATFORM GENERATOR_TOOLSET MULTI_CONFIG MAKE_PROGRAM C_COMPILER
                          CXX_COMPILER BUILD_TYPE PREFIX_PATH C_FLAGS CXX_FLAGS EXE_LINKER_FLAGS
                          SHARED_LINKER_FLAGS MODULE_LINKER_FLAGS OSX_ARCHITECTURES
                          OSX_DEPLOYMENT_TARGET WITH_CLI WITH_EXAMPLES INSTALL_EXAMPLES
                          PROVIDERS LICENSE_DEPENDENCIES LICENSE_DIRS)
    if(NOT DEFINED ${variable})
        set(${variable} "")
    endif()
endforeach()
foreach(variable IN ITEMS PRAENORA_MODULE_PATH PRAENORA_PROVIDER_PATH PRAENORA_CATALOG_PATH
                          PRAENORA_LOG_LEVEL PRAENORA_LOG_FORMAT)
    unset(ENV{${variable}})
endforeach()

set(exe "")
if(CMAKE_HOST_WIN32)
    set(exe ".exe")
endif()
set(prefix "${WORK_DIR}/prefix")
set(example_modules example-estimator example-predictor)

# _run(<prefix> <what> <command>...): praenora_test_run() (tests/support/run_command.cmake), which
# passes each argument after <what> to the program, an empty one included, and the command must
# exit with 0; its outputs go to <prefix>_OUT and <prefix>_ERR.
function(_run prefix what)
    praenora_test_run(command "${what}" "${ARGN}")
    if(NOT command_EXIT STREQUAL "0")
        praenora_test_fail("${what}" "exit code ${command_EXIT}, expected 0" "${ARGN}"
            "${command_EXIT}" "${command_OUT}" "${command_ERR}")
    endif()
    set(${prefix}_OUT "${command_OUT}" PARENT_SCOPE)
    set(${prefix}_ERR "${command_ERR}" PARENT_SCOPE)
endfunction()

# The files below `dir`, relative to it and sorted, in `out`.
function(_files_below dir out)
    file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${dir}" "${dir}/*")
    list(SORT files)
    set(${out} "${files}" PARENT_SCOPE)
endfunction()

# Fails unless the files below `installed` are exactly `expected` (relative paths).
function(_expect_files what installed expected)
    _files_below("${installed}" found)
    list(SORT expected)
    if(NOT found STREQUAL expected)
        string(REPLACE ";" "\n  " found "${found}")
        string(REPLACE ";" "\n  " expected "${expected}")
        message(FATAL_ERROR "step 1: the installed ${what} in ${installed} differ from the "
            "expected ones.\nexpected:\n  ${expected}\ninstalled:\n  ${found}\n")
    endif()
endfunction()

# Fails unless the installed file `installed` is there and equals `source`, byte for byte; `step`
# and `what` name it in messages.
function(_expect_same_file step what installed source)
    if(NOT EXISTS "${installed}" OR IS_DIRECTORY "${installed}")
        message(FATAL_ERROR "${step}: ${what} was not installed at ${installed}")
    endif()
    file(SHA256 "${installed}" installed_hash)
    file(SHA256 "${source}" source_hash)
    if(NOT installed_hash STREQUAL source_hash)
        message(FATAL_ERROR "${step}: the installed ${installed} differs from ${source} "
            "(SHA-256 ${installed_hash}, expected ${source_hash})")
    endif()
endfunction()

# Fails unless the installation in `root` holds the license files that step 1 lists: this
# project's LICENSE, a file for each of LICENSE_DEPENDENCIES and the files of LICENSE_DIRS; `step`
# names the check in messages.
function(_expect_licenses root step)
    set(licenses "${root}/${DATADIR}/praenora/licenses")
    _expect_same_file("${step}" "this project's license" "${licenses}/praenora-runtime/LICENSE"
        "${SOURCE_DIR}/LICENSE")
    message(STATUS "${step}: this project's license is installed at "
        "${licenses}/praenora-runtime/LICENSE")
    foreach(name IN LISTS LICENSE_DEPENDENCIES)
        _files_below("${licenses}/${name}" files)
        if(NOT files)
            message(FATAL_ERROR "${step}: no license file of ${name}, which is built in the tree, "
                "was installed in ${licenses}/${name}")
        endif()
        list(JOIN files ", " files)
        message(STATUS "${step}: the license files of ${name}, built in the tree: ${files}")
    endforeach()
    foreach(entry IN LISTS LICENSE_DIRS)
        if(entry STREQUAL "")
            continue()
        endif()
        cmake_path(SET dir NORMALIZE "${entry}")
        string(REGEX REPLACE "(.)/+$" "\\1" dir "${dir}")
        _files_below("${dir}" files)
        foreach(file IN LISTS files)
            _expect_same_file("${step}"
                "${dir}/${file}, of a directory that PRAENORA_THIRD_PARTY_LICENSE_DIRS names,"
                "${licenses}/${file}" "${dir}/${file}")
        endforeach()
        list(LENGTH files count)
        message(STATUS "${step}: the ${count} files of ${dir} are installed in ${licenses}")
    endforeach()
endfunction()

# Steps 5 and 6 for the installation in `root`; `step` names them in messages.
function(_check_tool root step)
    set(tool "${root}/${BINDIR}/praenora${exe}")
    _run(VERSION "${step}: praenora version" "${tool}" version --json)
    string(STRIP "${VERSION_OUT}" VERSION_OUT)
    string(JSON ok ERROR_VARIABLE json_error GET "${VERSION_OUT}" ok)
    if(json_error OR NOT ok)
        message(FATAL_ERROR "${step}: '${tool} version --json' did not print a successful JSON "
            "document:\n${VERSION_OUT}")
    endif()
    message(STATUS "${step}: ${tool} version --json:\n${VERSION_OUT}")

    # A step in a module host, the installation's own, which the runtime finds beside its library.
    if(NOT CMAKE_HOST_WIN32)
        set(isolation_out "${WORK_DIR}/isolation/out")
        file(REMOVE_RECURSE "${isolation_out}")
        _run(ISOLATION "${step}: praenora run --isolation process" "${tool}" run
            "${isolation_config}" --module-path "${module_dir}" --catalog-path "${CATALOG_DIR}"
            --isolation process --output-dir "${isolation_out}" --log-level debug --json)
        string(JSON state ERROR_VARIABLE json_error GET "${ISOLATION_OUT}" result state)
        if(json_error OR NOT state STREQUAL "completed")
            message(FATAL_ERROR "${step}: the run with --isolation process did not complete:\n"
                "${ISOLATION_OUT}\n${ISOLATION_ERR}")
        endif()
        if(NOT ISOLATION_ERR MATCHES "module host started for module consumer-echo@0\\.1\\.0")
            message(FATAL_ERROR "${step}: the run with --isolation process logged no start of a "
                "module host:\n${ISOLATION_ERR}")
        endif()
        file(READ "${isolation_out}/run-record.json" record)
        string(JSON mode ERROR_VARIABLE json_error GET "${record}" execution isolation a mode)
        if(json_error OR NOT mode STREQUAL "process")
            message(FATAL_ERROR "${step}: the run record does not say that step a ran in a module "
                "host: ${json_error}${mode}")
        endif()
        message(STATUS "${step}: the installed tool ran a step in its module host")
    endif()

    # The providers of the installation, found in its provider directory.
    if(PROVIDERS)
        _run(PROVIDERS_LIST "${step}: praenora providers list" "${tool}" providers list --json)
        string(JSON count ERROR_VARIABLE json_error LENGTH "${PROVIDERS_LIST_OUT}" result)
        if(json_error)
            message(FATAL_ERROR "${step}: 'praenora providers list --json' did not print a list "
                "of providers:\n${PROVIDERS_LIST_OUT}")
        endif()
        file(REAL_PATH "${root}/${LIBDIR}/praenora/providers" providers_dir)
        set(usable_providers "")
        if(count GREATER 0)
            math(EXPR last "${count} - 1")
            foreach(index RANGE ${last})
                string(JSON name GET "${PROVIDERS_LIST_OUT}" result ${index} name)
                string(JSON usable GET "${PROVIDERS_LIST_OUT}" result ${index} usable)
                string(JSON manifest GET "${PROVIDERS_LIST_OUT}" result ${index} manifest)
                file(TO_CMAKE_PATH "${manifest}" manifest)
                file(REAL_PATH "${manifest}" manifest)
                string(FIND "${manifest}" "${providers_dir}/" position)
                if(usable AND position EQUAL 0)
                    list(APPEND usable_providers "${name}")
                endif()
            endforeach()
        endif()
        foreach(name IN LISTS PROVIDERS)
            if(NOT name IN_LIST usable_providers)
                message(FATAL_ERROR "${step}: the installed praenora tool does not list provider "
                    "${name}, usable, from the installation's provider directory "
                    "${providers_dir}:\n${PROVIDERS_LIST_OUT}")
            endif()
        endforeach()
        list(JOIN PROVIDERS ", " listed_providers)
        message(STATUS "${step}: the installed tool lists ${listed_providers}, usable, from "
            "${providers_dir}")
    endif()

    if(NOT WITH_EXAMPLES)
        message(STATUS "${step}: modules list skipped (the build has no example modules)")
        return()
    endif()
    _run(LIST "${step}: praenora modules list" "${tool}" modules list --json)
    string(JSON count ERROR_VARIABLE json_error LENGTH "${LIST_OUT}" result)
    if(json_error)
        message(FATAL_ERROR "${step}: 'praenora modules list --json' did not print a list of "
            "modules:\n${LIST_OUT}")
    endif()
    file(REAL_PATH "${root}/${LIBDIR}/praenora/modules" modules_dir)
    set(listed "")
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(index RANGE ${last})
            string(JSON name GET "${LIST_OUT}" result ${index} name)
            string(JSON manifest GET "${LIST_OUT}" result ${index} manifest_path)
            file(TO_CMAKE_PATH "${manifest}" manifest)
            file(REAL_PATH "${manifest}" manifest)
            string(FIND "${manifest}" "${modules_dir}/" position)
            if(NOT position EQUAL 0)
                message(FATAL_ERROR "${step}: module ${name} was found at ${manifest}, outside "
                    "the installation's module directory ${modules_dir}")
            endif()
            list(APPEND listed "${name}")
        endforeach()
    endif()
    foreach(name IN LISTS example_modules)
        if(NOT name IN_LIST listed)
            message(FATAL_ERROR "${step}: the installed praenora tool does not list module "
                "${name} of the installation's module directory ${modules_dir}:\n${LIST_OUT}")
        endif()
    endforeach()
    list(JOIN listed ", " listed)
    message(STATUS "${step}: the installed tool lists ${listed} from ${modules_dir}")
    # Each installed example module loads, and passes its contract checks with its installed test
    # vectors.
    foreach(name IN LISTS example_modules)
        set(manifest "${modules_dir}/${name}/${name}.pmm.json")
        _run(CHECK "${step}: praenora modules check (${name})"
            "${tool}" modules check "${manifest}" --json)
        string(JSON passed ERROR_VARIABLE json_error GET "${CHECK_OUT}" result passed)
        if(json_error OR NOT passed)
            message(FATAL_ERROR "${step}: the contract checks of the installed module ${name} "
                "did not pass:\n${CHECK_OUT}")
        endif()
    endforeach()
    message(STATUS "${step}: the contract checks of ${listed} pass from the installation")
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(config_args "")
if(NOT BUILD_TYPE STREQUAL "")
    set(config_args --config "${BUILD_TYPE}")
endif()

# 1. The installation.
_run(INSTALL "step 1: cmake --install"
    "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}" ${config_args})
if(NOT CMAKE_HOST_WIN32)
    cmake_path(SET module_host NORMALIZE
        "${prefix}/${LIBDIR}/praenora/../../libexec/praenora/praenora-module-host")
    if(NOT EXISTS "${module_host}" OR IS_DIRECTORY "${module_host}")
        message(FATAL_ERROR "step 1: the module host was not installed at ${module_host}")
    endif()
    execute_process(COMMAND test -x "${module_host}" RESULT_VARIABLE executable ENCODING UTF-8)
    if(NOT executable EQUAL 0)
        message(FATAL_ERROR "step 1: the installed module host ${module_host} is not executable")
    endif()
    message(STATUS "step 1: the module host is installed at ${module_host}")
endif()
file(GLOB_RECURSE expected_headers LIST_DIRECTORIES false RELATIVE "${SOURCE_DIR}/include"
    "${SOURCE_DIR}/include/*.h" "${SOURCE_DIR}/include/*.hpp")
list(APPEND expected_headers praenora/runtime/export.h praenora/sdk/version.h)
_expect_files(headers "${prefix}/${INCLUDEDIR}" "${expected_headers}")
foreach(kind IN ITEMS schemas catalog)
    file(GLOB_RECURSE expected LIST_DIRECTORIES false RELATIVE "${SOURCE_DIR}/spec/${kind}"
        "${SOURCE_DIR}/spec/${kind}/*.json")
    _expect_files(${kind} "${prefix}/${DATADIR}/praenora/${kind}" "${expected}")
endforeach()
_expect_licenses("${prefix}" "step 1")
set(package_dir "${prefix}/${LIBDIR}/cmake/Praenora")
foreach(file IN ITEMS PraenoraConfig.cmake PraenoraConfigVersion.cmake PraenoraModule.cmake
                      PraenoraRuntimeTargets.cmake PraenoraModuleSDKTargets.cmake)
    if(NOT EXISTS "${package_dir}/${file}")
        message(FATAL_ERROR "step 1: ${package_dir}/${file} was not installed")
    endif()
endforeach()
file(GLOB runtime_library "${prefix}/${LIBDIR}/*praenora_runtime*")
if(NOT runtime_library)
    message(FATAL_ERROR "step 1: the runtime library was not installed in ${prefix}/${LIBDIR}")
endif()
if(WITH_CLI AND NOT EXISTS "${prefix}/${BINDIR}/praenora${exe}")
    message(FATAL_ERROR "step 1: the praenora tool was not installed in ${prefix}/${BINDIR}")
endif()
file(GLOB_RECURSE python_files LIST_DIRECTORIES false "${prefix}/*.py" "${prefix}/*_bindings*")
if(python_files)
    message(FATAL_ERROR "step 1: files of the Python package were installed: ${python_files}")
endif()
set(modules_dir "${prefix}/${LIBDIR}/praenora/modules")
# Fails unless every example module's manifest is installed; `how` names the installation.
function(_expect_example_modules how)
    foreach(name IN LISTS example_modules)
        if(NOT EXISTS "${modules_dir}/${name}/${name}.pmm.json")
            message(FATAL_ERROR "step 1: ${how} did not install module ${name} in ${modules_dir}")
        endif()
    endforeach()
endfunction()
if(NOT INSTALL_EXAMPLES AND EXISTS "${modules_dir}")
    _files_below("${modules_dir}" installed_modules)
    message(FATAL_ERROR "step 1: a default installation installed example modules, although "
        "PRAENORA_INSTALL_EXAMPLES is OFF: ${installed_modules}")
endif()
if(WITH_EXAMPLES)
    if(INSTALL_EXAMPLES)
        _expect_example_modules("a default installation (PRAENORA_INSTALL_EXAMPLES is ON)")
    endif()
    _run(EXAMPLES "step 1: cmake --install --component Examples"
        "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}" ${config_args}
        --component Examples)
    _expect_example_modules("cmake --install --component Examples")
else()
    message(STATUS "step 1: the build has no example modules; the steps with them are skipped")
endif()
set(providers_dir "${prefix}/${LIBDIR}/praenora/providers")
if(PROVIDERS)
    set(expected_providers "")
    foreach(name IN LISTS PROVIDERS)
        list(APPEND expected_providers "${name}/${name}.provider.json")
        file(GLOB provider_library LIST_DIRECTORIES false RELATIVE "${providers_dir}"
            "${providers_dir}/${name}/${name}.*")
        list(REMOVE_ITEM provider_library "${name}/${name}.provider.json")
        if(NOT provider_library)
            message(FATAL_ERROR "step 1: provider ${name}'s library was not installed beside "
                "its manifest in ${providers_dir}/${name}")
        endif()
        list(APPEND expected_providers ${provider_library})
    endforeach()
    _expect_files(providers "${providers_dir}" "${expected_providers}")
elseif(EXISTS "${providers_dir}")
    _files_below("${providers_dir}" installed_providers)
    message(FATAL_ERROR "step 1: a provider was installed, although the build has none: "
        "${installed_providers}")
endif()
_files_below("${prefix}" installed_files)
list(LENGTH installed_files installed_count)
message(STATUS "step 1: ${installed_count} files installed in ${prefix}")

# 2. The consumers, configured from an initial cache with this build's settings.
set(settings "")
function(_setting name type value)
    set(settings "${settings}set(${name} [==[${value}]==] CACHE ${type} \"\")\n" PARENT_SCOPE)
endfunction()
set(prefix_path "${prefix}")
if(NOT PREFIX_PATH STREQUAL "")
    list(APPEND prefix_path ${PREFIX_PATH})
endif()
_setting(CMAKE_PREFIX_PATH STRING "${prefix_path}")
if(NOT GENERATOR MATCHES "^Visual Studio")
    foreach(language IN ITEMS C CXX)
        if(NOT ${language}_COMPILER STREQUAL "")
            _setting(CMAKE_${language}_COMPILER FILEPATH "${${language}_COMPILER}")
        endif()
    endforeach()
endif()
if(NOT MAKE_PROGRAM STREQUAL "")
    _setting(CMAKE_MAKE_PROGRAM FILEPATH "${MAKE_PROGRAM}")
endif()
if(NOT MULTI_CONFIG AND NOT BUILD_TYPE STREQUAL "")
    _setting(CMAKE_BUILD_TYPE STRING "${BUILD_TYPE}")
endif()
if(CMAKE_HOST_APPLE)
    if(NOT OSX_ARCHITECTURES STREQUAL "")
        _setting(CMAKE_OSX_ARCHITECTURES STRING "${OSX_ARCHITECTURES}")
    endif()
    if(NOT OSX_DEPLOYMENT_TARGET STREQUAL "")
        _setting(CMAKE_OSX_DEPLOYMENT_TARGET STRING "${OSX_DEPLOYMENT_TARGET}")
    endif()
endif()
# The build's flags, empty ones included: then the consumers get exactly them, whatever CFLAGS,
# CXXFLAGS or LDFLAGS the test's environment holds.
foreach(language IN ITEMS C CXX)
    _setting(CMAKE_${language}_FLAGS STRING "${${language}_FLAGS}")
endforeach()
foreach(kind IN ITEMS EXE SHARED MODULE)
    _setting(CMAKE_${kind}_LINKER_FLAGS STRING "${${kind}_LINKER_FLAGS}")
endforeach()
set(settings_file "${WORK_DIR}/consumer-settings.cmake")
file(WRITE "${settings_file}" "${settings}")
set(generator_args -G "${GENERATOR}")
if(NOT GENERATOR_PLATFORM STREQUAL "")
    list(APPEND generator_args -A "${GENERATOR_PLATFORM}")
endif()
if(NOT GENERATOR_TOOLSET STREQUAL "")
    list(APPEND generator_args -T "${GENERATOR_TOOLSET}")
endif()
# Uninitialized variables are reported for files outside the consumer's own trees, such as the
# installed ones, only with --check-system-vars (CMake's own modules then report many, which are
# not this test's business). CMake 4.4 renamed --warn-uninitialized to -Wuninitialized.
set(uninitialized_args -Wuninitialized --check-system-vars)
if(CMAKE_VERSION VERSION_LESS 4.4)
    set(uninitialized_args --warn-uninitialized --check-system-vars)
endif()
foreach(consumer IN ITEMS consumer-module consumer-app)
    set(consumer_dir "${WORK_DIR}/${consumer}")
    _run(CONFIGURE "step 2: configuring ${consumer}"
        "${CMAKE_COMMAND}" -S "${TEST_DIR}/${consumer}" -B "${consumer_dir}" -C "${settings_file}"
        ${generator_args} ${uninitialized_args})
    set(configure_log "${WORK_DIR}/${consumer}-configure.log")
    file(WRITE "${configure_log}" "${CONFIGURE_OUT}${CONFIGURE_ERR}")
    # The package files that this project writes; CMake generates the others. A warning names its
    # file as "at <file>:<line> (<command>):" or, for a read in an elseif() condition for example,
    # "in <file>:".
    string(REGEX MATCHALL "CMake Warning \\([a-z]+\\) (at|in) [^\n]*\n +uninitialized variable"
        uninitialized "${CONFIGURE_OUT}${CONFIGURE_ERR}")
    foreach(warning IN LISTS uninitialized)
        foreach(file IN ITEMS PraenoraConfig.cmake PraenoraModule.cmake)
            string(FIND "${warning}" "${package_dir}/${file}:" position)
            if(NOT position EQUAL -1)
                message(FATAL_ERROR "step 2: configuring ${consumer} reads an uninitialized "
                    "variable in the installed ${file}:\n${warning}\n(the whole output is in "
                    "${configure_log})")
            endif()
        endforeach()
    endforeach()
    file(STRINGS "${consumer_dir}/CMakeCache.txt" found_dir REGEX "^Praenora_DIR:")
    string(REGEX REPLACE "^Praenora_DIR:[A-Z]+=" "" found_dir "${found_dir}")
    if(NOT found_dir STREQUAL package_dir)
        message(FATAL_ERROR "step 2: ${consumer} found Praenora in '${found_dir}', not in the "
            "installation's ${package_dir}")
    endif()
    _run(BUILD "step 2: building ${consumer}"
        "${CMAKE_COMMAND}" --build "${consumer_dir}" ${config_args})
    message(STATUS "step 2: ${consumer} built against ${package_dir}")
endforeach()
# These projects enable no language: a compiler check fails in the one whose directory name ends in
# -NOTFOUND (with the Visual Studio generator), and CMake searches <prefix>/lib64/cmake only once a
# language is enabled. So they name the installed package's directory (Praenora_DIR, with the
# build's own LIBDIR) instead of leaving it to a search of the prefix, which would not find an
# installation made to lib64, as GNUInstallDirs does on Fedora, RHEL, Rocky and openSUSE. The
# consumers above search the prefix.
# A project that requires an unknown component fails to configure, and the reason the package
# gives names that component and the package's components. The unknown component is the last one
# each project names. Off and Frobnicator-NOTFOUND are named like CMake false constants: if()
# reads a variable that holds just Off, or a list that ends in -NOTFOUND, as false, and takes an
# argument that ends in -NOTFOUND for a false constant without reading the variable it names.
foreach(components IN ITEMS "Runtime Frobnicator" "Off" "Runtime Frobnicator-NOTFOUND")
    string(REGEX MATCH "[^ ]+$" unknown "${components}")
    set(unknown_dir "${WORK_DIR}/unknown-component-${unknown}")
    file(WRITE "${unknown_dir}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\n"
        "project(praenora_unknown_component LANGUAGES NONE)\n"
        "find_package(Praenora 0.2 REQUIRED COMPONENTS ${components})\n")
    # The command is run and, on a failure, shown from this one list.
    set(unknown_command "${CMAKE_COMMAND}" -S "${unknown_dir}" -B "${unknown_dir}/build"
        -C "${settings_file}" "-DPraenora_DIR=${package_dir}" ${generator_args})
    praenora_test_run(UNKNOWN "step 2: an unknown component (${components})"
        "${unknown_command}")
    # CMake wraps the reason's lines.
    string(REGEX REPLACE "[ \t\r\n]+" " " unknown_output "${UNKNOWN_OUT}${UNKNOWN_ERR}")
    string(FIND "${unknown_output}" "Reason given by package: " reason_position)
    if(reason_position EQUAL -1)
        set(unknown_reason "")
    else()
        string(SUBSTRING "${unknown_output}" ${reason_position} -1 unknown_reason)
    endif()
    if(UNKNOWN_EXIT STREQUAL "0" OR NOT unknown_reason MATCHES "${unknown}"
       OR NOT unknown_reason MATCHES "Runtime" OR NOT unknown_reason MATCHES "ModuleSDK")
        set(problem "it did not fail with a reason that names the component ${unknown} and the ")
        string(APPEND problem "components Runtime and ModuleSDK")
        praenora_test_fail("step 2: requiring ${components}" "${problem}" "${unknown_command}"
            "${UNKNOWN_EXIT}" "${UNKNOWN_OUT}" "${UNKNOWN_ERR}")
    endif()
    message(STATUS "step 2: requiring ${components}: ${unknown} is not found, and the reason "
        "names it")
endforeach()
# A project that names an unknown optional component finds Praenora, with the components it
# requires and no other.
set(optional_dir "${WORK_DIR}/unknown-optional-component")
file(WRITE "${optional_dir}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\n"
    "project(praenora_unknown_optional_component LANGUAGES NONE)\n"
    "find_package(Praenora 0.2 REQUIRED COMPONENTS Runtime\n"
    "    OPTIONAL_COMPONENTS Frobnicator-NOTFOUND)\n"
    "if(NOT TARGET Praenora::Runtime OR TARGET Praenora::ModuleSDK\n"
    "   OR Praenora_Frobnicator-NOTFOUND_FOUND)\n"
    "    message(FATAL_ERROR \"expected Praenora::Runtime alone and no Frobnicator-NOTFOUND\")\n"
    "endif()\n")
_run(OPTIONAL "step 2: an unknown optional component"
    "${CMAKE_COMMAND}" -S "${optional_dir}" -B "${optional_dir}/build" -C "${settings_file}"
    "-DPraenora_DIR=${package_dir}" ${generator_args})
message(STATUS "step 2: requiring Runtime, with the optional component Frobnicator-NOTFOUND: "
    "Praenora is found, with Praenora::Runtime alone")
# A project that names no component gets both.
set(none_dir "${WORK_DIR}/no-component")
file(WRITE "${none_dir}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\n"
    "project(praenora_no_component LANGUAGES NONE)\n"
    "find_package(Praenora 0.2 REQUIRED)\n"
    "if(NOT TARGET Praenora::Runtime OR NOT TARGET Praenora::ModuleSDK)\n"
    "    message(FATAL_ERROR \"expected Praenora::Runtime and Praenora::ModuleSDK\")\n"
    "endif()\n")
_run(NONE "step 2: no component"
    "${CMAKE_COMMAND}" -S "${none_dir}" -B "${none_dir}/build" -C "${settings_file}"
    "-DPraenora_DIR=${package_dir}" ${generator_args})
message(STATUS "step 2: naming no component gives Praenora::Runtime and Praenora::ModuleSDK")
# consumer-module installs its module with the installed praenora_add_module()
# (INSTALL_DESTINATION), into a prefix of its own: its manifest and library, side by side.
set(module_prefix "${WORK_DIR}/module-prefix")
_run(MODULE_INSTALL "step 2: cmake --install consumer-module"
    "${CMAKE_COMMAND}" --install "${WORK_DIR}/consumer-module" --prefix "${module_prefix}"
    ${config_args})
file(GLOB_RECURSE module_manifest LIST_DIRECTORIES false "${module_prefix}/*.pmm.json")
list(LENGTH module_manifest module_manifest_count)
if(NOT module_manifest_count EQUAL 1)
    message(FATAL_ERROR "step 2: consumer-module installed ${module_manifest_count} manifests, "
        "expected 1: ${module_manifest}")
endif()
get_filename_component(module_dir "${module_manifest}" DIRECTORY)
file(GLOB module_library LIST_DIRECTORIES false "${module_dir}/consumer-echo.*")
list(REMOVE_ITEM module_library "${module_manifest}")
if(NOT module_library)
    message(FATAL_ERROR "step 2: consumer-module installed no library beside ${module_manifest}")
endif()
message(STATUS "step 2: consumer-module installed consumer-echo into ${module_dir}")

# 3. The application loads the installed module.
set(app "${WORK_DIR}/consumer-app/consumer_app${exe}")
if(MULTI_CONFIG)
    set(app "${WORK_DIR}/consumer-app/${BUILD_TYPE}/consumer_app${exe}")
endif()
if(CMAKE_HOST_WIN32)
    # Windows has no RPATH: the runtime library and the DLLs it needs are in <prefix>/<BINDIR>.
    file(TO_NATIVE_PATH "${prefix}/${BINDIR}" native_bindir)
    set(ENV{PATH} "${native_bindir};$ENV{PATH}")
endif()
_run(APP "step 3: consumer_app" "${app}" "${module_dir}" "${CATALOG_DIR}")
string(STRIP "${APP_OUT}" APP_OUT)
string(JSON name ERROR_VARIABLE json_error GET "${APP_OUT}" name)
if(json_error OR NOT name STREQUAL "consumer-echo")
    message(FATAL_ERROR "step 3: consumer_app did not print the describe document of "
        "consumer-echo:\n${APP_OUT}")
endif()
message(STATUS "step 3: consumer_app loaded the installed consumer-echo: ${APP_OUT}")

# 4. Nothing installed names the source or build tree.
file(GLOB_RECURSE scanned LIST_DIRECTORIES false "${package_dir}/*")
file(GLOB_RECURSE manifests LIST_DIRECTORIES false "${prefix}/*.pmm.json"
    "${prefix}/*.provider.json")
list(APPEND manifests "${module_manifest}")
list(APPEND scanned ${manifests})
set(forbidden "")
foreach(directory IN ITEMS "${BUILD_DIR}" "${SOURCE_DIR}")
    file(REAL_PATH "${directory}" real)
    file(TO_NATIVE_PATH "${directory}" native)
    list(APPEND forbidden "${directory}" "${real}" "${native}")
endforeach()
list(REMOVE_DUPLICATES forbidden)
foreach(file IN LISTS scanned)
    file(READ "${file}" text)
    foreach(directory IN LISTS forbidden)
        string(FIND "${text}" "${directory}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR "step 4: ${file} mentions ${directory}, which is the build or "
                "the source tree; installed files must not")
        endif()
    endforeach()
endforeach()
list(LENGTH scanned scanned_count)
list(LENGTH manifests manifest_count)
message(STATUS "step 4: ${scanned_count} files scanned (${manifest_count} manifests); none names "
    "the build or the source tree")

# 5 and 6. The installed tool. Its run in a module host uses this configuration of the test
# catalog's test.single, bound to consumer-echo.
if(NOT WITH_CLI)
    message(STATUS "steps 5 to 7: skipped (the build has no praenora tool)")
    return()
endif()
set(isolation_config "${WORK_DIR}/isolation/workflow.yaml")
file(WRITE "${WORK_DIR}/isolation/source.json" "{\"value\": 42}\n")
file(WRITE "${isolation_config}" "kind: workflow-config\n"
    "format_version: \"0.1.0\"\n"
    "workflow: { name: test.single, version: \"0.1.0\" }\n"
    "run: { ticks: 2 }\n"
    "inputs:\n"
    "  source: { source: json-document, path: source.json }\n"
    "steps:\n"
    "  a: { module: { name: consumer-echo, version: \"0.1.0\" } }\n")
_check_tool("${prefix}" "steps 5 and 6")

# 7. The installation, moved.
set(moved "${WORK_DIR}/moved prëfix")
file(RENAME "${prefix}" "${moved}")
_expect_licenses("${moved}" "step 7")
_check_tool("${moved}" "step 7")
message(STATUS "step 7: the installation works from ${moved}")
