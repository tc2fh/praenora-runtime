#include "engine/engine.hpp"

#include "core/duration.hpp"
#include "core/paths.hpp"
#include "core/time.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"
#include "engine/envelope.hpp"
#include "engine/result_check.hpp"
#include "isolation/child_settings.hpp"
#include "loader/library_file.hpp"
#include "loader/library_gate.hpp"
#include "observability/status_json.hpp"
#include "provider/provider_loader.hpp"

#include <praenora/sdk/status.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

// A module's error document longer than this is kept as truncated text.
constexpr std::size_t kMaxCauseBytes = 64 * 1024;
// Diagnostics longer than this are not embedded whole in an error's details.
constexpr std::size_t kMaxDiagnosticsBytes = 64 * 1024;

bool is_terminal(WorkflowState state) {
    return state == WorkflowState::completed || state == WorkflowState::stopped ||
           state == WorkflowState::failed;
}

std::string identity(const std::string& name, const SemVer& version) {
    return name + "@" + to_string(version);
}

std::string quoted(const std::string& text) { return "'" + text + "'"; }

// The module's error document of a call that failed: the JSON object it wrote (its code and
// message filled in from the call when missing), or {"code", "message"} made of its text.
json cause_of(const CallResult& result) {
    json cause;
    if (result.error_json.size() <= kMaxCauseBytes) {
        try {
            cause = parse_json_text(result.error_json, "error document");
        } catch (const Error&) {
            cause = nullptr;
        }
    }
    if (!cause.is_object()) {
        cause = json{{"code", result.status},
                     {"message", utf8_prefix(result.error_json, kMaxCauseBytes)}};
        if (result.error_json.size() > kMaxCauseBytes) {
            cause["truncated"] = true;
        }
    }
    if (!cause.contains("code")) {
        cause["code"] = result.status;
    }
    if (!cause.contains("message") || !cause.at("message").is_string()) {
        cause["message"] = "";
    }
    return cause;
}

// The module's error document of a call that did not succeed, when the module said why: none
// for an unresponsive or crashed call, which never returned, for a refused one, which never
// started, or for one that returned OK without an error document (late, after its deadline).
std::optional<json> module_cause(const CallResult& result) {
    if (result.outcome == CallOutcome::succeeded || result.outcome == CallOutcome::unresponsive ||
        result.outcome == CallOutcome::crashed || result.outcome == CallOutcome::refused ||
        (result.status == PRAENORA_STATUS_OK && result.error_json.empty())) {
        return std::nullopt;
    }
    return cause_of(result);
}

// A diagnostics document as an error's details hold it: the JSON it is (text that is not JSON as
// a string), or, beyond kMaxDiagnosticsBytes, {"truncated": true, "bytes": its size, "text": its
// first kMaxDiagnosticsBytes bytes}.
json bounded_diagnostics(const std::string& text) {
    if (text.size() > kMaxDiagnosticsBytes) {
        return {{"truncated", true},
                {"bytes", text.size()},
                {"text", utf8_prefix(text, kMaxDiagnosticsBytes)}};
    }
    try {
        return parse_json_text(text, "diagnostics");
    } catch (const Error&) {
        return text;
    }
}

