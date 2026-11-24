// The validation sequence (configuration, catalog, graph, bindings, modules, parameters, inputs):
// each problem is reported as a located diagnostic, all problems are collected, and checks that
// depend on a failed one are skipped. Uses the test catalog, the fixture modules and
// configurations written to scratch directories.
#include "api/validation.hpp"
#include "core/paths.hpp"
#include "core/seed.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"

#include "module_fixtures.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"
#include "validation_fixtures.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Diagnostic;
using praenora::ErrorCode;
using praenora::Severity;
using praenora::detail::path_to_utf8;
using praenora::test::RestorePermissions;
using praenora::test::ScratchDir;
using praenora::test::validate_text;
using praenora::test::write_text_file;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// A configuration of `workflow` (test.single unless given) with `body` after the workflow line.
std::string config(const std::string& body, const std::string& workflow = "test.single",
                   const std::string& version = "0.1.0") {
    return "kind: workflow-config\n"
           "format_version: \"0.1.0\"\n"
           "workflow: { name: " +
           workflow + ", version: \"" + version + "\" }\n" + body;
}

// The two kinds of data source.
const char* const kDataSources[] = {"observation-csv", "json-document"};

// A configuration of test.single whose input "source" reads data/obs.csv with a source of kind
// `source`; the path is on line 6.
std::string data_config(const std::string& source) {
    return config("run: { ticks: 1 }\n"
                  "inputs:\n"
                  "  source: { source: " +
                  source +
                  ", path: data/obs.csv }\n"
                  "steps:\n"
                  "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n");
}

// A json-document input "source" (any object fits test.blob) and one tick per run.
const char* const kSourceInput = "run: { ticks: 3 }\n"
                                 "inputs:\n"
                                 "  source: { source: json-document, path: source.json }\n";

} // namespace

TEST(Validation, MissingBinding) {
    ScratchDir dir;
    write_text_file(dir.path() / "source.json", "{}");
    const auto result = validate_text(dir.path(), config(std::string(kSourceInput) +
                                                             "steps:\n"
                                                             "  a: { module: { name: fx-echo, "
                                                             "version: \"0.1.0\" } }\n",
                                                         "test.chain"));
    EXPECT_FALSE(result.workflow.has_value());
    const auto errors = result.errors();
    ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
    EXPECT_EQ(errors[0].code, ErrorCode::BINDING_INVALID);
    EXPECT_EQ(errors[0].step, "b");
    EXPECT_EQ(errors[0].location.pointer, "/steps");
    EXPECT_EQ(errors[0].location.line, 7);
    EXPECT_EQ(errors[0].location.file, path_to_utf8(dir.path() / "workflow.yaml"));
    EXPECT_TRUE(contains(errors[0].message, "'b'")) << errors[0].message;
}

TEST(Validation, ExtraBinding) {
    ScratchDir dir;
    write_text_file(dir.path() / "source.json", "{}");
    const auto result = validate_text(
        dir.path(), config(std::string(kSourceInput) +
                           "steps:\n"
                           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"
                           "  z: { module: { name: fx-echo, version: \"0.1.0\" } }\n"));
    EXPECT_FALSE(result.workflow.has_value());
    const auto errors = result.errors();
    ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
    EXPECT_EQ(errors[0].code, ErrorCode::BINDING_INVALID);
    EXPECT_EQ(errors[0].step, "z");
    EXPECT_EQ(errors[0].location.pointer, "/steps/z");
    EXPECT_EQ(errors[0].location.line, 9);
}

TEST(Validation, UnknownWorkflowVersion) {
    ScratchDir dir;
    const auto result = validate_text(
        dir.path(), config(std::string(kSourceInput) +
                               "steps:\n"
                               "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n",
                           "test.single", "9.9.9"));
    EXPECT_FALSE(result.workflow.has_value());
    const auto errors = result.errors();
    // Nothing else can be checked without the definition.
    ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
    EXPECT_EQ(errors[0].code, ErrorCode::WORKFLOW_NOT_FOUND);
    EXPECT_EQ(errors[0].location.pointer, "/workflow/version");
    EXPECT_EQ(errors[0].location.line, 3);
    EXPECT_TRUE(contains(errors[0].message, "0.1.0")) << errors[0].message; // the known version

    const auto unknown = validate_text(
        dir.path(), config(std::string(kSourceInput) + "steps: {}\n", "test.unheard-of"));
    ASSERT_EQ(unknown.errors().size(), 1u) << unknown.report.to_text();
    EXPECT_EQ(unknown.errors()[0].code, ErrorCode::WORKFLOW_NOT_FOUND);
    EXPECT_EQ(unknown.errors()[0].location.pointer, "/workflow/name");
}

