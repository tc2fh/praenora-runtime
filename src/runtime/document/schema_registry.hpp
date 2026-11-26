// The schemas of the Praenora document kinds: one schema per (kind, major.minor), 2020-12 for the
// ones the runtime ships, in files named <kind>.<major>.<minor>.schema.json. Every such schema
// must have exactly the $id urn:praenora:schema:<kind>:<major>.<minor>.
#ifndef PRAENORA_RUNTIME_DOCUMENT_SCHEMA_REGISTRY_HPP
#define PRAENORA_RUNTIME_DOCUMENT_SCHEMA_REGISTRY_HPP

#include "core/semver.hpp"
#include "document/schema.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace praenora::detail {

class SchemaRegistry {
public:
    // The schemas embedded in the runtime (resources "schemas/<kind>.<major>.<minor>.schema.json").
    static const SchemaRegistry& builtin();

    // The schemas in the files <kind>.<major>.<minor>.schema.json directly inside `directory`;
    // other files are ignored. Throws praenora::Error(IO_ERROR) if the directory cannot be read.
    static SchemaRegistry from_directory(const std::filesystem::path& directory);

    SchemaRegistry(SchemaRegistry&&) noexcept;
    SchemaRegistry& operator=(SchemaRegistry&&) noexcept;
    SchemaRegistry(const SchemaRegistry&) = delete;
    SchemaRegistry& operator=(const SchemaRegistry&) = delete;
    ~SchemaRegistry();

    // The schema for documents of `kind` whose format_version is `format_version`: the schema of
    // its major.minor. Compiled on first use (thread-safe) and kept for the registry's lifetime;
    // the name of the compiled schema is "<kind>@<major>.<minor>". Throws praenora::Error with
    // FORMAT_VERSION_UNSUPPORTED if there is no such schema, PARSE_ERROR if its text is not JSON,
    // and SCHEMA_VIOLATION if it does not compile or its $id is missing or not the expected URN.
    const CompiledSchema& for_kind(std::string_view kind, const SemVer& format_version) const;

    // The schema's text. Throws praenora::Error(FORMAT_VERSION_UNSUPPORTED) like for_kind().
    std::string_view text_for_kind(std::string_view kind, const SemVer& format_version) const;

    // Every (kind, major.minor.0), sorted by kind, then version.
    std::vector<std::pair<std::string, SemVer>> entries() const;

private:
    struct State;
    explicit SchemaRegistry(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_DOCUMENT_SCHEMA_REGISTRY_HPP
