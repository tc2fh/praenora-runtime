// The run fingerprint: the SHA-256 of the canonical reproducibility section, which covers what
// determines a run's results (versions, catalog entries, the semantic configuration, modules,
// the runtime services of providers and their engines, input data and seeds) and nothing else
// (output and observability settings, paths).
#include "api/validation.hpp"
#include "core/paths.hpp"
#include "core/seed.hpp"
#include "core/semver.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"
#include "provider/provider_manifest.hpp"

#include "module_fixtures.hpp"
#include "provider_fixtures.hpp"
#include "scoped_environment.hpp"
#include "test_support.hpp"
#include "validation_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/version.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::detail::path_to_utf8;
using praenora::test::ScratchDir;
using praenora::test::validate_text;
using praenora::test::write_text_file;

namespace {

// A valid configuration of test.chain over the observations CSV `obs.csv` beside it. `run` and
// `extra` are inserted as given; `b_parameters` are the parameters of step b (fx-slow).
std::string chain_config(const std::string& run = "run: { seed: 7 }\n",
                         const std::string& b_parameters = "{}", const std::string& extra = "") {
    return "kind: workflow-config\n"
           "format_version: \"0.1.0\"\n"
           "workflow: { name: test.chain, version: \"0.1.0\" }\n" +
           run +
           "inputs:\n"
           "  source: { source: observation-csv, path: obs.csv }\n"
           "steps:\n"
           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"
           "  b:\n"
           "    module: { name: fx-slow, version: \"0.1.0\" }\n"
           "    parameters: " +
           b_parameters + "\n" + extra;
}

// Validates `yaml` in a fresh directory holding the observations CSV `csv`.
praenora::detail::ValidatedWorkflow
validated(const std::string& yaml, const praenora::WorkflowOptions& options = {},
          const std::string& csv = praenora::test::observations_csv()) {
    ScratchDir dir;
    write_text_file(dir.path() / "obs.csv", csv);
    auto result = validate_text(dir.path(), yaml, options);
    if (!result.workflow) {
        ADD_FAILURE() << result.report.to_text();
        throw std::runtime_error("validation failed");
    }
    return std::move(*result.workflow);
}

} // namespace

TEST(Fingerprint, StableAcrossLoads) {
    const auto first = validated(chain_config());
    const auto second = validated(chain_config()); // another directory, same content
    EXPECT_EQ(first.fingerprint, second.fingerprint);
    EXPECT_EQ(first.reproducibility, second.reproducibility);
    const std::string canonical = praenora::detail::canonical_json(first.reproducibility);
    EXPECT_EQ(first.fingerprint, praenora::detail::sha256_hex(canonical));
    // Paths are not part of it.
    EXPECT_EQ(first.reproducibility.dump().find("obs.csv"), std::string::npos);
}

TEST(Fingerprint, ChangesWithSeed) {
    const auto seven = validated(chain_config("run: { seed: 7 }\n"));
    const auto eight = validated(chain_config("run: { seed: 8 }\n"));
    EXPECT_NE(seven.fingerprint, eight.fingerprint);
    // The caller's seed takes the place of the file's.
    praenora::WorkflowOptions options;
    options.seed = 8;
    EXPECT_EQ(validated(chain_config("run: { seed: 7 }\n"), options).fingerprint,
              eight.fingerprint);
    EXPECT_EQ(eight.reproducibility.at("seed"), 8);
    EXPECT_EQ(eight.reproducibility.at("steps").at("b").at("seed"),
              praenora::detail::derive_step_seed(8, "b"));
}

TEST(Fingerprint, IgnoresObservabilityAndOutputs) {
    const auto plain = validated(chain_config());
    const auto dressed = validated(
        chain_config("run: { seed: 7 }\n", "{}",
                     "    log_level: trace\n"
                     "outputs: { directory: out, record: [result], trace_messages: true }\n"
                     "observability:\n"
                     "  log_level: debug\n"
                     "  log_format: json\n"
                     "  log_file: logs/run.log\n"
                     "  status_file: status.json\n"
                     "  status_interval: 250ms\n"));
    EXPECT_EQ(plain.fingerprint, dressed.fingerprint);
    // The caller's output and status settings do not count either.
    praenora::WorkflowOptions options;
    options.output_dir = "elsewhere";
    options.status_file = "status.json";
    options.trace_messages = true;
    EXPECT_EQ(validated(chain_config(), options).fingerprint, plain.fingerprint);
}

