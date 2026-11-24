// praenora::Workflow: one run, driven through the engine (engine/engine.hpp).
#include "api/workflow_impl.hpp"

#include "core/paths.hpp"
#include "core/time.hpp"
#include "io/run_record.hpp"
#include "observability/path_anchors.hpp"
#include "observability/status_json.hpp"
#include "platform/platform.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/status.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace praenora {

namespace {

// Holds a workflow's control-call flag for the duration of one control call. A control call that
// starts while another one is in progress is refused, before it touches anything.
class ControlCall {
public:
    ControlCall(std::atomic<bool>& busy, const char* call) : busy_(busy) {
        if (busy_.exchange(true)) {
            throw Error(ErrorCode::INVALID_STATE,
                        std::string(call) +
                            ": another control call (start, tick, run or close) is in progress "
                            "on this workflow; control calls are made one at a time");
        }
    }
    ~ControlCall() { busy_.store(false); }
    ControlCall(const ControlCall&) = delete;
    ControlCall& operator=(const ControlCall&) = delete;

private:
    std::atomic<bool>& busy_;
};

} // namespace

Workflow::Impl::Impl(std::shared_ptr<const detail::RuntimeContext> runtime_context,
                     detail::ValidatedWorkflow workflow, const LogOptions& logging, bool overwrite)
    : context(std::move(runtime_context)) {
    const std::optional<std::filesystem::path> output_path = workflow.config.output_dir;
    if (output_path) {
        output_dir = detail::path_to_utf8(*output_path);
    }
    const std::optional<std::filesystem::path> status_path = workflow.config.status_file;
    const std::chrono::nanoseconds status_interval = workflow.config.status_interval;
    // The run's directories, which the error records of the run record and of the status file name
    // by placeholders; the home directory as it is when the workflow is opened.
    detail::RunDirectories directories;
    directories.output = output_path;
    directories.configuration = workflow.config.file.parent_path();
    directories.modules = workflow.module_search_paths;
    directories.providers = context->providers.scanned_search_paths();
    directories.home = detail::home_directory();
    const auto anchors = std::make_shared<const detail::PathAnchors>(directories);
    // Nothing that can fail may run after an earlier run is removed: refuse it first, without
    // changing anything; remove it (with overwrite) only after the log hub and the engine exist.
    if (output_path) {
        detail::check_output_dir(*output_path, overwrite);
    }
    logs = std::make_unique<detail::LogHub>(logging);

    detail::EngineSettings settings;
    settings.logs = logs.get();
    settings.metrics = &metrics;
    settings.services = &context->services;
    settings.path_anchors = anchors;
    // Called on the control thread (or by the engine's destructor); never from request_stop().
    settings.on_state_change = [this] {
        if (detail::StatusFileWriter* writer = status_target.load()) {
            writer->notify();
        }
    };
    engine = std::make_unique<detail::Engine>(std::move(workflow), std::move(settings));
    if (output_path) {
        detail::prepare_output_dir(*output_path, overwrite);
    }

    if (status_path) {
        status_file = std::make_unique<detail::StatusFileWriter>(
            *status_path, status_interval,
            [this, anchors] {
                // Written for others to read: its error records name the run's directories by
                // their placeholders. status() and status_json() keep the absolute paths.
                nlohmann::json snapshot = detail::status_snapshot_json(metrics.snapshot());
                anchors->apply_to_status_snapshot(snapshot);
                return snapshot.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            },
            [this](const Error& error) {
                // A write that failed degrades the run's health.
                metrics.set_last_error(
                    detail::error_record_of(error, std::nullopt, detail::now_unix_ns()));
            });
        status_target.store(status_file.get());
    }
}

void Workflow::Impl::refuse_when_closed(const char* call) const {
    if (closed) {
        throw Error(ErrorCode::INVALID_STATE,
                    std::string(call) + ": the workflow is closed; open the configuration again "
                                        "for another run");
    }
}

void Workflow::Impl::keep_failure(const Error& error) {
    if (!failure && engine->state() == WorkflowState::failed) {
        failure = error;
    }
}

Workflow::Workflow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Workflow::~Workflow() {
    if (impl_ != nullptr) {
        close();
    }
}

