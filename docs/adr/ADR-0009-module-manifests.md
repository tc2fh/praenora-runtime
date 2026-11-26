# ADR-0009: Module manifests as separately discoverable documents; identity cross-check

- Status: Accepted (scope noted in Decision)
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none
- Amended by: [ADR-0043](ADR-0043-json-schema-dialects-and-validator.md) (the schemas the runtime ships, and a module's `parameters_schema`, may be 2020-12; the reference below is the draft-07 text this ADR was written against)

## Context

The host must find modules, check compatibility and report problems before running any native code. Loading a library to learn what it is would execute untrusted code during discovery.

## Decision

Status split: separate discoverable manifests are Accepted (required by the project); the field details and rules below are Proposed.

Accepted: a Praenora Module Manifest (PMM) is a JSON document named `*.pmm.json`, readable without loading code. It holds identity, library path, compatibility declarations, capabilities and a parameter schema.

Proposed details (see [docs/specs/manifest.md](../specs/manifest.md)):
- Fields: `module {name, version, ...}`, `abi {major, minor}`, `pmp_version`, `sdk_version`, `library {path, sha256?, dependency_paths}`, `platform {os, arch}`, `capabilities[]`, `parameters_schema`, `features`, `tests.vectors`.
- `library.path` is relative to the manifest's directory; absolute paths are rejected.
- A present `library.sha256` is verified before loading (`LIBRARY_DIGEST_MISMATCH`).
- `dependency_paths` are honored on Windows only, through `AddDllDirectory`.
- After loading, `describe()` must agree with the manifest on name, version, ABI major and capabilities (`MODULE_IDENTITY_MISMATCH`).
- Unknown top-level fields are rejected except extensions matching `^x-`.
- Search order: explicit and CLI module paths, configuration `modules.search_paths`, `PRAENORA_MODULE_PATH`, then the installation default. Each directory and its immediate subdirectories are scanned in sorted order; the first match wins and a shadowed duplicate is a warning naming both paths.
- A manifest for another platform never shadows a host-platform match; `PLATFORM_MISMATCH` is reported only when nothing matches.

### Builder notes (Proposed)

- `praenora_add_module()` checks NAME against the module-name pattern, VERSION against SemVer, and each capability for the `<id>@<version>` form, at configuration time; a capability id may appear once. It generates the manifest in two passes: the template is configured with `@ONLY` when CMake configures, then `file(GENERATE)` evaluates `$<TARGET_FILE_NAME:...>` for each configuration, so a template must not otherwise contain `$<`.
- Without a template, the generated manifest holds `kind`, `format_version`, `module {name, version}`, `abi`, `pmp_version`, `sdk_version`, `library.path`, `platform`, `capabilities`, `parameters_schema: {"type": "object"}` and, when test vectors are given, `tests.vectors`.
- `platform.arch` comes from the compiler's target architecture, then the Visual Studio platform, `CMAKE_OSX_ARCHITECTURES` and `CMAKE_SYSTEM_PROCESSOR`: the order the build itself uses for `praenora::version_info()`, so manifests and the runtime agree. Several `CMAKE_OSX_ARCHITECTURES` values are rejected.
- The default output directory is `<current binary dir>/<name>`, and a relative OUTPUT_DIRECTORY is relative to the current binary directory. For multi-config generators `/$<CONFIG>` is appended unless the directory already contains a generator expression. Test vectors are copied by a build step that reruns when the file changes. Modules are also built with `VISIBILITY_INLINES_HIDDEN`.
- `INSTALL_DESTINATION` installs a module's library, manifest and test vectors together. Two additive arguments go with it: `INSTALL_COMPONENT <component>` puts them in an install component, and `INSTALL_EXCLUDE_FROM_ALL` leaves them out of a full installation, so that only `cmake --install <build> --component <component>` installs them; either without `INSTALL_DESTINATION` stops the configuration. The example modules install through them (component `Examples`, excluded unless `PRAENORA_INSTALL_EXAMPLES`; ADR-0019), so the helper's own install rules are what the install test exercises; its consumer-module installs its module with the installed helper's `INSTALL_DESTINATION`, and the consumer application loads it from there. A manifest names its library and its vectors by file name, relative to itself, so installed manifests work wherever the installation is.
- The target properties `PRAENORA_MODULE_DIR`, `PRAENORA_MODULE_MANIFEST` and `PRAENORA_MODULE_TEST_VECTORS_PATH` (the copied vectors' full path; empty without TEST_VECTORS) let a project handle a module's files with rules of its own. The last is named apart from the template variable `@PRAENORA_MODULE_TEST_VECTORS@`, which holds only the file name.
- `load_manifest()` reads the file once, so `manifest_sha256` is the digest of exactly the bytes parsed (a file beyond the 16 MiB document limit is refused before it is read). It checks, in order: `kind`, `format_version` (`FORMAT_VERSION_UNSUPPORTED` names the supported formats), the `module-manifest` schema (`MANIFEST_INVALID` at the first problem's pointer, every problem in the error's details), then the rules below. Every error carries the manifest's file and, once the file has been read, the line and column of the problem, with its pointer where there is one; a file beyond the size limit, or one that cannot be read, carries the file only.
- `library.path`, `library.dependency_paths` and `tests.vectors` are refused when absolute in either POSIX or Windows notation (a leading `/` or `\`, a drive letter, a root name), because one manifest directory may serve several platforms. A capability listed twice with the same id and version is `MANIFEST_INVALID`. Without `parameters_schema`, `{"type": "object"}` applies.
- Paths in a loaded manifest are absolute and lexically normal; symbolic links are not resolved.
- Search paths are made absolute (relative ones against the current directory), lexically normal and without a trailing separator, then deduplicated in order. The installation default is included even when the directory does not exist.
- Scanning lists a search path's own `*.pmm.json` files (the name must end exactly so), then those of each immediate subdirectory, every list sorted by the UTF-8 bytes of the names. A missing search path is skipped silently; an existing path that is not a directory is a warning with `NOT_FOUND`, and an unreadable directory a warning with `IO_ERROR`. A manifest file reached twice through overlapping search paths is indexed once.
- The warning for a shadowed manifest carries the code `OK`: it reports no failure, and its severity says how much it weighs. Warnings for invalid manifests carry the manifest's own error code.
- Module versions compare by SemVer precedence, build metadata ignored, in resolution, in shadowing and in the identity check. `resolve()` reports `MODULE_NOT_FOUND` with the searched paths and the other versions of the module that were found, or `PLATFORM_MISMATCH` naming the platforms found, located at the first such manifest's `/platform`.
- Identity check: a `describe()` that disagrees with the manifest, by a different name, version, `abi.major` or set of capabilities (compared as sets), is `MODULE_IDENTITY_MISMATCH`; the message and the details (`differences`: field, manifest value, `describe()` value) name every differing field. A `describe()` that gives no usable identity at all is `MODULE_IDENTITY_MISMATCH` too, because the module owns the fix just the same: it fails, writes more than 1 MiB, writes something that is not JSON, writes a document violating the `pmp-describe` schema of the runtime's PMP version, or reports a version that cannot be compared (the schema's pattern admits numbers beyond 64 bits). `API_TABLE_INVALID` stays reserved for the function table itself.
- The discovery tests use search-path layouts generated by the build from `tests/fixtures/manifests/layouts/` into `<build>/test-layouts/` (with the configuration first for multi-config generators); their manifests point at `fx-echo`'s library through a relative path computed with `$<PATH:RELATIVE_PATH,...>`.
- A manifest whose `kind` is missing or is not `module-manifest` is `MANIFEST_INVALID`, like every other defect of a manifest, not `DOCUMENT_KIND_MISMATCH`: the module owns the fix either way.
- `features.cancellation` is read and kept but changes nothing: the runtime requests cancellation the same way from every module (`cancel_requested()`), and a module that never polls simply runs into its deadline. `features.diagnostics` decides whether a failure's details include the instance's `diagnostics()` output (ADR-0011), and `features.self_test` whether the contract checks require a `self_test()` function (ADR-0024).
- Later: [ADR-0033](ADR-0033-describe-and-manifest-agree-on-the-sdk-version.md) adds `sdk_version` to the identity check, compared by SemVer precedence like `version`: a `describe()` that reports another SDK version than the manifest is `MODULE_IDENTITY_MISMATCH`. `abi.minor` and `pmp_version` are still not compared.
- Later: [ADR-0036](ADR-0036-describe-and-manifest-agree-on-every-field-they-share.md) supersedes ADR-0033 and makes the identity check compare every field the two documents share: `abi.minor` as well, and `pmp_version` by SemVer precedence, so the compatibility decided from the manifest holds for the library (`MODULE_IDENTITY_MISMATCH`).
- Later: every integer of a manifest is read by its value, so an `abi.major`, `abi.minor` or `requires.services[]` major or minor written `-0`, which JSON allows and the schema accepts as an integer, is 0; `describe()`'s `abi` is compared the same way ([ADR-0036](ADR-0036-describe-and-manifest-agree-on-every-field-they-share.md), builder notes).

## Alternatives considered

- **Query the library by loading it.** Runs native code during discovery and cannot diagnose platform or ABI mismatches before loading.
- **Naming convention only (no manifest).** Cannot carry capabilities, parameter schema or digests.
- **A central registry file.** Needs a second source of truth to keep in step with the libraries.

## Consequences

- Validation can report manifest, platform, ABI, protocol and capability problems with no native code executed.
- Two documents (manifest and `describe`) must agree, which catches stale manifests.
- Module build systems must generate a manifest (the SDK helper `praenora_add_module` does).

## References

- [docs/specs/manifest.md](../specs/manifest.md)
- [docs/specs/module-abi.md](../specs/module-abi.md)
- [JSON Schema draft-07](https://json-schema.org/draft-07/json-schema-release-notes)
