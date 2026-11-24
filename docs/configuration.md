# Workflow configuration reference

A workflow configuration is the document a user writes to run a workflow. It pins one workflow
definition of the protocol catalog, binds each of its steps to a module and parameters, binds each
of its inputs to data, and sets run, output and logging options. It never says how steps connect:
connections come from the workflow definition
([specs/workflow-definitions.md](specs/workflow-definitions.md)).

- **Kind and format:** `kind: workflow-config`, `format_version: "0.1.0"`.
- **Schema:** [`spec/schemas/workflow-config.0.1.schema.json`](../spec/schemas/workflow-config.0.1.schema.json)
  (JSON Schema draft-07), embedded in the runtime: `praenora schema show workflow-config`.
- **Example:** [`examples/workflows/estimate-predict/workflow.yaml`](../examples/workflows/estimate-predict/workflow.yaml),
  which sets every section below.

`praenora validate <file>` checks a configuration without running it; [user-guide.md](user-guide.md)
shows the commands.

## A small configuration

Only `kind`, `format_version`, `workflow` and `steps` are required. This configuration runs the
example workflow's definition with the modules' default parameters, feeding the same observation
batch to every tick from a JSON document:

```yaml
kind: workflow-config
format_version: "0.1.0"
workflow: { name: example.estimate-predict, version: "0.1.0" }
run:
  ticks: 2
  time: { unit: d }
inputs:
  observations: { source: json-document, path: batch.json }
steps:
  estimate:
    module: { name: example-estimator, version: "0.1.0" }
  predict:
    module: { name: example-predictor, version: "0.1.0" }
```

`batch.json`, next to the configuration, holds one `pmp.observation-batch`:

```json
{
  "window": { "start": 0.0, "end": 1.0 },
  "time_unit": "d",
  "observations": [
    { "series_id": "S01", "variable_id": "population", "time": 0.5, "value": 12.0 }
  ]
}
```

Without `run.ticks` this configuration is invalid (`CONFIG_INVALID` at `/run/ticks`): no input
replays data, so nothing else would end the run.

## Document rules

A configuration is YAML or JSON in UTF-8; a byte order mark and CRLF line endings are accepted. YAML
is read as a subset that maps exactly onto JSON:

- One document per file. Anchors and aliases are allowed; merge keys (`<<`), duplicate keys and keys
  that are not strings are errors.
- Plain scalars follow the YAML 1.2 core schema. The booleans are `true`, `True`, `TRUE`, `false`,
  `False` and `FALSE`, so `yes`, `no`, `on`, `off` and other spellings such as `tRUE` are strings;
  null is `null`, `Null`, `NULL`, `~` or an empty value. `.inf` and `.nan`, in each of their
  spellings, are refused because JSON cannot hold them. Quote version numbers (`"0.1.0"`) and other
  values that must stay strings.
