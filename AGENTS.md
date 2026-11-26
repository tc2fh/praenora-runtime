# AGENTS.md

Guide for coding agents and human contributors working in `praenora-runtime`. Read it before
changing anything.

## Purpose

Praenora is a configurable system for continuous modeling and forecasting. This repository is its
integration layer:

- **Runtime (host):** `praenora_runtime` (`Praenora::Runtime`) validates a workflow configuration,
  discovers and loads modules, runs the workflow deterministically and reports status.
- **Module SDK:** `Praenora::ModuleSDK`, the C ABI v0 headers plus an optional header-only C++
  adapter, a CMake helper and an in-process test kit.
- **Protocol:** the Praenora Module Protocol (PMP) and Praenora Module Manifest (PMM) documents,
  their JSON Schemas and the protocol catalog.
- **Surfaces:** the C++ API, the `praenora` CLI and the Python package `praenora.runtime`.

Modules implement the science. They live in their own repositories and build against the SDK;
this repository holds only example and test modules.

## Status

The MVP is complete: the runtime (validation, discovery, loading, the engine with its run record,
outputs and message trace, metrics, status and health), the module SDK, the protocol schemas and
catalog, the C++ API, the `praenora` CLI, the Python bindings, the example modules and workflow,
the contract checks, the install rules and CMake package, the performance probe, the tests and the
documentation. It is verified on macOS arm64 with AppleClang (Debug, Release, AddressSanitizer
and ThreadSanitizer builds, and a Python wheel built offline). Linux x86-64 and Windows x86-64 have
presets and portable code but are not yet verified; neither are fetch-mode downloads and a wheel
build that downloads its build requirements, on any platform.

Since the MVP: service providers (engines loaded on demand, one per process, listed by
`praenora providers list`), module manifests 0.2 that declare the services a module requires, run
records 0.2 that put each service's provider and engine into the fingerprint, and the simulation
service `praenora.simulation@0` (`simulation.h`; level A simulates, level B hands out the compiled
model's functions) with the fixture provider `test-simulation`, and the model conversion service
`praenora.model-conversion@0` (`model_conversion.h`; translating one model source, such as Antimony
to SBML and back) with the fixture provider `test-conversion`. The optional provider `roadrunner`
(libRoadRunner) is built only with `PRAENORA_WITH_ROADRUNNER=ON`; it is verified on macOS arm64
with libRoadRunner 2.11.0 and LLVM 13.0.1 (every test of a Debug build; the labels `unit`,
`contract`, `negative`, `lifecycle`, `integration` and `install` of a ThreadSanitizer build). The
optional provider `antimony` (libAntimony) is built only with `PRAENORA_WITH_ANTIMONY=ON`; it is
verified on macOS arm64 with libAntimony 3.3.0, built from source against libRoadRunner 2.11.0's
dependencies (every test of a Debug build, alone and with the roadrunner provider; the labels
`unit`, `contract` and `integration` of a ThreadSanitizer build). Compiling module code through
libRoadRunner (level C) is future work, and so are hierarchical SBML output, CellML and a
WebAssembly build of the conversion engine.

Process isolation: a step's module instance can run in a module host (`praenora-module-host`, a
process of its own, with the providers its module uses), so that a crash fails only that step
(`MODULE_CRASHED`) and a call that never returns is ended by killing the module host; steps in
module hosts can share compiled models through a store in the runtime's process
(`share_compiled_models`); and on macOS a module host can apply a restricted sandbox profile to
itself (`sandbox: restricted`: its own process gets no network, no new processes and no writes
outside its own temporary directory, which limits a module but is no boundary against one meant to
do harm) ([docs/specs/process-isolation.md](docs/specs/process-isolation.md)). It is POSIX only and
verified on macOS arm64; its Linux code is not yet verified, Windows refuses process mode in this
version, and the Python wheel holds no module host. The tests of the restricted profile skip,
saying "nested sandbox:", where the tests run in a sandbox already. The contract checks still run
modules in the runtime's process (OQ-32), and process isolation and the profile on Linux and
Windows are open questions (OQ-31).

CI and releases: the GitHub Actions workflow `.github/workflows/build.yml` builds and tests every
pull request to `main` with the CI presets of the four platform keys, against the dependency
bundles that `cmake/PraenoraRuntimeDepsPin.cmake` pins, archives each installation and checks that
the archive needs only the system's libraries and its own; run by hand on `main` with the input
`publish`, it drafts a release `v<version>` with the archives, which an administrator publishes
([docs/releasing.md](docs/releasing.md)). It is written but has not run yet: Linux and Windows stay
not yet verified, and no release has been made.

## Setup and commands

