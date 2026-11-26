# Module developer guide

This guide is for teams that write Praenora modules: libraries, built in their own repositories,
that implement one or more capabilities of the protocol catalog and that the runtime loads at run
time. It covers the module SDK, the rules of the ABI, the C++ adapter, the manifest,
`praenora_add_module()`, runtime services and the simulation service, the in-process test kit, test
vectors, the contract checks, the determinism and portability rules, and versioning. The normative
rules are in [specs/module-abi.md](specs/module-abi.md), [specs/manifest.md](specs/manifest.md) and
[specs/threading-and-ownership.md](specs/threading-and-ownership.md).

The commands were run on macOS arm64 against an installation of this repository's `dev` build.
Linux and Windows are not yet verified ([platforms.md](platforms.md)).

## What a module is

- A library of type `MODULE` (`<name>.so` on Linux and macOS, `<name>.dll` on Windows) that exports
  one C function, `praenora_module_get_api`, which returns a static table of C function pointers:
  ABI v0. Nothing else crosses between the runtime and the module: no C++ types, no exceptions, no
  allocations that the other side frees.
- A manifest, `<name>.pmm.json`, beside the library: the module's name and version, the ABI,
  protocol and SDK versions it was built for, its platform, the capabilities it implements and the
  JSON Schema of its parameters. The runtime reads manifests to find and check modules without
  running any of their code.
- Optionally, test vectors: inputs and expected outputs that the contract checks run.

A module implements capabilities; it does not choose its inputs or its place in a workflow. A
workflow definition gives each step a capability, and a workflow configuration binds the step to a
module that implements it, so modules of the same capability are interchangeable.

## The SDK

The SDK is the CMake package component `ModuleSDK` of an installation of `praenora-runtime`. It
needs only a C or C++ compiler: no runtime library and no third-party package.