TEST(Validation, UnknownModuleListsSearchPaths) {
    ScratchDir dir;
    write_text_file(dir.path() / "source.json", "{}");
    fs::create_directories(dir.path() / "extra mods");
    const auto result =
        validate_text(dir.path(), config(std::string(kSourceInput) +
                                         "modules: { search_paths: [extra mods] }\n"
                                         "steps:\n"
                                         "  a:\n"
                                         "    module: { name: fx-nowhere, version: \"0.1.0\" }\n"));
    EXPECT_FALSE(result.workflow.has_value());
    const auto errors = result.errors();
    ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
    const Diagnostic& error = errors[0];
    EXPECT_EQ(error.code, ErrorCode::MODULE_NOT_FOUND);
    EXPECT_EQ(error.step, "a");
    EXPECT_EQ(error.module, "fx-nowhere@0.1.0");
    EXPECT_EQ(error.location.pointer, "/steps/a/module");
    EXPECT_EQ(error.location.line, 10);
    // The caller's module path is searched first, then the configuration's.
    const std::string explicit_path =
        path_to_utf8(praenora::test::utf8_path(PRAENORA_TEST_MODULES_DIR).lexically_normal());
    const std::string config_path = path_to_utf8(dir.path() / "extra mods");
    const auto first = error.message.find(explicit_path);
    const auto second = error.message.find(config_path);
    EXPECT_NE(first, std::string::npos) << error.message;
    EXPECT_NE(second, std::string::npos) << error.message;
    EXPECT_LT(first, second) << error.message;
}

TEST(Validation, InvalidParametersPointToConfigLine) {
    ScratchDir dir;
    write_text_file(dir.path() / "source.json", "{}");
    const auto result = validate_text(dir.path(), config(std::string(kSourceInput) +
                                                         "steps:\n"
                                                         "  a:\n"
                                                         "    module: { name: fx-slow, version: "
                                                         "\"0.1.0\" }\n"
                                                         "    parameters:\n"
                                                         "      sleep_ms: -5\n"));
    EXPECT_FALSE(result.workflow.has_value());
    const auto errors = result.errors();
    ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
    const Diagnostic& error = errors[0];
    EXPECT_EQ(error.code, ErrorCode::PARAMETERS_INVALID);
    EXPECT_EQ(error.step, "a");
    EXPECT_EQ(error.module, "fx-slow@0.1.0");
    EXPECT_EQ(error.location.file, path_to_utf8(dir.path() / "workflow.yaml"));
    EXPECT_EQ(error.location.pointer, "/steps/a/parameters/sleep_ms");
    EXPECT_EQ(error.location.line, 11);
    EXPECT_EQ(error.location.column, 17);
    EXPECT_TRUE(contains(result.report.to_text(), "workflow.yaml:11:17: error[PARAMETERS_INVALID]: "
                                                  "/steps/a/parameters/sleep_ms: "))
        << result.report.to_text();
}