// Process isolation is not available on Windows in this version: validation refuses a step in a
// module host there (docs/specs/process-isolation.md).
#ifndef _WIN32
TEST(Fingerprint, IgnoresIsolation) {
    // Results do not depend on where a module runs: the same configuration gives the same
    // reproducibility section, byte for byte, in either isolation mode, however the mode is set.
    const auto plain = validated(chain_config());
    const std::string canonical = praenora::detail::canonical_json(plain.reproducibility);
    const auto check = [&](const praenora::detail::ValidatedWorkflow& other) {
        EXPECT_EQ(praenora::detail::canonical_json(other.reproducibility), canonical);
        EXPECT_EQ(other.fingerprint, plain.fingerprint);
    };
    check(validated(chain_config("run: { seed: 7, isolation: { mode: process } }\n")));
    check(
        validated(chain_config("run: { seed: 7 }\n", "{}", "    isolation: { mode: process }\n")));
    check(validated(chain_config("run: { seed: 7, isolation: { mode: in-process } }\n")));
    check(validated(chain_config(
        "run: { seed: 7, isolation: { mode: process, share_compiled_models: true } }\n")));
#if defined(__APPLE__)
    // The restricted sandbox profile, where the platform has it.
    check(validated(
        chain_config("run: { seed: 7, isolation: { mode: process, sandbox: restricted } }\n")));
#endif
    for (const auto mode :
         {praenora::IsolationMode::process, praenora::IsolationMode::in_process}) {
        praenora::WorkflowOptions options;
        options.isolation = mode;
        check(validated(chain_config(), options));
    }
    // The steps' modes are in the run record's execution section instead.
    const auto process =
        validated(chain_config("run: { seed: 7, isolation: { mode: process } }\n"));
    EXPECT_EQ(process.isolation,
              json::parse(R"({"a":{"mode":"process","share_compiled_models":false,)"
                          R"("sandbox":"none"},)"
                          R"("b":{"mode":"process","share_compiled_models":false,)"
                          R"("sandbox":"none"}})"));
    EXPECT_EQ(plain.isolation,
              json::parse(R"({"a":{"mode":"in-process","share_compiled_models":false,)"
                          R"("sandbox":"none"},)"
                          R"("b":{"mode":"in-process","share_compiled_models":false,)"
                          R"("sandbox":"none"}})"));
}
#endif // _WIN32

TEST(Fingerprint, ChangesWithTicks) {
    // Without run.ticks the run lasts until the replay input is exhausted.
    const auto until_exhausted = validated(chain_config("run: { seed: 7 }\n"));
    const auto three = validated(chain_config("run: { seed: 7, ticks: 3 }\n"));
    const auto four = validated(chain_config("run: { seed: 7, ticks: 4 }\n"));
    EXPECT_NE(three.fingerprint, until_exhausted.fingerprint);
    EXPECT_NE(three.fingerprint, four.fingerprint);
    EXPECT_EQ(until_exhausted.reproducibility.at("ticks"), nullptr);
    EXPECT_EQ(three.reproducibility.at("ticks"), 3);
    // The caller's tick count takes the place of the file's.
    praenora::WorkflowOptions options;
    options.ticks = 3;
    EXPECT_EQ(validated(chain_config("run: { seed: 7, ticks: 4 }\n"), options).fingerprint,
              three.fingerprint);
    EXPECT_EQ(validated(chain_config("run: { seed: 7 }\n"), options).fingerprint,
              three.fingerprint);
}

TEST(Fingerprint, DefaultsMaterializedBeforeHashing) {
    // fx-slow's parameter defaults: sleep_ms 100, finalize_sleep_ms 0, cooperative true.
    const auto omitted = validated(chain_config("run: { seed: 7 }\n", "{}"));
    const auto explicit_defaults = validated(chain_config(
        "run: { seed: 7 }\n", "{ sleep_ms: 100, finalize_sleep_ms: 0, cooperative: true }"));
    EXPECT_EQ(omitted.fingerprint, explicit_defaults.fingerprint);
    EXPECT_EQ(omitted.reproducibility.at("steps").at("b").at("parameters"),
              json::parse(R"({"sleep_ms":100,"finalize_sleep_ms":0,"cooperative":true})"));
    // A value other than the default does change it.
    EXPECT_NE(validated(chain_config("run: { seed: 7 }\n", "{ sleep_ms: 101 }")).fingerprint,
              omitted.fingerprint);
}

