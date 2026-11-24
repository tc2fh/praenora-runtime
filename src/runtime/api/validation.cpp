#include "api/validation.hpp"

#include "compat/compat.hpp"
#include "core/duration.hpp"
#include "core/paths.hpp"
#include "core/seed.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"
#include "isolation/child_process.hpp"
#include "isolation/sandbox.hpp"
#include "loader/loaded_module.hpp"
#include "observability/logging.hpp"
#include "platform/platform.hpp"
#include "provider/provider_loader.hpp"

#include <praenora/runtime/version.hpp>

#include <algorithm>
#include <iterator>
#include <set>
#include <system_error>
#include <tuple>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string identity(const std::string& name, const SemVer& version) {
    return name + "@" + to_string(version);
}

template <typename Map> std::string list_keys(const Map& map) {
    std::string list;
    for (const auto& entry : map) {
        list += (list.empty() ? "" : ", ") + entry.first;
    }
    return list.empty() ? std::string("(none)") : list;
}

json timeouts_json(const Timeouts& timeouts) {
    return {{"invoke", format_duration(timeouts.invoke)},
            {"lifecycle", format_duration(timeouts.lifecycle)},
            {"cancel_grace", format_duration(timeouts.cancel_grace)}};
}

// A module that passed step 6: its manifest and the digest of its library file.
struct ResolvedModule {
    const ModuleManifest* manifest = nullptr;
    std::string library_sha256;
};

// A service that a step's module declares and that resolved (step 6), before one provider is
// chosen for each name and major.
struct StepService {
    std::string step, module;
    ServiceVersion requirement;       // as the manifest declares it
    const ProviderManifest* provider; // in the context's provider index
    std::uint32_t offered_minor = 0;
};

class Validator {
public:
    Validator(const RuntimeContext& context, ValidationReport& report)
        : context_(context), catalog_(context.catalog), report_(report),
          first_(report.diagnostics.size()) {}

    std::optional<ValidatedWorkflow> run(const fs::path& file, const WorkflowOptions& options) {
        // Steps 1 and 2.
        auto config = read_workflow_config(file, report_, options);
        if (!config) {
            return std::nullopt;
        }
        config_ = std::move(*config);
        // Step 3. A version the loader could not read is not looked up: the configuration then
        // holds 0.0.0 in its place.
        if (reported_at("/workflow/version")) {
            return std::nullopt;
        }
        definition_ = catalog_.find_workflow(config_.workflow_name, config_.workflow_version);
        if (definition_ == nullptr) {
            workflow_not_found();
            return std::nullopt;
        }
        who_ = "workflow " + identity(definition_->name, definition_->version) + ": ";
        // Step 4, which needs every entry the definition uses.
        if (check_catalog_references()) {
            const ValidationReport graph = validate_graph(*definition_, catalog_);
            for (const Diagnostic& diagnostic : graph.diagnostics) {
                report_.add(diagnostic);
            }
            if (graph.ok()) {
                schedule_ = build_graph(*definition_, catalog_).schedule;
            }
        }
        check_bindings();   // step 5
        resolve_modules();  // step 6
        check_isolation();  // step 6, the steps in module hosts
        check_parameters(); // step 7
        build_sources();    // step 8
        if (errors() > 0) {
            return std::nullopt;
        }
        try {
            return finish(); // step 9
        } catch (const Error& e) {
            add(e.code(), e.location(), e.what());
            return std::nullopt;
        }
    }

private:
    std::size_t errors() const {
        std::size_t count = 0;
        for (std::size_t i = first_; i < report_.diagnostics.size(); ++i) {
            count += report_.diagnostics[i].severity == Severity::error ? 1 : 0;
        }
        return count;
    }

    // True when an error located at `pointer` of the configuration has been reported already.
    bool reported_at(const std::string& pointer) const {
        for (std::size_t i = first_; i < report_.diagnostics.size(); ++i) {
            const Diagnostic& d = report_.diagnostics[i];
            if (d.severity == Severity::error && d.location.pointer == pointer &&
                d.location.file == config_.document.origin) {
                return true;
            }
        }
        return false;
    }

    // True when an error located at `pointer` of the configuration, or below it, has been
    // reported already.
    bool reported_under(const std::string& pointer) const {
        const std::string below = pointer + "/";
        for (std::size_t i = first_; i < report_.diagnostics.size(); ++i) {
            const Diagnostic& d = report_.diagnostics[i];
            if (d.severity == Severity::error && d.location.file == config_.document.origin &&
                (d.location.pointer == pointer ||
                 d.location.pointer.compare(0, below.size(), below) == 0)) {
                return true;
            }
        }
        return false;
    }

