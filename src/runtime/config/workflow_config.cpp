#include "config/workflow_config.hpp"

#include "core/duration.hpp"
#include "core/paths.hpp"
#include "core/seed.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"
#include "observability/logging.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kKind = "workflow-config";

std::string single_quoted(const std::string& text) { return "'" + text + "'"; }

std::string pointer_to(const std::string& base, const std::string& key) {
    return base + "/" + json_pointer_escape(key);
}

// The value of `key` in `object` when `object` is an object that has it, else nullptr.
const json* find_value(const json* object, const char* key) {
    if (object == nullptr || !object->is_object()) {
        return nullptr;
    }
    const auto it = object->find(key);
    return it == object->end() ? nullptr : &*it;
}

// A count or seed the schema has already checked: an integer from 0 to 2^53 - 1.
std::uint64_t as_count(const json& value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    }
    return static_cast<std::uint64_t>(value.get<double>());
}

// Builds the configuration from a document that satisfies the schema, reporting the problems the
// schema cannot see.
class Reader {
public:
    Reader(const LoadedDocument& document, ValidationReport& report, fs::path directory)
        : document_(document), report_(report), directory_(std::move(directory)) {}

    void error(ErrorCode code, const std::string& pointer, std::string message) {
        Diagnostic diagnostic;
        diagnostic.severity = Severity::error;
        diagnostic.code = code;
        diagnostic.message = std::move(message);
        diagnostic.location = locate(document_, pointer);
        report_.add(std::move(diagnostic));
    }

    void read(WorkflowConfig& config) {
        const json& root = document_.json;
        const json& workflow = root.at("workflow");
        config.workflow_name = workflow.at("name").get<std::string>();
        if (const auto version = version_at(workflow.at("version"), "/workflow/version")) {
            config.workflow_version = *version;
        }
        read_run(find_value(&root, "run"), config);
        read_modules(find_value(&root, "modules"), config);
        read_inputs(find_value(&root, "inputs"), config);
        read_steps(root.at("steps"), config);
        read_outputs(find_value(&root, "outputs"), config);
        read_observability(find_value(&root, "observability"), config);
    }

private:
    std::optional<SemVer> version_at(const json& value, const std::string& pointer) {
        const std::string& text = value.get_ref<const std::string&>();
        try {
            return parse_semver(text);
        } catch (const Error& e) {
            error(ErrorCode::CONFIG_INVALID, pointer,
                  single_quoted(text) + " is not a usable version: " + e.what());
            return std::nullopt;
        }
    }

    // A duration: a string <number><unit>, "0" or "none", or the integer 0.
    void duration_at(const json* value, const std::string& pointer,
                     std::chrono::nanoseconds& target) {
        if (value == nullptr) {
            return;
        }
        if (!value->is_string()) {
            target = std::chrono::nanoseconds(0); // the schema allows only the integer 0
            return;
        }
        try {
            target = parse_duration(value->get_ref<const std::string&>());
        } catch (const Error& e) {
            error(ErrorCode::CONFIG_INVALID, pointer, e.what());
        }
    }

    void timeouts_at(const json* object, const std::string& pointer, Timeouts& timeouts) {
        duration_at(find_value(object, "invoke"), pointer + "/invoke", timeouts.invoke);
        duration_at(find_value(object, "lifecycle"), pointer + "/lifecycle", timeouts.lifecycle);
        duration_at(find_value(object, "cancel_grace"), pointer + "/cancel_grace",
                    timeouts.cancel_grace);
    }

    // A path written in the file, relative to the file's directory; nullopt (and a diagnostic)
    // when it is empty.
    std::optional<fs::path> path_at(const json& value, const std::string& pointer) {
        const std::string& text = value.get_ref<const std::string&>();
        if (text.empty()) {
            error(ErrorCode::CONFIG_INVALID, pointer, "the path must not be empty");
            return std::nullopt;
        }
        return resolve_against(directory_, text);
    }

