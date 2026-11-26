#include "api/module_check.hpp"

#include "api/module_check_cases.hpp"

#include "compat/compat.hpp"
#include "core/json_near.hpp"
#include "core/paths.hpp"
#include "core/semver.hpp"
#include "core/sha256.hpp"
#include "discovery/manifest.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"
#include "document/yaml_loader.hpp"
#include "engine/envelope.hpp"
#include "engine/result_check.hpp"
#include "instance/module_instance.hpp"
#include "loader/library_file.hpp"
#include "loader/library_gate.hpp"
#include "loader/loaded_module.hpp"
#include "observability/logging.hpp"
#include "provider/provider_loader.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/sdk/abi.h>
#include <praenora/sdk/status.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace praenora::detail {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// Every check id, in report order.
const char* const kCheckIds[] = {
    "manifest.schema",
    "manifest.semantics",
    "library.digest",
    "library.load",
    "library.entry_point",
    "api.header",
    "api.required_functions",
    "describe.schema",
    "describe.identity",
    "lifecycle.create_destroy",
    "lifecycle.full",
    "invoke.result_schema",
    "invoke.expected_outputs",
    "invoke.determinism",
    "invoke.malformed_input",
    "self_test",
    "unload",
};

std::string identity(const std::string& id, const SemVer& version) {
    return id + "@" + to_string(version);
}

std::string quoted(const std::string& text) { return "'" + text + "'"; }

std::string shown(std::chrono::nanoseconds duration) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return ms % 1000 == 0 ? std::to_string(ms / 1000) + " s" : std::to_string(ms) + " ms";
}

using module_check_text::first_issue;
using module_check_text::text_of;
using module_check_text::where;

// The message of a module's error document (or the text it wrote, cut short), after ": ".
std::string module_message(const std::string& error_json) {
    if (error_json.empty()) {
        return std::string();
    }
    const json document = json::parse(error_json, nullptr, false);
    if (document.is_object()) {
        const auto message = document.find("message");
        if (message != document.end() && message->is_string()) {
            return ": " + message->get<std::string>();
        }
    }
    std::string text = error_json.substr(0, 200);
    return ": " + json(text).dump(-1, ' ', false, json::error_handler_t::replace);
}

// How a call that did not succeed went.
std::string call_problem(const char* call, const CallResult& result,
                         std::chrono::nanoseconds timeout, std::chrono::nanoseconds grace) {
    const std::string name = std::string(call) + "()";
    switch (result.outcome) {
    case CallOutcome::succeeded:
        return name + " succeeded";
    case CallOutcome::failed:
        return name + " failed with status " + status_text(result.status) +
               module_message(result.error_json);
    case CallOutcome::timed_out:
        return name + " returned only after its deadline of " + shown(timeout) + " (status " +
               status_text(result.status) + ")";
    case CallOutcome::cancelled:
        return name + " was cancelled";
    case CallOutcome::unresponsive:
        return name + " did not return within its deadline of " + shown(timeout) +
               " and the cancellation grace of " + shown(grace) + "; the module is unresponsive";
    case CallOutcome::refused:
        return name + " was refused: " +
               (result.refused_by ? refusal_reason(*result.refused_by)
                                  : std::string("an abandoned call may still be running in the "
                                                "library"));
    case CallOutcome::crashed: // the contract checks run modules in this process: not reached
        return name + " crashed: its module host ended during the call";
    }
    return name + " did not succeed";
}

// Where two texts first differ, with a short excerpt of each from just before there.
std::string first_difference(const std::string& a, const std::string& b) {
    std::size_t at = 0;
    while (at < a.size() && at < b.size() && a[at] == b[at]) {
        ++at;
    }
    const std::size_t from = at > 20 ? at - 20 : 0;
    const auto excerpt = [from](const std::string& text) {
        std::string part = text.substr(from, 60);
        return json((from > 0 ? "..." : "") + part + (from + 60 < text.size() ? "..." : ""))
            .dump(-1, ' ', false, json::error_handler_t::replace);
    };
    return "at byte " + std::to_string(at) + ": " + excerpt(a) + " the first time, " + excerpt(b) +
           " the second";
}

