// The contract checks of `praenora modules check` (Runtime::check_module()): their ids and order,
// that the example modules pass them, how fixture modules that break the contract fail the check
// at fault and skip the checks that depend on it, test vectors with expected outputs and a
// tolerance, and the report document.
#include "api/module_check.hpp"
#include "core/semver.hpp"
#include "discovery/manifest.hpp"
#include "document/yaml_loader.hpp"
#include "document/schema_registry.hpp"

#include "instance_fixtures.hpp"
#include "loader/library_gate.hpp"
#include "loader/loaded_module.hpp"
#include "module_fixtures.hpp"
#include "provider_fixtures.hpp"
#include "test_support.hpp"
#include "validation_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/sdk/abi.h>
#include <praenora/sdk/version.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::CheckResult;
using praenora::ModuleCheckReport;
using praenora::Runtime;
using praenora::RuntimeOptions;
using praenora::test::fixture_manifest;
using praenora::test::ScratchDir;
using praenora::test::utf8_path;
using praenora::test::write_text_file;

namespace {

// Every check id, in the order the report lists them.
const std::vector<std::string> kCheckIds = {
    "manifest.schema",
    "manifest.semantics",
    "library.digest",
    "library.load",
    "library.entry_point",
    "api.header",
    "api.required_functions",
    "describe.schema",
    "describe.identity",
    "lifecycle.create_destroy",
    "lifecycle.full",
    "invoke.result_schema",
    "invoke.expected_outputs",
    "invoke.determinism",
    "invoke.malformed_input",
    "self_test",
    "unload",
};

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// The built-in catalog and the test catalog (test.echo), without the environment and without
// logging to standard error.
RuntimeOptions check_options() {
    RuntimeOptions options;
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    options.use_environment = false;
    options.logging.to_stderr = false;
    return options;
}

std::vector<std::string> ids_of(const ModuleCheckReport& report) {
    std::vector<std::string> ids;
    for (const CheckResult& check : report.checks) {
        ids.push_back(check.id);
    }
    return ids;
}

// The status of every check, by id.
std::map<std::string, std::string> statuses_of(const ModuleCheckReport& report) {
    std::map<std::string, std::string> statuses;
    for (const CheckResult& check : report.checks) {
        statuses[check.id] = check.status;
    }
    return statuses;
}

const CheckResult& check_of(const ModuleCheckReport& report, const std::string& id) {
    for (const CheckResult& check : report.checks) {
        if (check.id == id) {
            return check;
        }
    }
    throw std::runtime_error("the report has no check " + id);
}

// The report, one "<status> <id>: <message>" line per check, for failure messages.
std::string shown(const ModuleCheckReport& report) {
    std::string text;
    for (const CheckResult& check : report.checks) {
        text += check.status + " " + check.id + ": " + check.message + "\n";
    }
    return text;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Whether the environment keeps every module library loaded (the sanitizer test runs on macOS
// do), which leaves the unload check nothing to check.
bool keep_libraries_requested() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv is fine here: nothing changes the environment
#endif
    const char* value = std::getenv("PRAENORA_DEBUG_KEEP_LIBRARIES");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return value != nullptr && std::string(value) == "1";
}

std::string expected_unload_status() { return keep_libraries_requested() ? "skip" : "pass"; }

// Every check from `first` on is skipped because of `failed`.
void expect_skipped_from(const ModuleCheckReport& report, const std::string& first,
                         const std::string& failed) {
    bool skipping = false;
    for (const CheckResult& check : report.checks) {
        skipping = skipping || check.id == first;
        if (skipping) {
            EXPECT_EQ(check.status, "skip") << check.id << "\n" << shown(report);
            EXPECT_TRUE(contains(check.message, failed)) << check.id << ": " << check.message;
        }
    }
    EXPECT_TRUE(skipping) << "no check " << first;
}

} // namespace

TEST(ModuleCheck, ExampleModulesPassAllChecks) {
#ifndef PRAENORA_TEST_WITH_EXAMPLES
    GTEST_SKIP() << "the example modules are not built (PRAENORA_BUILD_EXAMPLES=OFF)";
#endif
    const Runtime runtime(check_options());
    for (const std::string name : {"example-estimator", "example-predictor"}) {
        SCOPED_TRACE(name);
        const fs::path manifest = utf8_path(PRAENORA_EXAMPLE_MODULES_DIR) / utf8_path(name) /
                                  utf8_path(name + ".pmm.json");
        // The vectors are the ones the manifest names (tests.vectors).
        const ModuleCheckReport report = runtime.check_module(utf8(manifest));
        EXPECT_EQ(ids_of(report), kCheckIds);
        EXPECT_TRUE(report.passed()) << shown(report);
        for (const CheckResult& check : report.checks) {
            std::string expected = "pass";
            if (check.id == "self_test") {
                expected = "skip"; // neither example module provides self_test
            } else if (check.id == "unload") {
                expected = expected_unload_status();
            }
            EXPECT_EQ(check.status, expected) << check.id << ": " << check.message;
            EXPECT_FALSE(check.message.empty()) << check.id;
        }
    }
}

TEST(ModuleCheck, IdentityMismatchFailsDescribeIdentity) {
    const Runtime runtime(check_options());
    const ModuleCheckReport report =
        runtime.check_module(utf8(fixture_manifest("fx-identity-mismatch")));
    EXPECT_EQ(ids_of(report), kCheckIds);
    EXPECT_FALSE(report.passed());
    const auto statuses = statuses_of(report);
    for (const char* id :
         {"manifest.schema", "manifest.semantics", "library.digest", "library.load",
          "library.entry_point", "api.header", "api.required_functions", "describe.schema"}) {
        EXPECT_EQ(statuses.at(id), "pass") << id << "\n" << shown(report);
    }
    const CheckResult& identity = check_of(report, "describe.identity");
    EXPECT_EQ(identity.status, "fail");
    // describe() reports version 9.9.9; the manifest says 0.1.0.
    EXPECT_TRUE(contains(identity.message, "9.9.9")) << identity.message;
    EXPECT_TRUE(contains(identity.message, "0.1.0")) << identity.message;
    expect_skipped_from(report, "lifecycle.create_destroy", "describe.identity");
}

TEST(ModuleCheck, DescribeIdentityNamesTheVersionsItCompares) {
    const Runtime runtime(check_options());
    const ModuleCheckReport report = runtime.check_module(utf8(fixture_manifest("fx-echo")));
    const CheckResult& identity = check_of(report, "describe.identity");
    EXPECT_EQ(identity.status, "pass") << shown(report);
    EXPECT_EQ(identity.message, "describe() agrees with the manifest: fx-echo@0.1.0, ABI 0.0, "
                                "SDK " PRAENORA_SDK_VERSION_STRING
                                ", PMP " PRAENORA_PMP_VERSION_STRING ", 1 capability");
}

TEST(ModuleCheck, NegativeZeroAbiInDescribePassesDescribeIdentity) {
    // describe() writes the ABI 0.0 as -0 and -0, which are 0 and 0.
    const Runtime runtime(check_options());
    const ModuleCheckReport report =
        runtime.check_module(utf8(fixture_manifest("fx-negative-zero-abi")));
    EXPECT_EQ(ids_of(report), kCheckIds);
    const auto statuses = statuses_of(report);
    for (const char* id : {"manifest.schema", "manifest.semantics", "library.digest",
                           "library.load", "library.entry_point", "api.header",
                           "api.required_functions", "describe.schema", "describe.identity"}) {
        EXPECT_EQ(statuses.at(id), "pass") << id << "\n" << shown(report);
    }
    EXPECT_EQ(check_of(report, "describe.identity").message,
              "describe() agrees with the manifest: fx-negative-zero-abi@0.1.0, ABI 0.0, "
              "SDK " PRAENORA_SDK_VERSION_STRING ", PMP " PRAENORA_PMP_VERSION_STRING
              ", 1 capability");
}

TEST(ModuleCheck, AnotherAbiMinorInTheTableHeaderPassesTheLoadingChecks) {
    // The table's header reports a newer or an older ABI minor than describe() and the manifest:
    // api.header shows the header's version and describe.identity the documents', and both pass.
    struct Case {
        const char* fixture;
        const char* header_abi;
        const char* document_abi;
    };
    const Runtime runtime(check_options());
    for (const Case& c : {Case{"fx-newer-header-minor", "0.7", "0.0"},
                          Case{"fx-older-header-minor", "0.0", "0.7"}}) {
        SCOPED_TRACE(c.fixture);
        const ModuleCheckReport report = runtime.check_module(utf8(fixture_manifest(c.fixture)));
        EXPECT_EQ(ids_of(report), kCheckIds);
        const auto statuses = statuses_of(report);
        for (const char* id : {"manifest.schema", "manifest.semantics", "library.digest",
                               "library.load", "library.entry_point", "api.header",
                               "api.required_functions", "describe.schema", "describe.identity"}) {
            EXPECT_EQ(statuses.at(id), "pass") << id << "\n" << shown(report);
        }
        EXPECT_EQ(check_of(report, "api.header").message,
                  std::string("praenora_module_get_api(0) returns a table of ABI ") + c.header_abi +
                      " with struct_size " + std::to_string(sizeof(praenora_module_api_v0)));
        EXPECT_EQ(check_of(report, "describe.identity").message,
                  std::string("describe() agrees with the manifest: ") + c.fixture +
                      "@0.1.0, ABI " + c.document_abi +
                      ", SDK " PRAENORA_SDK_VERSION_STRING ", PMP " PRAENORA_PMP_VERSION_STRING
                      ", 1 capability");
    }
}

TEST(ModuleCheck, SdkVersionMismatchFailsDescribeIdentity) {
    // The manifest a module had before its library was rebuilt against another SDK.
    const ScratchDir dir;
    const fs::path manifest = praenora::test::copy_fixture_module(
        "fx-echo", dir.path(), [](json& document) { document["sdk_version"] = "9.9.9"; });
    const Runtime runtime(check_options());
    const ModuleCheckReport report = runtime.check_module(utf8(manifest));
    EXPECT_EQ(ids_of(report), kCheckIds);
    EXPECT_FALSE(report.passed());
    const auto statuses = statuses_of(report);
    for (const char* id :
         {"manifest.schema", "manifest.semantics", "library.digest", "library.load",
          "library.entry_point", "api.header", "api.required_functions", "describe.schema"}) {
        EXPECT_EQ(statuses.at(id), "pass") << id << "\n" << shown(report);
    }
    const CheckResult& identity = check_of(report, "describe.identity");
    EXPECT_EQ(identity.status, "fail");
    EXPECT_TRUE(contains(identity.message,
                         "sdk_version (manifest 9.9.9, describe() " PRAENORA_SDK_VERSION_STRING
                         ")"))
        << identity.message;
    expect_skipped_from(report, "lifecycle.create_destroy", "describe.identity");
}

TEST(ModuleCheck, AbiMinorMismatchFailsDescribeIdentity) {
    // A manifest that names another ABI minor than the library reports.
    const ScratchDir dir;
    const fs::path manifest = praenora::test::copy_fixture_module(
        "fx-echo", dir.path(), [](json& document) { document["abi"]["minor"] = 7; });
    const Runtime runtime(check_options());
    const ModuleCheckReport report = runtime.check_module(utf8(manifest));
    EXPECT_EQ(ids_of(report), kCheckIds);
    EXPECT_FALSE(report.passed());
    const auto statuses = statuses_of(report);
    for (const char* id :
         {"manifest.schema", "manifest.semantics", "library.digest", "library.load",
          "library.entry_point", "api.header", "api.required_functions", "describe.schema"}) {
        EXPECT_EQ(statuses.at(id), "pass") << id << "\n" << shown(report);
    }
    const CheckResult& identity = check_of(report, "describe.identity");
    EXPECT_EQ(identity.status, "fail");
    EXPECT_TRUE(contains(identity.message, "abi.minor (manifest 7, describe() 0)"))
        << identity.message;
    expect_skipped_from(report, "lifecycle.create_destroy", "describe.identity");
}

TEST(ModuleCheck, AnotherPmpInDescribeFailsDescribeIdentity) {
    // The manifest is compatible with this runtime, but the library speaks PMP 0.2.0.
    const Runtime runtime(check_options());
    const ModuleCheckReport report = runtime.check_module(utf8(fixture_manifest("fx-newer-pmp")));
    EXPECT_EQ(ids_of(report), kCheckIds);
    EXPECT_FALSE(report.passed());
    const auto statuses = statuses_of(report);
    for (const char* id :
         {"manifest.schema", "manifest.semantics", "library.digest", "library.load",
          "library.entry_point", "api.header", "api.required_functions", "describe.schema"}) {
        EXPECT_EQ(statuses.at(id), "pass") << id << "\n" << shown(report);
    }
    const CheckResult& identity = check_of(report, "describe.identity");
    EXPECT_EQ(identity.status, "fail");
    EXPECT_TRUE(contains(identity.message, "pmp_version (manifest " PRAENORA_PMP_VERSION_STRING
                                           ", describe() 0.2.0)"))
        << identity.message;
    expect_skipped_from(report, "lifecycle.create_destroy", "describe.identity");
}

TEST(ModuleCheck, MissingEntryPointFailsAndSkipsDependents) {
    const Runtime runtime(check_options());
    const ModuleCheckReport report = runtime.check_module(utf8(fixture_manifest("fx-no-entry")));
    EXPECT_EQ(ids_of(report), kCheckIds);
    EXPECT_FALSE(report.passed());
    const auto statuses = statuses_of(report);
    for (const char* id :
         {"manifest.schema", "manifest.semantics", "library.digest", "library.load"}) {
        EXPECT_EQ(statuses.at(id), "pass") << id << "\n" << shown(report);
    }
    const CheckResult& entry = check_of(report, "library.entry_point");
    EXPECT_EQ(entry.status, "fail");
    EXPECT_TRUE(contains(entry.message, "praenora_module_get_api")) << entry.message;
    expect_skipped_from(report, "api.header", "library.entry_point");
}

TEST(ModuleCheck, NondeterministicModuleFailsDeterminism) {
    const Runtime runtime(check_options());
    const ModuleCheckReport report =
        runtime.check_module(utf8(fixture_manifest("fx-nondeterministic")));
    EXPECT_EQ(ids_of(report), kCheckIds);
    EXPECT_FALSE(report.passed());
    for (const CheckResult& check : report.checks) {
        std::string expected = "pass";
        if (check.id == "invoke.determinism") {
            expected = "fail";
        } else if (check.id == "invoke.expected_outputs" || check.id == "self_test") {
            expected = "skip"; // the vectors expect no outputs; the module has no self_test
        } else if (check.id == "unload") {
            expected = expected_unload_status();
        }
        EXPECT_EQ(check.status, expected) << check.id << ": " << check.message;
    }
    const CheckResult& determinism = check_of(report, "invoke.determinism");
    EXPECT_TRUE(contains(determinism.message, "two-invocations")) << determinism.message;
    EXPECT_TRUE(contains(determinism.message, "tick 0")) << determinism.message;
}

TEST(ModuleCheck, ExpectedOutputsUseTheVectorsTolerance) {
    // fx-echo copies its input to its output, so the expectation is the input, give or take the
    // tolerance.
    const auto vectors = [](double expected, double relative) {
        return json{{"kind", "module-test-vectors"},
                    {"format_version", "0.1.0"},
                    {"cases",
                     {{{"name", "echo"},
                       {"capability", {{"id", "test.echo"}, {"version", "0.1.0"}}},
                       {"ticks",
                        {{{"inputs", {{"in", {{"value", 1.0}}}}},
                          {"expect",
                           {{"outputs", {{"out", {{"value", expected}}}}},
                            {"tolerance", {{"relative", relative}}}}}}}}}}}}
            .dump(2);
    };
    ScratchDir dir;
    const Runtime runtime(check_options());
    const std::string manifest = utf8(fixture_manifest("fx-echo"));

    write_text_file(dir.path() / "within.json", vectors(1.0005, 1e-3));
    const ModuleCheckReport within =
        runtime.check_module(manifest, utf8(dir.path() / "within.json"));
    EXPECT_EQ(check_of(within, "invoke.expected_outputs").status, "pass") << shown(within);
    EXPECT_TRUE(within.passed()) << shown(within);

    write_text_file(dir.path() / "beyond.json", vectors(1.01, 1e-3));
    const ModuleCheckReport beyond =
        runtime.check_module(manifest, utf8(dir.path() / "beyond.json"));
    const CheckResult& expected = check_of(beyond, "invoke.expected_outputs");
    EXPECT_EQ(expected.status, "fail") << shown(beyond);
    EXPECT_TRUE(contains(expected.message, "/out/value")) << expected.message;
    EXPECT_FALSE(beyond.passed());

    // Without vectors (fx-echo's manifest names none) the checks that need them are skipped.
    const ModuleCheckReport none = runtime.check_module(manifest);
    for (const char* id :
         {"invoke.result_schema", "invoke.expected_outputs", "invoke.determinism"}) {
        EXPECT_EQ(check_of(none, id).status, "skip") << id << "\n" << shown(none);
        EXPECT_TRUE(contains(check_of(none, id).message, "vectors")) << shown(none);
    }
    EXPECT_EQ(check_of(none, "invoke.malformed_input").status, "pass") << shown(none);
    EXPECT_TRUE(none.passed()) << shown(none);
}

TEST(ModuleCheck, InvalidVectorsFailWhenNoInstanceCanBeCreated) {
    // fx-needs-parameter's parameters_schema requires a parameter, so no instance can be created
    // without vectors; its vectors name a capability it does not implement.
    const Runtime runtime(check_options());
    const std::string manifest = utf8(fixture_manifest("fx-needs-parameter"));
    const ModuleCheckReport report = runtime.check_module(manifest);
    EXPECT_FALSE(report.passed()) << shown(report);
    const CheckResult& results = check_of(report, "invoke.result_schema");
    EXPECT_EQ(results.status, "fail") << shown(report);
    EXPECT_TRUE(contains(results.message, "test.nothing@0.1.0")) << results.message;
    const CheckResult& lifecycle = check_of(report, "lifecycle.create_destroy");
    EXPECT_EQ(lifecycle.status, "skip") << shown(report);
    EXPECT_TRUE(contains(lifecycle.message, "the test vectors are invalid")) << lifecycle.message;

    // Vectors whose parameters the module's parameters_schema refuses.
    ScratchDir dir;
    write_text_file(dir.path() / "vectors.json",
                    json{{"kind", "module-test-vectors"},
                         {"format_version", "0.1.0"},
                         {"cases",
                          {{{"capability", {{"id", "test.echo"}, {"version", "0.1.0"}}},
                            {"parameters", {{"gain", "high"}}},
                            {"ticks", {{{"inputs", json::object()}}}}}}}}
                        .dump(2));
    const ModuleCheckReport refused =
        runtime.check_module(manifest, utf8(dir.path() / "vectors.json"));
    EXPECT_FALSE(refused.passed()) << shown(refused);
    const CheckResult& refusal = check_of(refused, "invoke.result_schema");
    EXPECT_EQ(refusal.status, "fail") << shown(refused);
    EXPECT_TRUE(contains(refusal.message, "/cases/0/parameters/gain")) << refusal.message;
}

TEST(ModuleCheck, SelfTestReportDecidesTheCheck) {
    const Runtime runtime(check_options());
    const ModuleCheckReport passing = runtime.check_module(utf8(fixture_manifest("fx-self-test")));
    const CheckResult& passed = check_of(passing, "self_test");
    EXPECT_EQ(passed.status, "pass") << shown(passing);
    EXPECT_TRUE(contains(passed.message, "self_test() passed (1 checks)")) << passed.message;

    const ModuleCheckReport failing =
        runtime.check_module(utf8(fixture_manifest("fx-self-test-fails")));
    const CheckResult& failed = check_of(failing, "self_test");
    EXPECT_EQ(failed.status, "fail") << shown(failing);
    EXPECT_TRUE(contains(failed.message, "tables: wrong value")) << failed.message;
    EXPECT_FALSE(failing.passed());
}

TEST(ModuleCheck, ModuleAcceptingMalformedInputFailsTheCheck) {
    // fx-noop answers every invocation without looking at it, "{" included.
    const Runtime runtime(check_options());
    const ModuleCheckReport report = runtime.check_module(utf8(fixture_manifest("fx-noop")));
    const CheckResult& malformed = check_of(report, "invoke.malformed_input");
    EXPECT_EQ(malformed.status, "fail") << shown(report);
    EXPECT_TRUE(contains(malformed.message, "returned OK")) << malformed.message;
    EXPECT_FALSE(report.passed());
}

TEST(ModuleCheck, UnresponsiveCallSkipsLaterChecksAndFailsUnload) {
    // fx-hang blocks in invoke, ignoring cancellation, until the test.gate service opens: with
    // short deadlines its first invocation never returns. The gate is static, since the abandoned
    // call keeps polling it until it opens.
    static praenora::test::TestGate gate;
    gate.close();
    const auto context = praenora::test::make_test_context();
    context->services.add(PRAENORA_TEST_GATE_SERVICE, 0, gate.table());
    ScratchDir dir;
    write_text_file(dir.path() / "vectors.json",
                    json{{"kind", "module-test-vectors"},
                         {"format_version", "0.1.0"},
                         {"cases",
                          {{{"capability", {{"id", "test.echo"}, {"version", "0.1.0"}}},
                            {"ticks", {{{"inputs", json::object()}}}}}}}}
                        .dump(2));
    praenora::detail::CheckDeadlines deadlines;
    deadlines.invoke = std::chrono::milliseconds(100);
    deadlines.cancel_grace = std::chrono::milliseconds(100);
    const ModuleCheckReport report = praenora::detail::check_module(
        fixture_manifest("fx-hang"), dir.path() / "vectors.json", *context, nullptr, deadlines);
    gate.open(); // the abandoned call returns now
    EXPECT_TRUE(praenora::test::wait_until_loadable("fx-hang"));
    EXPECT_FALSE(report.passed());
    const auto statuses = statuses_of(report);
    EXPECT_EQ(statuses.at("lifecycle.full"), "pass") << shown(report);
    const CheckResult& results = check_of(report, "invoke.result_schema");
    EXPECT_EQ(results.status, "fail") << shown(report);
    EXPECT_TRUE(contains(results.message, "unresponsive")) << results.message;
    EXPECT_EQ(statuses.at("invoke.expected_outputs"), "skip");
    EXPECT_EQ(statuses.at("invoke.determinism"), "skip");
    const CheckResult& malformed = check_of(report, "invoke.malformed_input");
    EXPECT_EQ(malformed.status, "skip") << shown(report);
    EXPECT_TRUE(contains(malformed.message, "an earlier call never returned")) << malformed.message;
    const CheckResult& unload = check_of(report, "unload");
    EXPECT_EQ(unload.status, "fail") << shown(report);
    EXPECT_TRUE(contains(unload.message, "never returned")) << unload.message;
}

TEST(ModuleCheck, UnknownCapabilityFailsManifestSemantics) {
    // Without the test catalog, test.echo is unknown.
    RuntimeOptions options = check_options();
    options.catalog_paths.clear();
    const Runtime runtime(options);
    const ModuleCheckReport report = runtime.check_module(utf8(fixture_manifest("fx-echo")));
    const CheckResult& semantics = check_of(report, "manifest.semantics");
    EXPECT_EQ(semantics.status, "fail") << shown(report);
    EXPECT_TRUE(contains(semantics.message, "test.echo@0.1.0")) << semantics.message;
    expect_skipped_from(report, "library.digest", "manifest.semantics");
}

TEST(ModuleCheck, ReportValidatesAgainstSchema) {
    const Runtime runtime(check_options());
    const auto& schema = praenora::detail::SchemaRegistry::builtin().for_kind(
        "module-check-report", praenora::detail::parse_semver("0.1.0"));
    // fx-no-entry's report holds every status: pass, fail and skip.
    for (const char* name : {"fx-echo", "fx-no-entry"}) {
        SCOPED_TRACE(name);
        const ModuleCheckReport report = runtime.check_module(utf8(fixture_manifest(name)));
        const json document = json::parse(report.to_json());
        const auto issues = schema.validate(document);
        EXPECT_TRUE(issues.empty()) << issues.front().pointer << ": " << issues.front().message;
        EXPECT_EQ(document.at("kind"), "module-check-report");
        EXPECT_EQ(document.at("format_version"), "0.1.0");
        EXPECT_EQ(document.at("passed").get<bool>(), report.passed());
        ASSERT_EQ(document.at("checks").size(), kCheckIds.size());
        for (std::size_t i = 0; i < kCheckIds.size(); ++i) {
            EXPECT_EQ(document.at("checks")[i].at("id"), kCheckIds[i]);
        }
    }
}

TEST(ModuleCheck, UnreadableFilesAreIoErrors) {
    // A manifest or vectors file that cannot be read is no failed check but an I/O error.
    ScratchDir dir;
    const Runtime runtime(check_options());
    const auto code_of = [](const auto& call) {
        try {
            call();
        } catch (const praenora::Error& error) {
            return error.code();
        }
        return praenora::ErrorCode::OK;
    };
    EXPECT_EQ(code_of([&] { runtime.check_module(utf8(dir.path() / "missing.pmm.json")); }),
              praenora::ErrorCode::IO_ERROR);
    EXPECT_EQ(code_of([&] {
                  runtime.check_module(utf8(fixture_manifest("fx-echo")),
                                       utf8(dir.path() / "missing-vectors.json"));
              }),
              praenora::ErrorCode::IO_ERROR);
}

TEST(ModuleCheck, MalformedManifestFailsTheSchemaCheck) {
    ScratchDir dir;
    write_text_file(dir.path() / "broken.pmm.json", "{\"kind\": \"module-manifest\",");
    const Runtime runtime(check_options());
    const ModuleCheckReport report = runtime.check_module(utf8(dir.path() / "broken.pmm.json"));
    EXPECT_EQ(ids_of(report), kCheckIds);
    EXPECT_EQ(check_of(report, "manifest.schema").status, "fail") << shown(report);
    expect_skipped_from(report, "manifest.semantics", "manifest.schema");
}

// The services a module declares are resolved on the runtime's provider search paths, and their
// providers loaded, as part of library.load; the module's instances are then served them as in a
// run. fx-service's vectors require test.echo (require_echo) and expect it to echo the input.
TEST(ModuleCheck, ServicesAreServedDuringTheChecks) {
    ScratchDir dir;
    const json vectors = {
        {"kind", "module-test-vectors"},
        {"format_version", "0.1.0"},
        {"cases",
         {{{"name", "echo"},
           {"capability", {{"id", "test.echo"}, {"version", "0.1.0"}}},
           {"parameters", {{"require_echo", true}}},
           {"ticks",
            {{{"inputs", {{"in", {{"value", 1}}}}},
              {"expect",
               {{"outputs",
                 {{"out",
                   {{"echo", {{"value", 1}}}, {"found", {{"test.echo@0", true}}}}}}}}}}}}}}}};
    write_text_file(dir.path() / "vectors.json", vectors.dump());
    const auto context = praenora::test::make_provider_context({"test-echo"});
    const ModuleCheckReport report =
        praenora::detail::check_module(fixture_manifest("fx-service"), dir.path() / "vectors.json",
                                       *context, nullptr);
    EXPECT_EQ(ids_of(report), kCheckIds);
    EXPECT_TRUE(report.passed()) << shown(report);
    for (const char* id : {"library.load", "lifecycle.full", "invoke.result_schema",
                           "invoke.expected_outputs", "invoke.determinism"}) {
        EXPECT_EQ(check_of(report, id).status, "pass") << id << "\n" << shown(report);
    }
    const std::string& load = check_of(report, "library.load").message;
    EXPECT_TRUE(contains(load, "test.echo@0 from provider test-echo@0.1.0")) << load;
    EXPECT_TRUE(contains(load, "test.other@1 not available (optional)")) << load;
}

TEST(ModuleCheck, UnavailableServiceFailsLibraryLoad) {
    const auto context = praenora::test::make_provider_context({});
    const ModuleCheckReport report = praenora::detail::check_module(
        fixture_manifest("fx-service"), std::nullopt, *context, nullptr);
    EXPECT_EQ(ids_of(report), kCheckIds);
    EXPECT_FALSE(report.passed());
    EXPECT_EQ(check_of(report, "library.digest").status, "pass") << shown(report);
    const CheckResult& load = check_of(report, "library.load");
    EXPECT_EQ(load.status, "fail");
    EXPECT_EQ(load.message.rfind("SERVICE_UNAVAILABLE: ", 0), 0u) << load.message;
    EXPECT_TRUE(contains(load.message, "test.echo@0.0")) << load.message;
    expect_skipped_from(report, "library.entry_point", "library.load");
}

TEST(ModuleCheck, ProviderThatDoesNotLoadFailsLibraryLoad) {
    const auto context = praenora::test::make_provider_context({"test-start-fails"});
    const ModuleCheckReport report = praenora::detail::check_module(
        fixture_manifest("fx-service"), std::nullopt, *context, nullptr);
    const CheckResult& load = check_of(report, "library.load");
    EXPECT_EQ(load.status, "fail") << shown(report);
    EXPECT_EQ(load.message.rfind("LIBRARY_LOAD_FAILED: ", 0), 0u) << load.message;
    EXPECT_TRUE(contains(load.message, "the engine cannot start")) << load.message;
    expect_skipped_from(report, "library.entry_point", "library.load");
}

TEST(ModuleCheck, SimulatingModulePassesWithTheSimulationService) {
    // simulating-module's own test vectors run plans, a kernel and a numerical failure through
    // the simulation service of the fixture provider test-simulation.
    const auto context = praenora::test::make_provider_context({"test-simulation"});
    const ModuleCheckReport report = praenora::detail::check_module(
        fixture_manifest("simulating-module"), std::nullopt, *context, nullptr);
    EXPECT_EQ(ids_of(report), kCheckIds);
    EXPECT_TRUE(report.passed()) << shown(report);
    for (const char* id : {"library.load", "lifecycle.full", "invoke.result_schema",
                           "invoke.expected_outputs", "invoke.determinism"}) {
        EXPECT_EQ(check_of(report, id).status, "pass") << id << "\n" << shown(report);
    }
    EXPECT_EQ(check_of(report, "unload").status, expected_unload_status()) << shown(report);
    const std::string& load = check_of(report, "library.load").message;
    EXPECT_TRUE(contains(load, "praenora.simulation@0 from provider test-simulation@0.1.0"))
        << load;

    // Without the provider, the module cannot be checked.
    const auto without = praenora::test::make_provider_context({});
    const ModuleCheckReport refused = praenora::detail::check_module(
        fixture_manifest("simulating-module"), std::nullopt, *without, nullptr);
    EXPECT_EQ(check_of(refused, "library.load").status, "fail") << shown(refused);
    EXPECT_EQ(check_of(refused, "library.load").message.rfind("SERVICE_UNAVAILABLE: ", 0), 0u)
        << shown(refused);
}

TEST(ModuleCheck, ConvertingModulePassesWithTheConversionService) {
    // converting-module's own test vectors convert both ways through the model conversion service
    // of the fixture provider test-conversion, and simulate a converted model through the
    // simulation service of test-simulation, which the module can use.
    const auto context =
        praenora::test::make_provider_context({"test-conversion", "test-simulation"});
    const ModuleCheckReport report = praenora::detail::check_module(
        fixture_manifest("converting-module"), std::nullopt, *context, nullptr);
    EXPECT_EQ(ids_of(report), kCheckIds);
    EXPECT_TRUE(report.passed()) << shown(report);
    for (const char* id : {"library.load", "lifecycle.full", "invoke.result_schema",
                           "invoke.expected_outputs", "invoke.determinism"}) {
        EXPECT_EQ(check_of(report, id).status, "pass") << id << "\n" << shown(report);
    }
    const std::string& load = check_of(report, "library.load").message;
    EXPECT_TRUE(contains(load, "praenora.model-conversion@0 from provider test-conversion@0.1.0"))
        << load;
    EXPECT_TRUE(contains(load, "praenora.simulation@0 from provider test-simulation@0.1.0"))
        << load;

    // Without the conversion provider, the module cannot be checked.
    const auto without = praenora::test::make_provider_context({"test-simulation"});
    const ModuleCheckReport refused = praenora::detail::check_module(
        fixture_manifest("converting-module"), std::nullopt, *without, nullptr);
    EXPECT_EQ(check_of(refused, "library.load").status, "fail") << shown(refused);
    EXPECT_EQ(check_of(refused, "library.load").message.rfind("SERVICE_UNAVAILABLE: ", 0), 0u)
        << shown(refused);
    EXPECT_TRUE(contains(check_of(refused, "library.load").message, "praenora.model-conversion"))
        << shown(refused);
}

namespace {

// A call into fixture module `name`'s library that was abandoned, as an unresponsive invoke of
// step 'a' of workflow test.chain@0.1.0 leaves it: an exclusive lease on the library's gate,
// marked abandoned, held with the library (as the abandoned call's task holds it) until reset(),
// when the call returns.
class AbandonedCall {
public:
    explicit AbandonedCall(const char* name) : module_(praenora::test::load_fixture_module(name)) {
        using praenora::detail::LibraryGate;
        LibraryGate::Grant grant = module_->gate().acquire(
            LibraryGate::Access::exclusive,
            praenora::detail::LibraryCall{std::string(name) + "@0.1.0", "invoke",
                                          "step 'a' of workflow test.chain@0.1.0"});
        if (grant.lease == nullptr) {
            throw std::runtime_error("the library's gate granted no lease");
        }
        lease_ = std::move(grant.lease);
        lease_->abandon();
    }

