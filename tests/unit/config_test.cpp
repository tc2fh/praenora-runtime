// Workflow configuration loading: the YAML document, its schema, defaults, per-step timeouts,
// paths relative to the configuration file and the caller's overrides.
#include "config/workflow_config.hpp"

#include "core/paths.hpp"
#include "core/semver.hpp"

#include "module_fixtures.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using nlohmann::json;
using praenora::ErrorCode;
using praenora::IsolationMode;
using praenora::LogFormat;
using praenora::LogLevel;
using praenora::Severity;
using praenora::ValidationReport;
using praenora::WorkflowOptions;
using praenora::detail::load_workflow_config;
using praenora::detail::path_to_utf8;
using praenora::detail::read_workflow_config;
using praenora::detail::Timeouts;
using praenora::detail::WorkflowConfig;
using praenora::test::ScratchDir;
using praenora::test::utf8_path;
using praenora::test::write_text_file;

namespace {

void expect_timeouts(const Timeouts& actual, std::chrono::nanoseconds invoke,
                     std::chrono::nanoseconds lifecycle, std::chrono::nanoseconds cancel_grace) {
    EXPECT_EQ(actual.invoke.count(), invoke.count());
    EXPECT_EQ(actual.lifecycle.count(), lifecycle.count());
    EXPECT_EQ(actual.cancel_grace.count(), cancel_grace.count());
}

// The configuration `yaml`, written to `<dir>/workflow.yaml` and loaded.
WorkflowConfig load_text(const fs::path& dir, const std::string& yaml, ValidationReport& report,
                         const WorkflowOptions& overrides = {}) {
    const fs::path file = dir / "workflow.yaml";
    write_text_file(file, yaml);
    const auto config = read_workflow_config(file, report, overrides);
    EXPECT_TRUE(config.has_value()) << report.to_text();
    return config ? *config : WorkflowConfig{};
}

const char* const kMinimal = R"(kind: workflow-config
format_version: "0.1.0"
workflow: { name: test.single, version: "0.1.0" }
inputs:
  source: { source: observation-csv, path: data.csv }
steps:
  a: { module: { name: fx-echo, version: "0.1.0" } }
)";

} // namespace

TEST(Config, ParsesExampleConfig) {
    const fs::path file = praenora::test::fixture_path("configs/good/example.yaml");
    ValidationReport report;
    const WorkflowConfig config = load_workflow_config(file, report);
    ASSERT_TRUE(report.ok()) << report.to_text();
    EXPECT_TRUE(report.diagnostics.empty()) << report.to_text();

    const fs::path dir = fs::absolute(file).lexically_normal().parent_path();
    EXPECT_EQ(config.file, fs::absolute(file).lexically_normal());
    EXPECT_EQ(config.workflow_name, "example.estimate-predict");
    EXPECT_EQ(praenora::detail::to_string(config.workflow_version), "0.1.0");
    EXPECT_EQ(config.seed, 20261110u);
    ASSERT_TRUE(config.ticks.has_value());
    EXPECT_EQ(*config.ticks, 12u);
    EXPECT_EQ(config.time.unit, "d");
    EXPECT_EQ(config.time.start, 0.0);
    EXPECT_EQ(config.time.period, 1.0);
    expect_timeouts(config.timeouts, 30s, 10s, 2s);
    EXPECT_TRUE(config.module_search_paths.empty());

    ASSERT_EQ(config.inputs.size(), 1u);
    const auto& input = config.inputs.at("observations");
    EXPECT_EQ(input.input, "observations");
    EXPECT_EQ(input.source, "observation-csv");
    EXPECT_EQ(input.path, dir / "data" / "observations.csv");
    EXPECT_TRUE(input.columns.empty());

    ASSERT_EQ(config.steps.size(), 2u);
    const auto& estimate = config.steps.at("estimate");
    EXPECT_EQ(estimate.step, "estimate");
    EXPECT_EQ(estimate.module_name, "example-estimator");
    EXPECT_EQ(praenora::detail::to_string(estimate.module_version), "0.1.0");
    EXPECT_EQ(estimate.parameters, json::parse(R"({"gain":0.6})"));
    expect_timeouts(estimate.timeouts, 30s, 10s, 2s);
    EXPECT_FALSE(estimate.log_level.has_value());
    const auto& predict = config.steps.at("predict");
    EXPECT_EQ(predict.module_name, "example-predictor");
    EXPECT_EQ(predict.parameters, json::parse(R"({"growth_rate":0.35,"capacity":100.0,"horizon":4.0,
                              "step_size":0.05,"output_interval":0.5})"));
    expect_timeouts(predict.timeouts, 5s, 10s, 2s);
    ASSERT_TRUE(predict.log_level.has_value());
    EXPECT_EQ(*predict.log_level, LogLevel::debug);

    EXPECT_FALSE(config.output_dir.has_value());
    EXPECT_EQ(config.record, (std::vector<std::string>{"predictions", "state"}));
    EXPECT_FALSE(config.trace_messages);
    EXPECT_EQ(config.logging.level, LogLevel::info);
    EXPECT_EQ(config.logging.format, LogFormat::text);
    EXPECT_TRUE(config.logging.file.empty());
    EXPECT_TRUE(config.logging.to_stderr);
    EXPECT_FALSE(config.status_file.has_value());
    EXPECT_EQ(config.status_interval.count(), std::chrono::nanoseconds(1s).count());

    EXPECT_EQ(config.document.json.at("kind"), "workflow-config");
    EXPECT_EQ(config.document.positions.at("/steps/predict/log_level").line, 33);
}

