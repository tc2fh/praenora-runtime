#include "catalog/catalog.hpp"

#include "core/paths.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"
#include "document/resources.hpp"
#include "document/schema_registry.hpp"

#include <algorithm>
#include <system_error>
#include <tuple>
#include <utility>

namespace praenora::detail {

namespace {

constexpr std::string_view kMessageType = "message-type";
constexpr std::string_view kCapability = "capability-definition";
constexpr std::string_view kWorkflow = "workflow-definition";

// The catalog key of id@version; build metadata does not distinguish versions.
std::string entry_key(std::string_view id, const SemVer& version) {
    SemVer bare = version;
    bare.build.clear();
    return std::string(id) + "@" + to_string(bare);
}

// The key of an entry of the whole catalog: its kind and its id@version.
std::string kind_key(std::string_view kind, const std::string& key) {
    return std::string(kind) + " " + key;
}

[[noreturn]] void schema_violation(const LoadedDocument& document, const std::string& pointer,
                                   const std::string& message) {
    throw Error(ErrorCode::SCHEMA_VIOLATION,
                "catalog entry " + document.origin + ": " +
                    (pointer.empty() ? std::string("(root)") : pointer) + ": " + message,
                locate(document, pointer));
}

std::string string_at(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

} // namespace

Endpoint Endpoint::parse(std::string_view text) {
    const auto dot = text.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == text.size() ||
        text.find('.', dot + 1) != std::string_view::npos) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "invalid endpoint '" + std::string(text) +
                        "': expected <step>.<port>, inputs.<name> or outputs.<name>");
    }
    Endpoint endpoint;
    endpoint.node = std::string(text.substr(0, dot));
    endpoint.port = std::string(text.substr(dot + 1));
    if (endpoint.node == "inputs") {
        endpoint.kind = Kind::workflow_input;
    } else if (endpoint.node == "outputs") {
        endpoint.kind = Kind::workflow_output;
    } else {
        endpoint.kind = Kind::step;
    }
    return endpoint;
}

std::string Endpoint::to_string() const { return node + "." + port; }

struct Catalog::State {
    // Every entry by kind_key (kind and id@version): its kind and its document (origin and source
    // positions). The definitions are keyed by id@version within their kind.
    struct Entry {
        std::string kind;
        LoadedDocument document;
    };
    std::map<std::string, Entry> entries;
    std::map<std::string, MessageTypeDef> message_types;
    std::map<std::string, CapabilityDef> capabilities;
    std::map<std::string, WorkflowDef> workflows;

    void add(LoadedDocument document);
};