    void reset() {
        lease_.reset();
        module_.reset();
    }

private:
    std::shared_ptr<praenora::detail::LoadedModule> module_;
    std::shared_ptr<praenora::detail::LibraryGate::Lease> lease_;
};

} // namespace

TEST(ModuleCheck, LibraryWithAnAbandonedCallFailsLibraryLoad) {
    // A call into fx-echo's library was abandoned and may still be running: the checks do not
    // load the library (library.load fails, naming that call) and call nothing in it.
    const Runtime runtime(check_options());
    AbandonedCall abandoned("fx-echo");
    const ModuleCheckReport report = runtime.check_module(utf8(fixture_manifest("fx-echo")));
    const CheckResult& load = check_of(report, "library.load");
    EXPECT_EQ(load.status, "fail") << shown(report);
    EXPECT_EQ(load.message.rfind("MODULE_UNRESPONSIVE: ", 0), 0u) << load.message;
    EXPECT_TRUE(contains(load.message, "invoke by step 'a' of workflow test.chain@0.1.0"))
        << load.message;
    const auto statuses = statuses_of(report);
    EXPECT_EQ(statuses.at("library.entry_point"), "skip");
    EXPECT_EQ(statuses.at("lifecycle.full"), "skip");
    EXPECT_FALSE(report.passed());

    abandoned.reset(); // the abandoned call returns
    EXPECT_TRUE(runtime.check_module(utf8(fixture_manifest("fx-echo"))).passed());
}

TEST(Runtime, ValidateLoadRefusesALibraryWithAnAbandonedCall) {
    // validate --load loads every module as a run would, and so reports the library of fx-echo
    // as unusable while an abandoned call may still be running in it.
    ScratchDir dir;
    write_text_file(dir.path() / "source.json", "{\"value\": 1}");
    write_text_file(dir.path() / "workflow.yaml",
                    "kind: workflow-config\n"
                    "format_version: \"0.1.0\"\n"
                    "workflow: { name: test.single, version: \"0.1.0\" }\n"
                    "run: { ticks: 1 }\n"
                    "inputs:\n"
                    "  source: { source: json-document, path: source.json }\n"
                    "steps:\n"
                    "  a:\n"
                    "    module: { name: fx-echo, version: \"0.1.0\" }\n"
                    "    parameters: {}\n"
                    "outputs: { directory: out }\n");
    RuntimeOptions options = check_options();
    options.module_paths = {PRAENORA_TEST_MODULES_DIR};
    const Runtime runtime(options);
    praenora::ValidateOptions load;
    load.load = true;
    const std::string file = utf8(dir.path() / "workflow.yaml");
    ASSERT_TRUE(runtime.validate(file, load).ok());

    AbandonedCall abandoned("fx-echo");
    const praenora::ValidationReport report = runtime.validate(file, load);
    ASSERT_FALSE(report.ok()) << report.to_text();
    const praenora::Diagnostic& error = *report.first_error();
    EXPECT_EQ(error.code, praenora::ErrorCode::MODULE_UNRESPONSIVE) << report.to_text();
    EXPECT_EQ(error.step, "a");
    EXPECT_EQ(error.location.pointer, "/library/path");
    EXPECT_EQ(json::parse(error.details_json).at("abandoned_call").at("by"),
              "step 'a' of workflow test.chain@0.1.0");

    abandoned.reset();
    EXPECT_TRUE(runtime.validate(file, load).ok());
}

// A draft-07 parameters_schema with keywords beside a "$ref" passes, with a note: draft-07 ignores
// them, a default among them. A 2020-12 one needs no note.
TEST(ModuleCheck, DefaultBesideReferenceInDraft07IsNoted) {
    const fs::path source = fixture_manifest("fx-echo");
    for (const bool modern : {false, true}) {
        SCOPED_TRACE(modern ? "2020-12" : "draft-07");
        ScratchDir dir;
        json document = praenora::detail::load_document_file(source).json;
        const fs::path library = praenora::detail::load_manifest(source).library_path;
        fs::copy_file(library, dir.path() / library.filename(), fs::copy_options::overwrite_existing);
        document["library"]["path"] = library.filename().u8string();
        document["parameters_schema"] =
            modern ? json::parse(R"({"$schema": "https://json-schema.org/draft/2020-12/schema",
                                     "type": "object", "$defs": {"n": {"type": "number"}},
                                     "properties": {"gain": {"$ref": "#/$defs/n", "default": 1}}})")
                   : json::parse(R"({"type": "object", "definitions": {"n": {"type": "number"}},
                                     "properties": {"gain": {"$ref": "#/definitions/n", "default": 1}}})");
        write_text_file(dir.path() / "module.pmm.json", document.dump(2));
        const Runtime runtime(check_options());
        const ModuleCheckReport report = runtime.check_module(utf8(dir.path() / "module.pmm.json"));
        const CheckResult& semantics = check_of(report, "manifest.semantics");
        EXPECT_EQ(semantics.status, "pass") << shown(report);
        EXPECT_EQ(contains(semantics.message, "warning: parameters_schema is draft-07"), !modern)
            << semantics.message;
        if (!modern) {
            EXPECT_TRUE(contains(semantics.message, "/properties/gain (default)")) << semantics.message;
        }
    }
}