TEST(Config, DefaultsApplied) {
    ScratchDir scratch;
    ValidationReport report;
    const WorkflowConfig config = load_text(scratch.path(), kMinimal, report);
    ASSERT_TRUE(report.ok()) << report.to_text();
    EXPECT_TRUE(report.diagnostics.empty()) << report.to_text();

    EXPECT_EQ(config.seed, 0u);
    EXPECT_FALSE(config.ticks.has_value());
    EXPECT_EQ(config.time.unit, "1");
    EXPECT_EQ(config.time.start, 0.0);
    EXPECT_EQ(config.time.period, 1.0);
    expect_timeouts(config.timeouts, 300s, 60s, 5s);
    EXPECT_TRUE(config.module_search_paths.empty());
    ASSERT_EQ(config.steps.size(), 1u);
    const auto& step = config.steps.at("a");
    EXPECT_EQ(step.parameters, json::object());
    expect_timeouts(step.timeouts, 300s, 60s, 5s);
    EXPECT_FALSE(step.log_level.has_value());
    EXPECT_TRUE(config.inputs.at("source").columns.empty());
    EXPECT_FALSE(config.output_dir.has_value());
    EXPECT_TRUE(config.record.empty()); // every workflow output, once validated
    EXPECT_FALSE(config.trace_messages);
    EXPECT_EQ(config.logging.level, LogLevel::info);
    EXPECT_EQ(config.logging.format, LogFormat::text);
    EXPECT_TRUE(config.logging.file.empty());
    EXPECT_TRUE(config.logging.to_stderr);
    EXPECT_FALSE(config.status_file.has_value());
    EXPECT_EQ(config.status_interval.count(), std::chrono::nanoseconds(1s).count());
}

TEST(Config, StepTimeoutOverridesFieldwise) {
    ScratchDir scratch;
    ValidationReport report;
    const WorkflowConfig config = load_text(scratch.path(), R"(kind: workflow-config
format_version: "0.1.0"
workflow: { name: test.chain, version: "0.1.0" }
run:
  ticks: 2
  timeouts: { invoke: 30s, cancel_grace: 250ms }
inputs:
  source: { source: json-document, path: source.json }
steps:
  a:
    module: { name: fx-echo, version: "0.1.0" }
    timeouts: { invoke: 5s }
  b:
    module: { name: fx-echo, version: "0.1.0" }
    timeouts: { lifecycle: none, cancel_grace: 0 }
  c:
    module: { name: fx-echo, version: "0.1.0" }
    timeouts: { invoke: 1.5m, lifecycle: 2h }
  d: { module: { name: fx-echo, version: "0.1.0" } }
)",
                                            report);
    ASSERT_TRUE(report.ok()) << report.to_text();
    expect_timeouts(config.timeouts, 30s, 60s, 250ms);
    expect_timeouts(config.steps.at("a").timeouts, 5s, 60s, 250ms);
    expect_timeouts(config.steps.at("b").timeouts, 30s, 0s, 0s);
    expect_timeouts(config.steps.at("c").timeouts, 90s, 2h, 250ms);
    expect_timeouts(config.steps.at("d").timeouts, 30s, 60s, 250ms);
}