void Workflow::start() {
    const ControlCall control(impl_->busy, "start()");
    impl_->refuse_when_closed("start()");
    detail::Engine& engine = *impl_->engine;
    const WorkflowState current = engine.state();
    if (current != WorkflowState::created) {
        // Refused here, outside the call below, so that the refusal is never kept as the run's
        // failure.
        throw Error(ErrorCode::INVALID_STATE,
                    "start(): the run has already started (state " + to_string(current) + ")");
    }
    try {
        engine.start();
    } catch (const Error& error) {
        impl_->keep_failure(error);
        throw;
    }
}

TickResult Workflow::tick() {
    const ControlCall control(impl_->busy, "tick()");
    impl_->refuse_when_closed("tick()");
    detail::Engine& engine = *impl_->engine;
    const WorkflowState current = engine.state();
    if (current == WorkflowState::created) {
        throw Error(ErrorCode::INVALID_STATE,
                    "tick(): the run has not started; call start() first");
    }
    if (current != WorkflowState::ready && current != WorkflowState::running) {
        throw Error(ErrorCode::INVALID_STATE,
                    "tick(): the run is " + to_string(current) + "; no more ticks run");
    }
    try {
        return engine.run_tick();
    } catch (const Error& error) {
        impl_->keep_failure(error);
        throw;
    }
}

RunSummary Workflow::run(std::optional<std::uint64_t> max_ticks) {
    const ControlCall control(impl_->busy, "run()");
    impl_->refuse_when_closed("run()");
    detail::Engine& engine = *impl_->engine;
    if (engine.state() == WorkflowState::failed) {
        // run() returns only for completed and stopped runs: a failure is thrown, again.
        if (impl_->failure) {
            throw *impl_->failure;
        }
        // Nothing kept: the call that ended the run threw something other than a praenora::Error
        // (out of memory, which the engine passes on as it is). The run's error record says why,
        // with the error's location and details.
        const std::optional<ErrorRecord> error = engine.summary().error;
        if (!error) {
            throw Error(ErrorCode::INTERNAL, "the run failed");
        }
        throw Error(error->code, error->message, error->location,
                    error->details_json.empty() ? std::string("{}") : error->details_json);
    }
    try {
        if (engine.state() == WorkflowState::created) {
            engine.start();
        }
        // A stop requested between ticks does not finish the run by itself: the next tick winds
        // it down without calling a module, and the run is then finished (stopped).
        for (std::uint64_t done = 0; !engine.finished() && (!max_ticks || done < *max_ticks);
             ++done) {
            engine.run_tick();
        }
    } catch (const Error& error) {
        impl_->keep_failure(error);
        throw;
    }
    return engine.summary();
}

void Workflow::request_stop() noexcept {
    // Lock-free and async-signal-safe: the engine only raises its flag and cancels the call in
    // flight. Nothing here may lock or allocate (no status-file notification).
    impl_->engine->request_stop();
}

void Workflow::close() noexcept {
    Impl& impl = *impl_;
    if (impl.busy.exchange(true)) {
        // Another thread is in a control call: the engine may not be shut down under it. Ask it to
        // stop instead; it winds the run down, and a later close() finishes closing.
        impl.engine->request_stop();
        return;
    }
    if (!impl.closed) {
        impl.closed = true;
        impl.engine->shutdown(); // a ready or running run ends as stopped; never throws
        // The writer's destructor writes the final status once more; from now on no state change
        // may reach it.
        impl.status_target.store(nullptr);
        impl.status_file.reset();
    }
    impl.busy.store(false);
}

WorkflowState Workflow::state() const noexcept { return impl_->engine->state(); }

StatusSnapshot Workflow::status() const { return impl_->metrics.snapshot(); }

std::string Workflow::status_json() const { return status().to_json(); }

std::string Workflow::metrics_json() const {
    return detail::metrics_json(impl_->metrics.snapshot());
}

std::string Workflow::latest_outputs_json() const { return impl_->engine->latest_outputs_json(); }

std::string Workflow::run_record_json() const {
    // Messages may come from modules: bytes that are not UTF-8 are replaced, not refused.
    return impl_->engine->run_record().dump(-1, ' ', false,
                                            nlohmann::json::error_handler_t::replace);
}

const std::string& Workflow::fingerprint() const noexcept { return impl_->engine->fingerprint(); }

const std::string& Workflow::output_dir() const noexcept { return impl_->output_dir; }

const ValidationReport& Workflow::diagnostics() const noexcept { return impl_->diagnostics; }

} // namespace praenora
