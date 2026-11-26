// The protocol catalog: message types, capabilities and workflow definitions. The runtime embeds
// the entries of spec/catalog (resources "catalog/..."); extra directories add more. Every entry is
// a JSON document validated against the schema of its kind, and identified by its kind and
// "id@version" (workflow definitions use their name as id); entries of different kinds may share
// an id@version.
#ifndef PRAENORA_RUNTIME_CATALOG_CATALOG_HPP
#define PRAENORA_RUNTIME_CATALOG_CATALOG_HPP

#include "core/semver.hpp"
#include "document/schema.hpp"
#include "document/yaml_loader.hpp"

#include <praenora/runtime/error.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::detail {

struct MessageTypeDef {
    std::string id;
    SemVer version;
    std::string status, summary;
    nlohmann::json schema;                          // the JSON Schema of the data
    std::shared_ptr<const CompiledSchema> compiled; // `schema`, compiled
    std::string digest;                             // sha256_hex(canonical_json(entry))
};

struct PortDef {
    std::string type;
    SemVer type_version;
    bool required = false;
};

struct CapabilityDef {
    std::string id;
    SemVer version;
    std::string status, summary;
    std::map<std::string, PortDef> inputs, outputs;
    std::string digest;
};

// One end of a connection: "<step>.<port>", "inputs.<name>" or "outputs.<name>". For workflow
// inputs and outputs, `node` is "inputs" or "outputs" and `port` is the name.
struct Endpoint {
    enum class Kind { step, workflow_input, workflow_output } kind = Kind::step;
    std::string node, port;

    // Throws praenora::Error(INVALID_ARGUMENT) unless `text` is "<node>.<port>" with both parts
    // non-empty.
    static Endpoint parse(std::string_view text);
    std::string to_string() const;
};

struct ConnectionDef {
    Endpoint from, to;
    int delay = 0;
};

struct WorkflowStepDef {
    std::string capability;
    SemVer capability_version;
};

struct WorkflowPortDef {
    std::string type;
    SemVer type_version;
};

struct WorkflowDef {
    std::string name;
    SemVer version;
    std::string status, summary;
    std::map<std::string, WorkflowPortDef> inputs, outputs;
    std::map<std::string, WorkflowStepDef> steps;
    std::vector<ConnectionDef> connections; // in definition order
    std::string digest;
    nlohmann::json raw; // the entry as written
};

struct CatalogEntrySummary {
    std::string kind, id, version, status, summary;
};

class Catalog {
public:
    // The embedded entries, then every *.json file below each of `extra_dirs` (recursively, in
    // sorted path order). Throws praenora::Error:
    // - IO_ERROR when an extra directory cannot be read;
    // - PARSE_ERROR when an entry is not JSON;
    // - SCHEMA_VIOLATION when an entry does not match the schema of its kind, or a message type's
    //   schema does not compile (the location names the entry: "catalog/<path>" for embedded
    //   entries, the file path otherwise);
    // - FORMAT_VERSION_UNSUPPORTED for an unknown format_version;
    // - CATALOG_ENTRY_DUPLICATE when an id@version appears twice within one kind (the message
    //   names both origins).
    static Catalog load(const std::vector<std::filesystem::path>& extra_dirs);

    Catalog();
    Catalog(Catalog&&) noexcept;
    Catalog& operator=(Catalog&&) noexcept;
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    ~Catalog();

    // Lookups by id and exact version (build metadata ignored); nullptr when absent.
    const MessageTypeDef* find_message_type(std::string_view id, const SemVer& version) const;
    const CapabilityDef* find_capability(std::string_view id, const SemVer& version) const;
    const WorkflowDef* find_workflow(std::string_view name, const SemVer& version) const;

    // Every entry, sorted by kind, then id, then version.
    std::vector<CatalogEntrySummary> entries() const;

    // The entry document of id@version when exactly one kind defines it; nullptr when none or
    // several do.
    const nlohmann::json* raw_entry(std::string_view id, const SemVer& version) const;
    // The entry document of `kind` ("message-type", "capability-definition" or
    // "workflow-definition") and id@version; nullptr when absent.
    const nlohmann::json* raw_entry(std::string_view kind, std::string_view id,
                                    const SemVer& version) const;

    // Where `pointer` lies in the entry of `kind` and id@version: the entry's origin (the file it
    // was read from, or "catalog/<path>" for an embedded entry) as the file, with the line and
    // column of the pointer, or of its nearest ancestor. Only the pointer when the catalog has no
    // such entry. Diagnostics about an entry, such as a workflow definition's graph, are located
    // with it.
    SourceLocation locate(std::string_view kind, std::string_view id, const SemVer& version,
                          const std::string& pointer) const;

    // Cross-references: CATALOG_ENTRY_NOT_FOUND for every capability port whose message type is
    // unknown, and every workflow input, output or step whose message type or capability is
    // unknown. Diagnostics come in entry order (kind, id, version), located in the entry.
    ValidationReport check() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_CATALOG_CATALOG_HPP