TEST(Config, RelativePathsResolveAgainstFile) {
    ScratchDir scratch;
    const fs::path dir = scratch.path() / utf8_path("config d\xC3\xADr");
    const fs::path elsewhere = scratch.path() / "elsewhere";
    ValidationReport report;
    const WorkflowConfig config = load_text(dir,
                                            R"(kind: workflow-config
format_version: "0.1.0"
workflow: { name: test.single, version: "0.1.0" }
modules:
  search_paths: [mods, "../shared mods", ')" + path_to_utf8(elsewhere) +
                                                R"(']
inputs:
  source: { source: observation-csv, path: data/obs.csv }
steps:
  a: { module: { name: fx-echo, version: "0.1.0" } }
outputs:
  directory: out
observability:
  log_file: logs/run.log
  status_file: ../status/status.json
)",
                                            report);
    ASSERT_TRUE(report.ok()) << report.to_text();
    EXPECT_EQ(config.file, dir / "workflow.yaml");
    EXPECT_EQ(config.module_search_paths,
              (std::vector<fs::path>{dir / "mods", scratch.path() / "shared mods", elsewhere}));
    EXPECT_EQ(config.inputs.at("source").path, dir / "data" / "obs.csv");
    ASSERT_TRUE(config.output_dir.has_value());
    EXPECT_EQ(*config.output_dir, dir / "out");
    EXPECT_EQ(config.logging.file, path_to_utf8(dir / "logs" / "run.log"));
    ASSERT_TRUE(config.status_file.has_value());
    EXPECT_EQ(*config.status_file, scratch.path() / "status" / "status.json");
}

TEST(Config, OverridesTakePrecedence) {
    ScratchDir scratch;
    const std::string yaml = R"(kind: workflow-config
format_version: "0.1.0"
workflow: { name: test.single, version: "0.1.0" }
run: { seed: 1 }
inputs:
  source: { source: json-document, path: source.json }
steps:
  a: { module: { name: fx-echo, version: "0.1.0" } }
outputs: { directory: out, trace_messages: false }
observability: { status_file: status.json }
)";
    // Without ticks and without a replay input the run would never end.
    {
        ValidationReport report;
        (void)load_text(scratch.path(), yaml, report);
        ASSERT_EQ(report.diagnostics.size(), 1u) << report.to_text();
        EXPECT_EQ(report.diagnostics[0].code, ErrorCode::CONFIG_INVALID);
        EXPECT_EQ(report.diagnostics[0].location.pointer, "/run/ticks");
    }
    // The caller's values win; their relative paths are relative to the current directory.
    WorkflowOptions overrides;
    overrides.seed = 5;
    overrides.ticks = 7;
    overrides.output_dir = "results/run 1";
    overrides.trace_messages = true;
    overrides.status_file = "st.json";
    ValidationReport report;
    const WorkflowConfig config = load_text(scratch.path(), yaml, report, overrides);
    ASSERT_TRUE(report.ok()) << report.to_text();
    EXPECT_EQ(config.seed, 5u);
    ASSERT_TRUE(config.ticks.has_value());
    EXPECT_EQ(*config.ticks, 7u);
    const fs::path cwd = fs::current_path();
    ASSERT_TRUE(config.output_dir.has_value());
    EXPECT_EQ(*config.output_dir, (cwd / "results" / "run 1").lexically_normal());
    EXPECT_TRUE(config.trace_messages);
    ASSERT_TRUE(config.status_file.has_value());
    EXPECT_EQ(*config.status_file, (cwd / "st.json").lexically_normal());

    // A seed beyond 2^53 - 1 is refused.
    WorkflowOptions too_large;
    too_large.seed = 9007199254740992ULL;
    too_large.ticks = 1;
    ValidationReport refused;
    (void)load_text(scratch.path(), yaml, refused, too_large);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.diagnostics.at(0).code, ErrorCode::INVALID_ARGUMENT) << refused.to_text();
}

