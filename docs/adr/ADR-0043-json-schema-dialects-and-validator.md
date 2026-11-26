# ADR-0043: JSON Schema dialects (draft-07 and 2020-12) and the schema validator

- Status: Proposed
- Date: 2026-11-24
- Supersedes: [ADR-0008](ADR-0008-json-schema-draft-07-embedded-schemas.md)
- Superseded by: none

## Context

[ADR-0008](ADR-0008-json-schema-draft-07-embedded-schemas.md) fixed every Praenora schema, and every
module's `parameters_schema`, to JSON Schema draft-07, because the validator the project mandated,
pboettch/json-schema-validator 2.4.1, supports only draft-07. The upstream `ode-uq-spec` documents
use 2020-12, and the runtime has to read their schemas as they are ([OQ-2](../open-questions.md)).

pboettch/json-schema-validator does not only lack 2020-12: it accepts 2020-12 schemas and then
ignores the keywords it does not know. Measured with 2.4.1, an instance that violates `prefixItems`,
`unevaluatedProperties: false`, `dependentRequired`, a `$ref` with sibling keywords or a
`$dynamicRef` is reported as valid. Support for 2020-12 is an open upstream issue with no owner
(pboettch/json-schema-validator issues 153, 373 and 385).

A schema that refers to itself in place (`{"$ref": "#"}`) overflows the stack in pboettch and in
jsoncons, in both dialects; a third-party `parameters_schema` or catalog schema can therefore kill
the runtime today.

## Decision

- **Two dialects.** A schema declares its dialect with `$schema`: draft-07
  (`http://json-schema.org/draft-07/schema`, with or without the empty fragment) or 2020-12
  (`https://json-schema.org/draft/2020-12/schema`). A schema without `$schema` is draft-07, the
  dialect every Praenora schema has had; a module's `parameters_schema` without `$schema` therefore
  means what it meant. Any other `$schema` (draft-04, draft-06, 2019-09, unknown) is refused with
  `SCHEMA_VIOLATION`, although the library reads them.
