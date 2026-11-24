// praenora::Runtime: the runtime context (catalog, module index, provider index, services), the
// runtime's logging, validation of configurations and documents, module and provider listings, and
// opening workflows.
#include <praenora/runtime/runtime.hpp>

#include "api/graph_export.hpp"
#include "api/module_check.hpp"
#include "api/validation.hpp"
#include "api/workflow_impl.hpp"
#include "compat/compat.hpp"
#include "config/workflow_config.hpp"
#include "core/paths.hpp"
#include "core/semver.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"
#include "document/yaml_loader.hpp"
#include "graph/graph.hpp"
#include "io/run_record.hpp"
#include "loader/loaded_module.hpp"
#include "observability/logging.hpp"
#include "platform/platform.hpp"
#include "provider/provider_index.hpp"
#include "provider/provider_loader.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace praenora {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string identity(const std::string& name, const detail::SemVer& version) {
    return name + "@" + detail::to_string(version);
}

// The value of environment variable `name` read with `parse`, or nullopt when it is not set or is
// empty. An invalid value is CONFIG_INVALID, naming the variable.
template <class Parse>
auto environment_setting(const char* name, Parse parse) -> std::optional<decltype(parse(""))> {
    const std::optional<std::string> value = detail::environment_variable(name);
    if (!value || value->empty()) {
        return std::nullopt;
    }
    try {
        return parse(*value);
    } catch (const Error& e) {
        throw Error(ErrorCode::CONFIG_INVALID,
                    std::string("environment variable ") + name + ": " + e.what());
    }
}

void add(ValidationReport& report, ErrorCode code, std::string message, SourceLocation location,
         std::string step = {}, std::string module = {}, std::string details_json = {}) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::error;
    diagnostic.code = code;
    diagnostic.message = std::move(message);
    diagnostic.location = std::move(location);
    diagnostic.step = std::move(step);
    diagnostic.module = std::move(module);
    diagnostic.details_json = std::move(details_json);
    report.add(std::move(diagnostic));
}

// An error's details for a diagnostic: none when it has none ("{}").
std::string details_of(const Error& error) {
    return error.details_json() == "{}" ? std::string() : error.details_json();
}

ModuleInfo module_info(const detail::ModuleManifest& manifest) {
    ModuleInfo info;
    info.name = manifest.name;
    info.version = detail::to_string(manifest.version);
    info.manifest_path = detail::path_to_utf8(manifest.manifest_path);
    info.library_path = detail::path_to_utf8(manifest.library_path);
    info.os = manifest.os;
    info.arch = manifest.arch;
    for (const detail::CapabilityRef& capability : manifest.capabilities) {
        info.capabilities.push_back(identity(capability.id, capability.version));
    }
    return info;
}

ProviderInfo provider_info(const detail::IndexedProvider& entry) {
    const detail::ProviderManifest& manifest = entry.manifest;
    ProviderInfo info;
    info.name = manifest.name;
    info.version = detail::to_string(manifest.version);
    info.manifest_path = detail::path_to_utf8(manifest.manifest_path);
    info.library_path = detail::path_to_utf8(manifest.library_path);
    info.os = manifest.os;
    info.arch = manifest.arch;
    for (const detail::ServiceVersion& service : manifest.services) {
        info.services.push_back(detail::to_string(service));
    }
    info.engine_json = detail::canonical_json(manifest.engine);
    // As resolution reports it: shadowed first, then the first rule of usability it breaks.
    if (entry.shadowed_by) {
        info.problem = detail::shadowed_reason(*entry.shadowed_by);
    } else {
        info.problem = entry.problem.value_or(std::string());
    }
    info.usable = info.problem.empty();
    return info;
}