- A file whose name ends in `.json` is read as strict JSON.
- Limits: 16 MiB per document and a nesting depth of 128. The data files of the inputs have
  limits of their own: 256 MiB for an `observation-csv` file, and those of a document for a
  `json-document` ([`inputs`](#inputs)).
- Unknown keys are errors, except top-level keys that start with `x-`, which are ignored.
- A configuration is data: nothing in it is evaluated, and no variable or shell syntax is expanded,
  so `${...}` is plain text and a tag such as `!!python/object` is refused (`PARSE_ERROR`); paths are
  not expanded by a shell either ([Paths](#paths)).

Every problem is reported with the file, line, column and JSON pointer, for example
`workflow.yaml:29:25: error[PARAMETERS_INVALID]: /steps/estimate/parameters/gain: ...`.

## Sections

### Top level

| Key | Required | Content |
|---|---|---|
| `kind` | yes | `workflow-config`. |
| `format_version` | yes | `"0.1.0"`; another version is `FORMAT_VERSION_UNSUPPORTED`. |
| `workflow` | yes | The workflow definition to run. |
| `run` | no | Seed, ticks, time base and timeouts. |
| `modules` | no | Extra module search paths. |
| `inputs` | no | One binding per workflow input of the definition. |
| `steps` | yes | One binding per step of the definition. |
| `outputs` | no | Where the run writes its files, and what. |
| `observability` | no | Logging and the status file. |
| `x-...` | no | Extension keys, ignored. |

### `workflow`

| Key | Type | Content |
|---|---|---|
| `name` | catalog id | The workflow definition's name, for example `example.estimate-predict` (`praenora catalog list --kind workflows`). |
| `version` | semantic version string | Its exact version. A name or version the catalog does not hold is `WORKFLOW_NOT_FOUND`. |

### `run`

| Key | Type | Default | Content |
|---|---|---|---|
| `seed` | integer, 0 to 2^53 - 1 | `0` | The run seed. Each step's seed is derived from it and the step id ([specs/reproducibility.md](specs/reproducibility.md#seeds)). |
| `ticks` | integer, 0 to 2^53 - 1 | none | The number of ticks to run. Without it, the run lasts until every replay input is exhausted; a configuration without a replay input must set it (`CONFIG_INVALID`). |
| `time.unit` | non-empty string | `"1"` | The label of the logical-time unit, for example `d`. Units are never converted. |
| `time.start` | number | `0.0` | The logical time where tick 0 starts. |
| `time.period` | number above 0 | `1.0` | The length of a tick in logical time: tick `t` covers `[start + t * period, start + (t + 1) * period)`. |
| `timeouts.invoke` | duration | `300s` | The deadline of each `invoke` call. |
| `timeouts.lifecycle` | duration | `60s` | The deadline of every other module call (create, initialize, finalize, destroy). |
| `timeouts.cancel_grace` | duration | `5s` | How long a call may take to return once its deadline passed and it was asked to cancel. A call that returns within it fails with `STEP_TIMEOUT`; one that does not makes the instance unresponsive (`MODULE_UNRESPONSIVE`). |
| `isolation.mode` | `in-process` or `process` | `in-process` | Where every step's module instance runs, unless the step sets its own ([Isolation](#isolation)). |
| `isolation.share_compiled_models` | boolean | `false` | Whether every step's module host shares compiled models with the other sharing steps', unless the step sets its own ([Isolation](#isolation)). |
| `isolation.sandbox` | `none` or `restricted` | `none` | The sandbox profile of every step's module host, unless the step sets its own ([Isolation](#isolation)). |

### `modules`

| Key | Type | Content |
|---|---|---|
| `search_paths` | list of paths | More directories searched for module manifests, in the place the [search order](specs/manifest.md#discovery) gives them. Relative paths resolve against the configuration file's directory. |

### `inputs`

One entry per workflow input of the definition, under the input's name. A workflow input that is not
bound, or a binding for an input the definition does not declare, is `INPUT_BINDING_INVALID`.

| Key | Type | Content |
|---|---|---|
| `source` | `observation-csv` or `json-document` | The kind of data source. |
| `path` | path | The data file, relative to the configuration file's directory. |
| `columns` | map of strings | `observation-csv` only: observation column name to the CSV header it is read from, for example `{ time: t, value: y }`. |

The input's message type, from the definition, decides what the data must hold; for the example's
`observations` input it is `pmp.observation-batch`. Each source's file is read and checked during
validation, and its SHA-256 is part of the run fingerprint.

#### `observation-csv`

The source replays a long-form observation table: each tick's batch holds the rows whose `time`
lies in the tick's window (`start <= time < end`), stably sorted by time, series, variable,
observation id and position in the file. An empty batch is still delivered. The source is
exhausted once a window starts after the last row's time.

The file is UTF-8 CSV with a header row (RFC 4180; a byte order mark and CRLF line endings are
accepted, empty lines skipped, fields not trimmed), at most 256 MiB (268435456 bytes): a larger file
is refused before it is read (`TARGET_DATA_INVALID`, naming the file). Its columns are those of the
observation rows of [specs/protocol.md](specs/protocol.md#message-types), the column names of
`ode-uq-spec`:

| Column | Required | Cell |
|---|---|---|
| `series_id`, `variable_id` | yes | Non-empty text. |
| `time` | yes | A decimal number, in the time base's unit. |
| `value` | yes | A decimal number; empty only when `missing` is true, or the row is censored with its limit. |
| `observation_id`, `unit`, `group_id`, `batch_id` | no | Text; an empty cell means absent. |
| `missing` | no | `true` or `false`, in any letter case; empty means `false`. `true` makes the value `null`. |
| `censoring` | no | `none` (default), `left`, `right` or `interval`. |
| `lower_limit`, `upper_limit` | no | Decimal numbers. |
| `weight` | no | A number of at least 0; default 1. |

Rows whose `time` is before the run's `time.start` lie in no window and are never delivered;
validation counts them and reports one warning for the input (`TARGET_DATA_INVALID`, severity
warning, at the first of them in the file; the configuration stays valid), and the run record's
`execution.inputs.<input>` holds the counts
([specs/reproducibility.md](specs/reproducibility.md#run-record)).

Other columns are ignored. Numbers are read the same way whatever the process's locale; infinities,
NaN and surrounding spaces are refused. A row that breaks a rule is `TARGET_DATA_INVALID`, naming
the file and line; a data file that does not exist, is not a file or cannot be read (a directory on
its path that cannot be searched included) is `TARGET_DATA_UNAVAILABLE`, for either kind of source.
With `columns`, a column is read from the header named instead of its own name; a key that is not an
observation column is `CONFIG_INVALID`. No two columns may be read from one header, counting the
columns that are not remapped, which read the header of their own name:
`{ time: value, value: value }` and `{ value: time }` are each one `CONFIG_INVALID`, at the entry
that causes the clash (of two entries that name one header, the one whose column name sorts later).
An input whose `path` or `columns` has an error is not read. The example's data file,
[`data/observations.csv`](../examples/workflows/estimate-predict/data/observations.csv), uses the
columns directly.

#### `json-document`

The source delivers the same JSON or YAML document every tick, for static inputs. The document is
validated once against the input's message type (`INPUT_BINDING_INVALID` with every problem in the
details). It never replays: a configuration whose inputs are all `json-document` must set
`run.ticks`.

### `steps`

One entry per step of the definition, under the step's id: no more and no fewer
(`BINDING_INVALID`).

| Key | Type | Default | Content |
|---|---|---|---|
| `module.name` | module name | required | The module bound to the step. |
| `module.version` | semantic version string | required | Its exact version (versions are pinned, never ranges). The module's manifest must declare the step's capability at the definition's version (`CAPABILITY_NOT_IMPLEMENTED`). |
| `parameters` | object | `{}` | The module's parameters, validated against the `parameters_schema` of its manifest, whose defaults are filled in (`PARAMETERS_INVALID`). The resolved parameters are part of the run fingerprint. |
| `timeouts` | `invoke`, `lifecycle`, `cancel_grace` | the run's | The step's own deadlines, put over the run's field by field. |
| `log_level` | log level | none | The level of the step's module logger, `module.<step>`, whatever the run's level. |
| `isolation.mode` | `in-process` or `process` | the run's | Where the step's module instance runs ([Isolation](#isolation)). |
| `isolation.share_compiled_models` | boolean | the run's | Whether the step's module host shares compiled models ([Isolation](#isolation)). |
| `isolation.sandbox` | `none` or `restricted` | the run's | The sandbox profile of the step's module host ([Isolation](#isolation)). |

### Isolation

A step's module instance runs in the runtime's process (`in-process`, the default) or in a module
host, a process of its own (`process`): a crash of the module (a segmentation fault, an `abort()`,
an `exit()`) then fails the step with `MODULE_CRASHED` instead of ending the runtime, and a call
that never returns is ended for good by killing the module host
([specs/process-isolation.md](specs/process-isolation.md)). Modules do not change, and the results
do not either: a run gives the same outputs, message trace and fingerprint in both modes.

```yaml
run:
  isolation: { mode: process }     # every step in a module host of its own ...
steps:
  estimate:
    module: { name: example-estimator, version: "0.1.0" }
    isolation: { mode: in-process } # ... except this one
```

- `isolation` is an object at `run.isolation` (the default of every step) and at
  `steps.<id>.isolation` (that step). Each field is resolved on its own: the step's, else the
  run's, else the default. An unknown field or mode is `SCHEMA_VIOLATION`.
- `--isolation` (`WorkflowOptions::isolation`, Python's `isolation=`) sets every step's mode for one
  run, in place of the file's; `in-process` also turns every step's `share_compiled_models` off
  and sets its `sandbox` to `none`.
- Validation refuses a step in process mode (`UNSUPPORTED`, located at the `isolation.mode` field
  that set it) on Windows, where process isolation is not available in this version.
- A step in process mode has the service providers of its module's services loaded in its module
  host; the runtime's process loads only those of the steps that run in it
  ([specs/process-isolation.md](specs/process-isolation.md#service-providers)).
- The run record's `execution.isolation` lists every step's resolved settings; they never enter
  the run fingerprint.

#### Shared compiled models

Steps in process mode that set `share_compiled_models: true` share the models their providers
compile: a provider in one step's module host keeps a model it compiled in a store in the
runtime's process, and the same provider in another sharing step's module host loads it from there
instead of compiling it again
([specs/process-isolation.md](specs/process-isolation.md#shared-compiled-models)). The provider
`roadrunner` keeps a compiled SBML model this way, so a run of many steps on one large model
compiles it once. The results do not change.

```yaml
run:
  isolation: { mode: process, share_compiled_models: true }
```

- It applies only to a step that runs in a module host: validation refuses it on a step in mode
  `in-process` (`CONFIG_INVALID`, located at the field that set it).
- The store is in memory only and lasts as long as the runtime's process: at most 128 MiB an
  entry and 512 MiB in all, the least recently used entry out first. A step that does not share
  never reads it, and its providers never see it.
- **Trust.** A module host that shares can hand every other sharing step of the process a compiled
  model of its choosing, and for `roadrunner` that is native code the other module host runs. The
  providers check an entry before they use one (`roadrunner`'s envelope: its engine, length and
  SHA-256), which finds an entry that is corrupt or cut short, never one made on purpose; one that
  does not load is compiled again, with a warning. Share only between steps whose modules you trust
  alike. It is off by default.

#### The restricted sandbox profile

On macOS, a step in process mode with `sandbox: restricted` has its module host apply a sandbox
profile to itself before it loads any library: the module and its providers may not use the
network, may not start processes (`fork`, `exec`, `posix_spawn`, nor through launchd,
LaunchServices or Apple events), and may not write files anywhere but in the module host's own
temporary directory (which `TMPDIR` names) and to `/dev/null`. They may read files, load
libraries and generate code (the provider `roadrunner` compiles its models as usual), and their
records and outputs reach the run as without the profile
([specs/process-isolation.md](specs/process-isolation.md#the-restricted-profile)). The results do
not change, for a module that does nothing the profile refuses.

```yaml
steps:
  estimate:
    module: { name: example-estimator, version: "0.1.0" }
    isolation: { mode: process, sandbox: restricted }
```

- It applies only to a step that runs in a module host: validation refuses it on a step in mode
  `in-process` (`CONFIG_INVALID`), and on Linux and Windows, which have no such profile in this
  version (`UNSUPPORTED`), each located at the field that set it.
- What the profile refuses fails with `EPERM` (`Operation not permitted`) in the module; a module
  that needs it fails as it fails for any refused system call
  ([module-developer-guide.md](module-developer-guide.md)).
- A runtime that runs in a sandbox already (another program's) cannot give a module host the
  profile, since macOS applies no second sandbox: the step's load fails with
  `MODULE_HOST_UNAVAILABLE`, `details.reason` `sandbox`, "Operation not permitted".
- The profile limits what the module host's own process does. It is not a boundary against a
  module meant to do harm: the system services a module can still reach over Mach IPC may act for
  it. Run only modules you trust as code in your own process, profile or not.
- The temporary directory is removed when the module host ends; what a module made impossible to
  remove there (an immutable file) is left, with a warning (`module_host.directory_left`). A
  runtime that is killed leaves its module hosts' directories, until the system cleans its
  temporary directory.
- The run record's `execution.isolation.<step>.sandbox` says which profile each step had.

### `outputs`

| Key | Type | Default | Content |
|---|---|---|---|
| `directory` | path or `null` | `null` | Where the run writes its run record, outputs and trace; `null` writes nothing. `--output-dir` (or `WorkflowOptions::output_dir`) wins. A directory that already holds a run record is refused unless overwrite is requested. |
| `record` | list of workflow output names | every output | The workflow outputs written to `outputs/<output>.jsonl`; an empty list records none, an unknown name is `CONFIG_INVALID`. |
| `trace_messages` | boolean | `false` | Write every envelope into and out of a step, and the runtime services each call into a step's module looked up, to `trace/messages.jsonl`. `--trace` turns it on. Without an output directory nothing is traced, and a warning says so. |

The files are described in [user-guide.md](user-guide.md#what-a-run-writes).

### `observability`

| Key | Type | Default | Content |
|---|---|---|---|
| `log_level` | `trace`, `debug`, `info`, `warn`, `error`, `critical` or `off` | `info` | The run's log level. |
| `log_format` | `text` or `json` | `text` | Text lines, or JSON lines with OpenTelemetry field names. |
| `log_file` | path or `null` | `null` | Also log to this file (appended to; its directories are created). It must not lie among the files the output directory manages (`CONFIG_INVALID`). |
| `status_file` | path or `null` | `null` | Keep a status snapshot in this file, replaced atomically ([user-guide.md](user-guide.md#status-and-health)). |
| `status_interval` | duration | `1s` | How often the status file is rewritten besides every state change; `0` or `none` rewrites it only on changes. |

## Durations

A duration is a string `<number><unit>` with the unit `ns`, `us`, `ms`, `s`, `m` or `h`, such as
`30s`, `1.5m` or `250ms`; `none`, `0` or any zero value such as `0s` disables a timeout. The number
is decimal, without spaces, and the unit lower case (`SCHEMA_VIOLATION` otherwise); a duration that
is not a whole number of nanoseconds, such as `1.5ns`, or that does not fit in 64 bits is
`CONFIG_INVALID`. See [specs/time-cancellation-timeouts.md](specs/time-cancellation-timeouts.md).

## Paths

Paths are UTF-8 and may contain spaces and other characters; they are never expanded by a shell.
Relative paths in the file (input paths, `modules.search_paths`, `outputs.directory`, `log_file`,
`status_file`) resolve against the configuration file's directory; paths given on the command line
or through the API resolve against the current directory. The run fingerprint does not depend on
paths, so a configuration and its data moved elsewhere run with the same fingerprint.

## Overrides and precedence

A setting given by the caller wins over the file, which wins over the defaults:

| Setting | `praenora run` | C++ `WorkflowOptions` | File |
|---|---|---|---|
| Seed | `--seed S` | `seed` | `run.seed` |
| Ticks | `--ticks N` | `ticks` | `run.ticks` |
| Output directory | `--output-dir D` | `output_dir` | `outputs.directory` |
| Replace an earlier run | `--overwrite` | `overwrite` | |
| Message trace | `--trace` | `trace_messages` | `outputs.trace_messages` |
| Status file | `--status-file F` | `status_file` | `observability.status_file` |
| Isolation of every step | `--isolation M` | `isolation` | `run.isolation`, `steps.<id>.isolation` |

The logging settings (level, format and file) are resolved each on its own, the level and the
format also from the environment; their sources and order are specified in
[specs/observability.md](specs/observability.md#settings). Module search paths and catalog paths add
up instead of replacing each other: the caller's module paths (`--module-path`,
`RuntimeOptions::module_paths`), the configuration's and the environment's are all searched, in the
[search order](specs/manifest.md#discovery), and extra catalog entries come from `--catalog-path`
(`RuntimeOptions::catalog_paths`) and `PRAENORA_CATALOG_PATH`. Service providers are found on the
caller's provider paths (`--provider-path`, `RuntimeOptions::provider_paths`), then
`PRAENORA_PROVIDER_PATH`, then the installation's provider directory
([search order](specs/runtime-services.md#discovery)); a configuration names none. The seed, tick
and isolation overrides are listed in the run record's `execution.overrides`. In Python the same
overrides are the arguments of `Workflow` ([python.md](python.md)).

## Environment variables

| Variable | Read by | Effect |
|---|---|---|
| `PRAENORA_MODULE_PATH` | Discovery | More module search paths, `:`-separated (`;` on Windows), in their place in the [search order](specs/manifest.md#discovery). |
| `PRAENORA_PROVIDER_PATH` | Provider discovery | More service provider search paths, separated the same way, after the caller's provider paths and before the installation's provider directory ([search order](specs/runtime-services.md#discovery)). |
| `PRAENORA_CATALOG_PATH` | The catalog | More catalog directories, separated the same way; directories that do not exist are skipped. |
| `PRAENORA_LOG_LEVEL` | Logging | The log level ([precedence](specs/observability.md#settings)). |
| `PRAENORA_LOG_FORMAT` | Logging | The log format ([precedence](specs/observability.md#settings)). |
| `PRAENORA_DEBUG_KEEP_LIBRARIES` | The loader | `1` keeps every module library loaded until the process ends ([developer-guide.md](developer-guide.md#keeping-module-libraries-loaded)). |

The first five are read only when `RuntimeOptions::use_environment` is set (the default): when a
`Runtime` is constructed, and `PRAENORA_MODULE_PATH` again when a configuration that lists
`modules.search_paths` is validated. An empty variable counts as unset, and an invalid log level or
format is `CONFIG_INVALID`, naming the variable. `PRAENORA_DEBUG_KEEP_LIBRARIES` is read whenever a
module library is loaded, whatever `use_environment` says.

## What changes the run fingerprint

The fingerprint covers the semantic configuration: the workflow definition, the catalog entries it
uses, the seed, the tick count, the time base, the timeouts, each step's module (manifest and
library digests), resolved parameters, seed and the runtime services it may use, the service
providers that serve those services (each provider's identity, manifest and library digests and
engine), and each input's source kind, column mapping and file digest. The `outputs` and
`observability` sections, step log levels, the steps' isolation and every path are left out. The
full list is in [specs/reproducibility.md](specs/reproducibility.md#run-fingerprint).

## Validation

`praenora validate`, `Runtime::validate()` and every run check a configuration in this order,
collecting every problem; a later step is skipped only when it depends on one that failed:

1. Parse the file (`PARSE_ERROR`), check its kind and format version (`DOCUMENT_KIND_MISMATCH`,
   `FORMAT_VERSION_UNSUPPORTED`) and its schema (`SCHEMA_VIOLATION`), then the values the schema
   cannot check (`CONFIG_INVALID`).
2. Resolve the workflow definition (`WORKFLOW_NOT_FOUND`) and the capabilities and message types it
   uses (`CATALOG_ENTRY_NOT_FOUND`), and validate its graph.
3. Check the bindings of steps (`BINDING_INVALID`) and inputs (`INPUT_BINDING_INVALID`).
4. Find and check each step's module from its manifest (`MODULE_NOT_FOUND`, `MANIFEST_INVALID`,
   `PLATFORM_MISMATCH`, `ABI_INCOMPATIBLE`, `PROTOCOL_INCOMPATIBLE`, `CAPABILITY_NOT_IMPLEMENTED`,
   `LIBRARY_NOT_FOUND`, `LIBRARY_DIGEST_MISMATCH`), without loading it, and whether a step in
   process mode can run in a module host (`UNSUPPORTED`).
5. Validate the parameters and fill in their defaults (`PARAMETERS_INVALID`).
6. Read and check the input data (`TARGET_DATA_UNAVAILABLE`, `TARGET_DATA_INVALID`).
7. Compute the digests and the run fingerprint.

With `--load` (and in every run), the module libraries are then loaded and checked
(`LIBRARY_LOAD_FAILED`, `ENTRY_POINT_MISSING`, `API_TABLE_INVALID`, `MODULE_IDENTITY_MISMATCH`),
each where its steps run: in the runtime's process, or in a module host for a step in process mode
(`MODULE_HOST_UNAVAILABLE` when none can be started, `MODULE_CRASHED` when it ends while it loads
the library). The
[error taxonomy](specs/error-taxonomy.md#triage) says where to look for each code.