- **The validator is jsoncons** (the `jsonschema` extension; Boost Software License 1.0; header-only;
  C++11), required exactly, `find_package(jsoncons 1.10.0 EXACT CONFIG)` and the pin (the reporter's signature changed in 1.7, and the adapter reads three message texts, which a test pins), commit
  `9e57e421285422d7aeeb5aa7dc839f6a19121ee6`. It is found with `find_package(jsoncons CONFIG)` and,
  with `PRAENORA_FETCH_DEPENDENCIES=ON`, fetched like the other dependencies, with one difference:
  only its sources are downloaded and an interface target is defined for them, because its own
  CMake project installs its headers and a package and has no option to stop that, and
  [ADR-0017](ADR-0017-dependency-management.md) lets no fetched library install anything.
  pboettch/json-schema-validator is no longer a dependency; `PRAENORA_FETCH_JSON_SCHEMA_VALIDATOR`
  is removed.
- **`CompiledSchema` keeps its interface** (`validate`, `apply_defaults`, `name`; `SchemaIssue` is a
  pointer and a message). All of the validator code is in `src/runtime/document/schema.cpp`.
  Documents are converted from nlohmann/json to jsoncons node by node at each call.
- **Unchanged from ADR-0008:** every Praenora JSON document has `kind` and `format_version` (ABI
  documents `pmp_version`), one schema file per (kind, major.minor); schemas and catalog entries
  are embedded in the runtime and installed to `share/praenora/`; each schema has the `$id`
  `urn:praenora:schema:<kind>:<major>.<minor>`; the runtime does not validate `ode-uq-spec`
  artifacts unless a feature says so (the P6 descriptor, a later change); the 19 document schemas
  and the catalog's message-type schemas stay draft-07 in this change; the builder notes on
  embedding, the registry, self-contained schemas, durations, run record shapes and `if`/`then`/`else`
  stand.
- **References.** External `$ref`s may name an embedded Praenora schema by URN or the meta-schema of
  a dialect; anything else is refused, so compiling never reads files or the network. A schema is
  checked against the meta-schema of its dialect before it is compiled. The error is reported at the
  first problem that has no nested cause, with its place in the schema; a property that fails the
  schema of `additionalProperties` is reported at the property.
- **Formats are assertions in both dialects** (`require_format_validation`), for the formats jsoncons
  knows: `date`, `date-time`, `time`, `email`, `hostname`, `ipv4`, `ipv6`, `json-pointer`, `regex`,
  `uri`, `uri-reference`, and `uuid`, which jsoncons lacks and which a pattern checks (the same
  hexadecimal 8-4-4-4-12 form as before, added to the schema handed to the library; a failure reads
  "String 'x' is not a valid UUID"). Other formats
  are accepted. `uri-reference` and `json-pointer` are checked now, which the previous validator did
  not do.
- **Reference cycles.** A schema in which a subschema reaches itself through `$ref`, `$dynamicRef`,
  `allOf`, `anyOf`, `oneOf`, `not`, `if`, `then`, `else` or `dependentSchemas` alone, without a
  keyword that descends into the instance, is refused before it is compiled. The walk resolves
  references by JSON pointer, `$anchor`, `$dynamicAnchor`, draft-07 plain-name `$id`, nested `$id`
  (relative ones against their base) and embedded Praenora URNs; a dynamic reference counts as a
  reference to the dynamic anchors of its name in its own resource and in the resources that lead to
  it from the root (its possible dynamic scope), not in unrelated ones; draft-07 ignores an `$id`
  beside a `$ref`, as the library does. A reference it cannot resolve, and a schema nested
  or chained deeper than 256 levels, is refused with `SCHEMA_VIOLATION` rather than passed on. A
  schema document nests at most 128 levels of objects and arrays, the limit of documents. A cycle
  through `properties`, `items` and the like (a tree) is fine.
- **Order of issues.** The issues of one validation are the library's messages and their nested
  causes, flattened and ordered by pointer (a parent before its children, array indices compared as
  numbers, numbers before other names); at one pointer, issues without nested causes come before
  those that wrap them (an `anyOf` failure), then by keyword and by message (the order of two
  issues at one pointer is the library's keyword names, which the two dialects spell differently
  for a few rules; the tests compare both dialects over those rules and find the same first
  issue); what is still equal keeps the library's order. The first issue, which several callers
  print, therefore does not depend on the order in which a dialect evaluates its keywords. It is
  the same in both dialects for the rules the tests compare; that is not shown for every schema.
- **Stack use is bounded for nesting and chains** (the validator and the compiler recurse, and a
  stack overflow cannot be caught). The limits, which are what the runtime accepts:
  - an instance nests at most 136 levels (the document limit, 128, and the levels an envelope
    adds); one more is one issue saying so. Every validation goes through `CompiledSchema`, so the
    limit holds on every path that reaches the validator (documents, messages, provider output,
    the Python facade);
  - applicators that apply to the same value (`$ref`, `allOf`, `anyOf`, `oneOf`, `not`, `if`,
    `then`, `else`, `dependentSchemas`, `$dynamicRef`) chain at most 32 deep between two instance
    levels (the walk computes the longest chain; a longer one is `SCHEMA_VIOLATION`). The schema
    handed to the library has up to two more (the wrapper that moves a default, the `allOf` of a
    uuid pattern), and a `$ref` to a meta-schema counts as four; the budget counts all of them;
  - a schema document nests at most 128 levels of objects and arrays; the reference walk gives up
    at 256 levels of nesting plus reference hops.
- **The stack is sized, not hoped for.** A validation needs
  `512 KiB + (instance levels + 1) x (4 KiB + (chain + 2) x 6 KiB)` and compiling needs
  `512 KiB + 28 KiB x JSON levels` (at most 29 MiB and 4.1 MiB, reserved and not committed). Work
  that needs at most 192 KiB of the validation estimate, or a schema nested at most 8 levels, runs
  on the caller's thread and pays nothing; the rest runs on a thread with that stack
  (`run_on_thread_with_stack` in `platform/`: `pthread_attr_setstacksize`, or `CreateThread` with a
  reservation). A thread that cannot be started is `RESOURCE_EXHAUSTED`, like running out of
  memory, never an issue of the instance or a defect of the schema; `std::bad_alloc` propagates.
  The converted instance is created and destroyed on the caller's thread: its depth is at most 136
  and its destruction recurses once per level with small frames (the worst case runs on a 256 KiB
  caller in the tests).
- **Measured**, Windows, minimum stack that survives (KiB; a recursive schema, instance 136 levels
  deep, `$ref` closing the loop; slope = (minimum - 79 KiB base) / 137 levels):

  | Recursing keyword (chain 1) | Release | Debug | Debug per level |
  |---|---|---|---|
  | properties | 335 | 591 | 3.7 |
  | additionalProperties | 335 | 719 | 4.7 |
  | patternProperties | 271 | 527 | 3.3 |
  | unevaluatedProperties | 335 | 655 | 4.2 |
  | items, contains, unevaluatedItems | 271-303 | 527 | 3.3 |
  | prefixItems | 271 | 399 | 2.3 |
  | dependentSchemas | 399 | 719 | 4.7 |
  | `$dynamicRef` instead of `$ref` | 207 | 463 | 2.8 |
  | meta-schema `$ref`, schema-shaped instance (`not`/`items` nesting; draft-07 `items` worst) | 399-463 | 719-911 | 4.7-6.1 |

  | Chained applicator, 12 deep, 136 levels | Release | Debug | Debug per hop and level |
  |---|---|---|---|
  | allOf | 2,254 | 3,598 | 1.8 |
  | anyOf | 2,446 | 4,557 | 2.4 |
  | oneOf | 2,638 | 5,261 | 2.8 |
  | not (two hops a wrapper) | 3,534 | 5,773 | 1.6 |
  | if / then | 1,487 | 2,958 | 1.4 |
  | dependentSchemas | 1,615 | 2,638 | 1.2 |

  Compiling (minimum stack, Release / Debug): `items` or `additionalProperties` nested 126 levels
  527 / 1,679 KiB (12.7 KiB a JSON level, the worst; draft-07 1,231 in Debug), `properties` nested
  62 levels (124 JSON levels) 271 / 847. In-place shapes (`not`, `allOf`, `if`) cannot nest past the
  chain limit. The constants above are twice the worst Debug cost of every keyword (4.7 KiB a
  level, 2.8 KiB a hop, 6.1 KiB a meta-schema level, 12.7 KiB a JSON level), so Linux and macOS
  frames up to twice as large still fit. The unit tests run every keyword at 136 levels, every
  applicator chained to the limit, both meta-schemas, and schemas nested to the document limit
  with the stack of the started threads cut to half (a test hook), and the largest work that stays
  inline on a caller with 256 KiB, in Debug and Release, in the CI of all three platforms; one
  past each limit is refused cleanly.
- **Real messages stay inline.** The estimate over the valid fixture documents of the repository
  and observation batches of 10, 100 and 1,000 rows (40 cases): minimum 32 KiB, median 110 KiB,
  90th percentile 168 KiB, maximum 198 KiB; every message kind (envelope, invocation, result,
  error, create, initialize, describe) and an envelope with 1,000 rows are at most 112 KiB, and a
  test pins that they are inside the 192 KiB budget. Only documents (run records, status
  snapshots, test vectors, 168 to 198 KiB) take a thread, once per run or load.
- **Not bounded: regular expressions.** `pattern`, `patternProperties` and the `regex` format use
  `std::regex`, which recurses over the subject (libstdc++) or the pattern (MSVC, libc++) with no
  cap. This is unchanged from the previous validator (the same probes against both libraries
  agree on Windows, Linux and macOS: libstdc++ crashes on a 200,000-character subject, MSVC throws
  `regex_error(error_stack)` from 20,000, libc++ throws on a nested pattern of 10,000), so it is
  not fixed here; it is a separate, older problem of the runtime and needs its own issue.
- **What changed for inputs the runtime accepted:** an instance nested deeper than 136 levels (it
  was unbounded) and a schema whose applicators chain more than 32 are now refused; no shipped
  schema or fixture comes near either. Instances are converted to jsoncons node by node.
- **Behaviour that follows each dialect's specification, and is documented:**
  - Draft-07 ignores the keywords beside a `$ref`, a `default` among them; 2020-12 applies them.
    Parameters are hashed after defaults are filled in, so such a `default` matters. `praenora check
    module` reports it in the result of `manifest.semantics`.
  - A `default` in the schema that a property's `$ref` points at is applied to the absent property,
    as the previous validator did (jsoncons does not). A second compiled copy of the schema, with the
    default moved to the property, computes the defaults; what validates, and so the issues, is the
    schema as written. Defaults keep their JSON type (`1.0`, `1e300`, `-0.0`, integers up to 2^64-1).
  - `integer` accepts `1.0` in both dialects (the specification's reading; the previous validator
    refused it). Numbers keep their JSON type in the fingerprint (OQ-20), so `1.0` and `1` still
    hash differently.
- **Licenses.** jsoncons' headers carry the license text of code they include: MIT (`a5hash.hpp`,
  `grisu3.hpp`) and Apache 2.0 (the BSON headers). An installation built with a fetched jsoncons
  holds those four files under `share/praenora/licenses/jsoncons/` beside its `LICENSE`
  ([ADR-0040](ADR-0040-mit-license.md)); a bundle or package provides them for an installed one.