namespace {

// The configuration of test.chain with `run` and four steps: a sets no isolation, b sets mode
// in-process, c sets mode process, d an empty isolation object.
std::string isolation_config(const std::string& run) {
    return R"(kind: workflow-config
format_version: "0.1.0"
workflow: { name: test.chain, version: "0.1.0" }
)" + run + R"(steps:
  a: { module: { name: fx-echo, version: "0.1.0" } }
  b:
    module: { name: fx-echo, version: "0.1.0" }
    isolation: { mode: in-process }
  c:
    module: { name: fx-echo, version: "0.1.0" }
    isolation: { mode: process }
  d:
    module: { name: fx-echo, version: "0.1.0" }
    isolation: {}
)";
}

void expect_isolation(const WorkflowConfig& config, const std::string& step, IsolationMode mode,
                      const std::string& pointer, bool overridden = false) {
    SCOPED_TRACE("step " + step);
    const praenora::detail::Isolation& isolation = config.steps.at(step).isolation;
    EXPECT_EQ(isolation.mode, mode);
    EXPECT_EQ(isolation.mode_pointer, pointer);
    EXPECT_EQ(isolation.mode_overridden, overridden);
}

} // namespace

TEST(Config, IsolationResolvesFieldByField) {
    ScratchDir scratch;
    // Without run.isolation, a step that sets no mode gets the default, in-process.
    {
        ValidationReport report;
        const WorkflowConfig config =
            load_text(scratch.path() / "default", isolation_config("run: { ticks: 1 }\n"), report);
        ASSERT_TRUE(report.ok()) << report.to_text();
        expect_isolation(config, "a", IsolationMode::in_process, "");
        expect_isolation(config, "b", IsolationMode::in_process, "/steps/b/isolation/mode");
        expect_isolation(config, "c", IsolationMode::process, "/steps/c/isolation/mode");
        expect_isolation(config, "d", IsolationMode::in_process, "");
        EXPECT_FALSE(config.overrides.isolation.has_value());
    }
    // run.isolation is every step's default; a step's own field wins.
    {
        ValidationReport report;
        const WorkflowConfig config = load_text(
            scratch.path() / "run",
            isolation_config("run: { ticks: 1, isolation: { mode: process } }\n"), report);
        ASSERT_TRUE(report.ok()) << report.to_text();
        expect_isolation(config, "a", IsolationMode::process, "/run/isolation/mode");
        expect_isolation(config, "b", IsolationMode::in_process, "/steps/b/isolation/mode");
        expect_isolation(config, "c", IsolationMode::process, "/steps/c/isolation/mode");
        expect_isolation(config, "d", IsolationMode::process, "/run/isolation/mode");
    }
    // An empty run.isolation sets nothing.
    {
        ValidationReport report;
        const WorkflowConfig config =
            load_text(scratch.path() / "empty",
                      isolation_config("run: { ticks: 1, isolation: {} }\n"), report);
        ASSERT_TRUE(report.ok()) << report.to_text();
        expect_isolation(config, "a", IsolationMode::in_process, "");
        expect_isolation(config, "c", IsolationMode::process, "/steps/c/isolation/mode");
    }
    // An unknown mode or field is refused by the schema, at the field.
    {
        ValidationReport report;
        const fs::path file = scratch.path() / "bad" / "workflow.yaml";
        write_text_file(file, isolation_config("run: { ticks: 1, isolation: { mode: thread, "
                                               "priority: 1 } }\n"));
        EXPECT_FALSE(read_workflow_config(file, report).has_value());
        std::vector<std::string> pointers;
        for (const auto& diagnostic : report.diagnostics) {
            EXPECT_EQ(diagnostic.code, ErrorCode::SCHEMA_VIOLATION) << diagnostic.message;
            pointers.push_back(diagnostic.location.pointer);
        }
        std::sort(pointers.begin(), pointers.end());
        EXPECT_EQ(pointers,
                  (std::vector<std::string>{"/run/isolation/mode", "/run/isolation/priority"}))
            << report.to_text();
    }
}

TEST(Config, IsolationOverrideSetsEveryStep) {
    ScratchDir scratch;
    const std::string yaml = isolation_config("run: { ticks: 1, isolation: { mode: process } }\n");
    for (const IsolationMode mode : {IsolationMode::in_process, IsolationMode::process}) {
        WorkflowOptions overrides;
        overrides.isolation = mode;
        ValidationReport report;
        const WorkflowConfig config = load_text(scratch.path(), yaml, report, overrides);
        ASSERT_TRUE(report.ok()) << report.to_text();
        for (const char* step : {"a", "b", "c", "d"}) {
            expect_isolation(config, step, mode, "", true);
        }
        ASSERT_TRUE(config.overrides.isolation.has_value());
        EXPECT_EQ(*config.overrides.isolation, mode);
    }
}