    void read_run(const json* run, WorkflowConfig& config) {
        if (const json* seed = find_value(run, "seed")) {
            config.seed = as_count(*seed);
        }
        if (const json* ticks = find_value(run, "ticks")) {
            config.ticks = as_count(*ticks);
        }
        const json* time = find_value(run, "time");
        if (const json* unit = find_value(time, "unit")) {
            config.time.unit = unit->get<std::string>();
        }
        if (const json* start = find_value(time, "start")) {
            config.time.start = start->get<double>();
        }
        if (const json* period = find_value(time, "period")) {
            config.time.period = period->get<double>();
        }
        timeouts_at(find_value(run, "timeouts"), "/run/timeouts", config.timeouts);
        isolation_at(find_value(run, "isolation"), "/run/isolation", config.isolation);
    }

    // The fields of an isolation object the schema has already checked, put over `isolation`.
    static void isolation_at(const json* object, const std::string& pointer, Isolation& isolation) {
        if (const json* mode = find_value(object, "mode")) {
            // The schema allows only the names of the modes.
            isolation.mode = isolation_mode_from_name(mode->get_ref<const std::string&>())
                                 .value_or(IsolationMode::in_process);
            isolation.mode_pointer = pointer + "/mode";
        }
        if (const json* share = find_value(object, "share_compiled_models")) {
            isolation.share_compiled_models = share->get<bool>();
            isolation.share_pointer = pointer + "/share_compiled_models";
        }
        if (const json* sandbox = find_value(object, "sandbox")) {
            // The schema allows only the names of the profiles.
            isolation.sandbox = sandbox_profile_from_name(sandbox->get_ref<const std::string&>())
                                    .value_or(SandboxProfile::none);
            isolation.sandbox_pointer = pointer + "/sandbox";
        }
    }

    void read_modules(const json* modules, WorkflowConfig& config) {
        const json* paths = find_value(modules, "search_paths");
        if (paths == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < paths->size(); ++i) {
            if (auto path = path_at(paths->at(i), "/modules/search_paths/" + std::to_string(i))) {
                config.module_search_paths.push_back(std::move(*path));
            }
        }
    }

    void read_inputs(const json* inputs, WorkflowConfig& config) {
        if (inputs == nullptr) {
            return;
        }
        for (const auto& [name, entry] : inputs->items()) {
            const std::string pointer = pointer_to("/inputs", name);
            InputBinding binding;
            binding.input = name;
            binding.source = entry.at("source").get<std::string>();
            if (const json* path = find_value(&entry, "path")) {
                if (auto resolved = path_at(*path, pointer + "/path")) {
                    binding.path = std::move(*resolved);
                }
            }
            if (const json* descriptor = find_value(&entry, "descriptor")) {
                if (auto resolved = path_at(*descriptor, pointer + "/descriptor")) {
                    binding.descriptor = std::move(*resolved);
                }
            }
            check_data_location(entry, pointer, binding);
            if (const json* columns = find_value(&entry, "columns");
                columns != nullptr && !entry.contains("descriptor")) {
                read_columns(*columns, pointer + "/columns", binding);
            }
            config.inputs.emplace(name, std::move(binding));
        }
    }

    // A descriptor (the schema lets an input have a path, a descriptor or both) describes an
    // observation CSV and takes over the column mapping, so `columns` is refused beside it.
    void check_data_location(const json& entry, const std::string& pointer,
                             const InputBinding& binding) {
        const bool has_descriptor = entry.contains("descriptor");
        if (has_descriptor && binding.source != "observation-csv") {
            error(ErrorCode::CONFIG_INVALID, pointer + "/descriptor",
                  "a descriptor describes an observation CSV, not a " + binding.source + " input");
        } else if (has_descriptor && entry.contains("columns")) {
            error(ErrorCode::CONFIG_INVALID, pointer + "/columns",
                  "columns cannot be given beside a descriptor, whose columns are used");
        }
    }