    SourceLocation at(const std::string& pointer) const {
        return locate(config_.document, pointer);
    }

    // The location of `pointer` in the workflow definition's catalog entry.
    SourceLocation in_definition(const std::string& pointer) const {
        return catalog_.locate("workflow-definition", definition_->name, definition_->version,
                               pointer);
    }

    void add(ErrorCode code, SourceLocation location, std::string message, std::string step = {},
             std::string module = {}, Severity severity = Severity::error) {
        Diagnostic diagnostic;
        diagnostic.severity = severity;
        diagnostic.code = code;
        diagnostic.message = std::move(message);
        diagnostic.location = std::move(location);
        diagnostic.step = std::move(step);
        diagnostic.module = std::move(module);
        report_.add(std::move(diagnostic));
    }

    void workflow_not_found() {
        std::string versions;
        for (const auto& entry : catalog_.entries()) {
            if (entry.kind == "workflow-definition" && entry.id == config_.workflow_name) {
                versions += (versions.empty() ? "" : ", ") + entry.version;
            }
        }
        const std::string wanted = identity(config_.workflow_name, config_.workflow_version);
        if (versions.empty()) {
            add(ErrorCode::WORKFLOW_NOT_FOUND, at("/workflow/name"),
                "the catalog has no workflow definition named " + config_.workflow_name +
                    " (wanted " + wanted + ")");
        } else {
            add(ErrorCode::WORKFLOW_NOT_FOUND, at("/workflow/version"),
                "the catalog has no workflow definition " + wanted + "; its versions of " +
                    config_.workflow_name + " are " + versions);
        }
    }

    // CATALOG_ENTRY_NOT_FOUND for every message type and capability the definition uses that the
    // catalog lacks, located in the definition. True when all were found.
    bool check_catalog_references() {
        bool complete = true;
        const auto check_ports = [&](const std::map<std::string, WorkflowPortDef>& ports,
                                     const char* side) {
            for (const auto& [name, port] : ports) {
                if (catalog_.find_message_type(port.type, port.type_version) == nullptr) {
                    add(ErrorCode::CATALOG_ENTRY_NOT_FOUND,
                        in_definition(std::string("/") + side + "/" + json_pointer_escape(name)),
                        who_ + "workflow " + (side[0] == 'i' ? "input" : "output") + " '" + name +
                            "' has the unknown message type " +
                            identity(port.type, port.type_version));
                    complete = false;
                }
            }
        };
        check_ports(definition_->inputs, "inputs");
        check_ports(definition_->outputs, "outputs");
        for (const auto& [id, step] : definition_->steps) {
            const CapabilityDef* capability =
                catalog_.find_capability(step.capability, step.capability_version);
            if (capability == nullptr) {
                add(ErrorCode::CATALOG_ENTRY_NOT_FOUND,
                    in_definition("/steps/" + json_pointer_escape(id) + "/capability"),
                    who_ + "step '" + id + "' requires the unknown capability " +
                        identity(step.capability, step.capability_version),
                    id);
                complete = false;
                continue;
            }
            for (const auto* ports : {&capability->inputs, &capability->outputs}) {
                const char* const side = ports == &capability->inputs ? "inputs" : "outputs";
                for (const auto& [port, def] : *ports) {
                    if (catalog_.find_message_type(def.type, def.type_version) == nullptr) {
                        add(ErrorCode::CATALOG_ENTRY_NOT_FOUND,
                            catalog_.locate("capability-definition", capability->id,
                                            capability->version,
                                            std::string("/") + side + "/" +
                                                json_pointer_escape(port) + "/type"),
                            "capability " + identity(capability->id, capability->version) +
                                " (step '" + id + "'): port '" + port +
                                "' has the unknown message type " +
                                identity(def.type, def.type_version),
                            id);
                        complete = false;
                    }
                }
            }
        }
        return complete;
    }