void Catalog::State::add(LoadedDocument document) {
    const nlohmann::json& json = document.json;
    if (!json.is_object()) {
        schema_violation(document, "", "a catalog entry must be a JSON object");
    }
    const std::string kind = string_at(json, "kind");
    if (kind != kMessageType && kind != kCapability && kind != kWorkflow) {
        schema_violation(document, "/kind",
                         "a catalog entry has kind message-type, capability-definition or "
                         "workflow-definition");
    }
    const std::string format_version = string_at(json, "format_version");
    if (format_version.empty()) {
        schema_violation(document, "", "required property 'format_version' not found in object");
    }
    SemVer format;
    try {
        format = parse_semver(format_version);
    } catch (const Error&) {
        schema_violation(document, "/format_version",
                         "'" + format_version + "' is not a semantic version");
    }
    const CompiledSchema& schema = SchemaRegistry::builtin().for_kind(kind, format);
    const auto issues = schema.validate(json);
    if (!issues.empty()) {
        schema_violation(document, issues.front().pointer, issues.front().message);
    }

    const std::string id =
        kind == kWorkflow ? json.at("name").get<std::string>() : json.at("id").get<std::string>();
    const SemVer version = parse_semver(json.at("version").get<std::string>());
    const std::string key = entry_key(id, version);
    if (const auto existing = entries.find(kind_key(kind, key)); existing != entries.end()) {
        throw Error(ErrorCode::CATALOG_ENTRY_DUPLICATE,
                    "catalog entry " + key + " (" + kind + ") is defined twice: in " +
                        existing->second.document.origin + " and in " + document.origin,
                    detail::locate(document, ""));
    }

    const std::string status = json.at("status").get<std::string>();
    const std::string summary = string_at(json, "summary");
    const std::string digest = sha256_hex(canonical_json(json));
    if (kind == kMessageType) {
        MessageTypeDef type;
        type.id = id;
        type.version = version;
        type.status = status;
        type.summary = summary;
        type.schema = json.at("schema");
        try {
            type.compiled = std::make_shared<const CompiledSchema>(type.schema, key);
        } catch (const Error& error) {
            schema_violation(document, "/schema" + error.location().pointer,
                             std::string("the schema of the data does not compile: ") +
                                 error.what());
        }
        const auto beyond = detail::beyond_asyncapi_subset(type.schema);
        if (!beyond.empty()) {
            schema_violation(document, "/schema",
                             "the schema of the data uses what the AsyncAPI export cannot express "
                             "(a 2020-12 schema of a message type stays within draft-07's keywords "
                             "and annotations beside a $ref): " +
                                 beyond.front());
        }
        type.digest = digest;
        message_types.emplace(key, std::move(type));
    } else if (kind == kCapability) {
        CapabilityDef capability;
        capability.id = id;
        capability.version = version;
        capability.status = status;
        capability.summary = summary;
        const auto read_ports = [](const nlohmann::json& ports) {
            std::map<std::string, PortDef> result;
            for (const auto& [name, port] : ports.items()) {
                result.emplace(name,
                               PortDef{port.at("type").get<std::string>(),
                                       parse_semver(port.at("type_version").get<std::string>()),
                                       port.at("required").get<bool>()});
            }
            return result;
        };
        capability.inputs = read_ports(json.at("inputs"));
        capability.outputs = read_ports(json.at("outputs"));
        capability.digest = digest;
        capabilities.emplace(key, std::move(capability));
    } else {
        WorkflowDef workflow;
        workflow.name = id;
        workflow.version = version;
        workflow.status = status;
        workflow.summary = summary;
        const auto read_ports = [&json](const char* member) {
            std::map<std::string, WorkflowPortDef> result;
            if (const auto it = json.find(member); it != json.end()) {
                for (const auto& [name, port] : it->items()) {
                    result.emplace(
                        name,
                        WorkflowPortDef{port.at("type").get<std::string>(),
                                        parse_semver(port.at("type_version").get<std::string>())});
                }
            }
            return result;
        };
        workflow.inputs = read_ports("inputs");
        workflow.outputs = read_ports("outputs");
        for (const auto& [name, step] : json.at("steps").items()) {
            const std::string capability_version = step.at("capability_version");
            workflow.steps.emplace(name, WorkflowStepDef{step.at("capability").get<std::string>(),
                                                         parse_semver(capability_version)});
        }
        for (const auto& connection : json.at("connections")) {
            ConnectionDef def;
            def.from = Endpoint::parse(connection.at("from").get<std::string>());
            def.to = Endpoint::parse(connection.at("to").get<std::string>());
            def.delay = connection.value("delay", 0);
            workflow.connections.push_back(std::move(def));
        }
        workflow.digest = digest;
        workflow.raw = json;
        workflows.emplace(key, std::move(workflow));
    }
    entries.emplace(kind_key(kind, key), Entry{kind, std::move(document)});
}

Catalog::Catalog() : state_(std::make_unique<State>()) {}
Catalog::Catalog(Catalog&&) noexcept = default;
Catalog& Catalog::operator=(Catalog&&) noexcept = default;
Catalog::~Catalog() = default;

Catalog Catalog::load(const std::vector<std::filesystem::path>& extra_dirs) {
    Catalog catalog;
    State& state = *catalog.state_;
    for (const std::string& path : list_embedded_resources("catalog/")) {
        if (path.size() < 5 || path.compare(path.size() - 5, 5, ".json") != 0) {
            continue;
        }
        const auto text = find_embedded_resource(path);
        state.add(parse_document_text(*text, path));
    }
    for (const auto& dir : extra_dirs) {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(dir, ec), end;
        if (ec) {
            throw Error(ErrorCode::IO_ERROR,
                        "cannot read catalog directory " + path_to_utf8(dir) + ": " + ec.message());
        }
        for (; it != end; it.increment(ec)) {
            if (ec) {
                throw Error(ErrorCode::IO_ERROR, "cannot read catalog directory " +
                                                     path_to_utf8(dir) + ": " + ec.message());
            }
            if (it->is_regular_file() && it->path().extension() == ".json") {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            state.add(load_document_file(file));
        }
    }
    return catalog;
}

const MessageTypeDef* Catalog::find_message_type(std::string_view id, const SemVer& version) const {
    const auto it = state_->message_types.find(entry_key(id, version));
    return it == state_->message_types.end() ? nullptr : &it->second;
}

const CapabilityDef* Catalog::find_capability(std::string_view id, const SemVer& version) const {
    const auto it = state_->capabilities.find(entry_key(id, version));
    return it == state_->capabilities.end() ? nullptr : &it->second;
}

const WorkflowDef* Catalog::find_workflow(std::string_view name, const SemVer& version) const {
    const auto it = state_->workflows.find(entry_key(name, version));
    return it == state_->workflows.end() ? nullptr : &it->second;
}

std::vector<CatalogEntrySummary> Catalog::entries() const {
    std::vector<std::tuple<std::string, std::string, SemVer, CatalogEntrySummary>> sorted;
    for (const auto& [key, entry] : state_->entries) {
        const nlohmann::json& json = entry.document.json;
        CatalogEntrySummary summary;
        summary.kind = entry.kind;
        summary.id = string_at(json, entry.kind == kWorkflow ? "name" : "id");
        summary.version = string_at(json, "version");
        summary.status = string_at(json, "status");
        summary.summary = string_at(json, "summary");
        sorted.emplace_back(summary.kind, summary.id, parse_semver(summary.version), summary);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (std::get<0>(a) != std::get<0>(b)) {
            return std::get<0>(a) < std::get<0>(b);
        }
        if (std::get<1>(a) != std::get<1>(b)) {
            return std::get<1>(a) < std::get<1>(b);
        }
        return std::get<2>(a) < std::get<2>(b);
    });
    std::vector<CatalogEntrySummary> result;
    result.reserve(sorted.size());
    for (auto& item : sorted) {
        result.push_back(std::move(std::get<3>(item)));
    }
    return result;
}