    void read_columns(const json& columns, const std::string& pointer, InputBinding& binding) {
        if (binding.source != "observation-csv") {
            if (!columns.empty()) {
                error(ErrorCode::CONFIG_INVALID, pointer,
                      "columns can only be remapped for an observation-csv input, not for a " +
                          binding.source + " input");
            }
            return;
        }
        const auto& known = observation_columns();
        std::map<std::string, std::string> remapped; // observation column -> header
        for (const auto& [column, header_value] : columns.items()) {
            if (std::find(known.begin(), known.end(), column) == known.end()) {
                std::string list;
                for (const auto& name : known) {
                    list += (list.empty() ? "" : ", ") + name;
                }
                error(ErrorCode::CONFIG_INVALID, pointer_to(pointer, column),
                      single_quoted(column) + " is not an observation column; the columns are " +
                          list);
                continue;
            }
            remapped.emplace(column, header_value.get<std::string>());
        }
        // Every observation column reads one header, its remapping or else its own name, and no
        // two may read the same one. The columns that are not remapped take their own names
        // first, so a clash is reported at the remapping that causes it; of two remappings to one
        // header, at the one whose column sorts later. A refused remapping is left out.
        std::map<std::string, std::string> readers; // header -> the column that reads it
        for (const auto& column : known) {
            if (remapped.count(column) == 0) {
                readers.emplace(column, column);
            }
        }
        for (const auto& [column, header] : remapped) {
            const auto [it, inserted] = readers.emplace(header, column);
            if (!inserted) {
                const std::string& other = it->second;
                error(ErrorCode::CONFIG_INVALID, pointer_to(pointer, column),
                      "the columns " + single_quoted(other) + " and " + single_quoted(column) +
                          " cannot both be read from the header " + single_quoted(header) +
                          (remapped.count(other) == 0
                               ? " (" + single_quoted(other) + " reads it under its own name)"
                               : std::string()));
                continue;
            }
            binding.columns.emplace(column, header);
        }
    }

    void read_steps(const json& steps, WorkflowConfig& config) {
        for (const auto& [id, entry] : steps.items()) {
            const std::string pointer = pointer_to("/steps", id);
            StepBinding binding;
            binding.step = id;
            const json& module = entry.at("module");
            binding.module_name = module.at("name").get<std::string>();
            const auto version = version_at(module.at("version"), pointer + "/module/version");
            if (version) {
                binding.module_version = *version;
            }
            const json* parameters = find_value(&entry, "parameters");
            binding.parameters = parameters != nullptr ? *parameters : json::object();
            binding.timeouts = config.timeouts;
            timeouts_at(find_value(&entry, "timeouts"), pointer + "/timeouts", binding.timeouts);
            if (const json* level = find_value(&entry, "log_level")) {
                binding.log_level = parse_log_level(level->get<std::string>());
            }
            binding.isolation = config.isolation;
            isolation_at(find_value(&entry, "isolation"), pointer + "/isolation",
                         binding.isolation);
            config.steps.emplace(id, std::move(binding));
        }
    }

    void read_outputs(const json* outputs, WorkflowConfig& config) {
        if (const json* directory = find_value(outputs, "directory");
            directory != nullptr && !directory->is_null()) {
            config.output_dir = path_at(*directory, "/outputs/directory");
        }
        if (const json* record = find_value(outputs, "record")) {
            for (const auto& name : *record) {
                config.record.push_back(name.get<std::string>());
            }
        }
        if (const json* trace = find_value(outputs, "trace_messages")) {
            config.trace_messages = trace->get<bool>();
        }
    }

    void read_observability(const json* observability, WorkflowConfig& config) {
        if (const json* level = find_value(observability, "log_level")) {
            config.logging.level = parse_log_level(level->get<std::string>());
        }
        if (const json* format = find_value(observability, "log_format")) {
            config.logging.format = parse_log_format(format->get<std::string>());
        }
        if (const json* file = find_value(observability, "log_file");
            file != nullptr && !file->is_null()) {
            if (const auto path = path_at(*file, "/observability/log_file")) {
                config.logging.file = path_to_utf8(*path);
            }
        }
        if (const json* file = find_value(observability, "status_file");
            file != nullptr && !file->is_null()) {
            config.status_file = path_at(*file, "/observability/status_file");
        }
        duration_at(find_value(observability, "status_interval"), "/observability/status_interval",
                    config.status_interval);
    }

    const LoadedDocument& document_;
    ValidationReport& report_;
    const fs::path directory_;
};

