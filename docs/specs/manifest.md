# Praenora Module Manifest (PMM)

- **Version:** PMM 0.2.0 (documents of kind `module-manifest` with `format_version` 0.2.x, schema
  [`module-manifest.0.2.schema.json`](../../spec/schemas/module-manifest.0.2.schema.json));
  manifests of format 0.1.x are still read, with
  [`module-manifest.0.1.schema.json`](../../spec/schemas/module-manifest.0.1.schema.json)
- **Status:** Normative, MVP. Separately discoverable manifests are Accepted; the fields and rules
  below are Proposed ([ADR-0009](../adr/ADR-0009-module-manifests.md); the service requirements
  of format 0.2, [ADR-0026](../adr/ADR-0026-runtime-service-providers.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

A module manifest describes one module library so that the runtime can find it, check it and report
problems before any of the module's code runs: its identity, its library, its compatibility
declarations, the capabilities it implements, the schema of its parameters and the runtime services
it requires or can use. This document is for
module authors and for implementers of another host. The library's binary interface is the
[module ABI](module-abi.md).

## Example

The manifest of the example estimator, as `praenora_add_module()` generates it on macOS arm64:

```json
{
  "kind": "module-manifest",
  "format_version": "0.2.0",
  "module": {
    "name": "example-estimator",
    "version": "0.1.0",
    "summary": "Blends new observations with the previous tick's predictions (example).",
    "authors": ["Praenora maintainers"],
    "repository": "https://github.com/INFLECT-Praenora/praenora-runtime"
  },
  "abi": { "major": 0, "minor": 0 },
  "pmp_version": "0.1.0",
  "sdk_version": "0.2.0",
  "library": { "path": "example-estimator.so" },
  "platform": { "os": "macos", "arch": "arm64" },
  "capabilities": [{"id":"example.estimate","version":"0.1.0"}],
  "parameters_schema": {
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "properties": {
      "gain": { "type": "number", "minimum": 0, "maximum": 1, "default": 0.5 }
    },
    "additionalProperties": false
  },
  "features": {
    "concurrency": "instance",
    "cancellation": false,
    "diagnostics": true,
    "self_test": false
  },
  "tests": { "vectors": "vectors.json" }
}
```

A module that uses runtime services of service providers declares them in `requires`
([runtime-services.md](runtime-services.md)). The manifest of the tests' fixture module
`fx-service`, which requires `test.echo@0` and can use `test.other@1`, holds:

```json
"requires": {
  "services": [
    {"name": "test.echo", "major": 0, "minor": 0, "optional": false},
    {"name": "test.other", "major": 1, "minor": 0, "optional": true}
  ]
}
```

## Fields

| Field | Type | Required | Meaning |
|---|---|---|---|
| `kind` | `"module-manifest"` | yes | The document kind. |
| `format_version` | semantic version | yes | The PMM format; 0.2.x is read with the 0.2 schema, 0.1.x with the 0.1 schema. |
| `module.name` | module name | yes | The module's name ([protocol.md](protocol.md#identifiers)). |
| `module.version` | semantic version | yes | The module's version. |
| `module.summary`, `module.authors`, `module.repository` | string, array of strings, string | no | Informational. |
| `abi.major`, `abi.minor` | integers from 0 to 4294967295 | yes | The ABI version the library was built for. |
| `pmp_version` | semantic version | yes | The PMP version the module implements. |
| `sdk_version` | semantic version | yes | The SDK version the module was built with. |
| `library.path` | non-empty string | yes | The library file, relative to the manifest's directory. |
| `library.sha256` | 64 lowercase hex digits | no | The library's SHA-256, which the runtime then verifies. |
| `library.dependency_paths` | array of non-empty strings | no | Directories, relative to the manifest's directory, that hold the library's own dependency DLLs (Windows). |
| `platform.os` | `linux`, `macos` or `windows` | yes | The operating system the library was built for. |
| `platform.arch` | `x86_64` or `arm64` | yes | The architecture the library was built for. |
| `capabilities` | array of `{id, version}`, at least one | yes | The capabilities the module implements. |
| `parameters_schema` | object | no | A JSON Schema of a step's parameters, draft-07 or 2020-12 (its `$schema`; draft-07 when absent); `{"type": "object"}` when the field is absent. |
| `requires.services` | array of `{name, major, minor, optional}` | no (format 0.2) | The runtime services of providers that the module requires or can use. |
| `requires.services[].name` | service name | yes | A dotted identifier, such as `praenora.simulation` ([runtime-services.md](runtime-services.md#services)). |
| `requires.services[].major` | integer from 0 to 4294967295 | yes | The service's major version. |
| `requires.services[].minor` | integer from 0 to 4294967295 | no (default 0) | The lowest minor version the module accepts. |
| `requires.services[].optional` | boolean | no (default `false`) | `false`: the module requires the service; `true`: it uses the service when the run has it. |
| `features.concurrency` | `module` or `instance` | no (default `module`) | Whether the library's instances may be called concurrently. |
| `features.cancellation` | boolean | no (default `false`) | Whether the module polls `cancel_requested()`. |
| `features.diagnostics` | boolean | no (default `false`) | Whether the module implements `diagnostics()`. |
| `features.self_test` | boolean | no (default `false`) | Whether the module implements `self_test()`. |
| `tests.vectors` | non-empty string | no | The module's test vectors, relative to the manifest's directory. |
| `x-...` | any | no | Extension fields, at the top level only. |

## Rules

- **File.** A manifest is a strict JSON file whose name ends in `.pmm.json`, encoded in UTF-8 and at
  most 16 MiB. A manifest MUST be valid against the `module-manifest` schema: unknown properties are
  refused, except top-level properties whose names start with `x-`.
- **Paths.** `library.path`, every `library.dependency_paths` entry and `tests.vectors` MUST be
  relative to the manifest's directory. A path that is absolute in POSIX or in Windows notation (a
  leading `/` or `\`, a drive letter, a root name) is refused on every platform, because one
  directory may hold manifests for several platforms. Paths are resolved lexically; symbolic links
  are not resolved.
- **Digest.** When `library.sha256` is present, the runtime verifies the library file against it
  during validation and again before the library is loaded (`LIBRARY_DIGEST_MISMATCH`). Without it,
  the digest is still computed and recorded in the run record.
- **Dependency paths.** On Windows the runtime adds each `library.dependency_paths` directory to the
  DLL search with `AddDllDirectory` while it loads the library. On POSIX systems they are ignored: a
  module SHOULD find its own dependencies through run paths (`$ORIGIN`, `@loader_path`)
  ([ADR-0019](../adr/ADR-0019-platform-loading-rpath-dll-search.md)).
- **Capabilities.** Each `{id, version}` MUST appear at most once. A step can be bound to the
  module only when the manifest lists the step's capability at exactly the version the workflow
  definition requires (build metadata ignored).
- **Parameters.** `parameters_schema` MUST be a valid schema of its dialect: draft-07 (the default
  when it has no `$schema`) or 2020-12 (`"$schema": "https://json-schema.org/draft/2020-12/schema"`);
  any other `$schema` is refused, a schema that refers to itself without looking at the instance
  (`{"$ref": "#"}`) is refused, and `format` is asserted for the formats the validator knows. Draft-07
  ignores the keywords beside a `$ref`, a `default` among them; 2020-12 applies them (`praenora
  check module` notes the draft-07 case). The runtime validates a step's `parameters` against it
  (`PARAMETERS_INVALID`, located at `/steps/<step>/parameters<pointer>` in the configuration) and
  fills in its `default` values before `create`. The resulting parameters are what `create` receives
  and what the run record holds.
- **Features.** `concurrency: "module"` (the default) declares that no two instances of the library
  may be called concurrently; `"instance"` that its instances are independent
  ([threading-and-ownership.md](threading-and-ownership.md#concurrency-across-instances)).
  `cancellation` is informational: the runtime requests cancellation the same way from every module.
  With `diagnostics: true` the runtime asks a failing instance for `diagnostics()` and adds the
  document to the failure's details. With `self_test: true` the contract checks require a
  `self_test()` function.
- **Services.** Each `(name, major)` of `requires.services` MUST appear at most once. A manifest of
  format 0.1 has no `requires` and declares no service. Validation resolves each declared service on
  the provider search paths ([runtime-services.md](runtime-services.md#resolution)): a required one
  that does not resolve is `SERVICE_UNAVAILABLE`, an optional one is then not used. The module's
  instances are served exactly the declared services that resolved, besides the runtime's built-in
  ones ([runtime-services.md](runtime-services.md#visibility)). The declarations are not part of
  `describe()` and not compared with it.
- **Test vectors.** `tests.vectors` names a `module-test-vectors` document that
  `praenora modules check` uses unless `--vectors` names another
  ([ADR-0024](../adr/ADR-0024-contract-checks-and-test-vectors.md)).

## Reading a manifest

The runtime reads a manifest without loading the library and checks it in this order. Every error
is located in the manifest's file and, once the file has been read, at the line and column of the
problem, with its JSON pointer where there is one; a file beyond the size limit, or one that cannot
be read, is located at the file only:

| # | Check | Error |
|---:|---|---|
| 1 | The file is at most 16 MiB, can be read and is JSON. | `PARSE_ERROR` (too large, or not JSON), `IO_ERROR` |
| 2 | It is an object whose `kind` is `module-manifest`. | `MANIFEST_INVALID` |
| 3 | `format_version` is a semantic version for which the runtime has a schema. | `MANIFEST_INVALID`, `FORMAT_VERSION_UNSUPPORTED` (naming the supported formats) |
| 4 | It satisfies the schema. | `MANIFEST_INVALID` at the first problem's pointer, every problem in the details |
| 5 | `module.version` is a usable semantic version, `abi.major` and `abi.minor` fit in 32 bits, and `pmp_version` and `sdk_version` are usable semantic versions, in that order. | `MANIFEST_INVALID` |
| 6 | `library.path` and every `library.dependency_paths` entry are relative. | `MANIFEST_INVALID` |
| 7 | Capability by capability, in order: its version is a usable semantic version, and the capability is not listed before. | `MANIFEST_INVALID` |
| 8 | Service by service in `requires.services`, in order: its major and minor fit in 32 bits, and its name and major are not listed before. | `MANIFEST_INVALID` |
| 9 | `parameters_schema` compiles. | `MANIFEST_INVALID` |
| 10 | `tests.vectors` is relative. | `MANIFEST_INVALID` |

An integer is read by its value (checks 5 and 8): one written `-0`, which JSON allows and the schema
accepts as an integer, is 0.

The manifest's SHA-256 is the digest of exactly the bytes read; it is part of the run fingerprint
([reproducibility.md](reproducibility.md)).

## Discovery

The runtime looks for manifests on the module search paths, in this order; the first match wins, as
with `PATH`:

1. the caller's paths: `RuntimeOptions::module_paths` in the C++ API, `--module-path` on the command
   line, in the order given (relative paths against the current directory);
2. the configuration's `modules.search_paths` (relative paths against the configuration file's
   directory);
3. the entries of the environment variable `PRAENORA_MODULE_PATH`, separated by `:` (`;` on
   Windows), unless the caller disabled the environment (`RuntimeOptions::use_environment`);
4. the installation's module directory, `<prefix>/lib/praenora/modules`, located from the runtime
   library's own path.

Each path is made absolute and lexically normal, and a path named twice is searched once. In each
search path the runtime reads the path's own `*.pmm.json` files, then those of each immediate
subdirectory, each list sorted by the UTF-8 bytes of the names. Then:

- A missing search path is skipped; only one that the configuration names gets a warning
  (`NOT_FOUND`). An existing path that is not a directory gets a warning (`NOT_FOUND`), and an
  unreadable directory one with `IO_ERROR`.
- An invalid manifest is reported as a warning carrying its error, and skipped.
- A manifest file reached through two overlapping search paths is read once.
- A manifest with the name, version and platform of an earlier one is shadowed: it is never used,
  and a warning (code `OK`) names both files.
- Manifests for other platforms are indexed too, and never shadow a manifest for the host.

A step binds a module by name and exact version. The runtime resolves the binding to the first
manifest, in search order, of that name and version (build metadata ignored) for the host's
platform. When there is none, the error is `PLATFORM_MISMATCH` if manifests of that name and
version exist only for other platforms (naming them), and `MODULE_NOT_FOUND` otherwise (naming the
searched paths and the other versions found). `praenora modules list` and
`praenora modules inspect <name>[@<version>]` show what discovery found. The run record says where
each step's manifest was found, relative to its search path and without an absolute path
([reproducibility.md](reproducibility.md#where-a-manifest-was-found)).

## Compatibility and library checks

Before loading anything, validation checks the resolved manifest against the step, in this order
([compatibility-policy.md](compatibility-policy.md)):

| # | Rule | Error (pointer) |
|---:|---|---|
| 1 | `platform.os` and `platform.arch` equal the host's. | `PLATFORM_MISMATCH` (`/platform`) |
| 2 | `abi.major` equals the runtime's ABI major; any `abi.minor` is accepted. | `ABI_INCOMPATIBLE` (`/abi/major`) |
| 3 | `pmp_version` is in the runtime's PMP compatibility class and not newer than the runtime's PMP version. | `PROTOCOL_INCOMPATIBLE` (`/pmp_version`) |
| 4 | `capabilities` lists the step's capability at the required version. | `CAPABILITY_NOT_IMPLEMENTED` (`/capabilities`) |
| 5 | Each service `requires.services` lists as required (`optional` false) resolves on the provider search paths ([runtime-services.md](runtime-services.md#resolution)). | `SERVICE_UNAVAILABLE` (`/requires/services/<i>`) |
| 6 | The library file exists and can be read. | `LIBRARY_NOT_FOUND`, `IO_ERROR` (`/library/path`) |
| 7 | When `library.sha256` is present, the file has that digest. | `LIBRARY_DIGEST_MISMATCH` (`/library/sha256`) |

Rule 5 reports every required service that does not resolve, and does not keep the later rules
from running. The library's digest is computed in rule 6 or 7 and kept for the run. When the run
loads the library it computes the digest again and refuses a file that changed in between
(`LIBRARY_DIGEST_MISMATCH`). Then the library file of each provider the run uses is checked the
same way, located in the provider's manifest
([runtime-services.md](runtime-services.md#resolution)).

## Identity check

After loading the library, the runtime calls `describe()` ([module-abi.md](module-abi.md)) and
compares the document with the manifest. The two MUST agree on:

- `name`;
- `version`, by SemVer precedence;
- `abi.major` and `abi.minor`, as integers (a `-0` is 0);
- `sdk_version`, by SemVer precedence;
- `pmp_version`, by SemVer precedence;
- the capabilities, compared as sets of `{id, version}`.

These are all the fields the two documents share. A disagreement, or a `describe()` that fails or
writes no valid `pmp-describe` document, is `MODULE_IDENTITY_MISMATCH`; the message and the details
(`differences`: field, manifest value, `describe()` value) name every differing field, in the order
of the list above. A stale manifest beside a rebuilt library is caught here, also when the library
was only rebuilt against another SDK. The compatibility rules read the manifest before the library
is loaded ([Compatibility and library checks](#compatibility-and-library-checks)); since
`describe()` must report the same ABI and PMP versions, what they decided holds for the library
too. The SDK version decides no compatibility
([compatibility-policy.md](compatibility-policy.md#versioned-items)): the two documents must agree
on it, and the run fingerprint holds it ([reproducibility.md](reproducibility.md#run-fingerprint)).

A module reports its ABI minor a third time, in the header of its library's function table. That
one is compared with neither document ([module-abi.md](module-abi.md#entry-point)).

## Generating manifests

Module projects build with [`PraenoraModule.cmake`](../../cmake/PraenoraModule.cmake), installed
with the module SDK:

```cmake
find_package(Praenora 0.2 REQUIRED COMPONENTS ModuleSDK)
praenora_add_module(example_estimator
  NAME         example-estimator
  VERSION      0.1.0
  CAPABILITIES example.estimate@0.1.0
  SOURCES      src/estimator.cpp
  MANIFEST_TEMPLATE example-estimator.pmm.json.in
  TEST_VECTORS vectors.json)
```

`praenora_add_module()` always creates a `MODULE` library with hidden visibility, named after the
module without a `lib` prefix, and writes `<name>.pmm.json` beside it, evaluated for each build
configuration, in the manifest format of the SDK (0.2.0). `REQUIRES_SERVICES` and
`OPTIONAL_SERVICES` list the services the module requires and can use, each as
`<name>@<major>.<minor>` (for example `praenora.simulation@0.0`); a name and major appears once in
both lists together. Without `MANIFEST_TEMPLATE` it writes the required fields,
`"parameters_schema": {"type": "object"}` and, when the module lists services, `requires`. A
template is processed like `configure_file(@ONLY)` with these variables:

| Variable | Value |
|---|---|
| `@PRAENORA_MODULE_MANIFEST_FORMAT_VERSION@` | The manifest format the SDK writes, 0.2.0, for `format_version`. |
| `@PRAENORA_MODULE_NAME@`, `@PRAENORA_MODULE_VERSION@` | `NAME` and `VERSION`. |
| `@PRAENORA_MODULE_CAPABILITIES_JSON@` | `CAPABILITIES` as a JSON array of `{id, version}`. |
| `@PRAENORA_MODULE_REQUIRES_JSON@` | The value of `requires`: `{"services": [...]}`, the required services first, each `{name, major, minor, optional}`; an empty list when the module lists none. |
| `@PRAENORA_ABI_MAJOR@`, `@PRAENORA_ABI_MINOR@`, `@PRAENORA_PMP_VERSION@`, `@PRAENORA_SDK_VERSION@` | The versions of the SDK the module is built against. |
| `@PRAENORA_MODULE_OS@`, `@PRAENORA_MODULE_ARCH@` | The target platform in manifest vocabulary: `linux`, `macos` or `windows`; `x86_64` or `arm64`. |
| `@PRAENORA_MODULE_LIBRARY_FILE@` | The library's file name, for each build configuration. |
| `@PRAENORA_MODULE_TEST_VECTORS@` | The file name of the copied test vectors, or empty. |

A template MUST NOT otherwise contain `$<`, because generator expressions in the result are
evaluated. With `INSTALL_DESTINATION`, the library, the manifest and the test vectors are installed
together; since the manifest names the other two relative to itself, an installed module works
wherever the installation is moved.

## References

- [ADR-0009](../adr/ADR-0009-module-manifests.md): module manifests.
- [ADR-0014](../adr/ADR-0014-versioning-and-compatibility-policy.md): versioning and compatibility.
- [ADR-0019](../adr/ADR-0019-platform-loading-rpath-dll-search.md): platform loading.
- [ADR-0024](../adr/ADR-0024-contract-checks-and-test-vectors.md): contract checks and test vectors.
- [open-questions.md](../open-questions.md): OQ-4 (exact module pins), OQ-19 (signing),
  OQ-26 (deployment layout).
- [module-developer-guide.md](../module-developer-guide.md#the-manifest): manifest templates.