// What one run of a case produced: the result of each tick, up to the first call that did not
// succeed, which `problem` describes.
struct CaseRun {
    std::vector<std::string> results;
    std::optional<std::string> problem;
};

// The results of the checks, kept by id and reported in the order of kCheckIds.
class Results {
public:
    void pass(const char* id, std::string message) { record(id, "pass", std::move(message)); }
    void fail(const char* id, std::string message) { record(id, "fail", std::move(message)); }
    void skip(const char* id, std::string message) { record(id, "skip", std::move(message)); }

    // Skips `id` and returns true unless `prerequisite` passed: with "skipped because
    // <prerequisite> failed", or with the reason a skipped prerequisite gave.
    bool skip_unless(const char* id, const char* prerequisite) {
        const Entry& entry = entries_.at(prerequisite);
        if (entry.status == "pass") {
            return false;
        }
        skip(id, entry.status == "fail" ? std::string("skipped because ") + prerequisite + " failed"
                                        : entry.message);
        return true;
    }

    ModuleCheckReport report() const {
        ModuleCheckReport report;
        for (const char* id : kCheckIds) {
            const auto entry = entries_.find(id);
            if (entry == entries_.end()) {
                throw Error(ErrorCode::INTERNAL,
                            std::string("contract check ") + id + " did not report a result");
            }
            report.checks.push_back({id, entry->second.status, entry->second.message});
        }
        return report;
    }

private:
    struct Entry {
        std::string status, message;
    };

    void record(const char* id, const char* status, std::string message) {
        entries_[id] = Entry{status, std::move(message)};
    }

    std::map<std::string, Entry> entries_;
};

class Checker {
public:
    Checker(fs::path manifest_path, std::optional<fs::path> vectors_path,
            const RuntimeContext& context, std::shared_ptr<spdlog::logger> logger,
            const CheckDeadlines& deadlines, LogHub* logs)
        : manifest_path_(std::move(manifest_path)), vectors_path_(std::move(vectors_path)),
          context_(context), catalog_(context.catalog), logger_(std::move(logger)),
          deadlines_(deadlines), logs_(logs) {}

    ModuleCheckReport run() {
        check_manifest_schema();
        check_manifest_semantics();
        if (manifest_) {
            prepare_cases(); // before any module code runs: unreadable vectors throw
        }
        check_loading();
        check_create_destroy();
        check_full_lifecycle();
        check_results();
        check_expected_outputs();
        check_determinism();
        check_malformed_input();
        check_self_test();
        check_unload();
        return results_.report();
    }

private:
    // -- The manifest.

    void check_manifest_schema() {
        const char* const id = "manifest.schema";
        LoadedDocument document;
        try {
            document = load_document_file(manifest_path_);
        } catch (const Error& error) {
            if (error.code() == ErrorCode::IO_ERROR) {
                throw; // the file is missing or unreadable: nothing to check
            }
            results_.fail(id, "cannot read the manifest: " + text_of(error));
            return;
        }
        const json& root = document.json;
        if (!root.is_object()) {
            results_.fail(id, where(locate(document, "")) + "a module manifest is a JSON object");
            return;
        }
        const auto kind = root.find("kind");
        if (kind == root.end() || *kind != "module-manifest") {
            results_.fail(id, where(locate(document, kind == root.end() ? "" : "/kind")) +
                                  "kind must be \"module-manifest\"");
            return;
        }
        const auto format = root.find("format_version");
        std::optional<SemVer> version;
        if (format != root.end() && format->is_string()) {
            try {
                version = parse_semver(format->get<std::string>());
            } catch (const Error&) {
                // Reported below.
            }
        }
        if (!version) {
            results_.fail(id,
                          where(locate(document, format == root.end() ? "" : "/format_version")) +
                              "format_version must be a version string such as \"0.1.0\"");
            return;
        }
        const CompiledSchema* schema = nullptr;
        try {
            schema = &SchemaRegistry::builtin().for_kind("module-manifest", *version);
        } catch (const Error& error) {
            results_.fail(id, where(locate(document, "/format_version")) + error.what());
            return;
        }
        const auto issues = schema->validate(root);
        if (!issues.empty()) {
            results_.fail(id, first_issue(document, issues) + " (schema " + schema->name() + ")");
            return;
        }
        results_.pass(id, "the manifest satisfies the schema " + schema->name());
    }

