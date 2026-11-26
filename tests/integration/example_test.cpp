// The example workflow example.estimate-predict end to end, through the public API only (the
// shared library): the example modules (built from examples/modules), its configuration and data
// (examples/workflows/estimate-predict) and its golden outputs
// (tests/fixtures/golden/example.estimate-predict). Covers the expectations of the example
// (12 ticks, feedback from tick 1 on, golden outputs), determinism, what changes the run
// fingerprint and what does not, the run record (of a failed run too) and the status file on disk,
// an output directory that holds an earlier run, paths with spaces and non-ASCII characters, status
// queries during a run and a stop request (with the test catalog's test.chain and fixture modules).
//
// The golden files are refreshed only on request: PRAENORA_UPDATE_GOLDEN=1 makes
// Example.OutputsMatchGolden write them instead of comparing. Their envelopes hold sixteen zeros
// where an output's id holds the first 16 hex digits of the run fingerprint, which covers the
// module libraries' digests and so differs from build to build.
#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/workflow.hpp>

#include "directory_checks.hpp"
#include "json_compare.hpp"
#include "module_fixtures.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::Health;
using praenora::LogFormat;
using praenora::LogLevel;
using praenora::RunSummary;
using praenora::Runtime;
using praenora::RuntimeOptions;
using praenora::StatusSnapshot;
using praenora::StepState;
using praenora::ValidationReport;
using praenora::Workflow;
using praenora::WorkflowOptions;
using praenora::WorkflowState;
using praenora::test::directory_forms;
using praenora::test::expect_names_none;
using praenora::test::named_path;
using praenora::test::ScratchDir;
using praenora::test::utf8_path;
using praenora::test::write_text_file;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::uint64_t kTicks = 12;

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// The example modules (or the module directories given), without the environment and without
// logging to standard error.
RuntimeOptions example_options(std::vector<std::string> module_paths = {
                                   PRAENORA_EXAMPLE_MODULES_DIR}) {
    RuntimeOptions options;
    options.module_paths = std::move(module_paths);
    options.use_environment = false;
    options.logging.to_stderr = false;
    return options;
}

fs::path example_dir() { return utf8_path(PRAENORA_EXAMPLE_WORKFLOW_DIR); }

std::string example_config() { return utf8(example_dir() / "workflow.yaml"); }

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The JSON lines of `file`, parsed.
std::vector<json> json_lines(const fs::path& file) {
    std::vector<json> lines;
    std::istringstream text(read_file(file));
    for (std::string line; std::getline(text, line);) {
        lines.push_back(json::parse(line));
    }
    return lines;
}

// The praenora::Error that `call` throws; fails the test when it throws none.
template <class Call> Error error_of(Call&& call) {
    try {
        call();
    } catch (const Error& error) {
        return error;
    }
    ADD_FAILURE() << "no praenora::Error was thrown";
    return Error(ErrorCode::OK, "nothing thrown");
}

const praenora::StepStatus& step_status(const StatusSnapshot& status, const std::string& id) {
    for (const auto& step : status.steps) {
        if (step.step == id) {
            return step;
        }
    }
    throw std::runtime_error("no step " + id + " in the status");
}

struct ExampleRun {
    RunSummary summary;
    StatusSnapshot status;
    std::string fingerprint;
    fs::path out;
};

// Runs `config` (by default the example's) to its end with its output directory at `out`, and
// closes the workflow.
ExampleRun run_example(const Runtime& runtime, const fs::path& out, WorkflowOptions options = {},
                       const std::string& config = example_config()) {
    options.output_dir = utf8(out);
    const std::unique_ptr<Workflow> workflow = runtime.open(config, options);
    ExampleRun result;
    result.summary = workflow->run();
    result.status = workflow->status();
    result.fingerprint = workflow->fingerprint();
    result.out = out;
    workflow->close();
    return result;
}

// Whether the golden files are to be rewritten (PRAENORA_UPDATE_GOLDEN=1) instead of compared.
bool update_golden() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv is fine here: nothing changes the environment
#endif
    const char* value = std::getenv("PRAENORA_UPDATE_GOLDEN");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return value != nullptr && std::string(value) == "1";
}

// The envelope with the run fingerprint's 16 hex digits in its id replaced by zeros.
json without_fingerprint(json envelope) {
    const std::string id = envelope.at("id").get<std::string>();
    envelope["id"] = std::string(16, '0') + id.substr(16);
    return envelope;
}

} // namespace