TEST(Fingerprint, ChangesWithDataTimeBaseAndTimeouts) {
    const auto base = validated(chain_config());
    // One byte more in the data.
    EXPECT_NE(validated(chain_config(), {}, std::string(praenora::test::observations_csv()) + "\n")
                  .fingerprint,
              base.fingerprint);
    EXPECT_NE(validated(chain_config("run: { seed: 7, time: { period: 0.5 } }\n")).fingerprint,
              base.fingerprint);
    const auto ten = validated(chain_config("run: { seed: 7, timeouts: { invoke: 10s } }\n"));
    EXPECT_NE(ten.fingerprint, base.fingerprint);
    // The run's own timeouts are in the section, beside each step's effective ones.
    EXPECT_EQ(ten.reproducibility.at("timeouts"),
              json::parse(R"({"invoke":"10s","lifecycle":"60s","cancel_grace":"5s"})"));
    EXPECT_EQ(ten.reproducibility.at("steps").at("a").at("timeouts").at("invoke"), "10s");
}

TEST(Fingerprint, ReproducibilitySectionMatchesTheRunRecordSchema) {
    const auto workflow = validated(chain_config());
    const json& section = workflow.reproducibility;
    const praenora::VersionInfo versions = praenora::version_info();

    // The section is embedded in the run record; it must satisfy that schema.
    const json record = {
        {"kind", "run-record"},
        {"format_version", "0.2.0"},
        {"fingerprint", workflow.fingerprint},
        {"reproducibility", section},
        {"execution",
         {{"status", "running"},
          {"ticks_completed", 0},
          {"platform",
           {{"os", "macos"}, {"arch", "arm64"}, {"compiler", "c"}, {"build_type", "Debug"}}},
          {"overrides", json::object()},
          {"validation", "strict"},
          {"manifests", workflow.manifest_locations},
          {"inputs", workflow.input_counts},
          {"isolation", workflow.isolation}}}};
    const auto& schema = praenora::detail::SchemaRegistry::builtin().for_kind(
        "run-record", praenora::detail::parse_semver("0.2.0"));
    const auto issues = schema.validate(record);
    for (const auto& issue : issues) {
        ADD_FAILURE() << issue.pointer << ": " << issue.message;
    }

    EXPECT_EQ(section.at("versions"),
              (json{{"runtime", versions.runtime},
                    {"abi", {{"major", versions.abi_major}, {"minor", versions.abi_minor}}},
                    {"pmp", versions.pmp}}));
    EXPECT_EQ(section.at("workflow").at("name"), "test.chain");
    EXPECT_EQ(section.at("workflow").at("digest"), workflow.definition->digest);
    // The catalog entries the workflow uses, by id@version.
    json catalog = json::object();
    catalog["test.echo@0.1.0"] = workflow.steps.at(0).capability->digest;
    catalog["test.blob@0.1.0"] =
        praenora::test::test_context()
            .catalog.find_message_type("test.blob", praenora::detail::parse_semver("0.1.0"))
            ->digest;
    EXPECT_EQ(section.at("catalog"), catalog);
    EXPECT_EQ(section.at("time"), json::parse(R"({"unit":"1","start":0.0,"period":1.0})"));
    EXPECT_EQ(section.at("seed"), 7);
    EXPECT_EQ(section.at("ticks"), nullptr); // until the replay input is exhausted
    EXPECT_EQ(section.at("timeouts"),
              json::parse(R"({"invoke":"300s","lifecycle":"60s","cancel_grace":"5s"})"));
    EXPECT_EQ(
        section.at("inputs"),
        (json{{"source",
               {{"source", "observation-csv"},
                {"digest", praenora::detail::sha256_hex(praenora::test::observations_csv())}}}}));
    const json& a = section.at("steps").at("a");
    const auto& manifest = *workflow.steps.at(0).manifest;
    EXPECT_EQ(a.at("capability"), json::parse(R"({"id":"test.echo","version":"0.1.0"})"));
    EXPECT_EQ(a.at("module"),
              (json{{"name", "fx-echo"},
                    {"version", "0.1.0"},
                    {"manifest_sha256", manifest.manifest_sha256},
                    {"library_sha256", praenora::detail::sha256_file_hex(manifest.library_path)},
                    {"sdk_version", "0.2.0"}}));
    EXPECT_EQ(a.at("parameters"), json::object());
    EXPECT_EQ(a.at("seed"), praenora::detail::derive_step_seed(7, "a"));
    EXPECT_EQ(a.at("timeouts"),
              json::parse(R"({"invoke":"300s","lifecycle":"60s","cancel_grace":"5s"})"));
    // No module declares a service: the services are there, and empty.
    EXPECT_EQ(section.at("services"), json::object());
    EXPECT_EQ(a.at("services"), json::array());
    EXPECT_EQ(section.at("steps").at("b").at("services"), json::array());

    // Where each manifest was found is kept beside the section, for the run record's execution
    // section: relative to the caller's search path, the first one.
    EXPECT_EQ(workflow.manifest_locations, json::parse(R"({
        "steps": {
            "a": {"search_path": {"origin": "caller", "index": 0},
                  "file": "fx-echo/fx-echo.pmm.json"},
            "b": {"search_path": {"origin": "caller", "index": 0},
                  "file": "fx-slow/fx-slow.pmm.json"}},
        "services": {}})"));
}

