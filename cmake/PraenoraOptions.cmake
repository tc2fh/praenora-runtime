# Build options of praenora-runtime.
#
# BUILD_SHARED_LIBS is never read: the runtime is always a SHARED library and modules are always
# MODULE libraries, whatever the surrounding build prefers.

include_guard(GLOBAL)

# True when praenora-runtime is the top-level project rather than a sub-project of another build.
set(PRAENORA_TOP_LEVEL "${PROJECT_IS_TOP_LEVEL}")

option(PRAENORA_BUILD_TESTS "Build the tests (GoogleTest, CTest labels)" ${PRAENORA_TOP_LEVEL})
option(PRAENORA_BUILD_EXAMPLES "Build the example modules and workflow" ${PRAENORA_TOP_LEVEL})
option(PRAENORA_BUILD_CLI "Build the praenora command-line tool" ON)
option(PRAENORA_BUILD_PYTHON "Build the Python bindings (SWIG >= 4.0, Python >= 3.10)" OFF)
option(PRAENORA_FETCH_DEPENDENCIES
    "Download missing dependencies at pinned versions (installed packages are used first)" OFF)
option(PRAENORA_INSTALL "Generate install rules" ${PRAENORA_TOP_LEVEL})
option(PRAENORA_INSTALL_EXAMPLES "Install the example modules" OFF)
# Directories of third-party license files, such as the licenses/ directory of a dependency bundle
# whose libraries the build links statically: with the install rules, the contents of each are
# installed into ${CMAKE_INSTALL_DATADIR}/praenora/licenses/, beside this project's LICENSE and the
# license files of the dependencies built in this tree (cmake/PraenoraInstall.cmake), and each
# entry must be the absolute path of a directory, or the configuration stops.
set(PRAENORA_THIRD_PARTY_LICENSE_DIRS "" CACHE STRING
    "Directories (;-list) whose contents are installed into share/praenora/licenses/")
# PRAENORA_INSTALL_RULES: ON when this build generates the install rules of the package
# (cmake/PraenoraInstall.cmake and the install() rules of each directory): PRAENORA_INSTALL is ON
# and the build is not a Python wheel build. scikit-build-core (which sets SKBUILD) packs every
# install rule into the wheel, so a wheel build has only the Python package's own rules
# (src/python), and the wheel holds just praenora/runtime/.
if(PRAENORA_INSTALL AND NOT SKBUILD)
    set(PRAENORA_INSTALL_RULES ON)
else()
    set(PRAENORA_INSTALL_RULES OFF)
endif()
option(PRAENORA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
# The libRoadRunner service provider (src/providers/roadrunner), which needs an installed
# libRoadRunner 2.11 and the LLVM it was built with; they are never downloaded
# (cmake/PraenoraRoadrunner.cmake, docs/platforms.md).
option(PRAENORA_WITH_ROADRUNNER
    "Build the roadrunner service provider (needs installed libRoadRunner 2.11 and LLVM)" OFF)
# The libAntimony service provider (src/providers/antimony), which needs libAntimony 3.3 built from
# source as a static library, and the libSBML and SBMLNetwork it was built against (for example
# libRoadRunner's dependencies); they are never downloaded (cmake/PraenoraAntimony.cmake,
# docs/platforms.md).
option(PRAENORA_WITH_ANTIMONY
    "Build the antimony service provider (needs libAntimony 3.3 built from source and libSBML)" OFF)
# A local copy of the official AsyncAPI 3.0.0 JSON Schema (schemas/3.0.0.json of
# github.com/asyncapi/spec-json-schemas), never downloaded: when set, the protocol tests check the
# committed AsyncAPI documents against it (tests/CMakeLists.txt, docs/testing.md).
set(PRAENORA_ASYNCAPI_SCHEMA "" CACHE FILEPATH
    "With the tests: a copy of the official AsyncAPI 3.0.0 JSON Schema to check spec/asyncapi/")
# A local installation of the official AsyncAPI parser (@asyncapi/parser, from npm), never
# downloaded: when set, the protocol tests run the committed AsyncAPI documents through it with
# Node.js (tests/CMakeLists.txt, docs/testing.md).
set(PRAENORA_ASYNCAPI_PARSER "" CACHE PATH
    "With the tests: a directory into which @asyncapi/parser is installed, to check spec/asyncapi/")
set(PRAENORA_SANITIZERS "" CACHE STRING
    "Sanitizers for GCC and Clang builds, for example address;undefined or thread")
set_property(CACHE PRAENORA_SANITIZERS PROPERTY STRINGS "" "address;undefined" "thread")