std::string dump(const json& value) {
    // Text may come from modules: bytes that are not UTF-8 are replaced, not refused.
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Keeps an instance that may not be called (its call was refused: an abandoned call that it would
// have to wait for may still be running in its library): never finalized, destroyed or freed.
// Reachable from a static pointer, so leak checkers do not report it. Its runtime services are
// withdrawn first, as an abandoned instance's are: the registry goes away with the run, while the
// module's own threads may still ask for a service.
void keep_forever(std::unique_ptr<ModuleInstance> instance) noexcept {
    static auto* const mutex = new std::mutex;
    static auto* const kept = new std::vector<std::unique_ptr<ModuleInstance>>;
    instance->withdraw_services();
    try {
        const std::lock_guard<std::mutex> lock(*mutex);
        kept->push_back(std::move(instance));
    } catch (...) {
        static_cast<void>(instance.release()); // out of memory: leak it rather than destroy it
    }
}

// The record of `error`, with its location and details, stamped now.
ErrorRecord record_of(const Error& error, std::optional<std::uint64_t> tick) {
    return error_record_of(error, tick, now_unix_ns());
}

// The record of `error`, which a call of the teardown (`phase`: finalize or destroy) ended with:
// only its step's last error. Its details name the call, as a run failure's do, beside those of
// the error (how its module host ended, or that it was terminated).
ErrorRecord teardown_record_of(const Error& error, const char* phase) {
    ErrorRecord record = record_of(error, std::nullopt);
    json details = json::parse(record.details_json, nullptr, false);
    if (!details.is_object()) {
        details = json::object();
    }
    details["phase"] = phase;
    record.details_json = dump(details);
    return record;
}

// INTERNAL: an envelope the engine built around `what` in `tick` breaks pmp-envelope. Only a defect
// of the runtime can cause that, never the module or the data, so the error says so and blames
// neither. `details` (step, module and port, or input) gain the issues.
Error broken_envelope(const std::string& what, std::uint64_t tick,
                      const std::vector<SchemaIssue>& issues, json details, std::string pointer) {
    details["issues"] = issues_json(issues);
    SourceLocation location;
    location.pointer = std::move(pointer);
    return Error(ErrorCode::INTERNAL,
                 "internal error: the envelope of " + what + " at tick " + std::to_string(tick) +
                     " does not satisfy pmp-envelope: " + issues.front().pointer + ": " +
                     issues.front().message + " (a defect of the runtime)",
                 std::move(location), dump(details));
}

// The message of the exception being handled (call it in a handler only): what() of a
// std::exception, else a note that it is not one.
std::string current_exception_text() {
    try {
        throw;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "an exception that is not a std::exception";
    }
}

// What the teardown left undone for an instance in `state` that it may not call.
const char* left_undone(StepState state) {
    switch (state) {
    case StepState::ready:
        return "is neither finalized nor destroyed";
    case StepState::finalized:
        return "is finalized but not destroyed";
    default:
        return "is not destroyed";
    }
}

} // namespace

// One step of the run.
struct Engine::Step {
    const ResolvedStep* resolved = nullptr; // in workflow_.steps
    std::string module;                     // name@version
    std::shared_ptr<spdlog::logger> logger; // module.<step>, or none
    std::unique_ptr<ModuleInstance> instance;
    bool failed = false; // reported as failed in the metrics from now on
    bool crashed = false; // its module host crashed while it loaded the module: no instance
    // Set by the teardown when a call into the instance is refused: the abandoned call the
    // refusal named. The instance is then kept as it is.
    std::optional<LibraryCall> kept;
};

// Where a failure happened.
struct Engine::Failure {
    Failure(Step* step_, const char* phase_, std::optional<std::uint64_t> tick_ = std::nullopt,
            const CallResult* call_ = nullptr, CallOutcome counted_ = CallOutcome::failed)
        : step(step_), phase(phase_), tick(tick_), call(call_), counted(counted_) {}

    Step* step;                        // the step at fault, if any
    const char* phase;                 // load, create, initialize or invoke (with a step)
    std::optional<std::uint64_t> tick; // the tick it happened in, if any
    const CallResult* call;            // the call into the module, if it was one
    CallOutcome counted;               // how a failed invocation is counted
};

Engine::Engine(ValidatedWorkflow workflow, EngineSettings settings)
    : workflow_(std::move(workflow)), settings_(std::move(settings)),
      metrics_(settings_.metrics != nullptr ? *settings_.metrics
                                            : throw Error(ErrorCode::INVALID_ARGUMENT,
                                                          "the engine needs a metrics registry")) {
    if (workflow_.definition == nullptr || workflow_.steps.empty()) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "the engine needs a workflow from validate_workflow()");
    }
    fingerprint16_ = fingerprint16(workflow_.fingerprint);
    if (settings_.logs != nullptr) {
        logger_ = settings_.logs->logger("praenora.engine");
    } else {
        logger_ = std::make_shared<spdlog::logger>("praenora.engine");
        logger_->set_level(spdlog::level::off);
    }
    recorded_ = workflow_.config.record;
    std::sort(recorded_.begin(), recorded_.end());

    // Every message type the run handles must be known.
    for (const auto* ports : {&workflow_.definition->inputs, &workflow_.definition->outputs}) {
        for (const auto& entry : *ports) {
            message_type(entry.second.type, entry.second.type_version);
        }
    }
    metrics_.set_run_info(workflow_.definition->name, to_string(workflow_.definition->version),
                          workflow_.fingerprint);
    steps_.reserve(workflow_.steps.size());
    for (const ResolvedStep& resolved : workflow_.steps) {
        if (resolved.capability == nullptr || resolved.manifest == nullptr) {
            throw Error(ErrorCode::INVALID_ARGUMENT,
                        "step '" + resolved.id + "' was not resolved by validation");
        }
        for (const auto& entry : resolved.capability->outputs) {
            message_type(entry.second.type, entry.second.type_version);
        }
        Step step;
        step.resolved = &resolved;
        step.module = identity(resolved.manifest->name, resolved.manifest->version);
        if (settings_.logs != nullptr) {
            step.logger = settings_.logs->logger("module." + resolved.id);
            if (resolved.log_level) {
                set_log_level(*step.logger, *resolved.log_level);
            }
        }
        metrics_.register_step(resolved.id, resolved.manifest->name,
                               to_string(resolved.manifest->version));
        steps_.push_back(std::move(step));
    }
    metrics_.set_workflow_state(WorkflowState::created);
}

Engine::~Engine() { shutdown(); }

template <class Body> auto Engine::control(Body&& body) -> decltype(body()) {
    try {
        return body();
    } catch (const Error& error) {
        if (is_terminal(state_.load())) {
            throw; // the run is already over: this is how it ended
        }
        throw fail(error, Failure{nullptr, nullptr, current_tick_});
    } catch (const std::bad_alloc&) {
        if (is_terminal(state_.load())) {
            throw;
        }
        throw fail(Error(ErrorCode::RESOURCE_EXHAUSTED, "out of memory"),
                   Failure{nullptr, nullptr, current_tick_});
    } catch (const std::exception& e) {
        const Error error(ErrorCode::INTERNAL, std::string("internal error: ") + e.what());
        if (is_terminal(state_.load())) {
            throw error;
        }
        throw fail(error, Failure{nullptr, nullptr, current_tick_});
    }
}

void Engine::start() {
    const WorkflowState current = state_.load();
    if (current != WorkflowState::created) {
        throw Error(ErrorCode::INVALID_STATE,
                    "start(): the run has already started (state " + to_string(current) + ")");
    }
    control([this] { start_run(); });
}

void Engine::start_run() {
    set_state(WorkflowState::starting);
    const std::int64_t started_at = now_unix_ns();
    metrics_.mark_started(started_at);
    log(LogLevel::info, "run started",
        {{"event", "run.started"},
         {"workflow", identity(workflow_.definition->name, workflow_.definition->version)}});
    open_output(started_at);
    if (stop_flag_.load()) {
        stop_run(std::nullopt); // a run stopped before it started loads no provider either
        return;
    }
    load_providers();
    for (Step& step : steps_) {
        if (!start_step(step)) {
            return; // stopped
        }
    }
    set_state(WorkflowState::ready);
    update_end_reached();
    if (end_reached_.load()) {
        complete_run(); // a run of no ticks
    }
}

void Engine::load_providers() {
    // Only the providers of the steps in this process: a step in a module host has its own loaded
    // there (start_step()), and this process never loads a provider that only such steps use.
    for (const ProviderLoad& load : provider_loads(workflow_, IsolationMode::in_process)) {
        const LoadedProvider* provider = nullptr;
        try {
            provider =
                &load_provider(*load.provider, load.services, load.library_sha256, settings_.logs);
        } catch (const Error& error) {
            throw fail(error, Failure{nullptr, nullptr});
        }
        for (std::size_t i = 0; i < load.services.size(); ++i) {
            const ServiceVersion& service = load.services[i];
            provider_tables_[load.keys[i]] = provider->service(service.name, service.major);
        }
    }
}