// Where the manifests are does not change the fingerprint: the same module files, found through
// the configuration's own search path, give the same one. Only their recorded location changes.
TEST(Fingerprint, IgnoresWhereTheManifestsAre) {
    const auto reference = validated(chain_config());

    ScratchDir dir;
    write_text_file(dir.path() / "obs.csv", praenora::test::observations_csv());
    for (const char* name : {"fx-echo", "fx-slow"}) {
        // Byte for byte: the manifest's digest is part of the fingerprint.
        const fs::path manifest = praenora::test::fixture_manifest(name);
        const fs::path copy = dir.path() / "mods" / name;
        fs::create_directories(copy);
        fs::copy_file(manifest, copy / manifest.filename());
        const fs::path library = praenora::detail::load_manifest(manifest).library_path;
        fs::copy_file(library, copy / library.filename());
    }
    const auto context = praenora::test::make_test_context({});
    const auto result = validate_text(
        dir.path(),
        chain_config("run: { seed: 7 }\n", "{}", "modules: { search_paths: [absent, mods] }\n"), {},
        *context);
    ASSERT_TRUE(result.workflow.has_value()) << result.report.to_text();
    EXPECT_NE(result.workflow->steps.at(0).manifest->manifest_path,
              reference.steps.at(0).manifest->manifest_path);
    EXPECT_EQ(result.workflow->fingerprint, reference.fingerprint);
    EXPECT_EQ(result.workflow->manifest_locations.at("steps").at("a"), json::parse(R"({
        "search_path": {"origin": "configuration", "index": 1},
        "file": "fx-echo/fx-echo.pmm.json"})"));
    EXPECT_NE(result.workflow->manifest_locations, reference.manifest_locations);

    // The same copies found through PRAENORA_MODULE_PATH.
    const praenora::test::ScopedEnvironment variable("PRAENORA_MODULE_PATH",
                                                     path_to_utf8(dir.path() / "mods"));
    const auto from_environment = praenora::test::make_test_context({});
    from_environment->modules =
        praenora::detail::ModuleIndex::scan(praenora::detail::module_search_paths({}, {}, true));
    from_environment->use_environment = true;
    const auto found = validate_text(dir.path(), chain_config(), {}, *from_environment);
    ASSERT_TRUE(found.workflow.has_value()) << found.report.to_text();
    EXPECT_EQ(found.workflow->fingerprint, reference.fingerprint);
    EXPECT_EQ(found.workflow->manifest_locations.at("steps").at("b"), json::parse(R"({
        "search_path": {"origin": "environment", "index": 0},
        "file": "fx-slow/fx-slow.pmm.json"})"));
}