// Step 10 of the validation sequence: loads and starts each provider that `workflow`'s steps in
// mode "in-process" use, in this process, as a run does (load_provider() checks its library, table
// and identity, and that its library is still the one validation hashed; its records go to
// `logs`), then loads each module of `workflow` once for each place its steps run, as a run does:
// once in this process (load()) for its steps in mode "in-process", and in a module host for its
// steps in mode "process", once for each set of isolation settings they have (the sandbox profile
// and whether the module host shares compiled models), so that a load that fails under one step's
// settings only is reported (load_in_child(), with the providers of that step's services, which
// the module host loads and starts first; the module host's events go to the step's logger, the
// providers' records to `logs`), each checking that the library is still the one validation
// hashed before any of its code runs, its table and its identity against the manifest. Each
// problem is reported for the first step that uses the provider in this process, or the module in
// that place with those settings, with the loader's code, message, location and details. Providers
// stay loaded for the rest of the process; module libraries are released again, and module hosts
// ended, before this returns.
void check_loading(const detail::ValidatedWorkflow& workflow, ValidationReport& report,
                   detail::LogHub* logs) {
    for (const detail::ProviderLoad& load :
         detail::provider_loads(workflow, IsolationMode::in_process)) {
        const auto uses = [&load](const detail::ResolvedStep& step) {
            return step.isolation.mode == IsolationMode::in_process &&
                   std::any_of(load.keys.begin(), load.keys.end(), [&step](const auto& key) {
                       return std::find(step.services.begin(), step.services.end(), key) !=
                              step.services.end();
                   });
        };
        const auto user = std::find_if(workflow.steps.begin(), workflow.steps.end(), uses);
        try {
            detail::load_provider(*load.provider, load.services, load.library_sha256, logs);
        } catch (const Error& e) {
            add(report, e.code(), e.what(), e.location(),
                user == workflow.steps.end() ? std::string() : user->id,
                user == workflow.steps.end()
                    ? std::string()
                    : identity(user->manifest->name, user->manifest->version),
                details_of(e));
        }
    }
    // Whether two steps load a module alike: in this process, or in module hosts with the same
    // profile and sharing.
    const auto alike = [](const detail::ResolvedStep& a, const detail::ResolvedStep& b) {
        return a.isolation.mode == b.isolation.mode &&
               (a.isolation.mode == IsolationMode::in_process ||
                (a.isolation.sandbox == b.isolation.sandbox &&
                 a.isolation.share_compiled_models == b.isolation.share_compiled_models));
    };
    for (const auto& manifest : workflow.manifests) {
        const std::string module = identity(manifest->name, manifest->version);
        // The first step of each way its steps load it: in this process first, then in module
        // hosts, in the order of the steps.
        std::vector<const detail::ResolvedStep*> users;
        for (const IsolationMode mode : {IsolationMode::in_process, IsolationMode::process}) {
            for (const detail::ResolvedStep& step : workflow.steps) {
                if (step.manifest == manifest.get() && step.isolation.mode == mode &&
                    std::none_of(users.begin(), users.end(), [&](const detail::ResolvedStep* user) {
                        return alike(*user, step);
                    })) {
                    users.push_back(&step);
                }
            }
        }
        for (const detail::ResolvedStep* user : users) {
            const IsolationMode mode = user->isolation.mode;
            try {
                if (mode == IsolationMode::process) {
                    detail::ChildSettings child;
                    child.library_sha256 = user->library_sha256;
                    if (logs != nullptr) {
                        child.logger = logs->logger("module." + user->id);
                    }
                    child.providers = detail::child_providers(workflow, user->services, logs);
                    child.share_compiled_models = user->isolation.share_compiled_models;
                    child.sandbox = user->isolation.sandbox;
                    static_cast<void>(detail::LoadedModule::load_in_child(*manifest, child));
                } else {
                    static_cast<void>(
                        detail::LoadedModule::load(*manifest, nullptr, user->library_sha256));
                }
            } catch (const Error& e) {
                add(report, e.code(), e.what(), e.location(), user->id, module, details_of(e));
            }
        }
    }
}

// The kinds of the module ABI's documents (pmp-*): they carry pmp_version and no kind.
bool is_abi_kind(const std::string& kind) { return kind.compare(0, 4, "pmp-") == 0; }