void Engine::open_output(std::int64_t started_at) {
    const std::optional<fs::path>& dir = workflow_.config.output_dir;
    if (dir) {
        prepare_output_dir(*dir, false); // never replaces an earlier run
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        record_.emplace(dir ? *dir : fs::path(), workflow_.reproducibility, workflow_.fingerprint,
                        workflow_.manifest_locations, workflow_.isolation,
                        workflow_.config.overrides, settings_.validate_payloads,
                        settings_.path_anchors, workflow_.input_counts);
        record_->write_started(started_at);
    }
    if (dir) {
        if (!recorded_.empty()) {
            outputs_ = std::make_unique<OutputRecorder>(*dir, recorded_);
        }
        if (workflow_.config.trace_messages) {
            trace_ = std::make_unique<TraceWriter>(*dir / "trace" / "messages.jsonl");
        }
    } else if (workflow_.config.trace_messages) {
        log(LogLevel::warn, "no message trace is written: the run has no output directory",
            {{"event", "trace.skipped"}});
    }
}

template <class Call> CallResult Engine::call(Step& step, Call&& body) {
    // While the call runs, request_stop() can reach its instance. Afterwards, wait for any
    // request_stop() that read the pointer before it was cleared, so that it cannot reach a later
    // call (such as finalize) or an instance that is gone.
    struct Release {
        explicit Release(Engine& owner) : engine(owner) {}
        Release(const Release&) = delete;
        Release& operator=(const Release&) = delete;
        ~Release() {
            engine.in_flight_.store(nullptr);
            while (engine.cancellers_.load() != 0) {
                std::this_thread::yield();
            }
        }
        Engine& engine;
    };
    show_step_state(step, StepState::busy);
    in_flight_.store(step.instance.get());
    const Release release(*this);
    return body();
}

bool Engine::start_step(Step& step) {
    const ResolvedStep& resolved = *step.resolved;
    const ModuleManifest& manifest = *resolved.manifest;
    if (stop_flag_.load()) {
        stop_run(std::nullopt);
        return false;
    }

    std::shared_ptr<LoadedModule> module;
    if (resolved.isolation.mode == IsolationMode::process) {
        // A module host of the step's own, never shared with another step, which loads the module
        // with validation's digest too. A stop ends the load: the module host is killed.
        ChildSettings child;
        child.library_sha256 = resolved.library_sha256;
        child.logger = step.logger;
        child.stop_flag = &stop_flag_;
        // The providers of the step's services, loaded and started in the module host, with
        // validation's digests; their records go to the run's provider.<name>, as here.
        child.providers = child_providers(workflow_, resolved.services, settings_.logs);
        // Compiled models shared with the other sharing steps' module hosts, through the store in
        // this process; the store's events go to the step's logger.
        child.share_compiled_models = resolved.isolation.share_compiled_models;
        // The sandbox profile it applies before it loads any library.
        child.sandbox = resolved.isolation.sandbox;
        try {
            module = LoadedModule::load_in_child(manifest, std::move(child));
        } catch (const Error& error) {
            if (error.code() == ErrorCode::CANCELLED && stop_flag_.load()) {
                stop_run(std::nullopt);
                return false;
            }
            step.crashed = error.code() == ErrorCode::MODULE_CRASHED;
            throw fail(error, Failure{&step, "load"});
        }
    } else {
        auto loaded = modules_.find(&manifest);
        if (loaded == modules_.end()) {
            std::shared_ptr<LoadedModule> library;
            try {
                // With validation's digest: a library that changed since is refused before any of
                // its code runs (LIBRARY_DIGEST_MISMATCH), as in a module host.
                library = LoadedModule::load(manifest, nullptr, resolved.library_sha256);
            } catch (const Error& error) {
                throw fail(error, Failure{&step, "load"});
            }
            loaded = modules_.emplace(&manifest, std::move(library)).first;
        }
        module = loaded->second;
    }

    InstanceOptions options;
    options.step = resolved.id;
    options.caller = "step '" + resolved.id + "' of workflow " +
                     identity(workflow_.definition->name, workflow_.definition->version);
    options.lifecycle_timeout = resolved.timeouts.lifecycle;
    options.invoke_timeout = resolved.timeouts.invoke;
    options.cancel_grace = resolved.timeouts.cancel_grace;
    options.logger = step.logger;
    options.services = settings_.services;
    // A module in a module host is served its services there: the providers are in that process.
    if (!module->runs_in_child()) {
        for (const std::string& key : resolved.services) {
            const ServiceVersion& service = workflow_.services.at(key).service;
            options.provider_services.add(service.name, service.major, provider_tables_.at(key));
        }
    }
    options.run_fingerprint = workflow_.fingerprint;
    options.stop_flag = &stop_flag_;
    options.record_lookups = trace_ != nullptr;
    step.instance = std::make_unique<ModuleInstance>(std::move(module), std::move(options));
    sync_step_state(step);

    const json create = {
        {"pmp_version", kPmpVersion},
        {"step", resolved.id},
        {"capability",
         {{"id", resolved.capability->id}, {"version", to_string(resolved.capability->version)}}},
        {"module", {{"name", manifest.name}, {"version", to_string(manifest.version)}}},
        {"parameters", resolved.parameters}};
    const TimeBase& time = workflow_.config.time;
    const json initialize = {
        {"pmp_version", kPmpVersion},
        {"run", {{"fingerprint", workflow_.fingerprint}, {"seed", resolved.seed}}},
        {"time", {{"unit", time.unit}, {"start", time.start}, {"period", time.period}}}};
    if (!lifecycle_call(step, "create", &ModuleInstance::create, create) ||
        !lifecycle_call(step, "initialize", &ModuleInstance::initialize, initialize)) {
        return false;
    }
    log(LogLevel::debug, "step '" + resolved.id + "' is ready",
        {{"event", "step.ready"}, {"step", resolved.id}, {"module", step.module}});
    return true;
}

bool Engine::lifecycle_call(Step& step, const char* phase,
                            CallResult (ModuleInstance::*method)(const std::string&),
                            const json& document) {
    if (stop_flag_.load()) {
        stop_run(std::nullopt);
        return false;
    }
    const std::string text = canonical_json(document);
    const CallResult result = call(step, [&] { return ((*step.instance).*method)(text); });
    sync_step_state(step);
    if (result.outcome == CallOutcome::succeeded) {
        trace_lookups(step);
        return true;
    }
    // The lookups of a call that did not succeed are traced when the teardown starts, so that a
    // trace that cannot be written never hides how the call ended.
    if (result.outcome == CallOutcome::cancelled) {
        stop_run(std::nullopt);
        return false;
    }
    throw fail(call_error(step, phase, result, std::nullopt),
               Failure{&step, phase, std::nullopt, &result});
}

