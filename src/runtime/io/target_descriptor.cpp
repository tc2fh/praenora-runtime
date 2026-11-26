#include "io/target_descriptor.hpp"

#include "core/paths.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"
#include "document/resources.hpp"
#include "document/schema.hpp"

#include <praenora/runtime/error.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

namespace praenora::detail {

namespace {

using nlohmann::json;

constexpr const char* kSchemaResource = "ode-uq/target.0.0.schema.json";
constexpr std::string_view kVariablesPrefix = "variables[].";

std::string single_quoted(const std::string& text) { return "'" + text + "'"; }

[[noreturn]] void fail(const LoadedDocument& document, ErrorCode code, const std::string& pointer,
                       const std::string& message) {
    throw Error(code, "the target descriptor " + single_quoted(document.origin) + ": " + message,
                locate(document, pointer));
}

// ode-uq-spec descriptors are JSON (docs/conventions.md: "JSON uses no comments, duplicate keys,
// NaN, or infinity"). The content decides, not the file name: the bytes are parsed as strict JSON
// (the document loader does that for an origin ending in .json, which the parse is given whatever
// the file is called), and anything else, YAML included, is UNSUPPORTED.
LoadedDocument read_document(const std::filesystem::path& file) {
    const std::string origin = path_to_utf8(file);
    std::string bytes;
    try {
        check_document_size(file);
        bytes = read_file_bytes(file);
    } catch (const Error& e) {
        if (e.code() != ErrorCode::IO_ERROR) {
            throw;
        }
        SourceLocation location;
        location.file = origin;
        throw Error(ErrorCode::TARGET_DATA_UNAVAILABLE,
                    std::string("cannot read the target descriptor: ") + e.what(),
                    std::move(location));
    }
    try {
        LoadedDocument document = parse_document_text(bytes, origin + ".json");
        document.origin = origin;
        return document;
    } catch (const Error& e) {
        if (e.code() != ErrorCode::PARSE_ERROR) {
            throw;
        }
        SourceLocation location = e.location();
        location.file = origin;
        throw Error(ErrorCode::UNSUPPORTED,
                    "the target descriptor must be JSON, as ode-uq-spec writes it (YAML is not "
                    "read): " +
                        std::string(e.what()),
                    std::move(location));
    }
}

// A URI scheme of two or more characters followed by a colon (as the spec's validator reads one:
// a single letter is a Windows drive).
bool has_uri_scheme(const std::string& href) {
    if (href.empty() || !std::isalpha(static_cast<unsigned char>(href[0]))) {
        return false;
    }
    std::size_t i = 1;
    while (i < href.size() &&
           (std::isalnum(static_cast<unsigned char>(href[i])) || href[i] == '+' ||
            href[i] == '.' || href[i] == '-')) {
        ++i;
    }
    return i >= 2 && i < href.size() && href[i] == ':';
}

std::string lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return text;
}

// The one-byte ASCII character of the dialect member `key`, if the descriptor sets it.
std::optional<char> dialect_char(const LoadedDocument& document, const json& dialect,
                                 const char* key) {
    const auto it = dialect.find(key);
    if (it == dialect.end()) {
        return std::nullopt;
    }
    const std::string& text = it->get_ref<const std::string&>();
    const std::string pointer = std::string("/dialect/") + key;
    if (text.size() != 1 || static_cast<unsigned char>(text[0]) >= 0x80) {
        fail(document, ErrorCode::UNSUPPORTED, pointer,
             "the " + std::string(key) + " " + single_quoted(text) +
                 " is not one ASCII character, which is all the runtime reads");
    }
    if (text[0] == '\n' || text[0] == '\r') {
        fail(document, ErrorCode::SCHEMA_VIOLATION, pointer,
             "the " + std::string(key) + " must not be a line break");
    }
    return text[0];
}

json embedded_schema() {
    const auto text = find_embedded_resource(kSchemaResource);
    if (!text) {
        throw Error(ErrorCode::INTERNAL,
                    std::string("the embedded resource ") + kSchemaResource + " is missing");
    }
    return parse_json_text(*text, kSchemaResource);
}

const CompiledSchema& compiled_schema() {
    static const CompiledSchema schema(embedded_target_descriptor_schema(), "target-descriptor@0.0");
    return schema;
}

// The descriptor with the defaults of the members whose absence has a meaning filled in, so that
// writing a default and leaving it out give one digest.
json with_defaults(json document) {
    json& dialect = document["dialect"];
    if (!dialect.is_object()) {
        dialect = json::object();
    }
    dialect.emplace("delimiter", ",");
    dialect.emplace("quotechar", "\"");
    dialect.emplace("header", true);
    dialect.emplace("encoding", "UTF-8");
    for (json& variable : document["variables"]) {
        variable.emplace("role", "response");
    }
    return document;
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t dot = path.find('.', begin);
        parts.push_back(path.substr(begin, dot == std::string::npos ? dot : dot - begin));
        if (dot == std::string::npos) {
            return parts;
        }
        begin = dot + 1;
    }
}

