# WebAssembly: feasibility design

- **Status:** Feasibility design, informative. Nothing in this document is implemented. The
  interfaces, options and presets sketched here are drafts: none is part of ABI v0, PMP or PMM, and
  adopting any of them is a versioned change with its own ADR
  ([compatibility-policy.md](../specs/compatibility-policy.md)).
- **Decision record:** [ADR-0025](../adr/ADR-0025-webassembly-strategy.md) (Proposed) and the open
  question OQ-17 in [open-questions.md](../open-questions.md).
- **Sources:** upstream facts were checked on 2026-11-11. They are cited as [W1] to [W36] and listed
  under Sources at the end. A claim that could not be checked is marked "(unverified)".

## Question and recommendation

**Question.** WebAssembly can mean two different things for Praenora. In Model A the runtime itself
is compiled to WebAssembly with Emscripten and runs in a browser or under Node, with its modules
either statically registered or loaded as Emscripten side modules. In Model B the native runtime
embeds a WebAssembly engine, such as Wasmtime or WAMR, and runs modules that are WebAssembly
binaries. What would each take, measured against the MVP's interfaces, and which should come first?

**Recommendation.**

- Keep native modules on ABI v0 as the primary model. WebAssembly modules share the protocol (the
  PMP documents and lifecycle) but not the binary interface: memory, threads and preemption differ
  too much for one ABI ([ADR-0025](../adr/ADR-0025-webassembly-strategy.md)).
- If OQ-17 is settled in favor of running modules as WebAssembly, build **Model B first**. Each module
  instance gets its own engine store and linear memory; documents are copied in through an exported
  allocator and out through an imported sink function; deadlines become hard stops through epoch
  interruption. It gives memory isolation between modules and the host, which bears on OQ-5 and
  OQ-19, and native runtime services such as libRoadRunner stay usable. Start with Wasmtime; keep
  WAMR as the alternative where a Rust toolchain or binary size is a constraint.
- Build **Model A** only for a confirmed browser or Node deployment. Start with statically
  registered modules and an inline executor (no threads); add pthreads or side modules only when a
  use needs them. Model A cannot offer the libRoadRunner simulation service, whose LLVM JIT has no
  WebAssembly path ([libroadrunner.md](libroadrunner.md)); the libAntimony conversion service can be
  compiled in ([libantimony.md](libantimony.md)).
- Before either model is built, add two internal seams that change no ABI or format: an executor
  interface under `ModuleInstance` (a worker thread, or inline), and a way to build a `LoadedModule`
  from a v0 table that does not come from `dlopen`.

## Options compared

| Option | What runs where | Isolation of modules | Threads | Stopping a stuck module | Toolchain | Runtime services |
|---|---|---|---|---|---|---|
| Native only (today) | Runtime and modules native; modules loaded with `dlopen` or `LoadLibraryExW` | None: modules share the host process ([threading-and-ownership.md](../specs/threading-and-ownership.md)) | One worker per instance | Cooperative; an unresponsive instance is abandoned and its library pinned | C++17 compilers on three platforms | Native services |
| Model A, static registration | One WebAssembly binary holding runtime and modules, with Emscripten's JavaScript loader, in a browser or Node | None: one linear memory | None (inline executor), or pthreads on Web Workers [W1] | Without threads, nothing stops a call that never returns | Emscripten | Conversion yes, simulation no |
| Model A, side modules | Runtime as the main module, modules as side modules opened with `dlopen` from the virtual file system [W3] | None | As above; dynamic linking with pthreads is still experimental [W3] | As above | Emscripten | As above |
| Model B, Wasmtime | Native runtime; modules are `.wasm` files run by the engine | Each instance in its own sandbox and linear memory [W11] | Engine calls on the instance's worker thread | An epoch deadline traps the guest [W13] | C++ compiler plus the Wasmtime C API (prebuilt, or built with Rust) [W21] | Native services, later through imports |
| Model B, WAMR | As above, with WAMR | As above | As above | `wasm_runtime_terminate` from another thread; instruction limits in the interpreters [W26][W25] | C compiler and CMake | As above |

## The MVP's seams today

ADR-0025 asks the MVP to keep three seams. Their state in the code:

| Seam | State in the MVP | What WebAssembly needs |
|---|---|---|
| A loader separate from the engine | The loader is its own component ([`loaded_module.hpp`](../../src/runtime/loader/loaded_module.hpp)) over a platform library wrapper ([`shared_library.hpp`](../../src/runtime/loader/shared_library.hpp)). The engine, validation with loading and the contract checks call `LoadedModule::load(manifest)`; afterwards they use only its accessors: the copied v0 table (`api()`), the manifest, the `describe()` document, the library digest and `pin()`. | `LoadedModule` can only be built from a `SharedLibrary`. Both models need a second source of a v0 table: a registered getter (Model A) or a host-side adapter (Model B). |
| An inline executor when threads are unavailable | Not implemented. Every `ModuleInstance` starts an `InstanceWorker` thread ([`worker.hpp`](../../src/runtime/instance/worker.hpp)), and its deadline logic waits for each call from the controlling thread ([`module_instance.hpp`](../../src/runtime/instance/module_instance.hpp)). | An executor interface with two implementations: the worker thread of today, and an inline executor that runs each call on the calling thread. |
| No file I/O in the engine's core | Holds for outputs: without an output directory the engine writes nothing and keeps the run record in memory ([`engine.hpp`](../../src/runtime/engine/engine.hpp)); schemas and the catalog are embedded in the runtime. Configuration, manifests and input files are read from paths: `Runtime::validate()` and `Runtime::open()` take a configuration path. | Model A writes those files into Emscripten's in-memory file system before `open()`, or the API gains an overload that takes the configuration as text. |