Error Engine::call_error(const Step& step, const char* phase, const CallResult& result,
                         std::optional<std::uint64_t> tick) const {
    const ResolvedStep& resolved = *step.resolved;
    const std::chrono::nanoseconds timeout =
        std::string(phase) == "invoke" ? resolved.timeouts.invoke : resolved.timeouts.lifecycle;
    const std::string who = "step '" + resolved.id + "' (module " + step.module + "): " + phase;
    const std::string when = tick ? " at tick " + std::to_string(*tick) : std::string();
    switch (result.outcome) {
    case CallOutcome::timed_out:
        return Error(ErrorCode::STEP_TIMEOUT, who + " did not return within its timeout of " +
                                                  format_duration(timeout) + when);
    case CallOutcome::unresponsive:
        if (!result.details_json.empty()) { // in a module host, which was terminated
            return Error(ErrorCode::MODULE_UNRESPONSIVE,
                         who + " did not return within its timeout of " + format_duration(timeout) +
                             " and the cancellation grace of " +
                             format_duration(resolved.timeouts.cancel_grace) + when +
                             "; its module host was terminated",
                         SourceLocation{}, result.details_json);
        }
        return Error(ErrorCode::MODULE_UNRESPONSIVE,
                     who + " did not return within its timeout of " + format_duration(timeout) +
                         " and the cancellation grace of " +
                         format_duration(resolved.timeouts.cancel_grace) + when +
                         "; the instance is abandoned and its library stays loaded");
    case CallOutcome::crashed:
        return Error(ErrorCode::MODULE_CRASHED,
                     who + " crashed" + when + ": its module host " +
                         describe_crash(result.details_json),
                     SourceLocation{}, result.details_json);
    case CallOutcome::refused:
        if (result.refused_by) {
            return Error(ErrorCode::MODULE_UNRESPONSIVE,
                         who + " was refused" + when + ": " + refusal_reason(*result.refused_by),
                         SourceLocation{},
                         dump(json{{"abandoned_call", to_json(*result.refused_by)}}));
        }
        break;
    case CallOutcome::succeeded:
    case CallOutcome::failed:
    case CallOutcome::cancelled:
        break;
    }
    return Error(ErrorCode::STEP_FAILED, who + " failed" + when + " with status " +
                                             status_text(result.status) + ": " +
                                             cause_of(result).at("message").get<std::string>());
}

TickResult Engine::run_tick() {
    const WorkflowState current = state_.load();
    if (current != WorkflowState::ready && current != WorkflowState::running) {
        throw Error(ErrorCode::INVALID_STATE,
                    current == WorkflowState::created
                        ? std::string("run_tick(): the run has not started; call start() first")
                        : "run_tick(): the run is " + to_string(current) + "; no more ticks run");
    }
    return control([this] { return tick_once(); });
}

