# ADR-0041: CI and releases against pinned dependency bundles

- Status: Proposed
- Date: 2026-11-21
- Supersedes: none
- Superseded by: none

## Context

Until now every build and test of this repository ran on a developer's machine, on macOS arm64
only: Linux x86-64 and Windows x86-64 have presets and portable code but have never been built, and
there were no release archives. The engines of the service providers (libRoadRunner with its LLVM,
libAntimony) come from no package manager ready to use (json-schema-validator did too, until
[ADR-0043](ADR-0043-json-schema-dialects-and-validator.md) replaced it with the header-only jsoncons,
which the `ci-*` presets fetch), so a build on
another machine first needs them built.

The repository praenora-runtime-deps builds them, with nlohmann/json, into one dependency bundle
per platform key (`linux-x64-release`, `macos-arm64-release`, `windows-x64-release`,
`windows-x64-debug`). Its workflow "Build" builds and checks every bundle on each pull request to
`main` and, run by hand on `main` with the input `publish`, drafts a release `v<version>` with the
archives, which an administrator publishes. Its `docs/outputs.md` ("From a release") says how the
runtime consumes a bundle: pin the release and, per platform, the archive's name and SHA-256, and
check a download against it.

The maintainer asked for the same kind of automation here: a build on every pull request to
`main`, and a drafted release with a version tag and archives from `main`, built against the
bundles of a pinned release. The change rules of AGENTS.md froze every version, the runtime
package's included, so a release could never carry a new version.

## Decision

- **The workflow.** [`.github/workflows/build.yml`](../../.github/workflows/build.yml), named
  "Build", mirrors praenora-runtime-deps' workflow: it runs on each pull request to `main` and by
  hand on any branch, with `permissions: contents: read` and one run per pull request or ref
  (`concurrency`). Its job `Prepare` reads `project(VERSION)` and the pin; one build job per
  platform key, named after the key, on the runner of the same key in praenora-runtime-deps'
  workflow (`ubuntu-24.04`, `macos-26`, `windows-2022`), fetches the bundle of its key, configures,
  builds and tests with the presets `ci-<key>` (both providers, the Python bindings, every test,
  warnings as errors), archives the installation with the target `package` (`PACKAGE` with Visual
  Studio), checks that the archive needs only the system's libraries and its own, and keeps the
  archive and its `.sha256` file as a workflow artifact for 7 days. With
  `publish`, accepted on `main` only and only while the tag `v<version>` does not exist, the job
  `Draft release` checks the archives' names and SHA-256 and creates the draft release
  `v<version>` of the built commit with every archive. An administrator publishes it, which
  creates the tag. Each action is pinned to a commit, with its version in a comment.
