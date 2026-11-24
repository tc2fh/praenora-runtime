// The run record (kind run-record, run-record.json in the output directory): what a run used, in
// the hashed `reproducibility` section, and how it went, in the `execution` section. Also the
// preparation of a run's output directory.
#ifndef PRAENORA_RUNTIME_IO_RUN_RECORD_HPP
#define PRAENORA_RUNTIME_IO_RUN_RECORD_HPP

#include "config/workflow_config.hpp"
#include "io/output_recorder.hpp"
#include "observability/path_anchors.hpp"

#include <praenora/runtime/status.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace praenora::detail {

// Checks, without changing anything, that `dir` can receive a run's files: it is missing (to be
// created) or a directory, and, unless `overwrite`, it holds no earlier run (no run-record.json).
// Throws praenora::Error(IO_ERROR) otherwise: "output directory already contains a run record;
// use overwrite", located at that file, for an earlier run. Lets a caller refuse an earlier run
// before it does anything else, and clean it up (prepare_output_dir()) only once everything else
// that can fail has succeeded.
void check_output_dir(const std::filesystem::path& dir, bool overwrite);

// The paths in output directory `dir` that Praenora manages and that an overwrite removes
// (prepare_output_dir()): outputs/, trace/, status.json and run-record.json, in that order.
std::vector<std::filesystem::path> managed_output_paths(const std::filesystem::path& dir);

// Makes `dir` ready to receive a run's files: check_output_dir() first, then it creates `dir` (and
// its parents) when it is missing. When it holds run-record.json, it holds an earlier run, which
// `overwrite` replaces: the files Praenora manages there are removed, and only those: outputs/,
// trace/, status.json and run-record.json (the last one last). Everything else stays, log files
// included: they are appended to across runs. A directory without run-record.json is used as it
// is, even with `overwrite`. Throws praenora::Error(IO_ERROR) as check_output_dir() does, and when
// `dir` cannot be created or cleaned.
void prepare_output_dir(const std::filesystem::path& dir, bool overwrite);

// Keeps the run record of one run and writes it, replacing the file atomically each time, as
// UTF-8 JSON with two-space indentation, "\n" line endings and a final newline:
//   {"kind": "run-record", "format_version", "fingerprint", "reproducibility",
//    "execution": {"status": "running"|"completed"|"stopped"|"failed", "started_at",
//                  "finished_at" (null until the end), "ticks_completed", "error" (null or an
//                  error record), "platform": {"os", "arch", "compiler", "build_type"},
//                  "overrides": {"seed"?, "ticks"?, "isolation"?}, "validation": "strict"|"off",
//                  "manifests": {"steps", "services"} (where each manifest was found),
//                  "inputs": {<input>: {"rows_total", "rows_before_start"}} (what each replay
//                  source found in its data),
//                  "isolation": {<step>: {"mode", "share_compiled_models"}} (how each step's
//                  instance was isolated), and
//                  at the end "final_status" (the final status snapshot), "outputs" and
//                  "trace"}}.
// Wall-clock instants are RFC 3339 UTC with nine fractional digits. Nothing names the machine or
// its user. In the error records (execution.error and the last errors of final_status), every
// directory of the writer's anchors is named by its placeholder (PathAnchors), in the record kept
// (current()) as in the file; a path under no anchor stays as it is. No other absolute path is
// written but in values copied from the configuration and from manifests (parameters, a
// provider's engine object), which hold whatever those documents hold. Not thread-safe.
class RunRecordWriter {
public:
    // `output_dir` holds run-record.json; when it is empty, the record is kept in memory only.
    // `manifests` is execution.manifests (ValidatedWorkflow::manifest_locations), `isolation`
    // execution.isolation (ValidatedWorkflow::isolation), `inputs` execution.inputs
    // (ValidatedWorkflow::input_counts). `overrides` are
    // the run settings the caller overrode; `strict_validation` tells whether payloads are
    // validated against their message types ("strict") or not ("off"). `anchors` are the
    // directories of the run (PathAnchors) that its error records name by placeholders; without
    // them the error records keep their absolute paths. Throws praenora::Error(INVALID_ARGUMENT)
    // unless `fingerprint` is the SHA-256 of canonical_json(reproducibility).
    RunRecordWriter(std::filesystem::path output_dir, nlohmann::json reproducibility,
                    std::string fingerprint, nlohmann::json manifests, nlohmann::json isolation,
                    RunOverrides overrides = {}, bool strict_validation = true,
                    std::shared_ptr<const PathAnchors> anchors = nullptr,
                    nlohmann::json inputs = nlohmann::json::object());

    // The record of a run that has just started (status running). Throws
    // praenora::Error(IO_ERROR) when the file cannot be written (current() is updated anyway).
    void write_started(std::int64_t started_unix_ns);

    // The record of a run that has ended: its status (summary.final_state: completed, stopped or
    // failed; anything else is INVALID_ARGUMENT), ticks completed and error from `summary`, the
    // end time, the recorded output files and, when given, the message trace, and the final
    // status snapshot `final_status_json` (INVALID_ARGUMENT unless a JSON object). Throws
    // praenora::Error(IO_ERROR) when the file cannot be written (current() is updated anyway).
    void write_final(const RunSummary& summary, std::int64_t finished_unix_ns,
                     const std::vector<OutputRecorder::FileInfo>& outputs,
                     const std::string& final_status_json,
                     const std::optional<OutputRecorder::FileInfo>& trace = std::nullopt);

    // The record as last written (or as it would have been, if the write failed).
    const nlohmann::json& current() const;

private:
    void write() const;

    std::filesystem::path file_; // empty: memory only
    std::shared_ptr<const PathAnchors> anchors_;
    nlohmann::json document_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_IO_RUN_RECORD_HPP