// The kind of a document without a `kind` field: the one kind of the module ABI whose required
// fields it has. Reports DOCUMENT_KIND_MISMATCH and returns nullopt when none or several fit.
std::optional<std::string> abi_kind_of(const detail::LoadedDocument& document,
                                       ValidationReport& report) {
    const detail::SchemaRegistry& registry = detail::SchemaRegistry::builtin();
    const detail::SemVer pmp = detail::parse_semver(detail::kPmpVersion);
    std::vector<std::string> fitting, all;
    for (const auto& [kind, version] : registry.entries()) {
        if (!is_abi_kind(kind) || version.major != pmp.major || version.minor != pmp.minor) {
            continue;
        }
        all.push_back(kind);
        const json schema = json::parse(registry.text_for_kind(kind, version));
        bool fits = true;
        for (const json& field : schema.value("required", json::array())) {
            fits = fits && document.json.contains(field.get<std::string>());
        }
        if (fits) {
            fitting.push_back(kind);
        }
    }
    if (fitting.size() == 1) {
        return fitting.front();
    }
    std::string list;
    for (const std::string& kind : fitting.empty() ? all : fitting) {
        list += (list.empty() ? "" : ", ") + kind;
    }
    const std::string fit = fitting.empty() ? "none" : "more than one";
    add(report, ErrorCode::DOCUMENT_KIND_MISMATCH,
        "the document has no kind field, and it has the required fields of " + fit +
            " of the module ABI's documents, which have none (" + list + ")",
        detail::locate(document, ""));
    return std::nullopt;
}

// Validates `document` against the schema of its kind, reporting every problem.
void check_document(const detail::LoadedDocument& document, ValidationReport& report) {
    const json& root = document.json;
    if (!root.is_object()) {
        add(report, ErrorCode::DOCUMENT_KIND_MISMATCH,
            "a Praenora document is a JSON object (a YAML mapping)", detail::locate(document, ""));
        return;
    }
    const detail::SchemaRegistry& registry = detail::SchemaRegistry::builtin();
    std::string kind;
    std::string version_pointer;
    std::optional<detail::SemVer> version;
    const auto read_version = [&](const char* field) {
        version_pointer = std::string("/") + field;
        const auto found = root.find(field);
        if (found != root.end() && found->is_string()) {
            try {
                version = detail::parse_semver(found->get_ref<const std::string&>());
            } catch (const Error&) {
                // Reported below.
            }
        }
        if (!version) {
            add(report, ErrorCode::SCHEMA_VIOLATION,
                std::string(field) + " must be a version string such as \"0.1.0\"",
                detail::locate(document, found == root.end() ? "" : version_pointer));
        }
    };

    if (const auto found = root.find("kind"); found != root.end()) {
        std::set<std::string> kinds;
        for (const auto& entry : registry.entries()) {
            if (!is_abi_kind(entry.first)) {
                kinds.insert(entry.first);
            }
        }
        if (!found->is_string() || kinds.count(found->get<std::string>()) == 0) {
            std::string list;
            for (const std::string& known : kinds) {
                list += (list.empty() ? "" : ", ") + known;
            }
            add(report, ErrorCode::DOCUMENT_KIND_MISMATCH,
                "the document's kind is " + found->dump() + "; the kinds are " + list,
                detail::locate(document, "/kind"));
            return;
        }
        kind = found->get<std::string>();
        read_version("format_version");
    } else {
        const auto abi_kind = abi_kind_of(document, report);
        if (!abi_kind) {
            return;
        }
        kind = *abi_kind;
        if (root.contains("pmp_version")) {
            read_version("pmp_version");
        } else {
            version = detail::parse_semver(detail::kPmpVersion); // pmp-result may omit it
        }
    }
    if (!version) {
        return;
    }

    const detail::CompiledSchema* schema = nullptr;
    try {
        schema = &registry.for_kind(kind, *version);
    } catch (const Error& e) {
        if (e.code() != ErrorCode::FORMAT_VERSION_UNSUPPORTED) {
            throw;
        }
        std::string supported;
        for (const auto& [entry_kind, entry_version] : registry.entries()) {
            if (entry_kind == kind) {
                supported += (supported.empty() ? "" : ", ") + std::to_string(entry_version.major) +
                             "." + std::to_string(entry_version.minor) + ".x";
            }
        }
        add(report, ErrorCode::FORMAT_VERSION_UNSUPPORTED,
            "version " + detail::to_string(*version) + " of " + kind +
                " documents is not supported; this runtime reads " + supported,
            detail::locate(document, version_pointer));
        return;
    }
    for (const detail::SchemaIssue& issue : schema->validate(root)) {
        add(report, ErrorCode::SCHEMA_VIOLATION, issue.message + " (schema " + schema->name() + ")",
            detail::locate(document, issue.pointer));
    }
}

