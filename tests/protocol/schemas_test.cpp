// The schemas of the Praenora document kinds (spec/schemas), checked against fixture documents:
// tests/fixtures/documents/valid/<kind>/* must pass; every document in
// tests/fixtures/documents/invalid/<kind>/ must fail, with a problem at the JSON pointer named by
// its sibling <name>.expect.json ({"code": "<ERROR NAME>", "pointer": "<json pointer>"}). Each
// document is checked against the schema of its kind at its own format_version (0.1 for the
// module ABI's documents).
#include "catalog/catalog.hpp"
#include "core/semver.hpp"
#include "document/schema.hpp"
#include "document/schema_registry.hpp"
#include "document/yaml_loader.hpp"

#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::detail::Catalog;
using praenora::detail::load_document_file;
using praenora::detail::parse_semver;
using praenora::detail::SchemaIssue;
using praenora::detail::SchemaRegistry;

namespace {

const std::vector<std::string> kKinds = {"capability-definition",
                                         "message-type",
                                         "module-check-report",
                                         "module-manifest",
                                         "module-test-vectors",
                                         "pmp-create",
                                         "pmp-describe",
                                         "pmp-envelope",
                                         "pmp-error",
                                         "pmp-initialize",
                                         "pmp-invocation",
                                         "pmp-result",
                                         "provider-manifest",
                                         "run-record",
                                         "status-snapshot",
                                         "workflow-config",
                                         "workflow-definition"};

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The fixture documents below documents/<validity>/, as sorted (kind, path) pairs; the kind is the
// name of the directory holding the document.
std::vector<std::pair<std::string, fs::path>> fixture_documents(const std::string& validity) {
    std::vector<std::pair<std::string, fs::path>> documents;
    for (const auto& kind_dir :
         fs::directory_iterator(praenora::test::fixture_path("documents/" + validity))) {
        if (!kind_dir.is_directory()) {
            continue;
        }
        for (const auto& file : fs::directory_iterator(kind_dir.path())) {
            const std::string name = file.path().filename().string();
            if (ends_with(name, ".expect.json") ||
                !(ends_with(name, ".json") || ends_with(name, ".yaml"))) {
                continue;
            }
            documents.emplace_back(kind_dir.path().filename().string(), file.path());
        }
    }
    std::sort(documents.begin(), documents.end());
    return documents;
}

// The schema of `kind` at the format_version of `document`, or at 0.1 when it has none (or one
// that is not a version: the schema then reports it).
const praenora::detail::CompiledSchema& schema_for(const std::string& kind, const json& document) {
    praenora::detail::SemVer version = parse_semver("0.1.0");
    if (document.is_object() && document.contains("format_version") &&
        document.at("format_version").is_string()) {
        try {
            version = parse_semver(document.at("format_version").get<std::string>());
        } catch (const praenora::Error&) {
            // Checked against 0.1.
        }
    }
    return SchemaRegistry::builtin().for_kind(kind, version);
}

std::string describe(const std::vector<SchemaIssue>& issues) {
    std::string text;
    for (const auto& issue : issues) {
        text += "\n  '" + issue.pointer + "': " + issue.message;
    }
    return text;
}

} // namespace

TEST(Schemas, AllBuiltinSchemasCompile) {
    const SchemaRegistry& registry = SchemaRegistry::builtin();
    std::vector<std::string> kinds;
    std::map<std::string, std::vector<std::string>> versions;
    for (const auto& [kind, version] : registry.entries()) {
        if (kinds.empty() || kinds.back() != kind) {
            kinds.push_back(kind);
        }
        versions[kind].push_back(praenora::detail::to_string(version));
        EXPECT_NO_THROW((void)registry.for_kind(kind, version)) << kind;
    }
    EXPECT_EQ(registry.entries().size(), 19u);
    EXPECT_EQ(kinds, kKinds);
    // Module manifests and run records have a format 0.2; the 0.1 schemas stay for the documents
    // written before.
    for (const auto& kind : kKinds) {
        const std::vector<std::string> expected =
            kind == "module-manifest" || kind == "run-record"
                ? std::vector<std::string>{"0.1.0", "0.2.0"}
                : std::vector<std::string>{"0.1.0"};
        EXPECT_EQ(versions[kind], expected) << kind;
    }
}

TEST(Schemas, EveryKindHasValidAndInvalidFixtures) {
    std::map<std::string, int> valid, invalid;
    for (const auto& entry : fixture_documents("valid")) {
        ++valid[entry.first];
    }
    for (const auto& entry : fixture_documents("invalid")) {
        ++invalid[entry.first];
    }
    for (const auto& kind : kKinds) {
        EXPECT_GE(valid[kind], 1) << kind;
        EXPECT_GE(invalid[kind], 3) << kind;
    }
    EXPECT_GE(invalid["module-manifest"] + invalid["workflow-config"], 6);
}

TEST(Schemas, ValidFixturesPass) {
    const auto documents = fixture_documents("valid");
    ASSERT_FALSE(documents.empty());
    for (const auto& [kind, path] : documents) {
        SCOPED_TRACE(path.string());
        const auto document = load_document_file(path);
        const auto issues = schema_for(kind, document.json).validate(document.json);
        EXPECT_TRUE(issues.empty()) << describe(issues);
    }
}