TEST(Validation, MissingInputBinding) {
    ScratchDir dir;
    const auto result = validate_text(
        dir.path(), config("run: { ticks: 2 }\n"
                           "steps:\n"
                           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"));
    EXPECT_FALSE(result.workflow.has_value());
    const auto errors = result.errors();
    ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
    EXPECT_EQ(errors[0].code, ErrorCode::INPUT_BINDING_INVALID);
    EXPECT_EQ(errors[0].location.pointer, "/inputs");
    EXPECT_TRUE(contains(errors[0].message, "'source'")) << errors[0].message;
}

TEST(Validation, CsvMissingRequiredColumn) {
    ScratchDir dir;
    const fs::path data = dir.path() / "data" / "obs.csv";
    write_text_file(data, "series_id,variable_id,time\nS1,x,0.5\n");
    const auto result = validate_text(
        dir.path(), config("inputs:\n"
                           "  source: { source: observation-csv, path: data/obs.csv }\n"
                           "steps:\n"
                           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"));
    EXPECT_FALSE(result.workflow.has_value());
    const auto errors = result.errors();
    ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
    EXPECT_EQ(errors[0].code, ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(errors[0].location.file, path_to_utf8(data));
    EXPECT_EQ(errors[0].location.line, 1);
    EXPECT_TRUE(contains(errors[0].message, "'value'")) << errors[0].message;
    EXPECT_TRUE(contains(errors[0].message, "'source'")) << errors[0].message; // the input
}

TEST(Validation, DataFileThatCannotBeReadIsTargetDataUnavailable) {
    // A data file that does not exist, or a directory in its place, is located at the
    // configuration's path.
    ScratchDir dir;
    const fs::path data = dir.path() / "data" / "obs.csv";
    fs::create_directories(data.parent_path());
    for (const char* source : kDataSources) {
        for (const bool directory : {false, true}) {
            SCOPED_TRACE(std::string(source) + (directory ? ", a directory" : ", no file"));
            if (directory) {
                fs::create_directory(data);
            }
            const auto result = validate_text(dir.path(), data_config(source));
            fs::remove(data);
            EXPECT_FALSE(result.workflow.has_value());
            const auto errors = result.errors();
            ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
            EXPECT_EQ(errors[0].code, ErrorCode::TARGET_DATA_UNAVAILABLE);
            EXPECT_EQ(errors[0].location.pointer, "/inputs/source/path");
            EXPECT_EQ(errors[0].location.line, 6);
            EXPECT_TRUE(contains(errors[0].message, directory ? "is not a file" : "does not exist"))
                << errors[0].message;
        }
    }
}

TEST(Validation, UnreadableDataFileIsTargetDataUnavailable) {
    // A data file the process may not read is located at the file. One in a directory the
    // process may not search is located at the configuration's path, since not even its status
    // can be read; it is not reported as missing.
    ScratchDir dir;
    const fs::path locked = dir.path() / "data";
    const fs::path data = locked / "obs.csv";
    write_text_file(data, praenora::test::observations_csv());
    // Declared last, the directory's guard is destroyed first: the file's guard can reach the file
    // only once the directory has its permissions back.
    const RestorePermissions restore_data{data, fs::perms::owner_read | fs::perms::owner_write};
    const RestorePermissions restore_locked{locked, fs::perms::owner_all};
    fs::permissions(data, fs::perms::none);
    if (std::ifstream(data, std::ios::binary).is_open()) {
        GTEST_SKIP() << "the process can read a file without permissions";
    }
    for (const char* source : kDataSources) {
        SCOPED_TRACE(std::string(source) + ", unreadable");
        const auto result = validate_text(dir.path(), data_config(source));
        const auto errors = result.errors();
        ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
        EXPECT_EQ(errors[0].code, ErrorCode::TARGET_DATA_UNAVAILABLE);
        EXPECT_EQ(errors[0].location.file, path_to_utf8(data));
        EXPECT_TRUE(contains(errors[0].message, "input 'source': cannot read the"))
            << errors[0].message;
    }

    fs::permissions(data, fs::perms::owner_read | fs::perms::owner_write);
    fs::permissions(locked, fs::perms::none);
    std::error_code ec;
    (void)fs::status(data, ec);
    ASSERT_TRUE(ec) << "the status of a file in a directory without permissions was read";
    for (const char* source : kDataSources) {
        SCOPED_TRACE(std::string(source) + ", in a locked directory");
        const auto result = validate_text(dir.path(), data_config(source));
        const auto errors = result.errors();
        ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
        EXPECT_EQ(errors[0].code, ErrorCode::TARGET_DATA_UNAVAILABLE);
        EXPECT_EQ(errors[0].location.pointer, "/inputs/source/path");
        EXPECT_EQ(errors[0].location.line, 6);
        EXPECT_TRUE(contains(errors[0].message, "' cannot be read: ")) << errors[0].message;
        EXPECT_FALSE(contains(errors[0].message, "does not exist")) << errors[0].message;
    }
}

TEST(Validation, TwoColumnsFromOneHeaderAreReportedOnce) {
    // Two spellings of one problem, each reported once, as CONFIG_INVALID at the entry that
    // causes it: two remappings to one header, and a remapping to the header that another column
    // reads under its own name.
    ScratchDir dir;
    write_text_file(dir.path() / "obs.csv", praenora::test::observations_csv());
    const struct {
        const char* columns;
        const char* pointer;
        const char* header;
    } cases[] = {
        {"{ time: value, value: value }", "/inputs/source/columns/value", "'value'"},
        {"{ value: time }", "/inputs/source/columns/value", "'time'"},
    };
    for (const auto& c : cases) {
        const auto result = validate_text(
            dir.path(), config(std::string("inputs:\n"
                                           "  source:\n"
                                           "    source: observation-csv\n"
                                           "    path: obs.csv\n"
                                           "    columns: ") +
                               c.columns +
                               "\n"
                               "steps:\n"
                               "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"));
        EXPECT_FALSE(result.workflow.has_value()) << c.columns;
        const auto errors = result.errors();
        EXPECT_EQ(errors.size(), 1u) << c.columns << "\n" << result.report.to_text();
        if (errors.size() != 1u) {
            continue;
        }
        EXPECT_EQ(errors[0].code, ErrorCode::CONFIG_INVALID) << c.columns;
        EXPECT_EQ(errors[0].location.pointer, c.pointer) << c.columns;
        EXPECT_EQ(errors[0].location.line, 8) << c.columns;
        EXPECT_TRUE(contains(errors[0].message, "'time'")) << errors[0].message;
        EXPECT_TRUE(contains(errors[0].message, "'value'")) << errors[0].message;
        EXPECT_TRUE(contains(errors[0].message, "header " + std::string(c.header)))
            << errors[0].message;
    }
}

TEST(Validation, CollectsMultipleDiagnostics) {
    ScratchDir dir;
    // Two independent problems: the input is not bound, and a parameter is out of range.
    const auto result =
        validate_text(dir.path(), config("run: { ticks: 2 }\n"
                                         "steps:\n"
                                         "  a:\n"
                                         "    module: { name: fx-slow, version: \"0.1.0\" }\n"
                                         "    parameters: { cooperative: maybe }\n"));
    EXPECT_FALSE(result.workflow.has_value());
    const auto errors = result.errors();
    ASSERT_EQ(errors.size(), 2u) << result.report.to_text();
    EXPECT_EQ(errors[0].code, ErrorCode::INPUT_BINDING_INVALID);
    EXPECT_EQ(errors[1].code, ErrorCode::PARAMETERS_INVALID);
    EXPECT_EQ(errors[1].location.pointer, "/steps/a/parameters/cooperative");
}

TEST(Validation, UnknownCatalogReferencesAreReportedOnce) {
    ScratchDir dir;
    const auto context = praenora::test::make_test_context(
        {PRAENORA_TEST_MODULES_DIR},
        {praenora::test::fixture_path("catalog-bad/unknown-references")});
    write_text_file(dir.path() / "source.json", "{}");
    const auto result =
        validate_text(dir.path(),
                      config(std::string(kSourceInput) +
                                 "steps:\n"
                                 "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n",
                             "test.dangling"),
                      {}, *context);
    EXPECT_FALSE(result.workflow.has_value());
    std::vector<std::string> pointers;
    for (const auto& error : result.errors()) {
        EXPECT_EQ(error.code, ErrorCode::CATALOG_ENTRY_NOT_FOUND) << error.message;
        pointers.push_back(error.location.pointer);
    }
    // The input's and output's message types and the step's capability; the graph is not
    // checked again, and the module (whose capability is unknown) is not either.
    EXPECT_EQ(pointers, (std::vector<std::string>{"/inputs/source", "/outputs/result",
                                                  "/steps/a/capability"}))
        << result.report.to_text();
}

TEST(Validation, DefinitionProblemsAreLocatedInTheDefinitionFile) {
    // A problem of the workflow definition, not of the configuration: it is located in the
    // definition's catalog file, with line and column, like the configuration's own problems.
    ScratchDir dir;
    const fs::path catalog = praenora::test::fixture_path("catalog-bad/unknown-port");
    const auto context = praenora::test::make_test_context({PRAENORA_TEST_MODULES_DIR}, {catalog});
    write_text_file(dir.path() / "source.json", "{}");
    const auto result =
        validate_text(dir.path(),
                      config(std::string(kSourceInput) +
                                 "steps:\n"
                                 "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n",
                             "bad.unknown-port"),
                      {}, *context);
    EXPECT_FALSE(result.workflow.has_value());
    const auto errors = result.errors();
    ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
    EXPECT_EQ(errors[0].code, ErrorCode::PORT_UNKNOWN);
    EXPECT_EQ(errors[0].location.file,
              path_to_utf8(catalog / "workflows" / "bad.unknown-port" / "0.1.0.json"));
    EXPECT_EQ(errors[0].location.pointer, "/connections/0/to");
    EXPECT_EQ(errors[0].location.line, 22) << result.report.to_text();
    EXPECT_GT(errors[0].location.column, 0);
}

TEST(Validation, ModuleProblemsAreFoundWithoutLoading) {
    ScratchDir modules;
    const fs::path manifest_source = praenora::test::fixture_manifest("fx-echo");
    json manifest = json::parse(praenora::detail::read_file_bytes(manifest_source));
    const std::string library = manifest.at("library").at("path").get<std::string>();
    const auto write_manifest = [&](const json& document) {
        write_text_file(modules.path() / "fx-echo.pmm.json", document.dump(2));
    };
    // Each check scans the module directory afresh, after the manifest has been rewritten.
    const auto validate = [&]() {
        ScratchDir dir;
        write_text_file(dir.path() / "source.json", "{}");
        const auto context = praenora::test::make_test_context({path_to_utf8(modules.path())});
        auto result =
            validate_text(dir.path(),
                          config(std::string(kSourceInput) +
                                 "steps:\n"
                                 "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"),
                          {}, *context);
        EXPECT_FALSE(result.workflow.has_value());
        return result.errors();
    };

    // The manifest without its library.
    write_manifest(manifest);
    auto errors = validate();
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].code, ErrorCode::LIBRARY_NOT_FOUND) << errors[0].message;
    EXPECT_EQ(errors[0].step, "a");
    EXPECT_EQ(errors[0].location.file, path_to_utf8(modules.path() / "fx-echo.pmm.json"));
    EXPECT_EQ(errors[0].location.pointer, "/library/path");

    // With the library, but a different pinned digest.
    const fs::path library_file =
        praenora::test::fixture_module_dir("fx-echo") / praenora::test::utf8_path(library);
    fs::copy_file(library_file, modules.path() / praenora::test::utf8_path(library));
    json pinned = manifest;
    pinned["library"]["sha256"] = std::string(64, '0');
    write_manifest(pinned);
    errors = validate();
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].code, ErrorCode::LIBRARY_DIGEST_MISMATCH) << errors[0].message;
    EXPECT_EQ(errors[0].location.pointer, "/library/sha256");

    // A protocol version the runtime does not speak.
    json newer = manifest;
    newer["pmp_version"] = "0.9.0";
    write_manifest(newer);
    errors = validate();
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].code, ErrorCode::PROTOCOL_INCOMPATIBLE) << errors[0].message;
    EXPECT_EQ(errors[0].module, "fx-echo@0.1.0");

    // A manifest for another platform only.
    json elsewhere = manifest;
    elsewhere["platform"]["os"] =
        manifest.at("platform").at("os") == "windows" ? "linux" : "windows";
    write_manifest(elsewhere);
    errors = validate();
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].code, ErrorCode::PLATFORM_MISMATCH) << errors[0].message;
}

