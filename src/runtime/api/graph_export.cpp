#include "api/graph_export.hpp"

#include "core/semver.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"
#include "graph/graph.hpp"

#include <praenora/runtime/error.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace praenora::detail {

namespace {

using nlohmann::json;

std::string identity(const std::string& name, const SemVer& version) {
    return name + "@" + to_string(version);
}

// Two-space indented, keys sorted, a final newline.
std::string indented(const json& document) {
    return document.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

// The connection of the valid graph `graph` that `connection` of its definition became.
const ResolvedConnection& resolved(const WorkflowGraph& graph, const ConnectionDef& connection) {
    if (connection.to.kind == Endpoint::Kind::step) {
        return graph.incoming.at(connection.to.node).at(connection.to.port);
    }
    return graph.outputs.at(connection.to.port);
}

std::string text_graph(const WorkflowDef& definition, const WorkflowGraph& graph) {
    std::string schedule;
    for (const std::string& step : graph.schedule) {
        schedule += (schedule.empty() ? "" : ", ") + step;
    }
    std::string text = "workflow: " + identity(definition.name, definition.version) + "\n" +
                       "schedule: " + schedule + "\n" + "connections:\n";
    for (const ConnectionDef& connection : definition.connections) {
        text += "  " + connection.from.to_string() + " -> " + connection.to.to_string();
        if (connection.delay > 0) {
            text += " [delay " + std::to_string(connection.delay) + "]";
        }
        text += "\n";
    }
    return text;
}

std::string json_graph(const WorkflowDef& definition, const WorkflowGraph& graph) {
    json connections = json::array();
    for (const ConnectionDef& connection : definition.connections) {
        const ResolvedConnection& carried = resolved(graph, connection);
        connections.push_back({{"from", connection.from.to_string()},
                               {"to", connection.to.to_string()},
                               {"delay", connection.delay},
                               {"type", carried.type},
                               {"type_version", to_string(carried.type_version)}});
    }
    return indented(
        {{"workflow", {{"name", definition.name}, {"version", to_string(definition.version)}}},
         {"schedule", graph.schedule},
         {"connections", std::move(connections)}});
}

std::string dot_graph(const WorkflowDef& definition, const WorkflowGraph& graph) {
    const auto node = [](const std::string& name) { return "\"" + name + "\""; };
    std::string dot = "digraph " + node(identity(definition.name, definition.version)) + " {\n" +
                      "  rankdir=LR;\n";
    if (!definition.inputs.empty()) {
        dot += "  " + node("inputs") + " [shape=box];\n";
    }
    for (const std::string& step : graph.schedule) {
        dot += "  " + node(step) + ";\n";
    }
    if (!definition.outputs.empty()) {
        dot += "  " + node("outputs") + " [shape=box];\n";
    }
    for (const ConnectionDef& connection : definition.connections) {
        dot += "  " + node(connection.from.node) + " -> " + node(connection.to.node) +
               " [label=\"" + connection.from.port + " -> " + connection.to.port + "\"" +
               (connection.delay > 0 ? ", style=dashed" : "") + "];\n";
    }
    return dot + "}\n";
}

// `node` with every local reference ("$ref": "#/...") replaced by a copy of what it points to in
// `root`, recursively. `expanding` holds the references being replaced, so a reference that leads
// back to itself is refused (UNSUPPORTED) rather than expanded forever.
json inlined(const json& node, const json& root, const std::string& schema,
             std::vector<std::string>& expanding) {
    if (node.is_object()) {
        const auto ref = node.find("$ref");
        if (ref != node.end() && ref->is_string() &&
            ref->get_ref<const std::string&>().rfind("#", 0) == 0) {
            const std::string pointer = ref->get<std::string>().substr(1);
            if (std::find(expanding.begin(), expanding.end(), pointer) != expanding.end()) {
                throw Error(ErrorCode::UNSUPPORTED,
                            "the AsyncAPI export cannot inline the recursive reference \"#" +
                                pointer + "\" of " + schema);
            }
            json target;
            try {
                target = root.at(json::json_pointer(pointer));
            } catch (const json::exception&) {
                throw Error(ErrorCode::UNSUPPORTED, "the AsyncAPI export cannot resolve the "
                                                    "reference \"#" +
                                                        pointer + "\" of " + schema);
            }
            expanding.push_back(pointer);
            json result = inlined(target, root, schema, expanding);
            expanding.pop_back();
            return result;
        }
        json result = json::object();
        for (const auto& item : node.items()) {
            result[item.key()] = inlined(item.value(), root, schema, expanding);
        }
        return result;
    }
    if (node.is_array()) {
        json result = json::array();
        for (const json& item : node) {
            result.push_back(inlined(item, root, schema, expanding));
        }
        return result;
    }
    return node;
}

// `schema`, self-contained: local references inlined, and the keywords that only make sense at
// the root of a schema document dropped ("$schema", "$defs", "definitions" and those in `also`).
json self_contained(const json& schema, const std::string& name,
                    std::initializer_list<const char*> also = {}) {
    std::vector<std::string> expanding;
    json result = inlined(schema, schema, name, expanding);
    if (result.is_object()) {
        result.erase("$schema");
        result.erase("$defs");
        result.erase("definitions");
        for (const char* keyword : also) {
            result.erase(keyword);
        }
    }
    return result;
}

// The components message of `type`: its envelopes, with the type's data schema.
json message(const MessageTypeDef& type) {
    const SemVer pmp = parse_semver(kPmpVersion);
    json payload =
        self_contained(json::parse(SchemaRegistry::builtin().text_for_kind("pmp-envelope", pmp)),
                       "the pmp-envelope schema", {"$id"});
    payload["properties"]["data"] =
        self_contained(type.schema, "the schema of " + identity(type.id, type.version));
    return {{"name", type.id},
            {"title", identity(type.id, type.version)},
            {"contentType", "application/json"},
            {"payload", std::move(payload)}};
}

std::string asyncapi_graph(const WorkflowDef& definition, const WorkflowGraph& graph,
                           const Catalog& catalog) {
    json channels = json::object();
    json operations = json::object();
    json messages = json::object();
    std::map<std::string, SemVer> versions; // message type -> the version the workflow carries
    for (std::size_t i = 0; i < definition.connections.size(); ++i) {
        const ConnectionDef& connection = definition.connections[i];
        const ResolvedConnection& carried = resolved(graph, connection);
        const auto [known, added] = versions.emplace(carried.type, carried.type_version);
        if (!added && known->second != carried.type_version) {
            throw Error(ErrorCode::UNSUPPORTED,
                        "workflow " + identity(definition.name, definition.version) +
                            " carries message type " + carried.type + " at versions " +
                            to_string(known->second) + " and " + to_string(carried.type_version) +
                            "; the AsyncAPI export names messages by type and needs one version "
                            "per type",
                        catalog.locate("workflow-definition", definition.name, definition.version,
                                       "/connections/" + std::to_string(i)));
        }
        if (added) {
            const MessageTypeDef* type =
                catalog.find_message_type(carried.type, carried.type_version);
            if (type == nullptr) {
                throw Error(ErrorCode::CATALOG_ENTRY_NOT_FOUND,
                            "workflow " + identity(definition.name, definition.version) +
                                ": connection " + connection.from.to_string() + " -> " +
                                connection.to.to_string() + " carries the unknown message type " +
                                identity(carried.type, carried.type_version),
                            catalog.locate("workflow-definition", definition.name,
                                           definition.version,
                                           "/connections/" + std::to_string(i)));
            }
            messages[carried.type] = message(*type);
        }
        const std::string source = connection.from.to_string();
        channels[source] = {
            {"address", definition.name + "/" + source},
            {"messages",
             {{carried.type,
               {{"$ref", "#/components/messages/" + json_pointer_escape(carried.type)}}}}}};
        operations[source + ".publish"] = {
            {"action", "send"},
            {"channel", {{"$ref", "#/channels/" + json_pointer_escape(source)}}}};
    }
    json info = {{"title", definition.name}, {"version", to_string(definition.version)}};
    if (!definition.summary.empty()) {
        info["description"] = definition.summary;
    }
    return indented({{"asyncapi", "3.0.0"},
                     {"info", std::move(info)},
                     {"defaultContentType", "application/json"},
                     {"channels", std::move(channels)},
                     {"operations", std::move(operations)},
                     {"components", {{"messages", std::move(messages)}}}});
}

} // namespace

std::string export_graph(const WorkflowDef& definition, const Catalog& catalog,
                         GraphFormat format) {
    const WorkflowGraph graph = build_graph(definition, catalog);
    switch (format) {
    case GraphFormat::text:
        return text_graph(definition, graph);
    case GraphFormat::json:
        return json_graph(definition, graph);
    case GraphFormat::dot:
        return dot_graph(definition, graph);
    case GraphFormat::asyncapi:
        return asyncapi_graph(definition, graph, catalog);
    }
    throw Error(ErrorCode::INVALID_ARGUMENT, "unknown graph format");
}

} // namespace praenora::detail