const nlohmann::json* Catalog::raw_entry(std::string_view id, const SemVer& version) const {
    const nlohmann::json* found = nullptr;
    for (const std::string_view kind : {kMessageType, kCapability, kWorkflow}) {
        if (const nlohmann::json* entry = raw_entry(kind, id, version); entry != nullptr) {
            if (found != nullptr) {
                return nullptr; // ambiguous: several kinds define id@version
            }
            found = entry;
        }
    }
    return found;
}

const nlohmann::json* Catalog::raw_entry(std::string_view kind, std::string_view id,
                                         const SemVer& version) const {
    const auto it = state_->entries.find(kind_key(kind, entry_key(id, version)));
    return it == state_->entries.end() ? nullptr : &it->second.document.json;
}

SourceLocation Catalog::locate(std::string_view kind, std::string_view id, const SemVer& version,
                               const std::string& pointer) const {
    const auto it = state_->entries.find(kind_key(kind, entry_key(id, version)));
    if (it == state_->entries.end()) {
        SourceLocation location;
        location.pointer = pointer;
        return location;
    }
    return detail::locate(it->second.document, pointer);
}

ValidationReport Catalog::check() const {
    ValidationReport report;
    std::string current; // kind_key of the entry being checked
    const auto not_found = [&](const std::string& message, const std::string& pointer) {
        Diagnostic diagnostic;
        diagnostic.severity = Severity::error;
        diagnostic.code = ErrorCode::CATALOG_ENTRY_NOT_FOUND;
        diagnostic.message = message;
        diagnostic.location = detail::locate(state_->entries.at(current).document, pointer);
        report.add(std::move(diagnostic));
    };
    const auto check_type = [&](const std::string& key, const std::string& what,
                                const std::string& type, const SemVer& type_version,
                                const std::string& pointer) {
        if (find_message_type(type, type_version) == nullptr) {
            not_found(key + ": " + what + " references unknown message type " + type + "@" +
                          to_string(type_version),
                      pointer);
        }
    };
    for (const auto& summary : entries()) {
        const std::string key = entry_key(summary.id, parse_semver(summary.version));
        current = kind_key(summary.kind, key);
        if (summary.kind == kCapability) {
            const CapabilityDef& capability = state_->capabilities.at(key);
            for (const auto& [name, port] : capability.inputs) {
                check_type(key, "input port '" + name + "'", port.type, port.type_version,
                           "/inputs/" + name + "/type");
            }
            for (const auto& [name, port] : capability.outputs) {
                check_type(key, "output port '" + name + "'", port.type, port.type_version,
                           "/outputs/" + name + "/type");
            }
        } else if (summary.kind == kWorkflow) {
            const WorkflowDef& workflow = state_->workflows.at(key);
            for (const auto& [name, port] : workflow.inputs) {
                check_type(key, "workflow input '" + name + "'", port.type, port.type_version,
                           "/inputs/" + name + "/type");
            }
            for (const auto& [name, port] : workflow.outputs) {
                check_type(key, "workflow output '" + name + "'", port.type, port.type_version,
                           "/outputs/" + name + "/type");
            }
            for (const auto& [name, step] : workflow.steps) {
                if (find_capability(step.capability, step.capability_version) == nullptr) {
                    not_found(key + ": step '" + name + "' references unknown capability " +
                                  step.capability + "@" + to_string(step.capability_version),
                              "/steps/" + name + "/capability");
                }
            }
        }
    }
    return report;
}

} // namespace praenora::detail