TEST(Example, RunsTwelveTicks) {
    ScratchDir dir;
    const Runtime runtime(example_options());
    const ExampleRun run = run_example(runtime, dir.path() / "out");
    EXPECT_EQ(run.summary.final_state, WorkflowState::completed);
    EXPECT_EQ(run.summary.ticks_completed, kTicks);
    EXPECT_FALSE(run.summary.error.has_value());
    EXPECT_EQ(run.status.state, WorkflowState::completed);
    EXPECT_EQ(run.status.health, Health::healthy);
    EXPECT_EQ(run.status.ticks_completed, kTicks);
    ASSERT_EQ(run.status.steps.size(), 2u);
    for (const char* id : {"estimate", "predict"}) {
        SCOPED_TRACE(id);
        const praenora::StepStatus& step = step_status(run.status, id);
        EXPECT_EQ(step.invocations, kTicks);
        EXPECT_EQ(step.succeeded, kTicks);
        EXPECT_EQ(step.failed + step.timed_out + step.cancelled, 0u);
        EXPECT_EQ(step.state, StepState::destroyed);
        EXPECT_FALSE(step.last_error.has_value());
    }
    EXPECT_EQ(step_status(run.status, "estimate").module_name, "example-estimator");
    EXPECT_EQ(step_status(run.status, "predict").module_name, "example-predictor");
}

TEST(Example, FeedbackStartsAtTickOne) {
    ScratchDir dir;
    const Runtime runtime(example_options());
    const ExampleRun run = run_example(runtime, dir.path() / "out");
    ASSERT_EQ(run.summary.final_state, WorkflowState::completed);
    const std::vector<json> states = json_lines(run.out / "outputs" / "state.jsonl");
    ASSERT_EQ(states.size(), kTicks);
    for (std::uint64_t tick = 0; tick < kTicks; ++tick) {
        SCOPED_TRACE("tick " + std::to_string(tick));
        const json& envelope = states[tick];
        EXPECT_EQ(envelope.at("tick"), tick);
        const json& estimates = envelope.at("data").at("estimates");
        ASSERT_FALSE(estimates.empty());
        bool blended = false;
        for (const json& estimate : estimates) {
            const std::string basis = estimate.at("basis").get<std::string>();
            blended = blended || basis == "blend";
            if (tick == 0) {
                // No prediction exists before tick 0: the feedback connection is delayed.
                EXPECT_EQ(basis, "observation") << estimate.dump();
            }
        }
        if (tick >= 1) {
            EXPECT_TRUE(blended) << envelope.dump(); // the previous tick's predictions arrived
        }
    }
}

TEST(Example, OutputsMatchGolden) {
    ScratchDir dir;
    const Runtime runtime(example_options());
    const ExampleRun run = run_example(runtime, dir.path() / "out");
    ASSERT_EQ(run.summary.final_state, WorkflowState::completed);
    const fs::path golden_dir = utf8_path(PRAENORA_TEST_GOLDEN_DIR) / "example.estimate-predict";
    for (const std::string output : {"predictions", "state"}) {
        SCOPED_TRACE(output);
        const std::vector<json> lines = json_lines(run.out / "outputs" / (output + ".jsonl"));
        ASSERT_EQ(lines.size(), kTicks);
        const fs::path golden = golden_dir / (output + ".jsonl");
        if (update_golden()) {
            std::string text;
            for (const json& line : lines) {
                text += without_fingerprint(line).dump() + "\n";
            }
            write_text_file(golden, text);
            std::cout << "refreshed " << utf8(golden) << "\n";
            continue;
        }
        ASSERT_TRUE(fs::is_regular_file(golden))
            << utf8(golden) << " is missing; PRAENORA_UPDATE_GOLDEN=1 writes it";
        const std::vector<json> expected = json_lines(golden);
        ASSERT_EQ(expected.size(), lines.size());
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::string where;
            EXPECT_TRUE(praenora::test::json_near(without_fingerprint(lines[i]), expected[i], 1e-9,
                                                  1e-12, &where))
                << output << ".jsonl line " << i + 1 << ": " << where;
        }

        // The comparison can fail: a value moved beyond the tolerance is refused, one moved
        // within it is accepted.
        const std::string rows = output == "state" ? "estimates" : "predictions";
        json moved = expected.back();
        const double value = moved.at("data").at(rows).at(0).at("value").get<double>();
        moved["data"][rows][0]["value"] = value * (1.0 + 1e-7);
        std::string where;
        EXPECT_FALSE(praenora::test::json_near(moved, expected.back(), 1e-9, 1e-12, &where));
        EXPECT_EQ(where, "/data/" + rows + "/0/value: " + moved["data"][rows][0]["value"].dump() +
                             " differs from " + json(value).dump());
        moved["data"][rows][0]["value"] = value * (1.0 + 1e-11);
        EXPECT_TRUE(praenora::test::json_near(moved, expected.back(), 1e-9, 1e-12, &where));
    }
}