- **The pin.** [`cmake/PraenoraRuntimeDepsPin.cmake`](../../cmake/PraenoraRuntimeDepsPin.cmake)
  pins praenora-runtime-deps 0.1.1 and the names and SHA-256 of its four archives, and
  [`cmake/praenora_fetch_runtime_deps.cmake`](../../cmake/praenora_fetch_runtime_deps.cmake)
  downloads, checks and unpacks one of them, for the jobs and for developers. The pin is changed
  only after the maintainer confirms the change, together with the runner labels when that release
  of praenora-runtime-deps moved its jobs to other runner images
  ([releasing.md](../releasing.md#raising-the-deps-pin)).
- **The version exemption.** The runtime package's version, `project(VERSION)` in
  `CMakeLists.txt`, is raised when a release is made, by the maintainers' decision, following
  [releasing.md](../releasing.md#release-procedure); every other version (the SDK, PMP, the module
  ABI, the provider interface, the services, the document formats, the catalog entries and this
  repository's modules) stays frozen. The first release is v0.2.0, the current version, made
  without a raise. Decided by the maintainer on 2026-11-21.
- **Releases only from the workflow.** No release, `v*` tag or release asset is made by hand: a
  release is a draft of the workflow, published by an administrator.
- **A consistency test.** `ci.workflow_consistency` (label `ci`) checks, offline, that the
  workflow's matrix keys are the platforms of the pin, each with its `ci-<key>` presets, that each
  key runs on the runner label of its operating system and every job on a fixed label, and that
  every action is pinned to a commit with its version
  ([testing.md](../testing.md#labels)).

### Builder notes (Proposed)

- **A CMake fetch script** rather than `gh release download` and `shasum` in the workflow:
  `file(DOWNLOAD)` and `file(ARCHIVE_EXTRACT)` behave the same on the three platforms, where the
  shells differ (bash, and PowerShell on Windows); praenora-runtime-deps is public, so no token is
  needed; the repository's other drivers are CMake scripts already; a developer fetches the same
  bundle with the same command; and the test `ci.fetch_runtime_deps` runs the script offline on
  `file://` URLs. The pin is a CMake file that the script includes, rather than values in the
  workflow's YAML or a JSON file, which the workflow and Windows would each need a reader for. The
  script checks the SHA-256 itself rather than with `file(DOWNLOAD EXPECTED_HASH)`, so that its
  error gives both values and leaves no file, and tries a failed or stalled transfer again, up to
  3 attempts.
- **yaml-cpp, spdlog and GoogleTest are built from their pinned commits** in every job, with
  `PRAENORA_FETCH_DEPENDENCIES` and `CMAKE_DISABLE_FIND_PACKAGE_<name>` for each (`googletest`,
  FetchContent's content name, for GoogleTest), rather than taken from apt, Homebrew or vcpkg: the
  archives then link yaml-cpp and spdlog statically and need only the system's libraries, what a
  job builds does not depend on the packages of its runner image, and these jobs are the first to
  build the fetched dependencies. nlohmann/json comes from the bundle (and jsoncons is fetched, as the bundle does not hold it),
  which is first on `CMAKE_PREFIX_PATH`.
- **A step checks the archive's libraries** after the archive is made, since no test of the tree
  shows what an archive needs, and the smoke test of the release procedure, on a Mac with
  Homebrew, would pass with an archive that loads Homebrew's libraries. A bash step unpacks the
  archive and lists what each program and library of `bin/`, `lib/` and `libexec/` names, with
  `readelf -d`, `otool -L` or `dumpbin -dependents` (found with `vswhere`), and accepts only the
  system's libraries (glibc's, `libstdc++` and `libgcc_s` on Linux; `/usr/lib/*` and
  `/System/*` on macOS; the API sets, the Visual C++ runtime and the DLLs of
  `%SystemRoot%\System32` on Windows) and those the archive holds (on Windows only
  `praenora_runtime.dll`) ([releasing.md](../releasing.md#the-workflow)). Windows
  is checked too rather than left out: CMake searches the prefixes of the directories on `PATH`
  on every platform, and a runner image can have other software's libraries in some of them. The
  script fails, as it should, on a local macOS archive built with Homebrew's yaml-cpp and spdlog;
  its Linux and Windows parts ran only on hand-made archives; not yet verified on GitHub.
- **The runners follow praenora-runtime-deps.** A bundle holds static libraries built with the
  compiler of its runner image, and the runtime must be built with that compiler or a newer one of
  the same family; the same fixed labels give the same images, so the runtime's jobs use exactly
  the labels of praenora-runtime-deps' jobs, never `-latest`. The macOS bundle of 0.1.1 is named
  `appleclang21`, although praenora-runtime-deps' documentation expects AppleClang 17 on
  `macos-26`; the runtime's macOS job, on the same label, is expected to build with AppleClang 21.
- **Windows** uses the Visual Studio 2022 generator of the project's Windows presets, which finds
  MSVC itself, so the jobs need neither Ninja nor the developer environment that
  praenora-runtime-deps' jobs enter; the archive's target is `PACKAGE` there. No step shortens the
  checkout's path with `subst`, as praenora-runtime-deps' does for its object files: the longest
  path of a Windows job is estimated at 235 characters (a license file in CPack's staging
  directory) and the longest object file path at 172, below the limit of 259
  ([releasing.md](../releasing.md#the-workflow)); not yet verified.
- **Python** comes from `actions/setup-python` (3.12), whose `python-path` is passed as
  `Python3_EXECUTABLE`; the CI presets do not set it. The build step passes `--parallel`, so that
  MSBuild builds projects side by side (`/m`); Ninja builds in parallel without it.
- **`Prepare` reads the pin** (its version and repository, with `sed`) for the release notes,
  since the release job has no checkout. It tells `git ls-remote --exit-code` exiting with 2 (no
  such tag) from any other failure, which stops the run, rather than taking every failure for a
  missing tag. The checkout keeps no credentials, so `git ls-remote` needs a public repository,
  as this one is; a private one would need the job's token for the check, through `gh api`
  ([releasing.md](../releasing.md#the-workflow)).
- **The release notes** give the versions of the runtime, the module SDK, PMP and the module ABI,
  and each provider with its services and engine, from `praenora version --json` and
  `praenora providers list --json` of the Linux archive, run on the release job's runner, the image
  the archive was built on; the pinned release of praenora-runtime-deps; and the license
  paragraph: this repository's files are MIT, the runtime (yaml-cpp, spdlog with the fmt library
  it bundles, jsoncons, nlohmann/json) and the providers (libRoadRunner with LLVM,
  libSBML and libroadrunner-deps' other libraries; libAntimony) hold third-party code, linked
  statically or compiled in from its headers, that keeps its own licenses, whose files each
  archive holds in `share/praenora/licenses/`, and the terms of NLEQ1 and NLEQ2 are open
  (OQ-27). `praenora version` reports no version of the provider interface, so the notes give
  none.
- **`ci.workflow_consistency` reads the workflow as text**, line by line, since CMake reads no
  YAML; the workflow's header comment states the format it relies on (a line `- key: <key>`
  followed by `runner: <label>` for each matrix entry, `runs-on: <label>`, and
  `uses: <owner>/<name>@<commit> # v<version>`, each value on the line of its key), and the test
  refuses a key whose value is on a later line. It replaces `\`, `;`, `[` and `]` before it
  splits the text into lines, so that no line joins the next or splits into two.
- **The first run's warnings.** The first run of the workflow, on a pull request to `main`,
  passed the macOS job and failed the build of the Linux and Windows jobs on warnings that
  AppleClang does not give. Most were fixed in the code: a parameter that only the POSIX branch of
  a fixture used, a local variable that hid a member, the result of `write()` that a cast to
  `void` does not discard for GCC (glibc declares it `warn_unused_result`), and `getenv()` calls,
  which MSVC deprecates (one more is in a test support header that no job had reached). Those
  calls, and the three that the tests had marked before with
  `#pragma warning(suppress : 4996)`, which no MSVC build had compiled yet, now turn C4996 off
  with `push`, `disable` and `pop` around the call alone, which does not depend on what MSVC takes
  for the line after the pragma (here an `#endif`). Two are calls on the warnings themselves
  ([platforms.md](../platforms.md#common-to-every-platform)). GCC 13's `-Wdangling-reference` is
  turned off for C++ (`cmake/PraenoraWarnings.cmake`): it reports every reference bound to the
  result of a call that was given a temporary, whatever the reference refers to, and here these
  are a provider that `load_provider()` keeps loaded for the life of the process and the entries
  that test helpers look up by a string literal; rewriting each such call would have fixed
  nothing, since none of these references dangles. MSVC's C4702 (unreachable code) is turned off
  for the targets that compile libRoadRunner's headers, through the interface target
  `praenora_roadrunner_engine` (`cmake/PraenoraRoadrunner.cmake`): its `rr/Setting.h` returns after
  a `throw` in templates, and the code generator reports that where they are instantiated, out of
  reach of the warning level of external headers. A third is made ahead of the next run, since no
  job compiled the Python extension: the wrapper that SWIG generates is compiled with the
  compiler's default warnings and never as errors. It is not this project's code, and its overload
  dispatch redeclares variables in nested blocks (Clang's `-Wshadow` shows them), which MSVC's
  `/W4` would report as C4456; the facade it wraps is compiled with the project's warnings, as an
  object library of its own (`src/python/CMakeLists.txt`). Those builds stopped at their first
  failures (the Linux job after 113 of its 435 steps; the Windows jobs before the tool, the tests
  and the Python extension), so more of the code has yet to be compiled there; not yet verified on
  GitHub.
- **The second run's failures.** The second run built both Windows jobs, whose tests then failed 21
  of 988 tests, the same in each, and stopped the Linux job's build after 296 of its 435 steps, at a
  test program that used `std::int32_t` without `<cstdint>` (libstdc++ declares it there alone;
  glibc's `<stdint.h>`, which the program does include, declares only `::int32_t`). Every Windows
  failure was in the tests, none in the runtime. Most expected forward slashes where a path is
  lexically normal, which on Windows means backslashes, or turned a UTF-8 string into a path with
  the narrow constructor, which reads it in the ANSI code page; they now compare paths as paths, or
  text with the lexically normal path the runtime reports, or the list of a directory's files by
  their `'/'`-separated names, and the CLI tests accept either separator. Others: a Windows path
  written into a double-quoted YAML scalar, where `\U` starts an escape (now single quotes); two
  unit tests that validate steps in module hosts, which Windows refuses in this version (now
  compiled only off Windows, like the other tests of process isolation); a reader of the status file
  that counted a refused open, while the writer replaced the file, as a torn read (on Windows it now
  counts only what it read); a scratch file named `nul`, a device in every directory on Windows; and
  `printf`'s `%a`, which the Microsoft C runtime writes with every hexadecimal digit of the
  significand (`0x0.0000000000000p+0` for zero), where `run_twice.cmake` expected `0x0p+0`. Two had
  causes of their own. `$<TARGET_PROPERTY>` leaves a `$<CONFIG>` in a property of the project's own
  as it is, so the manifest path that `PRAENORA_PROVIDER_MANIFEST` gives a multi-config generator
  reached the tests unexpanded: the `processes` tests of the fixture providers found no manifest,
  and those of the roadrunner and antimony providers, and the tests compiled with their manifests'
  paths, found them only because `bin/$<CONFIG>/..` names `bin` wherever `..` is resolved by its
  text. Every such use is now wrapped in `$<GENEX_EVAL>`. And a build tree builds its providers
  where its runtime looks for an installation's, beside the runtime library; on Windows the
  white-box test executables, which hold the runtime's objects, sit in the runtime DLL's directory,
  so they found the roadrunner and antimony providers there, which the tests of a missing service
  and of where discovery found each manifest did not expect. The test helpers' provider search paths
  (`tests/support/validation_fixtures.hpp`), and that test's, leave out the installation's
  directory, so that a white-box test sees only the providers it names. Not yet verified on GitHub:
  the Linux job has yet to build the tool, both providers, the Python extension and the tests, and
  only the macOS job has made its archive.
- **The third run's failures.** The third run stopped the Linux job's build after 356 of its 435
  steps, at a unit test that compared a `std::optional` left empty by a conditional expression:
  GCC 13 at `-O3` reports the value of such an optional as maybe uninitialized, and the test now
  makes two comparisons instead. The rest of the Linux job was then run under Docker, in the image
  `ubuntu:24.04` for x86-64 (GCC 13.3, the pinned bundle, the CI preset, as an unprivileged user),
  which found what the job had yet to reach. Two more test warnings: loop variables of type
  `const std::string&` over lists of string literals (`-Wrange-loop-construct`; now `const char*`),
  and results of `json::parse()` cast to `void`, which GCC still reports (nlohmann declares it
  `warn_unused_result`; the test now counts the documents that `json::accept()` refuses). The tool
  did not link: the version script of the runtime library quoted its patterns for the type
  information and vtables (`"typeinfo for praenora::*"`), and a version script matches a quoted
  name literally, so no type information was exported and `catch (const praenora::Error&)` in the
  tool found no `typeinfo for praenora::Error`; the script now names them by their mangled names,
  `_ZTIN8praenora*`, `_ZTSN8praenora*` and `_ZTVN8praenora*`, as the macOS patterns do
  ([ADR-0023](ADR-0023-public-cpp-api.md)). Two tests failed. The channel to a module host grew a
  payload's buffer with `reserve()`, which in libstdc++ takes at least twice the old capacity, so a
  3 MiB payload took 4 MiB, past the announced length that
  [process-isolation.md](../specs/process-isolation.md) bounds it to; the buffer now grows into a
  new string reserved to the capacity wanted. And a test of a load failure wrote a text file of 43
  bytes as the library, of which glibc says only "file too short"; it is now longer than an ELF
  header, so glibc says "invalid ELF header", as the test expects. After these fixes, the build,
  one complete run of `ctest`, the archive and the check of its libraries passed under Docker, but
  for one test, which expects `posix_spawn()` to report that an executable does not exist. That
  failure is attributed to the emulation of x86-64 there (Rosetta): glibc's `posix_spawn()` gave a
  child that exited with 127, while the same glibc 2.39 on arm64, not emulated, reports `ENOENT`;
  not yet verified on native x86-64. Under that emulation `ctest` also hung at times on a child
  that had exited, until `ctest` was stopped and continued, which was done every 15 seconds during
  the complete run. In the third run the macOS job passed, and both Windows jobs built and then
  failed one test each, `providers.run_twice.shows_first_difference`, at its case 3, where the
  longer output continues with a line feed and the report named a carriage return instead (0x0d).
  CMake's `file(WRITE)` and `file(APPEND)` write in text mode on Windows (CMake 3.31.6 opens their
  stream without `std::ios::binary`), so the outputs that `check_run_twice.cmake` wrote for its
  printer held a carriage return before each line feed. It now writes them byte for byte, with
  `cmake -E echo_append` and `cmake -E cat` through `execute_process()` with `OUTPUT_FILE`, which
  takes what a program prints as it is, and checks each file's bytes against its text's. Not yet
  verified on GitHub: the fix has not yet run on Windows on any machine, and since the script stops
  at its first failure, its cases 4 to 14 have never run there. And every `execute_process()` of
  the CMake code now gives `ENCODING UTF-8`: the scripts require CMake 3.24, so the policy CMP0176
  (CMake 3.31) is not set, and CMake on Windows then decodes a program's output in the console's
  code page, or in the ANSI code page without a console. On the runners it evidently left UTF-8 as
  it was, since cases 1 and 2 passed with क in their outputs, but on a machine with another code
  page it would garble the tests' non-ASCII text.
- **The fourth run's failure.** In the fourth run the Linux, macOS and `windows-x64-release` jobs
  passed: on Linux every one of 1146 tests, among them the test of `posix_spawn()` that failed
  under Docker, which bears out the third run's attribution of that failure to the emulation; on
  Windows `providers.run_twice.shows_first_difference` with all its cases. The `windows-x64-debug`
  job failed one test, `StatusFile.AtomicReplaceAndParentCreation`, which had passed in that job in
  the third run: the last of the hundred values that the test has the status file's writer write
  never reached the file. On Windows a file that another handle holds open without
  `FILE_SHARE_DELETE`, as the Microsoft C++ library's `std::ifstream` opens it, cannot be
  replaced; `write_file_atomically()` tries the replacement ten times, at least 10 ms apart, then
  fails the write, which the writer reports and does not repeat until it has its next reason to
  write. The test's reader thread kept opening the file, and so did the test's wait for the last
  value, during the attempts of the last write; in the Debug build, which reads and parses
  the 64 KiB document slowly, every attempt evidently found the file open (the test gave the writer
  no error handler, so the log does not show the refusals). Refusing that one write on macOS gives
  the same failure. The writer does what
  [observability.md](../specs/observability.md#status-file) says (a failed write is reported, and
  the writer keeps going); the test assumed `rename()`, which replaces a file that a reader holds
  open. It now waits for the writer to take the last value without reading the file, then for the
  reader thread to read the last document, asking for a write again at each poll, checks the last
  value in what the destructor wrote, after the reader has stopped, counts the replacements that
  the writer reported refused (none may be refused off Windows) and expects no other error. Not yet
  verified on GitHub.

## Alternatives considered

- **The pin in the workflow, with `gh release download` and `shasum`**, as praenora-runtime-deps'
  documentation shows: simple, but the shell commands differ on Windows, the pin lives in YAML
  only, and developers cannot reuse it.
- **The pin in a JSON file read with `jq`**: the workflow could read it, but Windows and
  developers would need a second reader.
- **yaml-cpp, spdlog and GoogleTest from the runners' package managers**: the archives would load
  shared libraries that they do not hold, from places that differ between images.
- **`-latest` runner labels**: GitHub moves them to newer images, whose compilers can be newer or
  of another toolset than the bundles', without a change in this repository.
- **A release on each push to `main`, or a workflow started by pushing a tag**: releases would not
  wait for the maintainers' decision, and the tag would exist before the archives were checked;
  publishing a draft creates the tag, which the ruleset `v-tag` and immutable releases protect.
- **Ninja on Windows, in the Visual Studio developer environment**, as praenora-runtime-deps does:
  the project's Windows presets use the Visual Studio generator, and the CI presets inherit them.
- **A second workflow for releases**: the archives of a release would not be those that the
  pull request's jobs built and tested with the same steps.

## Consequences

- Every pull request to `main` builds and tests the runtime on four platform keys, with both
  providers, and reports the checks `Prepare`, `linux-x64-release`, `macos-arm64-release`,
  `windows-x64-release` and `windows-x64-debug`, which the ruleset on `main` can require.
- Linux and Windows get their first builds; the first runs are expected to need fixes. Until a run
  passes, they stay not yet verified ([platforms.md](../platforms.md)).
- Releases carry one archive per platform key with its `.sha256` file; wheels, manylinux and
  code signing stay open (OQ-13).
- A job needs the network: it downloads the bundle and the sources of yaml-cpp, spdlog and
  GoogleTest. The CI presets opt in to this; a default build and the tests still use no network.
- The version freeze has one exemption, the runtime package's version for a release
  ([specs/compatibility-policy.md](../specs/compatibility-policy.md#changing-a-version)).
- Raising the pin is a deliberate change, confirmed by the maintainer, in which the pin, the
  presets and the workflow's matrix and runners move together, and `ci.workflow_consistency` checks
  that they agree.
- The license questions of distributing the providers stay open, and the release procedure checks
  them before each release is published (OQ-27, OQ-29).

## References

- [releasing.md](../releasing.md): the workflow, the dependency bundles, raising the pin, the
  release procedure.
- [developer-guide.md](../developer-guide.md#using-a-dependency-bundle): using a dependency
  bundle; [CI presets](../developer-guide.md#ci-presets);
  [archiving an installation](../developer-guide.md#archiving-an-installation).
- [ADR-0017](ADR-0017-dependency-management.md): dependency management;
  [ADR-0040](ADR-0040-mit-license.md): the MIT license and the license files of installations.
- [open-questions.md](../open-questions.md): OQ-13, OQ-27, OQ-29.
- praenora-runtime-deps: its `.github/workflows/build.yml`, its `docs/releasing.md` ("Changing a
  runner image") and its `docs/outputs.md` ("From a release").
