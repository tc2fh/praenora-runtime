// The protocol catalog: message types, capabilities and workflow definitions, embedded in the
// runtime (spec/catalog) or read from extra directories.
#include "catalog/catalog.hpp"
#include "core/semver.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"
#include "document/schema.hpp"

#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::detail::Catalog;
using praenora::detail::parse_semver;
using praenora::detail::SchemaIssue;

namespace {

const praenora::detail::SemVer kV010 = parse_semver("0.1.0");

bool has_entry(const Catalog& catalog, const std::string& kind, const std::string& id) {
    const auto entries = catalog.entries();
    return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.kind == kind && entry.id == id && entry.version == "0.1.0";
    });
}

const praenora::detail::CompiledSchema& message_schema(const Catalog& catalog,
                                                       const std::string& id) {
    const auto* type = catalog.find_message_type(id, kV010);
    if (type == nullptr || !type->compiled) {
        throw std::runtime_error("no message type " + id);
    }
    return *type->compiled;
}

} // namespace

TEST(Catalog, BuiltinLoadsAndChecksClean) {
    const Catalog catalog = Catalog::load({});
    const auto report = catalog.check();
    EXPECT_TRUE(report.ok()) << report.to_text();

    const auto* example = catalog.find_workflow("example.estimate-predict", kV010);
    ASSERT_NE(example, nullptr);
    EXPECT_EQ(example->status, "stable");
    EXPECT_EQ(example->steps.size(), 2u);
    EXPECT_EQ(example->connections.size(), 5u);
    const auto* draft = catalog.find_workflow("pmp.continuous-forecast", kV010);
    ASSERT_NE(draft, nullptr);
    EXPECT_EQ(draft->status, "draft");
    EXPECT_EQ(draft->steps.count("propose") + draft->steps.count("curate") +
                  draft->steps.count("predict"),
              3u);

    EXPECT_TRUE(has_entry(catalog, "workflow-definition", "example.estimate-predict"));
    EXPECT_TRUE(has_entry(catalog, "workflow-definition", "pmp.continuous-forecast"));
    EXPECT_TRUE(has_entry(catalog, "message-type", "pmp.observation-batch"));
    EXPECT_TRUE(has_entry(catalog, "message-type", "pmp.prediction-set"));
    EXPECT_TRUE(has_entry(catalog, "message-type", "example.state-estimate"));
    for (const char* id :
         {"pmp.model-set", "pmp.candidate-set", "pmp.replenishment-request", "pmp.assessment"}) {
        const auto* type = catalog.find_message_type(id, kV010);
        ASSERT_NE(type, nullptr) << id;
        EXPECT_EQ(type->status, "draft") << id;
    }
    for (const char* id : {"pmp.propose", "pmp.curate", "pmp.predict", "pmp.assess"}) {
        const auto* capability = catalog.find_capability(id, kV010);
        ASSERT_NE(capability, nullptr) << id;
        EXPECT_EQ(capability->status, "draft") << id;
    }
    const auto* estimate = catalog.find_capability("example.estimate", kV010);
    ASSERT_NE(estimate, nullptr);
    EXPECT_TRUE(estimate->inputs.at("observations").required);
    EXPECT_FALSE(estimate->inputs.at("prior").required);
    EXPECT_EQ(estimate->outputs.at("state").type, "example.state-estimate");

    // The test catalog is not part of the embedded one.
    EXPECT_EQ(catalog.find_workflow("test.single", kV010), nullptr);
    EXPECT_EQ(catalog.find_workflow("example.estimate-predict", parse_semver("0.2.0")), nullptr);
}

TEST(Catalog, ExtraDirectoryAddsTestEntries) {
    const Catalog catalog = Catalog::load({praenora::test::utf8_path(PRAENORA_TEST_CATALOG_DIR)});
    const auto report = catalog.check();
    EXPECT_TRUE(report.ok()) << report.to_text();
    EXPECT_NE(catalog.find_message_type("test.blob", kV010), nullptr);
    const auto* echo = catalog.find_capability("test.echo", kV010);
    ASSERT_NE(echo, nullptr);
    EXPECT_FALSE(echo->inputs.at("in").required);
    EXPECT_TRUE(echo->outputs.at("out").required);
    for (const char* name : {"test.single", "test.chain", "test.loop"}) {
        EXPECT_NE(catalog.find_workflow(name, kV010), nullptr) << name;
    }
    // The embedded entries are still there.
    EXPECT_NE(catalog.find_workflow("example.estimate-predict", kV010), nullptr);
    ASSERT_NE(catalog.raw_entry("test.loop", kV010), nullptr);
    EXPECT_EQ(catalog.raw_entry("test.loop", kV010)->at("kind"), "workflow-definition");
}

TEST(Catalog, DuplicateEntryIsError) {
    try {
        (void)Catalog::load({praenora::test::fixture_path("catalog-bad/duplicate")});
        FAIL() << "expected CATALOG_ENTRY_DUPLICATE";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::CATALOG_ENTRY_DUPLICATE);
        const std::string message = error.what();
        EXPECT_NE(message.find("test.dup@0.1.0"), std::string::npos) << message;
        EXPECT_NE(message.find("0.1.0.json"), std::string::npos) << message;
        EXPECT_NE(message.find("copy.json"), std::string::npos) << message;
    }
}