// `path` made absolute, with the symbolic links of its existing part resolved, and lexically
// normal.
fs::path resolved_path(const fs::path& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    const fs::path canonical = fs::weakly_canonical(absolute, ec);
    return (ec ? absolute : canonical).lexically_normal();
}

// True when `path` is `base` or lies below it, comparing whole components as written.
bool at_or_under(const fs::path& path, const fs::path& base) {
    auto part = path.begin();
    for (const fs::path& base_part : base) {
        if (base_part.empty()) {
            continue; // a trailing separator
        }
        if (part == path.end() || *part != base_part) {
            return false;
        }
        ++part;
    }
    return true;
}

// True when `path` is `base` or lies below it: by whole components as written (which also serves
// paths that do not exist yet), or because `path`, or an existing ancestor of it, is the same
// file system object as `base` (another spelling on a case-insensitive file system, a hard link).
bool lies_in(const fs::path& path, const fs::path& base) {
    if (at_or_under(path, base)) {
        return true;
    }
    std::error_code ec;
    if (!fs::exists(base, ec)) {
        return false;
    }
    for (fs::path ancestor = path; !ancestor.empty(); ancestor = ancestor.parent_path()) {
        if (fs::equivalent(ancestor, base, ec)) { // false, setting ec, when either is missing
            return true;
        }
        if (ancestor == ancestor.parent_path()) {
            break; // the root
        }
    }
    return false;
}

} // namespace

struct Runtime::Impl {
    RuntimeOptions options;
    // PRAENORA_LOG_LEVEL and PRAENORA_LOG_FORMAT, read once (with use_environment).
    std::optional<LogLevel> environment_level;
    std::optional<LogFormat> environment_format;
    std::shared_ptr<detail::LogHub> logs;
    std::shared_ptr<spdlog::logger> logger; // praenora.runtime
    std::shared_ptr<const detail::RuntimeContext> context;

    // The logging of a run of `config` with the overrides in `overrides`, each setting on its own:
    // level and format from the overrides, the environment, the configuration (when the document
    // sets them), then RuntimeOptions::logging; the file from the overrides (relative to the
    // current directory; empty for none), the configuration, then RuntimeOptions::logging;
    // to_stderr from RuntimeOptions::logging.
    LogOptions run_logging(const detail::WorkflowConfig& config,
                           const WorkflowOptions& overrides) const {
        LogOptions logging = options.logging;
        const json& document = config.document.json;
        const auto observability = document.find("observability");
        if (observability != document.end() && observability->is_object()) {
            if (observability->contains("log_level")) {
                logging.level = config.logging.level;
            }
            if (observability->contains("log_format")) {
                logging.format = config.logging.format;
            }
        }
        if (!config.logging.file.empty()) {
            logging.file = config.logging.file;
        }
        if (environment_level) {
            logging.level = *environment_level;
        }
        if (environment_format) {
            logging.format = *environment_format;
        }
        if (overrides.log_level) {
            logging.level = *overrides.log_level;
        }
        if (overrides.log_format) {
            logging.format = *overrides.log_format;
        }
        if (overrides.log_file) {
            logging.file.clear();
            if (!overrides.log_file->empty()) {
                std::error_code ec;
                const fs::path file = detail::path_from_utf8(*overrides.log_file);
                const fs::path absolute = fs::absolute(file, ec);
                logging.file = detail::path_to_utf8((ec ? file : absolute).lexically_normal());
            }
        }
        return logging;
    }