    // Keywords beside a "$ref" in a draft-07 parameters_schema are ignored, and so is a `default`
    // among them: the parameter would not get it. A note for the pass message of the check.
    static std::string parameters_schema_warning(const ModuleManifest& manifest) {
        const auto places = ignored_ref_siblings(manifest.parameters_schema);
        if (places.empty()) {
            return "";
        }
        std::string text = "; warning: parameters_schema is draft-07, which ignores the keywords "
                           "beside a $ref (a default there is never applied), at ";
        for (std::size_t i = 0; i < places.size(); ++i) {
            text += (i == 0 ? "" : ", ") + places[i];
        }
        return text + "; move them next to a plain property or declare 2020-12";
    }

    void check_manifest_semantics() {
        const char* const id = "manifest.semantics";
        if (results_.skip_unless(id, "manifest.schema")) {
            return;
        }
        ModuleManifest manifest;
        try {
            manifest = load_manifest(manifest_path_);
        } catch (const Error& error) {
            results_.fail(id, text_of(error));
            return;
        }
        // Platform, ABI and protocol; the capability rule always holds for the module's own.
        if (const auto problem = check_module_compat(manifest, manifest.capabilities.front())) {
            results_.fail(id, problem->message);
            return;
        }
        std::string capabilities;
        for (const CapabilityRef& capability : manifest.capabilities) {
            const std::string name = identity(capability.id, capability.version);
            const CapabilityDef* definition =
                catalog_.find_capability(capability.id, capability.version);
            if (definition == nullptr) {
                results_.fail(id, "the catalog has no capability " + name +
                                      ", which the manifest lists; add the catalog directory "
                                      "that defines it to the catalog paths");
                return;
            }
            for (const auto* ports : {&definition->inputs, &definition->outputs}) {
                for (const auto& [port, def] : *ports) {
                    if (catalog_.find_message_type(def.type, def.type_version) == nullptr) {
                        results_.fail(id, "the catalog has no message type " +
                                              identity(def.type, def.type_version) +
                                              ", which port " + quoted(port) + " of capability " +
                                              name + " carries");
                        return;
                    }
                }
            }
            capabilities += (capabilities.empty() ? "" : ", ") + name;
        }
        results_.pass(
            id, "the manifest is consistent and compatible with this runtime (" + manifest.os +
                    "-" + manifest.arch + ", ABI " + std::to_string(manifest.abi_major) + "." +
                    std::to_string(manifest.abi_minor) + ", PMP " +
                    to_string(manifest.pmp_version) + "); the catalog defines " + capabilities +
                    parameters_schema_warning(manifest));
        manifest_ = std::move(manifest);
    }

    // -- The library, its table and its identity: the checks of LoadedModule::load().

    void check_loading() {
        using Check = LoadedModule::Check;
        const struct {
            Check check;
            const char* id;
        } steps[] = {
            {Check::library_file, "library.digest"},
            {Check::library_open, "library.load"},
            {Check::entry_point, "library.entry_point"},
            {Check::table, "api.header"},
            {Check::required_functions, "api.required_functions"},
            {Check::describe, "describe.schema"},
            {Check::identity, "describe.identity"},
        };
        Check reached = Check::library_file;
        std::optional<Error> failure;
        bool services_failed = false;
        if (manifest_) {
            try {
                // The library file first (library.digest), then the services, which belong to
                // library.load: as in a run, no module code runs before they are there.
                static_cast<void>(check_library_file(*manifest_));
                reached = Check::library_open;
                services_failed = true;
                load_services();
                services_failed = false;
                loaded_ = LoadedModule::load(*manifest_, &reached);
            } catch (const Error& error) {
                failure = error;
            }
        }
        const char* prerequisite = "manifest.semantics";
        for (const auto& step : steps) {
            if (!results_.skip_unless(step.id, prerequisite)) {
                if (failure && step.check == reached) {
                    // A service's error names its code, which the check's status cannot carry;
                    // so does a library refused while an abandoned call may still run in it.
                    std::string message = text_of(*failure);
                    if (services_failed || failure->code() == ErrorCode::MODULE_UNRESPONSIVE) {
                        message =
                            std::string(error_code_info(failure->code()).name) + ": " + message;
                    }
                    results_.fail(step.id, std::move(message));
                } else {
                    results_.pass(step.id, loading_passed(step.check));
                }
            }
            prerequisite = step.id;
        }
    }