// A path given by the caller: relative to the current directory.
std::optional<fs::path> caller_path(const std::string& text, const char* what,
                                    ValidationReport& report) {
    std::error_code ec;
    fs::path path = text.empty() ? fs::path() : fs::absolute(path_from_utf8(text), ec);
    if (text.empty() || ec) {
        Diagnostic diagnostic;
        diagnostic.code = ErrorCode::INVALID_ARGUMENT;
        diagnostic.message =
            std::string("the ") + what + " " + single_quoted(text) +
            (text.empty() ? " is empty" : " cannot be made absolute: " + ec.message());
        report.add(std::move(diagnostic));
        return std::nullopt;
    }
    return path.lexically_normal();
}

void apply_overrides(const WorkflowOptions& overrides, WorkflowConfig& config,
                     ValidationReport& report) {
    const auto count = [&](const std::optional<std::uint64_t>& value, const char* what,
                           auto&& assign) {
        if (!value) {
            return;
        }
        if (*value > kMaxJsonSafeInteger) {
            Diagnostic diagnostic;
            diagnostic.code = ErrorCode::INVALID_ARGUMENT;
            diagnostic.message = std::string("the ") + what + " " + std::to_string(*value) +
                                 " exceeds the largest allowed value, " +
                                 std::to_string(kMaxJsonSafeInteger) + " (2^53 - 1)";
            report.add(std::move(diagnostic));
            return;
        }
        assign(*value);
    };
    count(overrides.seed, "seed", [&](std::uint64_t seed) {
        config.seed = seed;
        config.overrides.seed = seed;
    });
    count(overrides.ticks, "number of ticks", [&](std::uint64_t ticks) {
        config.ticks = ticks;
        config.overrides.ticks = ticks;
    });
    if (overrides.output_dir) {
        if (auto path = caller_path(*overrides.output_dir, "output directory", report)) {
            config.output_dir = std::move(*path);
        }
    }
    if (overrides.trace_messages) {
        config.trace_messages = *overrides.trace_messages;
    }
    if (overrides.status_file) {
        if (auto path = caller_path(*overrides.status_file, "status file", report)) {
            config.status_file = std::move(*path);
        }
    }
    if (overrides.isolation) {
        // Every step's mode, the run's included; no field of the file set it. In the runtime's
        // process no step has a module host to share compiled models with, or to sandbox.
        const auto override_mode = [&overrides](Isolation& isolation) {
            isolation.mode = *overrides.isolation;
            isolation.mode_pointer.clear();
            isolation.mode_overridden = true;
            if (*overrides.isolation == IsolationMode::in_process) {
                isolation.share_compiled_models = false;
                isolation.share_pointer.clear();
                isolation.sandbox = SandboxProfile::none;
                isolation.sandbox_pointer.clear();
            }
        };
        override_mode(config.isolation);
        for (auto& entry : config.steps) {
            override_mode(entry.second.isolation);
        }
        config.overrides.isolation = *overrides.isolation;
    }
}

void add_error(ValidationReport& report, ErrorCode code, std::string message,
               SourceLocation location) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::error;
    diagnostic.code = code;
    diagnostic.message = std::move(message);
    diagnostic.location = std::move(location);
    report.add(std::move(diagnostic));
}