// The members of the (defaults filled) descriptor that the hashed paths name, variables sorted by
// id (their order does not change what the runtime does).
json digest_subset(const json& document) {
    const json effective = with_defaults(document);
    json subset = json::object();
    for (const std::string& path : descriptor_hashed_paths()) {
        if (path.compare(0, kVariablesPrefix.size(), kVariablesPrefix) == 0) {
            continue;
        }
        const std::vector<std::string> parts = split_path(path);
        const json* source = &effective;
        for (const std::string& part : parts) {
            source = source->is_object() && source->contains(part) ? &source->at(part) : nullptr;
            if (source == nullptr) {
                break;
            }
        }
        if (source == nullptr) {
            continue;
        }
        json* target = &subset;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            target = &(*target)[parts[i]];
        }
        *target = *source;
    }
    json variables = json::array();
    for (const json& variable : effective.at("variables")) {
        json entry = json::object();
        for (const std::string& path : descriptor_hashed_paths()) {
            if (path.compare(0, kVariablesPrefix.size(), kVariablesPrefix) != 0) {
                continue;
            }
            const std::string key = path.substr(kVariablesPrefix.size());
            if (variable.contains(key)) {
                // A bound is a number: 0, 0.0 and 1e0 are one value, and -0 is 0.
                if (key == "lower_bound" || key == "upper_bound") {
                    const double bound = variable.at(key).get<double>();
                    entry[key] = bound == 0 ? 0.0 : bound;
                } else {
                    entry[key] = variable.at(key);
                }
            }
        }
        variables.push_back(std::move(entry));
    }
    std::sort(variables.begin(), variables.end(),
              [](const json& a, const json& b) { return a.at("id") < b.at("id"); });
    subset["variables"] = std::move(variables);
    return subset;
}

} // namespace

const json& embedded_target_descriptor_schema() {
    static const json schema = embedded_schema();
    return schema;
}

const std::vector<std::string>& descriptor_hashed_paths() {
    static const std::vector<std::string> paths = {
        "columns.series_id",
        "columns.variable_id",
        "columns.time",
        "columns.value",
        "columns.observation_id",
        "columns.unit",
        "columns.missing",
        "columns.censoring",
        "columns.lower_limit",
        "columns.upper_limit",
        "columns.weight",
        "columns.group_id",
        "columns.batch_id",
        "dialect.delimiter",
        "dialect.quotechar",
        "dialect.header",
        "dialect.encoding",
        "dialect.missing_token",
        "time_unit",
        "duplicate_policy",
        "variables[].id",
        "variables[].unit",
        "variables[].value_type",
        "variables[].role",
        "variables[].lower_bound",
        "variables[].upper_bound",
    };
    return paths;
}

const std::vector<std::string>& descriptor_excluded_paths() {
    static const std::vector<std::string> paths = {
        "schema_version", // checked on its own (it must be 0.0.0)
        "id",
        "name",
        "description",
        "data.id",
        "data.href",
        "data.media_type",
        "data.sha256", // the CSV's own digest is part of the fingerprint
        "data.role",
        "data.x-*",
        "variables[].name",
        "x-*",
    };
    return paths;
}