    void check_bindings() {
        const WorkflowDef& def = *definition_;
        for (const auto& [id, step] : def.steps) {
            (void)step;
            if (config_.steps.count(id) == 0) {
                add(ErrorCode::BINDING_INVALID, at("/steps"),
                    who_ + "step '" + id +
                        "' is not bound to a module; bind every step: " + list_keys(def.steps),
                    id);
            }
        }
        for (const auto& [id, binding] : config_.steps) {
            (void)binding;
            if (def.steps.count(id) == 0) {
                add(ErrorCode::BINDING_INVALID, at("/steps/" + json_pointer_escape(id)),
                    who_ + "there is no step '" + id + "'; the steps are " + list_keys(def.steps),
                    id);
            }
        }
        for (const auto& [name, port] : def.inputs) {
            if (config_.inputs.count(name) == 0) {
                add(ErrorCode::INPUT_BINDING_INVALID, at("/inputs"),
                    who_ + "workflow input '" + name + "' (" +
                        identity(port.type, port.type_version) + ") is not bound to a data source");
            }
        }
        for (const auto& [name, binding] : config_.inputs) {
            (void)binding;
            if (def.inputs.count(name) == 0) {
                add(ErrorCode::INPUT_BINDING_INVALID, at("/inputs/" + json_pointer_escape(name)),
                    who_ + "there is no workflow input '" + name + "'; the inputs are " +
                        list_keys(def.inputs));
            }
        }
        // outputs.record: the outputs named, or every output when the file names none.
        const json* outputs = nullptr;
        if (const auto it = config_.document.json.find("outputs");
            it != config_.document.json.end()) {
            outputs = &*it;
        }
        if (outputs == nullptr || !outputs->contains("record")) {
            config_.record.clear();
            for (const auto& entry : def.outputs) {
                config_.record.push_back(entry.first);
            }
            return;
        }
        for (std::size_t i = 0; i < config_.record.size(); ++i) {
            if (def.outputs.count(config_.record[i]) == 0) {
                add(ErrorCode::CONFIG_INVALID, at("/outputs/record/" + std::to_string(i)),
                    who_ + "there is no workflow output '" + config_.record[i] +
                        "' to record; the outputs are " + list_keys(def.outputs));
            }
        }
    }

    void resolve_modules() {
        const ModuleIndex* index = &context_.modules;
        if (!config_.module_search_paths.empty()) {
            // The configuration's paths come after the caller's, before the environment's.
            local_index_ = ModuleIndex::scan(module_search_paths(
                context_.module_paths, config_.module_search_paths, context_.use_environment));
            index = &*local_index_;
            // The scan warns about a path that exists but is not a directory, and skips a missing
            // one silently: a configured path that does not exist is reported here.
            for (std::size_t i = 0; i < config_.module_search_paths.size(); ++i) {
                std::error_code ec;
                if (!fs::exists(config_.module_search_paths[i], ec)) {
                    add(ErrorCode::NOT_FOUND, at("/modules/search_paths/" + std::to_string(i)),
                        "the module search path '" + path_to_utf8(config_.module_search_paths[i]) +
                            "' does not exist",
                        {}, {}, Severity::warning);
                }
            }
        }
        for (const Diagnostic& diagnostic : index->diagnostics()) {
            report_.add(diagnostic);
        }
        for (const Diagnostic& diagnostic : context_.providers.diagnostics()) {
            report_.add(diagnostic);
        }

        std::map<fs::path, std::string> digests; // library digests, by manifest
        for (const auto& [id, step] : definition_->steps) {
            const auto binding = config_.steps.find(id);
            if (binding == config_.steps.end() ||
                reported_at("/steps/" + json_pointer_escape(id) + "/module/version")) {
                continue;
            }
            const std::string module =
                identity(binding->second.module_name, binding->second.module_version);
            Diagnostic why_not;
            const ModuleManifest* manifest = index->resolve(
                binding->second.module_name, binding->second.module_version, &why_not);
            if (manifest == nullptr) {
                why_not.step = id;
                if (why_not.location.file.empty()) {
                    why_not.location = at("/steps/" + json_pointer_escape(id) + "/module");
                }
                report_.add(std::move(why_not));
                continue;
            }
            if (catalog_.find_capability(step.capability, step.capability_version) == nullptr) {
                continue; // reported in step 3
            }
            if (auto problem = check_module_compat(
                    *manifest, CapabilityRef{step.capability, step.capability_version})) {
                problem->step = id;
                if (problem->module.empty()) {
                    problem->module = module;
                }
                report_.add(std::move(*problem));
                continue;
            }
            resolve_services(id, module, *manifest);
            auto digest = digests.find(manifest->manifest_path);
            if (digest == digests.end()) {
                try {
                    digest = digests.emplace(manifest->manifest_path, check_library_file(*manifest))
                                 .first;
                } catch (const Error& e) {
                    add(e.code(), e.location(), e.what(), id, module);
                    continue;
                }
            }
            modules_[id] = ResolvedModule{manifest, digest->second};
        }
        choose_providers();
    }