// Checks kind and format_version and validates the document against its schema. False when the
// document cannot be used.
bool check_document(const LoadedDocument& document, ValidationReport& report) {
    const json& root = document.json;
    if (!root.is_object()) {
        add_error(report, ErrorCode::SCHEMA_VIOLATION,
                  "a workflow configuration is a mapping (a JSON object)", locate(document, ""));
        return false;
    }
    const auto kind = root.find("kind");
    if (kind == root.end() || *kind != kKind) {
        add_error(report, ErrorCode::DOCUMENT_KIND_MISMATCH,
                  (kind == root.end() ? std::string("the document has no kind")
                                      : "the document's kind is " + kind->dump()) +
                      "; a workflow configuration has kind \"" + kKind + "\"",
                  locate(document, kind == root.end() ? "" : "/kind"));
        return false;
    }
    const auto format = root.find("format_version");
    std::optional<SemVer> version;
    if (format != root.end() && format->is_string()) {
        try {
            version = parse_semver(format->get_ref<const std::string&>());
        } catch (const Error&) {
            // Reported below.
        }
    }
    if (!version) {
        add_error(report, ErrorCode::SCHEMA_VIOLATION,
                  "format_version must be a version string such as \"0.1.0\"",
                  locate(document, format == root.end() ? "" : "/format_version"));
        return false;
    }
    const SchemaRegistry& registry = SchemaRegistry::builtin();
    const CompiledSchema* schema = nullptr;
    try {
        schema = &registry.for_kind(kKind, *version);
    } catch (const Error& e) {
        if (e.code() != ErrorCode::FORMAT_VERSION_UNSUPPORTED) {
            throw;
        }
        std::string supported;
        for (const auto& [entry_kind, entry_version] : registry.entries()) {
            if (entry_kind == kKind) {
                supported += (supported.empty() ? "" : ", ") + std::to_string(entry_version.major) +
                             "." + std::to_string(entry_version.minor) + ".x";
            }
        }
        add_error(report, ErrorCode::FORMAT_VERSION_UNSUPPORTED,
                  "format_version " + format->get<std::string>() +
                      " is not supported; this runtime reads workflow configurations of "
                      "format " +
                      (supported.empty() ? std::string("(none)") : supported),
                  locate(document, "/format_version"));
        return false;
    }
    const auto issues = schema->validate(root);
    for (const auto& issue : issues) {
        add_error(report, ErrorCode::SCHEMA_VIOLATION, issue.message,
                  locate(document, issue.pointer));
    }
    return issues.empty();
}

} // namespace

std::string_view isolation_mode_name(IsolationMode mode) {
    return mode == IsolationMode::process ? "process" : "in-process";
}

std::optional<IsolationMode> isolation_mode_from_name(std::string_view name) {
    if (name == "in-process") {
        return IsolationMode::in_process;
    }
    if (name == "process") {
        return IsolationMode::process;
    }
    return std::nullopt;
}

const std::vector<std::string>& observation_columns() {
    static const std::vector<std::string> columns = {
        "series_id", "variable_id", "time",        "value",  "observation_id", "unit",    "missing",
        "censoring", "lower_limit", "upper_limit", "weight", "group_id",       "batch_id"};
    return columns;
}

namespace {

// read_workflow_config() into `config`; false when the document cannot be used.
bool read_into(const fs::path& file, ValidationReport& report, const WorkflowOptions& overrides,
               WorkflowConfig& config) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(file, ec);
    config.file = (ec ? file : absolute).lexically_normal();

    try {
        config.document = load_document_file(file);
    } catch (const Error& e) {
        SourceLocation location = e.location();
        if (location.file.empty()) {
            location.file = path_to_utf8(file);
        }
        add_error(report, e.code(), e.what(), std::move(location));
        return false;
    }
    if (!check_document(config.document, report)) {
        return false;
    }

    Reader(config.document, report, config.file.parent_path()).read(config);
    apply_overrides(overrides, config, report);

    const bool replays = std::any_of(config.inputs.begin(), config.inputs.end(), [](const auto& e) {
        return e.second.source == "observation-csv";
    });
    if (!config.ticks && !replays) {
        add_error(report, ErrorCode::CONFIG_INVALID,
                  "run.ticks is required: no input is an observation-csv replay, so nothing "
                  "else would end the run",
                  locate(config.document, "/run/ticks"));
    }
    return true;
}

} // namespace

std::optional<WorkflowConfig> read_workflow_config(const fs::path& file, ValidationReport& report,
                                                   const WorkflowOptions& overrides) {
    WorkflowConfig config;
    if (!read_into(file, report, overrides, config)) {
        return std::nullopt;
    }
    return config;
}

WorkflowConfig load_workflow_config(const fs::path& file, ValidationReport& report) {
    // When the document cannot be used, read_into() has set only the file and, if it parsed,
    // the document.
    WorkflowConfig config;
    (void)read_into(file, report, WorkflowOptions{}, config);
    return config;
}

} // namespace praenora::detail