namespace {

// test.chain with step a bound to `a` and step b to `b` (fixture modules on `module_paths`),
// validated in a fresh directory against the providers on `provider_paths` (UTF-8).
praenora::detail::ValidatedWorkflow
validated_with_providers(const std::string& a, const std::string& b,
                         const std::vector<std::string>& provider_paths,
                         std::vector<std::string> module_paths = {PRAENORA_TEST_MODULES_DIR}) {
    const auto context =
        praenora::test::make_test_context(std::move(module_paths), {}, provider_paths);
    ScratchDir dir;
    write_text_file(dir.path() / "obs.csv", praenora::test::observations_csv());
    const std::string yaml = "kind: workflow-config\n"
                             "format_version: \"0.1.0\"\n"
                             "workflow: { name: test.chain, version: \"0.1.0\" }\n"
                             "run: { seed: 7 }\n"
                             "inputs:\n"
                             "  source: { source: observation-csv, path: obs.csv }\n"
                             "steps:\n"
                             "  a: { module: { name: " +
                             a +
                             ", version: \"0.1.0\" } }\n"
                             "  b: { module: { name: " +
                             b + ", version: \"0.1.0\" } }\n";
    auto result = validate_text(dir.path(), yaml, {}, *context);
    if (!result.workflow) {
        ADD_FAILURE() << result.report.to_text();
        throw std::runtime_error("validation failed");
    }
    return std::move(*result.workflow);
}

std::string utf8(const fs::path& path) { return path_to_utf8(path); }

} // namespace

// Every service the run resolved, with its provider's identity and engine; each step lists the
// services its module may use.
TEST(Fingerprint, RecordsTheServicesOfTheRunAndTheirProviders) {
    const auto workflow = validated_with_providers("fx-service", "fx-echo",
                                                   {PRAENORA_TEST_PROVIDERS_DIR "/test-echo"});
    const fs::path manifest = praenora::test::fixture_provider_manifest("test-echo");
    const auto provider = praenora::detail::load_provider_manifest(manifest);
    const json& section = workflow.reproducibility;
    EXPECT_EQ(section.at("services"),
              (json{{"test.echo@0",
                     {{"minor", 0},
                      {"provider",
                       {{"name", "test-echo"},
                        {"version", "0.1.0"},
                        {"manifest_sha256", praenora::detail::sha256_file_hex(manifest)},
                        {"library_sha256",
                         praenora::detail::sha256_file_hex(provider.library_path)}}},
                      {"engine", {{"name", "test-echo-engine"}, {"version", "1.0.0"}}}}}}));
    EXPECT_EQ(section.at("steps").at("a").at("services"), json({"test.echo@0"}));
    EXPECT_EQ(section.at("steps").at("b").at("services"), json::array());
    EXPECT_EQ(workflow.steps.at(0).services, std::vector<std::string>{"test.echo@0"});
    ASSERT_EQ(workflow.services.size(), 1u);
    EXPECT_EQ(praenora::detail::to_string(workflow.services.at("test.echo@0").service),
              "test.echo@0.0");
    // Where the provider's manifest was found: directly in the caller's first provider path.
    EXPECT_EQ(workflow.manifest_locations.at("services"), json::parse(R"({
        "test.echo@0": {"search_path": {"origin": "caller", "index": 0},
                        "file": "test-echo.provider.json"}})"));

    // As written in a run record, the section satisfies the run-record schema.
    const json record = {
        {"kind", "run-record"},
        {"format_version", "0.2.0"},
        {"fingerprint", workflow.fingerprint},
        {"reproducibility", section},
        {"execution",
         {{"status", "running"},
          {"ticks_completed", 0},
          {"platform",
           {{"os", "macos"}, {"arch", "arm64"}, {"compiler", "c"}, {"build_type", "Debug"}}},
          {"overrides", json::object()},
          {"validation", "strict"},
          {"manifests", workflow.manifest_locations},
          {"inputs", workflow.input_counts},
          {"isolation", workflow.isolation}}}};
    const auto& schema = praenora::detail::SchemaRegistry::builtin().for_kind(
        "run-record", praenora::detail::parse_semver("0.2.0"));
    for (const auto& issue : schema.validate(record)) {
        ADD_FAILURE() << issue.pointer << ": " << issue.message;
    }
}

