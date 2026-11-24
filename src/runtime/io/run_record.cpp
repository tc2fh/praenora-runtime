#include "io/run_record.hpp"

#include "compat/compat.hpp"
#include "core/paths.hpp"
#include "core/sha256.hpp"
#include "core/time.hpp"
#include "document/json_util.hpp"
#include "observability/status_json.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/version.hpp>

#include <system_error>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kRunRecordFile = "run-record.json";

std::string quoted(const fs::path& path) { return "'" + path_to_utf8(path) + "'"; }

// An IO_ERROR located at `path`.
Error io_error(std::string message, const fs::path& path) {
    SourceLocation location;
    location.file = path_to_utf8(path);
    return Error(ErrorCode::IO_ERROR, std::move(message), std::move(location));
}

json status_object(const std::string& status_json) {
    json status = json::parse(status_json, nullptr, false);
    if (!status.is_object()) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "run record: the status snapshot is not a JSON object");
    }
    return status;
}

json file_json(const OutputRecorder::FileInfo& info) {
    return {{"file", info.path}, {"sha256", info.sha256}, {"messages", info.messages}};
}

} // namespace

void check_output_dir(const fs::path& dir, bool overwrite) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return; // to be created
    }
    if (!fs::is_directory(dir, ec)) {
        throw io_error("the output directory " + quoted(dir) + " is not a directory", dir);
    }
    const fs::path record = dir / kRunRecordFile;
    if (!overwrite && fs::exists(fs::symlink_status(record, ec))) {
        SourceLocation location;
        location.file = path_to_utf8(record);
        throw Error(ErrorCode::IO_ERROR,
                    "output directory already contains a run record; use overwrite", location);
    }
}

std::vector<fs::path> managed_output_paths(const fs::path& dir) {
    return {dir / "outputs", dir / "trace", dir / "status.json", dir / kRunRecordFile};
}

void prepare_output_dir(const fs::path& dir, bool overwrite) {
    check_output_dir(dir, overwrite);
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        fs::create_directories(dir, ec);
        if (ec) {
            throw io_error("cannot create directory " + quoted(dir) + ": " + ec.message(), dir);
        }
        return;
    }
    const fs::path record = dir / kRunRecordFile;
    if (!fs::exists(fs::symlink_status(record, ec))) {
        return; // no earlier run
    }
    // An earlier run, and overwrite (check_output_dir() refused it otherwise). The run record goes
    // last, so a directory that could not be cleaned completely still says that it holds an
    // earlier run.
    for (const fs::path& path : managed_output_paths(dir)) {
        fs::remove_all(path, ec);
        if (ec) {
            throw io_error("cannot remove " + quoted(path) + ": " + ec.message(), path);
        }
    }
}

RunRecordWriter::RunRecordWriter(fs::path output_dir, json reproducibility, std::string fingerprint,
                                 json manifests, json isolation, RunOverrides overrides,
                                 bool strict_validation, std::shared_ptr<const PathAnchors> anchors,
                                 json inputs)
    : anchors_(std::move(anchors)) {
    if (sha256_hex(canonical_json(reproducibility)) != fingerprint) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "run record: the fingerprint is not the hash of the reproducibility section");
    }
    if (!output_dir.empty()) {
        file_ = output_dir / kRunRecordFile;
    }
    const VersionInfo versions = version_info();
    const HostPlatform platform = host_platform();
    json overridden = json::object();
    if (overrides.seed) {
        overridden["seed"] = *overrides.seed;
    }
    if (overrides.ticks) {
        overridden["ticks"] = *overrides.ticks;
    }
    if (overrides.isolation) {
        overridden["isolation"] = std::string(isolation_mode_name(*overrides.isolation));
    }
    document_ = {{"kind", "run-record"},
                 {"format_version", versions.formats.at("run-record")},
                 {"fingerprint", std::move(fingerprint)},
                 {"reproducibility", std::move(reproducibility)},
                 {"execution",
                  {{"status", "running"},
                   {"started_at", nullptr},
                   {"finished_at", nullptr},
                   {"ticks_completed", 0},
                   {"error", nullptr},
                   {"platform",
                    {{"os", platform.os},
                     {"arch", platform.arch},
                     {"compiler", versions.compiler},
                     {"build_type", versions.build_type}}},
                   {"overrides", std::move(overridden)},
                   {"validation", strict_validation ? "strict" : "off"},
                   {"manifests", std::move(manifests)},
                   {"isolation", std::move(isolation)},
                   {"inputs", std::move(inputs)}}}};
}

void RunRecordWriter::write_started(std::int64_t started_unix_ns) {
    json& execution = document_.at("execution");
    execution["status"] = "running";
    execution["started_at"] = format_rfc3339_utc(started_unix_ns);
    execution["finished_at"] = nullptr;
    execution["ticks_completed"] = 0;
    execution["error"] = nullptr;
    execution.erase("final_status");
    execution.erase("outputs");
    execution.erase("trace");
    write();
}

void RunRecordWriter::write_final(const RunSummary& summary, std::int64_t finished_unix_ns,
                                  const std::vector<OutputRecorder::FileInfo>& outputs,
                                  const std::string& final_status_json,
                                  const std::optional<OutputRecorder::FileInfo>& trace) {
    if (summary.final_state != WorkflowState::completed &&
        summary.final_state != WorkflowState::stopped &&
        summary.final_state != WorkflowState::failed) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "run record: a run cannot end in state " + to_string(summary.final_state));
    }
    json final_status = status_object(final_status_json);
    json error = summary.error ? error_record_json(*summary.error) : json(nullptr);
    if (anchors_) {
        // The record is written for others to read: the directories the run knows are named by
        // their placeholders, in its error records only.
        if (error.is_object()) {
            anchors_->apply_to_error_record(error);
        }
        anchors_->apply_to_status_snapshot(final_status);
    }
    json files = json::object();
    for (const OutputRecorder::FileInfo& info : outputs) {
        files[info.port] = file_json(info);
    }
    json& execution = document_.at("execution");
    execution["status"] = to_string(summary.final_state);
    execution["finished_at"] = format_rfc3339_utc(finished_unix_ns);
    execution["ticks_completed"] = summary.ticks_completed;
    execution["error"] = std::move(error);
    execution["outputs"] = std::move(files);
    if (trace) {
        execution["trace"] = file_json(*trace);
    } else {
        execution.erase("trace");
    }
    execution["final_status"] = std::move(final_status);
    write();
}

const json& RunRecordWriter::current() const { return document_; }

void RunRecordWriter::write() const {
    if (file_.empty()) {
        return;
    }
    // Messages may come from modules: bytes that are not UTF-8 are replaced, not refused.
    std::string text = document_.dump(2, ' ', false, json::error_handler_t::replace);
    text += '\n';
    write_file_atomically(file_, text);
}

} // namespace praenora::detail