    // The message of a check of LoadedModule::load() that passed.
    std::string loading_passed(LoadedModule::Check check) const {
        using Check = LoadedModule::Check;
        const ModuleManifest& manifest = *manifest_;
        switch (check) {
        case Check::library_file:
            if (loaded_ != nullptr) {
                return "the library's SHA-256 is " + loaded_->library_sha256() +
                       (manifest.library_sha256 ? ", as the manifest requires"
                                                : " (the manifest pins none)");
            }
            return manifest.library_sha256
                       ? "the library file exists and has the SHA-256 the manifest requires"
                       : "the library file exists (the manifest pins no SHA-256)";
        case Check::library_open:
            return "the library " + path_to_utf8(manifest.library_path) + " loads" +
                   (services_note_.empty() ? std::string() : "; services: " + services_note_);
        case Check::entry_point:
            return std::string("the library exports ") + PRAENORA_MODULE_ENTRY_POINT;
        case Check::table:
            if (loaded_ != nullptr) {
                const praenora_api_header& header = loaded_->api().header;
                return std::string(PRAENORA_MODULE_ENTRY_POINT) + "(0) returns a table of ABI " +
                       std::to_string(header.abi_major) + "." + std::to_string(header.abi_minor) +
                       " with struct_size " + std::to_string(header.struct_size);
            }
            return std::string(PRAENORA_MODULE_ENTRY_POINT) +
                   "(0) returns a table with a valid header";
        case Check::required_functions: {
            std::string text = "describe, create, initialize, invoke, finalize and destroy are set";
            if (loaded_ != nullptr) {
                std::string optional;
                if (loaded_->api().diagnostics != nullptr) {
                    optional = "diagnostics";
                }
                if (loaded_->api().self_test != nullptr) {
                    optional += (optional.empty() ? "" : " and ") + std::string("self_test");
                }
                text += optional.empty() ? "; no optional function" : "; optional: " + optional;
            }
            return text;
        }
        case Check::describe:
            return "describe() writes a document that satisfies pmp-describe";
        case Check::identity:
            return "describe() agrees with the manifest: " +
                   identity(manifest.name, manifest.version) + ", ABI " +
                   std::to_string(manifest.abi_major) + "." + std::to_string(manifest.abi_minor) +
                   ", SDK " + to_string(manifest.sdk_version) + ", PMP " +
                   to_string(manifest.pmp_version) + ", " +
                   std::to_string(manifest.capabilities.size()) + " capabilit" +
                   (manifest.capabilities.size() == 1 ? "y" : "ies");
        }
        return "passed";
    }

    // The services the module declares, resolved on the runtime's provider search paths as for a
    // run, and their providers loaded and started, so that the module's instances are served
    // them as in a run. Throws SERVICE_UNAVAILABLE for a required service that does not resolve,
    // and the errors of the provider loader.
    void load_services() {
        const ModuleManifest& manifest = *manifest_;
        for (std::size_t i = 0; i < manifest.service_requirements.size(); ++i) {
            const ServiceRequirement& requirement = manifest.service_requirements[i];
            const std::string key = service_key(requirement.service);
            const ServiceResolution resolution = context_.providers.resolve(requirement.service);
            if (resolution.provider == nullptr) {
                if (requirement.optional) {
                    services_note_ += (services_note_.empty() ? "" : ", ") + key +
                                      " not available (optional)";
                    continue;
                }
                const Diagnostic unavailable =
                    service_unavailable(manifest, i, context_.providers, resolution);
                throw Error(unavailable.code, unavailable.message, unavailable.location,
                            unavailable.details_json);
            }
            const ServiceVersion offered{requirement.service.name, requirement.service.major,
                                         resolution.offered_minor};
            const LoadedProvider& provider =
                load_provider(*resolution.provider, {offered}, std::nullopt, logs_);
            provider_services_.add(offered.name, offered.major,
                                   provider.service(offered.name, offered.major));
            services_note_ += (services_note_.empty() ? "" : ", ") + key + " from provider " +
                              identity(resolution.provider->name, resolution.provider->version);
        }
    }