TickResult Engine::tick_once() {
    const std::uint64_t tick = ticks_completed_.load();
    const double window_start = bound(tick);
    const double window_end = bound(tick + 1);
    TickResult result;
    result.tick = tick;
    result.logical_time = window_end;
    if (stop_flag_.load()) {
        stop_run(tick);
        result.finished = true;
        return result;
    }
    if (state_.load() == WorkflowState::ready) {
        set_state(WorkflowState::running);
    }
    current_tick_ = tick;
    const auto started = Clock::now();

    // 1. The workflow inputs.
    std::map<std::string, json> messages; // this tick's, by source endpoint
    for (const auto& [name, source] : workflow_.sources) {
        const WorkflowPortDef& port = workflow_.definition->inputs.at(name);
        json data = source->batch(tick, window_start, window_end);
        if (settings_.validate_payloads) {
            const MessageTypeDef& type = message_type(port.type, port.type_version);
            if (const auto issues = type.compiled->validate(data); !issues.empty()) {
                throw fail(
                    input_error(name, tick,
                                "the data does not satisfy " + identity(type.id, type.version),
                                issues, issues.front().pointer),
                    Failure{nullptr, nullptr, tick});
            }
        }
        const Endpoint source_endpoint{Endpoint::Kind::workflow_input, "inputs", name};
        json envelope = make_envelope(fingerprint16_, tick, window_end, source_endpoint, port.type,
                                      port.type_version, std::move(data));
        if (settings_.validate_payloads) {
            if (const auto issues = envelope_schema().validate(envelope); !issues.empty()) {
                throw fail(broken_envelope("workflow input " + quoted(name), tick, issues,
                                           {{"input", name}}, ""),
                           Failure{nullptr, nullptr, tick});
            }
        }
        messages.emplace(source_endpoint.to_string(), std::move(envelope));
    }

    // 2. The steps, in schedule order.
    for (Step& step : steps_) {
        if (!run_step(step, tick, window_end, messages)) {
            current_tick_.reset();
            result.finished = true;
            return result; // stopped
        }
    }

    // 3. The workflow outputs.
    for (const auto& [name, connection] : workflow_.graph.outputs) {
        const auto message = messages.find(connection.from.to_string());
        if (message == messages.end()) {
            continue; // an optional output the step left out
        }
        result.outputs.push_back(name);
        if (outputs_ != nullptr && std::binary_search(recorded_.begin(), recorded_.end(), name)) {
            outputs_->record(name, message->second);
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        latest_[name] = message->second;
    }
    if (outputs_ != nullptr) {
        outputs_->flush();
    }
    if (trace_ != nullptr) {
        trace_->flush();
    }
    previous_ = std::move(messages);
    current_tick_.reset();

    // 4. The tick is complete.
    const auto duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
    ticks_completed_.store(tick + 1);
    metrics_.record_tick(tick, window_end, duration);
    log(LogLevel::debug, "tick " + std::to_string(tick) + " completed",
        {{"event", "tick.completed"},
         {"tick", tick},
         {"logical_time", window_end},
         {"duration_ns", duration.count()}});
    update_end_reached();
    if (end_reached_.load()) {
        complete_run();
    } else if (stop_flag_.load()) {
        stop_run(std::nullopt); // asked for during the tick's last call, which still completed
    }
    result.finished = finished();
    return result;
}

bool Engine::run_step(Step& step, std::uint64_t tick, double logical_time,
                      std::map<std::string, json>& messages) {
    const ResolvedStep& resolved = *step.resolved;
    json inputs = json::object();
    const auto incoming = workflow_.graph.incoming.find(resolved.id);
    if (incoming != workflow_.graph.incoming.end()) {
        for (const auto& [port, connection] : incoming->second) {
            const auto& pool = connection.delay == 0 ? messages : previous_;
            const auto message = pool.find(connection.from.to_string());
            if (message != pool.end()) {
                inputs[port] = message->second;
                continue;
            }
            const auto def = resolved.capability->inputs.find(port);
            if (def != resolved.capability->inputs.end() && def->second.required) {
                throw fail(Error(ErrorCode::INPUT_MISSING,
                                 "step '" + resolved.id + "': its required input '" + port +
                                     "' has no message from " + connection.from.to_string() +
                                     " in tick " + std::to_string(tick)),
                           Failure{&step, "invoke", tick});
            }
        }
    }
    if (stop_flag_.load()) {
        stop_run(tick);
        return false;
    }
    const json invocation = {{"pmp_version", kPmpVersion},
                             {"step", resolved.id},
                             {"tick", tick},
                             {"logical_time", logical_time},
                             {"inputs", inputs}};
    const std::string document = canonical_json(invocation);
    if (trace_ != nullptr) {
        for (const auto& [port, envelope] : inputs.items()) {
            trace_->write(tick, resolved.id, "in", port, envelope, nullptr);
        }
    }

    const CallResult result = call(step, [&] { return step.instance->invoke(document, tick); });
    if (result.outcome == CallOutcome::cancelled) {
        // An invoke stopped while it waited for its library never reached the module: like a
        // stop that comes before the call, it is not an invocation.
        if (result.called) {
            metrics_.record_invocation(resolved.id, result.outcome, result.duration, now_unix_ns(),
                                       std::nullopt);
        }
        sync_step_state(step);
        stop_run(tick);
        return false;
    }
    if (result.outcome != CallOutcome::succeeded) {
        throw fail(call_error(step, "invoke", result, tick),
                   Failure{&step, "invoke", tick, &result, result.outcome});
    }

    std::optional<json> diagnostics;
    auto outputs = check_result(step, tick, logical_time, result, diagnostics);
    metrics_.record_invocation(resolved.id, CallOutcome::succeeded, result.duration, now_unix_ns(),
                               std::nullopt);
    sync_step_state(step);
    // The lookups of an invoke that did not succeed, or whose result is invalid, are traced when
    // the teardown starts, at the same place: nothing is traced in between.
    trace_lookups(step);
    if (trace_ != nullptr && diagnostics && outputs.empty()) {
        trace_->write_diagnostics(tick, resolved.id, *diagnostics); // nothing else carries them
    }
    for (auto& [port, envelope] : outputs) {
        if (trace_ != nullptr) {
            trace_->write(tick, resolved.id, "out", port, envelope,
                          diagnostics ? &*diagnostics : nullptr);
        }
        const Endpoint source{Endpoint::Kind::step, resolved.id, port};
        messages[source.to_string()] = std::move(envelope);
    }
    log(LogLevel::debug, "invocation completed",
        {{"event", "invocation.completed"},
         {"tick", tick},
         {"step", resolved.id},
         {"module", step.module},
         {"duration_ns", result.duration.count()}});
    return true;
}

void Engine::trace_lookups(Step& step) {
    if (step.instance == nullptr) {
        return;
    }
    const std::vector<ServiceLookup> lookups = step.instance->take_lookups();
    if (trace_ == nullptr) {
        return; // nothing is recorded then; whatever was, is dropped
    }
    for (const ServiceLookup& lookup : lookups) {
        trace_->write_lookup(lookup.tick, step.resolved->id, lookup.phase, lookup.service,
                             lookup.major, lookup.provided, lookup.count);
    }
}

std::vector<std::pair<std::string, json>> Engine::check_result(Step& step, std::uint64_t tick,
                                                               double logical_time,
                                                               const CallResult& result,
                                                               std::optional<json>& diagnostics) {
    const ResolvedStep& resolved = *step.resolved;
    const CapabilityDef& capability = *resolved.capability;
    const std::string who = "step '" + resolved.id + "' (module " + step.module +
                            ") returned an invalid result at tick " + std::to_string(tick) + ": ";
    const auto invalid = [&](std::string message, std::string pointer, json details) {
        SourceLocation location;
        location.pointer = std::move(pointer);
        return fail(
            Error(ErrorCode::OUTPUT_INVALID, who + message, std::move(location), dump(details)),
            Failure{&step, "invoke", tick, &result, CallOutcome::failed});
    };

    // The acceptance rules, shared with the contract checks.
    AcceptedResult accepted;
    if (const auto problem = check_invoke_result(
            result.output, capability, settings_.validate_payloads,
            [this](const PortDef& port) -> const MessageTypeDef& {
                return message_type(port.type, port.type_version);
            },
            accepted)) {
        throw invalid(problem->message, problem->pointer, problem->details);
    }

    std::vector<std::pair<std::string, json>> envelopes;
    for (auto& [port, data] : accepted.outputs) { // sorted by port
        const PortDef& def = capability.outputs.at(port);
        const Endpoint source{Endpoint::Kind::step, resolved.id, port};
        json envelope = make_envelope(fingerprint16_, tick, logical_time, source, def.type,
                                      def.type_version, std::move(data));
        if (settings_.validate_payloads) {
            if (const auto issues = envelope_schema().validate(envelope); !issues.empty()) {
                // Not the module's failure: its invocation succeeded and counts as such.
                metrics_.record_invocation(resolved.id, CallOutcome::succeeded, result.duration,
                                           now_unix_ns(), std::nullopt);
                sync_step_state(step);
                throw fail(broken_envelope(
                               "output " + quoted(port) + " of step " + quoted(resolved.id), tick,
                               issues,
                               {{"step", resolved.id}, {"module", step.module}, {"port", port}},
                               "/outputs/" + json_pointer_escape(port)),
                           Failure{nullptr, nullptr, tick});
            }
        }
        envelopes.emplace_back(port, std::move(envelope));
    }
    diagnostics = std::move(accepted.diagnostics);
    return envelopes;
}

Error Engine::fail(const Error& error, const Failure& failure) {
    ErrorRecord record;
    record.code = error.code();
    record.tick = failure.tick;
    record.at_unix_ns = now_unix_ns();
    std::string details_text = error.details_json();
    try {
        record.message = error.what();
        record.location = error.location();
        json details = json::parse(details_text, nullptr, false);
        if (!details.is_object()) {
            details = json::object();
        }
        // The module's error document, when the call failed and the module said why.
        const CallResult* failed_call = failure.call;
        if (failed_call != nullptr) {
            if (auto cause = module_cause(*failed_call)) {
                record.cause_json = dump(*cause);
                details["cause"] = std::move(*cause);
            }
        }
        if (failure.tick) {
            details["tick"] = *failure.tick;
        }
        if (failure.step != nullptr) {
            Step& step = *failure.step;
            const ResolvedStep& resolved = *step.resolved;
            details["step"] = resolved.id;
            details["module"] = step.module;
            if (failure.phase != nullptr) {
                details["phase"] = failure.phase;
            }
            // The failing instance's diagnostics, while it is still there: not when its call was
            // refused, since its library would refuse them too.
            const bool refused =
                failed_call != nullptr && failed_call->outcome == CallOutcome::refused;
            if (step.instance != nullptr && resolved.manifest->features.diagnostics &&
                step.instance->state() != StepState::unresponsive && !refused) {
                if (auto text = step.instance->diagnostics()) {
                    details["diagnostics"] = bounded_diagnostics(*text);
                }
            }
        }
        // The record holds the details of the error thrown, complete. Without memory for them,
        // the record goes without, and the step's metrics are still brought up to date.
        try {
            details_text = dump(details);
            record.details_json = details_text;
        } catch (...) {
        }
        if (failure.step != nullptr) {
            Step& step = *failure.step;
            const ResolvedStep& resolved = *step.resolved;
            // An invocation is counted (with its error); any other failure of the step is only
            // its last error, as is an invoke that was refused and never reached the module.
            if (failed_call != nullptr && failed_call->called && failure.phase != nullptr &&
                std::string(failure.phase) == "invoke") {
                metrics_.record_invocation(resolved.id, failure.counted, failed_call->duration,
                                           record.at_unix_ns, record);
            } else {
                metrics_.set_step_error(resolved.id, record);
            }
        }
        metrics_.set_last_error(record);
    } catch (...) {
        // Out of memory while describing the failure: the run still ends, with less detail.
    }
    if (failure.step != nullptr) {
        failure.step->failed = true;
        sync_step_state(*failure.step);
    }
    log(LogLevel::error, std::string("run failed: ") + error.what(),
        {{"event", "run.failed"}, {"code", std::string(error_code_info(error.code()).name)}});
    if (const auto problem = finish(WorkflowState::failed, std::move(record))) {
        log(LogLevel::error, std::string("while ending the failed run: ") + problem->what());
    }
    return Error(error.code(), error.what(), error.location(), std::move(details_text));
}

void Engine::stop_run(std::optional<std::uint64_t> tick) {
    ErrorRecord record;
    record.code = ErrorCode::RUN_STOPPED;
    record.message = stop_flag_.load() ? "the run was stopped on request"
                                       : "the run was ended before it finished";
    record.tick = tick;
    record.at_unix_ns = now_unix_ns();
    log(LogLevel::info,
        record.message + " after " + std::to_string(ticks_completed_.load()) + " tick(s)",
        {{"event", "run.stopped"}});
    if (auto problem = finish(WorkflowState::stopped, std::move(record))) {
        throw *problem;
    }
}

void Engine::complete_run() {
    log(LogLevel::info,
        "run completed after " + std::to_string(ticks_completed_.load()) + " tick(s)",
        {{"event", "run.completed"}});
    if (auto problem = finish(WorkflowState::completed, std::nullopt)) {
        throw *problem;
    }
}

std::optional<Error> Engine::finish(WorkflowState final_state, std::optional<ErrorRecord> error) {
    // Only fail() ends a run as failed from the start, with the failure it throws.
    const bool failing = final_state == WorkflowState::failed;
    std::optional<Error> problem;
    std::vector<OutputRecorder::FileInfo> files;
    std::optional<OutputRecorder::FileInfo> trace;
    try {
        if (final_state != WorkflowState::completed) {
            set_state(WorkflowState::stopping);
        }
        teardown();
        current_tick_.reset();

        // Each file is closed whatever happens to the other; one that cannot be completed is
        // still described, as far as it was written.
        if (outputs_ != nullptr) {
            try {
                files = outputs_->finish();
            } catch (const Error& e) {
                problem = e;
                files = outputs_->finish(); // finished now: returns what was recorded
            }
        }
        if (trace_ != nullptr) {
            const auto describe = [](const TraceWriter::Summary& written) {
                OutputRecorder::FileInfo info;
                info.path = "trace/messages.jsonl";
                info.sha256 = written.sha256;
                info.messages = written.messages;
                return info;
            };
            try {
                trace = describe(trace_->finish());
            } catch (const Error& e) {
                if (!problem) {
                    problem = e;
                }
                trace = describe(trace_->finish()); // finished now: returns what was written
            }
        }
        const auto turn_into_failure = [&](const Error& e) {
            log(LogLevel::error, e.what(), {{"event", "run.output_failed"}});
            metrics_.set_last_error(record_of(e, std::nullopt));
            if (final_state != WorkflowState::failed) {
                final_state = WorkflowState::failed;
                error = record_of(e, std::nullopt);
                problem = e;
            }
        };
        if (problem) {
            turn_into_failure(*problem);
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!record_) {
                break; // the run never got as far as its first record
            }
            RunSummary ending;
            ending.final_state = final_state;
            ending.ticks_completed = ticks_completed_.load();
            ending.fingerprint = workflow_.fingerprint;
            ending.error = error;
            StatusSnapshot status = metrics_.snapshot();
            status.state = final_state;
            status.health = compute_health(status);
            try {
                record_->write_final(ending, now_unix_ns(), files, status.to_json(), trace);
                break;
            } catch (const Error& e) {
                const WorkflowState before = final_state;
                turn_into_failure(e);
                if (before == WorkflowState::failed) {
                    break; // nothing more to say
                }
            }
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            error_ = std::move(error);
        }
        set_state(final_state);
    } catch (...) {
        // Something other than a praenora::Error (out of memory, most likely) broke off the
        // ending. The run is over all the same, as failed. Its error is the first failure, and the
        // run records the same error that is thrown:
        // - a run that was failing keeps the failure fail() throws (whatever is returned below is
        //   only logged by fail());
        // - otherwise the error captured earlier in the ending (an output file that could not be
        //   written), if any, else an INTERNAL error about this exception, is returned, so the
        //   control call that ended the run throws it (shutdown() and the destructor only record
        //   it), and the run's record is built from that very error.
        state_.store(WorkflowState::failed);
        if (!problem) {
            try {
                problem = Error(ErrorCode::INTERNAL,
                                "internal error while ending the run: " + current_exception_text());
            } catch (...) {
                // No memory even for that: the record below keeps the code alone.
            }
        }
        std::optional<ErrorRecord> run_error;
        try {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (failing) {
                if (!error_) {
                    error_ = std::move(error);
                }
            } else {
                // The record of the error returned (its message, location and details); when
                // memory runs out, its code alone.
                ErrorRecord first;
                first.code = problem ? problem->code() : ErrorCode::INTERNAL;
                if (problem) {
                    try {
                        first = record_of(*problem, std::nullopt);
                    } catch (...) {
                    }
                }
                error_ = std::move(first);
            }
            run_error = error_;
        } catch (...) {
        }
        try {
            metrics_.set_workflow_state(WorkflowState::failed);
            if (problem) {
                log(LogLevel::error, problem->what(), {{"event", "run.failed"}});
            }
            if (run_error) {
                metrics_.set_last_error(*run_error);
                const std::lock_guard<std::mutex> lock(mutex_);
                if (record_) {
                    RunSummary ending;
                    ending.final_state = WorkflowState::failed;
                    ending.ticks_completed = ticks_completed_.load();
                    ending.fingerprint = workflow_.fingerprint;
                    ending.error = run_error;
                    StatusSnapshot status = metrics_.snapshot();
                    status.state = WorkflowState::failed;
                    status.health = compute_health(status);
                    record_->write_final(ending, now_unix_ns(), files, status.to_json(), trace);
                }
            }
        } catch (...) {
            // The record or the metrics could not be brought up to date; the state says failed.
        }
        if (settings_.on_state_change) {
            try {
                settings_.on_state_change();
            } catch (...) {
            }
        }
    }
    return problem;
}

