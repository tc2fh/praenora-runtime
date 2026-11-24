# Reproducibility

- **Version:** runtime 0.2.0; document format `run-record` 0.2.0 (records of format 0.1.x are still
  read with the 0.1 schema)
- **Status:** Normative, MVP. Proposed ([ADR-0016](../adr/ADR-0016-reproducibility.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

The runtime removes every source of nondeterminism it controls and records everything that
determines a run's results. Each run has a fingerprint, the hash of what determines its results, and
a run record. This document specifies both, the seeds, the output files, and the determinism rules
for the runtime and for modules.

## Run fingerprint

```text
fingerprint = lowercase hex of SHA-256(canonical_json(reproducibility section))
```

The reproducibility section is computed during validation, before any module is loaded. Two runs
with the same fingerprint used the same runtime, ABI and protocol versions, the same catalog
entries, the same semantic configuration, the same module manifests and library files, the same
service providers (manifests, library files and engines) for the same runtime services, and the
same input data. The first 16 hex digits of the fingerprint prefix every envelope id.

| Key | Content |
|---|---|
| `versions` | `{runtime, abi {major, minor}, pmp}`. |
| `workflow` | `{name, version, digest}` of the workflow definition. |
| `catalog` | `{"<id>@<version>": digest}` for the capability of every step and every message type that the workflow's inputs and outputs and those capabilities' ports use. When a capability and a message type used by one workflow share an `id@version`, both keys take the form `<kind>:<id>@<version>`. |
| `time` | `{unit, start, period}`: the time base. |
| `seed` | The run seed. |
| `ticks` | The tick count, after any override, or `null` when the run lasts until its replay inputs are exhausted. |
| `timeouts` | The run's `{invoke, lifecycle, cancel_grace}`. |
| `inputs` | `{<input>: {source, digest, columns?}}`: the source kind, the SHA-256 of the input file's bytes, and the column mapping when there is one. |
| `services` | `{"<name>@<major>": {minor, provider {name, version, manifest_sha256, library_sha256}, engine}}` for every runtime service a provider serves to the run: the minor the provider offers, the provider's identity and digests, and the `engine` object of its manifest ([runtime-services.md](runtime-services.md#fingerprint)). `{}` when no step's module declares a service that resolved. |
| `steps` | `{<step>: {capability {id, version}, module {name, version, manifest_sha256, library_sha256, sdk_version}, parameters, seed, timeouts, services}}`, where `parameters` are the resolved parameters, `seed` the step seed, `timeouts` the step's effective timeouts and `services` the sorted keys of `services` that the step's module may use. |

A digest is the SHA-256 of a catalog entry's canonical JSON, of a file's bytes (module and provider
manifests, libraries, input files). `sdk_version` is the one the manifest declares; a module whose
`describe()` reports another is refused when it is loaded
([manifest.md](manifest.md#identity-check)). Durations are written canonically (see
[time-cancellation-timeouts.md](time-cancellation-timeouts.md#durations-in-the-configuration)).

What changes the fingerprint, and what does not:

- It changes with the seed, the tick count, any timeout, a parameter value, a module's manifest or
  library file, the services a module declares, a provider of the run's services (another
  provider, or its manifest, library file or engine), an input file's bytes (re-encoding a file
  counts), a catalog entry the workflow uses, and the runtime, ABI or PMP version.
- It does not change with paths (of the configuration, the input files, the manifests and the
  libraries), with the `outputs` and `observability` settings, with step log levels, with where
  the steps' module instances run (their isolation: in the runtime's process or in module hosts,
  which give the same results, [process-isolation.md](process-isolation.md#determinism)), or with
  the wall-clock time: the same configuration and data at other paths give the same fingerprint.
- Parameters are hashed after the defaults of the module's parameter schema are filled in, so an
  omitted parameter and an explicit default give the same fingerprint. Numbers keep their JSON type:
  `100` and `100.0` hash differently ([open-questions.md](../open-questions.md), OQ-20).

## Canonical JSON

Canonical JSON is the serialization of nlohmann/json's `dump()` without indentation on objects whose
keys are sorted:

- object keys sorted by their UTF-8 bytes, no insignificant whitespace;
- strings in UTF-8; a string that is not valid UTF-8 is refused (`DATA_INVALID`), never replaced;
- integers as integers; floating-point numbers in the shortest form that reads back to the same
  value, independent of the locale, with `.0` for whole values (`1.0`, `0.54`, `1e+20`).

The form is not claimed to be RFC 8785 (OQ-20). Another implementation that computes fingerprints
MUST produce the same bytes. Canonical JSON is used for the fingerprint, the catalog digests, the
documents passed to modules, the recorded outputs and the message trace.

## Seeds

- The run seed is the configuration's `run.seed`, an integer from 0 to 9007199254740991 (default 0).
  The caller MAY override it (`praenora run --seed`, `WorkflowOptions::seed`); the override is
  recorded.
- Each step gets its own seed:

  ```text
  step_seed = first 8 bytes, big-endian, of SHA-256("praenora-seed-v1" || u64be(run_seed) || utf8(step_id))
              AND (2^53 - 1)
  ```

  For example, run seed 20261110 and step `estimate` give 4059344203658876.
- The step seed reaches the module in the initialize document (`run.seed`) and is recorded in
  `steps.<step>.seed`. A module MUST draw all of its randomness from it.

## Run record

A run record (`kind: run-record`, [`run-record.0.2.schema.json`](../../spec/schemas/run-record.0.2.schema.json);
the runtime writes format 0.2.0, and the 0.1 schema,
[`run-record.0.1.schema.json`](../../spec/schemas/run-record.0.1.schema.json), stays for records of
format 0.1.x, which have no `services` and whose error records have no `location` and no
`details`) describes one run:
`{kind, format_version, fingerprint, reproducibility, execution}`. The
`reproducibility` section is the one hashed; the `execution` section holds facts about the run:

| Field | Content |
|---|---|
| `status` | `running` from the start, then `completed`, `stopped` or `failed`. |
| `started_at`, `finished_at` | Wall-clock instants (`finished_at` is null until the end). |
| `ticks_completed` | The ticks completed. |
| `error` | The failure of a failed run, a `RUN_STOPPED` record for a stopped one, null otherwise: an error record of the status snapshot ([observability.md](observability.md#status-snapshot)), so a failure keeps its location and details, with the run's directories named by placeholders ([observability.md](observability.md#known-directories-in-the-files-of-a-run)). |
| `platform` | `{os, arch, compiler, build_type}` of the runtime build. |
| `overrides` | The run settings the caller overrode (`seed`, `ticks`, and `isolation`, the mode set for every step); `{}` when none. |
| `validation` | `strict`, or `off` for a performance measurement. |
| `manifests` | Where each manifest of the run was found ([below](#where-a-manifest-was-found)): `{steps {<step>: location}, services {"<name>@<major>": location}}`, for the module manifest of every step and the provider manifest of every service in the reproducibility section. |
| `inputs` | What each replay input found in its data: `{<input>: {"rows_total": <count>, "rows_before_start": <count>}}` for every `observation-csv` input (a `json-document` input has no entry): the rows of the file, and those whose `time` is before `run.time.start`, which the replay never delivers ([ADR-0022](../adr/ADR-0022-target-data-intake.md)). Outside the hashed section: both numbers follow from the file's bytes and the configuration, which the fingerprint covers. |
| `isolation` | Where each step's module instance ran, whether its module host shared compiled models, and its sandbox profile: `{<step>: {"mode": "in-process" \| "process", "share_compiled_models": <boolean>, "sandbox": "none" \| "restricted"}}` for every step, with every field resolved ([process-isolation.md](process-isolation.md#configuration)). Outside the hashed section: sharing compiled models and the sandbox profile change no result. |
| `outputs` | `{<output>: {file, sha256, messages}}` for every recorded output, written at the end. |
| `trace` | `{file, sha256, messages}` of the message trace, written at the end when tracing is on; `messages` counts its envelopes ([observability.md](observability.md#message-trace)). |
| `final_status` | The final status snapshot ([observability.md](observability.md#status-snapshot)), written at the end, its error records naming the run's directories by placeholders. |

- The record is written when the run starts, before any module is loaded, and again when it ends,
  each time replacing the file atomically, as JSON indented by two spaces with a final newline.
  Without an output directory it is kept in memory only (`Workflow::run_record_json()`).
- The record names no host and no user. It identifies each module, and each provider, by name,
  version and the digests of its manifest and library, and says where its manifest was found only
  relative to a search path that it names by origin and index (`manifests`). Its error records
  (`error`, and the `last_error`s of `final_status`) name every directory that the run knows (its
  output directory, the configuration's directory, its module and provider search paths and the
  home directory) by a placeholder, in their `message`, `location.file`, `details` and `cause`:
  `<output>/outputs`, `<modules:caller:0>/fx-echo/fx-echo.pmm.json`, `<home>/data/x.csv`
  ([observability.md](observability.md#known-directories-in-the-files-of-a-run) has the
  placeholders and the rules). `Workflow::run_record_json()` returns the same document as the file;
  the run summary and `Workflow::status()` keep absolute paths. An error record still holds a path
  under none of these directories as it is written, and the values that the runtime copies from the
  configuration and from manifests (parameters and their defaults, a provider's `engine` object)
  hold whatever those documents hold, paths included; otherwise the runtime writes no absolute path
  into the record. Neither the placeholders nor anything else in the error records enters the
  reproducibility section, so they never change the fingerprint.
- A writer MUST refuse a fingerprint that is not the hash of the record's reproducibility section.

### Where a manifest was found

A location in `execution.manifests` is `{"search_path": {"origin", "index"}, "file"}`:

- `search_path` is the search path that reached the manifest first
  ([manifest.md](manifest.md#discovery), [runtime-services.md](runtime-services.md#discovery)),
  named by where it comes from, never by its value:
  - `origin` is `caller` (the caller's paths: `RuntimeOptions::module_paths` or `provider_paths`,
    `--module-path` or `--provider-path`, the Python package's paths), `configuration` (the
    configuration's `modules.search_paths`; module manifests only), `environment`
    (`PRAENORA_MODULE_PATH` or `PRAENORA_PROVIDER_PATH`) or `installation` (the installation's
    module or provider directory);
  - `index` is the path's position among the non-empty entries of its origin, as given, counting
    from 0. An entry that repeats an earlier path, and so is not searched again, still counts. The
    installation's directory has no `index`.
- `file` is the manifest file relative to that search path, with `/` as separator on every
  platform: `<file>` for a manifest in the search path itself, `<directory>/<file>` for one in an
  immediate subdirectory (discovery looks no deeper). A manifest reached through two overlapping
  search paths is relative to the first.

Given the run's search paths, a reader can tell which file a run used, even when several copies of
a module or provider are installed; runs whose search paths hold the same files in the same layout
record the same locations wherever the paths are. The locations are outside the reproducibility
section: they never change the fingerprint. `praenora modules inspect` and `praenora providers
list` show the absolute paths on the machine.

## Output files

A run writes files only when it has an output directory (`outputs.directory`, `praenora run
--output-dir`, `WorkflowOptions::output_dir`):

```text
<output directory>/
  run-record.json          the run record
  outputs/<output>.jsonl   one envelope per line, for each recorded workflow output
  trace/messages.jsonl     the message trace, when tracing is on
  status.json              the status file, when one is configured at this path
```

- Each line of an output file and of the trace is the canonical JSON of one record followed by
  `\n`, written in binary mode, so the files are byte-deterministic. Every recorded output has a
  file, even one that receives no message.
- A directory that already holds `run-record.json` holds an earlier run. The runtime refuses it
  (`IO_ERROR`) unless the caller asks for an overwrite (`praenora run --overwrite`,
  `WorkflowOptions::overwrite`), which removes the files Praenora manages there, and only those:
  `outputs/`, `trace/`, `status.json` and, last, `run-record.json`. Everything else stays; log files
  are appended to.
- An output file or a final run record that cannot be written turns a completed or stopped run into
  a failed one (`IO_ERROR`).

## Determinism

The runtime guarantees:

- a deterministic schedule (topological order, ties broken by step id), deterministic routing and
  exactly one module call at a time;
- deterministic envelope ids (`<fp16>:<tick>:<source>`) and logical times;
- ordered C++ containers wherever an order reaches an output, and canonical JSON for everything it
  records;
- no wall-clock time in envelopes, recorded outputs, the trace or the reproducibility section;
- numbers in input data read the same under any locale whose decimal separator is a single byte
  ([ADR-0022](../adr/ADR-0022-target-data-intake.md));
- seeds derived only from the run seed.

A module MUST:

- not depend on the wall clock;
- draw its randomness only from the seed of its initialize document;
- iterate in a deterministic order wherever the order reaches a result;
- have no data races and not read uninitialized memory;
- not let results depend on state shared with other instances
  ([threading-and-ownership.md](threading-and-ownership.md#what-a-module-must-do)).

The contract checks' `invoke.determinism` check runs every test-vector case twice, on new instances,
and requires byte-identical results ([ADR-0024](../adr/ADR-0024-contract-checks-and-test-vectors.md)).

**Guarantee.** The same fingerprint on the same platform with the same runtime build gives
byte-identical outputs and traces. Results that modules compute with a runtime service of a provider
are byte-identical only for the same engine identity: the same provider, library file and engine,
which the fingerprint records (`services`). The roadrunner provider pins libRoadRunner's JIT backend
to MCJit, without LLVM optimization passes, and records it in its engine object (`"jit": "mcjit"`,
`"llvm_optimization": "none"`): libRoadRunner's other backend, LLJit, generates code for the host's
CPU and fuses floating-point operations, so its results can differ between machines
([simulation-service.md](simulation-service.md#the-roadrunner-provider)). Whether MCJit's code is
the same on every CPU of one architecture is not yet verified. Across platforms and compilers,
results agree only within a numeric tolerance, because floating-point contraction and the math
library differ. The example modules are compiled with `-ffp-contract=off` on GCC and Clang to narrow
that gap, and the example's golden outputs are compared with a relative tolerance of 1e-9 and an
absolute one of 1e-12.

## References

- [ADR-0016](../adr/ADR-0016-reproducibility.md): reproducibility.
- [ADR-0020](../adr/ADR-0020-message-envelope-and-asyncapi.md): envelope ids, recorded outputs and the trace.
- [ADR-0022](../adr/ADR-0022-target-data-intake.md): input data and its digests.
- [ADR-0026](../adr/ADR-0026-runtime-service-providers.md): service providers and their engines in
  the fingerprint.
- [ADR-0029](../adr/ADR-0029-run-record-says-where-manifests-were-found.md): where manifests were
  found.
- [RFC 8785 (JSON Canonicalization Scheme)](https://www.rfc-editor.org/rfc/rfc8785).
- [user-guide.md](../user-guide.md#what-a-run-writes): the files of a run.