    // Resolves each service that step `id`'s module (`module`, name@version) declares on the
    // provider search paths: SERVICE_UNAVAILABLE for a required one that does not resolve.
    void resolve_services(const std::string& id, const std::string& module,
                          const ModuleManifest& manifest) {
        const ProviderIndex& providers = context_.providers;
        for (std::size_t i = 0; i < manifest.service_requirements.size(); ++i) {
            const ServiceRequirement& requirement = manifest.service_requirements[i];
            const ServiceResolution resolution = providers.resolve(requirement.service);
            if (resolution.provider != nullptr) {
                step_services_.push_back({id, module, requirement.service, resolution.provider,
                                          resolution.offered_minor});
                continue;
            }
            if (requirement.optional) {
                continue; // not used
            }
            Diagnostic diagnostic = service_unavailable(manifest, i, providers, resolution);
            diagnostic.step = id;
            report_.add(std::move(diagnostic));
        }
    }

    // The position of step `id` in the schedule, or, when the graph is invalid and there is no
    // schedule, in the definition's (step-id) order.
    std::size_t schedule_position(const std::string& id) const {
        const auto scheduled = std::find(schedule_.begin(), schedule_.end(), id);
        if (scheduled != schedule_.end()) {
            return static_cast<std::size_t>(scheduled - schedule_.begin());
        }
        const auto& steps = definition_->steps;
        return schedule_.size() +
               static_cast<std::size_t>(std::distance(steps.begin(), steps.find(id)));
    }

    // One provider for each service name and major of the run: the one that the highest minor
    // any step asks for resolved to (first found on a tie), which offers what every other step
    // asks for too. Then the library file of each provider the run uses, whose problem is reported
    // for the first step, in schedule order, that uses the provider.
    void choose_providers() {
        std::map<std::string, const StepService*> chosen; // by key
        for (const StepService& service : step_services_) {
            const auto [entry, added] = chosen.emplace(service_key(service.requirement), &service);
            if (!added && entry->second->requirement.minor < service.requirement.minor) {
                entry->second = &service;
            }
        }
        // The first user of each provider, in schedule order.
        std::map<fs::path, const StepService*> first_users; // by provider manifest
        for (const StepService& service : step_services_) {
            const fs::path& provider =
                chosen.at(service_key(service.requirement))->provider->manifest_path;
            const auto [entry, added] = first_users.emplace(provider, &service);
            if (!added &&
                schedule_position(service.step) < schedule_position(entry->second->step)) {
                entry->second = &service;
            }
        }
        std::map<fs::path, std::optional<std::string>> digests; // by provider manifest
        std::map<fs::path, std::shared_ptr<const ProviderManifest>> copies;
        for (const auto& [key, service] : chosen) {
            const ProviderManifest& provider = *service->provider;
            auto digest = digests.find(provider.manifest_path);
            if (digest == digests.end()) {
                std::optional<std::string> checked;
                try {
                    checked = check_provider_library_file(provider);
                } catch (const Error& e) {
                    const StepService& user = *first_users.at(provider.manifest_path);
                    add(e.code(), e.location(), e.what(), user.step, user.module);
                }
                digest = digests.emplace(provider.manifest_path, std::move(checked)).first;
            }
            if (!digest->second) {
                continue;
            }
            auto& copy = copies[provider.manifest_path];
            if (!copy) {
                copy = std::make_shared<const ProviderManifest>(provider);
            }
            ResolvedService resolved;
            resolved.service = {service->requirement.name, service->requirement.major,
                                service->offered_minor};
            resolved.provider = copy;
            resolved.library_sha256 = *digest->second;
            services_.emplace(key, std::move(resolved));
        }
        for (const StepService& service : step_services_) {
            const std::string key = service_key(service.requirement);
            if (services_.count(key) != 0) {
                step_service_keys_[service.step].insert(key);
            }
        }
    }