    // CONFIG_INVALID when a log file lies at or under a path that the run's output directory `dir`
    // manages (outputs/, trace/, status.json, run-record.json): an overwrite would remove it while
    // it is open, and log files are kept across runs. Checks the run's log file (`run`, from the
    // overrides, the configuration or RuntimeOptions::logging) and the runtime's own, before
    // anything is created or removed: by their resolved absolute paths, and, where the paths
    // exist, by asking the file system whether they are the same objects (lies_in()).
    void refuse_managed_log_files(const fs::path& dir, const detail::WorkflowConfig& config,
                                  const WorkflowOptions& overrides, const LogOptions& run) const {
        struct LogFile {
            std::string file, what;
            SourceLocation location;
        };
        std::vector<LogFile> files;
        if (!run.file.empty()) {
            LogFile log{run.file, "the run's log file (RuntimeOptions::logging.file)", {}};
            if (overrides.log_file) {
                log.what = "the run's log file (WorkflowOptions::log_file)";
            } else if (!config.logging.file.empty()) {
                log.what = "the run's log file (observability.log_file)";
                log.location = detail::locate(config.document, "/observability/log_file");
            }
            files.push_back(std::move(log));
        }
        if (!options.logging.file.empty() && options.logging.file != run.file) {
            files.push_back({options.logging.file,
                             "the runtime's log file (RuntimeOptions::logging.file)",
                             {}});
        }
        // Compared resolved; named as the output directory was given.
        const std::vector<fs::path> managed = detail::managed_output_paths(resolved_path(dir));
        const std::vector<fs::path> named = detail::managed_output_paths(dir);
        for (const LogFile& log : files) {
            const fs::path file = resolved_path(detail::path_from_utf8(log.file));
            for (std::size_t i = 0; i < managed.size(); ++i) {
                if (lies_in(file, managed[i])) {
                    throw Error(ErrorCode::CONFIG_INVALID,
                                log.what + " '" + log.file + "' lies in '" +
                                    detail::path_to_utf8(named[i]) +
                                    "', which the run's output directory manages and an overwrite "
                                    "removes, while log files are kept across runs; put the log "
                                    "file elsewhere, for example in '" +
                                    detail::path_to_utf8(dir / "logs") + "'",
                                log.location);
                }
            }
        }
    }
};

Runtime::Runtime(RuntimeOptions options) {
    auto impl = std::make_shared<Impl>();
    if (options.use_environment) {
        impl->environment_level =
            environment_setting("PRAENORA_LOG_LEVEL", detail::parse_log_level);
        impl->environment_format =
            environment_setting("PRAENORA_LOG_FORMAT", detail::parse_log_format);
    }
    LogOptions logging = options.logging;
    if (impl->environment_level) {
        logging.level = *impl->environment_level;
    }
    if (impl->environment_format) {
        logging.format = *impl->environment_format;
    }
    impl->logs = std::make_shared<detail::LogHub>(std::move(logging));
    impl->logger = impl->logs->logger("praenora.runtime");
    impl->context =
        std::make_shared<const detail::RuntimeContext>(detail::make_runtime_context(options));
    impl->options = std::move(options);

    const auto utf8_paths = [](const std::vector<fs::path>& paths) {
        json list = json::array();
        for (const fs::path& path : paths) {
            list.push_back(detail::path_to_utf8(path));
        }
        return list;
    };
    detail::log_event(*impl->logger, LogLevel::debug, "runtime ready",
                      {{"event", "runtime.ready"},
                       {"module_search_paths", utf8_paths(impl->context->modules.search_paths())},
                       {"modules", impl->context->modules.entries().size()},
                       {"provider_search_paths",
                        utf8_paths(impl->context->providers.search_paths())},
                       {"providers", impl->context->providers.entries().size()},
                       {"catalog_entries", impl->context->catalog.entries().size()}});
    impl_ = std::move(impl);
}

Runtime::~Runtime() = default;
Runtime::Runtime(const Runtime&) = default;
Runtime& Runtime::operator=(const Runtime&) = default;

ValidationReport Runtime::validate(const std::string& config_path,
                                   const ValidateOptions& options) const {
    ValidationReport report;
    const std::optional<detail::ValidatedWorkflow> workflow = detail::validate_workflow(
        detail::path_from_utf8(config_path), *impl_->context, WorkflowOptions{}, report);
    if (workflow && options.load) {
        check_loading(*workflow, report, impl_->logs.get());
    }
    return report;
}

