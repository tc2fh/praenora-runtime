# ADR-0022: Target data intake through runtime-provided replay and document sources

- Status: Proposed
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Target data must enter the workflow deterministically and reproducibly. Reading files inside modules would hide inputs from the fingerprint and duplicate parsing.

## Decision

The runtime owns input sources, bound in the configuration `inputs` section.
- **`observation-csv`:** UTF-8 CSV per RFC 4180 with a header row; column names as in `ode-uq-spec` (required `series_id`, `variable_id`, `time`, `value`), with optional header remapping. `missing=true` or an empty value becomes `value: null`. A row that breaks field rules is `TARGET_DATA_INVALID` naming file and line. Rows are stably sorted by (`time`, `series_id`, `variable_id`, `observation_id`, original row index). Each tick's batch holds rows whose `time` falls in that tick's window; an empty batch is still a batch. The source is exhausted once the window start passes the last row's time. Numbers are parsed independent of the global locale. The file's SHA-256 is part of the fingerprint.
- **`json-document`:** delivers the same payload every tick for static inputs; it is validated against the port's message type and its digest is part of the fingerprint.
- Units are labels only (OQ-18). Replay only: no real-time pacing and no streaming (OQ-8). The `ode-uq-spec` target descriptor is not read in the MVP (OQ-15).
- A run with an output directory writes `run-record.json`, `outputs/<port>.jsonl`, and optional trace, status and log files, all UTF-8 with `\n` endings, in binary mode; outputs and trace are byte-deterministic.

### Builder notes (Proposed)

- The overwrite rule of ADR-0019 applies to target-data runs that write outputs: an output directory already holding `run-record.json` is refused unless overwrite is requested, and overwrite removes only Praenora-managed files.
- CSV reading: a leading UTF-8 byte order mark is ignored; CRLF reads as LF everywhere, inside quoted fields too, and a lone CR is ordinary text; empty lines are skipped; the last record needs no line break. A quote may only open a field. Each problem is `TARGET_DATA_INVALID` with a line: a missing header at line 1; duplicate column names at the header's line; a ragged record and a field that is not UTF-8 at the line where the record starts; a quote inside an unquoted field and text after a closing quote at the line where that character is (which differs from the record's first line when an earlier quoted field of the record spans lines); a quoted field that never closes at the line where it opens. Fields are not trimmed.
- Columns other than the thirteen observation columns are ignored. `columns` reads a column from another header, and then the column's own name is not read. A remapped column whose header is absent is `TARGET_DATA_INVALID` (line 1). A `columns` key that is not an observation column, and two columns read from one header (a column that is not remapped reads its own name), are already `CONFIG_INVALID` in the configuration (ADR-0010), and validation builds no source for such an input; the source's own check of two columns read from one header (`INPUT_BINDING_INVALID`) remains as a defensive one.
- Field rules as implemented: an empty cell means absent; `missing` is `true` or `false` in any letter case; `censoring` is lower case; `weight` is at least 0 and defaults to 1; limits are numbers. An empty value is allowed only with `missing=true` or a censored row with its limit. `missing=true` makes the value `null` even when a number is written, and that number must still be a number.
- Numbers are decimal (`[+-]digits[.digits][(e|E)[+-]digits]`, also `.5` and `5.`); infinities, NaN, values beyond a double and surrounding spaces are refused. They are read by nlohmann/json's parser, which is locale-adaptive rather than locale-free: it replaces the `.` with the C locale's decimal point (read from `localeconv()`) before calling `strtod`, so a locale whose decimal separator is a single byte, such as `,` in `de_DE.UTF-8`, reads `0.5` as one half. A multi-byte decimal separator is not handled. The CLI and the Python bindings never change `LC_NUMERIC`, so the runtime's own programs run in the C locale's numbers; a host application that sets another locale gets the same values within the limit above.
- A batch row carries `series_id`, `variable_id`, `time`, `value`, `missing`, `censoring` and `weight`, plus whichever of `observation_id`, `unit`, `lower_limit`, `upper_limit`, `group_id` and `batch_id` are not empty. Every number is a double.
- Rows whose time lies before the first window's start are never delivered.
- When the source is built, all of its rows are validated as one batch against the input's message type: a problem inside a row is `TARGET_DATA_INVALID` at that row's line; any other problem is `INPUT_BINDING_INVALID`, because the type cannot carry observation batches. A type that accepts any object (such as a test type) can carry them.
- A `json-document` input may be JSON or YAML, chosen by the file's extension like every document. A payload that violates the message type is `INPUT_BINDING_INVALID`, located at the first problem in the document, with every problem in `details`. Its `replay()` is false and `exhausted_after()` always true: a static source never holds data back, so it never keeps a run going.
- A missing or unreadable data file is `TARGET_DATA_INVALID` (located at `/inputs/<name>/path` when the file does not exist), like the other data problems found during validation, so `praenora validate` exits with 3. `TARGET_DATA_UNAVAILABLE` is kept for data that cannot be read during a run.
- Later: [ADR-0031](ADR-0031-raise-the-reserved-error-codes.md) reports a data file that does not exist, is not a file or cannot be read as `TARGET_DATA_UNAVAILABLE` (located at `/inputs/<name>/path` when the file does not exist, is not a file or its status cannot be read, and at the file when it cannot be opened or read); `praenora validate` still exits with 3. `TARGET_DATA_INVALID` keeps what an observation CSV holds and its size limit.
- Rows before the run's start: a row whose `time` is before `run.time.start` lies in no window, so it was never delivered, silently. A source now counts such rows when it is built and validation reports one warning for the input (severity warning, code `TARGET_DATA_INVALID`, located at the CSV file and the line of the first such row in the file, the message giving how many of how many rows and the first row's time). The warning does not make the configuration invalid, `praenora validate` still exits with 0 and prints it, and `praenora run` and `Workflow::diagnostics()` give the same warnings. The run record holds the counts in `execution.inputs.<input>` (`rows_total`, `rows_before_start`), outside the hashed reproducibility section ([reproducibility.md](../specs/reproducibility.md#run-record)). The mechanism is general: an `InputSource` has `notes()` (warnings found while it was built, one diagnostic per kind of finding, naming the count and the first case) and `counts()`; a `json-document` source has neither.
- An observation CSV file may hold at most 256 MiB (268435456 bytes, the size limit of a module's output), so that CSV parsing, like the parsing of documents, has a maximum size. The file's size is checked before it is read, and the bytes read are checked again in case the file grew meanwhile; a larger file is `TARGET_DATA_INVALID`, located at the file ("the observation CSV is too large: N bytes; the limit is 268435456 bytes (256 MiB)"). The limit is `kMaxObservationCsvBytes` (`io/input_source.hpp`); `make_observation_csv_source()` takes it as a parameter, which only tests lower. A `json-document` input is a document and keeps the documents' 16 MiB.

## Alternatives considered

- **Modules read their own data.** Hides inputs from the fingerprint and from validation.
- **Streaming or live sources.** Needs pacing and ordering semantics (OQ-8).
- **Unit-aware conversion in the runtime.** Large scope (OQ-18).
- **Reading only `ode-uq-spec` target descriptors.** Defers the simple CSV case the examples need.

## Consequences

- Inputs are validated before the run starts and are part of the fingerprint.
- Long-form CSV matches `ode-uq-spec` and keeps module authors from writing parsers.
- Live operation needs a new source type later.

## References

- [docs/specs/protocol.md](../specs/protocol.md)
- [RFC 4180](https://www.rfc-editor.org/rfc/rfc4180)
- [docs/user-guide.md](../user-guide.md)
- [docs/open-questions.md](../open-questions.md)