    // A step that runs in a module host is refused (UNSUPPORTED) where the platform has no
    // process isolation, located at the field that set the step's mode; the caller's override has
    // none. Its module's runtime services are loaded in its module host. A step in the runtime's
    // process that would share compiled models or have a restricted sandbox profile is refused
    // (CONFIG_INVALID), and a step in a module host with the restricted profile where the platform
    // has none (UNSUPPORTED), each located at the field that set it.
    void check_isolation() {
        for (const auto& [id, binding] : config_.steps) {
            const Isolation& isolation = binding.isolation;
            if (definition_->steps.count(id) == 0) {
                continue;
            }
            if (isolation.mode == IsolationMode::in_process && isolation.share_compiled_models) {
                add(ErrorCode::CONFIG_INVALID, at(isolation.share_pointer),
                    "step '" + id +
                        "' runs in the runtime's process (isolation mode \"in-process\"), and "
                        "share_compiled_models applies only to a step that runs in a module host",
                    id);
            }
            if (isolation.sandbox == SandboxProfile::restricted) {
                if (isolation.mode == IsolationMode::in_process) {
                    add(ErrorCode::CONFIG_INVALID, at(isolation.sandbox_pointer),
                        "step '" + id +
                            "' runs in the runtime's process (isolation mode \"in-process\"), "
                            "and sandbox \"restricted\" applies only to a step that runs in a "
                            "module host",
                        id);
                } else if (!restricted_sandbox_available()) {
                    add(ErrorCode::UNSUPPORTED, at(isolation.sandbox_pointer),
                        "step '" + id +
                            "' asks for the restricted sandbox profile (sandbox "
                            "\"restricted\"), which is available only on macOS in this version",
                        id);
                }
            }
            if (isolation.mode != IsolationMode::process || process_isolation_available()) {
                continue;
            }
            const SourceLocation location =
                isolation.mode_pointer.empty() ? SourceLocation{} : at(isolation.mode_pointer);
            add(ErrorCode::UNSUPPORTED, location,
                "step '" + id + "' runs in a module host (isolation mode \"process\"" +
                    (isolation.mode_overridden
                         ? ", set for every step by the run's isolation override"
                         : std::string()) +
                    "), and process isolation is not available on Windows in this version",
                id);
        }
    }

    void check_parameters() {
        for (const auto& [id, resolved] : modules_) {
            const StepBinding& binding = config_.steps.at(id);
            const ModuleManifest& manifest = *resolved.manifest;
            const std::string module = identity(manifest.name, manifest.version);
            if (!manifest.parameters) {
                parameters_[id] = binding.parameters;
                continue;
            }
            std::vector<SchemaIssue> issues;
            json parameters = manifest.parameters->apply_defaults(binding.parameters, issues);
            for (const auto& issue : issues) {
                add(ErrorCode::PARAMETERS_INVALID,
                    at("/steps/" + json_pointer_escape(id) + "/parameters" + issue.pointer),
                    issue.message + " (parameter schema of module " + module + ")", id, module);
            }
            if (issues.empty()) {
                parameters_[id] = std::move(parameters);
            }
        }
    }

    void build_sources() {
        for (const auto& [name, port] : definition_->inputs) {
            const auto binding = config_.inputs.find(name);
            const MessageTypeDef* type = catalog_.find_message_type(port.type, port.type_version);
            const std::string pointer = "/inputs/" + json_pointer_escape(name);
            // A source is not built from a path or columns mapping already reported, which would
            // only report the same problem again.
            if (binding == config_.inputs.end() || type == nullptr ||
                reported_at(pointer + "/path") || reported_under(pointer + "/columns")) {
                continue;
            }
            // The status tells a missing file from one whose status cannot be read, such as a
            // file in a directory that the process may not search (`ec` is set then).
            std::error_code ec;
            const fs::file_status status = fs::status(binding->second.path, ec);
            if (!fs::is_regular_file(status)) {
                std::string why = "is not a file";
                if (status.type() == fs::file_type::not_found) {
                    why = "does not exist";
                } else if (ec) {
                    why = "cannot be read: " + ec.message();
                }
                add(ErrorCode::TARGET_DATA_UNAVAILABLE, at(pointer + "/path"),
                    "input '" + name + "': the data file '" + path_to_utf8(binding->second.path) +
                        "' " + why);
                continue;
            }
            try {
                sources_[name] = make_input_source(binding->second, *type, config_.time);
                // What the source noticed and does not refuse: warnings, as found.
                for (const Diagnostic& note : sources_[name]->notes()) {
                    report_.add(note);
                }
            } catch (const Error& e) {
                SourceLocation location = e.location();
                if (location.file.empty()) {
                    location = at(pointer);
                }
                add(e.code(), std::move(location), e.what());
            }
        }
    }