TEST(Schemas, InvalidFixturesFailAtExpectedPointer) {
    const auto documents = fixture_documents("invalid");
    ASSERT_FALSE(documents.empty());
    for (const auto& [kind, path] : documents) {
        SCOPED_TRACE(path.string());
        fs::path expect_path = path;
        expect_path.replace_extension(".expect.json");
        ASSERT_TRUE(fs::exists(expect_path)) << expect_path.string();
        const json expect = load_document_file(expect_path).json;
        const auto code = praenora::error_code_from_name(expect.at("code").get<std::string>());
        ASSERT_TRUE(code.has_value()) << expect.dump();
        EXPECT_EQ(*code, praenora::ErrorCode::SCHEMA_VIOLATION);

        const auto document = load_document_file(path);
        const auto issues = schema_for(kind, document.json).validate(document.json);
        const std::string pointer = expect.at("pointer").get<std::string>();
        const bool found = std::any_of(issues.begin(), issues.end(), [&](const SchemaIssue& issue) {
            return issue.pointer == pointer;
        });
        EXPECT_TRUE(found) << "expected a problem at '" << pointer << "'; got:" << describe(issues);
    }
}

// The manifests praenora_add_module() generates for the fixture modules are valid manifests, of
// the format the SDK writes unless their template names another.
TEST(Schemas, GeneratedFixtureManifestsAreValid) {
    int count = 0;
    for (const auto& module_dir :
         fs::directory_iterator(praenora::test::utf8_path(PRAENORA_TEST_MODULES_DIR))) {
        if (!module_dir.is_directory()) {
            continue;
        }
        for (const auto& file : fs::directory_iterator(module_dir.path())) {
            if (!ends_with(file.path().filename().string(), ".pmm.json")) {
                continue;
            }
            SCOPED_TRACE(file.path().string());
            ++count;
            const json manifest = load_document_file(file.path()).json;
            const auto issues = schema_for("module-manifest", manifest).validate(manifest);
            EXPECT_TRUE(issues.empty()) << describe(issues);
            if (file.path().filename() != "fx-service-format-0-1.pmm.json") {
                EXPECT_EQ(manifest.at("format_version"), "0.2.0");
            }
        }
    }
    EXPECT_GT(count, 0);
}

namespace {

// Appends what is draft-07 only in `node` (a schema or a document holding schemas): the keywords
// draft-07 spells differently in 2020-12 and the draft-07 references.
void find_draft07_spellings(const json& node, const std::string& pointer,
                            std::vector<std::string>& found) {
    if (node.is_object()) {
        for (const auto& item : node.items()) {
            const std::string where = pointer + "/" + item.key();
            const json& value = item.value();
            if ((item.key() == "definitions" || item.key() == "dependencies") &&
                value.is_object()) {
                found.push_back(where);
            } else if (item.key() == "additionalItems") {
                found.push_back(where);
            } else if (item.key() == "items" && value.is_array()) {
                found.push_back(where);
            } else if (item.key() == "$ref" && value.is_string() &&
                       value.get<std::string>().find("/definitions/") != std::string::npos) {
                found.push_back(where);
            }
            find_draft07_spellings(value, where, found);
        }
    } else if (node.is_array()) {
        for (std::size_t i = 0; i < node.size(); ++i) {
            find_draft07_spellings(node[i], pointer + "/" + std::to_string(i), found);
        }
    }
}

} // namespace

// Every schema the runtime owns declares 2020-12 and uses its spelling of the keywords: the 19
// document schemas here (the envelope also within what AsyncAPI expresses), the data schemas of the
// message types below.
TEST(Schemas, EveryDocumentSchemaIs202012) {
    const auto& registry = SchemaRegistry::builtin();
    const auto entries = registry.entries();
    EXPECT_EQ(entries.size(), 19u);
    for (const auto& entry : entries) {
        SCOPED_TRACE(entry.first);
        const json schema = json::parse(registry.text_for_kind(entry.first, entry.second));
        EXPECT_EQ(schema.value("$schema", ""), "https://json-schema.org/draft/2020-12/schema");
        std::vector<std::string> found;
        find_draft07_spellings(schema, "", found);
        EXPECT_TRUE(found.empty()) << "draft-07 spelling at " << (found.empty() ? "" : found.front());
        if (entry.first == "pmp-envelope") {
            // The AsyncAPI export embeds the envelope as it does a message type's schema.
            EXPECT_TRUE(praenora::detail::beyond_asyncapi_subset(schema).empty());
        }
    }
}

// The data schemas of the embedded message types are 2020-12 too, and stay within what AsyncAPI
// 3.0.0 expresses (the catalog refuses one that does not).
TEST(Schemas, EveryMessageTypeSchemaIs202012) {
    const Catalog catalog = Catalog::load({});
    int count = 0;
    for (const auto& entry : catalog.entries()) {
        if (entry.kind != "message-type") {
            continue;
        }
        SCOPED_TRACE(entry.id);
        ++count;
        const json* document = catalog.raw_entry("message-type", entry.id, parse_semver(entry.version));
        ASSERT_NE(document, nullptr);
        const json& schema = document->at("schema");
        EXPECT_EQ(schema.value("$schema", ""), "https://json-schema.org/draft/2020-12/schema");
        std::vector<std::string> found;
        find_draft07_spellings(schema, "", found);
        EXPECT_TRUE(found.empty()) << "draft-07 spelling at " << (found.empty() ? "" : found.front());
        EXPECT_TRUE(praenora::detail::beyond_asyncapi_subset(schema).empty());
    }
    EXPECT_EQ(count, 7);
}
