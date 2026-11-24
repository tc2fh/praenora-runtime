#include "facade.hpp"

#include <frontend.hpp>

#include <praenora/runtime/runtime.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace praenora::python {

namespace {

using nlohmann::json;

constexpr const char* kErrorPrefix = "PRAENORA_ERROR_JSON:";

// Compact JSON, with bytes that are not UTF-8 replaced.
std::string dump(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Runs `body` and returns its result. What it throws leaves as the facade's errors (facade.hpp):
// a praenora::Error, and any other std::exception except std::bad_alloc as INTERNAL, become
// std::runtime_error("PRAENORA_ERROR_JSON:" + Error::to_json()).
template <class Body> auto guarded(Body&& body) -> decltype(body()) {
    try {
        return body();
    } catch (const Error& error) {
        throw std::runtime_error(kErrorPrefix + error.to_json());
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        const Error internal(ErrorCode::INTERNAL, std::string("internal error: ") + error.what());
        throw std::runtime_error(kErrorPrefix + internal.to_json());
    }
}

Error argument_error(const std::string& message) {
    return Error(ErrorCode::INVALID_ARGUMENT, message);
}

// A value in a message: a string in single quotes, anything else as JSON.
std::string shown(const json& value) {
    return value.is_string() ? "'" + value.get<std::string>() + "'" : dump(value);
}

std::optional<std::string> given(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    return text;
}

RuntimeOptions runtime_options(const std::vector<std::string>& module_paths,
                               const std::vector<std::string>& catalog_paths,
                               const std::vector<std::string>& provider_paths = {}) {
    RuntimeOptions options;
    options.module_paths = module_paths;
    options.provider_paths = provider_paths;
    options.catalog_paths = catalog_paths;
    options.use_environment = true;
    return options;
}

// The report as JSON when it holds no error; otherwise the failure that the praenora tool reports
// for it (frontend::validation_failure()), with the report as the details.
std::string report_or_failure(const ValidationReport& report, const std::string& what) {
    if (report.first_error() == nullptr) {
        return report.to_json();
    }
    throw frontend::validation_failure(report, what, report.to_json());
}

// The values of the WorkflowHandle options (facade.hpp).

std::uint64_t count_option(const std::string& key, const json& value) {
    if (!value.is_number_unsigned() || value.get<std::uint64_t>() > frontend::kMaxCount) {
        throw argument_error(key + " " + shown(value) + " is not a whole number from 0 to " +
                             std::to_string(frontend::kMaxCount));
    }
    return value.get<std::uint64_t>();
}

bool flag_option(const std::string& key, const json& value) {
    if (!value.is_boolean()) {
        throw argument_error(key + " " + shown(value) + " is not true or false");
    }
    return value.get<bool>();
}

std::string text_option(const std::string& key, const json& value) {
    if (!value.is_string()) {
        throw argument_error(key + " " + shown(value) + " is not a path");
    }
    return value.get<std::string>();
}

std::vector<std::string> paths_option(const std::string& key, const json& value) {
    std::vector<std::string> paths;
    if (value.is_array()) {
        for (const json& item : value) {
            if (!item.is_string()) {
                break;
            }
            paths.push_back(item.get<std::string>());
        }
    }
    if (!value.is_array() || paths.size() != value.size()) {
        throw argument_error(key + " " + shown(value) + " is not a list of paths");
    }
    return paths;
}

LogLevel log_level_option(const json& value) {
    if (value.is_string()) {
        if (const auto level = frontend::log_level_from_name(value.get<std::string>())) {
            return *level;
        }
    }
    throw argument_error("log_level " + shown(value) +
                         " is not a log level: " + frontend::names_of(frontend::kLogLevels));
}

LogFormat log_format_option(const json& value) {
    if (value.is_string()) {
        if (const auto format = frontend::log_format_from_name(value.get<std::string>())) {
            return *format;
        }
    }
    throw argument_error("log_format " + shown(value) +
                         " is not a log format: " + frontend::names_of(frontend::kLogFormats));
}

IsolationMode isolation_option(const json& value) {
    if (value.is_string()) {
        if (const auto mode = frontend::isolation_mode(value.get<std::string>())) {
            return *mode;
        }
    }
    throw argument_error("isolation " + shown(value) + " is not an isolation mode: " +
                         frontend::names_of(frontend::kIsolationModes));
}

struct HandleOptions {
    RuntimeOptions runtime;
    WorkflowOptions workflow;
};

HandleOptions handle_options(const std::string& options_json) {
    json options;
    try {
        options = json::parse(options_json);
    } catch (const json::exception& error) {
        throw argument_error(std::string("the workflow options are not JSON: ") + error.what());
    }
    if (!options.is_object()) {
        throw argument_error("the workflow options are not a JSON object");
    }
    HandleOptions result;
    result.runtime.use_environment = true;
    WorkflowOptions& workflow = result.workflow;
    for (const auto& [key, value] : options.items()) {
        if (value.is_null()) {
            continue;
        }
        if (key == "module_paths") {
            result.runtime.module_paths = paths_option(key, value);
        } else if (key == "provider_paths") {
            result.runtime.provider_paths = paths_option(key, value);
        } else if (key == "catalog_paths") {
            result.runtime.catalog_paths = paths_option(key, value);
        } else if (key == "seed") {
            workflow.seed = count_option(key, value);
        } else if (key == "ticks") {
            workflow.ticks = count_option(key, value);
        } else if (key == "output_dir") {
            workflow.output_dir = text_option(key, value);
        } else if (key == "overwrite") {
            workflow.overwrite = flag_option(key, value);
        } else if (key == "trace_messages") {
            workflow.trace_messages = flag_option(key, value);
        } else if (key == "status_file") {
            workflow.status_file = text_option(key, value);
        } else if (key == "log_level") {
            workflow.log_level = log_level_option(value);
        } else if (key == "log_format") {
            workflow.log_format = log_format_option(value);
        } else if (key == "log_file") {
            workflow.log_file = text_option(key, value);
        } else if (key == "isolation") {
            workflow.isolation = isolation_option(value);
        } else {
            throw argument_error("unknown workflow option '" + key +
                                 "'; the options are catalog_paths, isolation, log_file, "
                                 "log_format, log_level, module_paths, output_dir, overwrite, "
                                 "provider_paths, seed, status_file, ticks and trace_messages");
        }
    }
    // The run's logging settings are the runtime's too, as with the praenora tool's --log-*
    // flags (which the environment would otherwise override for the runtime's own records).
    if (workflow.log_level) {
        result.runtime.logging.level = *workflow.log_level;
    }
    if (workflow.log_format) {
        result.runtime.logging.format = *workflow.log_format;
    }
    if (workflow.log_file) {
        result.runtime.logging.file = *workflow.log_file;
    }
    return result;
}

} // namespace

std::string version_json() {
    return guarded([] { return version_info_json(); });
}

std::string validate_json(const std::string& config, const std::vector<std::string>& module_paths,
                          const std::vector<std::string>& catalog_paths,
                          const std::vector<std::string>& provider_paths, bool load) {
    return guarded([&] {
        const Runtime runtime(runtime_options(module_paths, catalog_paths, provider_paths));
        ValidateOptions options;
        options.load = load;
        return report_or_failure(runtime.validate(config, options), config);
    });
}

std::string validate_document_json(const std::string& path) {
    return guarded([&] {
        const Runtime runtime(runtime_options({}, {}));
        return report_or_failure(runtime.validate_document(path), path);
    });
}

std::string list_modules_json(const std::vector<std::string>& module_paths) {
    return guarded([&] {
        const Runtime runtime(runtime_options(module_paths, {}));
        json modules = json::array();
        for (const ModuleInfo& module : runtime.list_modules()) {
            modules.push_back(frontend::module_json(module));
        }
        return dump(modules);
    });
}

std::string list_providers_json(const std::vector<std::string>& provider_paths) {
    return guarded([&] {
        const Runtime runtime(runtime_options({}, {}, provider_paths));
        json providers = json::array();
        for (const ProviderInfo& provider : runtime.list_providers()) {
            providers.push_back(frontend::provider_json(provider));
        }
        return dump(providers);
    });
}

std::string inspect_module_json(const std::string& name, const std::string& version, bool load,
                                const std::vector<std::string>& module_paths) {
    return guarded([&] {
        const Runtime runtime(runtime_options(module_paths, {}));
        return dump(frontend::module_json(runtime.inspect_module(name, given(version), load)));
    });
}

std::string check_module_json(const std::string& manifest, const std::string& vectors,
                              const std::vector<std::string>& catalog_paths,
                              const std::vector<std::string>& provider_paths) {
    return guarded([&] {
        const Runtime runtime(runtime_options({}, catalog_paths, provider_paths));
        const ModuleCheckReport report = runtime.check_module(manifest, given(vectors));
        if (!report.passed()) {
            throw frontend::check_failure(report);
        }
        return report.to_json();
    });
}

std::string graph(const std::string& config, const std::string& workflow, const std::string& format,
                  const std::vector<std::string>& module_paths,
                  const std::vector<std::string>& catalog_paths) {
    return guarded([&] {
        const std::optional<GraphFormat> graph_format = frontend::graph_format(format);
        if (!graph_format) {
            throw argument_error("format '" + format + "' is not " +
                                 frontend::names_of(frontend::kGraphFormats));
        }
        GraphRequest request;
        request.config_path = given(config);
        request.workflow = given(workflow);
        request.format = *graph_format;
        const Runtime runtime(runtime_options(module_paths, catalog_paths));
        return runtime.graph(request);
    });
}

std::string list_catalog_json(const std::string& kind,
                              const std::vector<std::string>& catalog_paths) {
    return guarded([&] {
        std::optional<std::string> wanted;
        if (!kind.empty()) {
            wanted = frontend::catalog_kind(kind);
            if (!wanted) {
                throw argument_error("kind '" + kind + "' is not " +
                                     frontend::names_of(frontend::kCatalogKinds));
            }
        }
        const Runtime runtime(runtime_options({}, catalog_paths));
        json entries = json::array();
        for (const CatalogEntryInfo& entry : runtime.list_catalog()) {
            if (!wanted || entry.kind == *wanted) {
                entries.push_back(frontend::catalog_entry_json(entry));
            }
        }
        return dump(entries);
    });
}

std::string catalog_entry_json(const std::string& id, const std::string& version,
                               const std::vector<std::string>& catalog_paths) {
    return guarded([&] {
        const Runtime runtime(runtime_options({}, catalog_paths));
        return runtime.catalog_entry(id, version);
    });
}

std::string list_schemas_json() {
    return guarded([] {
        const Runtime runtime(runtime_options({}, {}));
        return dump(runtime.list_schemas());
    });
}

std::string schema_json(const std::string& kind, const std::string& format_version) {
    return guarded([&] {
        const Runtime runtime(runtime_options({}, {}));
        return runtime.schema(kind, given(format_version));
    });
}

std::string read_status_json(const std::string& path) {
    return guarded([&] { return Runtime::read_status_file(path); });
}

WorkflowHandle::WorkflowHandle(const std::string& config, const std::string& options_json) {
    guarded([&] {
        const HandleOptions options = handle_options(options_json);
        const Runtime runtime(options.runtime);
        workflow_ = runtime.open(config, options.workflow);
    });
}

WorkflowHandle::~WorkflowHandle() = default;

void WorkflowHandle::start() {
    guarded([&] { workflow_->start(); });
}

std::string WorkflowHandle::tick() {
    return guarded([&] {
        const TickResult result = workflow_->tick();
        return dump({{"tick", result.tick},
                     {"logical_time", result.logical_time},
                     {"outputs", result.outputs},
                     {"finished", result.finished}});
    });
}

std::string WorkflowHandle::run(long long max_ticks) {
    return guarded([&] {
        if (max_ticks < -1 || max_ticks > static_cast<long long>(frontend::kMaxCount)) {
            throw argument_error("run(): max_ticks " + std::to_string(max_ticks) +
                                 " is not -1 (until the run is finished) or a whole number "
                                 "from 0 to " +
                                 std::to_string(frontend::kMaxCount));
        }
        std::optional<std::uint64_t> ticks;
        if (max_ticks >= 0) {
            ticks = static_cast<std::uint64_t>(max_ticks);
        }
        return dump(frontend::run_summary_json(workflow_->run(ticks)));
    });
}

std::string WorkflowHandle::status() const {
    return guarded([&] { return workflow_->status_json(); });
}

std::string WorkflowHandle::metrics() const {
    return guarded([&] { return workflow_->metrics_json(); });
}

std::string WorkflowHandle::latest_outputs() const {
    return guarded([&] { return workflow_->latest_outputs_json(); });
}

std::string WorkflowHandle::diagnostics() const {
    return guarded([&] { return dump(frontend::workflow_diagnostics_json(*workflow_)); });
}

std::string WorkflowHandle::run_record() const {
    return guarded([&] { return workflow_->run_record_json(); });
}

void WorkflowHandle::request_stop() { workflow_->request_stop(); }

void WorkflowHandle::close() { workflow_->close(); }

std::string WorkflowHandle::state() const {
    return guarded([&] { return to_string(workflow_->state()); });
}

std::string WorkflowHandle::fingerprint() const {
    return guarded([&] { return workflow_->fingerprint(); });
}

} // namespace praenora::python