    ValidatedWorkflow finish() {
        ValidatedWorkflow result;
        result.definition = definition_;
        result.isolation = json::object();
        result.graph = build_graph(*definition_, catalog_);
        std::map<fs::path, std::shared_ptr<const ModuleManifest>> manifests;
        for (const std::string& id : result.graph.schedule) {
            const ResolvedModule& module = modules_.at(id);
            auto& manifest = manifests[module.manifest->manifest_path];
            if (!manifest) {
                manifest = std::make_shared<const ModuleManifest>(*module.manifest);
                result.manifests.push_back(manifest);
            }
            const WorkflowStepDef& def = definition_->steps.at(id);
            const StepBinding& binding = config_.steps.at(id);
            ResolvedStep step;
            step.id = id;
            step.capability = catalog_.find_capability(def.capability, def.capability_version);
            step.manifest = manifest.get();
            step.parameters = parameters_.at(id);
            step.seed = derive_step_seed(config_.seed, id);
            step.timeouts = binding.timeouts;
            step.log_level = binding.log_level;
            step.library_sha256 = module.library_sha256;
            if (const auto keys = step_service_keys_.find(id); keys != step_service_keys_.end()) {
                step.services.assign(keys->second.begin(), keys->second.end());
            }
            step.isolation = binding.isolation;
            result.isolation[id] = {
                {"mode", std::string(isolation_mode_name(binding.isolation.mode))},
                {"share_compiled_models", binding.isolation.share_compiled_models},
                {"sandbox", std::string(sandbox_profile_name(binding.isolation.sandbox))}};
            result.steps.push_back(std::move(step));
        }
        result.services = services_;
        // The run record's execution.inputs: the counts of every source that has rows.
        result.input_counts = json::object();
        for (const auto& [name, source] : sources_) {
            if (const auto& counts = source->counts()) {
                result.input_counts[name] = {{"rows_total", counts->rows_total},
                                             {"rows_before_start", counts->rows_before_start}};
            }
        }
        result.sources = std::move(sources_);
        result.message_types = message_types();
        result.reproducibility = reproducibility(result);
        result.fingerprint = sha256_hex(canonical_json(result.reproducibility));
        result.manifest_locations = manifest_locations(result);
        // The search list that resolve_modules() used.
        result.module_search_paths =
            (local_index_ ? *local_index_ : context_.modules).scanned_search_paths();
        result.config = std::move(config_);
        return result;
    }

    json reproducibility(const ValidatedWorkflow& workflow) const {
        const VersionInfo versions = version_info();
        const WorkflowDef& def = *definition_;
        json section = json::object();
        section["versions"] = {
            {"runtime", versions.runtime},
            {"abi", {{"major", versions.abi_major}, {"minor", versions.abi_minor}}},
            {"pmp", versions.pmp}};
        section["workflow"] = {
            {"name", def.name}, {"version", to_string(def.version)}, {"digest", def.digest}};
        section["catalog"] = catalog_digests();
        section["time"] = {{"unit", config_.time.unit},
                           {"start", config_.time.start},
                           {"period", config_.time.period}};
        section["seed"] = config_.seed;
        // The run's length: a different tick count gives different outputs.
        section["ticks"] = config_.ticks ? json(*config_.ticks) : json(nullptr);
        section["timeouts"] = timeouts_json(config_.timeouts);

        json inputs = json::object();
        for (const auto& [name, source] : workflow.sources) {
            const InputBinding& binding = config_.inputs.at(name);
            json entry = {{"source", binding.source}, {"digest", source->digest()}};
            if (!binding.columns.empty()) {
                entry["columns"] = binding.columns;
            }
            inputs[name] = std::move(entry);
        }
        section["inputs"] = std::move(inputs);

        json services = json::object();
        for (const auto& [key, service] : workflow.services) {
            const ProviderManifest& provider = *service.provider;
            services[key] = {{"minor", service.service.minor},
                             {"provider",
                              {{"name", provider.name},
                               {"version", to_string(provider.version)},
                               {"manifest_sha256", provider.manifest_sha256},
                               {"library_sha256", service.library_sha256}}},
                             {"engine", provider.engine}};
        }
        section["services"] = std::move(services);

        json steps = json::object();
        for (const ResolvedStep& step : workflow.steps) {
            const ModuleManifest& manifest = *step.manifest;
            steps[step.id] = {
                {"capability",
                 {{"id", step.capability->id}, {"version", to_string(step.capability->version)}}},
                {"module",
                 {{"name", manifest.name},
                  {"version", to_string(manifest.version)},
                  {"manifest_sha256", manifest.manifest_sha256},
                  {"library_sha256", step.library_sha256},
                  {"sdk_version", to_string(manifest.sdk_version)}}},
                {"parameters", step.parameters},
                {"seed", step.seed},
                {"timeouts", timeouts_json(step.timeouts)},
                {"services", step.services}};
        }
        section["steps"] = std::move(steps);
        return section;
    }

