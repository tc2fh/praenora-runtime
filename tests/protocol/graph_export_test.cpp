// Exports of workflow definitions (praenora graph): text, JSON, DOT and AsyncAPI 3.0.0, and the
// AsyncAPI documents committed under spec/asyncapi/, which must equal the generator's output byte
// for byte. PRAENORA_UPDATE_GOLDEN=1 makes GraphExport.CommittedAsyncApiMatchesGenerator rewrite
// them instead of comparing.
#include "api/graph_export.hpp"
#include "catalog/catalog.hpp"
#include "core/semver.hpp"

#include "module_fixtures.hpp"
#include "test_support.hpp"

#include <praenora/runtime/options.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::GraphFormat;
using praenora::detail::Catalog;
using praenora::detail::export_graph;
using praenora::detail::parse_semver;
using praenora::detail::WorkflowDef;

namespace {

const WorkflowDef& workflow(const Catalog& catalog, const std::string& name) {
    const WorkflowDef* definition = catalog.find_workflow(name, parse_semver("0.1.0"));
    if (definition == nullptr) {
        throw std::runtime_error("no workflow " + name);
    }
    return *definition;
}

std::string read_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Whether the committed files are to be rewritten (PRAENORA_UPDATE_GOLDEN=1) instead of compared.
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

std::set<std::string> keys_of(const json& object) {
    std::set<std::string> keys;
    for (const auto& item : object.items()) {
        keys.insert(item.key());
    }
    return keys;
}

// Every "$ref" value in `value`, depth first.
void collect_refs(const json& value, std::vector<std::string>& refs) {
    if (value.is_object()) {
        for (const auto& item : value.items()) {
            if (item.key() == "$ref" && item.value().is_string()) {
                refs.push_back(item.value().get<std::string>());
            } else {
                collect_refs(item.value(), refs);
            }
        }
    } else if (value.is_array()) {
        for (const json& item : value) {
            collect_refs(item, refs);
        }
    }
}

} // namespace

TEST(GraphExport, TextListsScheduleAndConnections) {
    const Catalog catalog = Catalog::load({});
    const std::string text =
        export_graph(workflow(catalog, "example.estimate-predict"), catalog, GraphFormat::text);
    EXPECT_EQ(text, "workflow: example.estimate-predict@0.1.0\n"
                    "schedule: estimate, predict\n"
                    "connections:\n"
                    "  inputs.observations -> estimate.observations\n"
                    "  estimate.state -> predict.state\n"
                    "  predict.predictions -> estimate.prior [delay 1]\n"
                    "  predict.predictions -> outputs.predictions\n"
                    "  estimate.state -> outputs.state\n");
}

TEST(GraphExport, JsonListsScheduleAndTypedConnections) {
    const Catalog catalog = Catalog::load({});
    const std::string text =
        export_graph(workflow(catalog, "example.estimate-predict"), catalog, GraphFormat::json);
    const json document = json::parse(text);
    EXPECT_EQ(text, document.dump(2) + "\n");
    EXPECT_EQ(document.at("workflow"),
              (json{{"name", "example.estimate-predict"}, {"version", "0.1.0"}}));
    EXPECT_EQ(document.at("schedule"), (json{"estimate", "predict"}));
    ASSERT_EQ(document.at("connections").size(), 5u);
    EXPECT_EQ(document.at("connections")[2], (json{{"from", "predict.predictions"},
                                                   {"to", "estimate.prior"},
                                                   {"delay", 1},
                                                   {"type", "pmp.prediction-set"},
                                                   {"type_version", "0.1.0"}}));
    EXPECT_EQ(document.at("connections")[0].at("delay"), 0);
}

TEST(GraphExport, DotMarksDelayedEdgesDashed) {
    const Catalog catalog = Catalog::load({});
    const std::string dot =
        export_graph(workflow(catalog, "example.estimate-predict"), catalog, GraphFormat::dot);
    EXPECT_EQ(dot, "digraph \"example.estimate-predict@0.1.0\" {\n"
                   "  rankdir=LR;\n"
                   "  \"inputs\" [shape=box];\n"
                   "  \"estimate\";\n"
                   "  \"predict\";\n"
                   "  \"outputs\" [shape=box];\n"
                   "  \"inputs\" -> \"estimate\" [label=\"observations -> observations\"];\n"
                   "  \"estimate\" -> \"predict\" [label=\"state -> state\"];\n"
                   "  \"predict\" -> \"estimate\" [label=\"predictions -> prior\", "
                   "style=dashed];\n"
                   "  \"predict\" -> \"outputs\" [label=\"predictions -> predictions\"];\n"
                   "  \"estimate\" -> \"outputs\" [label=\"state -> state\"];\n"
                   "}\n");
}