// The fingerprint changes when the provider's library changes, and when its engine does.
TEST(Fingerprint, ChangesWithTheProviderLibraryAndItsEngine) {
    ScratchDir dir;
    const fs::path manifest =
        praenora::test::copy_fixture_provider("test-echo", dir.path() / "providers");
    const std::vector<std::string> paths = {utf8(dir.path() / "providers")};
    const auto before = validated_with_providers("fx-service", "fx-echo", paths);

    praenora::test::append_to_file(
        praenora::detail::load_provider_manifest(manifest).library_path, "another build");
    const auto rebuilt = validated_with_providers("fx-service", "fx-echo", paths);
    EXPECT_NE(rebuilt.fingerprint, before.fingerprint);
    EXPECT_NE(rebuilt.reproducibility.at("services").at("test.echo@0").at("provider").at(
                  "library_sha256"),
              before.reproducibility.at("services").at("test.echo@0").at("provider").at(
                  "library_sha256"));

    praenora::test::copy_fixture_provider("test-echo", dir.path() / "providers", [](json& m) {
        m["engine"]["version"] = "1.0.1";
    });
    praenora::test::append_to_file(
        praenora::detail::load_provider_manifest(manifest).library_path, "another build");
    const auto other_engine = validated_with_providers("fx-service", "fx-echo", paths);
    EXPECT_NE(other_engine.fingerprint, rebuilt.fingerprint);
    EXPECT_EQ(other_engine.reproducibility.at("services").at("test.echo@0").at("engine").at(
                  "version"),
              "1.0.1");
    // The library is the same as in `rebuilt`: only the manifest and its engine differ.
    EXPECT_EQ(other_engine.reproducibility.at("services").at("test.echo@0").at("provider").at(
                  "library_sha256"),
              rebuilt.reproducibility.at("services").at("test.echo@0").at("provider").at(
                  "library_sha256"));
}

// Steps that declare one service name and major at different minors share one provider: the one
// that the highest minor resolved to.
TEST(Fingerprint, OneProviderServesEachServiceOfTheRun) {
    ScratchDir dir;
    // test-echo offers test.echo@0.0 and comes first; test-echo-newer offers test.echo@0.7.
    praenora::test::copy_fixture_provider("test-echo", dir.path() / "providers" / "a-older");
    praenora::test::copy_fixture_provider(
        "test-echo", dir.path() / "providers" / "b-newer", [](json& m) {
            m["provider"]["name"] = "test-echo-newer";
            m["services"] = json::array({{{"name", "test.echo"}, {"major", 0}, {"minor", 7}}});
        });
    const std::vector<std::string> paths = {utf8(dir.path() / "providers")};

    // Alone, fx-service (test.echo@0.0) gets the first provider.
    const auto alone = validated_with_providers("fx-service", "fx-echo", paths);
    EXPECT_EQ(alone.reproducibility.at("services").at("test.echo@0").at("provider").at("name"),
              "test-echo");

    // With fx-service-minor (test.echo@0.7), both get test-echo-newer, which offers both.
    const auto both = validated_with_providers("fx-service", "fx-service-minor", paths);
    const json& service = both.reproducibility.at("services").at("test.echo@0");
    EXPECT_EQ(service.at("provider").at("name"), "test-echo-newer");
    EXPECT_EQ(service.at("minor"), 7);
    EXPECT_EQ(both.reproducibility.at("steps").at("a").at("services"), json({"test.echo@0"}));
    EXPECT_EQ(both.reproducibility.at("steps").at("b").at("services"), json({"test.echo@0"}));
}

// ---- the target descriptor ----

namespace {

json descriptor_json() {
    return json::parse(R"({
      "schema_version": "0.0.0", "id": "d1", "name": "Test",
      "data": {"id": "obs", "href": "../obs.csv"},
      "columns": {"series_id": "series_id", "variable_id": "variable_id", "time": "time",
                  "value": "value", "unit": "unit"},
      "time_unit": "1",
      "variables": [{"id": "x", "name": "X", "unit": "count", "value_type": "scalar"}]
    })");
}

// A valid configuration of test.chain whose input is described by `descriptor` (sub/target.json),
// over the observations CSV one level up, with a path too when `extra_input` gives one.
praenora::detail::ValidatedWorkflow validated_with(const json& descriptor,
                                                   const std::string& extra_input = "") {
    ScratchDir dir;
    write_text_file(dir.path() / "obs.csv", praenora::test::observations_csv());
    write_text_file(dir.path() / "sub" / "target.json", descriptor.dump(2));
    std::string yaml = chain_config();
    const std::string plain = "  source: { source: observation-csv, path: obs.csv }\n";
    yaml.replace(yaml.find(plain), plain.size(),
                 "  source:\n    source: observation-csv\n    descriptor: sub/target.json\n" +
                     extra_input);
    auto result = validate_text(dir.path(), yaml);
    if (!result.workflow) {
        ADD_FAILURE() << result.report.to_text();
        throw std::runtime_error("validation failed");
    }
    return std::move(*result.workflow);
}

} // namespace

