// praenora run: validates a workflow configuration and runs it to the end; SIGINT or SIGTERM
// stops it (signals.hpp).
#include "commands/commands.hpp"

#include "output.hpp"
#include "signals.hpp"

#include <frontend.hpp>

#include <praenora/runtime/runtime.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

namespace praenora::cli {

namespace {

using nlohmann::json;

// The names of the log formats and of the isolation modes come from the tables that the tool
// shares with the Python extension (src/frontend).
const std::string kUsage =
    "usage: praenora run <config.yaml> [--ticks N] [--seed S] [--output-dir D] [--overwrite]\n"
    "                    [--trace] [--status-file F] [--isolation " +
    frontend::names_of(frontend::kIsolationModes, "|", "|") +
    "]\n"
    "                    [--log-level L] [--log-format " +
    frontend::names_of(frontend::kLogFormats, "|", "|") +
    "]\n"
    "                    [--log-file F]\n"
    "\n"
    "Validates the configuration and runs the workflow until its ticks are complete, its replay\n"
    "inputs are exhausted, a step fails or a signal stops it (SIGINT or SIGTERM, such as Ctrl+C;\n"
    "a second one ends the process at once; one that the tool started with ignored, as for a\n"
    "command run in the background by a shell, stays ignored). Prints the run fingerprint, the\n"
    "ticks completed and the output directory. Warnings that validation found (the configuration\n"
    "is still valid, as for data rows before the run's start) go to standard error.\n"
    "Exit codes: 0 completed, 130 stopped, otherwise the failure's (for example 1 for a step that\n"
    "failed, 5 for a timeout, 3 when the configuration is invalid).\n"
    "\n"
    "  --ticks N              run N ticks\n"
    "  --seed S               the run seed (0 to 2^53-1)\n"
    "  --output-dir D         write the run record, the outputs and the trace to D\n"
    "  --overwrite            replace an earlier run in the output directory\n"
    "  --trace                trace every message and service lookup to\n"
    "                         <output-dir>/trace/messages.jsonl\n"
    "  --status-file F        keep a status file at F\n"
    "  --isolation M          " +
    frontend::names_of(frontend::kIsolationModes) +
    ": where every step's module instance\n"
    "                         runs, in place of the configuration's: in this process, or in\n"
    "                         a module host, a process of its own\n"
    "  --log-level, --log-format, --log-file\n"
    "                         the run's logging; they win over PRAENORA_LOG_LEVEL,\n"
    "                         PRAENORA_LOG_FORMAT and the configuration\n";

const std::vector<Flag> kFlags = {{"--ticks", true},    {"--seed", true}, {"--output-dir", true},
                                  {"--overwrite"},      {"--trace"},      {"--status-file", true},
                                  {"--isolation", true}};

// The run's summary lines, and the same as JSON.
std::string summary_text(const Workflow& workflow, const RunSummary& summary) {
    const std::string& dir = workflow.output_dir();
    return "fingerprint       " + summary.fingerprint + "\n" + "ticks             " +
           std::to_string(summary.ticks_completed) + "\n" + "output directory  " +
           (dir.empty() ? std::string("none (nothing written)") : dir) + "\n" +
           "state             " + to_string(summary.final_state) + "\n";
}

json summary_json(const Workflow& workflow, const RunSummary& summary) {
    const std::string& dir = workflow.output_dir();
    json result = frontend::run_summary_json(summary);
    result["output_dir"] = dir.empty() ? json(nullptr) : json(dir);
    // The warnings and notes of the validation that opened the run, as validate gives them.
    if (!workflow.diagnostics().diagnostics.empty()) {
        result["diagnostics"] = frontend::workflow_diagnostics_json(workflow).at("diagnostics");
    }
    return result;
}

// Stops watching a workflow before it is destroyed.
class Watching {
public:
    Watching(StopOnSignal& stop, Workflow& workflow) : stop_(stop) { stop_.watch(&workflow); }
    ~Watching() { stop_.watch(nullptr); }
    Watching(const Watching&) = delete;
    Watching& operator=(const Watching&) = delete;

private:
    StopOnSignal& stop_;
};

} // namespace

int command_run(const std::vector<std::string>& args) {
    const std::string usage = kUsage + "\n" + global_options_usage();
    return run_command(
        args, usage, kFlags,
        [](const Arguments& arguments, const GlobalOptions& globals, const Output& output) {
            if (arguments.positionals().size() != 1) {
                throw UsageError("run takes one configuration file");
            }
            WorkflowOptions options;
            if (const auto ticks = arguments.value("--ticks")) {
                options.ticks = parse_count(*ticks, "--ticks", frontend::kMaxCount);
            }
            if (const auto seed = arguments.value("--seed")) {
                options.seed = parse_count(*seed, "--seed", frontend::kMaxCount);
            }
            options.output_dir = arguments.value("--output-dir");
            options.overwrite = arguments.has("--overwrite");
            if (arguments.has("--trace")) {
                options.trace_messages = true;
            }
            options.status_file = arguments.value("--status-file");
            if (const auto mode = arguments.value("--isolation")) {
                options.isolation = frontend::isolation_mode(*mode);
                if (!options.isolation) {
                    throw UsageError("--isolation '" + *mode + "' is not " +
                                     frontend::names_of(frontend::kIsolationModes));
                }
            }
            options.log_level = globals.log_level;
            options.log_format = globals.log_format;
            options.log_file = globals.log_file;

            // The environment is written before the Runtime reads it and before the watcher thread
            // starts.
            const RuntimeOptions runtime_settings = runtime_options(globals);
            StopOnSignal stop;
            const Runtime runtime(runtime_settings);
            const std::unique_ptr<Workflow> workflow =
                runtime.open(arguments.positionals().front(), options);
            if (!workflow->diagnostics().diagnostics.empty()) {
                std::cerr << workflow->diagnostics().to_text(); // the warnings and notes
            }
            const Watching watching(stop, *workflow);
            RunSummary summary;
            try {
                summary = workflow->run();
            } catch (const Error&) {
                if (!output.json()) {
                    RunSummary failed_run;
                    failed_run.final_state = workflow->state();
                    failed_run.fingerprint = workflow->fingerprint();
                    failed_run.ticks_completed = workflow->status().ticks_completed;
                    std::cout << summary_text(*workflow, failed_run);
                }
                throw;
            }
            if (summary.final_state == WorkflowState::stopped) {
                if (!output.json()) {
                    std::cout << summary_text(*workflow, summary);
                }
                const Error stopped(ErrorCode::RUN_STOPPED,
                                    "the run was stopped after " +
                                        std::to_string(summary.ticks_completed) + " tick(s)",
                                    {}, dump(summary_json(*workflow, summary)));
                return output.failure(stopped, kExitStopped);
            }
            return output.result(summary_json(*workflow, summary),
                                 summary_text(*workflow, summary));
        });
}

} // namespace praenora::cli