// share_compiled_models resolves field by field, as the mode does, apart from it: the step's
// field, else the run's, else false; the pointer names the field that set it.
TEST(Config, SharingCompiledModelsResolvesFieldByField) {
    ScratchDir scratch;
    const std::string yaml = R"(kind: workflow-config
format_version: "0.1.0"
workflow: { name: test.chain, version: "0.1.0" }
run: { ticks: 1, isolation: { share_compiled_models: true } }
steps:
  a: { module: { name: fx-echo, version: "0.1.0" } }
  b:
    module: { name: fx-echo, version: "0.1.0" }
    isolation: { mode: process, share_compiled_models: false }
  c:
    module: { name: fx-echo, version: "0.1.0" }
    isolation: { mode: process }
)";
    ValidationReport report;
    const WorkflowConfig config = load_text(scratch.path() / "run", yaml, report);
    ASSERT_TRUE(report.ok()) << report.to_text();
    const auto expect_sharing = [&config](const char* step, bool share,
                                          const std::string& pointer) {
        SCOPED_TRACE(std::string("step ") + step);
        const praenora::detail::Isolation& isolation = config.steps.at(step).isolation;
        EXPECT_EQ(isolation.share_compiled_models, share);
        EXPECT_EQ(isolation.share_pointer, pointer);
    };
    expect_sharing("a", true, "/run/isolation/share_compiled_models");
    expect_sharing("b", false, "/steps/b/isolation/share_compiled_models");
    expect_sharing("c", true, "/run/isolation/share_compiled_models");
    EXPECT_TRUE(config.isolation.share_compiled_models);

    // Without the field anywhere, no step shares.
    ValidationReport plain_report;
    const WorkflowConfig plain =
        load_text(scratch.path() / "plain", isolation_config("run: { ticks: 1 }\n"), plain_report);
    ASSERT_TRUE(plain_report.ok()) << plain_report.to_text();
    for (const char* step : {"a", "b", "c", "d"}) {
        EXPECT_FALSE(plain.steps.at(step).isolation.share_compiled_models) << step;
        EXPECT_EQ(plain.steps.at(step).isolation.share_pointer, "") << step;
    }

    // It is a boolean.
    ValidationReport bad;
    const fs::path file = scratch.path() / "bad" / "workflow.yaml";
    write_text_file(file, isolation_config("run: { ticks: 1, isolation: { mode: process, "
                                           "share_compiled_models: \"yes\" } }\n"));
    EXPECT_FALSE(read_workflow_config(file, bad).has_value());
    ASSERT_EQ(bad.diagnostics.size(), 1u) << bad.to_text();
    EXPECT_EQ(bad.diagnostics.front().code, ErrorCode::SCHEMA_VIOLATION);
    EXPECT_EQ(bad.diagnostics.front().location.pointer, "/run/isolation/share_compiled_models");
}

// The override in-process resets every step's sharing (it has no module host to share with); the
// override process keeps what the file says.
TEST(Config, IsolationOverrideInProcessStopsSharing) {
    ScratchDir scratch;
    const std::string yaml = isolation_config(
        "run: { ticks: 1, isolation: { mode: process, share_compiled_models: true } }\n");
    {
        WorkflowOptions overrides;
        overrides.isolation = IsolationMode::in_process;
        ValidationReport report;
        const WorkflowConfig config = load_text(scratch.path() / "in", yaml, report, overrides);
        ASSERT_TRUE(report.ok()) << report.to_text();
        EXPECT_FALSE(config.isolation.share_compiled_models);
        for (const char* step : {"a", "b", "c", "d"}) {
            EXPECT_FALSE(config.steps.at(step).isolation.share_compiled_models) << step;
            EXPECT_EQ(config.steps.at(step).isolation.share_pointer, "") << step;
        }
    }
    {
        WorkflowOptions overrides;
        overrides.isolation = IsolationMode::process;
        ValidationReport report;
        const WorkflowConfig config = load_text(scratch.path() / "out", yaml, report, overrides);
        ASSERT_TRUE(report.ok()) << report.to_text();
        for (const char* step : {"a", "b", "c", "d"}) {
            SCOPED_TRACE(step);
            const praenora::detail::Isolation& isolation = config.steps.at(step).isolation;
            EXPECT_EQ(isolation.mode, IsolationMode::process);
            EXPECT_TRUE(isolation.share_compiled_models);
            EXPECT_EQ(isolation.share_pointer, "/run/isolation/share_compiled_models");
        }
    }
}