TEST(Validation, UnknownRecordedOutputIsConfigInvalid) {
    ScratchDir dir;
    write_text_file(dir.path() / "source.json", "{}");
    const auto result =
        validate_text(dir.path(), config(std::string(kSourceInput) +
                                         "steps:\n"
                                         "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"
                                         "outputs: { record: [result, resulted] }\n"));
    EXPECT_FALSE(result.workflow.has_value());
    const auto errors = result.errors();
    ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
    EXPECT_EQ(errors[0].code, ErrorCode::CONFIG_INVALID);
    EXPECT_EQ(errors[0].location.pointer, "/outputs/record/1");
}

TEST(Validation, ValidConfigurationResolvesEverything) {
    ScratchDir dir;
    write_text_file(dir.path() / "obs.csv", praenora::test::observations_csv());
    praenora::WorkflowOptions options;
    options.seed = 42;
    const auto result =
        validate_text(dir.path(),
                      config("inputs:\n"
                             "  source: { source: observation-csv, path: obs.csv }\n"
                             "steps:\n"
                             "  b: { module: { name: fx-slow, version: \"0.1.0\" } }\n"
                             "  a:\n"
                             "    module: { name: fx-echo, version: \"0.1.0\" }\n"
                             "    timeouts: { invoke: 2s }\n"
                             "    log_level: debug\n",
                             "test.chain"),
                      options);
    ASSERT_TRUE(result.workflow.has_value()) << result.report.to_text();
    EXPECT_TRUE(result.report.ok());
    const auto& workflow = *result.workflow;
    EXPECT_EQ(workflow.definition->name, "test.chain");
    EXPECT_EQ(workflow.graph.schedule, (std::vector<std::string>{"a", "b"}));
    ASSERT_EQ(workflow.steps.size(), 2u);
    const auto& a = workflow.steps[0];
    const auto& b = workflow.steps[1];
    EXPECT_EQ(a.id, "a");
    EXPECT_EQ(b.id, "b");
    EXPECT_EQ(a.capability->id, "test.echo");
    EXPECT_EQ(a.manifest->name, "fx-echo");
    EXPECT_EQ(b.manifest->name, "fx-slow");
    EXPECT_EQ(a.manifest->manifest_path, praenora::test::fixture_manifest("fx-echo"));
    EXPECT_EQ(a.library_sha256, praenora::detail::sha256_file_hex(a.manifest->library_path));
    EXPECT_EQ(a.seed, praenora::detail::derive_step_seed(42, "a"));
    EXPECT_EQ(b.seed, praenora::detail::derive_step_seed(42, "b"));
    EXPECT_EQ(a.timeouts.invoke.count(), 2'000'000'000);
    ASSERT_TRUE(a.log_level.has_value());
    EXPECT_EQ(*a.log_level, praenora::LogLevel::debug);
    EXPECT_EQ(a.parameters, json::object());
    // Defaults from fx-slow's parameter schema are filled in.
    EXPECT_EQ(b.parameters,
              json::parse(R"({"sleep_ms":100,"finalize_sleep_ms":0,"cooperative":true})"));
    ASSERT_EQ(workflow.sources.count("source"), 1u);
    EXPECT_TRUE(workflow.sources.at("source")->replay());
    EXPECT_EQ(workflow.config.seed, 42u);
    EXPECT_EQ(workflow.config.record, (std::vector<std::string>{"result"}));
    EXPECT_EQ(workflow.fingerprint.size(), 64u);
    EXPECT_EQ(workflow.fingerprint, praenora::detail::sha256_hex(praenora::detail::canonical_json(
                                        workflow.reproducibility)));
}

TEST(Validation, ConfigSearchPathsAreSearched) {
    ScratchDir dir;
    // fx-echo copied below the configuration, under another name the caller's paths do not have.
    const fs::path module_dir = dir.path() / "mods" / "fx-copy";
    json manifest =
        json::parse(praenora::detail::read_file_bytes(praenora::test::fixture_manifest("fx-echo")));
    const std::string library = manifest.at("library").at("path").get<std::string>();
    fs::create_directories(module_dir);
    const fs::path library_file =
        praenora::test::fixture_module_dir("fx-echo") / praenora::test::utf8_path(library);
    fs::copy_file(library_file, module_dir / praenora::test::utf8_path(library));
    manifest["module"]["name"] = "fx-copy";
    write_text_file(module_dir / "fx-copy.pmm.json", manifest.dump(2));
    write_text_file(dir.path() / "source.json", "{}");
    const auto result = validate_text(
        dir.path(), config(std::string(kSourceInput) +
                           "modules: { search_paths: [mods] }\n"
                           "steps:\n"
                           "  a: { module: { name: fx-copy, version: \"0.1.0\" } }\n"));
    ASSERT_TRUE(result.workflow.has_value()) << result.report.to_text();
    EXPECT_EQ(result.workflow->steps.at(0).manifest->manifest_path,
              module_dir / "fx-copy.pmm.json");
}

TEST(Validation, UnreadableWorkflowVersionIsNotLookedUp) {
    // A definition at version 0.0.0, the value an unreadable version would otherwise stand for.
    ScratchDir catalog;
    write_text_file(catalog.path() / "workflows" / "test.zero" / "0.0.0.json", R"({
  "kind": "workflow-definition",
  "format_version": "0.1.0",
  "name": "test.zero",
  "version": "0.0.0",
  "status": "stable",
  "summary": "A definition at version 0.0.0 (tests).",
  "steps": { "z": { "capability": "test.echo", "capability_version": "0.1.0" } },
  "connections": [ { "from": "z.out", "to": "outputs.result" } ],
  "outputs": { "result": { "type": "test.blob", "type_version": "0.1.0" } }
}
)");
    const auto context =
        praenora::test::make_test_context({PRAENORA_TEST_MODULES_DIR}, {catalog.path()});
    ScratchDir dir;
    write_text_file(dir.path() / "source.json", "{}");
    // The version matches the pattern of a semantic version but does not fit in 64 bits.
    const auto result =
        validate_text(dir.path(),
                      config(std::string(kSourceInput) +
                                 "steps:\n"
                                 "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n",
                             "test.zero", "99999999999999999999.0.0"),
                      {}, *context);
    EXPECT_FALSE(result.workflow.has_value());
    const auto errors = result.errors();
    ASSERT_EQ(errors.size(), 1u) << result.report.to_text();
    EXPECT_EQ(errors[0].code, ErrorCode::CONFIG_INVALID);
    EXPECT_EQ(errors[0].location.pointer, "/workflow/version");
}

