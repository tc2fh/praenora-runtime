// One run of a validated workflow configuration (Runtime::open()): start it, run it tick by tick
// or to the end, stop it, and query its state, status, metrics, outputs and run record.
#ifndef PRAENORA_RUNTIME_WORKFLOW_HPP
#define PRAENORA_RUNTIME_WORKFLOW_HPP

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/export.h>
#include <praenora/runtime/status.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace praenora {

class Runtime;

#ifdef _MSC_VER
#pragma warning(push)
// The exported class holds a std::unique_ptr, which is not exported itself.
#pragma warning(disable : 4251)
#endif

// A run, from `created` (as Runtime::open() returns it) through `starting` and `ready` to
// `running` and one of the final states `completed`, `stopped` or `failed` (`stopping` while a
// stop or a failure winds it down).
//
// Control calls (start, tick, run, close) are made by one thread at a time: one that starts
// while another is in progress fails with INVALID_STATE (close() instead asks the call in progress
// to stop, see there). The queries and request_stop() are thread-safe and may be called at any
// time from any thread, also while a control call runs; request_stop() is also lock-free and
// async-signal-safe.
//
// start(), tick() and run() throw praenora::Error when the run fails, after its modules are
// finalized and destroyed and its final run record is written; a stopped run is not an error.
class PRAENORA_RUNTIME_API Workflow {
public:
    // close().
    ~Workflow();
    Workflow(const Workflow&) = delete;
    Workflow& operator=(const Workflow&) = delete;

    // Loads and starts the service providers of the run's services, then loads the modules, and
    // creates and initializes an instance for each step, in schedule order: the state becomes
    // `ready` (`completed` for a run of no ticks, `stopped` when a stop came first). INVALID_STATE
    // unless the state is `created`, and once closed.
    void start();

    // Runs the next tick and returns its number, its logical time, the workflow outputs it
    // produced and whether the run is now finished. INVALID_STATE before start(), once the run is
    // over, and once closed.
    TickResult tick();

    // Starts the run when it has not started, then runs ticks until it is finished, or until
    // `max_ticks` more ticks have run, and returns the summary. It returns normally only while the
    // run goes on or once it is completed or stopped (a stop requested between ticks ends the run
    // at the next one, without calling any module); when the run fails, it throws the failure.
    // For a run that is already over it returns the summary at once, except for one that failed
    // earlier, in start(), tick() or run(): then it throws that failure again, with the same code,
    // message, location and details. INVALID_STATE once closed.
    RunSummary run(std::optional<std::uint64_t> max_ticks = std::nullopt);

    // Asks the run to stop: the module call in progress is asked to cancel, no further call
    // starts, and the run ends as `stopped` (summary error RUN_STOPPED), its modules finalized and
    // destroyed. Thread-safe, lock-free and async-signal-safe (it may be called from a signal
    // handler); it never touches the status file.
    void request_stop() noexcept;

    // Ends the run, if it is ready or running, as `stopped`; finalizes, destroys and unloads its
    // modules; writes the final run record and the last status file; then refuses every further
    // control call. Idempotent; the destructor calls it too. Queries keep working afterwards.
    // While another thread is in start(), tick() or run(), close() only requests a stop and
    // returns: that call then winds the run down, and a later close() (or the destructor)
    // finishes closing.
    void close() noexcept;

    WorkflowState state() const noexcept;
    // The status snapshot, and status() as a status-snapshot document. Their error records hold
    // absolute paths; those of the status file, written for others to read, name the run's
    // directories by placeholders (<output>, <configuration>, <modules:caller:0>, <home>, ...;
    // docs/specs/observability.md).
    StatusSnapshot status() const;
    std::string status_json() const;
    std::string metrics_json() const;        // OpenTelemetry-style metric points of status()
    std::string latest_outputs_json() const; // {"<output>": its newest envelope}; {} at first
    // The run record as last written, the same document as run-record.json, its error records
    // naming the run's directories by placeholders like the status file's; null before start.
    std::string run_record_json() const;
    const std::string& fingerprint() const noexcept; // the run fingerprint (SHA-256, hex)
    // The run's output directory as an absolute UTF-8 path (WorkflowOptions::output_dir, else the
    // configuration's outputs.directory), or empty when the run writes no files.
    const std::string& output_dir() const noexcept;
    // What validation found when Runtime::open() opened the workflow and did not refuse it: its
    // warnings and notes (an error would have made open() throw). Each is a Diagnostic of the
    // report that Runtime::validate() gives for the same configuration, in the same order; empty
    // when there are none. Fixed once the workflow is open.
    const ValidationReport& diagnostics() const noexcept;

private:
    friend class Runtime;
    struct Impl;
    explicit Workflow(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace praenora

#endif // PRAENORA_RUNTIME_WORKFLOW_HPP