**Dependencies.** CMake 3.24+; Ninja (Linux, macOS) or Visual Studio 2022 (Windows); GCC 9+,
Clang 10+, AppleClang 13+ or MSVC 19.29+; nlohmann/json 3.11+, jsoncons 1.10.0 exactly (header-only),
yaml-cpp 0.8+ (0.7 works through a target shim), spdlog 1.10+, GoogleTest 1.11+; for the
Python bindings SWIG 4.0+ and Python 3.10+ with headers.

- macOS: `brew install cmake ninja swig nlohmann-json yaml-cpp spdlog googletest`
- Ubuntu 24.04+ (not yet verified): `sudo apt install cmake ninja-build g++ nlohmann-json3-dev
  libyaml-cpp-dev libspdlog-dev libgtest-dev swig python3-dev`
- Fedora (not yet verified): `sudo dnf install cmake ninja-build gcc-c++ json-devel yaml-cpp-devel
  spdlog-devel gtest-devel swig python3-devel`
- Windows (not yet verified): Visual Studio 2022 with the libraries from vcpkg or conda-forge.
- jsoncons (the JSON Schema validator, header-only) is needed on every platform and is not
  packaged by Homebrew or conda-forge, and the versions of apt and dnf are older than the 1.7 it
  needs ([docs/platforms.md](docs/platforms.md#jsoncons)): configure with
  `-DPRAENORA_FETCH_DEPENDENCIES=ON`, or install it into a prefix of your own and list that prefix
  in `CMAKE_PREFIX_PATH`.
- Instead of installing packages, configure with `-DPRAENORA_FETCH_DEPENDENCIES=ON`: installed
  packages are still preferred and only missing ones are downloaded, at pinned commits. A default
  build never uses the network.
- jsoncons is header-only, so a sanitizer build instruments it with the rest of the runtime; no
  option is needed.
- Optional, for the roadrunner provider only: an installed libRoadRunner 2.11 (its static library
  `roadrunner-static` and the packages it links) and the LLVM 13 it was built with, never
  downloaded; [docs/platforms.md](docs/platforms.md#the-roadrunner-provider) says what to install
  and how CMake finds it.
- Optional, for the antimony provider only: libAntimony 3.3 built from source as a static library
  (`antimony-static`) against libSBML (with comp), SBMLNetwork and expat, such as libRoadRunner's
  dependencies, never downloaded; [docs/platforms.md](docs/platforms.md#the-antimony-provider) says
  how to build it and how CMake finds it.
- A dependency bundle of praenora-runtime-deps holds nlohmann/json (in its `json-schema-validator`
  directory, with the validator the runtime no longer uses), libRoadRunner, LLVM and libAntimony, built; `cmake/PraenoraRuntimeDepsPin.cmake`
  pins its release and, for each platform key of `PRAENORA_RUNTIME_DEPS_PLATFORMS` (such as
  `macos-arm64-release`), the archive's name and SHA-256.
  `cmake -DPLATFORM=<key> -DDESTINATION=<dir> -P cmake/praenora_fetch_runtime_deps.cmake`
  downloads the archive (up to 3 attempts), checks the pinned SHA-256, unpacks it into `<dir>` and
  prints `PRAENORA_RUNTIME_DEPS=<bundle>`, an absolute path (appended to the file that
  `GITHUB_ENV` names, when set); the local presets then list the bundle's directories
  ([docs/developer-guide.md](docs/developer-guide.md#using-a-dependency-bundle)). Build with the
  bundle's compiler or a newer one of the same family. A download from the release is not yet
  verified.
- Optional, for the AsyncAPI parser tests only: Node.js and `@asyncapi/parser` 3.6.6, installed
  with npm into a directory outside the checkout, never downloaded by the build;
  [docs/testing.md](docs/testing.md#the-official-asyncapi-parser) says how.

**Local presets.** Copy `CMakeUserPresets.json.example` to `CMakeUserPresets.json` (git-ignored)
and edit its hidden `dev-local` preset: `CMAKE_PREFIX_PATH` (dependency prefixes, `;`-separated),
`Python3_EXECUTABLE` and `PRAENORA_FETCH_DEPENDENCIES`. On macOS, add
`CMAKE_OSX_DEPLOYMENT_TARGET` only when your dependency binaries target a newer macOS than the
project default of 11.0 (Homebrew builds for the running release); otherwise the linker warns
about every such library. The `dev`, `dev-release`, `dev-asan` and `dev-tsan` presets inherit
`macos-arm64-*`; on Linux or Windows, inherit `linux-x64-*` or `windows-x64-*` instead. For the
roadrunner provider, add a hidden `dev-roadrunner-local` preset (the libRoadRunner and LLVM
prefixes in `CMAKE_PREFIX_PATH`, `LLVM_INSTALL_PREFIX`, `PRAENORA_WITH_ROADRUNNER=ON`) and the
presets `dev-roadrunner` and `dev-roadrunner-tsan`, which inherit it before `dev-local`
([docs/developer-guide.md](docs/developer-guide.md#the-roadrunner-provider)). For the antimony
provider, likewise a hidden `dev-antimony-local` preset (the libAntimony and libSBML prefixes,
`PRAENORA_WITH_ANTIMONY=ON`) with the presets `dev-antimony` and `dev-antimony-tsan`, and for both
providers a hidden `dev-engines-local` preset (both prefix lists, `LLVM_INSTALL_PREFIX`, both
options) with the preset `dev-engines`
([docs/developer-guide.md](docs/developer-guide.md#the-antimony-provider)).

**Configure, build, test.** Trees go to `build/<preset>`; installs go to `install/<preset>`.

```bash
cmake --preset dev                             # prints one line per dependency
cmake --build --preset dev
ctest --preset dev                             # output is shown for failing tests
./build/dev/bin/praenora version --json
ctest --preset dev -L unit                     # one label
ctest --preset dev -R '^Version\.'             # one suite (CTest names are Suite.Case)
ctest --preset dev --output-junit junit.xml    # JUnit XML in build/dev/junit.xml
ctest --preset dev -L python -V                # the Python tests, each one listed
PYTHONPATH=build/dev/python python3 -c 'import praenora.runtime as pr; print(pr.version())'
cmake --install build/dev                      # into install/dev; --prefix <dir> for another
cmake --install build/dev --component Examples # the example modules, into lib/praenora/modules
cmake --build --preset dev --target package    # its archive and .sha256, in build/dev/package
ctest --preset dev -L perf -V                  # the performance probe's JSON reports
ctest --preset dev -L isolation                # process isolation: module hosts, white-box
ctest --preset dev -L docs -V                  # the documentation checked against the tree
ctest --preset dev -L ci                       # the CI tooling: fetch script, workflow checks
./build/dev/bin/praenora providers list --provider-path build/dev/test-providers/test-simulation
ctest --preset dev-roadrunner                  # with the roadrunner provider (local presets)
ctest --preset dev-roadrunner-tsan -L contract # its concurrency checks under ThreadSanitizer
ctest --preset dev-antimony                    # with the antimony provider (local presets)
ctest --preset dev-antimony-tsan -L 'contract|integration|unit'
ctest --preset dev-engines                     # both providers, in one process too
```

**CI.** A build job of `.github/workflows/build.yml` runs these commands for its key, with
`<key>` one of `PRAENORA_RUNTIME_DEPS_PLATFORMS` and `<python>` the Python 3.12 that
`actions/setup-python` set up ([docs/releasing.md](docs/releasing.md#the-workflow)):

```bash
cmake -DPLATFORM=<key> -DDESTINATION=<runner temp>/deps -P cmake/praenora_fetch_runtime_deps.cmake
cmake --preset ci-<key> -DPython3_EXECUTABLE=<python>
cmake --build --preset ci-<key> --parallel
ctest --preset ci-<key>
cmake --build --preset ci-<key> --target package      # PACKAGE on the windows-* keys
gh workflow run build.yml --ref main -f publish=true  # by a maintainer: drafts v<version>
```

The Python line needs the interpreter the build found (`Python3_EXECUTABLE`): the extension is
built for one Python version. An installation holds the `praenora` tool, the runtime library,
the headers, the CMake package (`lib/cmake/Praenora`: `find_package(Praenora COMPONENTS Runtime
ModuleSDK)`; `ModuleSDK` alone needs no other package), the schemas and catalog
(`share/praenora`), the license files (`share/praenora/licenses`: `praenora-runtime/LICENSE`, those
of the dependencies built in the tree and the contents of the directories that
`PRAENORA_THIRD_PARTY_LICENSE_DIRS` names) and, when built, the roadrunner provider
(`lib/praenora/providers/roadrunner`) and the antimony provider (`lib/praenora/providers/antimony`);
it can be moved. The example modules are installed only with `PRAENORA_INSTALL_EXAMPLES=ON` or
`--component Examples`; the fixture providers of the tests (`build/<preset>/test-providers`) are
never installed. The target `package` (`PACKAGE` with Visual Studio) archives the default install
component, so never the example modules, into
`build/<preset>/package/praenora-runtime-<version>-<os>-<arch>-<compiler>-<config>.tar.xz`
(`.zip` on Windows), `<config>` being the configuration packaged, `release` or `debug`, with a
`.sha256` file in the format of `sha256sum` beside it
([docs/developer-guide.md](docs/developer-guide.md#archiving-an-installation)). A wheel build
(scikit-build-core) has only the Python package's install rules, and the wheel holds `LICENSE` in
its `.dist-info` (in `licenses/` there with scikit-build-core 1.1.2; 0.10 not yet verified).

Project presets: `linux-x64-{debug,release,asan,tsan}`, `macos-arm64-{debug,release,asan,tsan}` and
`windows-x64-{debug,release}`, each with configure, build and test presets. The CI presets
`ci-linux-x64-release`, `ci-macos-arm64-release`, `ci-windows-x64-release` and
`ci-windows-x64-debug` (configure, build and test; trees in `build/ci-<key>`) inherit the preset of
their key and build against the dependency bundle that the environment variable
`PRAENORA_RUNTIME_DEPS` names, with both providers, the Python bindings and warnings as errors, and
with yaml-cpp, spdlog and GoogleTest built from their pinned commits
(`CMAKE_DISABLE_FIND_PACKAGE_{yaml-cpp,spdlog,googletest}`); a configuration stops when that
variable names no bundle. They are not yet verified on Linux and Windows, nor with those three
downloaded ([docs/developer-guide.md](docs/developer-guide.md#ci-presets)); the build jobs of
`.github/workflows/build.yml` run them. Labels: `unit`, `protocol`, `contract`, `negative`,
`lifecycle`, `isolation`, `integration`, `cli`, `python`, `install`, `perf`, `docs`, `ci`. The
`isolation` label holds the white-box tests of process isolation, `ChildProcess.*` and
`Isolation.*` (`praenora_isolation_tests`), which start module hosts
(`praenora-module-host`, from `build/<preset>/libexec/praenora/`) with the fixture modules, some of
which crash on purpose, and the fixture providers, which those module hosts load; runs with steps
in module hosts are tested through the public API in the `integration` label (`IsolationRun.*`,
`EveryCall/IsolationRunCrash.*`, `Fixtures/IsolationRunProviders.*`, `IsolationValidation.*`,
`IsolationExample.*`), the validation of such steps in `negative` (`IsolationValidation.*`), and
the tool's `--isolation` in `cli` (`cli.run.isolation_*`). The tests of the restricted sandbox
profile (in `isolation` and `integration`) need a process outside any sandbox, since macOS applies
no second one: in a sandbox (an agent's or a CI service's own) they skip, saying "nested sandbox:
this process runs in a sandbox already, ...", which the `SKIP_REGULAR_EXPRESSION` of those two
executables names (`praenora_add_gtest(... SANDBOX_SKIP)`), so CTest shows them as skipped; run
`ctest --preset dev -L 'isolation|integration'` from an unsandboxed shell to run them
([docs/testing.md](docs/testing.md#module-hosts)).
The `contract` label also holds the tests of provider libraries as files,
`providers.<provider>.exports` (the library exports only the provider entry point) and
`providers.<provider>.processes` (two fresh processes print the same bytes), for `test-simulation`
and, when it is built, `roadrunner`, with `praenora_simulation_print`, and for `test-conversion`
and, when it is built, `antimony`, with `praenora_conversion_print`. It also holds
`providers.run_twice.shows_first_difference`, which checks how the driver of the `processes` tests
shows two outputs that differ, and a run that fails; and, when the roadrunner provider is built,
`RoadrunnerEngine.*` (`praenora_roadrunner_engine_tests`, the provider's engine code compiled into a
test of its own: models loaded from saved states). The `cli` label
holds the CLI driver tests, named `cli.<command>.<case>`; `cli.driver.passes_empty_arguments`, which
checks that each argument of a command reaches its program, an empty one included, and
`cli.driver.shows_failing_command`, which checks how that driver shows a failing command; and the
`CliTable.*` and `Frontend.*` unit tests of the tool's helpers and of what it shares with the Python
extension (`ctest --preset dev -L cli`). The `python` label
holds one CTest test, `python.unittest`, which runs the unittest suite of `tests/python` against the
package in `build/<preset>/python`; with `PRAENORA_SANITIZERS` set it is registered but disabled,
because an instrumented extension does not load into an interpreter that is not instrumented. So a
label-only run of a sanitizer tree (`ctest --preset dev-asan -L python`, or `dev-tsan`) runs no test
and exits 8, since the presets set `noTestsAction: error`; a full run of those trees is unaffected.
The `install` label holds `install.consumer`: its driver, `tests/install/run_install_test.cmake`,
installs the build into `build/<preset>/tests/install/work`, builds the consumer projects next to it
and lists every step it checks, among them the installed module host, the license files (this
project's `LICENSE`, those of the dependencies built in the tree and the files of the directories
that `PRAENORA_THIRD_PARTY_LICENSE_DIRS` names) and a step that the installed tool runs in the
module host, before and after the installation is moved; with the roadrunner or the antimony
provider it also checks each installed provider, which the installed tool lists as usable before
and after the installation is moved. It also holds `install.package`, whose driver,
`tests/install/check_package.cmake`, makes the archive of the target `package` with `cpack -C`
and checks its name, its `.sha256` file and its files against an installation, byte for byte,
and that there is no source package. Builds without install rules (`PRAENORA_INSTALL=OFF`) have
neither. The `perf` label holds `perf.validation_strict` and `perf.validation_off`, which run
`praenora_perf` for 2000 ticks and print its JSON report, and, except on Windows,
`perf.process_isolation`, the same with validation and every step in a module host; they fail only
when the probe fails or prints no report, never on timings. Both labels also pass in the sanitizer
trees, whose flags reach the consumer builds. The `docs` label holds `docs.consistency`, the CMake
script `tests/docs/check_docs.cmake`: every `ErrorCode` name is in `docs/specs/error-taxonomy.md`,
every `spec/schemas/*.json` name in `docs/specs/protocol.md`, every ADR in `docs/adr/README.md`,
every relative Markdown link of `README.md`, `AGENTS.md` and `docs/` resolves, and the
open-questions register has its 32 rows. The `ci` label holds `ci.fetch_runtime_deps`, the CMake
script `tests/ci/check_fetch_runtime_deps.cmake`: it runs `cmake/praenora_fetch_runtime_deps.cmake`
on fake bundle archives (`.tar.xz` and `.zip`) from `file://` URLs, with pin files of its own, and
checks the line it prints and appends to `GITHUB_ENV`'s file, what a second run reuses, that the
leftovers of a stopped run are removed, the retried download, and its errors, and that it accepts
each platform of `cmake/PraenoraRuntimeDepsPin.cmake`; it uses no network. It also holds
`ci.workflow_consistency`, the CMake script `tests/ci/check_workflow_consistency.cmake`, which
reads `.github/workflows/build.yml` as text, in the format its header comment states: its matrix
keys are the platforms of `PRAENORA_RUNTIME_DEPS_PLATFORMS`, each once and each with configure,
build and test presets `ci-<key>` in `CMakePresets.json`; each key's runner is the label of its
operating system (`ubuntu-24.04`, `macos-26`, `windows-2022`) and each `runs-on` one of these or
the matrix's runner, never a `-latest` label; and each `uses:` names a commit of 40 hexadecimal
digits followed by `# v<version>`.
Build options are in `cmake/PraenoraOptions.cmake`, among them `PRAENORA_WITH_ROADRUNNER` (OFF),
which builds the roadrunner provider; with it, the cache variable `LLVM_INSTALL_PREFIX`
(`cmake/PraenoraRoadrunner.cmake`) names the LLVM that libRoadRunner was built with;
`PRAENORA_WITH_ANTIMONY` (OFF), which builds the antimony provider
(`cmake/PraenoraAntimony.cmake`); `PRAENORA_THIRD_PARTY_LICENSE_DIRS` (empty), absolute paths
of directories whose contents every installation holds in `share/praenora/licenses/`, such as a
dependency bundle's `licenses/` (`cmake/PraenoraInstall.cmake`,
[docs/developer-guide.md](docs/developer-guide.md#license-files)); `PRAENORA_ASYNCAPI_SCHEMA`
(empty), the absolute path of a local copy of the official AsyncAPI 3.0.0 JSON Schema, never
downloaded, against which three `GraphExport.*` tests of the `protocol` label then check
`spec/asyncapi/`; and `PRAENORA_ASYNCAPI_PARSER` (empty), the absolute path of a directory into
which the official AsyncAPI parser (`@asyncapi/parser` 3.6.6, from npm) is installed, never
downloaded, with which Node.js then runs two `asyncapi.parser.*` tests of the `protocol` label on
`spec/asyncapi/`; [docs/testing.md](docs/testing.md) has the details.

## Repository map

```
CMakeLists.txt CMakePresets.json CMakeUserPresets.json.example AGENTS.md README.md
LICENSE                       the MIT license of this repository's files (ADR-0040)
.github/workflows/build.yml   the GitHub Actions workflow Build: a build job per platform key
                              against the pinned dependency bundle, and the draft release
                              (docs/releasing.md, ADR-0041)
pyproject.toml                Python distribution praenora-runtime (scikit-build-core)
cmake/                        options, versions, compiler checks, warnings, sanitizers,
                              dependencies, praenora_add_module(), praenora_embed_resources(),
                              install rules and package configuration (PraenoraInstall.cmake,
                              PraenoraConfig.cmake.in); the target package, an archive of an
                              installation (PraenoraPackage.cmake); praenora_add_provider()
                              (PraenoraProvider.cmake, not installed); the roadrunner and
                              antimony providers' dependencies (PraenoraRoadrunner.cmake,
                              PraenoraAntimony.cmake and its probe praenora_antimony_probe.cpp,
                              and what both share, PraenoraEngineDependencies.cmake); the pin of
                              the dependency bundles (PraenoraRuntimeDepsPin.cmake) and the
                              script that fetches one (praenora_fetch_runtime_deps.cmake), which
                              the build does not include
include/praenora/runtime/     public C++ API (runtime.hpp, workflow.hpp, version.hpp, error.hpp,
                              status.hpp, options.hpp; export.h is generated)
include/praenora/sdk/         SDK: abi.h, status.h, simulation.h (the simulation service),
                              model_conversion.h (the model conversion service), module.hpp,
                              testing.hpp (version.h generated)
src/runtime/                  runtime internals, one directory per component (api/, catalog/,
                              compat/, config/, core/, discovery/, document/, engine/, graph/,
                              instance/, io/, isolation/ (process isolation: the channel to a
                              module host, the runtime's side of one, child processes, the
                              module host's own server, the store of shared compiled models,
                              and the sandbox profiles), loader/, observability/,
                              platform/{posix,windows}/, provider/ (provider manifests, index and
                              loader; the internal provider interface provider_abi.h))
src/module_host/              praenora-module-host, the module host (main.cpp; POSIX only, linked
                              with the runtime's objects), installed in libexec/praenora/
src/providers/                service providers built with the runtime: roadrunner/
                              (libRoadRunner; only with PRAENORA_WITH_ROADRUNNER=ON), antimony/
                              (libAntimony; only with PRAENORA_WITH_ANTIMONY=ON), common/ (what
                              providers share: cancellable_lock.hpp, header-only, never installed)
src/sdk/                      Praenora::ModuleSDK target
src/frontend/                 what the CLI and the Python extension share: frontend.hpp
                              (praenora::frontend; header-only, internal, never installed)
src/cli/                      praenora command-line tool: main.cpp, args, output, signals,
                              commands/ (one file per command)
src/python/                   Python extension: SWIG interface praenora_runtime.i and the facade
                              (facade.hpp, facade.cpp) it wraps
python/praenora/runtime/      pure-Python package praenora.runtime (praenora: namespace package)
spec/                         schemas/, catalog/, asyncapi/ (generated AsyncAPI documents,
                              committed; refreshed with PRAENORA_UPDATE_GOLDEN=1)
examples/                     modules/estimator/, modules/predictor/ (the example modules) and
                              workflows/estimate-predict/ (configuration, data, README)
tests/                        unit/, protocol/, negative/, contract/, lifecycle/, isolation/
                              (process isolation, white-box, with the helper programs
                              praenora_isolation_parent_probe, praenora_fake_module_host,
                              praenora_isolation_short_host_probe and
                              praenora_isolation_restricted_probe),
                              integration/,
                              cli/ (CLI tests and their driver run_cli.cmake), python/ (unittest
                              suite of praenora.runtime), install/ (install tests: the driver
                              run_install_test.cmake and the projects consumer-module/ and
                              consumer-app/; the archive's driver check_package.cmake), perf/
                              (performance probe praenora_perf and its driver run_perf.cmake),
                              providers/ (tests of provider libraries:
                              check_exports.cmake, run_twice.cmake, check_run_twice.cmake (its
                              check), praenora_simulation_print and praenora_conversion_print),
                              docs/ (documentation test check_docs.cmake), ci/ (tests of the
                              bundle fetch script, check_fetch_runtime_deps.cmake, and of the
                              workflow, check_workflow_consistency.cmake), support/ (test
                              helpers, and run_command.cmake, which the CMake drivers share);
                              fixtures/: modules/ (fixture modules), providers/ (fixture
                              providers, praenora_add_fixture_provider(); test-simulation and
                              test-conversion offer the public services, test-store drives the
                              shared compiled models), models/ (sbml/ and
                              test-decay/ models of the simulation service, test-decay-text/
                              sources of the model conversion service, antimony/ Antimony models
                              and the antimony provider's expected outputs), documents/,
                              catalog/, catalog-bad/, catalog-shared-id/, manifests/ (bad module
                              and provider manifests, search-path layouts), configs/ (good, bad
                              and CLI configurations), data/ (target data), golden/ (golden
                              outputs)
tools/                        developer scripts (generate_example_data.py)
docs/                         index.md, the guides (architecture, user-guide, configuration,
                              developer-guide, module-developer-guide, testing, python,
                              platforms, releasing), specs/ (normative specifications),
                              feasibility/ (feasibility documents), adr/ (decision records),
                              open-questions.md
```

## Architecture in six bullets

- The runtime compiles once into the OBJECT library `praenora_runtime_objects`, linked into the
  shared `praenora_runtime` (only the public API is exported) and into white-box test executables.
  Its two front ends, the CLI and the Python extension, use only the public API and share their
  documents, failures and names through the header-only internal `praenora_frontend`.
- Modules are independently built `MODULE` libraries, found through `*.pmm.json` manifests on
  module search paths, loaded with `dlopen`/`LoadLibraryExW` and called only through the ABI v0
  function table returned by `praenora_module_get_api`.
- Each module instance has its own worker thread and calls into it are serialized; calls into a
  library that allows no concurrent calls (`features.concurrency: "module"`) take turns across the
  whole process, at one gate per loaded library. The host enforces timeouts; cancellation is
  cooperative. A step may run its instance in a module host instead (`praenora-module-host`, linked
  with the runtime's objects, started with `posix_spawn()` and reached over a private channel),
  which loads the step's providers too: its crash fails only that step, and a call that outlives
  its deadline and grace is ended by killing it.
- Heavy engines come from service providers: separately built `MODULE` libraries, found through
  `*.provider.json` manifests on provider search paths and loaded on demand, when a run,
  `validate --load` or the contract checks need one of their services; each is started once and
  kept loaded, so a process holds one engine per provider. Modules declare the services they
  require or can use in their manifests (PMM 0.2), reach them only through the host API's
  `get_service` and never link an engine; each service a run uses enters its fingerprint. The
  repository builds two, each only with its build option: `roadrunner` (the simulation service,
  libRoadRunner) and `antimony` (the model conversion service, libAntimony).
- A YAML workflow configuration pins a versioned workflow definition from the protocol catalog and
  binds its steps to modules; connections come from the definition, never from the YAML. Every
  document is validated against an embedded JSON Schema (draft-07, or 2020-12 where a schema
  declares it) before anything runs.
- Execution is a sequence of synchronous ticks in deterministic order; feedback uses connections
  with `delay: 1`. Each run has a fingerprint and a run record; status, metrics and logs are
  available from C++, the CLI and Python.

## Conventions

- **Language.** C++17 without extensions; C99 for the ABI headers. snake_case functions, variables
  and files; PascalCase types; internals in `praenora::detail` under `src/runtime/<component>/`;
  what both front ends need in `praenora::frontend` (`src/frontend/`), never copied into one.
  Format with `.clang-format`; `.clang-tidy` holds the static checks.
- **Errors.** Throw `praenora::Error` with an error-taxonomy code. Validation collects every
  diagnostic in a report instead of stopping at the first.
- **Public headers** use only the standard library and Praenora types (no nlohmann, spdlog or
  yaml-cpp types); third-party dependencies link PRIVATE; exports use `PRAENORA_RUNTIME_API`.
- **ABI.** Only C types cross the module boundary (fixed-width integers, `size_t`, pointers,
  length-delimited UTF-8); no C++ types, exceptions or allocator ownership. Minor ABI changes only
  append fields and function pointers.
- **Determinism.** No unordered-container iteration reaches an output; no wall-clock time in
  envelopes or reproducibility data; randomness only from seeds derived from the run seed.
  Canonical JSON is nlohmann `dump()` without indentation.
- **Text.** UTF-8 everywhere, LF line endings (`.gitattributes`, `.editorconfig`); paths travel as
  UTF-8 strings. Every `execute_process()` of the CMake code gives `ENCODING UTF-8`: without it,
  CMake on Windows decodes a program's output in the console's or the ANSI code page (CMP0176). A
  CMake script that must write a file with line feeds byte for byte does not use `file(WRITE)` or
  `file(APPEND)`, which write in text mode on Windows, a carriage return before each line feed
  (`tests/providers/check_run_twice.cmake` shows a way).
- **Tests.** Add GoogleTest executables with `praenora_add_gtest()` (`tests/CMakeLists.txt`) and
  fixture modules with `praenora_add_fixture_module()` (`tests/fixtures/modules/CMakeLists.txt`).
  Tests are named `Suite.Case` in PascalCase, where the suite names the component (for example
  `Loader.LoadsModuleFromUnicodePath`); the case may end in a `_WhenCondition` suffix. Python
  tests are stdlib `unittest` modules `tests/python/test_*.py` (classes `<Topic>Test`, methods
  `test_<what>`), which `python.unittest` discovers; what they share (CTest's environment, fixture
  paths) is in `tests/python/support.py`. Write temporary files only under the system
  temporary directory or the build tree. Keep the build warning-free
  (`PRAENORA_WARNINGS_AS_ERRORS=ON` enforces it).
- **Licenses.** This repository's files are under the MIT license (`LICENSE`, copyright The
  Praenora developers, [ADR-0040](docs/adr/ADR-0040-mit-license.md); `pyproject.toml` says so for
  the wheel). A third-party dependency linked statically brings its license files into the
  installation (`share/praenora/licenses/`): one built in the tree through the rules of
  `cmake/PraenoraInstall.cmake`, one found installed through `PRAENORA_THIRD_PARTY_LICENSE_DIRS`.
- **Agent standards.** Use `AGENTS.md` (this file) and the `agentskills.io` standard for agent skills.
  Use local symlinks as necessary for support in non-supporting agent tools.

## Change rules

- Schema, catalog and documentation changes travel together.
- **Versions are not bumped for now.** Nothing outside this repository uses the runtime, the SDK,
  the ABI, the document formats or the catalog yet, so compatibility with earlier versions is not a
  concern. Every version stays as it is: the runtime package (but for a release, below), the SDK,
  PMP, the module ABI, the provider interface, the simulation service, the model conversion
  service, each document format, each catalog entry and the modules of this repository. Make each
  change within the current version: edit the schema file, header, catalog entry, documentation,
  tests and fixtures it affects. Never raise a version number, and never add a schema file or a
  catalog entry file for a new version. This holds until the maintainers decide to bump versions
  again; until then, the bump rules of
  [docs/specs/compatibility-policy.md](docs/specs/compatibility-policy.md#changing-a-version) and
  the specifications' rules that a change is a new version describe what will apply then, not what
  to do now. One exemption, decided by the maintainer on 2026-11-21: the runtime package's version,
  `project(VERSION)` in `CMakeLists.txt`, is raised when a release is made, by the maintainers'
  decision, following [docs/releasing.md](docs/releasing.md#release-procedure); every other version
  stays. The first release is v0.2.0, made at the current version without a raise.
- **Dependency bundle pin.** `cmake/PraenoraRuntimeDepsPin.cmake` is changed only after the
  maintainer confirms the change, with every archive name and SHA-256 of the release it names, and
  together with the runner labels of `.github/workflows/build.yml` and
  `tests/ci/check_workflow_consistency.cmake` when that release of praenora-runtime-deps moved its
  jobs to other runner images, following
  [docs/releasing.md](docs/releasing.md#raising-the-deps-pin).
- An ABI, protocol or document-format change adds an ADR in `docs/adr/`. Once versions are bumped
  again, it also bumps its version (ABI: `include/praenora/sdk/abi.h`; SDK, PMP and document
  formats: `cmake/PraenoraVersions.cmake`; runtime package: `project(VERSION)` in
  `CMakeLists.txt`).
- Resolving an open question updates its row in `docs/open-questions.md` and adds or supersedes an
  ADR. Calls made while building go into the relevant ADR as "Builder notes" (Proposed).
- Golden files, and the committed AsyncAPI documents in `spec/asyncapi/`, are refreshed only with
  `PRAENORA_UPDATE_GOLDEN=1`, never implicitly; regenerate the AsyncAPI documents whenever a
  workflow definition, a message type or the envelope schema changes, and, with a local copy of
  the official AsyncAPI 3.0.0 schema at hand, check them against it (`PRAENORA_ASYNCAPI_SCHEMA`),
  and with the official AsyncAPI parser installed, through it (`PRAENORA_ASYNCAPI_PARSER`).

## Prohibitions

- No vendored dependencies: they come from `find_package` or, opt-in, pinned FetchContent.
- No release, `v*` tag or release asset made by hand: a release is a draft of
  `.github/workflows/build.yml`, which an administrator publishes
  ([docs/releasing.md](docs/releasing.md#release-procedure)).
- No network access in tests or in a default build.
- `BUILD_SHARED_LIBS` is never read: the runtime is always SHARED, modules always MODULE.
- Never commit build trees, install trees or `CMakeUserPresets.json`.

## Docs map

- [README.md](README.md): overview and quick start.
- [docs/index.md](docs/index.md): map of all documentation.
- Guides: [architecture](docs/architecture.md), [user guide](docs/user-guide.md),
  [configuration](docs/configuration.md), [developer guide](docs/developer-guide.md),
  [module developer guide](docs/module-developer-guide.md), [testing](docs/testing.md),
  [Python](docs/python.md), [platforms](docs/platforms.md), and
  [releasing](docs/releasing.md) (the CI workflow, the dependency bundle pin and the release
  procedure).
- Normative specifications: `docs/specs/` (ABI, protocol, manifest, runtime services and
  providers, the simulation service, the model conversion service, workflow definitions,
  compatibility, error taxonomy, threading and ownership, process isolation, time and timeouts,
  observability, reproducibility, the draft starting workflow), listed in
  [docs/index.md](docs/index.md); the three on services are
  [runtime-services.md](docs/specs/runtime-services.md),
  [simulation-service.md](docs/specs/simulation-service.md) and
  [model-conversion-service.md](docs/specs/model-conversion-service.md); process isolation, module
  hosts and their configuration are in [process-isolation.md](docs/specs/process-isolation.md).
- Feasibility documents: [WebAssembly](docs/feasibility/webassembly.md) is a design, nothing of
  which is built; [libRoadRunner](docs/feasibility/libroadrunner.md) holds the upstream facts
  behind the built simulation service and its provider `roadrunner`, and the design of level C
  (future work); [libAntimony](docs/feasibility/libantimony.md) holds the upstream facts behind the
  built model conversion service and its provider `antimony`, and its future work.
- [docs/adr/README.md](docs/adr/README.md): architecture decision records and their process.
- [docs/open-questions.md](docs/open-questions.md): open-questions register.