### Builder notes (Proposed)

- Known library behaviour the runtime lives with: jsoncons reports a property that fails the schema
  of `additionalProperties` as "found but was invalid" at the object, without the nested cause, so
  the issue reads "property 'k' is invalid" (the previous validator gave the cause); an `anyOf`
  failure is reported at the `anyOf` node with the causes nested. Open jsoncons issues 813 to 819
  (filed 2026-11-20) concern `unevaluated*`, `dependentSchemas`, `$anchor` and `%` in references,
  none used by a Praenora schema; issue 813 is the stack overflow above, which the cycle check
  covers. Very deep instances can also exhaust the stack in validation (untested here); documents
  are limited to 128 levels of nesting when they are read, which bounds the documents the runtime
  loads.
- The dependency bundles of praenora-runtime-deps do not hold jsoncons yet; until they do, the `ci-*`
  presets, which set `PRAENORA_FETCH_DEPENDENCIES=ON`, fetch it. The bundle keeps nlohmann/json in
  its `json-schema-validator` directory, which `CMAKE_PREFIX_PATH` still names.

## Alternatives considered

- **Keep pboettch/json-schema-validator and contribute 2020-12.** The work is unassigned upstream,
  estimated at weeks, and the runtime would depend on an unfinished feature of a project that
  releases about once a year.