    // -- The cases: the test vectors, and the case of the lifecycle checks.

    void prepare_cases() {
        const ModuleManifest& manifest = *manifest_;
        const std::optional<fs::path> path = vectors_path_ ? vectors_path_ : manifest.test_vectors;
        if (!path) {
            vectors_problem_ = "no test vectors: the manifest names none (tests.vectors) and "
                               "none were given";
        } else {
            TestVectors vectors = read_test_vectors(*path, manifest, catalog_);
            cases_ = std::move(vectors.cases);
            vectors_invalid_ = std::move(vectors.invalid);
        }
        if (!cases_.empty()) {
            lifecycle_case_ = cases_.front();
            return;
        }
        lifecycle_case_ =
            default_case(manifest, catalog_, !vectors_invalid_.empty(), lifecycle_problem_);
    }

    // -- Calls into the module.

    std::unique_ptr<ModuleInstance> new_instance(const std::string& fingerprint) const {
        InstanceOptions options;
        options.step = kCheckStep;
        options.caller = "the contract checks";
        options.lifecycle_timeout = deadlines_.lifecycle;
        options.invoke_timeout = deadlines_.invoke;
        options.cancel_grace = deadlines_.cancel_grace;
        options.logger = logger_;
        options.services = &context_.services;
        options.provider_services = provider_services_;
        options.run_fingerprint = fingerprint;
        return std::make_unique<ModuleInstance>(loaded_, std::move(options));
    }

    // True when `result` succeeded. Otherwise `problem` says how the call went (`what` names it),
    // and a call that never returned is remembered: no later check calls into the module then.
    bool succeeded(const std::string& what, const char* call, const CallResult& result,
                   std::chrono::nanoseconds timeout, std::string& problem) {
        if (result.outcome == CallOutcome::succeeded) {
            return true;
        }
        problem = (what.empty() ? std::string() : what + ": ") +
                  call_problem(call, result, timeout, deadlines_.cancel_grace);
        // A refused call names an abandoned call, of this session or another run, that may still
        // be running in the library: nothing more can be called in it either.
        if ((result.outcome == CallOutcome::unresponsive ||
             result.outcome == CallOutcome::refused) &&
            !unresponsive_) {
            unresponsive_ = problem;
        }
        return false;
    }

    // Finalizes `instance` when it is ready, then destroys it. The first problem goes to
    // `problem` when that holds none yet.
    void finish(ModuleInstance& instance, const std::string& what,
                std::optional<std::string>& problem) {
        if (instance.state() == StepState::ready) {
            std::string text;
            const CallResult result = instance.finalize();
            if (!succeeded(what, "finalize", result, deadlines_.lifecycle, text) && !problem) {
                problem = text;
            }
            if (result.outcome == CallOutcome::refused) {
                return; // never destroyed unfinalized: its destructor keeps it, or finishes it
            }
        }
        if (instance.destroy() == CallOutcome::refused) {
            // The instance is kept, never destroyed (ModuleInstance's destructor), and nothing
            // more can be called in the library.
            CallResult refused;
            refused.outcome = CallOutcome::refused;
            refused.refused_by = instance.last_refusal();
            const std::string text =
                (what.empty() ? std::string() : what + ": ") +
                call_problem("destroy", refused, deadlines_.lifecycle, deadlines_.cancel_grace);
            if (!unresponsive_) {
                unresponsive_ = text;
            }
            if (!problem) {
                problem = text;
            }
        }
        if (instance.state() == StepState::unresponsive) {
            const std::string text = (what.empty() ? std::string() : what + ": ") +
                                     "the module is unresponsive (a call did not return within "
                                     "its deadline and the cancellation grace)";
            if (!unresponsive_) {
                unresponsive_ = text;
            }
            if (!problem) {
                problem = text;
            }
        }
    }

