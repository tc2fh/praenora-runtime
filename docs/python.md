# Python

The Python package `praenora.runtime` (distribution `praenora-runtime`) offers what the `praenora`
tool offers, with the same results: validating configurations and documents, listing, inspecting
and checking modules, listing service providers, workflow graphs, the protocol catalog and the
schemas, and running workflows with their status, metrics and outputs. It is a pure-Python layer
over an extension that SWIG generates from a narrow facade of the C++ API, which passes only
strings and numbers ([ADR-0018](adr/ADR-0018-python-bindings-with-swig.md)). `praenora` is a
namespace package.

It needs Python 3.10 or newer. The examples below were run on macOS arm64 with CPython 3.12,
from the repository root, against the `dev` build.

## Building the bindings

The bindings are built with `PRAENORA_BUILD_PYTHON=ON`, which needs SWIG 4.0 or newer and Python
with its headers (CMake's `Development.Module`). In the local presets, set `PRAENORA_BUILD_PYTHON`
and `Python3_EXECUTABLE` in `dev-local` ([developer-guide.md](developer-guide.md#local-presets)).
The build tree then holds the importable package in `build/<preset>/python/praenora/runtime/`: the
package's `__init__.py`, SWIG's proxy module `_bindings.py` and the extension
`_bindings_native.<suffix>` (for example `_bindings_native.cpython-312-darwin.so`).

The extension is built for one interpreter, the one CMake found, so use that one, with the build
tree on `PYTHONPATH`. The build's cache names it:

```bash
PY=$(sed -n 's/^Python3_EXECUTABLE:[A-Z]*=//p' build/dev/CMakeCache.txt)
PYTHONPATH=build/dev/python "$PY" -c 'import praenora.runtime as pr; print(pr.version()["runtime"])'
```

## A first session

```python
import praenora.runtime as pr

print(pr.version()["runtime"])

config = "examples/workflows/estimate-predict/workflow.yaml"
modules = ["build/dev/modules"]
print(pr.validate(config, module_paths=modules))

for module in pr.list_modules(module_paths=modules):
    print(module["name"], module["version"], module["capabilities"])

print(pr.graph(config=config, format="text"))

with pr.Workflow(config, module_paths=modules, log_level="warn") as wf:
    summary = wf.run()
    print(summary["state"], summary["ticks_completed"], summary["fingerprint"])
    print(wf.status()["workflow"]["health"])
```

`pr.validate()` returns `{'ok': True, 'diagnostics': []}`, and the run ends `completed` after 12
ticks with health `healthy`.

## Functions

Paths are `str` or `os.PathLike`; `module_paths`, `provider_paths` and `catalog_paths` are lists of
paths, which play the part of the tool's `--module-path`, `--provider-path` and `--catalog-path`
options: the caller's module search paths ([search order](specs/manifest.md#discovery)), the
caller's service provider search paths ([search order](specs/runtime-services.md#discovery)),
searched before `PRAENORA_PROVIDER_PATH` and the installation's provider directory, and extra
catalog directories ([specs/protocol.md](specs/protocol.md#catalog)). `provider_paths` is a
keyword-only argument wherever it is not the first. A single path where a list is expected is a
`TypeError`. Results are decoded JSON: what the tool's `--json` output holds as its `result`,
without the `{"ok": true, "result": ...}` around it.

| Function | Returns |
|---|---|
| `version()` | `{"runtime", "sdk", "pmp", "abi": {"major", "minor"}, "formats", "platform", "compiler", "build_type"}`. |
| `validate(config, module_paths=None, catalog_paths=None, load=False, *, provider_paths=None)` | The validation report, `{"ok": True, "diagnostics": [...]}`, whose diagnostics are warnings and notes; raises `PraenoraError` when it holds an error. `load=True` also starts the service providers of the workflow's services, and loads each module and checks its identity. |
| `validate_document(path)` | The report for any Praenora document, checked against the schema of its kind; raises like `validate()`. |
| `list_modules(module_paths=None)` | Every manifest on the search paths, in search order: `[{"name", "version", "manifest_path", "library_path", "os", "arch", "capabilities"}]`. |
| `list_providers(provider_paths=None)` | Every service provider manifest on the provider search paths, in search order: `[{"name", "version", "manifest", "library", "platform": {"os", "arch"}, "services": [{"name", "major", "minor"}], "engine", "usable"}]`, with `"problem"`, why the runtime cannot use it, for a provider that is not usable; a manifest shadowed by an earlier one of the same name and platform is unusable, its problem naming the manifest that shadows it. Nothing is loaded. |
| `inspect_module(name, version=None, load=False, module_paths=None)` | One module as discovery resolves it (its highest version for this platform by default), with `"describe"` and `"library_sha256"` when `load=True`. Inspecting a module loads no service provider, so it takes no `provider_paths`. |
| `check_module(manifest, vectors=None, *, catalog_paths=None, provider_paths=None)` | The contract checks' report, `{"kind", "format_version", "passed", "checks"}`; raises `PraenoraError` `FAILED` when a check failed. `catalog_paths` adds the catalog entries the module uses that are not built in, as `--catalog-path` does for `praenora modules check`; the services the module's manifest declares are resolved on the provider search paths, `provider_paths` first, and their providers started for the checks. |
| `graph(config=None, workflow=None, format="json", *, module_paths=None, catalog_paths=None)` | The graph of the definition `workflow` (`"<name>@<version>"`) or of the one `config` pins: a dict for `"json"`, a string for `"text"`, `"dot"` and `"asyncapi"`. |
| `list_catalog(kind=None, *, catalog_paths=None)` | The catalog's entries, `[{"kind", "id", "version", "status", "summary"}]`, sorted; `kind` is `"workflows"`, `"capabilities"` or `"messages"`. |
| `catalog_entry(id, version, *, catalog_paths=None)` | One entry's document. |
| `list_schemas()` | The embedded schemas, `["<kind>@<major>.<minor>", ...]`. |
| `schema(kind, format_version=None)` | One schema, of the highest version by default. |
| `read_status(path)` | The status-snapshot document of a status file, whose error records name the run's directories by placeholders ([observability.md](specs/observability.md#known-directories-in-the-files-of-a-run)). |

## Workflow

```python
Workflow(config, module_paths=None, output_dir=None, seed=None, log_level=None, *,
         ticks=None, overwrite=False, trace_messages=None, status_file=None,
         log_format=None, log_file=None, catalog_paths=None, provider_paths=None,
         isolation=None)
```

The constructor validates the configuration, with its arguments as overrides
([configuration.md](configuration.md#overrides-and-precedence)), and prepares the run; nothing is
loaded yet. `seed` and `ticks` are whole numbers from 0 to 2^53 - 1; `log_level` and `log_format`
take the names of the tool's options in any case; `isolation` is `"in-process"` or `"process"`,
exactly so, as `--isolation` takes them, and sets every step's isolation mode
([Process isolation](#process-isolation)); `None` leaves a setting to the configuration. A `with`
block closes the run when it ends.

| Method or property | Does |
|---|---|
| `start()` | Loads and starts the service providers of the run's services, then loads the modules and creates and initializes an instance for each step. |
| `tick()` | Runs the next tick: `{"tick", "logical_time", "outputs", "finished"}`, `outputs` naming the workflow outputs it produced. |
| `run(ticks=None)` | Starts the run if needed, then runs ticks until it is finished, or until `ticks` more ran: `{"state", "ticks_completed", "fingerprint"}`. It returns for a completed or stopped run, and raises the failure of a failed one, again on every later call. |
| `request_stop()` | Asks the run to stop: the call in progress is asked to cancel and no further call starts. Safe from any thread. |
| `close()` | Ends a ready or running run as `stopped`, finalizes, destroys and unloads its modules, and refuses later control calls. Idempotent. |
| `status()` | The status snapshot (`{"kind", "format_version", "generated_at", "runtime_version", "pid", "workflow", "steps"}`), its error records with absolute paths. |
| `metrics()` | `{"metrics": [...]}`: the metric points, such as `praenora.step.invocations`. |
| `latest_outputs()` | The newest envelope of each workflow output, `{"<output>": envelope}`. |
| `run_record()` | The run record as last written, the same document as its file, whose error records name the run's directories by placeholders such as `<output>`; `None` before the run starts. |
| `diagnostics()` | What validation found when the run was opened and did not refuse it: the report of `validate()`, `{"ok": True, "diagnostics": [...]}`, whose diagnostics are warnings and notes (for example data rows before the run's start); `praenora run` prints the same ones. |
| `state`, `fingerprint` | Properties: the state name and the run fingerprint. |

### Process isolation

`isolation="process"` (or the configuration's `isolation.mode`,
[configuration.md](configuration.md#isolation)) runs each step's module instance in a module host,
the executable `praenora-module-host`, a process of its own, instead of the Python process: a crash
of the module fails its step with `MODULE_CRASHED` (its state `crashed`) and the interpreter goes
on, and a call that never returns is ended by killing its module host. The results are those of a
run in the Python process.

```python
import praenora.runtime as pr

with pr.Workflow("examples/workflows/estimate-predict/workflow.yaml",
                 module_paths=["build/dev/modules"], isolation="process") as workflow:
    print(workflow.run()["state"])
    print(workflow.run_record()["execution"]["isolation"]["predict"])
```

```text
completed
{'mode': 'process', 'sandbox': 'none', 'share_compiled_models': False}
```

The runtime finds the module host relative to its runtime library, in `libexec/praenora/`: the
package in the build tree (`build/<preset>/python`) uses the build tree's runtime library, and so
its module host. **A wheel holds no module host**: an installation from the wheel refuses process
mode, as a step in a module host fails to load with `MODULE_HOST_UNAVAILABLE` (code 503, category
`load`) and `details.reason` `"missing"`. Run such steps in the Python process there
(`isolation="in-process"`, the default), or use the runtime of a build tree or of a full
installation.

## Errors

Every failure of the runtime raises `praenora.runtime.PraenoraError`, with the attributes `code`
(for example 301), `name` (`"MODULE_NOT_FOUND"`), `category` (`"validation"`), `message`, `details`
(a dict) and `location` (a dict with any of `file`, `line`, `column` and `pointer`); `str()` gives
`"NAME: message"`. The codes are those of the [error taxonomy](specs/error-taxonomy.md).

`validate()`, `validate_document()` and `check_module()` raise when what they check fails, as the
tool exits with an error: the error carries the first problem's code and location, and `details`
holds the whole report, a validation report (`{"ok": False, "diagnostics": [...]}`) or the check
report. `Workflow(...)` raises the same way when the configuration is invalid. Each diagnostic of a
validation report has the keys of the tool's `--json` diagnostics: `severity`, `code`, `name`,
`category`, `message`, `location` and, when set, `step`, `module` and `details`. A
`SERVICE_UNAVAILABLE` diagnostic (code 407, category `compatibility`), for a service that a module
requires and no usable provider offers, has the details `{"service", "major", "minor",
"search_paths", "providers"}`, the providers being those that offer the service's name but cannot
serve it, each with its `reason`:

```python
import praenora.runtime as pr

config = "tests/fixtures/configs/cli/service/workflow.yaml"
paths = {"module_paths": ["build/dev/test-modules"], "catalog_paths": ["tests/fixtures/catalog"]}
try:
    pr.validate(config, **paths)
except pr.PraenoraError as error:
    print(error.code, error.name, error.location["pointer"])
    print(error.details["diagnostics"][0]["details"]["service"])
print(pr.validate(config, provider_paths=["build/dev/test-providers/test-echo"], **paths))
```

```text
407 SERVICE_UNAVAILABLE /requires/services/0
test.echo
{'ok': True, 'diagnostics': []}
```

```python
import praenora.runtime as pr

config = "examples/workflows/estimate-predict/workflow.yaml"
try:
    pr.validate(config)  # no module path: the example modules are not found
except pr.PraenoraError as error:
    print(error)
    print(error.code, error.name, error.category, error.location["line"])
    for diagnostic in error.details["diagnostics"]:
        print(diagnostic["location"]["line"], diagnostic["name"], diagnostic["message"])
```

```text
MODULE_NOT_FOUND: examples/workflows/estimate-predict/workflow.yaml: validation failed: 2 error(s)
301 MODULE_NOT_FOUND validation 28
28 MODULE_NOT_FOUND module example-estimator@0.1.0 was not found; searched '.../build/dev/lib/praenora/modules'
31 MODULE_NOT_FOUND module example-predictor@0.1.0 was not found; searched '.../build/dev/lib/praenora/modules'
```

Wrong arguments are `INVALID_ARGUMENT` (a tick count of `-1`, an unknown log level) or a `TypeError`
(a path of the wrong type).

## Threads and interruption

Every call into the runtime releases the GIL, so while one thread runs a workflow, another may call
its `status()`, `metrics()`, `latest_outputs()` or `request_stop()`. The control calls (`start`,
`tick`, `run`, `close`) are made one at a time.

Two workflows may also run at the same time on two threads, and a workflow beside a
`check_module()`. When they use the same module library whose manifest keeps the default
`features.concurrency: "module"`, their calls into it take turns: a call waits while another run's
call into the library runs, and the wait does not count toward its timeout. Once a call into such a
library has become unresponsive, whether it failed the run with `MODULE_UNRESPONSIVE` or was only
the last error of a `finalize` or `destroy` step of a run that ended `completed` or `stopped`, the
abandoned call may still be running, and until it returns the library is refused:

- a workflow's `start()`, `tick()` or `run()` that would load the library, or call `create`,
  `initialize` or `invoke` in it, raises `MODULE_UNRESPONSIVE`, whose details name the abandoned
  call (`abandoned_call`); a teardown leaves the workflow's instances of the library as they are;
- `validate(..., load=True)` raises with the report's first error, `MODULE_UNRESPONSIVE` when that
  is the library's; in the report (the error's `details`), the library's diagnostic names the
  abandoned call in its own `details`;
- `check_module()` raises `FAILED`, its report's `library.load` check failed with a message that
  names the abandoned call.

A new interpreter starts without the abandoned call. Taking turns keeps the runs safe, not
reproducible: they still share the library's global state, so run workflows one after another when
their results must not depend on each other
([specs/threading-and-ownership.md](specs/threading-and-ownership.md#concurrency-across-instances)).

Ctrl+C does not interrupt a blocking `run()`: Python handles the signal only when the call returns
to Python ([open-questions.md](open-questions.md), OQ-25). To stop a run on Ctrl+C, either run the
ticks in a loop, which returns to Python after each tick:

```python
import praenora.runtime as pr

config = "examples/workflows/estimate-predict/workflow.yaml"
with pr.Workflow(config, module_paths=["build/dev/modules"], log_level="warn") as wf:
    wf.start()
    while True:
        result = wf.tick()
        print(result["tick"], result["logical_time"], result["outputs"])
        if result["finished"]:
            break
```

(leaving the `with` block on `KeyboardInterrupt` closes the run as `stopped`), or run it on another
thread and request the stop from the main thread:

```python
import threading

import praenora.runtime as pr

config = "examples/workflows/estimate-predict/workflow.yaml"
with pr.Workflow(config, module_paths=["build/dev/modules"], log_level="warn") as wf:
    runner = threading.Thread(target=wf.run)
    runner.start()
    try:
        while runner.is_alive():
            runner.join(0.1)
    except KeyboardInterrupt:
        wf.request_stop()
        runner.join()
    print(wf.state)
```

## Logging

A run's `log_level`, `log_format` and `log_file` arguments are the run's options of
[specs/observability.md](specs/observability.md#settings), which ranks them as it ranks the tool's
`--log-level`, `--log-format` and `--log-file`. Records go to the process's standard error (file
descriptor 2), and also to the log file when one is set. Two limits:

- A step whose configuration sets its own `log_level` keeps that level: Python cannot make it
  quieter. The example's `predict` step logs at `debug` even with `log_level="warn"` or `"off"`.
- Python cannot turn off the runtime's logging to standard error. To keep the records out of a
  program's output, redirect file descriptor 2, or log to a file and silence the run with
  `log_level="off"` where no step sets its own level.

The few records of the runtime itself, outside a run (such as "runtime ready" at level `debug`),
follow `PRAENORA_LOG_LEVEL` and `PRAENORA_LOG_FORMAT` when they are set.

## Tests

CTest's `python.unittest` (label `python`) runs the `unittest` suite of `tests/python/` with the
interpreter the extension was built for, against the package in the build tree:

```bash
ctest --preset dev -L python -V
```

It sets `PYTHONPATH` to `build/<preset>/python`, the fixture and example locations the tests use
and `PRAENORA_LOG_LEVEL=off`, and unsets the other `PRAENORA_*` variables the runtime reads, so the
caller's environment cannot change the results. In builds with `PRAENORA_SANITIZERS` the test is
registered but disabled, because an instrumented extension does not load into an interpreter that is
not instrumented; `ctest --preset dev-asan -L python`, a run of that label alone, then exits with 8
([testing.md](testing.md#sanitizer-builds)).

## Building a wheel

The root [`pyproject.toml`](../pyproject.toml) builds the distribution `praenora-runtime` with
scikit-build-core (0.10 or newer) and SWIG (4.2 or newer, from PyPI or the system). It sets
`minimum-version = "0.10"`, so a newer scikit-build-core keeps the defaults of 0.10 (the wheel's
metadata is `Metadata-Version: 2.1`) and does not warn about the `tool.scikit-build.metadata` table
that reads the version from `CMakeLists.txt`. It configures this CMake project with the Python
bindings on and the tests, the examples and the tool off, so the C++ dependencies must be findable,
for example through the `CMAKE_PREFIX_PATH` environment variable. In the environment, prefixes are
separated by `:` (`;` on Windows), not by the `;` of a preset's cache variable.

Verified on macOS arm64 (macOS 15) with CPython 3.12, scikit-build-core 1.1.2 and Homebrew's
SWIG 4.5.1, offline: scikit-build-core is installed once into a virtual environment, and the build
uses that environment and the SWIG on `PATH` instead of downloading its requirements
(`--no-build-isolation`). From the repository root (git ignores `build/`):

```sh
uv venv --python 3.12 build/wheel-tools
uv pip install --python build/wheel-tools "scikit-build-core>=0.10"
CMAKE_PREFIX_PATH="/opt/homebrew:/path/to/json-schema-validator/prefix" \
    uv build --wheel --no-build-isolation --python build/wheel-tools/bin/python -o build/wheel \
    -C cmake.define.CMAKE_OSX_DEPLOYMENT_TARGET=15.0
```

pip does the same, run by the environment's Python (`pip --python build/wheel-tools/bin/python
wheel . --no-deps --no-build-isolation -w build/wheel
--config-settings=cmake.define.CMAKE_OSX_DEPLOYMENT_TARGET=15.0`, with pip 22.3 or newer). Not yet
verified: a build that downloads its requirements (the default build isolation of pip and uv, with
SWIG from PyPI), and wheels for Linux, Windows, and Python 3.10 and 3.11.

**macOS deployment target.** scikit-build-core tags the wheel with `CMAKE_OSX_DEPLOYMENT_TARGET`
when it is given as a `cmake.define`, as above; otherwise with `MACOSX_DEPLOYMENT_TARGET` from the
environment; otherwise with the running macOS version. It does not see the project's default of
11.0 in `CMakeLists.txt`, and CMake keeps that default even when `MACOSX_DEPLOYMENT_TARGET` is set.
So give the define, with the macOS version the dependency binaries target: Homebrew builds for the
running release, as for the [local presets](developer-guide.md#local-presets). Without it the
binaries target 11.0, the linker warns about every Homebrew library, and the tag names the running
macOS version all the same (`macosx_15_0_arm64` on macOS 15).

**Contents.** A wheel build has only the Python package's install rules, so the wheel holds just
`praenora/runtime/` and its `.dist-info` (`METADATA`, `WHEEL`, `RECORD` and `licenses/LICENSE`);
`praenora` has no `__init__.py`. `licenses/LICENSE` is this repository's MIT license, which
scikit-build-core copies from the root by its default `wheel.license-files` patterns, and
`METADATA` says `License: MIT`, from `pyproject.toml`'s `license = { text = "MIT" }`
([ADR-0040](adr/ADR-0040-mit-license.md)). The license files of the third-party code linked into
the runtime library, such as a static json-schema-validator, are not in the wheel, whose
distribution is open ([open-questions.md](open-questions.md), OQ-13). The wheel built above is
1.04 MB, and 2.4 MB once installed:

| File | Size | From |
|---|---|---|
| `__init__.py` | 18.3 kB | `python/praenora/runtime/`, through `wheel.packages` |
| `_bindings.py` | 9.3 kB | SWIG's proxy module, installed by the build |
| `_bindings_native.cpython-312-darwin.so` | 349 kB | The extension, installed by the build |
| `libpraenora_runtime.0.2.dylib` | 2.1 MB | The runtime library, installed by the build |

The wheel holds no module host (`praenora-module-host`): steps in process mode are refused there
(checked with the wheel above) with `MODULE_HOST_UNAVAILABLE`, `details.reason` `"missing"`
([Process isolation](#process-isolation), [open-questions.md](open-questions.md), OQ-13).

The binaries are Release builds, stripped on installation. The extension finds the runtime library
beside itself (an RPATH of `$ORIGIN` on Linux, `@loader_path` on macOS; on Windows the DLL lies next
to the extension), and it is specific to one Python version (no stable-ABI wheel;
[open-questions.md](open-questions.md), OQ-21). Each file is in the wheel once:

- A wheel cannot hold a symbolic link, so the runtime library's soname link would be stored as a
  second copy of the whole library. In a wheel build the library's file name is its soname
  (`libpraenora_runtime.0.2.dylib`; `libpraenora_runtime.so.0.2` on Linux), so there is no link;
  its Mach-O current version is still the full runtime version.
- `__init__.py` comes from `wheel.packages` only, and the install rules add only what the build
  makes. scikit-build-core leaves out a package file that an install rule has already installed, so
  installing a file both ways does not fail: the install rule's copy wins without a warning.

**Installing it.** Into a fresh virtual environment:

```sh
uv venv --python 3.12 build/wheel-test
uv pip install --python build/wheel-test build/wheel/praenora_runtime-0.2.0-cp312-cp312-macosx_15_0_arm64.whl
build/wheel-test/bin/python -c 'import praenora.runtime as pr; print(pr.version()["runtime"], pr.version()["build_type"])'
```

prints `0.2.0 Release`. The tests of `tests/python` pass against the installed package as well, run
with that environment's Python, CTest's environment for `python.unittest` and `PYTHONPATH` naming
the environment's `site-packages` instead of `build/<preset>/python`.

The runtime's third-party shared libraries are not bundled into the wheel: the macOS wheel above
links yaml-cpp, spdlog and fmt from Homebrew by their absolute paths (json-schema-validator is
linked statically, and nlohmann/json is header-only). Such a wheel works only where those libraries
are installed at the same places, so it is not yet a portable binary; bundling and manylinux wheels
are open ([open-questions.md](open-questions.md), OQ-13).

## Platform notes

- **Windows** (not yet verified): the extension's suffix is `.pyd`, and `praenora_runtime.dll` and
  the third-party DLLs are copied beside it in the build tree, where Python looks for an
  extension's libraries. Debug builds link the release Python library.
- **Multi-configuration generators** (not yet verified): every configuration builds into the same
  package directory, so `PYTHONPATH` stays `build/<preset>/python`.