void Engine::teardown() noexcept {
    // Every call goes through its library's gate (loader/library_gate.hpp), which refuses one
    // that would have to wait for an abandoned call, of this run or another one in the process,
    // since that call may still be running in the library. The instance of a refused call is
    // left as it is from then on: neither finalized nor destroyed, never freed (Step::kept).
    const auto keep = [](Step& step, const std::optional<LibraryCall>& by) noexcept {
        try {
            step.kept = by ? *by : LibraryCall{};
        } catch (...) {
            step.kept.emplace(); // out of memory: kept all the same, without its reason
        }
    };

    // The lookups of the calls not traced yet (a call that did not succeed, a failure's
    // diagnostics), in schedule order, then those of each finalize and destroy. A write that fails
    // leaves the trace's stream failed, which finish() reports.
    const auto trace = [this](Step& step) noexcept {
        try {
            trace_lookups(step);
        } catch (...) {
        }
    };
    for (Step& step : steps_) {
        trace(step);
    }

    // An instance whose module host the runtime killed, because a stop came before the module
    // host had taken a call, went with that process: nothing is left to finalize or destroy, and
    // no call would reach it. Not a failure (the run was stopped on request), but a warning says
    // what was left undone.
    for (auto it = steps_.rbegin(); it != steps_.rend(); ++it) {
        Step& step = *it;
        if (step.instance == nullptr || step.kept) {
            continue;
        }
        if (const auto was = step.instance->release_terminated()) {
            const std::string& id = step.resolved->id;
            try {
                log(LogLevel::warn,
                    "step '" + id + "' (module " + step.module + ") " + left_undone(*was) +
                        ": its module host was ended when a stop came before it had taken a "
                        "call, and the instance went with it",
                    {{"event", "teardown.skipped"},
                     {"step", id},
                     {"module", step.module},
                     {"state", to_string(*was)}});
            } catch (...) {
                // Out of memory while building the warning; the instance is gone all the same.
            }
            sync_step_state(step);
        }
    }

    for (auto it = steps_.rbegin(); it != steps_.rend(); ++it) {
        Step& step = *it;
        if (step.instance == nullptr || step.instance->state() != StepState::ready) {
            continue;
        }
        try {
            show_step_state(step, StepState::busy);
            const CallResult result = step.instance->finalize();
            trace(step);
            if (result.outcome == CallOutcome::refused) {
                keep(step, result.refused_by);
            } else if (result.outcome != CallOutcome::succeeded) {
                step.failed = step.instance->state() != StepState::unresponsive;
                const Error error = call_error(step, "finalize", result, std::nullopt);
                ErrorRecord record = teardown_record_of(error, "finalize");
                if (auto cause = module_cause(result)) {
                    record.cause_json = dump(*cause);
                }
                metrics_.set_step_error(step.resolved->id, std::move(record));
                log(LogLevel::warn, error.what(),
                    {{"event", "teardown.finalize_failed"}, {"step", step.resolved->id}});
            }
        } catch (const std::exception& e) {
            log(LogLevel::warn, "step '" + step.resolved->id + "': finalize: " + e.what(),
                {{"event", "teardown.finalize_failed"}, {"step", step.resolved->id}});
        }
        sync_step_state(step);
    }
    for (auto it = steps_.rbegin(); it != steps_.rend(); ++it) {
        if (it->instance == nullptr || it->kept ||
            it->instance->state() == StepState::unresponsive ||
            it->instance->state() == StepState::crashed) {
            continue; // an unresponsive or crashed instance is never destroyed
        }
        show_step_state(*it, StepState::busy);
        std::string details;
        const CallOutcome destroyed = it->instance->destroy(&details);
        trace(*it);
        if (destroyed == CallOutcome::refused) {
            keep(*it, it->instance->last_refusal());
        } else if (destroyed == CallOutcome::crashed || destroyed == CallOutcome::unresponsive) {
            // Recorded as a finalize that crashes or never returns is: the step's last error
            // (MODULE_CRASHED or MODULE_UNRESPONSIVE, phase destroy) and a warning. A destroy
            // that returns after its deadline, within the grace, has freed its instance: it
            // leaves only its call.deadline warning.
            try {
                if (destroyed == CallOutcome::crashed) {
                    it->failed = true;
                }
                CallResult result;
                result.outcome = destroyed;
                result.details_json = details;
                const Error error = call_error(*it, "destroy", result, std::nullopt);
                metrics_.set_step_error(it->resolved->id, teardown_record_of(error, "destroy"));
                log(LogLevel::warn, error.what(),
                    {{"event", "teardown.destroy_failed"}, {"step", it->resolved->id}});
            } catch (const std::exception& e) {
                log(LogLevel::warn, "step '" + it->resolved->id + "': destroy: " + e.what(),
                    {{"event", "teardown.destroy_failed"}, {"step", it->resolved->id}});
            }
        }
        sync_step_state(*it);
    }
    // Instances first (their worker threads are joined), then the libraries. The instances that
    // may not be called are left as they are, for good.
    for (auto it = steps_.rbegin(); it != steps_.rend(); ++it) {
        if (it->kept) {
            const std::string& id = it->resolved->id;
            const StepState state = it->instance->state();
            try {
                log(LogLevel::warn,
                    "step '" + id + "' (module " + it->module + ") " + left_undone(state) + ": " +
                        refusal_reason(*it->kept),
                    {{"event", "teardown.skipped"},
                     {"step", id},
                     {"module", it->module},
                     {"state", to_string(state)}});
            } catch (...) {
                // Out of memory while building the warning; the instance is kept all the same.
            }
            keep_forever(std::move(it->instance));
            continue;
        }
        it->instance.reset();
    }
    modules_.clear();
}