ValidationReport Runtime::validate_document(const std::string& path) const {
    ValidationReport report;
    detail::LoadedDocument document;
    try {
        document = detail::load_document_file(detail::path_from_utf8(path));
    } catch (const Error& e) {
        SourceLocation location = e.location();
        if (location.file.empty()) {
            location.file = path;
        }
        add(report, e.code(), e.what(), std::move(location));
        return report;
    }
    check_document(document, report);
    return report;
}

std::vector<ModuleInfo> Runtime::list_modules() const {
    std::vector<ModuleInfo> modules;
    for (const detail::ModuleManifest* manifest : impl_->context->modules.entries()) {
        modules.push_back(module_info(*manifest));
    }
    return modules;
}

std::vector<ProviderInfo> Runtime::list_providers() const {
    std::vector<ProviderInfo> providers;
    for (const detail::IndexedProvider& entry : impl_->context->providers.entries()) {
        providers.push_back(provider_info(entry));
    }
    return providers;
}

ModuleInfo Runtime::inspect_module(const std::string& name,
                                   const std::optional<std::string>& version, bool load) const {
    const detail::ModuleIndex& index = impl_->context->modules;
    std::optional<detail::SemVer> wanted;
    if (version) {
        wanted = detail::parse_semver(*version);
    } else {
        // The highest version for this platform; failing that, the highest for any platform, which
        // resolve() then explains (PLATFORM_MISMATCH).
        const detail::HostPlatform host = detail::host_platform();
        std::optional<detail::SemVer> any;
        for (const detail::ModuleManifest* manifest : index.entries()) {
            if (manifest->name != name) {
                continue;
            }
            if (!any || *any < manifest->version) {
                any = manifest->version;
            }
            if (manifest->os == host.os && manifest->arch == host.arch &&
                (!wanted || *wanted < manifest->version)) {
                wanted = manifest->version;
            }
        }
        if (!wanted) {
            wanted = any;
        }
        if (!wanted) {
            std::string paths;
            for (const fs::path& path : index.search_paths()) {
                paths += (paths.empty() ? "" : ", ") + detail::path_to_utf8(path);
            }
            throw Error(ErrorCode::MODULE_NOT_FOUND,
                        "no module named " + name + " on the module search paths (" +
                            (paths.empty() ? std::string("none") : paths) + ")");
        }
    }
    Diagnostic why_not;
    const detail::ModuleManifest* manifest = index.resolve(name, *wanted, &why_not);
    if (manifest == nullptr) {
        throw Error(why_not.code, why_not.message, why_not.location);
    }
    ModuleInfo info = module_info(*manifest);
    if (load) {
        const std::shared_ptr<detail::LoadedModule> loaded = detail::LoadedModule::load(*manifest);
        info.describe_json =
            loaded->describe().dump(-1, ' ', false, json::error_handler_t::replace);
        info.library_sha256 = loaded->library_sha256();
    }
    return info;
}

ModuleCheckReport Runtime::check_module(const std::string& manifest_path,
                                        const std::optional<std::string>& vectors_path) const {
    std::optional<fs::path> vectors;
    if (vectors_path) {
        vectors = detail::path_from_utf8(*vectors_path);
    }
    return detail::check_module(detail::path_from_utf8(manifest_path), vectors, *impl_->context,
                                impl_->logs->logger("module.check"), {}, impl_->logs.get());
}

