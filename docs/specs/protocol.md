# Praenora Module Protocol (PMP)

- **Version:** PMP 0.1.0, with document formats 0.2.0 (`module-manifest`, `run-record`) and 0.1.0
  (every other kind)
- **Status:** Normative, MVP. The decisions behind it are Proposed unless the linked ADR says
  otherwise.
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

PMP defines the documents the runtime reads and writes, the message envelope, the invocation and
result that a module exchanges with the runtime, the protocol catalog (message types, capabilities
and workflow definitions) and the execution semantics. Its C binding is the
[module ABI](module-abi.md). The module manifest (PMM) is specified in [manifest.md](manifest.md),
the rules of workflow graphs in [workflow-definitions.md](workflow-definitions.md), and versioning in
[compatibility-policy.md](compatibility-policy.md).

## Documents and schemas

Every Praenora document is a JSON object validated against a JSON Schema before it is used. The
schemas of the runtime ([`spec/schemas/`](../../spec/schemas/) and the data schemas of the catalog's
message types) are JSON Schema 2020-12; a module's `parameters_schema` and a message type from an
extra catalog directory may also be draft-07 ([manifest.md](manifest.md)) ([ADR-0043](../adr/ADR-0043-json-schema-dialects-and-validator.md),
which replaces [ADR-0008](../adr/ADR-0008-json-schema-draft-07-embedded-schemas.md)):

- A document that has a `kind` carries it with a constant value per schema, together with
  `format_version`, a semantic version. The module-ABI documents (`pmp-*`) have no `kind`; the
  documents the runtime writes for modules carry `pmp_version` instead.
- Each pair of kind and `major.minor` has exactly one schema, the file
  `spec/schemas/<kind>.<major>.<minor>.schema.json`, whose `$id` is
  `urn:praenora:schema:<kind>:<major>.<minor>`. A document is validated against the schema of the
  major and minor of its version; patch, pre-release and build parts are ignored. A version that no
  schema serves is `FORMAT_VERSION_UNSUPPORTED`, and the message names the formats the runtime reads.
- The schemas are self-contained (they reference no other schema file) and embedded in the runtime;
  the runtime never reads a schema from disk. The same files are installed to
  `share/praenora/schemas/`.
- Every document's top level accepts extension properties whose names start with `x-` and rejects
  every other property its schema does not list. Nested objects accept only what their schema lists.
- Documents are UTF-8. A file whose name ends in `.json` MUST be strict JSON. Other files (the
  workflow configuration, `json-document` inputs) MAY be YAML, read with the YAML 1.2 core schema: a
  single document, no merge keys, no tags other than the standard scalar tags, no `.inf` or `.nan`,
  no duplicate keys. A document file is limited to 16 MiB, nesting to a depth of 128, and a YAML
  document to 2^18 values after alias expansion; anything beyond is `PARSE_ERROR`
  ([ADR-0010](../adr/ADR-0010-workflow-definitions-as-catalog-data.md)).
- Documents are data: the runtime parses and validates them, and never evaluates any part of one.
  Message data reaches modules as it is: a model source, an expression or an `href` in it is for the
  receiving module to interpret, and the runtime neither evaluates it nor fetches what it names.
- Integers that count or seed (ticks, seeds) lie in [0, 9007199254740991], so every JSON
  implementation reads them exactly.
- `praenora validate --document <file>` validates any document: the kind comes from `kind`; a
  document without one is a module-ABI document of the one `pmp-*` kind whose required fields it has
  (`DOCUMENT_KIND_MISMATCH` when none or several fit), validated by its `pmp_version`.