void Engine::shutdown() noexcept {
    const WorkflowState current = state_.load();
    if (current != WorkflowState::ready && current != WorkflowState::running) {
        return; // not started, or over
    }
    try {
        stop_run(std::nullopt);
    } catch (const std::exception& e) {
        log(LogLevel::error, std::string("shutdown: ") + e.what());
    }
}

bool Engine::finished() const {
    // A stop that was requested is not the end yet: the next run_tick() (or shutdown()) winds the
    // run down, calling no module, and the run is then over.
    return is_terminal(state_.load()) || end_reached_.load();
}

void Engine::request_stop() noexcept {
    stop_flag_.store(true);
    cancellers_.fetch_add(1);
    if (ModuleInstance* instance = in_flight_.load()) {
        instance->request_cancel();
    }
    cancellers_.fetch_sub(1);
}

std::string Engine::latest_outputs_json() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return dump(latest_);
}

json Engine::run_record() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return record_ ? record_->current() : json(nullptr);
}

WorkflowState Engine::state() const noexcept { return state_.load(); }

RunSummary Engine::summary() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    RunSummary result;
    result.final_state = state_.load();
    result.ticks_completed = ticks_completed_.load();
    result.fingerprint = workflow_.fingerprint;
    result.error = error_;
    return result;
}