// sandbox resolves field by field, as the other fields do: the step's field, else the run's, else
// none; the pointer names the field that set it. Only the names of the profiles are taken.
TEST(Config, SandboxResolvesFieldByField) {
    ScratchDir scratch;
    const std::string yaml = R"(kind: workflow-config
format_version: "0.1.0"
workflow: { name: test.chain, version: "0.1.0" }
run: { ticks: 1, isolation: { mode: process, sandbox: restricted } }
steps:
  a: { module: { name: fx-echo, version: "0.1.0" } }
  b:
    module: { name: fx-echo, version: "0.1.0" }
    isolation: { sandbox: none }
)";
    ValidationReport report;
    const WorkflowConfig config = load_text(scratch.path() / "run", yaml, report);
    ASSERT_TRUE(report.ok()) << report.to_text();
    using praenora::detail::SandboxProfile;
    EXPECT_EQ(config.isolation.sandbox, SandboxProfile::restricted);
    EXPECT_EQ(config.steps.at("a").isolation.sandbox, SandboxProfile::restricted);
    EXPECT_EQ(config.steps.at("a").isolation.sandbox_pointer, "/run/isolation/sandbox");
    EXPECT_EQ(config.steps.at("b").isolation.sandbox, SandboxProfile::none);
    EXPECT_EQ(config.steps.at("b").isolation.sandbox_pointer, "/steps/b/isolation/sandbox");

    // Without the field anywhere, no step has a profile.
    ValidationReport plain_report;
    const WorkflowConfig plain =
        load_text(scratch.path() / "plain", isolation_config("run: { ticks: 1 }\n"), plain_report);
    ASSERT_TRUE(plain_report.ok()) << plain_report.to_text();
    for (const char* step : {"a", "b", "c", "d"}) {
        EXPECT_EQ(plain.steps.at(step).isolation.sandbox, SandboxProfile::none) << step;
        EXPECT_EQ(plain.steps.at(step).isolation.sandbox_pointer, "") << step;
    }

    // Another name is refused by the schema, at the field.
    ValidationReport bad;
    const fs::path file = scratch.path() / "bad" / "workflow.yaml";
    write_text_file(file, isolation_config("run: { ticks: 1, isolation: { mode: process, "
                                           "sandbox: Restricted } }\n"));
    EXPECT_FALSE(read_workflow_config(file, bad).has_value());
    ASSERT_EQ(bad.diagnostics.size(), 1u) << bad.to_text();
    EXPECT_EQ(bad.diagnostics.front().code, ErrorCode::SCHEMA_VIOLATION);
    EXPECT_EQ(bad.diagnostics.front().location.pointer, "/run/isolation/sandbox");
}

// The override in-process sets every step's sandbox to none (a step in the runtime's process has
// no module host to sandbox); the override process keeps what the file says.
TEST(Config, IsolationOverrideInProcessDropsTheSandbox) {
    ScratchDir scratch;
    const std::string yaml =
        isolation_config("run: { ticks: 1, isolation: { mode: process, sandbox: restricted } }\n");
    using praenora::detail::SandboxProfile;
    for (const IsolationMode mode : {IsolationMode::in_process, IsolationMode::process}) {
        SCOPED_TRACE(mode == IsolationMode::process ? "process" : "in-process");
        WorkflowOptions overrides;
        overrides.isolation = mode;
        ValidationReport report;
        const WorkflowConfig config =
            load_text(scratch.path() / (mode == IsolationMode::process ? "out" : "in"), yaml,
                      report, overrides);
        ASSERT_TRUE(report.ok()) << report.to_text();
        const bool kept = mode == IsolationMode::process;
        EXPECT_EQ(config.isolation.sandbox,
                  kept ? SandboxProfile::restricted : SandboxProfile::none);
        for (const char* step : {"a", "b", "c", "d"}) {
            SCOPED_TRACE(step);
            const praenora::detail::Isolation& isolation = config.steps.at(step).isolation;
            EXPECT_EQ(isolation.sandbox, kept ? SandboxProfile::restricted : SandboxProfile::none);
            EXPECT_EQ(isolation.sandbox_pointer, kept ? "/run/isolation/sandbox" : "");
        }
    }
}