TEST(Fingerprint, ADescriptorAddsItsDigestToTheInputsEntry) {
    const auto workflow = validated_with(descriptor_json());
    const json& entry = workflow.reproducibility.at("inputs").at("source");
    // The CSV's digest as before, the descriptor's beside it; no path, no columns.
    EXPECT_EQ(entry.at("source"), "observation-csv");
    EXPECT_EQ(entry.at("digest"),
              praenora::detail::sha256_hex(praenora::test::observations_csv()));
    EXPECT_EQ(entry.at("descriptor").at("schema_version"), "0.0.0");
    EXPECT_EQ(entry.at("descriptor").at("digest").get<std::string>().size(), 64u);
    EXPECT_EQ(entry.size(), 3u);
    // The section, with it, satisfies the run record's schema.
    const json record = {
        {"kind", "run-record"},
        {"format_version", "0.2.0"},
        {"fingerprint", workflow.fingerprint},
        {"reproducibility", workflow.reproducibility},
        {"execution",
         {{"status", "running"},
          {"ticks_completed", 0},
          {"platform",
           {{"os", "macos"}, {"arch", "arm64"}, {"compiler", "c"}, {"build_type", "Debug"}}},
          {"overrides", json::object()},
          {"validation", "strict"},
          {"manifests", workflow.manifest_locations},
          {"inputs", workflow.input_counts},
          {"isolation", workflow.isolation}}}};
    const auto& schema = praenora::detail::SchemaRegistry::builtin().for_kind(
        "run-record", praenora::detail::parse_semver("0.2.0"));
    for (const auto& issue : schema.validate(record)) {
        ADD_FAILURE() << issue.pointer << ": " << issue.message;
    }
}

TEST(Fingerprint, ADescriptorChangesItAsPartOfTheResultsDo) {
    const json base = descriptor_json();
    const auto reference = validated_with(base);
    // Not the same as the plain binding of the same data: the descriptor is part of what decides.
    EXPECT_NE(validated(chain_config()).fingerprint, reference.fingerprint);
    EXPECT_EQ(validated_with(base).fingerprint, reference.fingerprint); // stable

    // Words and places: no change.
    json prose = base;
    prose["id"] = "another";
    prose["name"] = "Another name";
    prose["description"] = "Words";
    prose["data"]["id"] = "other-data";
    prose["data"]["media_type"] = "text/csv";
    prose["x-circle-outcomes"] = {{"variable_ids", json::array({"x"})}};
    prose["variables"][0]["name"] = "Renamed";
    EXPECT_EQ(validated_with(prose).fingerprint, reference.fingerprint);
    // The descriptor's id is not hashed, but the run record reads it.
    EXPECT_EQ(validated_with(prose).input_counts.at("source").at("descriptor_id"), "another");
    // The href can be given by the configuration's path instead: the same data, the same run.
    json remote = base;
    remote["data"]["href"] = "https://example.org/x.csv";
    EXPECT_EQ(validated_with(remote, "    path: obs.csv\n").fingerprint, reference.fingerprint);

    // What the runtime does with the data: a change.
    for (const auto& edit : std::vector<std::pair<std::string, std::function<void(json&)>>>{
             {"role", [](json& d) { d["variables"][0]["role"] = "covariate"; }},
             {"bounds", [](json& d) { d["variables"][0]["lower_bound"] = -1e9; }},
             {"duplicate policy", [](json& d) { d["duplicate_policy"] = "error"; }},
             {"column mapping", [](json& d) { d["columns"].erase("unit"); }},
             {"time unit", [](json& d) { d.erase("time_unit"); }}}) {
        SCOPED_TRACE(edit.first);
        json changed = base;
        edit.second(changed);
        EXPECT_NE(validated_with(changed).fingerprint, reference.fingerprint);
    }
}

TEST(Fingerprint, AnInputWithoutADescriptorHasTheEntryItAlwaysHad) {
    const auto workflow = validated(chain_config());
    const json& entry = workflow.reproducibility.at("inputs").at("source");
    EXPECT_EQ(entry, (json{{"source", "observation-csv"},
                           {"digest", praenora::detail::sha256_hex(
                                          praenora::test::observations_csv())}}));
    // Its counts are the two they were; nothing of a descriptor.
    EXPECT_EQ(workflow.input_counts,
              json::parse(R"({"source":{"rows_total":4,"rows_before_start":0}})"));
}