const std::string& Engine::fingerprint() const noexcept { return workflow_.fingerprint; }

void Engine::set_state(WorkflowState next) {
    state_.store(next);
    metrics_.set_workflow_state(next);
    if (settings_.on_state_change) {
        try {
            settings_.on_state_change();
        } catch (...) {
            // The callback's problem, not the run's.
        }
    }
}

void Engine::sync_step_state(const Step& step) noexcept {
    StepState shown = step.instance != nullptr ? step.instance->state()
                      : step.crashed           ? StepState::crashed
                                               : StepState::loaded;
    if (step.failed && shown != StepState::unresponsive && shown != StepState::crashed) {
        shown = StepState::failed;
    }
    show_step_state(step, shown);
}

void Engine::show_step_state(const Step& step, StepState shown) noexcept {
    try {
        metrics_.set_step_state(step.resolved->id, shown);
    } catch (...) {
        // A status detail; the run goes on.
    }
}

void Engine::update_end_reached() {
    const std::uint64_t done = ticks_completed_.load();
    bool end = true;
    if (workflow_.config.ticks) {
        end = done >= *workflow_.config.ticks;
    } else {
        const double window_end = bound(done); // the end of the last window, or the start
        for (const auto& entry : workflow_.sources) {
            if (entry.second->replay() && !entry.second->exhausted_after(window_end)) {
                end = false;
                break;
            }
        }
    }
    end_reached_.store(end);
}

double Engine::bound(std::uint64_t tick) const {
    return workflow_.config.time.start + static_cast<double>(tick) * workflow_.config.time.period;
}

const MessageTypeDef& Engine::message_type(const std::string& id, const SemVer& version) const {
    const auto found = workflow_.message_types.find(identity(id, version));
    if (found == workflow_.message_types.end() || found->second == nullptr) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "message type " + identity(id, version) + " is not among the workflow's");
    }
    return *found->second;
}

Error Engine::input_error(const std::string& input, std::uint64_t tick, const std::string& problem,
                          const std::vector<SchemaIssue>& issues, std::string pointer) const {
    SourceLocation location;
    const auto binding = workflow_.config.inputs.find(input);
    if (binding != workflow_.config.inputs.end()) {
        location.file = path_to_utf8(binding->second.path);
    }
    location.pointer = std::move(pointer);
    return Error(ErrorCode::TARGET_DATA_INVALID,
                 "input '" + input + "', tick " + std::to_string(tick) + ": " + problem + ": " +
                     issues.front().pointer + ": " + issues.front().message,
                 std::move(location), dump({{"input", input}, {"issues", issues_json(issues)}}));
}

void Engine::log(LogLevel level, const std::string& message,
                 std::vector<LogAttribute> attributes) const noexcept {
    try {
        attributes.insert(attributes.begin(),
                          LogAttribute{"run.fingerprint", workflow_.fingerprint});
        log_event(*logger_, level, message, attributes);
    } catch (...) {
        // The message is lost.
    }
}

} // namespace praenora::detail