    // The run record's execution.manifests: where each step's module manifest and each service's
    // provider manifest was found, by the keys of the reproducibility section's steps and
    // services. Every manifest of a run comes from a scan of the search paths, so each has a
    // location.
    static json manifest_locations(const ValidatedWorkflow& workflow) {
        const auto location_json = [](const std::optional<ManifestLocation>& location,
                                      const std::filesystem::path& manifest) {
            if (!location) {
                throw Error(ErrorCode::INTERNAL, "the manifest '" + path_to_utf8(manifest) +
                                                     "' was not found on a search path");
            }
            json search_path = {{"origin", to_string(location->origin)}};
            if (location->index) {
                search_path["index"] = *location->index;
            }
            return json{{"search_path", std::move(search_path)}, {"file", location->file}};
        };
        json steps = json::object();
        for (const ResolvedStep& step : workflow.steps) {
            steps[step.id] = location_json(step.manifest->location, step.manifest->manifest_path);
        }
        json services = json::object();
        for (const auto& [key, service] : workflow.services) {
            services[key] =
                location_json(service.provider->location, service.provider->manifest_path);
        }
        return {{"steps", std::move(steps)}, {"services", std::move(services)}};
    }

    // The message types of the definition's inputs and outputs and of its steps' capability ports,
    // by "<id>@<version>". Step 3 found them all.
    std::map<std::string, const MessageTypeDef*> message_types() const {
        std::map<std::string, const MessageTypeDef*> types;
        const auto add = [&](const std::string& id, const SemVer& version) {
            if (const MessageTypeDef* type = catalog_.find_message_type(id, version)) {
                types.emplace(identity(type->id, type->version), type);
            }
        };
        for (const auto* ports : {&definition_->inputs, &definition_->outputs}) {
            for (const auto& entry : *ports) {
                add(entry.second.type, entry.second.type_version);
            }
        }
        for (const auto& entry : definition_->steps) {
            const CapabilityDef* capability =
                catalog_.find_capability(entry.second.capability, entry.second.capability_version);
            if (capability == nullptr) {
                continue;
            }
            for (const auto* ports : {&capability->inputs, &capability->outputs}) {
                for (const auto& port : *ports) {
                    add(port.second.type, port.second.type_version);
                }
            }
        }
        return types;
    }

    // The digests of the capabilities and message types the definition uses, keyed
    // "<id>@<version>", or "<kind>:<id>@<version>" when two kinds share that id@version.
    json catalog_digests() const {
        std::set<std::tuple<std::string, std::string, std::string>> used; // kind, id@v, digest
        const auto message_type = [&](const std::string& id, const SemVer& version) {
            if (const MessageTypeDef* type = catalog_.find_message_type(id, version)) {
                used.emplace("message-type", identity(type->id, type->version), type->digest);
            }
        };
        for (const auto* ports : {&definition_->inputs, &definition_->outputs}) {
            for (const auto& entry : *ports) {
                message_type(entry.second.type, entry.second.type_version);
            }
        }
        for (const auto& entry : definition_->steps) {
            const CapabilityDef* capability =
                catalog_.find_capability(entry.second.capability, entry.second.capability_version);
            if (capability == nullptr) {
                continue;
            }
            used.emplace("capability-definition", identity(capability->id, capability->version),
                         capability->digest);
            for (const auto* ports : {&capability->inputs, &capability->outputs}) {
                for (const auto& port : *ports) {
                    message_type(port.second.type, port.second.type_version);
                }
            }
        }
        std::map<std::string, int> kinds_per_entry;
        for (const auto& [kind, entry, digest] : used) {
            (void)kind;
            (void)digest;
            ++kinds_per_entry[entry];
        }
        json digests = json::object();
        for (const auto& [kind, entry, digest] : used) {
            digests[kinds_per_entry[entry] > 1 ? kind + ":" + entry : entry] = digest;
        }
        return digests;
    }

    const RuntimeContext& context_;
    const Catalog& catalog_;
    ValidationReport& report_;
    const std::size_t first_;
    WorkflowConfig config_;
    const WorkflowDef* definition_ = nullptr;
    std::string who_;
    std::optional<ModuleIndex> local_index_;
    std::map<std::string, ResolvedModule> modules_;
    std::vector<std::string> schedule_; // once the graph is valid (step 4)
    std::vector<StepService> step_services_;                         // in the order resolved
    std::map<std::string, ResolvedService> services_;                // by key
    std::map<std::string, std::set<std::string>> step_service_keys_; // by step
    std::map<std::string, json> parameters_;
    std::map<std::string, std::unique_ptr<InputSource>> sources_;
};

// `path` made absolute and lexically normal, without a trailing separator.
fs::path normalized_directory(const fs::path& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    absolute = (ec ? path : absolute).lexically_normal();
    if (!absolute.has_filename() && absolute.has_relative_path()) {
        absolute = absolute.parent_path();
    }
    return absolute;
}

} // namespace