TEST(Example, TwoRunsAreByteIdentical) {
    ScratchDir dir;
    const Runtime runtime(example_options());
    const ExampleRun first = run_example(runtime, dir.path() / "first");
    const ExampleRun second = run_example(runtime, dir.path() / "second");
    ASSERT_EQ(first.summary.final_state, WorkflowState::completed);
    ASSERT_EQ(second.summary.final_state, WorkflowState::completed);
    EXPECT_EQ(first.fingerprint, second.fingerprint);
    EXPECT_EQ(first.summary.fingerprint, first.fingerprint);
    for (const char* file : {"outputs/predictions.jsonl", "outputs/state.jsonl"}) {
        SCOPED_TRACE(file);
        const std::string bytes = read_file(first.out / file);
        EXPECT_FALSE(bytes.empty());
        EXPECT_EQ(bytes, read_file(second.out / file));
    }
    const json first_record = json::parse(read_file(first.out / "run-record.json"));
    const json second_record = json::parse(read_file(second.out / "run-record.json"));
    EXPECT_EQ(first_record.at("reproducibility"), second_record.at("reproducibility"));
    EXPECT_EQ(first_record.at("execution").at("outputs"),
              second_record.at("execution").at("outputs"));
}

TEST(Example, LogLevelDoesNotChangeFingerprint) {
    ScratchDir dir;
    const Runtime runtime(example_options());
    // Everything at trace level in JSON lines, against errors only: the logs differ, the run does
    // not.
    WorkflowOptions verbose;
    verbose.log_level = LogLevel::trace;
    verbose.log_format = LogFormat::json;
    verbose.log_file = utf8(dir.path() / "verbose.log");
    WorkflowOptions quiet;
    quiet.log_level = LogLevel::error;
    quiet.log_format = LogFormat::json;
    quiet.log_file = utf8(dir.path() / "quiet.log");
    const ExampleRun loud = run_example(runtime, dir.path() / "verbose", verbose);
    const ExampleRun calm = run_example(runtime, dir.path() / "quiet", quiet);
    ASSERT_EQ(loud.summary.final_state, WorkflowState::completed);
    ASSERT_EQ(calm.summary.final_state, WorkflowState::completed);
    EXPECT_EQ(loud.fingerprint, calm.fingerprint);
    EXPECT_EQ(read_file(loud.out / "outputs" / "state.jsonl"),
              read_file(calm.out / "outputs" / "state.jsonl"));

    // The settings did take effect on the run's own logger. (The predict step's log_level of the
    // configuration filters that step's module logger whatever the run's level.)
    const auto engine_events = [](const fs::path& file) {
        std::vector<json> events;
        for (const json& event : json_lines(file)) {
            if (event.at("attributes").at("logger") == "praenora.engine") {
                events.push_back(event);
            }
        }
        return events;
    };
    const std::vector<json> verbose_events = engine_events(dir.path() / "verbose.log");
    EXPECT_TRUE(std::any_of(verbose_events.begin(), verbose_events.end(), [](const json& event) {
        return event.at("severity_text") == "DEBUG";
    }));
    for (const json& event : verbose_events) {
        EXPECT_EQ(event.at("attributes").at("run.fingerprint"), loud.fingerprint);
    }
    EXPECT_TRUE(engine_events(dir.path() / "quiet.log").empty()); // nothing went wrong
}

TEST(Example, SeedChangesFingerprint) {
    const Runtime runtime(example_options());
    const std::string fingerprint = runtime.open(example_config())->fingerprint();
    WorkflowOptions reseeded;
    reseeded.seed = 1;
    const std::unique_ptr<Workflow> other = runtime.open(example_config(), reseeded);
    EXPECT_NE(other->fingerprint(), fingerprint);
    // The configuration's own seed, given again, changes nothing.
    WorkflowOptions same;
    same.seed = 20261110;
    EXPECT_EQ(runtime.open(example_config(), same)->fingerprint(), fingerprint);
}

