// Input sources: the runtime-owned data behind the workflow inputs, bound in the configuration's
// `inputs` section. A source is built (and its data read and checked) during validation; the
// engine then asks it for one message payload per tick.
#ifndef PRAENORA_RUNTIME_IO_INPUT_SOURCE_HPP
#define PRAENORA_RUNTIME_IO_INPUT_SOURCE_HPP

#include "catalog/catalog.hpp"
#include "config/workflow_config.hpp"

#include <nlohmann/json.hpp>

#include <praenora/runtime/error.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace praenora::detail {

// What a replay source found in its data while it was built, for the run record's
// execution.inputs.<input> (outside the hashed section: both numbers follow from the file's bytes
// and the configuration, which the fingerprint covers already).
struct SourceCounts {
    std::uint64_t rows_total = 0;        // the rows of the file
    std::uint64_t rows_before_start = 0; // rows whose time is before run.time.start: never delivered
};

class InputSource {
public:
    virtual ~InputSource();

    // What the source noticed while it was built and that is no reason to refuse its data: each a
    // Diagnostic of severity warning, which validation adds to its report. One diagnostic per
    // kind of finding, naming how many rows it concerns and the first of them, so that a file of
    // a million such rows gives one line. Empty by default.
    virtual const std::vector<Diagnostic>& notes() const;

    // The counts of the source's data, or nullopt for a source that has no rows (a
    // json-document). Validation puts them into execution.inputs of the run record.
    virtual const std::optional<SourceCounts>& counts() const;

    // The `data` of the input's message for tick `tick`, whose window is
    // [window_start, window_end) in the workflow's time unit.
    virtual nlohmann::json batch(std::uint64_t tick, double window_start, double window_end) = 0;

    // True for a replay source, whose data runs out (the run may end when every replay source is
    // exhausted); false for a static source.
    virtual bool replay() const = 0;

    // True when no data is left for the windows after the one ending at `window_end`. A static
    // source has nothing pending, so it is always exhausted.
    virtual bool exhausted_after(double window_end) const = 0;

    // The SHA-256 of the source's file (64 lowercase hex digits), part of the run fingerprint.
    virtual const std::string& digest() const = 0;
};

// Builds the source of `binding`, reading and checking its whole file now. `port_type` is the
// message type of the workflow input.
//
// observation-csv: a long-form observation CSV (io/csv.hpp), replayed one tick window at a time.
// - Size: a file larger than kMaxObservationCsvBytes (256 MiB) is refused before it is read.
// - Columns: series_id, variable_id, time and value are required; observation_id, unit, missing,
//   censoring, lower_limit, upper_limit, weight, group_id and batch_id are optional; other columns
//   are ignored. binding.columns maps a column to the header it is read from instead.
// - Rows follow the observation row rules: series_id and variable_id are non-empty; time is a
//   number; value is a number or empty; missing is true or false in any letter case (empty:
//   false); censoring is none, left, right or interval (empty: none); the limits are numbers or
//   empty; weight is a number >= 0 (empty: 1). An empty value is allowed only when missing is
//   true or the row is censored with its limit (left: upper_limit, right: lower_limit,
//   interval: both). missing=true makes the value null.
// - Numbers are decimal ([+-], digits with an optional fraction, an optional exponent), parsed
//   independently of the locale; anything else, and values beyond the range of a double, are
//   refused.
// - Rows are stably sorted by (time, series_id, variable_id, observation_id, position in the
//   file). batch() returns {"window": {"start", "end"}, "time_unit", "observations": [...]} with
//   the rows whose time t satisfies start <= t < end; each row carries series_id, variable_id,
//   time, value, missing, censoring and weight, plus the optional fields that are not empty.
// - exhausted_after(end) is true when there are no rows or end > the last row's time.
// json-document: a JSON or YAML document (document/yaml_loader.hpp) that must satisfy
// `port_type`; batch() returns it every tick.
//
// Throws praenora::Error:
// - TARGET_DATA_UNAVAILABLE, located at the file, for a file that cannot be read (one that does
//   not exist or is a directory too);
// - TARGET_DATA_INVALID, located at the file and line, for malformed CSV, a missing required
//   column (line 1) and rows that break the rules, and located at the file for an observation
//   CSV beyond the size limit;
// - PARSE_ERROR for a json-document that is not JSON or YAML;
// - INPUT_BINDING_INVALID for an unknown source kind, two columns read from one header, an
//   observation batch that `port_type` does not accept, and a json-document that violates
//   `port_type` (located at the problem in the document).
std::unique_ptr<InputSource> make_input_source(const InputBinding& binding,
                                               const MessageTypeDef& port_type,
                                               const TimeBase& time);

// The largest observation CSV file a source reads, in bytes: 256 MiB, the size limit of a module's
// output.
inline constexpr std::uintmax_t kMaxObservationCsvBytes = std::uintmax_t{256} << 20;

// The two kinds of source; make_input_source() picks one by binding.source. An observation CSV
// larger than `max_bytes` (kMaxObservationCsvBytes; tests lower it) is refused, by its size before
// it is read, and again if it grew before it was read: TARGET_DATA_INVALID located at the file.
std::unique_ptr<InputSource>
make_observation_csv_source(const InputBinding& binding, const MessageTypeDef& port_type,
                            const TimeBase& time,
                            std::uintmax_t max_bytes = kMaxObservationCsvBytes);
std::unique_ptr<InputSource> make_json_document_source(const InputBinding& binding,
                                                       const MessageTypeDef& port_type);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_IO_INPUT_SOURCE_HPP