RuntimeContext make_runtime_context(const RuntimeOptions& options) {
    std::vector<fs::path> catalog_dirs;
    const auto add = [&catalog_dirs](const fs::path& dir) {
        const fs::path normal = normalized_directory(dir);
        if (std::find(catalog_dirs.begin(), catalog_dirs.end(), normal) == catalog_dirs.end()) {
            catalog_dirs.push_back(normal);
        }
    };
    for (const std::string& path : options.catalog_paths) {
        add(path_from_utf8(path));
    }
    if (options.use_environment) {
        if (const auto list = environment_variable("PRAENORA_CATALOG_PATH")) {
            for (const std::string& entry : split_path_list(*list)) {
                const fs::path dir = path_from_utf8(entry);
                std::error_code ec;
                if (fs::is_directory(dir, ec)) {
                    add(dir);
                }
            }
        }
    }
    RuntimeContext context;
    context.catalog = Catalog::load(catalog_dirs);
    context.modules =
        ModuleIndex::scan(module_search_paths(options.module_paths, {}, options.use_environment));
    context.providers = ProviderIndex::scan(
        provider_search_paths(options.provider_paths, options.use_environment));
    context.module_paths = options.module_paths;
    context.use_environment = options.use_environment;
    return context;
}

std::optional<ValidatedWorkflow> validate_workflow(const fs::path& config,
                                                   const RuntimeContext& context,
                                                   const WorkflowOptions& options,
                                                   ValidationReport& report) {
    return Validator(context, report).run(config, options);
}

Diagnostic service_unavailable(const ModuleManifest& manifest, std::size_t index,
                               const ProviderIndex& providers,
                               const ServiceResolution& resolution) {
    const ServiceVersion& service = manifest.service_requirements.at(index).service;
    std::string why;
    for (const auto& rejected : resolution.rejected) {
        why += (why.empty() ? "" : "; ") + std::string("provider ") +
               identity(rejected.provider->name, rejected.provider->version) + ": " +
               rejected.reason;
    }
    if (why.empty()) {
        std::string paths;
        for (const fs::path& path : providers.search_paths()) {
            paths += (paths.empty() ? "" : ", ") + path_to_utf8(path);
        }
        why = "no provider on the provider search paths (" +
              (paths.empty() ? std::string("none") : paths) + ") offers " + service.name;
    }
    Diagnostic diagnostic;
    diagnostic.severity = Severity::error;
    diagnostic.code = ErrorCode::SERVICE_UNAVAILABLE;
    diagnostic.module = identity(manifest.name, manifest.version);
    diagnostic.message = "module " + diagnostic.module + ": requires service " +
                         to_string(service) + ", which no provider can serve: " + why;
    diagnostic.location.file = path_to_utf8(manifest.manifest_path);
    diagnostic.location.pointer = "/requires/services/" + std::to_string(index);
    diagnostic.details_json = providers.unresolved_details(service, resolution);
    return diagnostic;
}

std::vector<ProviderLoad> provider_loads(const ValidatedWorkflow& workflow,
                                         const std::vector<std::string>& keys) {
    std::vector<ProviderLoad> loads;
    std::map<fs::path, std::size_t> by_manifest;
    for (const auto& [key, service] : workflow.services) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            continue;
        }
        const auto [entry, added] =
            by_manifest.emplace(service.provider->manifest_path, loads.size());
        if (added) {
            ProviderLoad load;
            load.provider = service.provider;
            load.library_sha256 = service.library_sha256;
            loads.push_back(std::move(load));
        }
        ProviderLoad& load = loads[entry->second];
        load.services.push_back(service.service);
        load.keys.push_back(key);
    }
    return loads;
}

std::vector<ProviderLoad> provider_loads(const ValidatedWorkflow& workflow, IsolationMode mode) {
    std::vector<std::string> keys;
    for (const ResolvedStep& step : workflow.steps) {
        if (step.isolation.mode == mode) {
            keys.insert(keys.end(), step.services.begin(), step.services.end());
        }
    }
    return provider_loads(workflow, keys);
}

std::vector<ChildProvider> child_providers(const ValidatedWorkflow& workflow,
                                           const std::vector<std::string>& keys, LogHub* logs) {
    std::vector<ChildProvider> providers;
    for (const ProviderLoad& load : provider_loads(workflow, keys)) {
        ChildProvider provider;
        provider.manifest = load.provider;
        provider.library_sha256 = load.library_sha256;
        provider.services = load.services;
        if (logs != nullptr) {
            provider.logger = logs->logger("provider." + load.provider->name);
        }
        providers.push_back(std::move(provider));
    }
    return providers;
}

} // namespace praenora::detail