TEST(Example, RunRecordValidatesAgainstSchema) {
    ScratchDir dir;
    const Runtime runtime(example_options());
    const ExampleRun run = run_example(runtime, dir.path() / "out");
    ASSERT_EQ(run.summary.final_state, WorkflowState::completed);
    const fs::path file = run.out / "run-record.json";
    const ValidationReport report = runtime.validate_document(utf8(file));
    EXPECT_TRUE(report.ok()) << report.to_text();
    EXPECT_TRUE(report.diagnostics.empty()) << report.to_text();

    const json record = json::parse(read_file(file));
    EXPECT_EQ(record.at("kind"), "run-record");
    EXPECT_EQ(record.at("fingerprint"), run.fingerprint);
    EXPECT_EQ(record.at("reproducibility").at("seed"), 20261110);
    EXPECT_EQ(record.at("reproducibility").at("ticks"), kTicks);
    const json& execution = record.at("execution");
    EXPECT_EQ(execution.at("status"), "completed");
    EXPECT_EQ(execution.at("ticks_completed"), kTicks);
    EXPECT_TRUE(execution.at("error").is_null());
    EXPECT_EQ(execution.at("validation"), "strict");
    EXPECT_EQ(execution.at("outputs").at("predictions").at("messages"), kTicks);
    EXPECT_EQ(execution.at("outputs").at("state").at("file"), "outputs/state.jsonl");
    EXPECT_EQ(execution.at("final_status").at("workflow").at("state"), "completed");
    // Where each step's manifest was found: relative to the caller's module path, the first one.
    EXPECT_EQ(execution.at("manifests"), json::parse(R"({
        "steps": {
            "estimate": {"search_path": {"origin": "caller", "index": 0},
                         "file": "example-estimator/example-estimator.pmm.json"},
            "predict": {"search_path": {"origin": "caller", "index": 0},
                        "file": "example-predictor/example-predictor.pmm.json"}},
        "services": {}})"));
    // No absolute path anywhere in the record of a run without an error: it names neither the
    // directories of the modules, the configuration and the output nor the user.
    expect_names_none(record, directory_forms({utf8_path(PRAENORA_EXAMPLE_MODULES_DIR),
                                               example_dir(), dir.path()}));

    // The same check catches a record that breaks the schema.
    json broken = record;
    broken["execution"]["status"] = "done";
    write_text_file(dir.path() / "broken.json", broken.dump());
    const ValidationReport refused = runtime.validate_document(utf8(dir.path() / "broken.json"));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.diagnostics.front().code, ErrorCode::SCHEMA_VIOLATION) << refused.to_text();
    EXPECT_EQ(refused.diagnostics.front().location.pointer, "/execution/status")
        << refused.to_text();
}

// Nor in the record of a failed run: its error records name the directories the run knows by
// placeholders, here the output directory, which cannot receive the outputs.
TEST(Example, FailedRunRecordHoldsNoAbsolutePath) {
    ScratchDir dir;
    const Runtime runtime(example_options());
    const fs::path out = dir.path() / "out";
    write_text_file(out / "outputs", "a file, not a directory\n");
    WorkflowOptions options;
    options.output_dir = utf8(out);
    const std::unique_ptr<Workflow> workflow = runtime.open(example_config(), options);
    const Error error = error_of([&] { workflow->run(); });
    ASSERT_EQ(error.code(), ErrorCode::IO_ERROR) << error.to_json();
    EXPECT_EQ(error.location().file, utf8(out / "outputs"));
    workflow->close();

    const fs::path file = out / "run-record.json";
    const ValidationReport report = runtime.validate_document(utf8(file));
    EXPECT_TRUE(report.ok()) << report.to_text();
    const json record = json::parse(read_file(file));
    const json& execution = record.at("execution");
    EXPECT_EQ(execution.at("status"), "failed");
    const std::string outputs = named_path("<output>", out, out / "outputs");
    EXPECT_EQ(execution.at("error").at("location").at("file"), outputs);
    EXPECT_TRUE(contains(execution.at("error").at("message").get<std::string>(), outputs))
        << execution.at("error").dump();
    expect_names_none(record, directory_forms({utf8_path(PRAENORA_EXAMPLE_MODULES_DIR),
                                               example_dir(), dir.path()}));
}