| Schema file | Kind | Version field | Document | Specified in |
|---|---|---|---|---|
| [`capability-definition.0.1.schema.json`](../../spec/schemas/capability-definition.0.1.schema.json) | `capability-definition` | `format_version` | A catalog entry: the typed ports of a capability. | [workflow-definitions.md](workflow-definitions.md) |
| [`message-type.0.1.schema.json`](../../spec/schemas/message-type.0.1.schema.json) | `message-type` | `format_version` | A catalog entry: a message type and the schema of its data. | [Catalog](#catalog) |
| [`module-check-report.0.1.schema.json`](../../spec/schemas/module-check-report.0.1.schema.json) | `module-check-report` | `format_version` | The result of the contract checks (`praenora modules check`). | [ADR-0024](../adr/ADR-0024-contract-checks-and-test-vectors.md) |
| [`module-manifest.0.1.schema.json`](../../spec/schemas/module-manifest.0.1.schema.json) | `module-manifest` | `format_version` | A module manifest (PMM) of format 0.1.x, read but no longer written. | [manifest.md](manifest.md) |
| [`module-manifest.0.2.schema.json`](../../spec/schemas/module-manifest.0.2.schema.json) | `module-manifest` | `format_version` | A module manifest (PMM) of format 0.2.x: the runtime services it requires or can use (`requires`). | [manifest.md](manifest.md) |
| [`module-test-vectors.0.1.schema.json`](../../spec/schemas/module-test-vectors.0.1.schema.json) | `module-test-vectors` | `format_version` | A module's inputs and expected outputs for the contract checks. | [ADR-0024](../adr/ADR-0024-contract-checks-and-test-vectors.md) |
| [`pmp-create.0.1.schema.json`](../../spec/schemas/pmp-create.0.1.schema.json) | `pmp-create` | `pmp_version` | The create document. | [Create document](#create-document) |
| [`pmp-describe.0.1.schema.json`](../../spec/schemas/pmp-describe.0.1.schema.json) | `pmp-describe` | `pmp_version` (the module's) | The identity a module reports through `describe()`. | [Describe document](#describe-document) |
| [`pmp-envelope.0.1.schema.json`](../../spec/schemas/pmp-envelope.0.1.schema.json) | `pmp-envelope` | `pmp_version` | A message envelope. | [Envelope](#envelope) |
| [`pmp-error.0.1.schema.json`](../../spec/schemas/pmp-error.0.1.schema.json) | `pmp-error` | none | A module's error document. | [Error document](#error-document) |
| [`pmp-initialize.0.1.schema.json`](../../spec/schemas/pmp-initialize.0.1.schema.json) | `pmp-initialize` | `pmp_version` | The initialize document. | [Initialize document](#initialize-document) |
| [`pmp-invocation.0.1.schema.json`](../../spec/schemas/pmp-invocation.0.1.schema.json) | `pmp-invocation` | `pmp_version` | The invocation document of one tick. | [Invocation document](#invocation-document) |
| [`pmp-result.0.1.schema.json`](../../spec/schemas/pmp-result.0.1.schema.json) | `pmp-result` | `pmp_version` (optional) | The result document of one invocation. | [Result document](#result-document) |
| [`provider-manifest.0.1.schema.json`](../../spec/schemas/provider-manifest.0.1.schema.json) | `provider-manifest` | `format_version` | A service provider's manifest. | [runtime-services.md](runtime-services.md#provider-manifest) |
| [`run-record.0.1.schema.json`](../../spec/schemas/run-record.0.1.schema.json) | `run-record` | `format_version` | The record of one run, format 0.1.x (written by runtime 0.1). | [reproducibility.md](reproducibility.md) |
| [`run-record.0.2.schema.json`](../../spec/schemas/run-record.0.2.schema.json) | `run-record` | `format_version` | The record of one run, format 0.2.x: with the services of the run, error records with their `location` and `details`, where each manifest was found (`execution.manifests`), and where each step's module instance ran, whether it shared compiled models, and its sandbox profile (`execution.isolation`, [process-isolation.md](process-isolation.md#configuration)). | [reproducibility.md](reproducibility.md) |
| [`status-snapshot.0.1.schema.json`](../../spec/schemas/status-snapshot.0.1.schema.json) | `status-snapshot` | `format_version` | The status of a run at one moment. | [observability.md](observability.md) |
| [`workflow-config.0.1.schema.json`](../../spec/schemas/workflow-config.0.1.schema.json) | `workflow-config` | `format_version` | A workflow configuration (YAML or JSON). | [ADR-0010](../adr/ADR-0010-workflow-definitions-as-catalog-data.md) |
| [`workflow-definition.0.1.schema.json`](../../spec/schemas/workflow-definition.0.1.schema.json) | `workflow-definition` | `format_version` | A catalog entry: a workflow definition. | [workflow-definitions.md](workflow-definitions.md) |

## Identifiers

| Identifier | Pattern | Examples |
|---|---|---|
| Module names and catalog ids (message types, capabilities, workflow definitions) | `^[a-z][a-z0-9-]*(\.[a-z][a-z0-9-]*)*$` | `example-estimator`, `pmp.observation-batch` |
| Step ids, port names, workflow input and output names | `^[a-z][a-z0-9_]*$`, except the reserved `inputs` and `outputs` | `estimate`, `model_set` |
| Endpoints | `<step>.<port>`, `inputs.<name>` or `outputs.<name>` | `estimate.state`, `inputs.observations` |
| Versions | [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html) | `0.1.0` |

## Envelope

A message travels between endpoints in an envelope (`pmp-envelope`), modeled on CloudEvents
([ADR-0020](../adr/ADR-0020-message-envelope-and-asyncapi.md)):

| Field | Type | Value |
|---|---|---|
| `pmp_version` | string | The runtime's PMP version. |
| `id` | string | `<fp16>:<tick>:<source>`, where `<fp16>` is the first 16 hex digits of the run fingerprint. Deterministic and unique within a run. |
| `type`, `type_version` | strings | The message type of the producing port: catalog id and version. |
| `source` | string | The producing endpoint: `<step>.<port>` for a step output, `inputs.<name>` for a workflow input. |
| `tick` | integer | The tick that produced the message. |
| `logical_time` | number | The logical time of that tick: the end of its window. |
| `data` | any JSON value | The payload, which satisfies the schema of the message type. |

- The runtime builds every envelope; a module returns only the `data` of each output port and MUST
  NOT expect its own fields to reach the envelope.
- An envelope MUST NOT carry wall-clock time. Wall-clock instants belong to logs, status snapshots
  and the `execution` section of the run record.
- With strict validation (the default) the runtime checks every envelope it builds against
  `pmp-envelope`. Only a defect of the runtime can break one, so a violation is `INTERNAL`.

```json
{
  "data": {"estimates": [{"basis": "observation", "series_id": "S01", "time": 0.0, "value": 10.0, "variable_id": "population"}], "time_unit": "d"},
  "id": "4664e72704c1c728:0:estimate.state",
  "logical_time": 1.0,
  "pmp_version": "0.1.0",
  "source": "estimate.state",
  "tick": 0,
  "type": "example.state-estimate",
  "type_version": "0.1.0"
}
```

## Module-ABI documents

The ABI functions that carry these documents are specified in [module-abi.md](module-abi.md). The
runtime writes the documents it passes in as canonical JSON; a module MUST read them as ordinary
JSON.

### Describe document

`describe()` returns the module's identity (`pmp-describe`):

| Field | Rule |
|---|---|
| `name` | The module name. MUST equal the manifest's `module.name`. |
| `version` | The module version. MUST equal the manifest's `module.version` (SemVer precedence). |
| `abi` | `{major, minor}`: the ABI the module was built for. Both MUST equal the manifest's `abi`, as integers (a `-0` is 0). |
| `sdk_version` | The SDK the module was built with. MUST equal the manifest's `sdk_version` (SemVer precedence); it decides no compatibility. |
| `pmp_version` | The PMP version the module implements. MUST equal the manifest's `pmp_version` (SemVer precedence); compatibility is decided from the manifest. |
| `capabilities` | `[{id, version}]`. MUST equal the manifest's capabilities as a set. |

### Create document

`create()` receives (`pmp-create`):

| Field | Value |
|---|---|
| `pmp_version` | The runtime's PMP version. |
| `step` | The step id. |
| `capability` | `{id, version}`: the capability the step requires. |
| `module` | `{name, version}`: the module bound to the step. |
| `parameters` | The step's parameters, validated against the manifest's `parameters_schema`, with its `default` values filled in. |

### Initialize document

`initialize()` receives (`pmp-initialize`):

| Field | Value |
|---|---|
| `pmp_version` | The runtime's PMP version. |
| `run` | `{fingerprint, seed}`: the run fingerprint (64 lowercase hex digits) and the step's seed, derived from the run seed and the step id ([reproducibility.md](reproducibility.md#seeds)). A module MUST draw all of its randomness from this seed. |
| `time` | `{unit, start, period}`: the run's time base; `period` is greater than 0 ([time-cancellation-timeouts.md](time-cancellation-timeouts.md#logical-time-and-ticks)). |

### Invocation document

`invoke()` receives one invocation per tick (`pmp-invocation`):

| Field | Value |
|---|---|
| `pmp_version` | The runtime's PMP version. |
| `step` | The step id. |
| `tick` | The tick number, from 0. |
| `logical_time` | The end of the tick's window. |
| `inputs` | `{<port>: <envelope>}` for every input port that has a message in this tick. An optional input without a message is left out. |

### Result document

`invoke()` returns (`pmp-result`):

| Field | Rule |
|---|---|
| `outputs` | Required. `{<port>: {"data": <payload>}}`. |
| `diagnostics` | Optional. An object of the module's choosing; it is written to the message trace. |
| `pmp_version` | Optional. |

The runtime accepts a result only when all of the following hold, checked in this order; the
first failure is `OUTPUT_INVALID`, located by JSON pointer into the result, and counts as a failed
invocation:

1. It is JSON (otherwise the error's `details.raw_prefix` holds its first 200 bytes).
2. It satisfies `pmp-result`.
3. It names only output ports that the step's capability declares (pointer `/outputs/<port>`).
4. It holds every required output port (pointer `/outputs`). An optional output MAY be left out;
   its consumers then receive nothing in that tick.
5. With strict validation, the `data` of each output satisfies the port's message type (pointer
   `/outputs/<port>/data<pointer>`).

The contract checks apply the same rules ([ADR-0024](../adr/ADR-0024-contract-checks-and-test-vectors.md)).

### Error document

A module that fails a call SHOULD write an error document (`pmp-error`) to the call's error sink:

| Field | Rule |
|---|---|
| `code` | Required integer, at least 1: a general code (1 to 99) or a module code (10000 and up). |
| `message` | Required string. |
| `details` | Optional object. |

The runtime keeps the document as the `cause` of its own error. Text that is not a JSON object
becomes `{"code": <the call's status>, "message": <the text>}`; text beyond 64 KiB is cut and
marked `"truncated": true`; a missing `code` is taken from the call's status and a missing
`message` is empty.

## Catalog

The protocol catalog holds three kinds of entries, each a strict JSON document validated against the
schema of its kind:

| Kind | File in the catalog | Identity |
|---|---|---|
| `message-type` | `messages/<id>/<version>.json` | `id`, `version` |
| `capability-definition` | `capabilities/<id>/<version>.json` | `id`, `version` |
| `workflow-definition` | `workflows/<name>/<version>.json` | `name`, `version` |

- The entries of `spec/catalog/`, listed below, are embedded in the runtime and installed to
  `share/praenora/catalog/`. Extra directories add entries: the API's
  `RuntimeOptions::catalog_paths`, the CLI's `--catalog-path` and the environment variable
  `PRAENORA_CATALOG_PATH`. Every `*.json` file below such a directory is read, recursively, in
  sorted path order.
- An entry is identified by its kind and `id@version`; versions compare without build metadata.
  Entries of different kinds MAY share an `id@version`. A second entry of one kind with the same
  `id@version` is `CATALOG_ENTRY_DUPLICATE`, also when it comes from an extra directory: an entry is
  never replaced.
- A message type's `schema` is the JSON Schema (2020-12 for the embedded entries; draft-07, or
  2020-12 if it declares it, for an entry from an extra directory) of its `data`; it MUST compile,
  or the catalog does not load (`SCHEMA_VIOLATION`). A 2020-12 schema MUST also stay within what the
  AsyncAPI export expresses (AsyncAPI 3.0.0 reads draft-07): no `prefixItems`, `unevaluatedItems`,
  `unevaluatedProperties`, `dependentRequired`, `dependentSchemas`, `$dynamicRef`, `$dynamicAnchor`,
  `$anchor`, `minContains` or `maxContains`, no `$id` but the root's, no nesting deeper than 256
  levels, and no keyword but `title`, `description`, `$comment` or `examples` beside a `$ref`
  (entries of the root `$defs` that no reference reaches are not looked at); otherwise the catalog
  does not load (`SCHEMA_VIOLATION`, at `/schema`).
- `status` is `draft`, `stable` or `deprecated`. It informs users; the runtime treats every status
  alike.
- An entry's digest is the SHA-256 of its canonical JSON. The digests of the entries a run uses are
  part of its fingerprint ([reproducibility.md](reproducibility.md)).
- References between entries (a port's message type, a step's capability) are resolved when a
  workflow that uses them is validated: an unknown one is `CATALOG_ENTRY_NOT_FOUND`, located in the
  entry that names it.
- A change to an entry is a new version of it once versions are bumped again; for now an entry
  changes in place ([compatibility-policy.md](compatibility-policy.md#changing-a-version)).
  `praenora catalog list` and `praenora catalog show <id>@<version>` print the catalog.

Capabilities and workflow definitions are specified in
[workflow-definitions.md](workflow-definitions.md). The catalog of this version holds the entries
below.

### Workflow definitions

| Entry | Status | Content |
|---|---|---|
| [`example.estimate-predict@0.1.0`](../../spec/catalog/workflows/example.estimate-predict/0.1.0.json) | stable | The example workflow: `estimate` and `predict`, with the feedback connection `predict.predictions -> estimate.prior` (delay 1). |
| [`pmp.continuous-forecast@0.1.0`](../../spec/catalog/workflows/pmp.continuous-forecast/0.1.0.json) | draft | The starting workflow: `propose`, `curate` and `predict` ([starting-workflow.md](starting-workflow.md)). |

### Capabilities

| Entry | Status | Inputs | Outputs |
|---|---|---|---|
| [`example.estimate@0.1.0`](../../spec/catalog/capabilities/example.estimate/0.1.0.json) | stable | `observations` (`pmp.observation-batch`), `prior` (`pmp.prediction-set`, optional) | `state` (`example.state-estimate`) |
| [`example.predict@0.1.0`](../../spec/catalog/capabilities/example.predict/0.1.0.json) | stable | `state` (`example.state-estimate`) | `predictions` (`pmp.prediction-set`) |
| [`pmp.propose@0.1.0`](../../spec/catalog/capabilities/pmp.propose/0.1.0.json) | draft | `observations` (`pmp.observation-batch`), `model_set` (`pmp.model-set`), `replenish` (`pmp.replenishment-request`, optional) | `candidates` (`pmp.candidate-set`) |
| [`pmp.curate@0.1.0`](../../spec/catalog/capabilities/pmp.curate/0.1.0.json) | draft | `candidates` (`pmp.candidate-set`), `observations` (`pmp.observation-batch`) | `survivors` (`pmp.candidate-set`), `replenish` (`pmp.replenishment-request`, optional) |
| [`pmp.predict@0.1.0`](../../spec/catalog/capabilities/pmp.predict/0.1.0.json) | draft | `candidates` (`pmp.candidate-set`), `observations` (`pmp.observation-batch`) | `predictions` (`pmp.prediction-set`) |
| [`pmp.assess@0.1.0`](../../spec/catalog/capabilities/pmp.assess/0.1.0.json) | draft | `predictions` (`pmp.prediction-set`), `observations` (`pmp.observation-batch`) | `assessment` (`pmp.assessment`) |

Every port carries version 0.1.0 of its message type; ports are required unless marked optional.

### Message types

| Entry | Status | Data |
|---|---|---|
| [`pmp.observation-batch@0.1.0`](../../spec/catalog/messages/pmp.observation-batch/0.1.0.json) | stable | `window {start, end}`, `time_unit`, `observations[]` (rows below). |
| [`pmp.prediction-set@0.1.0`](../../spec/catalog/messages/pmp.prediction-set/0.1.0.json) | stable | `issued_at` (a logical time), `time_unit`, `predictions[]` (rows below). |
| [`example.state-estimate@0.1.0`](../../spec/catalog/messages/example.state-estimate/0.1.0.json) | stable | `time_unit`, `estimates[] {series_id, variable_id, time, value, basis}`, `basis` one of `observation`, `blend`, `prior`. |
| [`pmp.model-set@0.1.0`](../../spec/catalog/messages/pmp.model-set/0.1.0.json) | draft | `models[] {id, href, sha256}`: model artifacts by reference. |
| [`pmp.candidate-set@0.1.0`](../../spec/catalog/messages/pmp.candidate-set/0.1.0.json) | draft | `candidates[] {id, model {href, sha256}, parameters, status, origin}`. |
| [`pmp.replenishment-request@0.1.0`](../../spec/catalog/messages/pmp.replenishment-request/0.1.0.json) | draft | `requested` (an integer of at least 1), `reason`. |
| [`pmp.assessment@0.1.0`](../../spec/catalog/messages/pmp.assessment/0.1.0.json) | draft | `scores[] {series_id, variable_id, metric, value}`. |

The data of every message type above accepts `x-` extension properties at its top level and in each
array item. The draft types are minimal placeholders whose final shapes are open
([open-questions.md](../open-questions.md), OQ-15).

The rows of the two stable long-form types follow the observation and prediction tables of
`ode-uq-spec` 0.0.0. Observation rows (`observations[]`):

| Field | Type | Rule |
|---|---|---|
| `series_id`, `variable_id` | non-empty string | Required. |
| `time` | number | Required, in the batch's `time_unit`. |
| `value` | number or null | Required. `null` only when `missing` is true, or the row is censored with its limit: `left` with `upper_limit`, `right` with `lower_limit`, `interval` with both. |
| `observation_id`, `group_id`, `batch_id` | string | Optional. |
| `unit` | string | Optional. A label; units are never converted. |
| `missing` | boolean | Optional, default `false`. |
| `censoring` | `none`, `left`, `right` or `interval` | Optional, default `none`. |
| `lower_limit`, `upper_limit` | number or null | Optional. |
| `weight` | number of at least 0 | Optional, default 1. |

Prediction rows (`predictions[]`):

| Field | Type | Rule |
|---|---|---|
| `series_id`, `variable_id` | non-empty string | Required. |
| `time` | number | Required. |
| `prediction_kind` | string matching `^[a-z][a-z0-9_]*$` | Required. Documented values: `latent_mean`, `observation`, `state`, `residual`, `quantile`. |
| `value` | number | Required. |
| `unit` | string | Optional. |
| `draw` | integer of at least 0 | Optional. |
| `quantile` | number in (0, 1) | Required when `prediction_kind` is `quantile`, and forbidden otherwise. |

## Execution semantics

A run executes a validated workflow configuration in ticks
([ADR-0011](../adr/ADR-0011-tick-based-synchronous-execution.md)). The graph rules that make these
semantics well defined are in [workflow-definitions.md](workflow-definitions.md); time and stopping
are detailed in [time-cancellation-timeouts.md](time-cancellation-timeouts.md).

- **Ticks.** Ticks are numbered from 0. Tick `t` covers the logical-time window
  `[start + t * period, start + (t + 1) * period)` of the time base; both bounds are computed by
  multiplication. The tick's logical time, used in its invocations and envelopes, is the window's
  end.
- **Schedule.** The steps run in a schedule computed once per run: a topological order over the
  delay-0 connections between steps, ties broken by the lexicographic order of step ids.
- **Order within a tick.** First every workflow input produces its message, in the order of input
  names: its source's data for the window, in an envelope with source `inputs.<name>`. Then each
  step runs once, in schedule order. Every step runs in every tick.
- **Delivery.** A step input fed by a delay-0 connection receives the message its producer made in
  the same tick; one fed by a delay-1 connection receives the message of the previous tick, and none
  in tick 0. An input without a message is left out of the invocation. A required input without a
  message, which a valid graph excludes, fails the run with `INPUT_MISSING`. A producer MAY feed any
  number of inputs; each input has at most one producer.
- **Outputs.** The runtime checks each result (see [Result document](#result-document)), wraps each
  output's data in an envelope with source `<step>.<port>`, and makes it available to the connected
  inputs. The messages that reach workflow outputs are the tick's outputs; those of the outputs the
  configuration records are appended to the run's output files
  ([reproducibility.md](reproducibility.md#output-files)).
- **Validation.** With strict validation, which a run always uses, the data a workflow input
  produces MUST satisfy the input's message type (`TARGET_DATA_INVALID` otherwise), and so MUST
  every output's data (`OUTPUT_INVALID`). Turning validation off is reserved for performance
  measurements; the run record states which was used.
- **One call at a time.** The runtime makes exactly one module call at a time, so with
  deterministic modules a run is deterministic.
- **Termination.** A run completes when `run.ticks` ticks are complete, or, without `run.ticks`, as
  soon as every replay input is exhausted, checked after each tick and before the first one: no row
  lies at or after the next window's start. A configuration without a replay input MUST set
  `run.ticks`. A run also ends when a stop is
  requested (state `stopped`) or when a step fails (state `failed`): the failure policy is
  fail-fast, so the first failure ends the run, and its modules are finalized and destroyed.

### Input sources

Workflow inputs are fed by sources that the runtime owns, bound in the configuration's `inputs`
section ([ADR-0022](../adr/ADR-0022-target-data-intake.md)):

| Source | Replays | Delivers |
|---|---|---|
| `observation-csv` | yes | A `pmp.observation-batch` per tick: `window {start, end}`, the time base's unit as `time_unit`, and the rows of a long-form observation CSV whose `time` lies in the window (`start <= time < end`), stably sorted by time, series, variable, observation id and position in the file. An empty batch is still delivered. |
| `json-document` | no | The same JSON or YAML document every tick, validated once against the input's message type. |

The file of every source is read and checked during validation; its SHA-256 is part of the run
fingerprint.

## CloudEvents mapping

PMP envelopes map onto [CloudEvents 1.0](https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md)
events as follows. The mapping is documented, not implemented: the runtime of this version neither
emits nor reads CloudEvents. A component that converts envelopes MUST follow it.

| CloudEvents attribute | From the envelope |
|---|---|
| `specversion` | `"1.0"`. |
| `id` | `id`, unchanged. |
| `source` | `urn:praenora:run:<fp16>:<source>`, for example `urn:praenora:run:4664e72704c1c728:estimate.state`. |
| `type` | `<type>.v<type_version>`, for example `example.state-estimate.v0.1.0`. |
| `datacontenttype` | `"application/json"`. |
| `dataschema` | Omitted: PMP 0.1 gives message-type data schemas no URI. `type` names the catalog entry, which holds the schema. |
| `pmptick` (extension) | `tick`, as a String holding the tick in decimal digits, such as `"12"`, because CloudEvents integers are 32-bit while ticks go up to 2^53 - 1. |
| `pmplogicaltime` (extension) | `logical_time`, as a String holding the number as canonical JSON writes it, because CloudEvents has no floating-point type. |
| `pmpversion` (extension) | `pmp_version`, as a String. |
| `data` | `data`, unchanged. |

The CloudEvents `time` attribute is never set: envelopes carry no wall-clock time.

## AsyncAPI documents

`praenora graph --format asyncapi` (and `Runtime::graph()`) generates an
[AsyncAPI 3.0.0](https://www.asyncapi.com/docs/reference/specification/v3.0.0) document for a
workflow definition: one channel per connection source, addressed `<workflow name>/<endpoint>`, a
`send` operation per channel, and one message per message type whose payload is the `pmp-envelope`
schema with the type's data schema in place of `data`. The generated documents of the catalog's
workflows are committed and compared with the generator's output by the tests:

- [`example.estimate-predict-0.1.0.asyncapi.json`](../../spec/asyncapi/example.estimate-predict-0.1.0.asyncapi.json)
- [`pmp.continuous-forecast-0.1.0.asyncapi.json`](../../spec/asyncapi/pmp.continuous-forecast-0.1.0.asyncapi.json)

They are regenerated, with `PRAENORA_UPDATE_GOLDEN=1`, whenever a workflow definition, a message
type or the envelope schema changes. Both are valid against the official AsyncAPI 3.0.0 JSON Schema;
a build that names a local copy of it checks them against it
([testing.md](../testing.md#the-official-asyncapi-schema)). The official AsyncAPI parser
(`@asyncapi/parser` 3.6.6) parses both with no error and no warning, and notes only, for each, that
the latest AsyncAPI version is 3.1.0; a build that names a local installation of it checks them
with it ([testing.md](../testing.md#the-official-asyncapi-parser)).

## References

- [ADR-0007](../adr/ADR-0007-json-messages-for-v0.md): JSON messages for v0.
- [ADR-0043](../adr/ADR-0043-json-schema-dialects-and-validator.md): the schema dialects and the validator; [ADR-0008](../adr/ADR-0008-json-schema-draft-07-embedded-schemas.md): draft-07 schemas, embedded (superseded).
- [ADR-0010](../adr/ADR-0010-workflow-definitions-as-catalog-data.md): the catalog and the configuration.
- [ADR-0011](../adr/ADR-0011-tick-based-synchronous-execution.md): tick-based execution.
- [ADR-0020](../adr/ADR-0020-message-envelope-and-asyncapi.md): envelope, CloudEvents and AsyncAPI.
- [ADR-0022](../adr/ADR-0022-target-data-intake.md): target data intake.
- [configuration.md](../configuration.md): the workflow configuration.