TEST(GraphExport, AsyncApiHasRequiredTopLevelFields) {
    const Catalog catalog = Catalog::load({});
    for (const std::string name : {"example.estimate-predict", "pmp.continuous-forecast"}) {
        SCOPED_TRACE(name);
        const WorkflowDef& definition = workflow(catalog, name);
        const std::string text = export_graph(definition, catalog, GraphFormat::asyncapi);
        const json document = json::parse(text);
        // Two-space indentation, keys sorted (nlohmann's objects), a final newline.
        EXPECT_EQ(text, document.dump(2) + "\n");
        EXPECT_EQ(document.at("asyncapi"), "3.0.0");
        EXPECT_EQ(document.at("info").at("title"), name);
        EXPECT_EQ(document.at("info").at("version"), "0.1.0");
        if (definition.summary.empty()) {
            EXPECT_FALSE(document.at("info").contains("description"));
        } else {
            EXPECT_EQ(document.at("info").at("description"), definition.summary);
        }
        EXPECT_EQ(document.at("defaultContentType"), "application/json");

        // One channel per connection source, one publish operation per channel.
        std::set<std::string> sources;
        for (const auto& connection : definition.connections) {
            sources.insert(connection.from.to_string());
        }
        const json& channels = document.at("channels");
        const json& operations = document.at("operations");
        EXPECT_EQ(keys_of(channels), sources);
        EXPECT_EQ(operations.size(), sources.size());
        for (const std::string& source : sources) {
            const json& channel = channels.at(source);
            EXPECT_EQ(channel.at("address"), name + "/" + source);
            ASSERT_EQ(channel.at("messages").size(), 1u);
            const json& operation = operations.at(source + ".publish");
            EXPECT_EQ(operation.at("action"), "send");
            EXPECT_EQ(operation.at("channel").at("$ref"), "#/channels/" + source);
        }

        // One message per message type, its payload the envelope with the type's data schema.
        const json& messages = document.at("components").at("messages");
        for (const auto& item : messages.items()) {
            SCOPED_TRACE(item.key());
            const json& message = item.value();
            EXPECT_EQ(message.at("name"), item.key());
            EXPECT_EQ(message.at("title"), item.key() + "@0.1.0");
            EXPECT_EQ(message.at("contentType"), "application/json");
            const auto* type = catalog.find_message_type(item.key(), parse_semver("0.1.0"));
            ASSERT_NE(type, nullptr);
            json data_schema = type->schema;
            data_schema.erase("$schema");
            const json& payload = message.at("payload");
            EXPECT_EQ(payload.at("properties").at("data"), data_schema);
            EXPECT_EQ(payload.at("required").back(), "data");
            EXPECT_EQ(payload.at("properties").at("tick").at("type"), "integer");
            EXPECT_FALSE(payload.contains("$id"));
            EXPECT_FALSE(payload.contains("definitions"));
            EXPECT_FALSE(payload.contains("$defs"));
        }

        // Every reference resolves within the document.
        std::vector<std::string> refs;
        collect_refs(document, refs);
        EXPECT_FALSE(refs.empty());
        for (const std::string& ref : refs) {
            ASSERT_EQ(ref.rfind("#/", 0), 0u) << ref;
            EXPECT_TRUE(document.contains(json::json_pointer(ref.substr(1)))) << ref;
        }
    }
}

TEST(GraphExport, CommittedAsyncApiMatchesGenerator) {
    const Catalog catalog = Catalog::load({});
    for (const std::string name : {"example.estimate-predict", "pmp.continuous-forecast"}) {
        SCOPED_TRACE(name);
        const std::string generated =
            export_graph(workflow(catalog, name), catalog, GraphFormat::asyncapi);
        const fs::path committed = praenora::test::utf8_path(PRAENORA_TEST_SOURCE_DIR) / "spec" /
                                   "asyncapi" /
                                   praenora::test::utf8_path(name + "-0.1.0.asyncapi.json");
        if (update_golden()) {
            praenora::test::write_text_file(committed, generated);
            std::cout << "refreshed " << committed.u8string() << "\n";
            continue;
        }
        ASSERT_TRUE(fs::is_regular_file(committed))
            << committed.u8string() << " is missing; PRAENORA_UPDATE_GOLDEN=1 writes it";
        EXPECT_TRUE(read_bytes(committed) == generated)
            << committed.u8string()
            << " differs from the generator's output; PRAENORA_UPDATE_GOLDEN=1 rewrites it";
    }
}