TEST(Example, StatusFileWritten) {
    ScratchDir dir;
    const Runtime runtime(example_options());
    WorkflowOptions options;
    options.status_file = utf8(dir.path() / "out" / "status.json");
    const ExampleRun run = run_example(runtime, dir.path() / "out", options);
    ASSERT_EQ(run.summary.final_state, WorkflowState::completed);

    const json status = json::parse(Runtime::read_status_file(*options.status_file));
    EXPECT_EQ(status.at("kind"), "status-snapshot");
    const json& workflow = status.at("workflow");
    EXPECT_EQ(workflow.at("name"), "example.estimate-predict");
    EXPECT_EQ(workflow.at("fingerprint"), run.fingerprint);
    EXPECT_EQ(workflow.at("state"), "completed");
    EXPECT_EQ(workflow.at("health"), "healthy");
    EXPECT_EQ(workflow.at("ticks_completed"), kTicks);
    ASSERT_EQ(status.at("steps").size(), 2u);
    for (const json& step : status.at("steps")) {
        EXPECT_EQ(step.at("invocations"), kTicks) << step.dump();
        EXPECT_EQ(step.at("state"), "destroyed") << step.dump();
    }
    const ValidationReport report = runtime.validate_document(*options.status_file);
    EXPECT_TRUE(report.ok()) << report.to_text();

    // A file of another kind is not a status file.
    EXPECT_EQ(
        error_of([&] { Runtime::read_status_file(utf8(run.out / "run-record.json")); }).code(),
        ErrorCode::DOCUMENT_KIND_MISMATCH);
    EXPECT_EQ(error_of([&] { Runtime::read_status_file(utf8(dir.path() / "none.json")); }).code(),
              ErrorCode::IO_ERROR);
}

TEST(Example, ExistingOutputDirRequiresOverwrite) {
    ScratchDir dir;
    const Runtime runtime(example_options());
    const fs::path out = dir.path() / "out";
    ASSERT_EQ(run_example(runtime, out).summary.final_state, WorkflowState::completed);
    write_text_file(out / "notes.txt", "mine\n");
    write_text_file(out / "logs" / "praenora.log", "an earlier log line\n");
    const std::string earlier = read_file(out / "run-record.json");

    // Without overwrite the earlier run is refused, before anything else happens.
    WorkflowOptions options;
    options.output_dir = utf8(out);
    const Error error = error_of([&] { runtime.open(example_config(), options); });
    EXPECT_EQ(error.code(), ErrorCode::IO_ERROR) << error.to_json();
    EXPECT_TRUE(contains(error.what(), "already contains a run record")) << error.what();
    EXPECT_TRUE(contains(error.location().file, "run-record.json")) << error.to_json();
    EXPECT_EQ(read_file(out / "run-record.json"), earlier);

    // With overwrite only the files the runtime manages are replaced.
    options.overwrite = true;
    const ExampleRun again = run_example(runtime, out, options);
    EXPECT_EQ(again.summary.final_state, WorkflowState::completed);
    EXPECT_NE(read_file(out / "run-record.json"), earlier);
    EXPECT_EQ(json_lines(out / "outputs" / "state.jsonl").size(), kTicks);
    EXPECT_EQ(read_file(out / "notes.txt"), "mine\n");
    EXPECT_EQ(read_file(out / "logs" / "praenora.log"), "an earlier log line\n");
}