std::string Runtime::graph(const GraphRequest& request) const {
    const detail::Catalog& catalog = impl_->context->catalog;
    if (request.config_path.has_value() == request.workflow.has_value()) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "a graph request names exactly one of a workflow configuration (config_path) "
                    "and a workflow (<name>@<version>)");
    }
    const detail::WorkflowDef* definition = nullptr;
    if (request.workflow) {
        const std::string& text = *request.workflow;
        const std::size_t at = text.find('@');
        if (at == std::string::npos || at == 0 || at + 1 == text.size()) {
            throw Error(ErrorCode::INVALID_ARGUMENT,
                        "'" + text + "' does not name a workflow as <name>@<version>");
        }
        const std::string name = text.substr(0, at);
        const detail::SemVer version = detail::parse_semver(text.substr(at + 1));
        definition = catalog.find_workflow(name, version);
        if (definition == nullptr) {
            throw Error(ErrorCode::WORKFLOW_NOT_FOUND,
                        "the catalog has no workflow definition " + identity(name, version));
        }
    } else {
        // Only the workflow the configuration pins matters here; problems elsewhere in it do not.
        ValidationReport report;
        const std::optional<detail::WorkflowConfig> config =
            detail::read_workflow_config(detail::path_from_utf8(*request.config_path), report);
        for (const Diagnostic& diagnostic : report.diagnostics) {
            if (diagnostic.severity == Severity::error &&
                (!config || diagnostic.location.pointer.compare(0, 9, "/workflow") == 0)) {
                throw Error(diagnostic.code,
                            "cannot tell which workflow the configuration pins: " +
                                diagnostic.message,
                            diagnostic.location, report.to_json());
            }
        }
        if (!config) {
            throw Error(ErrorCode::INTERNAL,
                        "the workflow configuration could not be read, and "
                        "no error says why",
                        SourceLocation{}, report.to_json());
        }
        definition = catalog.find_workflow(config->workflow_name, config->workflow_version);
        if (definition == nullptr) {
            throw Error(ErrorCode::WORKFLOW_NOT_FOUND,
                        "the catalog has no workflow definition " +
                            identity(config->workflow_name, config->workflow_version),
                        detail::locate(config->document, "/workflow"));
        }
    }
    // An invalid definition: every problem, located in its catalog entry, in the details. Each
    // problem's message already names the workflow ("workflow <name>@<version>: ...").
    const ValidationReport problems = detail::validate_graph(*definition, catalog);
    if (const Diagnostic* first = problems.first_error()) {
        throw Error(first->code, "invalid workflow definition: " + first->message, first->location,
                    problems.to_json());
    }
    return detail::export_graph(*definition, catalog, request.format);
}

std::vector<CatalogEntryInfo> Runtime::list_catalog() const {
    std::vector<CatalogEntryInfo> entries;
    for (const detail::CatalogEntrySummary& entry : impl_->context->catalog.entries()) {
        entries.push_back({entry.kind, entry.id, entry.version, entry.status, entry.summary});
    }
    return entries;
}

std::string Runtime::catalog_entry(const std::string& id, const std::string& version) const {
    const detail::Catalog& catalog = impl_->context->catalog;
    const detail::SemVer wanted = detail::parse_semver(version);
    if (const json* entry = catalog.raw_entry(id, wanted)) {
        return detail::canonical_json(*entry);
    }
    std::string kinds;
    std::size_t count = 0;
    for (const char* kind : {"capability-definition", "message-type", "workflow-definition"}) {
        if (catalog.raw_entry(kind, id, wanted) != nullptr) {
            kinds += (kinds.empty() ? "" : " and ") + std::string(kind);
            ++count;
        }
    }
    const std::string name = identity(id, wanted);
    if (count > 1) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "catalog entries of more than one kind are named " + name + " (" + kinds +
                        "); look an entry up by its kind instead");
    }
    std::string versions;
    for (const detail::CatalogEntrySummary& entry : catalog.entries()) {
        if (entry.id == id) {
            versions += (versions.empty() ? "" : ", ") + entry.version;
        }
    }
    throw Error(
        ErrorCode::CATALOG_ENTRY_NOT_FOUND,
        "the catalog has no entry " + name +
            (versions.empty() ? std::string() : "; its versions of " + id + " are " + versions));
}

std::vector<std::string> Runtime::list_schemas() const {
    std::vector<std::string> schemas;
    for (const auto& [kind, version] : detail::SchemaRegistry::builtin().entries()) {
        schemas.push_back(kind + "@" + std::to_string(version.major) + "." +
                          std::to_string(version.minor));
    }
    std::sort(schemas.begin(), schemas.end());
    return schemas;
}