    // Creates and initializes an instance for case `c`; nullptr, with `problem` set, when either
    // call does not succeed (the instance is then torn down).
    std::unique_ptr<ModuleInstance> ready_instance(const CheckCase& c, std::string& problem) {
        auto instance = new_instance(c.fingerprint);
        std::optional<std::string> ignored;
        if (!succeeded("", "create", instance->create(create_document(c, *manifest_)),
                       deadlines_.lifecycle, problem) ||
            !succeeded("", "initialize", instance->initialize(initialize_document(c)),
                       deadlines_.lifecycle, problem)) {
            finish(*instance, "", ignored);
            return nullptr;
        }
        return instance;
    }

    // Runs case `c` once on a new instance: create, initialize, one invoke per tick, finalize
    // and destroy.
    CaseRun run_case(const CheckCase& c) {
        CaseRun run;
        std::string problem;
        auto instance = ready_instance(c, problem);
        if (instance == nullptr) {
            run.problem = problem;
            return run;
        }
        for (std::size_t t = 0; t < c.ticks.size(); ++t) {
            CallResult result = instance->invoke(invocation_document(c, t), t);
            if (!succeeded("tick " + std::to_string(t), "invoke", result, deadlines_.invoke,
                           problem)) {
                run.problem = problem;
                break;
            }
            run.results.push_back(std::move(result.output));
        }
        finish(*instance, "", run.problem);
        return run;
    }

    // Why a check that would call into the module is skipped, if it is.
    bool skipped_for_unresponsive(const char* id) {
        if (!unresponsive_) {
            return false;
        }
        results_.skip(id, "skipped because an earlier call never returned: " + *unresponsive_);
        return true;
    }

    // -- The lifecycle.

    void check_create_destroy() {
        const char* const id = "lifecycle.create_destroy";
        if (results_.skip_unless(id, "describe.identity")) {
            return;
        }
        if (!lifecycle_case_) {
            results_.skip(id, lifecycle_problem_);
            return;
        }
        auto instance = new_instance(lifecycle_case_->fingerprint);
        std::string problem;
        std::optional<std::string> teardown;
        const std::string create = create_document(*lifecycle_case_, *manifest_);
        if (!succeeded("", "create", instance->create(create), deadlines_.lifecycle, problem)) {
            finish(*instance, "", teardown);
            results_.fail(id, problem);
            return;
        }
        finish(*instance, "", teardown);
        if (teardown) {
            results_.fail(id, *teardown);
            return;
        }
        results_.pass(id, "create() and destroy() succeeded (" + lifecycle_case_->label + ")");
    }

    void check_full_lifecycle() {
        const char* const id = "lifecycle.full";
        if (results_.skip_unless(id, "lifecycle.create_destroy")) {
            return;
        }
        std::string problem;
        auto instance = ready_instance(*lifecycle_case_, problem);
        if (instance == nullptr) {
            results_.fail(id, problem);
            return;
        }
        std::optional<std::string> teardown;
        finish(*instance, "", teardown);
        if (teardown) {
            results_.fail(id, *teardown);
            return;
        }
        results_.pass(id, "create(), initialize(), finalize() and destroy() succeeded (" +
                              lifecycle_case_->label + ")");
    }

    // -- Invocations.

    // Checks one result of case `c` by the engine's acceptance rules; on success `outputs`
    // receives its outputs' data by port.
    std::optional<std::string> check_result(const CheckCase& c, const std::string& text,
                                            json& outputs) const {
        AcceptedResult accepted;
        const auto problem = check_invoke_result(
            text, *c.definition, true,
            [this](const PortDef& port) -> const MessageTypeDef& {
                return *catalog_.find_message_type(port.type, port.type_version);
            },
            accepted);
        if (problem) {
            return "the result is not acceptable: " + problem->message;
        }
        outputs = json::object();
        for (auto& [port, data] : accepted.outputs) {
            outputs[port] = std::move(data);
        }
        return std::nullopt;
    }