TEST(Example, PathsWithSpacesAndUnicode) {
    ScratchDir dir;
    // The example and its modules copied below "praenora t\u00ebst dir", their own directory
    // names holding spaces and non-ASCII characters too.
    const fs::path root = dir.path() / utf8_path("praenora t\xC3\xABst dir");
    const fs::path workflow_dir = root / utf8_path("estimate predict \xE2\x86\x92 copy");
    const fs::path module_dir = root / utf8_path("m\xC3\xB6"
                                                 "dules");
    fs::create_directories(root);
    fs::copy(example_dir(), workflow_dir, fs::copy_options::recursive);
    fs::copy(utf8_path(PRAENORA_EXAMPLE_MODULES_DIR), module_dir, fs::copy_options::recursive);

    const Runtime runtime(example_options({utf8(module_dir)}));
    // The copies are what the runtime finds.
    const praenora::ModuleInfo estimator = runtime.inspect_module("example-estimator", {}, true);
    EXPECT_TRUE(contains(estimator.manifest_path, utf8(module_dir))) << estimator.manifest_path;
    const ExampleRun copied = run_example(runtime, root / utf8_path("out put \xC3\xA9"), {},
                                          utf8(workflow_dir / "workflow.yaml"));
    ASSERT_EQ(copied.summary.final_state, WorkflowState::completed)
        << (copied.summary.error ? copied.summary.error->message : "");
    EXPECT_EQ(copied.summary.ticks_completed, kTicks);

    // Paths are not part of a run: the same files elsewhere give the same run.
    const Runtime original(example_options());
    const ExampleRun reference = run_example(original, dir.path() / "reference");
    EXPECT_EQ(copied.fingerprint, reference.fingerprint);
    for (const char* file : {"outputs/predictions.jsonl", "outputs/state.jsonl"}) {
        EXPECT_EQ(read_file(copied.out / file), read_file(reference.out / file)) << file;
    }
    // The records say where the manifests were found relative to the search path, so they agree.
    const auto manifests = [](const fs::path& out) {
        return json::parse(read_file(out / "run-record.json")).at("execution").at("manifests");
    };
    EXPECT_EQ(manifests(copied.out), manifests(reference.out));
}