| Part | Content |
|---|---|
| `praenora/sdk/abi.h` | The C ABI v0: the entry point, the function table, the host API, sinks (C99). |
| `praenora/sdk/status.h` | The status codes `PRAENORA_STATUS_*` and `praenora_status_name()` (C99). |
| `praenora/sdk/simulation.h` | The table of the simulation service `praenora.simulation@0` (C99; [Using the simulation service](#using-the-simulation-service)). |
| `praenora/sdk/model_conversion.h` | The table of the model conversion service `praenora.model-conversion@0` (C99; [Using the model conversion service](#using-the-model-conversion-service)). |
| `praenora/sdk/version.h` | The SDK and protocol versions the SDK was built with. |
| `praenora/sdk/module.hpp` | The optional C++ adapter (header-only, C++17 standard library only). |
| `praenora/sdk/testing.hpp` | The in-process test kit (header-only). |
| `PraenoraModule.cmake` | `praenora_add_module()`, loaded by `find_package`. |

```cmake
find_package(Praenora 0.2 REQUIRED COMPONENTS ModuleSDK)
```

gives the target `Praenora::ModuleSDK` and `praenora_add_module()`. The package accepts every
0.2.z installation (`SameMinorVersion` while the version is 0.x).

## A first module

This example implements the capability `example.estimate@0.1.0`, which the example workflow's
`estimate` step needs, in a simpler way than the example module: each tick, every series and
variable is estimated by its latest usable observation. It uses nlohmann/json to read and write
JSON, as the example modules do; the SDK itself parses no JSON, so any library will do.

First install `praenora-runtime` somewhere, from its build tree, with the example modules (the
commands of this section run from a checkout of this repository):

```bash
PREFIX="${TMPDIR:-/tmp}/praenora-install"
cmake --install build/dev --prefix "$PREFIX"
cmake --install build/dev --prefix "$PREFIX" --component Examples
```

Then create the module's directory, for example `${TMPDIR:-/tmp}/last-value-estimator`, with these
files.

`CMakeLists.txt`: the library, its manifest and its test vectors, and an in-process test:

```cmake
cmake_minimum_required(VERSION 3.24)
project(last_value_estimator VERSION 0.1.0 LANGUAGES CXX)

find_package(Praenora 0.2 REQUIRED COMPONENTS ModuleSDK)
find_package(nlohmann_json 3.11 REQUIRED)

praenora_add_module(last_value_estimator
    NAME last-value-estimator
    VERSION 0.1.0
    CAPABILITIES example.estimate@0.1.0
    SOURCES last_value.cpp
    MANIFEST_TEMPLATE last-value-estimator.pmm.json.in
    TEST_VECTORS vectors.json
    LINK_LIBRARIES nlohmann_json::nlohmann_json)

# In-process tests with the SDK's test kit (praenora/sdk/testing.hpp): no library is loaded.
enable_testing()
find_package(GTest 1.11 REQUIRED)
include(GoogleTest)
add_executable(last_value_test last_value_test.cpp)
target_link_libraries(last_value_test PRIVATE
    Praenora::ModuleSDK nlohmann_json::nlohmann_json GTest::gtest_main)
gtest_discover_tests(last_value_test)
```

`last_value.hpp`: the module class, in a header so that the test can include it too:

```cpp
// last-value-estimator: the capability example.estimate@0.1.0. Each tick, every series and
// variable is estimated by its latest usable observation in the tick's batch.
#ifndef LAST_VALUE_HPP
#define LAST_VALUE_HPP

#include <praenora/sdk/module.hpp>

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace last_value {

namespace sdk = praenora::sdk;
using nlohmann::json;

class LastValue final : public sdk::Module {
public:
    explicit LastValue(const sdk::CreateContext& context) : host_(context.host) {}

    std::string invoke(std::string_view invocation_json) override {
        const json invocation = json::parse(invocation_json, nullptr, false);
        if (!invocation.is_object()) {
            throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT, "the invocation is not JSON");
        }
        try {
            const json& batch = invocation.at("inputs").at("observations").at("data");
            // Rows arrive sorted by time, so the last usable row of a key is its latest. A
            // std::map keeps the estimates in a deterministic order.
            std::map<std::pair<std::string, std::string>, json> latest;
            for (const json& row : batch.at("observations")) {
                if (row.value("missing", false) || row.value("censoring", "none") != "none" ||
                    !row.at("value").is_number()) {
                    continue;
                }
                latest[{row.at("series_id").get<std::string>(),
                        row.at("variable_id").get<std::string>()}] = row;
            }
            json estimates = json::array();
            for (const auto& [key, row] : latest) {
                estimates.push_back({{"series_id", key.first},
                                     {"variable_id", key.second},
                                     {"time", row.at("time")},
                                     {"value", row.at("value")},
                                     {"basis", "observation"}});
            }
            if (host_.log_enabled(sdk::LogLevel::debug)) {
                host_.log(sdk::LogLevel::debug, std::to_string(estimates.size()) + " estimates");
            }
            json state = {{"time_unit", batch.at("time_unit")}, {"estimates", estimates}};
            return json{{"outputs", {{"state", {{"data", std::move(state)}}}}}}.dump();
        } catch (const json::exception& error) {
            throw sdk::Error(PRAENORA_STATUS_DATA_INVALID, error.what());
        }
    }

private:
    sdk::Host host_;
};

} // namespace last_value

#endif // LAST_VALUE_HPP
```

`last_value.cpp`: the library's one source file, which defines the exported entry point:

```cpp
// The module's library: its single exported entry point, praenora_module_get_api.
#include "last_value.hpp"

PRAENORA_SDK_DEFINE_MODULE(last_value::LastValue)
```

`last-value-estimator.pmm.json.in`: the manifest template; `praenora_add_module()` fills in the
`@...@` variables:

```json
{
  "kind": "module-manifest",
  "format_version": "@PRAENORA_MODULE_MANIFEST_FORMAT_VERSION@",
  "module": {
    "name": "@PRAENORA_MODULE_NAME@",
    "version": "@PRAENORA_MODULE_VERSION@",
    "summary": "Estimates each series by its latest usable observation."
  },
  "abi": { "major": @PRAENORA_ABI_MAJOR@, "minor": @PRAENORA_ABI_MINOR@ },
  "pmp_version": "@PRAENORA_PMP_VERSION@",
  "sdk_version": "@PRAENORA_SDK_VERSION@",
  "library": { "path": "@PRAENORA_MODULE_LIBRARY_FILE@" },
  "platform": { "os": "@PRAENORA_MODULE_OS@", "arch": "@PRAENORA_MODULE_ARCH@" },
  "capabilities": @PRAENORA_MODULE_CAPABILITIES_JSON@,
  "parameters_schema": {
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "additionalProperties": false
  },
  "features": { "concurrency": "instance" },
  "tests": { "vectors": "@PRAENORA_MODULE_TEST_VECTORS@" }
}
```

`vectors.json`: one case of one tick, with the expected output:

```json
{
  "kind": "module-test-vectors",
  "format_version": "0.1.0",
  "cases": [
    {
      "name": "latest-observation-wins",
      "capability": { "id": "example.estimate", "version": "0.1.0" },
      "time": { "unit": "d", "start": 0.0, "period": 1.0 },
      "ticks": [
        {
          "inputs": {
            "observations": {
              "window": { "start": 0.0, "end": 1.0 },
              "time_unit": "d",
              "observations": [
                { "series_id": "S01", "variable_id": "population", "time": 0.25, "value": 10.0 },
                { "series_id": "S02", "variable_id": "population", "time": 0.5, "value": null,
                  "missing": true },
                { "series_id": "S01", "variable_id": "population", "time": 0.75, "value": 12.5 }
              ]
            }
          },
          "expect": {
            "outputs": {
              "state": {
                "time_unit": "d",
                "estimates": [
                  { "series_id": "S01", "variable_id": "population", "time": 0.75,
                    "value": 12.5, "basis": "observation" }
                ]
              }
            }
          }
        }
      ]
    }
  ]
}
```

`last_value_test.cpp`: the module driven in process by the test kit:

```cpp
// The module driven in process by the SDK's test kit: FakeHost stands in for the runtime's host
// API and DirectModule calls the module's function table directly.
#define PRAENORA_MODULE_NAME "last-value-estimator"
#define PRAENORA_MODULE_VERSION "0.1.0"
#define PRAENORA_MODULE_CAPABILITIES "[{\"id\":\"example.estimate\",\"version\":\"0.1.0\"}]"

#include "last_value.hpp"

#include <praenora/sdk/testing.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
PRAENORA_SDK_DEFINE_MODULE_GETTER(last_value::LastValue, last_value_get_api)
} // namespace

using praenora::sdk::testing::DirectModule;
using praenora::sdk::testing::FakeHost;

TEST(LastValue, KeepsTheLatestUsableObservation) {
    FakeHost host;
    DirectModule module(&last_value_get_api);
    ASSERT_EQ(module.create(host, R"({"parameters": {}})").status, PRAENORA_STATUS_OK);
    const auto result = module.invoke(R"({"inputs": {"observations": {"data": {
        "time_unit": "d", "observations": [
          {"series_id": "S01", "variable_id": "x", "time": 0.25, "value": 1.0},
          {"series_id": "S01", "variable_id": "x", "time": 0.75, "value": 2.0}]}}}})");
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error;
    const auto state = nlohmann::json::parse(result.output)["outputs"]["state"]["data"];
    EXPECT_EQ(state["estimates"].size(), 1u);
    EXPECT_EQ(state["estimates"][0]["value"], 2.0);
}

TEST(LastValue, RefusesAMalformedInvocation) {
    FakeHost host;
    DirectModule module(&last_value_get_api);
    ASSERT_EQ(module.create(host, R"({"parameters": {}})").status, PRAENORA_STATUS_OK);
    EXPECT_EQ(module.invoke("{").status, PRAENORA_STATUS_INVALID_ARGUMENT);
}
```

Build it against the installation, run its tests, and check it with the installed tool:

```bash
MODULE="${TMPDIR:-/tmp}/last-value-estimator"
cmake -S "$MODULE" -B "$MODULE/build" -G Ninja -DCMAKE_PREFIX_PATH="$PREFIX"
cmake --build "$MODULE/build"
ctest --test-dir "$MODULE/build"
"$PREFIX/bin/praenora" modules list --module-path "$MODULE/build"
"$PREFIX/bin/praenora" modules check "$MODULE/build/last-value-estimator/last-value-estimator.pmm.json"
```

`CMAKE_PREFIX_PATH` also needs the prefixes of nlohmann/json and GoogleTest when CMake does not find
them by itself (it found Homebrew's here). The build writes
`build/last-value-estimator/last-value-estimator.so`, its manifest and a copy of the vectors. The
check ends with `17 checks: 16 passed, 0 failed, 1 skipped` (the module has no `self_test`).

The module can now replace the example estimator. A copy of the example configuration binds the
`estimate` step to it; the installed tool finds the example predictor in its installation's module
directory and the new module in its build directory:

```bash
WF="${TMPDIR:-/tmp}/last-value-workflow"
mkdir -p "$WF/data"
cp examples/workflows/estimate-predict/data/observations.csv "$WF/data/"
sed -e 's/name: example-estimator/name: last-value-estimator/' \
    -e 's/parameters: { gain: 0.6 }/parameters: {}/' \
    examples/workflows/estimate-predict/workflow.yaml > "$WF/workflow.yaml"
"$PREFIX/bin/praenora" run "$WF/workflow.yaml" --module-path "$MODULE/build" --ticks 3
```

## ABI rules

The ABI is specified in [specs/module-abi.md](specs/module-abi.md); the C++ adapter follows these
rules for you, and a module written in C follows them itself.

**Entry point and table.** The library exports `praenora_module_get_api(uint32_t abi_major)`. For
major 0 it returns a pointer to a static `praenora_module_api_v0` whose header says ABI 0.0, its
`struct_size` and `reserved` 0, with the required functions `describe`, `create`, `initialize`,
`invoke`, `finalize` and `destroy` set; `diagnostics` and `self_test` are optional (NULL). For any
other major it returns NULL. Both the entry point and `describe` must be thread-safe and free of
side effects.

**Lifecycle.** For each step, the runtime calls `create` with the create document (step,
capability, module and the validated parameters, defaults filled in), then `initialize` with the
initialize document (run fingerprint, the step's seed, the time base), `invoke` once per tick with
the invocation document (tick, logical time, the inputs as envelopes; absent optional inputs left
out), `finalize` once at the end if `initialize` succeeded, and `destroy` exactly once for every
instance `create` returned. A result document is `{"outputs": {"<port>": {"data": ...}},
"diagnostics": {...}}`: the runtime builds the envelopes, so a module returns only the data of each
output port. Every required output must be present, and only declared ports may appear
([specs/protocol.md](specs/protocol.md#module-abi-documents)).

**Ownership.** Inputs (`praenora_bytes`, the host API pointer) belong to the runtime: copy what you
keep beyond the call (the host API stays valid until `destroy` returns). Outputs go through sinks:
call `sink->write` as often as needed; the runtime copies the bytes before `write` returns, so your
buffers stay yours. A sink is valid only during its call. An instance is allocated by `create` and
freed by `destroy` with your own allocator. Nothing else crosses: no `malloc`/`free` pairs across
the boundary, no `FILE*`, no C++ objects, no exceptions, no `longjmp`. A result may be at most
256 MiB; beyond that the sink refuses the write with `PRAENORA_STATUS_RESOURCE_EXHAUSTED`, which the
module should return.

**Threading.** The runtime makes every call for one instance, from `create` to `destroy`, on one
thread of its own (`praenora:<step>`), never two at once. When the manifest's
`features.concurrency` is `"module"` (the default), different instances of a library are never
called concurrently anywhere in the process: two workflows that run at the same time and use your
library take turns calling it, and so do the contract checks. `"instance"` declares instances
independent; only `self_test` then waits for the other calls. While a call into your library that
became unresponsive may still be running, the runtime makes no call that would have to wait for it
(with `"module"`, no call into the library at all) and does not load the library for such calls
([specs/threading-and-ownership.md](specs/threading-and-ownership.md#concurrency-across-instances)).
Global state of a `"module"` library is still shared by the runs of a process, so keep results
independent of it, or expect them to depend on the order of the runs' calls. The host callbacks
(`log`, `log_enabled`, `cancel_requested`, `get_service`) are thread-safe and may be called from
any thread of the module while the instance exists. A module that starts threads must join them
before `destroy` returns.

**Deadlines and cancellation.** Each call has a deadline (`invoke` 300 s and other calls 60 s by
default, set by the configuration). When it passes, `cancel_requested()` returns nonzero for that
call: a module with long loops should poll it and return `PRAENORA_STATUS_CANCELLED` soon. A call
that still has not returned after the cancellation grace makes the instance unresponsive: the
runtime abandons it, never destroys it and keeps its library loaded, and an unresponsive `create`,
`initialize` or `invoke` fails the run, while an unresponsive `finalize` or `destroy` is its step's
last error (`MODULE_UNRESPONSIVE`, naming the call in `details.phase`). The time a call waits for
its turn at the library is not part of its deadline.

**Errors.** A failing call returns a status: a general code from 1 to 99 (`PRAENORA_STATUS_*`, 1 to
12 defined) or a code of the module's own from `PRAENORA_STATUS_MODULE_BASE` (10000) up, never one
from 100 to 9999. It should write an error document to its error sink:
`{"code": <int>, "message": "<text>", "details": {...}}`. The runtime reports the failure as
`STEP_FAILED` (or `STEP_TIMEOUT`, `OUTPUT_INVALID`) with the module's document as its `cause`.

| Status | Use it for |
|---|---|
| `PRAENORA_STATUS_INVALID_ARGUMENT` (2) | A document the module cannot read (the contract checks send `{` and expect an error). |
| `PRAENORA_STATUS_UNSUPPORTED` (5) | A capability, parameter or host feature the module does not support. |
| `PRAENORA_STATUS_RESOURCE_EXHAUSTED` (6) | Out of memory, or a sink that refused a write. |
| `PRAENORA_STATUS_CANCELLED` (7) | A call that stopped because `cancel_requested()` said so. |
| `PRAENORA_STATUS_FAILED` (11) | A computation that failed. |
| `PRAENORA_STATUS_DATA_INVALID` (12) | Input data with the wrong form or values. |

**Host API.** `log(level, message)` writes to the step's logger (`module.<step>`), which adds the
step, module, tick and run fingerprint; `log_enabled(level)` saves formatting messages nobody reads.
`get_service(name, major)` returns a runtime service's function table, or NULL: only the services
of service providers that the manifest declares ([Requiring services](#requiring-services)), and
the runtime's built-in ones, of which this version has no production one (and which a module host
does not offer). Read `host->struct_size` before using a callback added in a later ABI minor
version.

## The C++ adapter

`praenora/sdk/module.hpp` implements the table for a C++ class. Derive from
`praenora::sdk::Module`, give the class a constructor taking `const praenora::sdk::CreateContext&`,
and expand `PRAENORA_SDK_DEFINE_MODULE(ClassName)` once in the library, at namespace scope.

| Name | What it is |
|---|---|
| `Module` | The base class: `invoke(std::string_view invocation_json)` returns the result document (required); `initialize(const InitializeContext&)`, `finalize()` and `diagnostics()` have defaults. The constructor is `create`, the destructor `destroy`. |
| `CreateContext` | `host` (valid until the instance is destroyed) and `create_json` (valid during the constructor). |
| `InitializeContext` | `initialize_json`, which holds the run fingerprint, the step's seed and the time base. |
| `Host` | `log()`, `log_enabled()`, `cancel_requested()` and `service()`; each checks the host's `struct_size` first. `api()` gives the host API itself, for service functions that take it. |
| `LogLevel` | `trace`, `debug`, `info`, `warn`, `error`, `critical`. |
| `Error` | `Error(code, message, details_json = "{}")`: throw it to fail a call with a status of your choice. |
| `PRAENORA_SDK_DEFINE_MODULE(ClassName)` | The exported entry point and the static table. |
| `PRAENORA_SDK_DEFINE_MODULE_GETTER(ClassName, getter)` | A getter for the table without an exported entry point, for in-process tests. |

- Every exception is caught at the boundary: `sdk::Error` becomes its code (or `INTERNAL` for a code
  outside 1 to 99 and below 10000), `std::bad_alloc` becomes `RESOURCE_EXHAUSTED`, and anything else
  `INTERNAL` with `what()` as the message. The adapter writes the error document.
- `describe()` reports `PRAENORA_MODULE_NAME`, `PRAENORA_MODULE_VERSION` and
  `PRAENORA_MODULE_CAPABILITIES`, which `praenora_add_module()` defines from its arguments, and the
  SDK's ABI version (`abi.h`), `PRAENORA_SDK_VERSION_STRING` and `PRAENORA_PMP_VERSION_STRING`, the
  versions `praenora_add_module()` writes into the manifest, so the library and its manifest cannot
  disagree.
- The adapter parses no JSON: parse the create and initialize documents yourself for the parameters
  and the seed ([specs/protocol.md](specs/protocol.md#create-document)).
- Outputs are written to the sinks in pieces of 4096 bytes; a refused write ends the call with the
  sink's status.

A module may also implement the table in C. The fixture module `fx-minimal-c` is a complete C99
module: its table and entry point are in
[`fx-minimal-c/fx_minimal_c.c`](../tests/fixtures/modules/fx-minimal-c/fx_minimal_c.c) and its
functions in [`common/c_fixture.c`](../tests/fixtures/modules/common/c_fixture.c), both under
`tests/fixtures/modules/`. `PRAENORA_MODULE_IMPLEMENTATION`, which `praenora_add_module()` defines,
makes `abi.h` declare the entry point. Its `describe()` reports `PRAENORA_MODULE_NAME`,
`PRAENORA_MODULE_VERSION` and `PRAENORA_MODULE_CAPABILITIES`, ABI 0.0, and
`PRAENORA_SDK_VERSION_STRING` and `PRAENORA_PMP_VERSION_STRING` (`praenora/sdk/version.h`) as its
`sdk_version` and `pmp_version`, as
[`common/c_fixture.h`](../tests/fixtures/modules/common/c_fixture.h) does. `praenora_add_module()`
writes the ABI, SDK and PMP versions of the SDK it builds with into the manifest, and `describe()`
must report the same ones: when a later SDK raises the ABI minor (`PRAENORA_ABI_MINOR`), a C module
that writes its ABI version by hand must be updated, or it is refused when it is loaded. Update the
`abi_minor` of its table's header with it (the C fixtures write `0u`; `PRAENORA_ABI_MINOR` keeps it
current): the host does not compare it, so nothing refuses a stale one.

## The manifest

`praenora_add_module()` generates the manifest beside the library, from a template or, without
one, with the required fields and a parameter schema that accepts any object. A template is a JSON
file in which `@...@` variables stand for what the build knows, such as `@PRAENORA_MODULE_NAME@`;
the variables are listed in [specs/manifest.md](specs/manifest.md#generating-manifests).

Besides the variables, a template sets what only the module knows:

- `module.summary`, `authors`, `repository` and extension fields `x-...`.
- `parameters_schema`: a JSON Schema for the step's `parameters`, draft-07 (the default) or 2020-12
  (declare `"$schema": "https://json-schema.org/draft/2020-12/schema"`). Draft-07 ignores keywords
  beside a `$ref`, a `default` among them; `praenora check module` notes it. The runtime validates the
  configuration's parameters against it (`PARAMETERS_INVALID`) and fills in its `default`s before
  `create`. Give every parameter a default where one makes sense: the contract checks use the
  defaults when no test vectors are given.
- `features`: `concurrency` (`"module"` or `"instance"`), `diagnostics` (whether a failure's details
  include the instance's `diagnostics()`), `self_test` (whether the contract checks require a
  `self_test` function) and `cancellation` (informational).
- `tests.vectors`: the test vectors' file name, relative to the manifest.
- `library.sha256`: an optional pin of the library's digest, checked before loading; and
  `library.dependency_paths`: directories, relative to the manifest, where the module's own DLLs
  live (honored on Windows only; on Linux and macOS use an RPATH of `$ORIGIN` or `@loader_path`).
- `"requires": @PRAENORA_MODULE_REQUIRES_JSON@`, when the module declares services
  ([Requiring services](#requiring-services)); without a template the manifest gets it by itself.

The template must not contain `$<` otherwise: the result is evaluated for generator expressions.
Every field and rule is in [specs/manifest.md](specs/manifest.md), and
`praenora validate --document <manifest>` checks a manifest against its schema.

## `praenora_add_module()`

```cmake
praenora_add_module(<target>
    NAME <name> VERSION <semver> CAPABILITIES <id@version>... SOURCES <source>...
    [REQUIRES_SERVICES <name>@<major>.<minor>...] [OPTIONAL_SERVICES <name>@<major>.<minor>...]
    [MANIFEST_TEMPLATE <file>] [TEST_VECTORS <file>] [OUTPUT_DIRECTORY <dir>]
    [INSTALL_DESTINATION <dir> [INSTALL_COMPONENT <component>] [INSTALL_EXCLUDE_FROM_ALL]]
    [LINK_LIBRARIES <library>...])
```

| Argument | Meaning |
|---|---|
| `NAME` | The module name: lower-case words of letters, digits and `-`, separated by dots. It is also the library's file name, without a `lib` prefix. |
| `VERSION` | The module's semantic version. |
| `CAPABILITIES` | Each implemented capability as `<id>@<version>`, as the catalog names it. |
| `SOURCES` | The sources. |
| `REQUIRES_SERVICES`, `OPTIONAL_SERVICES` | The runtime services the module requires, and those it can use when they are there, each as `<name>@<major>.<minor>` with the lowest minor it accepts ([Requiring services](#requiring-services)). |
| `MANIFEST_TEMPLATE` | The manifest template, relative to the current source directory. |
| `TEST_VECTORS` | A test vectors file, copied beside the manifest at build time. |
| `OUTPUT_DIRECTORY` | Where the library and manifest go (default `<current binary dir>/<name>`); multi-configuration generators get a directory per configuration. |
| `INSTALL_DESTINATION` | Install the library, the manifest and the vectors together into this directory; `<libdir>/praenora/modules/<name>` inside an installation's prefix is where the runtime looks by itself. |
| `INSTALL_COMPONENT`, `INSTALL_EXCLUDE_FROM_ALL` | Put the installed files in a component, and leave them out of a full installation. |
| `LINK_LIBRARIES` | More libraries to link privately, such as a JSON library. |

The manifest it writes has format 0.2.0, which a runtime 0.2 or later reads. The target is always an
`add_library(MODULE)` target, whatever `BUILD_SHARED_LIBS` says, compiled with hidden symbol
visibility so that only the entry point is exported, and linked to `Praenora::ModuleSDK`. The target
properties `PRAENORA_MODULE_DIR`, `PRAENORA_MODULE_MANIFEST` and `PRAENORA_MODULE_TEST_VECTORS_PATH`
give the output directory, the manifest and the copied vectors for rules of your own. With a
multi-config generator, such as Visual Studio's, they hold `$<CONFIG>`, which
`$<TARGET_PROPERTY>` leaves as it is in a property of its own: read them with
`$<GENEX_EVAL:$<TARGET_PROPERTY:<target>,PRAENORA_MODULE_MANIFEST>>`, in `add_test()` or a
target's compile definitions, for example. The helper's reference is the comment at the top of
[`cmake/PraenoraModule.cmake`](../cmake/PraenoraModule.cmake).

## Requiring services

Some services come with a heavy engine, a simulation engine for example, that a module should not
link itself. The runtime offers them through service providers, separately built libraries shipped
with it, which it loads only when a run needs them
([specs/runtime-services.md](specs/runtime-services.md)). A module that uses such a service declares
it, and gets it only then:

```cmake
praenora_add_module(simulating_module
    NAME simulating-module VERSION 0.1.0 CAPABILITIES example.estimate@0.1.0
    SOURCES src/module.cpp
    REQUIRES_SERVICES praenora.simulation@0.0)
```

- `REQUIRES_SERVICES` lists the services the module cannot work without, `OPTIONAL_SERVICES` those
  it uses when the run has them. Each is `<name>@<major>.<minor>`: its name, the major version whose
  table the module uses, and the lowest minor it needs (a later minor only appends to the table). A
  name and major appears once in both lists together.
- `praenora_add_module()` writes them into the manifest's `requires.services`; a template takes them
  with `"requires": @PRAENORA_MODULE_REQUIRES_JSON@`.
- Validation resolves each one to a usable provider on the provider search paths (`--provider-path`
  or `RuntimeOptions::provider_paths`, then `PRAENORA_PROVIDER_PATH`, then the installation's
  `lib/praenora/providers`; `praenora providers list` shows what is found there). A required service
  that does not resolve fails validation with `SERVICE_UNAVAILABLE`, located at
  `/requires/services/<i>` of the manifest, whose details list the searched paths and every provider
  that came close and why it does not serve. An optional one that does not resolve is simply not
  there.
- A run loads and starts the providers before the module that uses them is loaded. The module then
  asks for each service with `get_service(name, major)` (`host.service(name, major)` in the C++
  adapter), usually once, in `create`, and keeps the table: tables live until the process ends. It
  gets the table of every declared service that resolved, NULL for an optional one that did not,
  and NULL for any provider service it did not declare. Check for NULL, and that the table's
  `header.struct_size` covers the fields you use. A traced run (`--trace`) lists the lookups each
  call made, and whether they got a table, in its message trace
  ([specs/observability.md](specs/observability.md#message-trace)).
- The run record names each service with its provider and engine, so the run fingerprint changes
  when the engine does ([specs/reproducibility.md](specs/reproducibility.md)).
- A step that runs in a module host (isolation mode `process`,
  [specs/process-isolation.md](specs/process-isolation.md#service-providers)) has the providers of
  its module's services loaded and started in that process, before its library. Nothing changes
  for the module: it asks for the same services and gets the same tables, its lookups are traced
  the same way, and the services give the same results. The one difference a module can meet is
  that the runtime's built-in services (test services such as `test.gate`, which tests register)
  are not offered there; a module should use only the services its manifest declares.
- `praenora modules check` resolves the services and loads their providers as part of its
  `library.load` check, and serves them to the module as a run does; test vectors can then exercise
  the module with the real service.
- A service's own specification says what its table does, which objects it hands out and who frees
  them, and which threads may use them.

## Using the simulation service

The simulation service `praenora.simulation@0` simulates a model with an engine that a service
provider brings, for example an SBML model with libRoadRunner; its normative specification is
[specs/simulation-service.md](specs/simulation-service.md), and its table is declared in
`praenora/sdk/simulation.h`. Require it with `REQUIRES_SERVICES praenora.simulation@0.0` (above),
then use it in three steps: a **session** opens a model; a module that runs the model many times
**prepares** a **plan** and runs it with numbers; a module with an integrator of its own opens a
**kernel**, the compiled model's C functions.

```cpp
#include <praenora/sdk/module.hpp>
#include <praenora/sdk/simulation.h>

namespace sdk = praenora::sdk;

// A sink that keeps what the service writes.
struct Text {
    std::string text;
    praenora_sink sink{this, [](void* self, const char* data, std::size_t size) -> praenora_status {
        if (data == nullptr && size != 0) {
            return PRAENORA_STATUS_INVALID_ARGUMENT;
        }
        try { // no exception may reach the provider
            static_cast<Text*>(self)->text.append(data == nullptr ? "" : data, size);
            return PRAENORA_STATUS_OK;
        } catch (...) {
            return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        }
    }};
};

// In the constructor (create): the table, once.
table_ = static_cast<const praenora_simulation_v0*>(
    host_.service(PRAENORA_SIMULATION_SERVICE, PRAENORA_SIMULATION_SERVICE_MAJOR));
if (table_ == nullptr || table_->header.abi_major != PRAENORA_SIMULATION_SERVICE_MAJOR ||
    table_->header.reserved != 0 || table_->header.struct_size < PRAENORA_SIMULATION_V0_BASE_SIZE) {
    throw sdk::Error(PRAENORA_STATUS_UNSUPPORTED, "no usable praenora.simulation@0");
}

// In initialize: open the model and prepare a plan; the plan outlives the session.
Text info, error;
praenora_simulation_session* session = nullptr;
praenora_status status = table_->open_session(table_->context, host_.api(), model, options,
                                              &session, &error.sink);
if (status != PRAENORA_STATUS_OK) throw sdk::Error(status, "open_session failed", error.text);
status = table_->prepare(session, host_.api(), plan_json, &plan_, &info.sink, &error.sink);
table_->close_session(session);
if (status != PRAENORA_STATUS_OK) throw sdk::Error(status, "prepare failed", error.text);

// In invoke: one run with this tick's numbers (inputs: the plan's parameters, then its initial
// conditions); out has rows * columns values, as info says.
std::int32_t outcome = PRAENORA_SIMULATION_RUN_OK;
Text failure;
status = table_->run(plan_, host_.api(), values.data(), values.size(), out.data(), out.size(),
                     &outcome, &failure.sink, &error.sink);

// In the destructor (destroy): close what is still open.
table_->close_plan(plan_);
```

The fixture module `simulating-module`
([`simulating_module.cpp`](../tests/fixtures/modules/simulating-module/simulating_module.cpp)) is a
complete module that does this, kernel included, and ships test vectors that run it.

- **Sessions.** `open_session(context, host, model, options, &session, error)` takes the model's
  text (never a path) and `{"format": "<model format>"}`; `describe()` lists the formats and solver
  methods the provider offers and its engine. Opening may compile the model, which takes time:
  open sessions and prepare plans in `create` or `initialize`, not in `invoke`.
  `describe_model` lists the model's `states`, `parameters` and `quantities`.
- **SBML models.** The provider `roadrunner` (built with `PRAENORA_WITH_ROADRUNNER`) offers the
  format `sbml`: the text of an SBML document of Level 1, 2 or 3. It refuses document type
  declarations, Level 3 documents that require a package or declare `comp`, and models
  libRoadRunner cannot simulate; which SBML elements become states, parameters and quantities, and
  how its one method, `cvode`, maps the solver settings, is in
  [specs/simulation-service.md](specs/simulation-service.md#the-roadrunner-provider).
- **One time course.** `simulate(session, host, request, result, error)` takes a JSON request with
  the field names of `ode-uq-spec`'s simulation configuration (`time_domain`,
  `initial_conditions`, `parameter_values`, `solver`, `output`) and writes
  `{"ok": true, "columns", "rows", "solver"}`. Every run starts from the model's original state.
  Fields the service does not know are `INVALID_ARGUMENT` and features it does not map
  `UNSUPPORTED`, with a JSON pointer in the error document's `details`; nothing is ignored.
- **Many runs.** `prepare` fixes the time domain, solver, output and which parameters and initial
  conditions are inputs, and writes the plan's `inputs`, `columns` and `rows`. `run` takes one set
  of input values and fills `out` (`rows * columns` doubles, row-major); `run_batch` takes
  `set_count` sets and fills `set_count` outputs and outcomes, in order. The counts must match
  exactly. The numbers are those `simulate` would give for the same settings.
- **Numerical failures are data.** A run that diverges, hits `max_steps` or produces a value that
  is not finite does not fail the call: the status is OK, the outcome is
  `PRAENORA_SIMULATION_RUN_FAILED`, the run's output is NaN, and `run` writes the failure document
  (`details.reason` `integrator`, `step_limit` or `nonfinite`) to `failure_json` (which may be
  NULL). A module fitting many parameter sets can treat such sets as data.
- **Parameters and events.** Parameter values you set (`parameter_values`, a plan's parameter
  inputs, `set_parameters`) take effect as the model's initial values: initial values defined from
  parameters follow them, and initial conditions you set then replace state values. Level A applies
  the model's events; `describe_model` says whether a model has any.
- **Kernels.** For an integrator of your own, `open_kernel(session, host, {"parameters": [...],
  "quantities": [...]}, &kernel, error)`: set `kernel.struct_size = sizeof kernel` first. Then
  `kernel.rhs(kernel.model, t, y, dydt)`, `set_parameters`, `initial_state` and `quantities` work
  on the compiled model, in the orders you chose, without logging or polling; their results depend
  only on their arguments and the parameters you set. A kernel is the continuous right-hand side
  only, so a model with events has none (`UNSUPPORTED`, `details.reason` `"events"`). Close it with
  `close_kernel(&kernel)`.
- **Lifetime.** You close every session, plan and kernel exactly once, before your `destroy`
  returns. Plans and kernels keep the compiled model alive on their own, so a session can be closed
  as soon as its plans and kernels exist.
- **Threads.** A session or plan serves one call at a time; a call on one that is busy returns
  `INVALID_STATE`. Different sessions, plans and kernels can be used at the same time from threads
  of your own (joined before `destroy` returns); call a kernel's functions from one thread at a
  time.
- **Host, cancellation and logging.** Pass your host API (`host.api()` in the C++ adapter) to every
  call that takes one: the service logs through it, with your step's attributes, and returns
  `CANCELLED` when your call's deadline passes or a stop is requested; return then, too.
- **Reproducibility.** The run record names the provider and its engine, so the fingerprint changes
  with the engine. Record the `solver` settings the service reports with your results, and derive a
  stochastic method's `seed` from your step's seed.
- **Testing.** In this repository, the fixture provider `test-simulation` offers the service for
  the toy model format `test-decay`, without an engine: `praenora modules check
  <manifest> --provider-path <build>/test-providers/test-simulation` (with `--catalog-path` for
  capabilities outside the built-in catalog) runs a module's vectors against it.

## Using the model conversion service

The model conversion service `praenora.model-conversion@0` translates a model source from one
format to another with an engine that a service provider brings, for example an Antimony model to
SBML; its normative specification is
[specs/model-conversion-service.md](specs/model-conversion-service.md), and its table is declared in
`praenora/sdk/model_conversion.h`. Require it in the manifest, and look it up once, as any service
([Requiring services](#requiring-services)):

```cmake
praenora_add_module(converting_module
    NAME converting-module VERSION 0.1.0 CAPABILITIES example.estimate@0.1.0
    SOURCES src/module.c
    REQUIRES_SERVICES praenora.model-conversion@0.0
    OPTIONAL_SERVICES praenora.simulation@0.0)
```

A module that converts a model and then simulates it, as here, requires the conversion service and
can use the simulation service, or requires both. The table has two functions, `describe` and
`convert`, and hands out no objects. In C:

```c
#include <praenora/sdk/model_conversion.h>

#include <stdlib.h>
#include <string.h>

/* A sink that keeps what the service writes, in memory of its own. */
typedef struct text {
    char* data;
    size_t size;
} text;

static praenora_status PRAENORA_CALL text_write(void* context, const char* data, size_t size) {
    text* out = (text*)context;
    char* grown = realloc(out->data, out->size + size + 1);
    if (grown == NULL) {
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    }
    if (size != 0) {
        memcpy(grown + out->size, data, size);
    }
    out->data = grown;
    out->size += size;
    out->data[out->size] = '\0';
    return PRAENORA_STATUS_OK;
}

/* In create: the table, once. */
const praenora_model_conversion_v0* conversion =
    (const praenora_model_conversion_v0*)host->get_service(
        host->context, PRAENORA_MODEL_CONVERSION_SERVICE, 25,
        PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR);
if (conversion == NULL || conversion->header.abi_major != PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR ||
    conversion->header.reserved != 0 ||
    conversion->header.struct_size < PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE) {
    return PRAENORA_STATUS_UNSUPPORTED; /* no usable praenora.model-conversion@0 */
}

/* In initialize: translate the Antimony source `model` (model_size bytes) to SBML. */
static const char options[] = "{\"from\":\"antimony\",\"to\":\"sbml\"}";
const praenora_bytes options_json = {options, sizeof options - 1};
const praenora_bytes source = {model, model_size};
text sbml = {NULL, 0}, report = {NULL, 0}, error = {NULL, 0};
praenora_sink sbml_sink = {&sbml, text_write};
praenora_sink report_sink = {&report, text_write};
praenora_sink error_sink = {&error, text_write};
const praenora_status status = conversion->convert(conversion->context, host, options_json, source,
                                                   &sbml_sink, &report_sink, &error_sink);
/* OK: sbml holds the SBML text, report {"from", "to", "module", "warnings"}. Otherwise error holds
   the error document; free all three buffers when done. */
```

- **Options.** `{"from": <format>, "to": <format>}`, and optionally `"module": <name>` to translate
  a module other than the source's main one. `describe` lists the pairs of formats the provider
  converts, and its engine.
- **Sources.** The source is the model's text, never a path: UTF-8, without NUL bytes, at most
  16 MiB. A source that is empty or only white space is refused in every format (`DATA_INVALID`,
  `details.rule` `"empty"`), and so is a source that one of the provider's rules refuses, for
  example an Antimony `import`, which would read another file: `details.rule` names the rule. A
  source the engine cannot read is `DATA_INVALID` with `details.engine_error`, and a module the
  source does not have is `NOT_FOUND`.
- **Threads and cancellation.** `convert` may be called from any thread; a provider whose engine
  has process-wide state serializes conversions, so a call may wait for others. Pass your host API:
  the wait returns `CANCELLED` when your call's deadline passes or a stop is requested. Convert in
  `create` or `initialize`, not in `invoke`.
- **Antimony and SBML.** The provider `antimony` (built with `PRAENORA_WITH_ANTIMONY`) converts
  the format `antimony`, Antimony text, to `sbml` and back with libAntimony: SBML Level 3 Version 2,
  flattened, of the source's main module or of the module the options name, and Antimony with its
  function definitions kept. It refuses an Antimony source that imports another file or leaves a
  comment, a string or a block open, Antimony text that is an XML document, and SBML that holds a
  document type declaration, declares `comp` or, at Level 3, requires a package; the details are in
  [specs/model-conversion-service.md](specs/model-conversion-service.md#the-antimony-provider).
  An Antimony source that asks for a layout (`model.layout = on`, positions, colors) is refused
  with `UNSUPPORTED` and `details.reason` `"layout"`, because the layout libAntimony makes depends
  on the clock. Such a request, and SBML that holds a layout, reseed the C library's `rand()` for
  the whole process: a module that needs random numbers uses a generator of its own, seeded from
  its step's seed.
- **Simulating what you converted.** Open a session of the simulation service with the converted
  text and `{"format": <to>}` ([Using the simulation service](#using-the-simulation-service)).
- **Reproducibility.** The run record names the provider and its engine, so the fingerprint changes
  with the engine; record the report, or a digest of the converted text, with your results.
- **Testing.** In this repository, the fixture provider `test-conversion` offers the service for
  two toy formats, `test-decay` and `test-decay-text`, without an engine, and the fixture module
  `converting-module`
  ([`converting_module.cpp`](../tests/fixtures/modules/converting-module/converting_module.cpp))
  converts with it and simulates the result with `test-simulation`.

## The test kit

`praenora/sdk/testing.hpp` drives a module in process, with no library loaded, from any test
framework:

- `FakeHost` implements the host API for tests: it records every log message (`logs()`), raises and
  clears the cancel flag on demand (`request_cancel()`, `reset_cancel()`), serves service tables
  (`register_service()`), can report a smaller `struct_size` to act as an older host, and can refuse
  sink writes (`refuse_sink_writes(true)`).
- `DirectModule` takes the module's getter (or any `praenora_module_get_api_fn`, also one found
  with `dlsym` in a built library), checks the table as the runtime does, and calls `describe()`,
  `create(host, json)`, `initialize(json)`, `invoke(json)`, `finalize()`, `diagnostics()` and
  `destroy()`. Each returns a `CallResult`: the `status` and what the module wrote to its output
  sink (`output`) and its error sink (`error`).
- `PRAENORA_SDK_DEFINE_MODULE_GETTER(ClassName, getter)` defines the getter in the test; the test
  defines `PRAENORA_MODULE_NAME`, `PRAENORA_MODULE_VERSION` and `PRAENORA_MODULE_CAPABILITIES`
  itself, as `last_value_test.cpp` above does.

## Test vectors

A test vectors document (`kind: module-test-vectors`, schema
[`module-test-vectors.0.1.schema.json`](../spec/schemas/module-test-vectors.0.1.schema.json)) holds
cases. Each case names a `capability` the module implements and gives `parameters` (validated and
completed with the defaults, as in a run), a `seed`, a `time` base and a list of `ticks`. Each tick
gives the `data` of each input port, from which the checks build the envelopes, and optionally an
`expect`ed `outputs` object with a `tolerance` (`relative`, `absolute`, both 0 by default). Numbers
match when `|a - b| <= max(absolute, relative * max(|a|, |b|))`; everything else must be equal, and
the expected and actual outputs must have the same ports.

Good vectors come from real runs: the example modules' vectors are the envelopes they received and
produced in the first two ticks of the example workflow, taken from its message trace
([`examples/modules/estimator/vectors.json`](../examples/modules/estimator/vectors.json)). Ship the
vectors with the module (`TEST_VECTORS`, `tests.vectors`), so anyone can check it.

## Contract checks

`praenora modules check <manifest> [--vectors F]` loads the module and runs checks with stable ids,
each `pass`, `fail` or `skip` with a message. It exits with 0 when no check failed and with 1
otherwise. With `--json` it prints `{"ok": true, "result": <the module-check-report document>}`, or,
when a check failed, `{"ok": false, "error": ..., "diagnostics": []}`, an error `FAILED` that holds
the report in its `details`. `--catalog-path` adds the catalog that defines the module's
capabilities when they are not in the built-in catalog.

| Check | Passes when |
|---|---|
| `manifest.schema` | The manifest satisfies the `module-manifest` schema. |
| `manifest.semantics` | Its paths are relative, its capabilities unique, its parameter schema compiles, it fits this runtime (platform, ABI major, PMP version) and the catalog defines its capabilities. |
| `library.digest` | The library exists and matches `library.sha256` when the manifest pins one. |
| `library.load` | The library loads, and no abandoned (unresponsive) call that its calls would have to wait for may still be running in it. |
| `library.entry_point` | It exports `praenora_module_get_api`. |
| `api.header` | The table's header is valid for ABI 0. Its ABI minor, which the message shows, is not checked. |
| `api.required_functions` | Every required function is set. |
| `describe.schema` | `describe()` writes a valid `pmp-describe` document. |
| `describe.identity` | It agrees with the manifest: name, version, ABI major and minor, SDK version, PMP version, capabilities. |
| `lifecycle.create_destroy` | An instance can be created and destroyed. |
| `lifecycle.full` | An instance can be created, initialized, finalized and destroyed. |
| `invoke.result_schema` | Every result of the vectors is valid: declared ports only, every required one, each datum valid for its message type. |
| `invoke.expected_outputs` | The outputs match the vectors' expectations within their tolerance. |
| `invoke.determinism` | A second run of the vectors on a new instance gives byte-identical results. |
| `invoke.malformed_input` | `invoke("{")` returns an error status, in time. |
| `self_test` | The module's `self_test` reports `passed` (skipped when it has none and the manifest does not declare it). |
| `unload` | Every instance was destroyed and the library released. |

A check runs only when the one it depends on passed; otherwise it is skipped with the reason.
Without test vectors the lifecycle checks use the first capability and the parameter schema's
defaults, and the checks that need inputs are skipped, so a module should ship vectors. The checks
and their order are specified in [ADR-0024](adr/ADR-0024-contract-checks-and-test-vectors.md).

**The acceptance gate.** Before this project adopts a module version, the module's own test suite
passes, `praenora modules check` passes on its manifest, and its test vectors are part of the module
([testing.md](testing.md#the-module-acceptance-gate)).

## In a module host

A step can run its module instance in a module host, a process of its own (isolation mode
`process`, [specs/process-isolation.md](specs/process-isolation.md)). Nothing changes for the
module: the ABI, the documents, the host API and the threading rules are those above, and the
results are the same. What a module can meet there:

- Its process is the module host's: the process id differs, and standard output and error are the
  runtime's, shared with it. A crash ends the module host and fails the step (`MODULE_CRASHED`),
  not the runtime; a call that does not return after its deadline and grace is ended with its
  module host.
- The stdio buffers are the module host's own: what you write through `stdout` or `std::cout`
  reaches the runtime's standard output only when the module host flushes it, at the latest as it
  exits at the end of the run, and is lost when the module host is killed or crashes (a call that
  does not return, a crash, a stop that ends it). Its order among the runtime's own output can
  differ from a run in the runtime's process. Flush what must arrive (`fflush(stdout)`,
  `std::flush`), or write diagnostics to standard error, which is unbuffered; better still, log
  through the host API.
- The library loads on a thread with a main thread's stack (8 MiB unless `RLIMIT_STACK` says
  otherwise), as it does in the runtime's process on its caller's thread; the calls run on a thread
  with the stack of the instance's worker thread, as they do there.
- The runtime's built-in services are not offered; the services the manifest declares are
  ([Requiring services](#requiring-services)).
- **Under the restricted sandbox profile** (`sandbox: restricted`, macOS,
  [specs/process-isolation.md](specs/process-isolation.md#the-restricted-profile)) the module, its
  static initializers included, may not use the network, may not start processes (`fork`, `exec`,
  `posix_spawn`, `system()`, nor through launchd, LaunchServices or Apple events), and may not
  write files anywhere but below its temporary directory (`TMPDIR`,
  `std::filesystem::temp_directory_path()`), which is its module host's own, and to `/dev/null`.
  Links do not get around it: a hard link to a file outside cannot be made there, and a symbolic
  link to one cannot be written through. Write to standard output and error through descriptors
  1 and 2 (`stdout`, `stderr`, `std::cerr`): opening `/dev/stdout`, `/dev/stderr`, `/dev/tty` or
  `/dev/fd/<n>` by its path for writing is refused. What it may not do fails with `EPERM`
  ("Operation not permitted"): return an error then (`PRAENORA_STATUS_UNSUPPORTED` or
  `PRAENORA_STATUS_FAILED`, with a message that says what was refused). Reading files, loading
  libraries, threads, and generating code at run time work. A module that keeps the rules below
  (no file I/O in algorithm code, everything through its documents) needs nothing the profile
  refuses.
- The temporary directory is removed when the module host ends, with everything in it; leave
  nothing there that cannot be removed (the runtime warns about what is left). Do not rely on it
  between runs.
- The profile keeps a module from doing these things by accident or by a library's habit; it is
  not a boundary against a module meant to do harm (system services it can reach over Mach IPC
  may act for it).

## Determinism and portability

The runtime makes runs reproducible: same fingerprint, same platform and same binaries give
byte-identical outputs. That holds only if modules keep these rules, which the `invoke.determinism`
check tests in part:

- **No global mutable state.** Keep all state in the instance: two instances of a module, in one
  run or in two, must not influence each other. Statics are for constant data only.
- **No file I/O in algorithm code.** Everything a module computes from comes through its documents:
  parameters, inputs and the initialize document. Data a step needs is a workflow input, which the
  runtime reads, checks and covers with the fingerprint. Report through outputs, diagnostics and the
  log callback, not files.
- **No wall-clock time.** Use the tick's `logical_time` and the time base; never the system clock,
  timers or the process id.
- **Randomness only from the seed.** Seed every generator from the step's seed in the initialize
  document, which the runtime derives from the run seed and the step id.
- **Deterministic order.** Never let the iteration order of a hash table, or of anything keyed by
  pointers, reach an output. Sort, or use ordered C++ containers such as `std::map`.
- **Threads are yours to join.** Threads a module starts must not race on shared data, must not
  call into the instance after `destroy`, and must be joined before `destroy` returns.
- **No uninitialized memory**, and no undefined behavior: the sanitizer builds of the runtime run
  module code too.
- **Floating point.** Results agree across platforms only within a tolerance. Compile with
  `-ffp-contract=off` (GCC and Clang) to stop fused multiply-adds from changing results, as the
  example modules do, and parse and format numbers without the process's locale.
- **Text and paths.** Every document is UTF-8 JSON. A module receives no paths; if it must name a
  file in its outputs, use UTF-8.

## Versioning

| Version | Rule |
|---|---|
| The module's (`VERSION`) | Semantic versioning: before 1.0, a change of the minor may break users and a change of the patch must not. Configurations pin an exact version, so a new version is adopted by editing configurations. |
| ABI (`abi`) | The module's ABI major must equal the runtime's; any minor is accepted. A module built with this SDK declares ABI 0.0. The manifest and `describe()` must report the same major and minor. |
| PMP (`pmp_version`) | Must be in the runtime's compatibility class (0.1.x) and not newer than the runtime's. The manifest and `describe()` must report the same version (build metadata aside). |
| SDK (`sdk_version`) | Decides no compatibility. The manifest and `describe()` must report the same version (build metadata aside), or the module is refused when it is loaded; the run record holds the manifest's. |
| Capabilities | Exact: a module implements `example.estimate@0.1.0` or it does not. A new capability version means a new module version that declares it. |

`praenora_add_module()` writes the ABI, PMP and SDK versions of the SDK it builds with, so
rebuilding against a newer SDK updates them. The policy is in
[specs/compatibility-policy.md](specs/compatibility-policy.md).