    void check_results() {
        const char* const id = "invoke.result_schema";
        // Invalid vectors fail this check whatever else happened, also when no instance could be
        // created: the vectors themselves are at fault.
        if (!vectors_invalid_.empty()) {
            results_.fail(id, vectors_invalid_);
            return;
        }
        if (results_.skip_unless(id, "lifecycle.full") || skipped_for_unresponsive(id)) {
            return;
        }
        if (cases_.empty()) {
            results_.skip(id, vectors_problem_);
            return;
        }
        std::size_t count = 0;
        for (const CheckCase& c : cases_) {
            CaseRun run = run_case(c);
            if (run.problem) {
                results_.fail(id, c.label + ": " + *run.problem);
                return;
            }
            std::vector<json> outputs(run.results.size());
            for (std::size_t t = 0; t < run.results.size(); ++t) {
                if (const auto problem = check_result(c, run.results[t], outputs[t])) {
                    results_.fail(id, c.label + ", tick " + std::to_string(t) + ": " + *problem);
                    return;
                }
            }
            count += run.results.size();
            first_runs_.push_back(std::move(run));
            outputs_.push_back(std::move(outputs));
        }
        results_.pass(id, "the " + std::to_string(count) + " result(s) of " +
                              std::to_string(cases_.size()) +
                              " case(s) satisfy pmp-result and the message types of their ports");
    }

    void check_expected_outputs() {
        const char* const id = "invoke.expected_outputs";
        if (results_.skip_unless(id, "invoke.result_schema")) {
            return;
        }
        std::size_t compared = 0;
        for (std::size_t i = 0; i < cases_.size(); ++i) {
            const CheckCase& c = cases_[i];
            for (std::size_t t = 0; t < c.ticks.size(); ++t) {
                const VectorTick& tick = c.ticks[t];
                if (!tick.expected) {
                    continue;
                }
                std::string difference;
                if (!json_near(outputs_[i][t], *tick.expected, tick.relative, tick.absolute,
                               &difference)) {
                    results_.fail(id, c.label + ", tick " + std::to_string(t) +
                                          ": the outputs differ from the expected ones (the "
                                          "result first) at " +
                                          difference);
                    return;
                }
                ++compared;
            }
        }
        if (compared == 0) {
            results_.skip(id, "the test vectors expect no outputs (no tick has an expect)");
            return;
        }
        results_.pass(id, "the outputs of " + std::to_string(compared) +
                              " tick(s) match the expected ones within their tolerance");
    }

    void check_determinism() {
        const char* const id = "invoke.determinism";
        if (results_.skip_unless(id, "invoke.result_schema") || skipped_for_unresponsive(id)) {
            return;
        }
        std::size_t count = 0;
        for (std::size_t i = 0; i < cases_.size(); ++i) {
            const CheckCase& c = cases_[i];
            const CaseRun second = run_case(c);
            if (second.problem) {
                results_.fail(
                    id, c.label + ": the second run failed, the first did not: " + *second.problem);
                return;
            }
            const std::vector<std::string>& first = first_runs_[i].results;
            for (std::size_t t = 0; t < first.size(); ++t) {
                if (second.results[t] != first[t]) {
                    results_.fail(id, c.label + ", tick " + std::to_string(t) +
                                          ": a second run gave another result, " +
                                          first_difference(first[t], second.results[t]));
                    return;
                }
            }
            count += first.size();
        }
        results_.pass(id, "a second run of the " + std::to_string(cases_.size()) +
                              " case(s) gave byte-identical results (" + std::to_string(count) +
                              " result(s))");
    }

    void check_malformed_input() {
        const char* const id = "invoke.malformed_input";
        if (results_.skip_unless(id, "lifecycle.full") || skipped_for_unresponsive(id)) {
            return;
        }
        std::string problem;
        auto instance = ready_instance(*lifecycle_case_, problem);
        if (instance == nullptr) {
            results_.fail(id, problem);
            return;
        }
        const CallResult result = instance->invoke("{", 0);
        const bool refused = result.outcome == CallOutcome::failed;
        if (!refused && result.outcome != CallOutcome::succeeded) {
            succeeded("", "invoke", result, deadlines_.invoke, problem);
        }
        std::optional<std::string> teardown;
        finish(*instance, "", teardown);
        if (refused) {
            results_.pass(id, "invoke() refused the malformed invocation document \"{\" with "
                              "status " +
                                  status_text(result.status));
        } else if (result.outcome == CallOutcome::succeeded) {
            results_.fail(id, "invoke() returned OK for the malformed invocation document "
                              "\"{\"; it must return an error status");
        } else {
            results_.fail(id, problem);
        }
    }