TEST(Validation, ConfiguredSearchPathProblemsAreWarnedOnce) {
    ScratchDir dir;
    write_text_file(dir.path() / "source.json", "{}");
    write_text_file(dir.path() / "plain.txt", "not a directory");
    const auto result = validate_text(
        dir.path(), config(std::string(kSourceInput) +
                           "modules: { search_paths: [absent, plain.txt] }\n"
                           "steps:\n"
                           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"));
    ASSERT_TRUE(result.workflow.has_value()) << result.report.to_text(); // warnings only
    std::vector<std::string> warnings;
    for (const auto& diagnostic : result.report.diagnostics) {
        if (diagnostic.severity == Severity::warning && diagnostic.code == ErrorCode::NOT_FOUND) {
            warnings.push_back(diagnostic.message);
        }
    }
    ASSERT_EQ(warnings.size(), 2u) << result.report.to_text();
    for (const char* name : {"absent", "plain.txt"}) {
        const std::string path = "'" + path_to_utf8(dir.path() / name) + "'";
        int mentions = 0;
        for (const auto& message : warnings) {
            mentions += contains(message, path) ? 1 : 0;
        }
        EXPECT_EQ(mentions, 1) << name << "\n" << result.report.to_text();
    }
}

TEST(Validation, RowsBeforeTheRunStartAreAWarningAndTheWorkflowStillValidates) {
    ScratchDir dir;
    const fs::path data = dir.path() / "data" / "obs.csv";
    // Lines 2 and 3 are before the default start of 0, line 4 is at it.
    write_text_file(data, "series_id,variable_id,time,value\n"
                          "S1,x,-3.0,1\n"
                          "S1,x,-0.5,2\n"
                          "S1,x,0.0,3\n"
                          "S1,x,1.5,4\n");
    const auto result = validate_text(
        dir.path(), config("inputs:\n"
                           "  source: { source: observation-csv, path: data/obs.csv }\n"
                           "steps:\n"
                           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"));
    ASSERT_TRUE(result.workflow.has_value()) << result.report.to_text();
    EXPECT_TRUE(result.errors().empty());
    std::vector<Diagnostic> warnings;
    for (const Diagnostic& diagnostic : result.report.diagnostics) {
        if (diagnostic.severity == Severity::warning) {
            warnings.push_back(diagnostic);
        }
    }
    ASSERT_EQ(warnings.size(), 1u) << result.report.to_text();
    EXPECT_EQ(warnings[0].code, ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(warnings[0].location.file, path_to_utf8(data));
    EXPECT_EQ(warnings[0].location.line, 2);
    EXPECT_TRUE(contains(warnings[0].message, "input 'source'")) << warnings[0].message;
    EXPECT_TRUE(contains(warnings[0].message, "2 of 4 rows")) << warnings[0].message;
    // The counts go to the run record's execution.inputs, and are not in the hashed section.
    EXPECT_EQ(result.workflow->input_counts,
              json::parse(R"({"source":{"rows_total":4,"rows_before_start":2}})"));
    EXPECT_FALSE(
        result.workflow->reproducibility.at("inputs").at("source").contains("rows_total"));
    EXPECT_FALSE(
        result.workflow->reproducibility.at("inputs").at("source").contains("rows_before_start"));
}

TEST(Validation, ARunStartAtTheFirstRowLeavesNoWarning) {
    ScratchDir dir;
    write_text_file(dir.path() / "data" / "obs.csv", "series_id,variable_id,time,value\n"
                                                     "S1,x,-3.0,1\n"
                                                     "S1,x,2.0,2\n");
    const auto result = validate_text(
        dir.path(), config("run: { time: { unit: d, start: -3.0, period: 1.0 } }\n"
                           "inputs:\n"
                           "  source: { source: observation-csv, path: data/obs.csv }\n"
                           "steps:\n"
                           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"));
    ASSERT_TRUE(result.workflow.has_value()) << result.report.to_text();
    EXPECT_TRUE(result.report.diagnostics.empty()) << result.report.to_text();
    EXPECT_EQ(result.workflow->input_counts,
              json::parse(R"({"source":{"rows_total":2,"rows_before_start":0}})"));
}

TEST(Validation, AJsonDocumentInputHasNoCounts) {
    ScratchDir dir;
    write_text_file(dir.path() / "source.json", "{}");
    const auto result = validate_text(
        dir.path(), config(std::string(kSourceInput) +
                           "steps:\n"
                           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"));
    ASSERT_TRUE(result.workflow.has_value()) << result.report.to_text();
    EXPECT_EQ(result.workflow->input_counts, json::object());
}

TEST(Validation, AWarningOfASourceDoesNotHideAnErrorElsewhere) {
    // The file's early row is a warning; the unknown module is the error, and both are reported.
    ScratchDir dir;
    write_text_file(dir.path() / "data" / "obs.csv", "series_id,variable_id,time,value\n"
                                                     "S1,x,-1.0,1\n");
    const auto result = validate_text(
        dir.path(), config("inputs:\n"
                           "  source: { source: observation-csv, path: data/obs.csv }\n"
                           "steps:\n"
                           "  a: { module: { name: no-such-module, version: \"0.1.0\" } }\n"));
    EXPECT_FALSE(result.workflow.has_value());
    EXPECT_FALSE(result.errors().empty());
    int warnings = 0;
    for (const Diagnostic& diagnostic : result.report.diagnostics) {
        warnings += diagnostic.severity == Severity::warning &&
                            diagnostic.code == ErrorCode::TARGET_DATA_INVALID
                        ? 1
                        : 0;
    }
    EXPECT_EQ(warnings, 1) << result.report.to_text();
}

TEST(Validation, EveryReplayInputHasItsOwnWarningAndCounts) {
    // Two observation-csv inputs: only "left" has early rows. Each input has its own key in the
    // counts, and the warning names the input that it concerns.
    ScratchDir dir;
    write_text_file(dir.path() / "left.csv", "series_id,variable_id,time,value\n"
                                             "S1,x,-1.0,1\n"
                                             "S1,x,2.0,2\n"
                                             "S1,x,-4.0,3\n");
    write_text_file(dir.path() / "right.csv", "series_id,variable_id,time,value\n"
                                              "S1,y,0.5,1\n"
                                              "S1,y,1.5,2\n");
    const auto result = validate_text(
        dir.path(), config("inputs:\n"
                           "  left: { source: observation-csv, path: left.csv }\n"
                           "  right: { source: observation-csv, path: right.csv }\n"
                           "steps:\n"
                           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"
                           "  b: { module: { name: fx-echo, version: \"0.1.0\" } }\n",
                           "test.two-sources"));
    ASSERT_TRUE(result.workflow.has_value()) << result.report.to_text();
    std::vector<Diagnostic> warnings;
    for (const Diagnostic& diagnostic : result.report.diagnostics) {
        if (diagnostic.severity == Severity::warning) {
            warnings.push_back(diagnostic);
        }
    }
    ASSERT_EQ(warnings.size(), 1u) << result.report.to_text();
    EXPECT_TRUE(contains(warnings[0].message, "input 'left'")) << warnings[0].message;
    EXPECT_TRUE(contains(warnings[0].message, "2 of 3 rows")) << warnings[0].message;
    EXPECT_EQ(warnings[0].location.file, path_to_utf8(dir.path() / "left.csv"));
    EXPECT_EQ(warnings[0].location.line, 2);
    EXPECT_EQ(result.workflow->input_counts,
              json::parse(R"({"left":{"rows_total":3,"rows_before_start":2},
                              "right":{"rows_total":2,"rows_before_start":0}})"));
}

TEST(Validation, AWarningPerInputWhenBothHaveEarlyRows) {
    ScratchDir dir;
    write_text_file(dir.path() / "left.csv", "series_id,variable_id,time,value\n"
                                             "S1,x,-1.0,1\n");
    write_text_file(dir.path() / "right.csv", "series_id,variable_id,time,value\n"
                                              "S1,y,-2.0,1\n"
                                              "S1,y,-3.0,2\n");
    const auto result = validate_text(
        dir.path(), config("inputs:\n"
                           "  left: { source: observation-csv, path: left.csv }\n"
                           "  right: { source: observation-csv, path: right.csv }\n"
                           "steps:\n"
                           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"
                           "  b: { module: { name: fx-echo, version: \"0.1.0\" } }\n",
                           "test.two-sources"));
    ASSERT_TRUE(result.workflow.has_value()) << result.report.to_text();
    int left = 0;
    int right = 0;
    for (const Diagnostic& diagnostic : result.report.diagnostics) {
        left += contains(diagnostic.message, "input 'left'") ? 1 : 0;
        right += contains(diagnostic.message, "input 'right'") ? 1 : 0;
    }
    EXPECT_EQ(left, 1);
    EXPECT_EQ(right, 1);
}

TEST(Validation, AllRowsBeforeTheStartGiveOneWarningAndCountsOfTwoOfTwo) {
    ScratchDir dir;
    write_text_file(dir.path() / "data" / "obs.csv", "series_id,variable_id,time,value\n"
                                                     "S1,x,-3.0,1\n"
                                                     "S1,x,-0.5,2\n");
    const auto result = validate_text(
        dir.path(), config("inputs:\n"
                           "  source: { source: observation-csv, path: data/obs.csv }\n"
                           "steps:\n"
                           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"));
    ASSERT_TRUE(result.workflow.has_value()) << result.report.to_text();
    ASSERT_EQ(result.report.diagnostics.size(), 1u) << result.report.to_text();
    EXPECT_EQ(result.report.diagnostics[0].severity, Severity::warning);
    EXPECT_TRUE(contains(result.report.diagnostics[0].message, "2 of 2 rows"))
        << result.report.diagnostics[0].message;
    EXPECT_EQ(result.workflow->input_counts,
              json::parse(R"({"source":{"rows_total":2,"rows_before_start":2}})"));
}