A fourth seam already exists. [`module.hpp`](../../include/praenora/sdk/module.hpp) offers
`PRAENORA_SDK_DEFINE_MODULE_GETTER`, which defines a module's table getter without exporting it, so
that several modules can live in one executable; the in-process test kit
([`testing.hpp`](../../include/praenora/sdk/testing.hpp)) relies on it. Model A's static registration
needs exactly that: every module built with `PRAENORA_SDK_DEFINE_MODULE` exports the same symbol,
`praenora_module_get_api`, so two of them cannot be linked into one binary.

## Model A: the runtime compiled with Emscripten

### Shape

- The runtime's OBJECT library, `praenora_runtime_objects`, is linked straight into an Emscripten
  executable together with the modules, as the white-box test executables link it today. The SHARED
  `praenora_runtime` target is not built: Emscripten "focuses on compiling an entire project into a
  single Wasm file" and recommends avoiding dynamic linking in most cases [W2].
- An embedder (a page, a Node program, or a small C++ `main`) constructs a `Runtime`, puts the
  configuration, manifests and data into the file system, opens the workflow and runs ticks.
- In a browser there is no output directory: the default file system is in memory and lost on
  reload, and browsers offer no synchronous API for persistent storage [W5]. Results come from
  `latest_outputs_json()` and `run_record_json()`. Under Node, NODEFS and NODERAWFS reach the real
  file system [W5], so the CLI could run as a Node program.

### Registering modules

Static registration, recommended first:

```c
/* SKETCH for Model A, not part of ABI v0. Declared by the runtime with C linkage. Makes a module
 * linked into the same binary loadable: a manifest whose library names it is loaded by calling
 * get_api(0) instead of opening a file. Call before constructing a Runtime; not thread-safe.
 * Returns PRAENORA_STATUS_INVALID_ARGUMENT for a malformed name or a NULL getter, and
 * PRAENORA_STATUS_INVALID_STATE when the name is already registered. */
praenora_status praenora_register_static_module(const char* name, size_t name_size,
                                                praenora_module_get_api_fn get_api);
```

- Each module defines its getter with `PRAENORA_SDK_DEFINE_MODULE_GETTER(MyModule, my_module_get_api)`
  and the application registers it under the module's name.
- The manifest stays the source of the module's identity, capabilities and parameter schema, and
  `describe()` is still checked against it. Its `library` object needs a form without a file, such
  as `{"static": "example-estimator"}`, and `platform` needs a WebAssembly value: today
  `library.path` is required and `platform.os` and `platform.arch` allow only native values
  ([`module-manifest.0.1.schema.json`](../../spec/schemas/module-manifest.0.1.schema.json)). Both are
  PMM changes.