std::string Runtime::schema(const std::string& kind,
                            const std::optional<std::string>& format_version) const {
    const detail::SchemaRegistry& registry = detail::SchemaRegistry::builtin();
    std::optional<detail::SemVer> version;
    if (format_version) {
        // major.minor names the schema as well as a whole version does.
        std::string text = *format_version;
        if (std::count(text.begin(), text.end(), '.') == 1) {
            text += ".0";
        }
        version = detail::parse_semver(text);
    } else {
        std::set<std::string> kinds;
        for (const auto& [entry_kind, entry_version] : registry.entries()) {
            kinds.insert(entry_kind);
            if (entry_kind == kind && (!version || *version < entry_version)) {
                version = entry_version;
            }
        }
        if (!version) {
            std::string list;
            for (const std::string& known : kinds) {
                list += (list.empty() ? "" : ", ") + known;
            }
            throw Error(ErrorCode::FORMAT_VERSION_UNSUPPORTED,
                        "there is no schema of documents of kind '" + kind + "'; the kinds are " +
                            list);
        }
    }
    return std::string(registry.text_for_kind(kind, *version));
}

std::unique_ptr<Workflow> Runtime::open(const std::string& config_path,
                                        const WorkflowOptions& options) const {
    ValidationReport report;
    std::optional<detail::ValidatedWorkflow> workflow = detail::validate_workflow(
        detail::path_from_utf8(config_path), *impl_->context, options, report);
    if (!workflow) {
        const Diagnostic* first = report.first_error();
        if (first == nullptr) {
            throw Error(ErrorCode::INTERNAL, "workflow validation failed without an error",
                        SourceLocation{}, report.to_json());
        }
        throw Error(first->code,
                    "workflow validation failed: " + std::to_string(report.error_count()) +
                        " error(s)",
                    first->location, report.to_json());
    }
    const LogOptions logging = impl_->run_logging(workflow->config, options);
    if (const std::optional<fs::path>& dir = workflow->config.output_dir) {
        impl_->refuse_managed_log_files(*dir, workflow->config, options, logging);
    }
    auto impl = std::make_unique<Workflow::Impl>(impl_->context, std::move(*workflow), logging,
                                                 options.overwrite);
    impl->diagnostics = std::move(report);
    return std::unique_ptr<Workflow>(new Workflow(std::move(impl)));
}

std::string Runtime::read_status_file(const std::string& path) {
    const fs::path file = detail::path_from_utf8(path);
    detail::check_document_size(file);
    std::string text;
    try {
        text = detail::read_file_bytes(file);
    } catch (const Error& e) {
        SourceLocation location;
        location.file = path;
        throw Error(ErrorCode::IO_ERROR, std::string("cannot read the status file: ") + e.what(),
                    std::move(location));
    }
    const json document = detail::parse_json_text(text, path);
    const json* kind = nullptr;
    if (document.is_object()) {
        if (const auto found = document.find("kind"); found != document.end()) {
            kind = &*found;
        }
    }
    if (kind == nullptr || *kind != "status-snapshot") {
        SourceLocation location;
        location.file = path;
        location.pointer = kind != nullptr ? "/kind" : "";
        throw Error(
            ErrorCode::DOCUMENT_KIND_MISMATCH,
            "'" + path + "' is not a status file: " +
                (kind != nullptr ? "its kind is " + kind->dump() : std::string("it has no kind")) +
                ", and a status file has kind \"status-snapshot\"",
            std::move(location));
    }
    return text;
}

bool ModuleCheckReport::passed() const {
    return std::none_of(checks.begin(), checks.end(),
                        [](const CheckResult& check) { return check.status == "fail"; });
}

std::string ModuleCheckReport::to_json() const {
    json list = json::array();
    for (const CheckResult& check : checks) {
        list.push_back({{"id", check.id}, {"status", check.status}, {"message", check.message}});
    }
    const json document = {{"kind", "module-check-report"},
                           {"format_version", version_info().formats.at("module-check-report")},
                           {"passed", passed()},
                           {"checks", std::move(list)}};
    return document.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace praenora