- **sourcemeta/blaze.** Complete and fast, but AGPL-3.0-or-later or a commercial license; the
  runtime is MIT ([ADR-0040](ADR-0040-mit-license.md)) and links the validator statically.
- **valijson.** No 2019-09 or 2020-12 support, and none planned.
- **Two libraries** (pboettch for runtime schemas, another for spec schemas). Two sets of semantics
  and two dependencies for the module authors and the build to carry.
- **Migrating the runtime-owned schemas to 2020-12 in the same change.** Decided as the end state
  (the owner chose it), delivered as a second change after this one, so that the library swap is
  checked on its own with every schema still draft-07.
- **2020-12 as the default for a schema without `$schema`.** Shorter long term, but changes what
  existing manifests mean (`definitions` references, `dependencies`).

## Consequences

- Module authors may declare 2020-12. A `parameters_schema` without `$schema` still means draft-07.
- Validator messages change wording (the text users see in `PARAMETERS_INVALID` and similar);
  pointers of issues stay the same, except that a property inside an `anyOf` that
  `additionalProperties` rejects is now located at the property.
- A runtime release is needed to move the library; the previous validator's sanitizer carve-out
  (`PRAENORA_FETCH_JSON_SCHEMA_VALIDATOR`, the AddressSanitizer option in the official AsyncAPI
  tests) is gone, because jsoncons is compiled with the tree's flags.
- The dependency bundle can drop the validator it no longer needs when its next release is made.
- Open: migrating the runtime-owned schemas to 2020-12 and the check that keeps the schemas AsyncAPI
  embeds within the draft-07-compatible subset (AsyncAPI 3.0.0 cannot express more).

## References

- [JSON Schema 2020-12](https://json-schema.org/draft/2020-12/json-schema-core) and
  [draft-07](https://json-schema.org/draft-07/json-schema-release-notes)
- [jsoncons jsonschema](https://github.com/danielaparker/jsoncons/blob/master/doc/ref/jsonschema/jsonschema.md)
- [ADR-0008](ADR-0008-json-schema-draft-07-embedded-schemas.md),
  [ADR-0017](ADR-0017-dependency-management.md), [ADR-0020](ADR-0020-message-envelope-and-asyncapi.md),
  [ADR-0040](ADR-0040-mit-license.md), [ADR-0041](ADR-0041-ci-and-releases-against-pinned-dependency-bundles.md)
- [docs/specs/manifest.md](../specs/manifest.md), [docs/specs/protocol.md](../specs/protocol.md)