TEST(Catalog, EntryViolatingItsSchemaIsSchemaViolation) {
    // A test.loop workflow without connections is not a workflow definition.
    const auto dir = praenora::test::fixture_path("catalog-bad/schema-violation");
    try {
        (void)Catalog::load({dir});
        FAIL() << "expected SCHEMA_VIOLATION";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::SCHEMA_VIOLATION);
        EXPECT_NE(std::string(error.what()).find("broken.json"), std::string::npos) << error.what();
        EXPECT_NE(error.location().file.find("broken.json"), std::string::npos);
    }
}

// A message type may declare 2020-12 (with $defs and a reference), as long as AsyncAPI 3.0.0 can
// express it: the export inlines references and drops keywords beside them, and has no tuples.
TEST(Catalog, MessageTypeSchemaMayBe202012) {
    const Catalog catalog = Catalog::load({praenora::test::fixture_path("catalog-2020")});
    const auto* type = catalog.find_message_type("test.modern", kV010);
    ASSERT_NE(type, nullptr);
    ASSERT_NE(type->compiled, nullptr);
    EXPECT_TRUE(type->compiled->validate(json::parse(R"({"name": "x"})")).empty());
    EXPECT_FALSE(type->compiled->validate(json::parse(R"({"name": ""})")).empty());
    EXPECT_FALSE(type->compiled->validate(json::parse(R"({"name": 1})")).empty());
}

TEST(Catalog, MessageTypeSchemaBeyondAsyncApiIsSchemaViolation) {
    try {
        (void)Catalog::load({praenora::test::fixture_path("catalog-bad/asyncapi-subset")});
        FAIL() << "expected SCHEMA_VIOLATION";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::SCHEMA_VIOLATION);
        const std::string message = error.what();
        EXPECT_NE(message.find(": /properties/pair (prefixItems)"), std::string::npos) << message;
        EXPECT_NE(error.location().file.find("0.1.0.json"), std::string::npos);
        EXPECT_EQ(error.location().pointer, "/schema");
    }
}

TEST(Catalog, UnknownMessageTypeReferenceReported) {
    const Catalog catalog =
        Catalog::load({praenora::test::fixture_path("catalog-bad/unknown-message-type")});
    const auto report = catalog.check();
    ASSERT_FALSE(report.ok());
    ASSERT_EQ(report.diagnostics.size(), 1u) << report.to_text();
    const auto& diagnostic = report.diagnostics.front();
    EXPECT_EQ(diagnostic.code, ErrorCode::CATALOG_ENTRY_NOT_FOUND);
    EXPECT_NE(diagnostic.message.find("test.missing@0.1.0"), std::string::npos)
        << diagnostic.message;
    EXPECT_NE(diagnostic.message.find("test.orphan@0.1.0"), std::string::npos)
        << diagnostic.message;
    EXPECT_EQ(diagnostic.location.pointer, "/inputs/in/type");
}

TEST(Catalog, DigestIsStable) {
    const Catalog first = Catalog::load({});
    const Catalog second = Catalog::load({});
    const auto* a = first.find_workflow("example.estimate-predict", kV010);
    const auto* b = second.find_workflow("example.estimate-predict", kV010);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->digest.size(), 64u);
    EXPECT_EQ(a->digest, b->digest);
    EXPECT_EQ(a->digest, praenora::detail::sha256_hex(praenora::detail::canonical_json(a->raw)));
    const auto* type = first.find_message_type("pmp.observation-batch", kV010);
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->digest, second.find_message_type("pmp.observation-batch", kV010)->digest);
    EXPECT_NE(type->digest, a->digest);
}

TEST(Catalog, ObservationBatchSchemaAppliesSpecDefaults) {
    const Catalog catalog = Catalog::load({});
    const auto& schema = message_schema(catalog, "pmp.observation-batch");
    const json batch = {
        {"window", {{"start", 0.0}, {"end", 1.0}}},
        {"time_unit", "d"},
        {"observations",
         json::array({json::parse(R"({"series_id":"S","variable_id":"v","time":1,"value":2})")})}};
    std::vector<SchemaIssue> issues;
    const json filled = schema.apply_defaults(batch, issues);
    EXPECT_TRUE(issues.empty());
    const json& row = filled.at("observations").at(0);
    EXPECT_EQ(row.at("missing"), false);
    EXPECT_EQ(row.at("censoring"), "none");
    EXPECT_EQ(row.at("weight"), 1);

    // A null value needs a reason: a missing row, or a censored row with its limit.
    json missing = batch;
    missing["observations"][0]["value"] = nullptr;
    EXPECT_FALSE(schema.validate(missing).empty());
    missing["observations"][0]["missing"] = true;
    EXPECT_TRUE(schema.validate(missing).empty());
    json censored = batch;
    censored["observations"][0]["value"] = nullptr;
    censored["observations"][0]["censoring"] = "left";
    EXPECT_FALSE(schema.validate(censored).empty());
    censored["observations"][0]["upper_limit"] = 0.5;
    EXPECT_TRUE(schema.validate(censored).empty());
}

