// The workflow configuration (kind workflow-config): the YAML or JSON file a user writes to pin a
// workflow definition, bind its steps to modules and its inputs to data, and set run, output and
// observability options. Loading checks the document against its schema, applies the defaults,
// resolves relative paths against the file's directory and applies the caller's overrides.
#ifndef PRAENORA_RUNTIME_CONFIG_WORKFLOW_CONFIG_HPP
#define PRAENORA_RUNTIME_CONFIG_WORKFLOW_CONFIG_HPP

#include "core/semver.hpp"
#include "document/yaml_loader.hpp"
#include "isolation/sandbox.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::detail {

// Deadlines of module calls; 0 disables one. `invoke` applies to invoke, `lifecycle` to every
// other call, and `cancel_grace` is how long a call may take to return once cancelled.
struct Timeouts {
    std::chrono::nanoseconds invoke{300'000'000'000}, lifecycle{60'000'000'000},
        cancel_grace{5'000'000'000};
};

// Logical time: tick k covers [start + k * period, start + (k + 1) * period) in `unit`.
struct TimeBase {
    std::string unit = "1";
    double start = 0.0, period = 1.0;
};

// The columns of a long-form observation CSV (the observation columns of ode-uq-spec):
// series_id, variable_id, time, value, observation_id, unit, missing, censoring, lower_limit,
// upper_limit, weight, group_id and batch_id. These are the names InputBinding::columns remaps.
const std::vector<std::string>& observation_columns();

// A workflow input bound to a data source ("observation-csv" or "json-document"). `columns` maps
// observation column names to the CSV's header names (observation-csv only). `descriptor` is the
// ode-uq-spec target descriptor of an observation-csv input (absolute; empty when there is none):
// it gives the dialect, the column mapping and the variables, and names the CSV, so `path` may
// then be empty (validation fills it in from the descriptor's data.href when it builds the
// source), and `columns` is empty.
struct InputBinding {
    std::string input, source;
    std::filesystem::path path;       // absolute
    std::filesystem::path descriptor; // absolute, or empty
    std::map<std::string, std::string> columns;
};

// The names of the isolation modes in the configuration and the run record: "in-process" and
// "process".
std::string_view isolation_mode_name(IsolationMode mode);
// The mode that `name` names, exactly as written; nullopt for anything else.
std::optional<IsolationMode> isolation_mode_from_name(std::string_view name);

// Where a step's module instance runs (docs/specs/process-isolation.md), and where that was set:
// `mode_pointer` is the JSON pointer of the field of the configuration that set the mode
// (/steps/<id>/isolation/mode or /run/isolation/mode), empty when the mode is the default or the
// caller's override (`mode_overridden`), which no field of the file sets. Whether its module host
// shares compiled models with the other sharing steps' (share_compiled_models), and the sandbox
// profile its module host applies (sandbox), each with the pointer of the field that set it (empty
// for the default: false, none).
struct Isolation {
    IsolationMode mode = IsolationMode::in_process;
    std::string mode_pointer;
    bool mode_overridden = false;
    bool share_compiled_models = false;
    std::string share_pointer;
    SandboxProfile sandbox = SandboxProfile::none;
    std::string sandbox_pointer;
};

// A step bound to a module. `parameters` is the object written in the file ({} when absent);
// `timeouts` are the run's timeouts with the step's own fields put over them; `isolation` is
// resolved field by field: the step's field, else the run's, else the default, unless the caller
// overrides it.
struct StepBinding {
    std::string step, module_name;
    SemVer module_version;
    nlohmann::json parameters;
    Timeouts timeouts;
    std::optional<LogLevel> log_level;
    Isolation isolation;
};

// The run settings a caller overrode (CLI flags or API options), as applied; the run record's
// execution section lists them.
struct RunOverrides {
    std::optional<std::uint64_t> seed, ticks;
    std::optional<IsolationMode> isolation;
};

struct WorkflowConfig {
    std::filesystem::path file; // absolute and lexically normal
    std::string workflow_name;
    SemVer workflow_version;
    std::uint64_t seed = 0;
    std::optional<std::uint64_t> ticks;
    RunOverrides overrides; // which of seed and ticks came from the caller
    TimeBase time;
    Timeouts timeouts;
    Isolation isolation; // run.isolation: the steps' default (with any override applied)
    std::vector<std::filesystem::path> module_search_paths; // absolute, in the order written
    std::map<std::string, InputBinding> inputs;
    std::map<std::string, StepBinding> steps;
    std::optional<std::filesystem::path> output_dir;
    // The workflow outputs to record, as written. Empty when the file does not list them, which
    // means every workflow output: validate_workflow() fills the list in from the definition.
    std::vector<std::string> record;
    bool trace_messages = false;
    LogOptions logging; // `file` is an absolute UTF-8 path, or empty
    std::optional<std::filesystem::path> status_file;
    // How often the status file is rewritten; 0: only when the state changes.
    std::chrono::nanoseconds status_interval{1'000'000'000};
    LoadedDocument document; // the file as loaded, for locating diagnostics
};

// Reads the workflow configuration in `file` and adds every problem found to `report`, in this
// order:
//   1. the file is read and parsed (IO_ERROR, PARSE_ERROR);
//   2. its kind is "workflow-config" (DOCUMENT_KIND_MISMATCH), its format_version has a schema
//      (FORMAT_VERSION_UNSUPPORTED), and it satisfies that schema (one SCHEMA_VIOLATION per
//      problem);
//   3. values the schema cannot check are checked (CONFIG_INVALID): durations, version numbers
//      beyond 64 bits, empty paths, `columns` keys that are not observation columns (or appear on
//      a json-document input), two observation columns that would read one header (a column
//      that is not remapped reads the header of its own name; the refused remapping is left
//      out), a `descriptor` on a json-document input or together with `columns`, and run.ticks,
//      which is required when no input is an observation-csv replay (an override of ticks
//      counts).
// Diagnostics are located in the file (file, line, column, JSON pointer). Returns nullopt when
// step 1 or 2 failed, since nothing else can be trusted then; otherwise the configuration, even
// if step 3 reported errors.
//
// Defaults: seed 0; time unit "1", start 0, period 1; timeouts invoke 300 s, lifecycle 60 s,
// cancel_grace 5 s; isolation mode in-process, sharing no compiled models, sandbox profile none;
// logging at level info in text format to standard error; status_interval 1 s. Relative paths
// (input paths, modules.search_paths, outputs.directory, log_file, status_file) resolve against
// the file's directory.
//
// `overrides` are the caller's settings (CLI flags or API options), which take precedence over
// the file: seed and ticks (each at most 2^53 - 1, else INVALID_ARGUMENT; the ones applied are
// noted in WorkflowConfig::overrides), output_dir, trace_messages and status_file (relative paths
// resolve against the current directory), and isolation, which sets every step's mode (noted in
// WorkflowConfig::overrides too; in-process also turns every step's share_compiled_models off and
// sets its sandbox to none). The logging overrides are not applied here, because environment
// variables rank between them and the file; the caller resolves logging.
std::optional<WorkflowConfig> read_workflow_config(const std::filesystem::path& file,
                                                   ValidationReport& report,
                                                   const WorkflowOptions& overrides = {});

// read_workflow_config() without overrides. When the document cannot be used, the returned
// configuration holds only `file` (and `document`, when it could be parsed); `report` says why.
WorkflowConfig load_workflow_config(const std::filesystem::path& file, ValidationReport& report);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_CONFIG_WORKFLOW_CONFIG_HPP