TEST(Config, IsolationModeNames) {
    using praenora::detail::isolation_mode_from_name;
    using praenora::detail::isolation_mode_name;
    EXPECT_EQ(isolation_mode_name(IsolationMode::in_process), "in-process");
    EXPECT_EQ(isolation_mode_name(IsolationMode::process), "process");
    EXPECT_EQ(isolation_mode_from_name("in-process"), IsolationMode::in_process);
    EXPECT_EQ(isolation_mode_from_name("process"), IsolationMode::process);
    EXPECT_FALSE(isolation_mode_from_name("Process").has_value());
    EXPECT_FALSE(isolation_mode_from_name("in_process").has_value());
    EXPECT_FALSE(isolation_mode_from_name("").has_value());
}

TEST(Config, UnusableDocumentGivesNoConfiguration) {
    ScratchDir scratch;
    const fs::path file = scratch.path() / "workflow.yaml";
    write_text_file(file, "kind: workflow-config\nformat_version: \"0.1.0\"\nsteps: []\n");
    ValidationReport report;
    EXPECT_FALSE(read_workflow_config(file, report).has_value());
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.diagnostics.at(0).code, ErrorCode::SCHEMA_VIOLATION);
    EXPECT_EQ(report.diagnostics.at(0).severity, Severity::error);

    // load_workflow_config() still returns a configuration naming the file.
    ValidationReport again;
    const WorkflowConfig config = load_workflow_config(file, again);
    EXPECT_FALSE(again.ok());
    EXPECT_EQ(config.file, file);

    // A missing file is an I/O error located at the file.
    ValidationReport missing;
    EXPECT_FALSE(read_workflow_config(scratch.path() / "absent.yaml", missing).has_value());
    ASSERT_EQ(missing.diagnostics.size(), 1u);
    EXPECT_EQ(missing.diagnostics[0].code, ErrorCode::IO_ERROR);
}

TEST(Config, AnInputMayNameADescriptorInsteadOfAPath) {
    ScratchDir scratch;
    ValidationReport report;
    const WorkflowConfig config = load_text(scratch.path(),
                                            "kind: workflow-config\n"
                                            "format_version: \"0.1.0\"\n"
                                            "workflow: { name: test.single, version: \"0.1.0\" }\n"
                                            "inputs:\n"
                                            "  source: { source: observation-csv, "
                                            "descriptor: meta/target.json }\n"
                                            "steps:\n"
                                            "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n",
                                            report);
    ASSERT_TRUE(report.ok()) << report.to_text();
    const auto& input = config.inputs.at("source");
    // The descriptor resolves against the configuration's directory; the CSV is found later, from
    // the descriptor's data.href.
    EXPECT_EQ(input.descriptor, scratch.path() / "meta" / "target.json");
    EXPECT_TRUE(input.path.empty());
    EXPECT_TRUE(input.columns.empty());
}

TEST(Config, AnInputMayNameBothAPathAndADescriptor) {
    ScratchDir scratch;
    ValidationReport report;
    const WorkflowConfig config = load_text(scratch.path(),
                                            "kind: workflow-config\n"
                                            "format_version: \"0.1.0\"\n"
                                            "workflow: { name: test.single, version: \"0.1.0\" }\n"
                                            "inputs:\n"
                                            "  source: { source: observation-csv, path: data.csv, "
                                            "descriptor: target.json }\n"
                                            "steps:\n"
                                            "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n",
                                            report);
    ASSERT_TRUE(report.ok()) << report.to_text();
    EXPECT_EQ(config.inputs.at("source").path, scratch.path() / "data.csv");
    EXPECT_EQ(config.inputs.at("source").descriptor, scratch.path() / "target.json");
}

TEST(Config, AnInputWithoutADescriptorHasNone) {
    ScratchDir scratch;
    ValidationReport report;
    const WorkflowConfig config = load_text(scratch.path(), kMinimal, report);
    ASSERT_TRUE(report.ok()) << report.to_text();
    EXPECT_TRUE(config.inputs.at("source").descriptor.empty());
}