TEST(Catalog, PredictionQuantileRequiredOnlyForQuantileKind) {
    const Catalog catalog = Catalog::load({});
    const auto& schema = message_schema(catalog, "pmp.prediction-set");
    const auto set = [](const json& row) {
        return json{{"issued_at", 3.0}, {"time_unit", "d"}, {"predictions", json::array({row})}};
    };
    json row = {{"series_id", "S"},
                {"variable_id", "v"},
                {"time", 4.0},
                {"value", 1.5},
                {"prediction_kind", "quantile"}};
    EXPECT_FALSE(schema.validate(set(row)).empty()); // quantile kind without quantile
    row["quantile"] = 0.9;
    EXPECT_TRUE(schema.validate(set(row)).empty());
    row["quantile"] = 1.0;
    EXPECT_FALSE(schema.validate(set(row)).empty()); // outside (0, 1)
    row["prediction_kind"] = "latent_mean";
    row["quantile"] = 0.9;
    EXPECT_FALSE(schema.validate(set(row)).empty()); // quantile on another kind
    row.erase("quantile");
    EXPECT_TRUE(schema.validate(set(row)).empty());
}

TEST(Catalog, SameIdInDifferentKindsIsNotDuplicate) {
    const Catalog catalog = Catalog::load({praenora::test::fixture_path("catalog-shared-id")});
    const auto report = catalog.check();
    EXPECT_TRUE(report.ok()) << report.to_text();
    EXPECT_NE(catalog.find_message_type("test.shared", kV010), nullptr);
    EXPECT_NE(catalog.find_capability("test.shared", kV010), nullptr);
    // Two kinds define test.shared@0.1.0, so the kind-less lookup is ambiguous.
    EXPECT_EQ(catalog.raw_entry("test.shared", kV010), nullptr);
    const auto* type = catalog.raw_entry("message-type", "test.shared", kV010);
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->at("kind"), "message-type");
    const auto* capability = catalog.raw_entry("capability-definition", "test.shared", kV010);
    ASSERT_NE(capability, nullptr);
    EXPECT_EQ(capability->at("kind"), "capability-definition");
    EXPECT_EQ(catalog.raw_entry("workflow-definition", "test.shared", kV010), nullptr);
    // An id@version defined by one kind only is found without the kind.
    ASSERT_NE(catalog.raw_entry("pmp.observation-batch", kV010), nullptr);
}

TEST(Catalog, UnknownWorkflowReferencesReported) {
    const Catalog catalog =
        Catalog::load({praenora::test::fixture_path("catalog-bad/unknown-references")});
    const auto report = catalog.check();
    ASSERT_EQ(report.diagnostics.size(), 3u) << report.to_text();
    for (const auto& diagnostic : report.diagnostics) {
        EXPECT_EQ(diagnostic.code, ErrorCode::CATALOG_ENTRY_NOT_FOUND) << diagnostic.message;
        EXPECT_NE(diagnostic.message.find("test.dangling@0.1.0"), std::string::npos);
    }
    const auto mentions = [&](const std::string& text, const std::string& pointer) {
        return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                           [&](const praenora::Diagnostic& d) {
                               return d.message.find(text) != std::string::npos &&
                                      d.location.pointer == pointer;
                           });
    };
    EXPECT_TRUE(mentions("unknown message type test.nowhere@0.1.0", "/inputs/source/type"));
    EXPECT_TRUE(mentions("unknown message type test.void@0.1.0", "/outputs/result/type"));
    EXPECT_TRUE(mentions("unknown capability test.nothing@0.1.0", "/steps/a/capability"));
}

TEST(Catalog, LocatesPointersInEntries) {
    const Catalog catalog = Catalog::load({praenora::test::fixture_path("catalog")});
    // An embedded entry is named by its resource path.
    const auto embedded = catalog.locate("workflow-definition", "example.estimate-predict", kV010,
                                         "/connections/2/delay");
    EXPECT_EQ(embedded.file, "catalog/workflows/example.estimate-predict/0.1.0.json");
    EXPECT_EQ(embedded.pointer, "/connections/2/delay");
    EXPECT_GT(embedded.line, 0);
    EXPECT_GT(embedded.column, 0);
    // An entry read from a directory is named by its file.
    const auto read = catalog.locate("capability-definition", "test.echo", kV010, "/outputs");
    EXPECT_EQ(praenora::test::utf8_path(read.file),
              praenora::test::fixture_path("catalog/capabilities/test.echo/0.1.0.json"))
        << read.file;
    EXPECT_GT(read.line, 0);
    // No such entry (of that kind): the pointer only.
    const auto none = catalog.locate("message-type", "test.echo", kV010, "/schema");
    EXPECT_TRUE(none.file.empty());
    EXPECT_EQ(none.line, 0);
    EXPECT_EQ(none.pointer, "/schema");
}
