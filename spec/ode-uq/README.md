# ode-uq-spec schemas

Copies of schemas of the ode-uq-spec that the runtime reads (JSON Schema 2020-12). A copy is the published file with LF line endings (the repository normalises line endings, and a working copy of the spec on Windows may have CRLF), so it is equal to the published file in content, and its sha256 is that of the git blob. They are not Praenora document formats: they are not in `spec/schemas/`, have no `kind` and are embedded under `ode-uq/`.

| File | Source | sha256 |
|---|---|---|
| `target.0.0.schema.json` | `schemas/target.schema.json`, ode-uq-spec branch `start`, commit d540e28 | `0c364a8e754cdd961b740c61d1229fbb79f545f9b063b34ce21436f09589f910` |

To update one, copy the new file over it (LF line endings) and change the row above and the hash in `TargetDescriptorSchema.IsTheFileThatTheReadmeRecords`, which runs everywhere and fails when they differ. `TargetDescriptorSchema.IsTheOneThatOdeUqSpecPublishes` compares the embedded copy with a checkout of the ode-uq-spec when the environment variable `PRAENORA_ODE_UQ_SPEC_DIR` names one (it skips otherwise; line endings do not matter to it). `TargetDescriptorSchema.EnumerationsAndConstantsAreTheOnesTheRuntimeKnows` fails when an enumeration or constant changes (a new role, a new duplicate policy), and `TargetDescriptorDigest.EveryPropertyOfTheSchemaIsHashedOrExcluded` fails until every property that a new version adds is classified in `src/runtime/io/target_descriptor.cpp` ([ADR-0042](../../docs/adr/ADR-0042-target-descriptor-intake.md)).