TEST(Example, StatusQueriesDuringRunAreSafe) {
    ScratchDir dir;
    const Runtime runtime(example_options());
    WorkflowOptions options;
    options.output_dir = utf8(dir.path() / "out");
    const std::unique_ptr<Workflow> workflow = runtime.open(example_config(), options);

    std::atomic<bool> done{false};
    std::atomic<int> queries{0};
    std::atomic<int> backwards{0};
    std::atomic<int> malformed{0};
    std::atomic<std::uint64_t> last_seen{0};
    std::thread observer([&] {
        std::uint64_t last = 0;
        while (!done.load()) {
            const StatusSnapshot status = workflow->status();
            if (status.ticks_completed < last) {
                backwards.fetch_add(1);
            }
            last = status.ticks_completed;
            last_seen.store(last);
            const json document = json::parse(workflow->status_json());
            if (document.at("workflow").at("ticks_completed").get<std::uint64_t>() < last) {
                backwards.fetch_add(1);
            }
            // json::accept(), not json::parse() cast to void: GCC reports a discarded parse()
            // even then (nlohmann declares it warn_unused_result).
            for (const std::string& text :
                 {workflow->metrics_json(), workflow->latest_outputs_json(),
                  workflow->run_record_json()}) {
                if (!json::accept(text)) {
                    malformed.fetch_add(1);
                }
            }
            static_cast<void>(workflow->state());
            queries.fetch_add(1);
        }
    });
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (queries.load() == 0 && Clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (queries.load() == 0) {
        done.store(true);
        observer.join();
        FAIL() << "the observer made no query within 10 s";
    }
    const RunSummary summary = workflow->run();
    done.store(true);
    observer.join();
    EXPECT_EQ(summary.final_state, WorkflowState::completed);
    EXPECT_EQ(summary.ticks_completed, kTicks);
    EXPECT_GT(queries.load(), 0);
    EXPECT_EQ(backwards.load(), 0);
    EXPECT_EQ(malformed.load(), 0);
    EXPECT_LE(last_seen.load(), kTicks);
    EXPECT_EQ(workflow->status().ticks_completed, kTicks);
}

TEST(Example, RequestStopEndsRunAsStopped) {
    // The test catalog's test.chain: a (fx-echo), then b (fx-slow), which sleeps 5 s in each
    // invocation, polling for cancellation.
    ScratchDir dir;
    RuntimeOptions runtime_options;
    runtime_options.module_paths = {PRAENORA_TEST_MODULES_DIR};
    runtime_options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    runtime_options.use_environment = false;
    runtime_options.logging.to_stderr = false;
    const Runtime runtime(runtime_options);
    write_text_file(dir.path() / "source.json", "{\"value\": 1}");
    // A configuration in `name`.yaml whose step b sleeps `sleep_ms` in each invocation.
    const auto chain = [&dir](const std::string& name, int sleep_ms) {
        const fs::path file = dir.path() / (name + ".yaml");
        write_text_file(file, "kind: workflow-config\n"
                              "format_version: \"0.1.0\"\n"
                              "workflow: { name: test.chain, version: \"0.1.0\" }\n"
                              "run: { ticks: 5 }\n"
                              "inputs:\n"
                              "  source: { source: json-document, path: source.json }\n"
                              "steps:\n"
                              "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"
                              "  b:\n"
                              "    module: { name: fx-slow, version: \"0.1.0\" }\n"
                              "    parameters: { sleep_ms: " +
                                  std::to_string(sleep_ms) +
                                  " }\n"
                                  "outputs: { directory: " +
                                  name + " }\n");
        return utf8(file);
    };
    const std::string config = chain("slow", 5000);

    // A stop from another thread while b's invocation runs: the call is cancelled, and run()
    // returns normally. (b is busy during its create and initialize too, while run() starts the
    // run; only a busy b in a running run is in its invocation.)
    const std::unique_ptr<Workflow> workflow = runtime.open(config);
    std::thread stopper([&] {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        for (;;) {
            const StatusSnapshot status = workflow->status();
            if ((status.state == WorkflowState::running &&
                 step_status(status, "b").state == StepState::busy) ||
                Clock::now() >= deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        workflow->request_stop();
    });
    RunSummary summary;
    EXPECT_NO_THROW(summary = workflow->run());
    stopper.join();
    EXPECT_EQ(summary.final_state, WorkflowState::stopped);
    EXPECT_EQ(summary.ticks_completed, 0u);
    ASSERT_TRUE(summary.error.has_value());
    EXPECT_EQ(summary.error->code, ErrorCode::RUN_STOPPED);
    EXPECT_EQ(workflow->state(), WorkflowState::stopped);
    const StatusSnapshot status = workflow->status();
    EXPECT_EQ(status.health, Health::degraded);
    EXPECT_EQ(step_status(status, "b").cancelled, 1u);
    EXPECT_EQ(step_status(status, "b").state, StepState::destroyed);
    const json record = json::parse(read_file(dir.path() / "slow" / "run-record.json"));
    EXPECT_EQ(record.at("execution").at("status"), "stopped");
    EXPECT_EQ(record.at("execution").at("error").at("name"), "RUN_STOPPED");
    workflow->close();

    // A stop requested between ticks ends the run at the next one, which calls no module.
    const std::unique_ptr<Workflow> between = runtime.open(chain("quick", 10));
    between->start();
    EXPECT_FALSE(between->tick().finished);
    between->request_stop();
    const RunSummary stopped = between->run();
    EXPECT_EQ(stopped.final_state, WorkflowState::stopped);
    EXPECT_EQ(stopped.ticks_completed, 1u);
    ASSERT_TRUE(stopped.error.has_value());
    EXPECT_EQ(stopped.error->code, ErrorCode::RUN_STOPPED);
    EXPECT_EQ(step_status(between->status(), "b").invocations, 1u);
}

namespace {

// The example's configuration and data in `dir`, with the input bound through a target descriptor
// (meta/target.json) instead of a path; returns the configuration's file.
fs::path example_with_descriptor(const fs::path& dir) {
    std::string config = read_file(example_dir() / "workflow.yaml");
    const std::string plain = "    path: data/observations.csv";
    const auto at = config.find(plain);
    if (at == std::string::npos) {
        throw std::runtime_error("the example's input line changed");
    }
    config.replace(at, plain.size(), "    descriptor: meta/target.json");
    write_text_file(dir / "workflow.yaml", config);
    write_text_file(dir / "data" / "observations.csv",
                    read_file(example_dir() / "data" / "observations.csv"));
    const json descriptor = json::parse(R"({
      "schema_version": "0.0.0", "id": "example-observations", "name": "Example observations",
      "data": {"id": "observations", "href": "../data/observations.csv", "media_type": "text/csv"},
      "dialect": {"delimiter": ",", "quotechar": "\"", "missing_token": "NA"},
      "columns": {"observation_id": "observation_id", "series_id": "series_id",
                  "variable_id": "variable_id", "time": "time", "value": "value", "unit": "unit",
                  "missing": "missing", "censoring": "censoring", "upper_limit": "upper_limit"},
      "time_unit": "d",
      "variables": [{"id": "population", "name": "Population", "unit": "count",
                     "value_type": "scalar", "lower_bound": 0}],
      "x-circle-outcomes": {"variable_ids": []}
    })");
    write_text_file(dir / "meta" / "target.json", descriptor.dump(2));
    return dir / "workflow.yaml";
}

} // namespace

// The same data through a descriptor: the run is the example's run (same outputs apart from the
// fingerprint in their ids), the run record says which descriptor was read, and two runs are
// byte-identical.
TEST(Example, ADescriptorBindsTheDataAndTheRunIsTheSame) {
    ScratchDir dir;
    const Runtime runtime(example_options());
    const fs::path config = example_with_descriptor(dir.path());
    const ExampleRun plain = run_example(runtime, dir.path() / "plain");
    const ExampleRun first = run_example(runtime, dir.path() / "first", {}, utf8(config));
    const ExampleRun second = run_example(runtime, dir.path() / "second", {}, utf8(config));
    ASSERT_EQ(first.summary.final_state, WorkflowState::completed);
    ASSERT_EQ(second.summary.final_state, WorkflowState::completed);
    EXPECT_EQ(first.summary.ticks_completed, kTicks);
    EXPECT_EQ(first.fingerprint, second.fingerprint);
    EXPECT_NE(first.fingerprint, plain.fingerprint); // the descriptor is part of the run
    for (const char* file : {"outputs/predictions.jsonl", "outputs/state.jsonl"}) {
        SCOPED_TRACE(file);
        EXPECT_EQ(read_file(first.out / file), read_file(second.out / file));
        const auto with_descriptor = json_lines(first.out / file);
        const auto without = json_lines(plain.out / file);
        ASSERT_EQ(with_descriptor.size(), without.size());
        for (std::size_t i = 0; i < without.size(); ++i) {
            EXPECT_EQ(without_fingerprint(with_descriptor[i]), without_fingerprint(without[i]));
        }
    }

    const json record = json::parse(read_file(first.out / "run-record.json"));
    EXPECT_TRUE(runtime.validate_document(utf8(first.out / "run-record.json")).ok());
    const json& input = record.at("reproducibility").at("inputs").at("observations");
    EXPECT_EQ(input.at("source"), "observation-csv");
    EXPECT_EQ(input.at("descriptor").at("schema_version"), "0.0.0");
    EXPECT_EQ(input.at("descriptor").at("digest").get<std::string>().size(), 64u);
    EXPECT_EQ(input.at("digest"),
              record.at("reproducibility").at("inputs").at("observations").at("digest"));
    EXPECT_EQ(input.at("digest"), json::parse(read_file(plain.out / "run-record.json"))
                                      .at("reproducibility")
                                      .at("inputs")
                                      .at("observations")
                                      .at("digest"));
    const json& counts = record.at("execution").at("inputs").at("observations");
    EXPECT_EQ(counts.at("rows_total"), 30);
    EXPECT_EQ(counts.at("rows_before_start"), 0);
    EXPECT_EQ(counts.at("rows_not_delivered_by_role"), 0);
    EXPECT_EQ(counts.at("descriptor_id"), "example-observations");
    // Neither the descriptor's nor the data's directory is in the record.
    expect_names_none(record, directory_forms({utf8_path(PRAENORA_EXAMPLE_MODULES_DIR),
                                               dir.path()}));
}

// A descriptor whose row checks fail is refused when the workflow is opened, with its location.
TEST(Example, ADescriptorThatDisagreesWithTheDataRefusesTheWorkflow) {
    ScratchDir dir;
    const Runtime runtime(example_options());
    const fs::path config = example_with_descriptor(dir.path());
    json descriptor = json::parse(read_file(dir.path() / "meta" / "target.json"));
    descriptor["variables"][0]["unit"] = "kg"; // the CSV says count
    write_text_file(dir.path() / "meta" / "target.json", descriptor.dump(2));
    const ValidationReport report = runtime.validate(utf8(config));
    ASSERT_FALSE(report.ok()) << report.to_text();
    ASSERT_EQ(report.diagnostics.size(), 1u) << report.to_text();
    const auto& error = report.diagnostics.front();
    EXPECT_EQ(error.code, ErrorCode::TARGET_DATA_INVALID) << report.to_text();
    EXPECT_EQ(error.location.line, 2) << report.to_text();
    EXPECT_TRUE(contains(error.message, "unit 'count' differs from the unit 'kg'"))
        << error.message;
    // Opening the workflow refuses it the same way.
    const Error thrown = error_of([&] { (void)runtime.open(utf8(config), {}); });
    EXPECT_EQ(thrown.code(), ErrorCode::TARGET_DATA_INVALID) << thrown.what();
    EXPECT_EQ(thrown.location().line, 2);
}