- There is no library file to hash. The run fingerprint would record, for every statically
  registered module, the SHA-256 of the whole WebAssembly binary, which contains its code
  ([reproducibility.md](../specs/reproducibility.md#run-fingerprint)).

Side modules, when modules must be added without relinking:

- The runtime is the main module (`-sMAIN_MODULE`) and each module a side module (`-sSIDE_MODULE` or
  `-shared`); a project has exactly one main module [W3].
- `dlopen` at run time needs the side module in the virtual file system. `emscripten_dlopen` loads
  asynchronously, and Chromium does not compile modules over 8 MB synchronously on the main thread
  [W3].
- A main module disables dead code elimination by default (`-sMAIN_MODULE=2` restores it, with
  explicit exports); dynamic linking "can add overhead, so for best performance static linking
  should still be preferred"; and dynamic linking with pthreads is still experimental [W3]. The
  page notes that it is being refreshed [W3].

### Threads, or none

With pthreads:

- Code is compiled and linked with `-pthread` [W1].
- Browsers allow `SharedArrayBuffer`, and a shared `WebAssembly.Memory`, only in a secure context that
  is cross-origin isolated [W7]. The page must be served with
  `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` [W8];
  without those headers pthreads code does not work in a deployment [W1].
- `Atomics.wait` does not work on the main browser thread, so Emscripten busy-waits there, which can
  make the tab unresponsive; `PROXY_TO_PTHREAD` moves `main()` to a pthread and is recommended [W1].
- Starting a thread is asynchronous: it runs only after control returns to the event loop, unless a
  pool was created in advance (`PTHREAD_POOL_SIZE`) [W1]. The runtime starts one worker per instance
  (and a status-file writer when configured) and then waits for the worker, so the pool must be at
  least that large or the run deadlocks. That is an inference from [W1], to be confirmed by the spike.
- Pthreads with memory growth (`ALLOW_MEMORY_GROWTH`) are "especially tricky" [W1].
- Node.js shares `SharedArrayBuffer` instances between worker threads, and its documentation states
  no cross-origin requirement [W10]. Whether Emscripten's pthreads need further settings under Node
  is unverified.

Without pthreads, the inline executor:

- `create` through `destroy` run on the calling thread, in order, as one thread would run them
  today.
- Deadlines still work for a module that polls: `cancel_requested()` compares the steady clock with
  the call's deadline each time it is called, instead of reading a flag that the controlling thread
  sets. A call that returns after its deadline is still a timeout.
- A call that never returns cannot be abandoned, so `MODULE_UNRESPONSIVE` cannot happen: the run
  hangs. In a browser the runtime therefore runs in a dedicated Web Worker, so that the page stays
  responsive and can terminate the worker.
- The status-file writer and the CLI's signal watcher are not started.
- Whether `std::thread` fails cleanly in an Emscripten build without pthreads is unverified; the
  inline executor never creates one.

### Exceptions, memory and time

- The runtime reports errors internally with C++ exceptions (`praenora::Error`). Emscripten disables
  exception catching by default; `-fexceptions` works on every engine at "relatively high overhead",
  and `-fwasm-exceptions` uses WebAssembly's own exception handling, supported in several major
  browsers but not yet in all engines [W4]. Model A needs one of them at compile and link time.
- A 32-bit WebAssembly memory addresses at most 4 GiB, 65,536 pages of 64 KiB [W9]. The runtime's
  per-sink limit of 256 MiB ([module-abi.md](../specs/module-abi.md#sinks)) fits, but large documents
  rely on memory growth.
- Timers are coarser unless the page is cross-origin isolated [W8]. That affects measured durations
  and deadlines, never logical time ([time-cancellation-timeouts.md](../specs/time-cancellation-timeouts.md)).

### Runtime services

- `praenora.simulation@0` is not available: libRoadRunner generates native code with LLVM [W35],
  and WebAssembly code is immutable and not observable at run time, with call targets fixed at load
  [W11], so a JIT inside the module cannot run what it generates. `get_service` returns NULL, and
  modules must handle that ([libroadrunner.md](libroadrunner.md)).
- `praenora.model-conversion@0` can be compiled in: libAntimony, libSBML and expat have been built
  with Emscripten into one module [W34] ([libantimony.md](libantimony.md)).

## Model B: WebAssembly modules in the native runtime

### Shape

- A WebAssembly module is a `.wasm` file named by a manifest, as a native library is. The loader
  reads it, checks its SHA-256 as for a native library, compiles it once per manifest, and presents
  it through a **host-side v0 table**: a `praenora_module_api_v0` whose functions are host functions
  that drive the engine. The engine and `ModuleInstance` stay as they are: they call a v0 table on
  the instance's worker thread and apply the same deadlines.
- `create` instantiates the module: one engine store and one linear memory per Praenora instance,
  so instances never share memory and behave as `features.concurrency: "instance"` by construction.
  `destroy` drops the store.
- The loader's checks map one to one:

| Native check ([module-abi.md](../specs/module-abi.md#loading-and-table-acceptance)) | Model B |
|---|---|
| 1. File exists; pinned SHA-256 matches | Same, on the `.wasm` file. |
| 2. The library loads | The engine compiles the binary; its error text goes into `LIBRARY_LOAD_FAILED`. |
| 3. Entry point exported | The required exports below exist with their signatures, else `ENTRY_POINT_MISSING`. |
| 4 to 7. Table and required functions | Always satisfied by the host-side table. |
| 8 and 9. `describe()` and identity | `pmp_describe` runs on a short-lived instance, or the identity is read from a custom section so that no guest code runs while loading; then the same checks. |

### Guest interface (sketch)

All integers are `i32`. A status is a `praenora_status`. Pointers are offsets into the guest's
linear memory.

| Kind | Name | Signature | Mirrors |
|---|---|---|---|
| export | `memory` | The module's linear memory. | |
| export | `pmp_alloc` | `(size) -> offset`; 0 on failure. | |
| export | `pmp_free` | `(offset, size)` | |
| export | `pmp_describe` | `(out_sink) -> status` | `describe` |
| export | `pmp_create` | `(doc, doc_size, error_sink) -> status` | `create` |
| export | `pmp_initialize` | `(doc, doc_size, error_sink) -> status` | `initialize` |
| export | `pmp_invoke` | `(doc, doc_size, result_sink, error_sink) -> status` | `invoke` |
| export | `pmp_finalize` | `(error_sink) -> status` | `finalize` |
| export | `pmp_destroy` | `()` | `destroy` |
| import | `praenora.sink_write` | `(sink, data, size) -> status` | `praenora_sink.write` |
| import | `praenora.log` | `(level, message, size)` | `log` |
| import | `praenora.log_enabled` | `(level) -> int` | `log_enabled` |
| import | `praenora.cancel_requested` | `() -> int` | `cancel_requested` |

- **Inputs.** For each call the host asks `pmp_alloc` for room, copies the document into linear
  memory, calls the export, and calls `pmp_free` when it returns. The document is borrowed for the
  call, as in [`abi.h`](../../include/praenora/sdk/abi.h).
- **Outputs.** Sinks are small integers that the host assigns per call. `sink_write` checks that the
  range lies inside linear memory and copies the bytes out before it returns, so the guest keeps
  ownership and the sink limits apply unchanged
  ([`output_sink.hpp`](../../src/runtime/loader/output_sink.hpp)).
- **Memory growth.** WAMR's API documentation warns that native addresses into a module's memory can
  be invalidated by memory growth [W26]; the host reads the memory's base address again after every
  guest call. Wasmtime's `wasmtime_memory_data()` returns the base pointer and
  `wasmtime_memory_data_size()` the length [W17]; that its pointer moves on growth is unverified, and
  the host treats it the same way.
- **No WASI by default.** No file system, clock, random or network import is offered: randomness
  comes from the seed in the initialize document, and time from the invocation document.
- **No services yet.** A native service table cannot be handed to a guest. Services would become
  imports of their own, such as `praenora.simulation.simulate(...)`, in a later version.

### Deadlines and preemption

- The deadlines of today stay. At the deadline `cancel_requested()` turns nonzero, and a guest that
  polls returns `PRAENORA_STATUS_CANCELLED`.
- **Hard stop with epochs (Wasmtime).** Epoch interruption has "relatively low overhead", measured at
  about a 10% slowdown, and is not deterministic because it follows wall time [W13]. The engine's
  epoch is incremented by `wasmtime_engine_increment_epoch()`, which is safe from any thread and
  async-signal-safe [W16]; each store gets a deadline in epochs with
  `wasmtime_context_set_epoch_deadline()` [W15]. Design: one ticker thread per runtime increments the
  epoch every 10 ms; each call sets its store's deadline to the call's deadline plus half the
  cancellation grace. A guest that ignores cancellation traps then, and the call returns
  `PRAENORA_STATUS_TIMEOUT` within the grace: `STEP_TIMEOUT`, never `MODULE_UNRESPONSIVE`. A trapped
  instance is not called again, and teardown drops its store.
- **Fuel (Wasmtime).** Fuel is deterministic: the same program with the same fuel stops at the same
  place, at a higher cost than epochs [W13]; it is consumed per executed instruction, and execution
  traps at zero [W14]. It suits an optional, deterministic per-call budget, not the deadline.
- **WAMR.** `wasm_runtime_terminate()` makes an instance fail as if it trapped and may be called
  from another thread [W26]; `wasm_runtime_set_instruction_count_limit()` bounds a call in the
  classic and fast interpreters only, not in AOT or JIT modes [W25][W26].
- A guest can still hang inside a host import that blocks; the imports above never block.

### Engines

| | Wasmtime | WAMR |
|---|---|---|
| Embedding API | C API: `wasm.h` from the WebAssembly C API project plus `wasmtime.h` [W21]; the C API is Tier 1, the C++ header Tier 2 [W19] | `wasm_export.h`, and the WebAssembly C API [W24] |
| Obtaining it | Prebuilt archives whose names end in `-c-api` on the release page, or `cargo build --release -p wasmtime-c-api` with a Rust compiler; CMake `add_subdirectory` support with a `BUILD_SHARED_LIBS` option [W21] | Built from source with CMake; the running modes are chosen at build time [W25] |
| Platforms | Tier 1: x86-64 Linux, macOS and Windows (MSVC); Tier 2: arm64 Linux and arm64 macOS [W19] | Linux, Windows, macOS, Android and several embedded systems; x86-64, AArch64 and others [W24] |
| Execution | Cranelift and Winch compilers (Tier 1); the Pulley interpreter is Tier 2 [W19] | Classic and fast interpreters, AOT, Fast JIT (x86-64 only) and LLVM JIT [W24][W25] |
| Interruption | Epochs and fuel [W13][W14] | Termination from another thread; instruction limits in the interpreters [W25][W26] |
| Determinism controls | NaN canonicalization, deterministic relaxed SIMD, fuel [W14][W18] | Not checked (unverified) |
| Component model | Tier 1, with its async extension [W19] | Not mentioned on the repository page [W24] |
| Releases | A new major version every month, supported for two months; every twelfth release is a long-term release supported for 24 months [W20]. Latest stable: 50.0.1 of 2026-10-29 [W22] | Latest: 2.4.5 of 2026-06-29, which fixes three CVEs; 2.4.4 fixed two [W27] |
| License | Apache-2.0 as shown on the repository page [W36] | "the same license as LLVM": Apache 2.0 with the LLVM exception [W24] |

Wasmtime's arm64 macOS support is Tier 2 [W19], and that is the platform this repository is verified
on; the spike checks it first. Wasmtime publishes security advisories, at least five of them on
2026-10-29 [W23], so an embedding must follow its releases or a long-term release.

### Manifest and discovery

- `library.path` names the `.wasm` file. A field such as `library.format: "wasm"` tells the loader
  which path to take, and `platform` needs a value that matches every host, since a WebAssembly
  binary is not tied to an operating system
  ([`module-manifest.0.1.schema.json`](../../spec/schemas/module-manifest.0.1.schema.json)). Discovery's
  rule that manifests for other platforms never shadow host matches ([manifest.md](../specs/manifest.md))
  then needs a precedence between a native and a WebAssembly build of one module. These are PMM
  changes.
- `features.concurrency` has no effect: WebAssembly instances never share state.

### The component model, later

- A WIT world for PMP would export `describe`, `create`, `initialize`, `invoke`, `finalize` and
  `destroy` with `string` documents. The canonical ABI passes such values through linear memory
  [W29], obtains guest memory through the component's `realloc` canonical option, and uses UTF-8
  strings by default [W30]; generated bindings would replace the hand-written allocator and sink
  imports.
- WASI 0.2.0 shipped in January 2024; WASI 0.3.0, which adds native async to the component model,
  shipped on 2026-06-11 and is available in Wasmtime 46 and later [W31]; the component model's own
  pages call WASI 0.3.2 the current stable release [W28].
- Take that step after the core-module interface has proven the semantics. It would be a new
  generation of the WebAssembly module interface, with its own ADR.

## Determinism

- **WebAssembly itself.** Execution is nondeterministic only in a few well-defined cases: values
  exchanged with the host, shared memory between threads, NaN bit patterns and signs, relaxed SIMD,
  and resource exhaustion, such as failing allocations or stack exhaustion [W12].
- **Model B settings.** No threads and no shared memory; NaN canonicalization on; relaxed SIMD in
  deterministic mode or disabled [W14][W18]; no host imports that read clocks or randomness [W18];
  and a failed memory growth treated as a failure of the call (the guest returns
  `PRAENORA_STATUS_RESOURCE_EXHAUSTED`), because growth below the maximum may succeed or fail
  depending on the host [W18]. With these, the same `.wasm` file, engine version and settings give
  the same outputs on every host that runs them (unverified across engines and architectures; a
  spike item). The engine's version and settings belong in the reproducibility section.
- **Model B timeouts.** An epoch deadline depends on wall time, as native deadlines do. It decides
  only whether a call fails; it never changes the outputs of a run that completes.
- **Model A.** A run compiled to WebAssembly is another platform: results agree with native runs
  within a tolerance only, as across native platforms
  ([reproducibility.md](../specs/reproducibility.md#determinism)). The inline executor keeps the
  runtime's order of calls; the threaded variant keeps it too, since the engine makes one module call
  at a time.

## Threading

- **Model A with pthreads:** cross-origin isolation in browsers, `PROXY_TO_PTHREAD`, and a thread
  pool at least as large as the number of instances plus the runtime's own threads (see above).
- **Model A without pthreads:** the inline executor; everything runs on one thread, which in a
  browser is a dedicated Web Worker.
- **Model B:** each store is created, called and dropped on its instance's worker thread. Wasmtime's
  store documentation states no thread-safety guarantee [W15], so a store never changes threads. The
  epoch ticker is the only extra thread, and the function it calls is thread-safe [W16].

## Security

- **Model A** runs inside the browser's or Node's sandbox, but modules and runtime share one linear
  memory: no isolation between them. Modules are trusted code compiled into the application, as
  native modules are trusted code today.
- **Model B** isolates every instance: linear memory accesses are bounds-checked, compiled code is
  immutable and not observable, call targets are checked [W11], and a guest can reach the host only
  through the imports it is given. A trap ends the call, not the process, which narrows OQ-5 for
  modules built this way. Store limits cap a linear memory's size [W15]. The engine is now trusted
  code with its own record of vulnerabilities [W23][W27], so it must be kept current.
- **Digests.** `library.sha256` pinning works unchanged for `.wasm` files.
- **macOS hardened runtime.** A JIT-compiling engine needs memory that is writable and executable,
  which the Hardened Runtime allows only with the `com.apple.security.cs.allow-jit` entitlement and
  `MAP_JIT` [W33]. Whether Wasmtime's or WAMR's code generators use `MAP_JIT`, and whether WAMR's
  interpreters need no executable memory, is unverified.

## Build and deployment

### Model A

A toolchain preset, sketched for `CMakeUserPresets.json` or a later `CMakePresets.json`. `EMSCRIPTEN`
names the Emscripten root, the directory that holds `cmake/Modules/Platform/Emscripten.cmake` [W6];
where an Emscripten installation keeps that directory is not checked here (unverified).

```json
{
  "version": 5,
  "configurePresets": [
    {
      "name": "wasm-emscripten-release",
      "displayName": "WebAssembly (Emscripten), Release (sketch)",
      "inherits": ["base", "release"],
      "toolchainFile": "$env{EMSCRIPTEN}/cmake/Modules/Platform/Emscripten.cmake",
      "cacheVariables": {
        "PRAENORA_BUILD_PYTHON": "OFF",
        "PRAENORA_INSTALL": "OFF",
        "PRAENORA_INLINE_EXECUTOR": "ON",
        "PRAENORA_STATIC_MODULES": "ON"
      }
    }
  ],
  "buildPresets": [
    {"name": "wasm-emscripten-release", "configurePreset": "wasm-emscripten-release"}
  ],
  "testPresets": [
    {"name": "wasm-emscripten-release", "configurePreset": "wasm-emscripten-release",
     "output": {"outputOnFailure": true}}
  ]
}
```

```cmake
# Sketch: settings the project would add for Emscripten builds.
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    add_compile_options(-fwasm-exceptions)                         # praenora::Error needs catching
    add_link_options(-fwasm-exceptions -sALLOW_MEMORY_GROWTH=1)
    # Threaded variant instead of the inline executor:
    #   -pthread at compile and link, -sPROXY_TO_PTHREAD, -sPTHREAD_POOL_SIZE=<instances + 1>
endif()
```

- `toolchainFile` exists in presets from version 3, which this repository's version 5 includes, and
  takes precedence over `CMAKE_TOOLCHAIN_FILE`; `$env{...}` expands an environment variable, or to an
  empty string when it is unset [W32]. `emcmake` passes the same toolchain file automatically [W2].
- The toolchain sets `CMAKE_SYSTEM_NAME` to `Emscripten` and the executable suffix to `.js`, uses
  Node as `CMAKE_CROSSCOMPILING_EMULATOR` when it finds one, and searches for libraries, headers and
  packages only under the target root by default [W6]. So `ctest` can run test executables under
  Node, and the dependencies (nlohmann/json, jsoncons, yaml-cpp, spdlog, GoogleTest)
  must be built with Emscripten as well; that each builds unchanged is unverified.
- `PRAENORA_INLINE_EXECUTOR` and `PRAENORA_STATIC_MODULES` are names proposed here. The Python
  bindings and install rules stay off. Tests that read fixture files need Node's file system
  (NODERAWFS, so far mostly tested on Linux [W5]).
- Emscripten is a cross-compiler, so one WebAssembly build serves Linux, macOS and Windows hosts;
  that the toolchain itself runs on all three is unverified.

### Model B

- **Option.** `PRAENORA_WITH_WASMTIME` (OFF; a name proposed here). When ON, a find module locates a
  prebuilt C API archive through `CMAKE_PREFIX_PATH` and links it PRIVATE through the runtime's
  private dependency target, as the other third-party libraries are
  ([ADR-0017](../adr/ADR-0017-dependency-management.md)):

  ```cmake
  if(PRAENORA_WITH_WASMTIME)
      find_package(Wasmtime 50 REQUIRED)   # cmake/FindWasmtime.cmake, to be written
      target_link_libraries(praenora_runtime_deps INTERFACE Wasmtime::Wasmtime)
  endif()
  ```

  Using the prebuilt archive avoids a Rust toolchain in the default build, and avoids building the
  C API as a subproject, whose CMake reads `BUILD_SHARED_LIBS` [W21], a variable this project never
  reads ([`PraenoraOptions.cmake`](../../cmake/PraenoraOptions.cmake)).
- **Linux.** The archive includes both a static and a dynamic library; a static link adds
  `-lpthread -ldl -lm` [W21]. The 50.0.1 release carries `aarch64-linux` and `aarch64-macos` C API
  archives [W22]; the x86-64 archives are expected but were not visible in the listing (unverified).
- **macOS.** No extra system libraries for a static link [W21]. arm64 is Tier 2 [W19]. See the
  hardened-runtime note under Security.
- **Windows.** A static link needs `WASM_API_EXTERN` and `WASI_API_EXTERN` defined empty and the
  system libraries `ws2_32.lib`, `advapi32.lib`, `userenv.lib`, `ntdll.lib`, `shell32.lib`,
  `ole32.lib` and `bcrypt.lib` [W21].
- **WAMR instead.** A CMake-native C library, configured with options such as `WAMR_BUILD_INTERP`,
  `WAMR_BUILD_FAST_INTERP`, `WAMR_BUILD_AOT`, `WAMR_BUILD_JIT`, `WAMR_BUILD_FAST_JIT` and
  `WAMR_BUILD_INSTRUCTION_METERING`, with the target platform chosen by `WAMR_BUILD_PLATFORM` [W25];
  it could come through the opt-in FetchContent mode with a pinned commit.
- **Guest modules** are compiled for a 32-bit WebAssembly target. Which toolchain the project's
  fixture modules would use is open (unverified); `praenora_add_module()` would need a variant that
  produces a `.wasm` file and its manifest ([`PraenoraModule.cmake`](../../cmake/PraenoraModule.cmake)).

## Risks

| Risk | Model | Effect | Mitigation |
|---|---|---|---|
| The runtime's dependencies do not build with Emscripten, or need patches | A | No WebAssembly runtime | Spike step A1 builds each first. |
| Pthreads in browsers need cross-origin isolation and a large enough thread pool | A | Deployments that cannot set the headers lose threads; a pool too small deadlocks | Inline executor as the default for Model A. |
| A module that never returns hangs a run without threads | A | The page or program hangs | Dedicated Web Worker that the embedder can terminate. |
| No simulation service in the browser | A | Modules that simulate cannot run | Keep simulation in native or Model B deployments; revisit if libRoadRunner gains a backend without code generation. |
| The engine is a large, fast-moving dependency with security advisories | B | Upgrade effort; exposure to engine defects | Follow Wasmtime's long-term releases; pin versions; record them in the run record. |
| Epoch interruption costs about 10% [W13] | B | Slower modules | Measure on the example; make the ticker period configurable. |
| arm64 macOS is Tier 2 for Wasmtime [W19] | B | Less tested on the platform this repository is verified on | Spike on macOS arm64 first. |
| Manifest changes (static libraries, `.wasm` libraries, platform values) | A, B | PMM version bump, discovery rules to extend | One ADR covering both, with compatibility tests. |
| Guest toolchain choice | B | Module authors need another compiler | Choose one in the spike and document it in the module developer guide. |

## Spike plan

Model B comes first. Each step has an exit criterion; the spike stops at the first that fails and
records why. The outcome is an ADR that supersedes ADR-0025 with the chosen model.

1. **B1, link.** Find a prebuilt Wasmtime C API (a long-term release, or 50.x) with a find module on
   macOS arm64 and Linux x86-64 and link it PRIVATE. *Exit:* warning-free build, the runtime exports
   exactly the symbols it exports today, and `ctest --preset dev` passes.
2. **B2, adapter.** Implement the host-side v0 table and build the example estimator for a 32-bit
   WebAssembly target. *Exit:* the example workflow runs with the WebAssembly estimator and matches
   its golden outputs within the existing tolerances.
3. **B3, contract checks.** *Exit:* `praenora modules check` passes on the WebAssembly module with
   the same checks as on the native one.
4. **B4, deadlines.** A guest that loops without polling. *Exit:* `STEP_TIMEOUT` within the
   cancellation grace, never `MODULE_UNRESPONSIVE`; the epoch overhead on the example is measured
   (Wasmtime's figure is about 10% [W13]).
5. **B5, determinism.** NaN canonicalization on, relaxed SIMD deterministic, no threads. *Exit:* two
   runs give byte-identical outputs, and the native and WebAssembly estimators agree within the
   example's tolerance.
6. **B6, faults.** A guest that traps, and one that grows memory past the store limit. *Exit:* both
   end the run with `STEP_FAILED` (the second with `RESOURCE_EXHAUSTED` in its cause), with the
   process alive and the other instances finalized and destroyed.
7. **B7, Windows.** Steps B1 to B5 on `windows-x64-*`. *Exit:* the same results.
8. **A1, Emscripten build.** Build the runtime's objects, the CLI and the example modules (statically
   registered) with the preset sketch and the inline executor. *Exit:* the example workflow runs
   under Node and matches its golden outputs within tolerance, or a list of the dependency patches
   needed is recorded.
9. **A2, browser.** Run the same workflow in a dedicated Web Worker with its files in memory. *Exit:*
   the outputs equal those of A1, without `SharedArrayBuffer`.
10. **A3, threads.** The pthreads variant with `PROXY_TO_PTHREAD`, a thread pool and cross-origin
    isolation. *Exit:* a timeout fixture behaves as on native builds.
11. **A4, size.** *Exit:* the size of the WebAssembly binary and of its JavaScript loader is
    recorded.

## Sources

Every page was read on 2026-11-11.

| Id | Source | Supports |
|---|---|---|
| W1 | [Emscripten: Pthreads support](https://emscripten.org/docs/porting/pthreads.html) | `-pthread`; COOP and COEP needed for deployment; busy-waiting on the main browser thread; `PROXY_TO_PTHREAD`; asynchronous thread start and `PTHREAD_POOL_SIZE`; memory growth with pthreads. |
| W2 | [Emscripten: Building Projects](https://emscripten.org/docs/compiling/Building-Projects.html) | `emcmake` passes the toolchain file; one WebAssembly file per project; avoid dynamic linking in most cases. |
| W3 | [Emscripten: Dynamic Linking](https://emscripten.org/docs/compiling/Dynamic-Linking.html) | Main and side modules; `dlopen` from the file system; `emscripten_dlopen`; the 8 MB synchronous compile limit; dead code elimination; overhead; pthreads still experimental; page being refreshed. |
| W4 | [Emscripten: C++ exceptions support](https://emscripten.org/docs/porting/exceptions.html) | Catching disabled by default; `-fexceptions` and `-fwasm-exceptions` and their trade-offs. |
| W5 | [Emscripten: File System API](https://emscripten.org/docs/api_reference/Filesystem-API.html) | MEMFS default and lost on reload; NODEFS and NODERAWFS for Node; no synchronous persistent storage in browsers. |
| W6 | [Emscripten's CMake toolchain file](https://raw.githubusercontent.com/emscripten-core/emscripten/main/cmake/Modules/Platform/Emscripten.cmake) | `CMAKE_SYSTEM_NAME` Emscripten; `.js` executables; Node as the cross-compiling emulator; find-root modes. |
| W7 | [MDN: SharedArrayBuffer](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer) | Shared memory needs a secure, cross-origin isolated context; the same applies to shared `WebAssembly.Memory`. |
| W8 | [web.dev: Making your website cross-origin isolated using COOP and COEP](https://web.dev/articles/coop-coep) | The header values `same-origin` and `require-corp`; isolation enables `SharedArrayBuffer` and finer timers. |
| W9 | [MDN: WebAssembly.Memory() constructor](https://developer.mozilla.org/en-US/docs/WebAssembly/Reference/JavaScript_interface/Memory/Memory) | 64 KiB pages; 4 GiB for 32-bit addressing; shared memories need a maximum. |
| W10 | [Node.js: Worker threads](https://nodejs.org/api/worker_threads.html) | Worker threads share `SharedArrayBuffer` instances; no cross-origin requirement stated. |
| W11 | [WebAssembly: Security](https://webassembly.org/docs/security/) | Sandboxed modules; immutable, unobservable code; bounds-checked linear memory; checked call targets. |
| W12 | [WebAssembly design: Nondeterminism](https://github.com/WebAssembly/design/blob/main/Nondeterminism.md) | The few well-defined sources of nondeterminism. |
| W13 | [Wasmtime: Interrupting Wasm execution](https://docs.wasmtime.dev/examples-interrupting-wasm.html) | Fuel (deterministic, costlier) and epochs (about 10% slowdown, wall-time based); trap or async yield. |
| W14 | [Wasmtime C API: `config.h`](https://docs.wasmtime.dev/c-api/config_8h.html) | Settings for fuel, epochs, NaN canonicalization, threads, shared memory, relaxed SIMD (deterministic mode), stack size, component model; fuel consumed per instruction. |
| W15 | [Wasmtime C API: `store.h`](https://docs.wasmtime.dev/c-api/store_8h.html) | Fuel and epoch deadlines per store; store limits such as `memory_size`; no thread-safety statement. |
| W16 | [Wasmtime C API: `engine.h`](https://docs.wasmtime.dev/c-api/engine_8h.html) | `wasmtime_engine_increment_epoch` is safe from any thread and async-signal-safe. |
| W17 | [Wasmtime C API: `memory.h`](https://docs.wasmtime.dev/c-api/memory_8h.html) | `wasmtime_memory_data`, `wasmtime_memory_data_size`, `wasmtime_memory_grow`. |
| W18 | [Wasmtime: Deterministic Wasm execution](https://docs.wasmtime.dev/examples-deterministic-wasm-execution.html) | Sources of nondeterminism and the settings that remove them; fuel rather than epochs for determinism. |
| W19 | [Wasmtime: Tiers of support](https://docs.wasmtime.dev/stability-tiers.html) | Platform tiers; Cranelift and Winch Tier 1, Pulley Tier 2; component model Tier 1; C API Tier 1, C++ Tier 2. |
| W20 | [Wasmtime: Release process](https://docs.wasmtime.dev/stability-release.html) | Monthly major releases supported for two months; every twelfth release supported for 24 months. |
| W21 | [Wasmtime: C API](https://docs.wasmtime.dev/c-api/) | Headers; prebuilt `-c-api` archives or a Rust build; CMake `add_subdirectory` and `BUILD_SHARED_LIBS`; static and dynamic libraries; system libraries per platform. |
| W22 | [Wasmtime releases](https://github.com/bytecodealliance/wasmtime/releases) | 50.0.1 of 2026-10-29 is the latest stable release; arm64 Linux and macOS C API archives. |
| W23 | [Wasmtime security advisories](https://github.com/bytecodealliance/wasmtime/security/advisories) | The five most recent advisories were all published on 2026-10-29. |
| W24 | [WebAssembly Micro Runtime, repository page](https://github.com/bytecodealliance/wasm-micro-runtime) | Running modes; platforms and architectures; Apache 2.0 with the LLVM exception; `wasm_export.h` and the WebAssembly C API; threads and WASI support. |
| W25 | [WAMR: `doc/build_wamr.md`](https://raw.githubusercontent.com/bytecodealliance/wasm-micro-runtime/main/doc/build_wamr.md) | Build options for the running modes, threads and instruction metering; metering in the interpreters only; Fast JIT on x86-64 only; `WAMR_BUILD_PLATFORM`. |
| W26 | [WAMR: `core/iwasm/include/wasm_export.h`](https://raw.githubusercontent.com/bytecodealliance/wasm-micro-runtime/main/core/iwasm/include/wasm_export.h) | `wasm_runtime_terminate` from another thread; `wasm_runtime_set_instruction_count_limit`; module heap allocation; addresses invalidated by memory growth. |
| W27 | [WAMR releases](https://github.com/bytecodealliance/wasm-micro-runtime/releases) | 2.4.5 of 2026-06-29 with three CVE fixes; 2.4.4 with two. |
| W28 | [The WebAssembly Component Model](https://component-model.bytecodealliance.org/) | What the component model is; WASI 0.3.2 named as the current stable release. |
| W29 | [Component Model: Canonical ABI](https://component-model.bytecodealliance.org/advanced/canonical-abi.html) | Parameters and results pass as core values or through linear memory. |
| W30 | [Component Model explainer](https://raw.githubusercontent.com/WebAssembly/component-model/main/design/mvp/Explainer.md) | The `memory` and `realloc` canonical options; UTF-8 as the default string encoding. |
| W31 | [WASI roadmap](https://wasi.dev/roadmap) | WASI 0.2.0 in January 2024; 0.3.0 on 2026-06-11 with native async, in Wasmtime 46 and later. |
| W32 | [cmake-presets(7)](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html) | `toolchainFile` since presets version 3 and its precedence; `$env{}` expansion. |
| W33 | [Allow execution of JIT-compiled code entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.allow-jit) (read through the page's documentation data) | The Hardened Runtime disallows writable and executable memory without the entitlement and `MAP_JIT`. |
| W34 | [sys-bio/libantimonyjs](https://github.com/sys-bio/libantimonyjs) | libAntimony, libSBML and expat built with Emscripten into one module. |
| W35 | [sys-bio/roadrunner, repository page](https://github.com/sys-bio/roadrunner) | libRoadRunner generates code with LLVM by default. |
| W36 | [bytecodealliance/wasmtime, repository page](https://github.com/bytecodealliance/wasmtime) | Apache-2.0 license shown. |
