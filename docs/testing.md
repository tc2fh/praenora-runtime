# Testing

This guide explains how to run the tests, what each label covers, how golden files are refreshed,
how a failure leads to the component and the party that has to fix it, the acceptance gate for
module versions, and how to add tests. The commands run from the repository root against the `dev`
build ([developer-guide.md](developer-guide.md#configure-build-and-test)) and were run on macOS
arm64.

## Running the tests

```bash
ctest --preset dev
```

runs every test of the build. The test presets print the output of failing tests and fail when no
test matches the selection. Subsets:

```bash
ctest --preset dev -N
ctest --preset dev -L lifecycle
ctest --preset dev -R '^Loader\.'
ctest --preset dev -R '^Example\.RunsTwelveTicks$'
ctest --preset dev -L docs -V
ctest --preset dev -L unit --output-junit "${TMPDIR:-/tmp}/praenora-junit.xml"
```

- `-N` lists the tests without running them.
- `-L <label>` selects a label and `-LE <label>` leaves one out; labels are regular expressions,
  so `-L 'unit|protocol'` selects two.
- `-R <regex>` selects tests by name. GoogleTest tests are named `Suite.Case`, and the suite names
  the component, so `-R '^Loader\.'` runs the loader's tests.
- `-V` prints the output of every test, passing ones included.
- `--output-junit <file>` writes JUnit XML, for tools that read it; a relative file name is relative
  to the build directory.

A GoogleTest executable can also run on its own, with GoogleTest's options:

```bash
./build/dev/bin/praenora_unit_tests --gtest_filter='Csv.*'
```

The tests write files only under the system's temporary directory or the build tree, use no
network, and need nothing from the environment: tests that read `PRAENORA_*` variables set them
themselves, except the tests of the official AsyncAPI schema, to which CTest gives the schema's
path ([below](#the-official-asyncapi-schema)).

## Labels

| Label | What it covers | Tests |
|---|---|---|
| `unit` | Runtime internals, white-box: documents and YAML, canonical JSON, SHA-256, versions, durations, seeds, CSV and other input sources, configuration, logging, metrics, health, status files, run records, the store of shared compiled models (its limits and its least-recently-used order), the sandbox profiles (their names, the restricted profile's text, a module host's private temporary directory), the error table against `status.h`, the SDK's headers, adapter and test kit, and the antimony provider's rules that read a source's text alone. | `praenora_unit_tests` |
| `protocol` | Every schema and catalog entry, valid and invalid documents per kind, graph validation and schedules, graph export, and the committed AsyncAPI documents, also against the official AsyncAPI 3.0.0 schema when the build names a copy of it ([below](#the-official-asyncapi-schema)), and through the official AsyncAPI parser when the build names an installation of it ([below](#the-official-asyncapi-parser)). | `praenora_protocol_tests`, `praenora_asyncapi_official_schema_tests`, `asyncapi.parser.accepts_committed_documents`, `asyncapi.parser.refuses_broken_copies` |
| `contract` | Manifests and discovery, the loader, module instances, the engine and the contract checks, through the real loader and the fixture modules; service providers and runs that use their services, with the fixture providers; the service checks of the simulation and model conversion services; provider libraries as files: their exported symbols, and the same results in two processes, and how the driver of the latter shows two outputs that differ and a run that fails. | `praenora_contract_tests`, `providers.<provider>.exports`, `providers.<provider>.processes`, `providers.run_twice.shows_first_difference` |
| `negative` | What must be refused: bad manifests, configurations and graphs, incompatible modules, failing validations, services no provider serves, providers that do not load in a run, each with its code and location. | `praenora_negative_tests` |
| `lifecycle` | Call order and cleanup, timeouts, unresponsive modules, cancellation and stop requests, calls into one library from instances of several loads of it (they take turns, and are refused behind an abandoned call), and the runtime services an instance is served. | `praenora_lifecycle_tests` |
| `isolation` | Process isolation, white-box: child processes and their process groups, module instances in a module host (calls with the results of the runtime's process, logs, lookups, cancellation and deadlines, a call that never returns killed, crashes in every call and at load, garbage on the channel, sink limits, load errors, a missing or foreign module host, a stop during the start, a runtime that dies), and the service providers of a module in its module host (their services, their log records, their load errors, and that the runtime's process loads only the providers of its own steps), and the compiled models that module hosts share (through the store, during the load and between calls, under each provider's namespace, none for a step that does not share, nor for a host too short for the store's callbacks, entries and keys over their limits refused, store requests that break the protocol, a stored model that does not load compiled instead), writes to a module host that stops reading or reads slowly (none waits for good, and a stop during one leaves no failed teardown), and the sandbox profiles of a module host (what the restricted profile refuses and allows, its temporary directory, a sandboxed runtime that cannot apply it, module hosts that share compiled models under it) ([below](#module-hosts)). | `praenora_isolation_tests` |
| `integration` | The public API through the shared library only, and the example workflow end to end: golden outputs, determinism, the fingerprint, files on disk, status queries during a run; and steps in module hosts: the same outputs, trace and fingerprint as in the runtime's process, crashes, a killed call, stops, Ctrl+C, `validate --load`, and service providers loaded in module hosts (the same outputs, trace and provider records; a changed provider library, a provider that fails to start or crashes; with the engines' providers, roadrunner and antimony, and a long antimony translation ended with its module host), and steps that share compiled models (the second loads what the first compiled, a step that does not share never reads the store, and the outputs and fingerprint are those of steps that do not share and of the runtime's process; with test-simulation, and with roadrunner), and steps under the restricted sandbox profile (a run and `validate --load` give the module host its profile, and the results are those of the runtime's process; with test-simulation, and with roadrunner). | `praenora_integration_tests` |
| `cli` | The `praenora` tool: every command, exit code, `--json` shape and diagnostics format (`cli.<command>.<case>`, driven by `tests/cli/run_cli.cmake`), that driver passing each argument of a command to its program, an empty one included (`cli.driver.passes_empty_arguments`), and showing a command that fails (`cli.driver.shows_failing_command`), and the unit tests of its helpers (`CliTable.*`) and of what it shares with the Python extension (`Frontend.*`, `src/frontend`). | `praenora_cli_tests`, the CLI driver and `tests/cli/check_driver.cmake` |
| `python` | The Python package: CLI parity, errors, the context manager, calls from a second thread. One CTest test, `python.unittest`, runs the `unittest` suite of `tests/python`. | `python.unittest` |
| `install` | An installation into a scratch prefix: its layout (service providers, the module host and the license files included: this repository's `LICENSE`, byte for byte, a file for each dependency built in the tree, and the files of the directories that `PRAENORA_THIRD_PARTY_LICENSE_DIRS` names), a module and an application built against it, the installed tool, a step run in the installation's module host, a moved installation, no build or source paths in installed files. The archive of an installation that the target `package` makes ([developer-guide.md](developer-guide.md#archiving-an-installation)), made with `cpack -C <config>`: its name (`praenora-runtime-<version>-<os>-<arch>-<compiler>-<config>`, `.tar.xz`, or `.zip` on Windows), its `.sha256` file in the format of `sha256sum`, one top-level directory, and its files, those of an installation without the example modules, byte for byte; a configuration other than Release or Debug must make cpack stop, writing no archive; and there is no source package (no target `package_source`, and cpack stops for the source configuration). | `install.consumer`, `install.package` |
| `perf` | The performance probe `praenora_perf`: 2000 ticks of a four-step chain of no-op modules, with and without payload validation, and, on POSIX, with every step in a module host. It prints a JSON report and fails only when the probe fails, never on timings. | `perf.validation_strict`, `perf.validation_off`, `perf.process_isolation` |
| `docs` | The documentation against the source tree: every error code in the error taxonomy, every schema file in the protocol specification, every ADR in its index, every relative link resolving, the open-questions register complete. | `docs.consistency` |
| `ci` | The CI tooling: `cmake/praenora_fetch_runtime_deps.cmake`, which downloads, checks and unpacks a pinned dependency bundle, run on fake bundle archives (`.tar.xz` and `.zip`) from `file://` URLs whose paths hold a space and a non-ASCII character, with pin files of the test's own: the bundle directory it prints, an absolute path also for a relative destination, and appends to the file that `GITHUB_ENV` names; what a second run reuses (the kept archive and the bundle) and what it downloads or unpacks again (an archive with another SHA-256, a bundle whose manifest names another version); that the leftovers of a stopped run (a partial download, a staging directory) are removed; a download that fails tried 3 times, and one with another SHA-256 than the pinned one not tried again; its errors (an archive with another SHA-256 than the pinned one, a platform the pin does not name, an archive without the manifest or whose manifest names another version, `PLATFORM` or `DESTINATION` not set); and that it accepts each platform of the repository's pin file, `cmake/PraenoraRuntimeDepsPin.cmake`, and checks a download against its pinned SHA-256. And the workflow `.github/workflows/build.yml`, read as text in the format its header comment states ([releasing.md](releasing.md)): its matrix keys are the platforms of the pin file, each once, each with a configure, a build and a test preset `ci-<key>` in `CMakePresets.json`; each key's runner is the label of its operating system (`ubuntu-24.04`, `macos-26`, `windows-2022`), and each `runs-on` is one of these labels or the matrix's runner, never a `-latest` label; and each action is pinned to a commit, 40 hexadecimal digits, followed by a comment `# v<version>`; and no `runs-on:`, `uses:`, `runner:` or `- key:` leaves its value to a later line. Lines that end in `\` are read as lines of their own, so each problem names its line. No network. | `ci.fetch_runtime_deps`, `ci.workflow_consistency` |

The suites of the GoogleTest labels name the components they test:

| Label | Suites |
|---|---|
| `unit` | `Adapter`, `AntimonyImportCheck`, `AntimonyTextCheck`, `Channel`, `CompiledModelStore`, `Config`, `Csv`, `Duration`, `Error`, `ErrorTaxonomy`, `Fingerprint`, `Health`, `InputSource`, `Json`, `JsonDocument`, `JsonNear`, `LibraryGate`, `Locale`, `Logging`, `Metrics`, `MetricsJson`, `ModelConversionAbi`, `NestingScan`, `ObservationCsv`, `OutputDir`, `OutputRecorder`, `OutputSink`, `PathAnchors`, `Paths`, `Platform`, `ProviderAbi`, `Resources`, `RunRecord`, `RuntimeContext`, `Sandbox`, `SbmlTextCheck`, `Schema`, `SchemaRegistry`, `SdkAbi`, `Seed`, `SemVer`, `Sha256`, `SimulationAbi`, `StatusFile`, `StatusJson`, `StatusNames`, `TestKit`, `Time`, `TraceWriter`, `ValidationReport`, `Version`, `Yaml` |
| `protocol` | `Catalog`, `Graph`, `GraphExport`, `Schemas` |
| `contract` | `Discovery`, `Engine`, `Envelope`, `Instance`, `Loader`, `Manifest`, `ModuleCheck`, `ProviderDiscovery`, `ProviderIndex`, `ProviderLoader`, `ProviderManifest`, `Runtime`, `ServiceRun`, `Services`, `TestConversionProvider`, the parameterized `SimulationService` and `ModelConversionService` (the service checks: `<Provider>/SimulationService.<Check>/<ParameterSet>` and `<Provider>/ModelConversionService.<Check>/<ParameterSet>`), and `RoadrunnerProvider` and `RoadrunnerEngine` with the roadrunner provider and `AntimonyProvider` with the antimony provider |
| `negative` | `Compat`, `Config`, `ConfigBadFixtures`, `Graph`, `IsolationValidation`, `Manifest`, `ProviderLoadFailure`, `ProviderManifest`, `ServiceValidation`, `Validation` |
| `lifecycle` | `Cancel`, `Lifecycle`, `ServiceLookups`, `ServiceVisibility`, `Timeout` |
| `isolation` | `ChildProcess`, `Isolation` |
| `integration` | `Api`, `Example`, `IsolationExample`, `IsolationRun`, `IsolationValidation`, and the parameterized `IsolationRunCrash` (a crash in each call of a run: `EveryCall/IsolationRunCrash.EndsOnlyItsStep/<Call><How>`) and `IsolationRunProviders` (a module that uses the fixture providers, in a module host and in the runtime's process: `Fixtures/IsolationRunProviders.GiveTheResultsOfTheRuntimesProcess/<Module>`) |
| `cli` | `CliTable`, `Frontend` (beside the `cli.<command>.<case>` driver tests, which are not GoogleTest) |

Builds without the examples, the CLI, the Python bindings or the install rules have no tests that
need them: `PRAENORA_BUILD_EXAMPLES=OFF` drops the `Example.*` tests, the CLI tests of the example
workflow and the contract checks of the example modules (and the Python tests that need the example
modules are skipped), `PRAENORA_BUILD_CLI=OFF` drops the `cli` label, `PRAENORA_BUILD_PYTHON=OFF`
drops `python.unittest`, and `PRAENORA_INSTALL=OFF` drops `install.consumer` and `install.package`
(a build of praenora-runtime as a sub-project of another has no `install.package`). On Windows,
where no module host is built, the `isolation` label holds only
`Isolation.IsNotAvailableOnWindows`, and the tests of steps in module hosts (`IsolationRun.*`,
`IsolationRunCrash`, `IsolationRunProviders`, `IsolationExample.*`, `cli.run.isolation_*`,
`perf.process_isolation`, the Python `IsolationWorkflowTest`) are left out or check the refusal.

### Module hosts

The tests of process isolation start module hosts (`praenora-module-host`) from the build tree's
`libexec/praenora/`, which every test target that needs it depends on, and check after each test
that none is left running.

- Some fixture modules end their module host on purpose: `fx-crash` (in the call and in the way
  its parameters say), `fx-crash-at-load` (a static initializer); `fx-scribble` writes garbage onto
  the channel, and `fx-hang-at-load` never finishes loading. So do the fixture providers
  `test-start-crashes` (in its `start()`) and `test-echo-crashes` (in its `echo()`, at once or from
  a thread of its own after the call, once the file that `PRAENORA_TEST_ECHO_CRASH_AFTER` names
  exists: the test makes it after the call returned, and waits for the record of the crash before
  its next call, so the crash is between calls on a machine of any speed). No test loads them in
  its own process. `fx-crash` also writes its parameter `print` to its standard output's buffer in
  each invoke, without flushing it, for the test that a module host asked to exit flushes it (with
  `timeouts.lifecycle: none` too).
- `fx-deep-stack-at-load` uses 2 MiB of stack in a static initializer: it loads on the test's main
  thread and in a module host, whose load thread has a main thread's stack, and would crash on a
  thread's default stack (512 KiB on macOS).
- The fixture provider `test-store` (`tests/fixtures/providers/test-store/`) offers `test.echo`
  and does with the shared compiled models of its provider host what its input says (probe, save,
  load, or save and load later from a thread of its own, once the file that
  `PRAENORA_TEST_STORE_LATER` names exists; and save and load in `start()`, as
  `PRAENORA_TEST_STORE_AT_START` asks). The store belongs to the test's process, so the tests of
  shared compiled models use keys, and model texts, that no earlier test of the process used (a
  counter and the process id; for a test-decay model, spaces after its JSON object; for an SBML
  model, an XML comment): they pass in one process and repeated.
- The helper `praenora_fake_module_host` stands where the runtime looks for its module host (a
  test hook) and answers as no module host of this build does, in the mode its environment says:
  among them, modes that break the rules of the shared compiled models (one announces a save of
  128 MiB from the module host of a step that does not share and sends none of it, which the
  runtime refuses at the frame's header, without waiting for its payload), and three that answer the
  load as a module host does (they load the module with the loader in their own process) and then
  stop reading their channel, one of them after it has answered a create and an initialize, for
  the tests that no write of the runtime waits for good on such a module host
  (`tests/isolation/deaf_module_host_test.cpp`: a call larger than the channel's buffers, a stop
  during it, in a run too, and the end of a module host that does not take a store answer). The
  bounds of a write against a peer that keeps taking a few bytes are tested on a socket pair
  (`Channel.ABoundedWriteKeepsItsBoundAgainstAPeerThatReadsSlowly`).
- `Isolation.AProviderHostTooShortForTheStoreOffersNone` starts the fixture provider `test-store`
  with a host whose `struct_size` ends before the store's callbacks in a helper process of its own
  (`praenora_isolation_short_host_probe`), since a provider is started once in a process, and
  earlier tests start `test-store` in the test's process through the provider loader.
- The sandbox profiles are tested with the fixture module `fx-sandbox-probe`, which tries to
  connect (to a TCP listener of the test on `127.0.0.1`), fork, spawn, write outside and inside its
  temporary directory, read, link to a file outside (a hard link, and a symbolic link written
  through), open `/dev/stderr` by its path, and leave an immutable file behind, as its library
  loads and in an invoke, and reports what each try got (with the environment variable
  `PRAENORA_TEST_SANDBOX_PROBE_EXIT_AT_LOAD` set, a refused try as its library loads ends its
  process instead, as a library whose static initializer fails hard would, for the test that
  `validate --load` loads a module under each step's profile), and whether its standard error is
  `/dev/null` (under the profile it must be the runtime's, which the module host points at
  `/dev/null` only while it applies the profile). It writes to its standard error only when a test
  asks it to, and that test gives the module host a scratch file as its standard error (it points
  its own descriptor 2 at the file while the module host starts): under the profile the line lands
  there, although the profile refuses making a file in that directory. One test applies the
  profile with a double quote, a backslash and a character beyond ASCII in the path of the module
  host's directory (it sets `TMPDIR` for the module host's runtime), and another checks the warning
  for a directory that cannot be removed (below a `TMPDIR` of its own, which it removes with the
  immutable file however it ends). macOS applies no second sandbox, so a test process that runs in
  a sandbox already (a developer tool's or a CI service's own) cannot test the restricted profile:
  there its tests skip, saying "nested sandbox: this process runs in a sandbox already, ...",
  which CTest shows as skipped: the `SKIP_REGULAR_EXPRESSION` of the two executables that have
  such tests (`praenora_add_gtest(... SANDBOX_SKIP)`) names that start of the message, beside
  GoogleTest's own skips. Whether the process runs in a sandbox is asked of `/usr/bin/sandbox-exec`
  (`tests/support/sandbox_support.hpp`), not of the runtime. A few tests run in both places and
  check the other side there: in a sandbox, that a restricted load fails with
  `MODULE_HOST_UNAVAILABLE`, reason `sandbox`:
  `Isolation.ARestrictedModuleHostOfASandboxedRuntimeCannotStart` (which outside a sandbox runs a
  helper, `praenora_isolation_restricted_probe`, in a sandbox of its own),
  `IsolationRun.ARestrictedStepRunsUnderItsProfile` and
  `IsolationValidation.LoadGivesAModuleHostItsProfile`. To run the sandbox tests, run CTest from a
  shell outside any sandbox, for example `ctest --preset dev -L 'isolation|integration'`; with the
  roadrunner provider, `ctest --preset dev-roadrunner -L 'isolation|integration'`. Under the
  profile a module host cannot start a sanitizer's symbolizer (`atos` needs a fork and an exec), so
  a sanitizer's report from a restricted module host comes without symbols.
- The tests of service providers in module hosts compare a run in the runtime's process with the
  same run in module hosts. A provider starts, and logs its start, once in a process, so the
  comparison leaves out each provider's records of its start (up to its `provider.loaded` event),
  which the runtime's process has only when no earlier test started the provider, and the tests
  check them in the module host's log. Like the other provider tests, they pass when one process
  runs them all, and repeated (`--gtest_repeat`): a test that checks what a load opens in the
  runtime's process compares `opened_providers_for_testing()` before and after it, and
  `Isolation.TheRuntimesProcessLoadsOnlyTheProvidersOfItsSteps`, which needs `test-simulation` and
  `test-conversion` not open yet, is skipped, saying so, when an earlier test opened them.
- On macOS every crash of a module host on purpose leaves a crash report in
  `~/Library/Logs/DiagnosticReports`, and the system's crash reporter makes each crash take a moment
  (up to a few seconds); each run of the `isolation`, `integration`, `cli` or `python` label adds a
  few reports, which can be deleted.
- Under sanitizers, the tests whose module hosts crash on purpose append
  `handle_segv=0:handle_abort=0` to `ASAN_OPTIONS` (with `detect_leaks=0`) and to `TSAN_OPTIONS` for
  those module hosts (`OwnSignals`, `tests/support/scoped_environment.hpp`; the CLI test
  `cli.run.isolation_crash` sets them for its command): the module host then ends by the signal of
  the crash, which the tests check, instead of by the sanitizer's report.
- `IsolationRun.ACtrlCReachesTheRuntimeOnly` makes the test process lead a process group of its
  own and sends SIGINT to that group, as a terminal's Ctrl+C would; it is skipped, visibly, where
  the process may not change its group.

### The roadrunner provider

A build with `PRAENORA_WITH_ROADRUNNER`
([developer-guide.md](developer-guide.md#the-roadrunner-provider)) adds tests of the roadrunner
provider ([specs/simulation-service.md](specs/simulation-service.md#the-roadrunner-provider)),
most in the `contract` label:

- the service checks with two SBML models whose solutions are known exactly
  (`Roadrunner/SimulationService.<Check>/Decay` and `.../TwoCompartments`, from
  `tests/contract/simulation_service_roadrunner_test.cpp`), which require agreement to a relative
  1e-9;
- `RoadrunnerProvider.*` (`tests/contract/roadrunner_provider_test.cpp`): the engine identity, the
  model texts it refuses (paths, URIs, texts libRoadRunner would read as a path or a URI, document
  type declarations, required packages, `comp`, a flux-balance model it cannot simulate), SBML
  Level 1, Level 2 and Level 3 Version 1 (the other models it simulates are Level 3 Version 2), the
  ids of a model with every kind of rule and of models whose stoichiometry an initial assignment or
  a `stoichiometryMath` sets, events, delayed events across runs and at the start, simultaneous
  events, solver settings, the step limit, solver steps (also at the row limit) and cancellation
  within a run;
- `RoadrunnerEngine.*` (`tests/contract/roadrunner_engine_test.cpp`, its own executable
  `praenora_roadrunner_engine_tests`, which compiles the provider's engine code with the provider's
  definitions): a model loaded from its saved state is the model compiled from its text, field by
  field (ids, the engine's index of each parameter, how each quantity is read, events), saves the
  same state again (but for libRoadRunner's number of the object in its process), and simulates the
  same doubles, bit for bit, events included, in two runs; a host too short for the store's
  callbacks offers no store; and an
  entry of the shared compiled models that does not load says why (garbage, cut short, a changed
  byte under its SHA-256, another engine, bytes that are no saved state);
- `Isolation.ARoadrunnerStateThatDoesNotLoadIsCompiledInstead` (label `isolation`): garbage, a
  changed byte and a state cut short under a model's key, each compiled instead with a warning and
  the outputs of a compile; and
  `IsolationRun.RoadrunnerStepsThatShareCompiledModelsGiveTheResultsOfStepsThatDoNot` (label
  `integration`);
- `providers.roadrunner.exports` and `providers.roadrunner.processes`
  ([Adding tests](#adding-tests), provider files);
- `Api.TheBuildTreeRuntimeFindsTheRoadrunnerProvider` (label `integration`), and
  `IsolationRun.RoadrunnerGivesTheResultsOfTheRuntimesProcess` (label `integration`), which runs an
  SBML model through a plan and a kernel in a module host and in the runtime's process and finds the
  same output bytes, trace and provider records; and `install.consumer` checks that the
  installation holds the provider and that the installed tool lists it, usable.

In such a build the build tree's runtime finds the provider without a provider path, so a test that
expects no provider of `praenora.simulation@0` there (`cli.providers.list_empty`, part of
`Api.RunSimulatesThroughTheSimulationService`) is left out or adapted. The SBML models are in
`tests/fixtures/models/sbml/`. `ctest --preset dev-roadrunner-tsan -L contract` runs the concurrency
checks under ThreadSanitizer, which sees only the provider's own code: libRoadRunner, LLVM and the
code they generate are not instrumented.

### The antimony provider

A build with `PRAENORA_WITH_ANTIMONY`
([developer-guide.md](developer-guide.md#the-antimony-provider)) adds tests of the antimony provider
([specs/model-conversion-service.md](specs/model-conversion-service.md#the-antimony-provider)),
most in the `contract` label:

- the service checks with Antimony and SBML (`Antimony/ModelConversionService.<Check>/Antimony`,
  from `tests/contract/model_conversion_service_antimony_test.cpp`): exact translations both ways
  (decay, function definitions, events, rules, a submodel flattened, a submodule chosen by
  `module`, SBML Level 2 Version 4 and Level 3 Versions 1 and 2), the sources each rule refuses, a
  layout request (`UNSUPPORTED`), a syntax error, and a generated chain of 700 reactions whose
  translation holds the provider's lock;
- `AntimonyProvider.*` (`tests/contract/antimony_provider_test.cpp`): an import of a missing file,
  refused in code and harmless in every form of comment, string and block; the round trip; the main
  module and the option `module`; function definitions; libSBML's errors with their line and
  column; warnings; the engine object against what the libraries report; byte order marks and
  CR LF; non-ASCII text; error text that is valid UTF-8; libAntimony's errors, which have no line;
  SBML that names other files, which are never read; distributions, which do not convert back;
  layout requests, refused the same way in different seconds of the wall clock; and SBML that holds
  a layout (`tests/fixtures/models/sbml/laid-out.xml`), which converts to the same Antimony in
  different seconds (both tests sleep until just past the next second between their calls);
- `providers.antimony.exports` and `providers.antimony.processes`
  ([Adding tests](#adding-tests), provider files);
- `Api.TheBuildTreeRuntimeFindsTheAntimonyProvider` (label `integration`) and, in a build with the
  roadrunner provider too, `Api.AntimonyModelSimulatesThroughRoadrunner`, which converts an
  Antimony model with `antimony` and simulates the SBML with `roadrunner` in one process, to a
  relative 1e-9 of the exact solution; and `install.consumer` checks that the installation holds the
  provider and that the installed tool lists it, usable;
- in module hosts (label `integration`): `IsolationRun.AntimonyGivesTheResultsOfTheRuntimesProcess`
  (the same SBML bytes, trace and provider records as in the runtime's process),
  `IsolationRun.ALongAntimonyTranslationIsKilledAndTheNextConvertsAtOnce` (a generated chain of
  6000 reactions, which takes libAntimony about 19 s, ended with its module host 200 ms into the
  call, and the next step's conversion done at once; `converting-module` translates in its first
  invoke there, with parameter `convert_in`, so that only the invoke deadline is short and create
  and initialize keep the default lifecycle timeout under a sanitizer too), and, with the
  roadrunner provider too, `IsolationRun.AntimonyAndRoadrunnerGiveTheResultsOfTheRuntimesProcess`
  (both engines in one module host).

The unit tests of the rules that read a source's text alone (`AntimonyImportCheck.*`,
`AntimonyTextCheck.*` and `SbmlTextCheck.*`, in `tests/unit/antimony_source_checks_test.cpp`) need
no libAntimony and run in every build. As with roadrunner, `cli.providers.list_empty` is left out
of such a build. The Antimony models and the expected outputs are in
`tests/fixtures/models/antimony/`. `ctest --preset dev-antimony-tsan -L 'contract|integration|unit'`
runs the concurrency checks under ThreadSanitizer, which sees only the provider's own code:
libAntimony, libSBML and SBMLNetwork are not instrumented. The local preset `dev-engines` builds
both providers and runs the tests of both in one tree.

### The official AsyncAPI schema

A build whose cache variable `PRAENORA_ASYNCAPI_SCHEMA` holds the absolute path of a local copy of
the official AsyncAPI 3.0.0 JSON Schema adds three tests to the `protocol` label
(`tests/protocol/asyncapi_official_schema_test.cpp`). They compile that schema with the validator
of Praenora's own documents:

- `GraphExport.OfficialSchemaIsTheAsyncApi300Schema`: the file is the schema of AsyncAPI 3.0.0
  (its `$id`, and the `asyncapi` version it requires);
- `GraphExport.CommittedAsyncApiIsValidAgainstTheOfficialSchema`: every
  `spec/asyncapi/*.asyncapi.json` is valid against it;
- `GraphExport.OfficialSchemaRefusesBrokenAsyncApiCopies`: copies of a committed document, each
  broken in one place (two of them in a string format, `uri` and `regex`), are refused, with every
  issue at that place or at an object that encloses it, so the test above cannot pass on a schema
  that accepts anything or on a validator that skips formats.

Neither the build nor the tests download the schema, and it is not committed. The copy that was
checked is `schemas/3.0.0.json` of release v6.11.1 of
[asyncapi/spec-json-schemas](https://github.com/asyncapi/spec-json-schemas), with SHA-256
`d4571a420e6ffb7fcc7066c95a6db1202f299a3c51daa103d0706bf30f95e626`; both committed documents are
valid against it. Download it once, outside the source tree, then configure with it:

```bash
curl -fL -o "$HOME/asyncapi-3.0.0.json" \
    https://raw.githubusercontent.com/asyncapi/spec-json-schemas/v6.11.1/schemas/3.0.0.json
shasum -a 256 "$HOME/asyncapi-3.0.0.json"
cmake --preset dev -DPRAENORA_ASYNCAPI_SCHEMA="$HOME/asyncapi-3.0.0.json"
cmake --build --preset dev
ctest --preset dev -R '^GraphExport\.'
```

Every build of the tests compiles `tests/protocol/asyncapi_official_schema_test.cpp` into an
executable of its own, `praenora_asyncapi_official_schema_tests`, so a change to what it uses
breaks the build at once; only a build with the variable registers its tests. CTest gives them the
copy's path in the environment variable `PRAENORA_TEST_ASYNCAPI_SCHEMA`. Run on its own, the
executable needs that variable, and without it each test fails and says so:

```bash
PRAENORA_TEST_ASYNCAPI_SCHEMA="$HOME/asyncapi-3.0.0.json" \
    ./build/dev/bin/praenora_asyncapi_official_schema_tests
```

CMake makes a relative value absolute against the current directory; a value that then does not
name a file, or that holds a `;`, `[` or `]`, fails the configuration. Configuring with an empty
value (`-DPRAENORA_ASYNCAPI_SCHEMA=`) removes the three tests; the executable is still built.

The tests pass in the ThreadSanitizer and AddressSanitizer trees as they are: the JSON Schema
validator, jsoncons, is header-only and compiled with the tree's own flags. (With the earlier
validator, a library built without the sanitizer, compiling the official schema tripped a false
AddressSanitizer container-overflow report, and the tests ran with that check off; that carve-out
is gone, [ADR-0043](adr/ADR-0043-json-schema-dialects-and-validator.md).)

```bash
cmake --preset dev-asan -DPRAENORA_ASYNCAPI_SCHEMA="$HOME/asyncapi-3.0.0.json"
cmake --build --preset dev-asan
ctest --preset dev-asan
```

This was verified with a local copy of the validator's sources at the pinned commit, in the
`dev-asan` and `dev-tsan` trees; the download is not yet verified.

When the validator is an installed library instead (an imported target), the build takes it to be
built without AddressSanitizer. So, in a build whose `PRAENORA_SANITIZERS` holds `address` and
whose validator is installed, CTest runs only these three tests with that check off: it appends
`:detect_container_overflow=0` to the `ASAN_OPTIONS` it runs with, and every other test keeps the
check. Run on its own there, the executable needs the same option:

```bash
ASAN_OPTIONS=detect_container_overflow=0 \
    PRAENORA_TEST_ASYNCAPI_SCHEMA="$HOME/asyncapi-3.0.0.json" \
    ./build/dev-asan/bin/praenora_asyncapi_official_schema_tests
```

### The official AsyncAPI parser

The schema checks the documents' structure only. The official AsyncAPI parser,
[`@asyncapi/parser`](https://github.com/asyncapi/parser-js), also applies the rules of the AsyncAPI
specification that a JSON Schema cannot express. A build whose cache variable
`PRAENORA_ASYNCAPI_PARSER` holds the absolute path of a directory into which that parser is
installed adds two tests to the `protocol` label. Each runs
`tests/protocol/asyncapi_parser_check.cjs` with Node.js. The script first checks that the parser
is version 3.6.6, the version whose results it records, then:

- `asyncapi.parser.accepts_committed_documents`: the parser, with its default rules, parses every
  `spec/asyncapi/*.asyncapi.json` and reports for each exactly the diagnostics that the script
  records (`RECORDED_DIAGNOSTICS`), none of which is an error. A diagnostic more, or one fewer,
  fails the test.
- `asyncapi.parser.refuses_broken_copies`: the parser refuses copies of a committed document,
  each broken in one place, each with an error at that place whose code is the broken rule's name
  (`invalid-ref` for a reference that does not resolve). Six of the seven breaks
  are rules that the AsyncAPI 3.0.0 JSON Schema does not express: an operation's message must
  belong to the operation's channel; an operation's channel must be under the root `channels`, and
  a channel's servers under the root `servers`; a channel's key holds no `?`; a channel has no
  parameter that its address does not use; and every reference resolves. The seventh breaks the
  schema, which the parser applies too. So neither a parser that accepts anything nor a check of
  the JSON Schema alone passes this test, while the first test alone would pass on a parser that
  accepts anything and reports the recorded diagnostics.

The parser is given each document's text and reads no other file and no URL: the documents refer
only to themselves. Version 3.6.6 reports one diagnostic for each document, which the script
records: `asyncapi-latest-version` at `/asyncapi`, at the `info` level, saying that the latest
AsyncAPI version is 3.1.0. The documents are AsyncAPI 3.0.0
([ADR-0020](adr/ADR-0020-message-envelope-and-asyncapi.md)). The parser's rules at the `warning`
level, its recommended rules, apply to AsyncAPI 2 documents only.

Neither the build nor the tests download the parser, and nothing of it is committed. Install it
once, outside the source tree, at the version that the script records (`--ignore-scripts` keeps
npm from running the packages' install scripts), then configure with it:

```bash
npm install --prefix "$HOME/asyncapi-parser" --ignore-scripts --no-audit --no-fund \
    @asyncapi/parser@3.6.6
cmake --preset dev -DPRAENORA_ASYNCAPI_PARSER="$HOME/asyncapi-parser"
ctest --preset dev -R '^asyncapi\.parser\.'
```

The script also runs on its own, from the source tree:

```bash
node tests/protocol/asyncapi_parser_check.cjs accepts_committed_documents \
    "$HOME/asyncapi-parser" spec/asyncapi
```

The configuration looks for Node.js (`node` or `nodejs`) with CMake's `find_program()`, which
searches the `bin` directories of `CMAKE_PREFIX_PATH` before the `PATH`; the cache variable
`PRAENORA_NODE_EXECUTABLE` holds the one found, or names another (unset it,
`-UPRAENORA_NODE_EXECUTABLE`, to search again). CTest runs the script without the caller's
`NODE_OPTIONS` and `NODE_PATH`. CMake makes a relative value given without a type
(`-DPRAENORA_ASYNCAPI_PARSER=<dir>`) absolute against the current directory; a value that is then
not absolute, that does not name a directory holding `node_modules/@asyncapi/parser`, or that
holds a `;`, fails the configuration. Configuring with an empty value
(`-DPRAENORA_ASYNCAPI_PARSER=`) removes the two tests. They run Node.js, not code that this build
compiles, so they run alike in every tree, the sanitizer trees included.

With another version of the parser, both tests fail and name the version found. To move to
another version, install it, change `PARSER_VERSION` in the script, run the tests and record in
`RECORDED_DIAGNOSTICS` what the new version reports, once no error is left: an error in a
committed document is fixed in the generator, and the documents regenerated
([Golden files](#golden-files)). If `asyncapi.parser.refuses_broken_copies` then fails, check each
break's expected code and place (`BREAKS`) against the new version's rules, without dropping a
break. npm installs the parser's dependencies at their newest versions that the parser allows, so
another installation of 3.6.6 may run with newer ones; the tests print the versions of the parser,
of `@asyncapi/specs`, the package of the AsyncAPI JSON Schemas, and of `@stoplight/spectral-core`,
which runs the rules, that they ran with. The recorded results come from `@asyncapi/parser` 3.6.6
with `@asyncapi/specs` 6.11.1, whose AsyncAPI 3.0.0 schema is the file of the previous section
(the same SHA-256), and `@stoplight/spectral-core` 1.24.1, on Node.js 26.8.2, macOS arm64.

## Sanitizer builds

The `dev-asan` and `dev-tsan` presets ([developer-guide.md](developer-guide.md#sanitizer-builds))
run the same tests under AddressSanitizer with UndefinedBehaviorSanitizer, or ThreadSanitizer:

```bash
ctest --preset dev-asan
ctest --preset dev-tsan
```

- AddressSanitizer and ThreadSanitizer reports make the process exit with an error, which fails
  the test. So does the first UndefinedBehaviorSanitizer report (`runtime error: ...`): a build
  whose `PRAENORA_SANITIZERS` holds `undefined`, such as `dev-asan`, compiles with
  `-fno-sanitize-recover=undefined`, so the process ends with an error there instead of going on.
  The CLI, install, perf and provider-file drivers also fail a command whose output holds any
  sanitizer report, whatever its exit code.
- On macOS, AddressSanitizer test runs set `PRAENORA_DEBUG_KEEP_LIBRARIES=1`, so module libraries
  stay loaded: the sanitizer keeps an unloaded library's globals registered, which would trip false
  reports against a library loaded later at the same address. Unloading is tested by every other
  build, ThreadSanitizer included.
- A sanitizer sees only the code compiled with its flags, and the compiled third-party libraries
  are linked as installed, normally without them: yaml-cpp, spdlog and GoogleTest from the package
  manager. Errors inside them go unnoticed, and AddressSanitizer can report a false container
  overflow when such a library fills a container that instrumented code made; no false report from
  them has been seen. The header-only libraries (jsoncons, nlohmann/json) are compiled with the
  tree's flags in every tree.
- The module hosts that tests make crash on purpose run with the sanitizers' own signal handling
  off, so they end by the crash's signal ([Module hosts](#module-hosts)).
- `python.unittest` is registered but disabled: an instrumented extension cannot be loaded into an
  interpreter that is not instrumented. A run of the label alone therefore matches no test that
  runs, and the presets' "no tests" rule makes it fail with exit code 8:

```bash
ctest --preset dev-asan -L python
```

A full run of a sanitizer tree is unaffected and lists `python.unittest` as not run.

## Golden files

Two kinds of expected output are committed and compared byte for byte or within a tolerance:

| Files | Compared by | Refreshed by |
|---|---|---|
| `tests/fixtures/golden/example.estimate-predict/` (`predictions.jsonl`, `state.jsonl`) | `Example.OutputsMatchGolden`, within a tolerance | `PRAENORA_UPDATE_GOLDEN=1 ctest --preset dev -R '^Example\.OutputsMatchGolden$'` |
| `spec/asyncapi/*.asyncapi.json` | `GraphExport.CommittedAsyncApiMatchesGenerator` and `cli.graph.asyncapi_matches_committed`, byte for byte | `PRAENORA_UPDATE_GOLDEN=1 ctest --preset dev -R '^GraphExport\.CommittedAsyncApiMatchesGenerator$'` |

Refreshing never happens by itself: only `PRAENORA_UPDATE_GOLDEN=1` makes those tests write the
files instead of comparing them. Refresh after a deliberate change to the example modules, their
parameters or data, a workflow definition, a message type or the envelope schema, then review the
difference before keeping it:

```bash
PRAENORA_UPDATE_GOLDEN=1 ctest --preset dev -R '^Example\.OutputsMatchGolden$'
PRAENORA_UPDATE_GOLDEN=1 ctest --preset dev -R '^GraphExport\.CommittedAsyncApiMatchesGenerator$'
git diff --stat -- tests/fixtures/golden spec/asyncapi
```

Check refreshed AsyncAPI documents against the official AsyncAPI 3.0.0 schema and with the official
AsyncAPI parser too ([The official AsyncAPI schema](#the-official-asyncapi-schema),
[The official AsyncAPI parser](#the-official-asyncapi-parser)).

The golden envelopes hold sixteen zeros where an envelope id holds the first 16 hex digits of the
run fingerprint, because the fingerprint covers the module libraries' digests and so differs from
build to build. Before refreshing the example's outputs, check what the example promises: at tick 0
every estimate rests on observations, from tick 1 on estimates blend, and every value is finite and
positive.

## Triage

A failing test names its component, and the failure message names an error code, a JSON pointer or
`file:line`, and the step and tick where they matter. The CLI, install, perf, provider-file and CI
tooling drivers show a command that failed on one line, with each argument that is empty or holds
whitespace or a quote character shown in double quotes, and its outputs line by line as they were
printed, as CMake captures them, without the carriage return of each carriage return and line feed
and without NUL bytes ([`tests/support/run_command.cmake`](../tests/support/run_command.cmake) gives
the rules).
Start from the label:

| Failing label | Component | Likely owner |
|---|---|---|
| `unit` | The suite's component (`Csv` is `src/runtime/io/csv`, `Yaml` is `src/runtime/document`, `SdkAbi`, `Adapter` and `TestKit` are the SDK). | runtime |
| `protocol` | `spec/schemas/`, `spec/catalog/` or the graph code (`src/runtime/graph`, `src/runtime/api/graph_export`). A difference from the committed AsyncAPI documents needs a refresh or is a regression. For `asyncapi.parser.*`, the diagnostics that the test prints: a committed document that the parser refuses, or a parser of another version ([The official AsyncAPI parser](#the-official-asyncapi-parser)). | protocol |
| `contract` | The loader, discovery, instance or engine code, or a fixture module that changed. For `providers.run_twice.*`, the driver `tests/providers/run_twice.cmake` or the drivers' shared `tests/support/run_command.cmake`, or, after an intended change of its report, the expected texts in `tests/providers/check_run_twice.cmake`. | runtime |
| `negative` | Validation: a bad input was accepted, or refused with another code or location. | runtime |
| `lifecycle` | `src/runtime/instance` and `src/runtime/engine`: call order, deadlines, cancellation, teardown. | runtime |
| `isolation` | `src/runtime/isolation` (the channel, the runtime's side of a module host, the store of shared compiled models, the sandbox profiles, the module host's server), `src/module_host`, and the platform's child processes (`src/runtime/platform/posix/child_process_posix.cpp`). | runtime |
| `integration` | The public API (`src/runtime/api`), or the example modules when their outputs changed (golden files). | runtime, or the example modules |
| `cli` | `src/cli`, or the API behind the command; the test's name says which command. For `cli.driver.*`, the drivers' shared `tests/support/run_command.cmake`. | runtime |
| `python` | `src/python` and `python/praenora/runtime`; or the interpreter, when the import fails. | runtime, or environment |
| `install` | The install rules and the CMake package (`cmake/PraenoraInstall.cmake`, `cmake/PraenoraConfig.cmake.in`, `cmake/PraenoraModule.cmake`); the driver prints each step it checked. | runtime, or environment |
| `perf` | Only when the probe crashes or prints no report. | runtime |
| `docs` | The document or link named in the message: a new error code, schema or ADR without its documentation, or a moved file. | whoever changed it |
| `ci` | `cmake/praenora_fetch_runtime_deps.cmake`, or the pin file `cmake/PraenoraRuntimeDepsPin.cmake`; the report names the case that failed (`tests/ci/check_fetch_runtime_deps.cmake` describes each). | runtime |

When a run or a validation fails, its error code points further. By code range:

| Codes | Component | Likely owner |
|---|---|---|
| 100-103 (`PARSE_ERROR`, `SCHEMA_VIOLATION`, ...) | `document`: the file at the location given | user, or the module for a manifest |
| 200-204 (`CONFIG_INVALID`, `BINDING_INVALID`, `PARAMETERS_INVALID`, ...) | `config` and validation: the workflow configuration | user |
| 300-304 (`MANIFEST_INVALID`, `MODULE_NOT_FOUND`, `LIBRARY_DIGEST_MISMATCH`, ...) | `discovery` and `loader`: the manifest, the module search paths | module, or environment |
| 400-407 (`ABI_INCOMPATIBLE`, `PLATFORM_MISMATCH`, `CATALOG_ENTRY_NOT_FOUND`, ...) | `compat` and `catalog`; `graph` for `MESSAGE_TYPE_INCOMPATIBLE`; validation and the contract checks for `SERVICE_UNAVAILABLE` | module, environment or protocol |
| 500-502 (`LIBRARY_LOAD_FAILED`, `ENTRY_POINT_MISSING`, `API_TABLE_INVALID`) | `loader`: the module's library | module, or environment |
| 503 (`MODULE_HOST_UNAVAILABLE`) | `isolation`: the module host executable and its start | environment |
| 600-604 (`GRAPH_INVALID`, `GRAPH_CYCLE`, `PORT_*`) | `graph`: the workflow definition | protocol |
| 700-706 (`STEP_FAILED`, `OUTPUT_INVALID`, `STEP_TIMEOUT`, `MODULE_UNRESPONSIVE`, `MODULE_CRASHED`, ...) | `engine` and `instance`: the step named in the details | module, or the configuration's timeouts; `INPUT_MISSING` is the runtime's |
| 800-801 (`TARGET_DATA_INVALID`, `TARGET_DATA_UNAVAILABLE`) | `io` and validation: the data file and line, or the input's `path` | user data, or environment |
| 1-12 (`INVALID_ARGUMENT`, `IO_ERROR`, `INTERNAL`, ...) | Wherever the message says | caller, environment, or the runtime for `INTERNAL` and `UNKNOWN` |

The [error taxonomy](specs/error-taxonomy.md) gives each code's owner, exit code and retryable
flag, and its [triage table](specs/error-taxonomy.md#triage) says where to look for each one. A
module's own status is never the error's code: it is the `cause` of a `STEP_FAILED`, with the
module's message.

## The module acceptance gate

A module version is adopted only when:

1. the module's own test suite passes, in its own repository (the SDK's in-process test kit,
   `praenora/sdk/testing.hpp`, drives a module without loading it);
2. `praenora modules check <manifest>` passes against a build of the module: no check fails;
3. the module ships test vectors (`tests.vectors` in its manifest), so that the checks
   `invoke.result_schema`, `invoke.expected_outputs` and `invoke.determinism` run instead of being
   skipped.

The checks, the test vectors and the test kit are described in
[module-developer-guide.md](module-developer-guide.md#contract-checks). The example modules pass the
gate in every build that has them: the contract tests run the checks on them.

## Adding tests

- **GoogleTest.** Add the source to an executable in
  [`tests/CMakeLists.txt`](../tests/CMakeLists.txt), or a new executable with
  `praenora_add_gtest(<name> LABEL <label> [WHITEBOX] [SANDBOX_SKIP] SOURCES ...)`. With
  `WHITEBOX` it links the runtime's object library and may include internal headers
  (`praenora::detail`); without it, only the shared library and the public API. `NO_TESTS` in
  place of `LABEL` builds the executable but registers none of its tests, for tests that need what
  a build may lack. The caller then registers them with `gtest_discover_tests()` when they can run,
  as for [the official AsyncAPI schema](#the-official-asyncapi-schema): with the build's own test
  environment if they load module libraries, and with one value per property
  (`tests/CMakeLists.txt` says why). `SANDBOX_SKIP` is for an executable with tests of the
  restricted sandbox profile: CTest then also shows a test as skipped by the start of
  `kNestedSandboxSkip` ([Module hosts](#module-hosts)). Name tests `Suite.Case` in PascalCase, the
  suite naming the component (`Loader.LoadsModuleFromUnicodePath`); the case may end in
  `_WhenCondition`. Assertions should print the error code's name, the JSON pointer or
  `file:line`, and the step and tick where they matter.
- **Helpers.** `tests/support/` holds what tests share: scratch directories, JSON comparison with a
  tolerance, log capture, fixture paths. A test that takes a file's or a directory's permissions
  away gives them back through a `RestorePermissions` guard (`temp_dir.hpp`), declared after its
  `ScratchDir`; one in which a file is made immutable (macOS) removes it, and its directory,
  through a `RemoveAllEvenImmutable` guard (`sandbox_support.hpp`) declared before the file is
  made. Tests get the locations of the source tree, the fixtures, the test catalog and the fixture
  and example modules as compile definitions (`PRAENORA_TEST_SOURCE_DIR`,
  `PRAENORA_TEST_FIXTURES_DIR`, `PRAENORA_TEST_CATALOG_DIR`, `PRAENORA_TEST_MODULES_DIR`,
  `PRAENORA_EXAMPLE_MODULES_DIR`).
- **Fixture modules.** Add one with `praenora_add_fixture_module()` in
  [`tests/fixtures/modules/CMakeLists.txt`](../tests/fixtures/modules/CMakeLists.txt)
  ([developer-guide.md](developer-guide.md#adding-a-module-locally)); test capabilities and message
  types go into `tests/fixtures/catalog/`. Its manifest comes from `<name>/<name>.pmm.json.in` when
  that file exists, otherwise `praenora_add_module()` generates it; `REQUIRES_SERVICES` and
  `OPTIONAL_SERVICES` declare runtime services. The modules built from `fx-service/` use the test
  service `test.echo@0` of the fixture provider `test-echo` and report what `get_service()`
  returned.
- **Fixture providers.** Add one with `praenora_add_fixture_provider()` in
  [`tests/fixtures/providers/CMakeLists.txt`](../tests/fixtures/providers/CMakeLists.txt). A
  provider is loaded once per process and never unloaded, so a test that needs a provider's first
  load, or a copy of one under its name, uses a provider no other test loads.
  `make_provider_context()` (`tests/support/validation_fixtures.hpp`) builds a runtime context over
  fixture providers and no others: like every test context of that header, it leaves out the
  installation's provider directory, where a build tree builds the roadrunner and antimony
  providers and where, on Windows, a white-box test executable beside the runtime DLL would find
  them; `copy_fixture_provider()` and `copy_fixture_module()`
  (`tests/support/provider_fixtures.hpp`) copy one, manifest edited, into a scratch directory.
  `test-simulation` offers the simulation service for the toy model format `test-decay`
  ([specs/simulation-service.md](specs/simulation-service.md#the-test-provider-test-simulation)),
  and the fixture module `simulating-module` uses it in workflows and in its own test vectors.
  `test-conversion` offers the model conversion service for the toy formats `test-decay` and
  `test-decay-text`
  ([specs/model-conversion-service.md](specs/model-conversion-service.md#the-test-provider-test-conversion)),
  and the fixture module `converting-module` uses it, with `test-simulation` to simulate what it
  converted, in workflows (`Api.ConvertingModule*`) and in its own test vectors
  (`ModuleCheck.ConvertingModulePassesWithTheConversionService`).
- **Provider files.** `tests/providers/` tests a provider library as a file, for `test-simulation`,
  `test-conversion` and, when built, `roadrunner` and `antimony`: `providers.<provider>.exports`
  lists its exported symbols with `nm` (`check_exports.cmake`; not on Windows) and requires the
  entry point to be the only one, and `providers.<provider>.processes` runs a print tool twice, each
  time in a new process (`run_twice.cmake`), and the two outputs must be the same bytes. Each run's
  standard output is written to a file, `first.out` or `second.out` in
  `build/<preset>/tests/providers/processes/<provider>/`, and the two files are compared byte for
  byte: CMake's capture of an output drops the carriage return of each carriage return and line
  feed and every NUL byte, so two captured texts can be the same when the bytes differ. For the
  simulation providers the tool is `praenora_simulation_print`: it simulates a model of
  `tests/fixtures/models/`, runs a kernel and a plan's batch, and prints every double exactly. For
  the conversion providers it is `praenora_conversion_print`: it converts a source and the result
  back, `tests/fixtures/models/test-decay-text/two-states.txt` to `test-decay` for `test-conversion`
  and `tests/fixtures/models/antimony/decay.ant` to `sbml` for `antimony`, and prints both texts and
  reports verbatim. A run that exits with a nonzero code or prints a sanitizer report fails the
  test, and so does an output that shows no simulation and batch, or no conversion and conversion
  back. The report names the tool and the run, as in
  `praenora_simulation_print (second run): exit code 1, expected 0`, and shows that run's command,
  exit code and outputs; before those of the second run, it shows the first run's outputs. When
  the outputs differ, the report names the tool and the first line where they differ and shows it
  as each run printed it, as CMake captures it; it says which output is shorter when one is a
  prefix of the other, and names the longer one's next byte in hexadecimal; otherwise it names the
  byte of that line where they first differ, counted in the bytes as printed, and that byte of each
  output in hexadecimal, and says why the line looks the same in both when it does; the first run's
  outputs follow, and then the second run's command, exit code and outputs;
  `providers.run_twice.shows_first_difference` (`check_run_twice.cmake`) checks that report, also
  for two outputs that differ only by a carriage return before a line feed or by a NUL byte, the
  report on each failure of either run, and that two runs that printed the same bytes pass, with
  their count. It writes the outputs that its printer prints byte for byte, with
  `cmake -E echo_append` and `cmake -E cat` through `execute_process()` with `OUTPUT_FILE`, and
  checks each file's bytes: `file(WRITE)` and `file(APPEND)` write in text mode on Windows, a
  carriage return before each line feed.
- **Service checks.** The simulation service's checks
  ([`tests/contract/simulation_service_checks.hpp`](../tests/contract/simulation_service_checks.hpp))
  are a parameterized suite: a provider of the service is checked by a source file that instantiates
  `SimulationService` with its parameter sets (provider manifest, model format and text, ids, exact
  solution, tolerances), as `tests/contract/simulation_service_test.cpp` does for `test-simulation`.
  The model conversion service's checks
  ([`tests/contract/model_conversion_service_checks.hpp`](../tests/contract/model_conversion_service_checks.hpp))
  work the same way: `ModelConversionService` with a parameter set (provider manifest, engine
  object, conversions, exact translations, refusals, an engine failure, optionally a source with
  several modules and one that holds the provider's lock), as
  `tests/contract/model_conversion_service_test.cpp` does for `test-conversion` and
  `tests/contract/model_conversion_service_antimony_test.cpp` for `antimony`. Add the file to
  `praenora_contract_tests` in `tests/CMakeLists.txt`.
- **CLI tests.** Add one with `praenora_add_cli_test(cli.<command>.<case> CMD ... EXPECT_EXIT ...)`
  in [`tests/cli/CMakeLists.txt`](../tests/cli/CMakeLists.txt); the driver checks the exit code,
  regular expressions on both outputs, values in the JSON output and files left behind. The tests
  run in the caller's environment: one that must find no service provider there sets
  `ENVIRONMENT "PRAENORA_PROVIDER_PATH="`, an empty value, which counts as unset.
- **Python tests.** Add a `test_*.py` module of stdlib `unittest` tests to `tests/python/`
  (classes `<Topic>Test`, methods `test_<what>`); `tests/python/support.py` holds the environment
  and fixture paths they share. `python.unittest` discovers them.
- **Documents.** A new error code, schema file or ADR needs its documentation, or `docs.consistency`
  fails, naming what is missing.
- **Data.** Test data and golden files use LF line endings (`.gitattributes`), except the fixtures
  under `bom-crlf/` directories, which carry a byte order mark and CRLF on purpose.