    // -- The rest.

    void check_self_test() {
        const char* const id = "self_test";
        if (results_.skip_unless(id, "describe.identity")) {
            return;
        }
        if (loaded_->api().self_test == nullptr) {
            if (manifest_->features.self_test) {
                results_.fail(id, "the manifest declares features.self_test, but the function "
                                  "table's self_test is NULL");
            } else {
                results_.skip(id, "the module provides no self_test (its table's self_test is "
                                  "NULL)");
            }
            return;
        }
        if (skipped_for_unresponsive(id)) {
            return;
        }
        auto instance = new_instance("");
        std::string problem;
        const CallResult result = instance->self_test();
        if (!succeeded("", "self_test", result, deadlines_.invoke, problem)) {
            results_.fail(id, problem);
            return;
        }
        const json report = json::parse(result.output, nullptr, false);
        if (!report.is_object() || !report.contains("passed") ||
            !report.at("passed").is_boolean()) {
            results_.fail(id, "self_test() did not write a self-test report "
                              "({\"passed\": <bool>, \"checks\": [...]})");
            return;
        }
        const json checks = report.value("checks", json::array());
        if (report.at("passed").get<bool>()) {
            results_.pass(id, "self_test() passed" +
                                  (checks.is_array() && !checks.empty()
                                       ? " (" + std::to_string(checks.size()) + " checks)"
                                       : std::string()));
            return;
        }
        std::string failed;
        if (checks.is_array()) {
            for (const json& check : checks) {
                if (check.is_object() && check.value("passed", true) == false) {
                    failed += (failed.empty() ? "" : "; ") +
                              check.value("name", std::string("(unnamed)")) + ": " +
                              check.value("message", std::string());
                }
            }
        }
        results_.fail(id, "self_test() reports a failure" +
                              (failed.empty() ? std::string() : ": " + failed));
    }

    void check_unload() {
        const char* const id = "unload";
        if (results_.skip_unless(id, "describe.identity")) {
            return;
        }
        if (unresponsive_) {
            results_.fail(id, "the library cannot be unloaded: a call into it never returned (" +
                                  *unresponsive_ + ")");
            return;
        }
        if (loaded_->pinned()) {
            results_.skip(id, "PRAENORA_DEBUG_KEEP_LIBRARIES=1 keeps module libraries loaded");
            return;
        }
        loaded_.reset(); // the last owner: the library is closed
        results_.pass(id, "every instance was destroyed and the library was released");
    }

    fs::path manifest_path_;
    std::optional<fs::path> vectors_path_;
    const RuntimeContext& context_;
    const Catalog& catalog_;
    std::shared_ptr<spdlog::logger> logger_;
    CheckDeadlines deadlines_;
    LogHub* logs_; // where the providers' records go, or none
    Results results_;
    ServiceRegistry provider_services_; // the module's provider services, once loaded
    std::string services_note_;         // what became of each service, for library.load

    std::optional<ModuleManifest> manifest_;  // once manifest.semantics passed
    std::shared_ptr<LoadedModule> loaded_;    // once describe.identity passed
    std::vector<CheckCase> cases_;            // the test vectors, when usable
    std::string vectors_problem_;             // why there are none
    std::string vectors_invalid_;             // what is wrong with them
    std::optional<CheckCase> lifecycle_case_; // the case of the lifecycle checks
    std::string lifecycle_problem_;           // why there is none
    std::vector<CaseRun> first_runs_;         // invoke.result_schema's runs, by case
    std::vector<std::vector<json>> outputs_;  // their outputs, by case and tick
    std::optional<std::string> unresponsive_; // the call that never returned
};

} // namespace

ModuleCheckReport check_module(const fs::path& manifest, const std::optional<fs::path>& vectors,
                               const RuntimeContext& context,
                               std::shared_ptr<spdlog::logger> logger,
                               const CheckDeadlines& deadlines, LogHub* logs) {
    return Checker(manifest, vectors, context, std::move(logger), deadlines, logs).run();
}

} // namespace praenora::detail
