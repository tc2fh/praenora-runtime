# Error taxonomy

- **Version:** runtime 0.2.0; the general codes 0 to 12 belong to ABI 0.0
  ([`status.h`](../../include/praenora/sdk/status.h))
- **Status:** Normative, MVP. The code space and its names are stable; the table is Proposed
  ([ADR-0013](../adr/ADR-0013-error-taxonomy-and-status-codes.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

Every failure the runtime reports has one code from a single `int32` code space shared by the module
ABI and the runtime. A code has a stable name, a category that decides the CLI's exit code, a likely
owner (who usually has to act), and a retryable flag. This document lists every code and says where
to look when one appears.

## Code space

| Range | Meaning | Who may use it |
|---|---|---|
| 0 | `OK`: success. | Everyone. |
| 1 to 99 | General codes; 1 to 12 are defined in this version, in `status.h` as `PRAENORA_STATUS_*` and in C++ as `praenora::ErrorCode`. | Modules (as call statuses) and the runtime. |
| 100 to 999 | Runtime codes, by component: 1xx documents, 2xx configuration, 3xx manifests and libraries, 4xx compatibility and catalog, 5xx loading, 6xx workflow graphs, 7xx execution, 8xx target data. | The runtime only. |
| 1000 to 9999 | Unassigned. | Nobody. |
| 10000 and up | Module-defined codes (`PRAENORA_STATUS_MODULE_BASE`). | Modules. They are reported verbatim as the `cause` of a runtime error, never as an `ErrorCode` of their own. |

Names and numbers are stable: changing either is a breaking change. A unit test checks that
`status.h` and `praenora::ErrorCode` agree, and a documentation test checks that every `ErrorCode`
name appears below.

## Categories and exit codes

| Category | CLI exit code | Meaning |
|---|---:|---|
| `usage` | 2 | The caller used the API or the CLI wrongly. |
| `validation` | 3 | A document, a configuration, a manifest or a workflow graph is invalid. |
| `compatibility` | 3 | Versions or features do not fit together. |
| `load` | 4 | A module library could not be loaded or checked. |
| `execution` | 1 | A run failed while executing. |
| `timeout` | 5 | A module call passed its deadline. |
| `cancelled` | 130 | The operation or the run was stopped on request. |
| `data` | 1 | Data has the wrong form or values. |
| `environment` | 74 | Files, directories or resources of the machine. |
| `internal` | 70 | A defect, usually of the runtime. |

The CLI exits with 0 on success. A command that fails with a validation report exits with 3, or
with 74 when every error in the report is `IO_ERROR`; any other failing command exits with the exit
code of its error's category.

## Codes

The table is the runtime's own code table (`praenora::all_error_codes()`, defined in
[`error.cpp`](../../src/runtime/core/error.cpp)); "Meaning" is each row's summary. An owner written
as `a/b` names both parties that commonly own the fix; `none` marks codes that report no fault.

| Code | Name | Category | Exit | Likely owner | Retryable | Meaning |
|---:|---|---|---:|---|---|---|
| 0 | `OK` | execution | 0 | none | no | The operation succeeded. |
| 1 | `UNKNOWN` | internal | 70 | runtime | no | An error of unknown origin occurred. |
| 2 | `INVALID_ARGUMENT` | usage | 2 | caller | no | An argument or input value is invalid. |
| 3 | `INVALID_STATE` | usage | 2 | caller/runtime | no | The operation is not allowed in the current state. |
| 4 | `NOT_FOUND` | environment | 74 | environment | no | A required file, directory or resource does not exist. |
| 5 | `UNSUPPORTED` | compatibility | 3 | caller/module | no | The requested feature or version is not supported. |
| 6 | `RESOURCE_EXHAUSTED` | environment | 74 | environment | yes | A resource limit was reached, for example memory or an output size limit. |
| 7 | `CANCELLED` | cancelled | 130 | none | no | The operation was cancelled. |
| 8 | `TIMEOUT` | timeout | 5 | module | yes | The operation did not finish before its deadline. |
| 9 | `IO_ERROR` | environment | 74 | environment | yes | Reading or writing a file failed. |
| 10 | `INTERNAL` | internal | 70 | runtime/module | no | An internal invariant was violated. |
| 11 | `FAILED` | execution | 1 | module | no | The operation failed. |
| 12 | `DATA_INVALID` | data | 1 | user data/module | no | Data does not have the expected form or values. |
| 100 | `PARSE_ERROR` | validation | 3 | user | no | A document is not well-formed JSON or YAML, or exceeds a document limit. |
| 101 | `SCHEMA_VIOLATION` | validation | 3 | user/module | no | A document or payload does not satisfy its JSON Schema. |
| 102 | `FORMAT_VERSION_UNSUPPORTED` | validation | 3 | user/module | no | The document's format_version is not supported by this runtime. |
| 103 | `DOCUMENT_KIND_MISMATCH` | validation | 3 | user | no | The document's kind is not the kind expected. |
| 200 | `CONFIG_INVALID` | validation | 3 | user | no | A configuration value is invalid. |
| 201 | `WORKFLOW_NOT_FOUND` | validation | 3 | user | no | The configured workflow definition is not in the catalog. |
| 202 | `BINDING_INVALID` | validation | 3 | user | no | The step bindings do not match the steps of the workflow definition. |
| 203 | `PARAMETERS_INVALID` | validation | 3 | user | no | Step parameters do not satisfy the module's parameter schema. |
| 204 | `INPUT_BINDING_INVALID` | validation | 3 | user | no | A workflow input is not bound to a valid data source. |
| 300 | `MANIFEST_INVALID` | validation | 3 | module | no | A module or provider manifest is malformed or inconsistent. |
| 301 | `MODULE_NOT_FOUND` | validation | 3 | user/environment | no | No manifest for the requested module name and version is on the search paths. |
| 302 | `MODULE_IDENTITY_MISMATCH` | load | 4 | module | no | The loaded module's describe output disagrees with its manifest. |
| 303 | `LIBRARY_DIGEST_MISMATCH` | load | 4 | environment/module | no | A module or provider library's SHA-256 differs from the digest in its manifest. |
| 304 | `LIBRARY_NOT_FOUND` | validation | 3 | module/environment | no | The module or provider library named by the manifest does not exist. |
| 400 | `ABI_INCOMPATIBLE` | validation | 3 | module | no | The module was built for a different ABI major version. |
| 401 | `PROTOCOL_INCOMPATIBLE` | validation | 3 | module | no | The module's protocol (PMP) version is incompatible with the runtime. |
| 402 | `CAPABILITY_NOT_IMPLEMENTED` | validation | 3 | user/module | no | The module does not implement the capability the step requires. |
| 403 | `PLATFORM_MISMATCH` | validation | 3 | environment | no | No manifest of the module matches the host operating system and architecture. |
| 404 | `MESSAGE_TYPE_INCOMPATIBLE` | validation | 3 | protocol | no | A message type version differs from the one required, such as at the two ends of a connection. |
| 405 | `CATALOG_ENTRY_NOT_FOUND` | validation | 3 | protocol | no | A referenced message type, capability or workflow definition is not in the catalog. |
| 406 | `CATALOG_ENTRY_DUPLICATE` | compatibility | 3 | protocol | no | Two catalog entries have the same id and version. |
| 407 | `SERVICE_UNAVAILABLE` | compatibility | 3 | environment | no | No usable service provider offers a runtime service that a module requires. |
| 500 | `LIBRARY_LOAD_FAILED` | load | 4 | environment/module | no | A module or provider library could not be loaded, or a provider failed its load checks. |
| 501 | `ENTRY_POINT_MISSING` | load | 4 | module | no | The library does not export its entry point (praenora_module_get_api or praenora_provider_get_api). |
| 502 | `API_TABLE_INVALID` | load | 4 | module | no | The function table of a module or provider is missing, malformed or incomplete. |
| 503 | `MODULE_HOST_UNAVAILABLE` | load | 4 | environment | no | A module host (praenora-module-host) for a step's module could not be found, started or used: it is missing, did not start, failed the handshake, is of another runtime version or channel protocol, or cannot apply its sandbox profile. |
| 600 | `GRAPH_INVALID` | validation | 3 | protocol | no | The workflow graph breaks a structural rule, such as fan-in or a delayed connection into a required input. |
| 601 | `GRAPH_CYCLE` | validation | 3 | protocol | no | The connections without delay form a cycle. |
| 602 | `PORT_UNCONNECTED` | validation | 3 | protocol | no | A required step input or a workflow output has no incoming connection. |
| 603 | `PORT_UNKNOWN` | validation | 3 | protocol | no | A connection names a step, port, workflow input or workflow output that does not exist. |
| 604 | `PORT_TYPE_MISMATCH` | validation | 3 | protocol | no | A connection joins ports of different message types. |
| 700 | `STEP_FAILED` | execution | 1 | module | no | A module call returned an error. |
| 701 | `OUTPUT_INVALID` | execution | 1 | module | no | A module result is malformed, names an undeclared port or violates the port's schema. |
| 702 | `STEP_TIMEOUT` | timeout | 5 | module/config | yes | A module call passed its deadline and returned within the cancellation grace period. |
| 703 | `MODULE_UNRESPONSIVE` | timeout | 5 | module | no | A module call did not return within its deadline plus the cancellation grace period (in a module host, which is then terminated), or a call or load was refused because such a call may still be running in the module's library. |
| 704 | `INPUT_MISSING` | execution | 1 | runtime | no | A required input message was not available when a step ran. |
| 705 | `RUN_STOPPED` | cancelled | 130 | none | no | The run was stopped on request. |
| 706 | `MODULE_CRASHED` | execution | 1 | module | no | A module host ended (a signal or an exit) or broke its channel's protocol during a load or a call into its module. |
| 800 | `TARGET_DATA_INVALID` | data | 1 | user data | no | Target data is malformed or breaks the observation rules; with severity warning, a finding about data that is still accepted (rows before `run.time.start`, never delivered). |
| 801 | `TARGET_DATA_UNAVAILABLE` | data | 1 | environment | yes | Target data could not be read, for example because its file does not exist. |

Notes on the table:

- `OK` is not a failure. It has no category of its own and is reported as `execution` with exit
  code 0. A warning about a shadowed module manifest carries `OK`, because it reports no failure;
  its severity says how much it weighs.
- `CANCELLED` and `TIMEOUT` are statuses of module calls. The runtime reports a stopped run as
  `RUN_STOPPED` and a call past its deadline as `STEP_TIMEOUT` or `MODULE_UNRESPONSIVE`; an
  unresponsive call's own status is `TIMEOUT`.
- `MESSAGE_TYPE_INCOMPATIBLE` is raised by graph validation for a connection whose ends carry one
  message type at two versions, located at `/connections/<i>` of the workflow definition, because
  v0 requires the exact version ([workflow-definitions.md](workflow-definitions.md#graph-rules));
  ends of two different message types are `PORT_TYPE_MISMATCH`
  ([ADR-0031](../adr/ADR-0031-raise-the-reserved-error-codes.md)).
- `TARGET_DATA_UNAVAILABLE` is raised by validation for a data file that cannot be read: one that
  does not exist, is not a file or whose status cannot be read (a directory on its path cannot be
  searched, for example), located at `/inputs/<name>/path` of the configuration, and one that
  cannot be opened or read, located at the file. What an observation CSV holds, and its size, are
  `TARGET_DATA_INVALID`; a `json-document` that is not JSON or YAML, or is beyond 16 MiB, is
  `PARSE_ERROR`, and one that violates its message type is `INPUT_BINDING_INVALID`. Input data is
  read only during validation, never during a run.
- `SERVICE_UNAVAILABLE` is raised by validation for a service that a module's manifest requires
  and that no usable provider offers, located at `/requires/services/<i>` of the manifest, and by
  the contract checks' `library.load` check for the same reason
  ([runtime-services.md](runtime-services.md#resolution)).
- Service providers ([runtime-services.md](runtime-services.md)) report problems with the codes of
  modules: `MANIFEST_INVALID` for a provider manifest, `LIBRARY_NOT_FOUND`,
  `LIBRARY_DIGEST_MISMATCH`, `ENTRY_POINT_MISSING` and `API_TABLE_INVALID` for its library, and
  `LIBRARY_LOAD_FAILED` with `details.reason` `library`, `identity`, `start`, `service` or
  `conflict` for the rest of its loading.
- `MODULE_HOST_UNAVAILABLE`, `MODULE_CRASHED` and `MODULE_UNRESPONSIVE` with `details.terminated`
  come from process isolation, a module instance that runs in a module host of its own
  ([process-isolation.md](process-isolation.md)). `MODULE_HOST_UNAVAILABLE` is located in the
  module's manifest, and `details.reason` says why: `missing` (no executable where the runtime
  looks for it, `details.executable`), `spawn` (it could not be started, for example because it is
  not executable; `details.system` holds the operating system's text; always on Windows, where
  process isolation is not available in this version), `handshake` (it did not answer as a module
  host, or not in time), `build` (it answered with another runtime version or channel protocol) or
  `sandbox` (the restricted sandbox profile cannot be applied, for example in a runtime that runs in
  a sandbox already, or its temporary directory cannot be made, or the platform has no restricted
  profile; `details.system` holds the system's text).
  `MODULE_CRASHED` has the details `{"phase", "signal", "signal_name"}` (a signal ended the module
  host, for example 11 and `SIGSEGV`), `{"phase", "exit_code"}` (it exited, for example by `exit()`
  in a module), `{"phase", "reason": "protocol"}` (it broke the channel's protocol, or closed the
  channel and kept running) or `{"phase", "reason": "ended"}` (it ended, and the system could not
  say how: its status was taken by someone else, as when children are reaped automatically), where
  `phase` is the call (`create`, `initialize`, `invoke`, `finalize`, `destroy`, `diagnostics`) or
  `load`. Under sanitizers a crash may end the process with another signal or an exit code instead
  of its own signal. A `MODULE_UNRESPONSIVE` in a module host has the details
  `{"terminated": true}`: the module host was killed, so nothing of the call is left running. As
  a step's last error from the teardown, it names the call too, as every error of the teardown
  does (`{"phase": "destroy", "terminated": true}`).
  Load errors that a module host reports keep the codes, messages and locations they have in the
  runtime's process, those of the service providers it loads for the module included (located in
  the provider's manifest). A provider runs in the module host's process: one that ends it is a
  `MODULE_CRASHED` of the step, with `phase` `load` when it ends during the load (its `start()`
  included), the call that was running when it ends during a call, and the next call when it ends
  between calls.
- `TARGET_DATA_INVALID` exits with 1 when a run meets it and with 3 when validation finds it, like
  every error of a validation report. As a warning (rows before `run.time.start`) it changes no exit
  code: the tool prints it to standard error and the exit code stays 0. `TARGET_DATA_UNAVAILABLE` is found only by validation, so the
  CLI exits with 3 for it.
- Retryable means that retrying the same operation unchanged may succeed: `RESOURCE_EXHAUSTED`,
  `TIMEOUT`, `IO_ERROR`, `STEP_TIMEOUT` and `TARGET_DATA_UNAVAILABLE`. A retry of a run is a new run
  with its own run record.

## How errors are reported

- **C++ API.** Failures throw `praenora::Error`, which carries the code, the message, a source
  location (file, line, column, JSON pointer) and details. `Error::to_json()` writes
  `{"code", "name", "category", "message", "location", "details"}`, with only the parts of the
  location that are set.
- **Validation.** Validation collects every problem in a `praenora::ValidationReport` instead of
  stopping at the first. Its JSON form is
  `{"ok": bool, "diagnostics": [{"severity", "code", "name", "category", "message", "location", "step", "module", "details"}]}`
  (`step`, `module` and `details` only when set; `details` is the object a code's details hold,
  for example those of `SERVICE_UNAVAILABLE`); its text form is one line per diagnostic in the
  compiler-style format `file:line:col: severity[NAME]: pointer: message`. Only diagnostics of
  severity `error` fail a validation; warnings and notes do not.
- **CLI.** Human-readable messages go to standard error. With `--json`, standard output holds
  exactly one document: `{"ok": true, "result": ...}` or
  `{"ok": false, "error": <Error::to_json()>, "diagnostics": [...]}`.
- **Run failures.** The error of a failed run is also in the run summary, the run record's
  `execution.error` and the status snapshot's `last_error`, each an error record with the error's
  code, message, `location` and `details` ([observability.md](observability.md#status-snapshot)),
  even when an exception broke off the ending of the run. Its details name the `step`, the
  `module` (`name@version`), the `phase` (`load`, `create`, `initialize` or `invoke`), the `tick`
  during a tick, the module's error document as `cause`, and the instance's `diagnostics()` when the
  manifest declares them ([module-abi.md](module-abi.md#status-codes-and-errors)). A service
  provider that does not load when the run starts fails it with the provider loader's error and
  details, located in the provider's manifest and naming no step
  ([runtime-services.md](runtime-services.md#runs-and-checks)). In the run record and the status
  file, the error record names the run's directories by placeholders
  ([observability.md](observability.md#known-directories-in-the-files-of-a-run)); the thrown error,
  the run summary and `Workflow::status()` keep absolute paths.
- **Teardown errors.** A `finalize` or `destroy` of the teardown never fails the run. One that
  ends in an error (which ones do is in
  [time-cancellation-timeouts.md](time-cancellation-timeouts.md#deadlines)) is only its step's last
  error, in the status snapshot and the run record's `execution.final_status`, with a warning
  (event `teardown.finalize_failed` or `teardown.destroy_failed`). Its details name the call,
  `phase` (`finalize` or `destroy`), beside those of its code, and its `cause` is the module's
  error document when the module returned one.
- **Python.** The bindings raise `praenora.runtime.PraenoraError` with the same code, name,
  category, message, location and details.

## Triage

If you see a code, start where this table points. The owner column of the code table says who
usually fixes it; [`praenora modules check`](../adr/ADR-0024-contract-checks-and-test-vectors.md)
isolates module problems from configuration problems. A module's own status, a general code or one
from 10000 up, never appears as the error's code: it is `details.cause.code` of a `STEP_FAILED`
that failed the run, and `cause.code` of one that a teardown `finalize` left as its step's last
error.

| If you see | Look at |
|---|---|
| `PARSE_ERROR` | The file, line and column in the message: YAML or JSON syntax, a duplicate key, a merge key or tag, `.inf` or `.nan`, a `.json` file that is not strict JSON, or a document beyond 16 MiB or nested deeper than 128. |
| `SCHEMA_VIOLATION` | The JSON pointer: usually a misspelled property ("property 'x' is not allowed") or a value of the wrong type. For a catalog entry, the entry's file. |
| `FORMAT_VERSION_UNSUPPORTED` | The document's `format_version`; the message lists what this runtime reads (`praenora schema list`). |
| `DOCUMENT_KIND_MISMATCH` | The document's `kind`: the wrong file was given. |
| `CONFIG_INVALID` | Values of the configuration that the schema cannot check: durations, paths, `columns`, `outputs.record`, a missing `run.ticks` when no input replays data, a log file inside the output directory's managed files, `share_compiled_models` or `sandbox: restricted` on a step that runs in the runtime's process ([process-isolation.md](process-isolation.md#configuration)); or an invalid `PRAENORA_LOG_LEVEL` or `PRAENORA_LOG_FORMAT`. |
| `WORKFLOW_NOT_FOUND` | `workflow.name` and `workflow.version` against `praenora catalog list --kind workflows`, and any extra catalog paths. |
| `BINDING_INVALID` | The configuration's `steps` against the definition's steps (`praenora graph --workflow <name>@<version>`). |
| `PARAMETERS_INVALID` | The pointer `/steps/<step>/parameters/...` against the module's `parameters_schema` (`praenora modules inspect <name>@<version>`). |
| `INPUT_BINDING_INVALID` | The configuration's `inputs`: every workflow input bound, a message type that cannot carry the source's data, a `json-document` that violates the input's message type. |
| `TARGET_DATA_INVALID` | The data file and line in the message: CSV structure, a missing column, numbers, the `missing` and `censoring` rules ([protocol.md](protocol.md#message-types)), or a CSV file beyond 256 MiB. |
| `TARGET_DATA_UNAVAILABLE` | The input's `path` in the configuration (relative to the configuration file's directory) and the file it names: it must exist, be a file and be readable by the process, and every directory on the path must be searchable. |
| `MODULE_NOT_FOUND` | The searched paths and the other versions found, both listed in the message; `--module-path`, `modules.search_paths`, `PRAENORA_MODULE_PATH`; `praenora modules list`. |
| `PLATFORM_MISMATCH` | The platforms named in the message: the module is not built or installed for this platform. |
| `MANIFEST_INVALID` | The manifest file and pointer ([manifest.md](manifest.md)); `praenora modules check <manifest>`. Regenerate the manifest with `praenora_add_module()`. |
| `ABI_INCOMPATIBLE`, `PROTOCOL_INCOMPATIBLE` | The manifest's `abi` or `pmp_version` against `praenora version`: rebuild the module against this SDK ([compatibility-policy.md](compatibility-policy.md)). |
| `CAPABILITY_NOT_IMPLEMENTED` | The step's capability version against the manifest's `capabilities`: bind a module version that implements it. |
| `LIBRARY_NOT_FOUND`, `LIBRARY_DIGEST_MISMATCH` | `library.path` and `library.sha256` in the manifest: the library is missing, or it changed after its manifest was written or after validation. |
| `LIBRARY_LOAD_FAILED` | The operating system's message in the error: missing dependency libraries (run paths, `library.dependency_paths`), the wrong architecture, code signing. For a service provider, `details.reason` too: `library` (the operating system could not load it; its text is in the message), `identity` (`details.differences`: a stale provider manifest), `start` (`details.cause`, the provider's own error), `service`, `conflict` (another library of the provider is already loaded in the process). |
| `SERVICE_UNAVAILABLE` | `details.service`, `details.search_paths` and `details.providers`, each provider that offers the service's name but cannot serve it, with the reason; the provider search paths (`--provider-path`, `PRAENORA_PROVIDER_PATH` and the installation's provider directory, [runtime-services.md](runtime-services.md#discovery)); `praenora providers list` shows every provider found and why one is unusable. |
| `ENTRY_POINT_MISSING`, `API_TABLE_INVALID`, `MODULE_IDENTITY_MISMATCH` | The module's build: `praenora modules check` names the failing check (`library.entry_point`, `api.header`, `api.required_functions`, `describe.identity`); for an identity mismatch, `details.differences` (often a stale manifest beside a rebuilt library). |
| `CATALOG_ENTRY_NOT_FOUND`, `CATALOG_ENTRY_DUPLICATE` | The catalog entry and origins named in the message; `--catalog-path` and `PRAENORA_CATALOG_PATH`. |
| `GRAPH_INVALID`, `GRAPH_CYCLE`, `PORT_UNCONNECTED`, `PORT_UNKNOWN`, `PORT_TYPE_MISMATCH` | The workflow definition at the pointer in the message ([workflow-definitions.md](workflow-definitions.md#graph-rules)); `praenora graph --workflow <name>@<version>`. |
| `MESSAGE_TYPE_INCOMPATIBLE` | The connection at the pointer in the message: its ends carry two versions of one message type. The workflow definition's inputs, outputs and step capabilities must agree on the version of each message type they connect; pin a definition whose versions agree ([workflow-definitions.md](workflow-definitions.md#graph-rules)). |
| `STEP_FAILED` | `details.step`, `details.phase`, `details.cause` (the module's own code and message) and `details.diagnostics`; for a step's last error from the teardown, `details.phase` and the record's `cause`; the step's log records (logger `module.<step>`). |
| `OUTPUT_INVALID` | `details.port`, the pointer into the result and `details.issues`; with tracing on, the trace's last records ([observability.md](observability.md#message-trace)); the module's test vectors. |
| `STEP_TIMEOUT` | The step's timeouts (`run.timeouts`, `steps.<step>.timeouts`) against its durations in the metrics, and whether the module polls `cancel_requested()` ([time-cancellation-timeouts.md](time-cancellation-timeouts.md)). |
| `MODULE_UNRESPONSIVE` | A module that ignored cancellation and never returned. In the runtime's process, its library stays loaded and its thread keeps running, and until the call returns the process refuses the calls and loads of that library that would wait for it (all of them for `features.concurrency: "module"`); a refusal names the call in `details.abandoned_call` (`call`, `by`, `module`) ([threading-and-ownership.md](threading-and-ownership.md#concurrency-across-instances)). In a module host (`details.terminated`), the module host was killed and nothing is held on to. Make the module poll `cancel_requested()`; a new process starts without the abandoned call. |
| `MODULE_CRASHED` | `details.phase` and the signal or exit code that ended the module host, or `details.reason` (`protocol`, or `ended` when the system could not say how); the step's log records (event `module_host.crashed`) and what the module wrote to standard error. A crash is the module's: run it under a debugger or a sanitizer in the runtime's process to find it ([process-isolation.md](process-isolation.md#crashes)). |
| `MODULE_HOST_UNAVAILABLE` | `details.reason`: `missing` (the installation lacks `libexec/praenora/praenora-module-host`, or the runtime came from a Python wheel, which has none), `spawn` (`details.system`: for example an executable that is not executable; or Windows), `handshake`, `build` (a module host of another runtime version or channel protocol: reinstall) or `sandbox` (the restricted profile cannot be applied, `details.system`: "Operation not permitted" when the runtime itself runs in a sandbox; or not on this platform) ([process-isolation.md](process-isolation.md#the-module-host)). |
| `RUN_STOPPED` | A stop was requested (Ctrl+C, SIGTERM, `request_stop()`, `close()`). Not a fault. |
| `IO_ERROR` | The path in the message: permissions, free space; an output directory that already holds a run record (pass `--overwrite`). |
| `INVALID_ARGUMENT`, `INVALID_STATE` | The calling code or command line: argument values, and the order of calls (for example `tick()` before `start()`, or two control calls at once). |
| `UNSUPPORTED` | A version or feature this runtime does not offer, named in the message; for a step in process mode, located at the `isolation.mode` that set it: process isolation on Windows; for a step in process mode with `sandbox: restricted`, located at the `isolation.sandbox` that set it: the restricted sandbox profile anywhere but on macOS ([process-isolation.md](process-isolation.md#configuration)). |
| `RESOURCE_EXHAUSTED` | Memory, or a module output beyond the 256 MiB sink limit. |
| `NOT_FOUND` | A module search path that is not a directory, or a configured one that does not exist (warnings). |
| `INPUT_MISSING`, `INTERNAL`, `UNKNOWN` | A defect, usually of the runtime: keep the run record and the log, and report it. An envelope that fails its schema is `INTERNAL` and blames neither the module nor the data. |
| `FAILED` | Contract checks that failed (`praenora modules check`); the check report is in the details. |
| `DATA_INVALID` | Text that is not valid UTF-8 where the runtime writes canonical JSON. |

By exit code: 1 points at a step and its module or at the data, 2 at the command line, 3 at the
printed diagnostics, 4 at a module library, 5 at a step's timeouts, 70 at a defect, 74 at files and
the environment, and 130 at a stop.

## Adding a code

A new code needs an `ErrorCode` enumerator in
[`error.hpp`](../../include/praenora/runtime/error.hpp), a row in the runtime's table in
[`error.cpp`](../../src/runtime/core/error.cpp), a row in this document and, for a general code, a
`PRAENORA_STATUS_*` macro and a name in `praenora_status_name()` in `status.h` together with an ADR
and, once versions are bumped again, an ABI minor version
([compatibility-policy.md](compatibility-policy.md#changing-a-version)).

## References

- [ADR-0013](../adr/ADR-0013-error-taxonomy-and-status-codes.md): error taxonomy and status codes.
- [ADR-0023](../adr/ADR-0023-public-cpp-api.md): exceptions in the C++ API.
- [ADR-0031](../adr/ADR-0031-raise-the-reserved-error-codes.md): where `MESSAGE_TYPE_INCOMPATIBLE`
  and `TARGET_DATA_UNAVAILABLE` are raised.
- [testing.md](../testing.md#triage): triage of failing tests.