TargetDescriptor load_target_descriptor(const std::filesystem::path& file) {
    TargetDescriptor descriptor;
    descriptor.file = file;
    descriptor.document = read_document(file);
    const LoadedDocument& document = descriptor.document;
    const json& root = document.json;

    if (const auto version = root.is_object() ? root.find("schema_version") : root.end();
        root.is_object() && version != root.end() && version->is_string() &&
        version->get_ref<const std::string&>() != kTargetDescriptorSchemaVersion) {
        fail(document, ErrorCode::FORMAT_VERSION_UNSUPPORTED, "/schema_version",
             "schema_version " + single_quoted(version->get<std::string>()) +
                 " is not supported; the runtime reads " +
                 single_quoted(std::string(kTargetDescriptorSchemaVersion)));
    }
    const std::vector<SchemaIssue> issues = compiled_schema().validate(root);
    if (!issues.empty()) {
        fail(document, ErrorCode::SCHEMA_VIOLATION, issues.front().pointer,
             (issues.front().pointer.empty() ? std::string("(root)") : issues.front().pointer) +
                 ": " + issues.front().message +
                 (issues.size() > 1 ? " (and " + std::to_string(issues.size() - 1) + " more)"
                                    : std::string()));
    }

    descriptor.id = root.at("id").get<std::string>();
    descriptor.schema_version = root.at("schema_version").get<std::string>();
    const json& data = root.at("data");
    descriptor.href = data.at("href").get<std::string>();
    descriptor.href_remote = has_uri_scheme(descriptor.href);
    if (!descriptor.href_remote) {
        descriptor.data_path = resolve_against(file.parent_path(), descriptor.href);
    }
    if (const auto it = data.find("sha256"); it != data.end()) {
        descriptor.sha256 = lower_ascii(it->get<std::string>());
    }
    if (const auto it = root.find("dialect"); it != root.end()) {
        const auto delimiter = dialect_char(document, *it, "delimiter");
        const auto quote = dialect_char(document, *it, "quotechar");
        descriptor.dialect.delimiter = delimiter.value_or(descriptor.dialect.delimiter);
        descriptor.dialect.quote = quote.value_or(descriptor.dialect.quote);
        if (descriptor.dialect.delimiter == descriptor.dialect.quote) {
            // Point at the key that was set; when both are, at the quotechar.
            fail(document, ErrorCode::SCHEMA_VIOLATION,
                 quote ? "/dialect/quotechar" : "/dialect/delimiter",
                 "the delimiter and the quotechar are the same character");
        }
        if (const auto token = it->find("missing_token"); token != it->end()) {
            descriptor.missing_token = token->get<std::string>();
        }
    }
    for (const auto& [column, header] : root.at("columns").items()) {
        descriptor.columns.emplace(column, header.get<std::string>());
    }
    if (const auto it = root.find("time_unit"); it != root.end()) {
        descriptor.time_unit = it->get<std::string>();
    }
    if (const auto it = root.find("duplicate_policy"); it != root.end()) {
        descriptor.duplicate_policy = it->get<std::string>();
        if (*descriptor.duplicate_policy == "aggregate_mean" ||
            *descriptor.duplicate_policy == "aggregate_median") {
            fail(document, ErrorCode::UNSUPPORTED, "/duplicate_policy",
                 "duplicate_policy " + single_quoted(*descriptor.duplicate_policy) +
                     " is not supported: it would change the data; the runtime reads 'error' and "
                     "'require_observation_id'");
        }
    }
    std::set<std::string> ids;
    const json& variables = root.at("variables");
    for (std::size_t i = 0; i < variables.size(); ++i) {
        const json& entry = variables.at(i);
        const std::string at = "/variables/" + std::to_string(i);
        TargetVariable variable;
        variable.id = entry.at("id").get<std::string>();
        variable.unit = entry.at("unit").get<std::string>();
        if (const auto it = entry.find("role"); it != entry.end()) {
            variable.role = it->get<std::string>();
        }
        if (const auto it = entry.find("lower_bound"); it != entry.end()) {
            variable.lower_bound = it->get<double>();
        }
        if (const auto it = entry.find("upper_bound"); it != entry.end()) {
            variable.upper_bound = it->get<double>();
        }
        if (!ids.insert(variable.id).second) {
            fail(document, ErrorCode::TARGET_DATA_INVALID, at + "/id",
                 "the variable id " + single_quoted(variable.id) + " appears twice");
        }
        if (variable.lower_bound && variable.upper_bound &&
            *variable.lower_bound > *variable.upper_bound) {
            fail(document, ErrorCode::TARGET_DATA_INVALID, at,
                 "the variable " + single_quoted(variable.id) + " has lower_bound " +
                     json(*variable.lower_bound).dump() + " above upper_bound " +
                     json(*variable.upper_bound).dump());
        }
        descriptor.variables.push_back(std::move(variable));
    }
    descriptor.digest = sha256_hex(canonical_json(digest_subset(root)));
    return descriptor;
}

} // namespace praenora::detail
