#include "document/schema.hpp"

#include "core/semver.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"
#include "platform/platform.hpp"

#include <praenora/runtime/error.hpp>

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace praenora::detail {

namespace {

namespace jsch = jsoncons::jsonschema;
using JcJson = jsoncons::json;

enum class Dialect { draft07, draft202012 };

constexpr std::string_view kDraft07 = "http://json-schema.org/draft-07/schema";
constexpr std::string_view kDraft202012 = "https://json-schema.org/draft/2020-12/schema";
constexpr std::string_view kUrnPrefix = "urn:praenora:schema:";
constexpr std::string_view kUuidPattern =
    "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$";

// A schema nests, and a chain of references runs, at most this deep before the reference walk
// refuses it: a schema this deep is not one that anyone wrote by hand.
constexpr int kMaxSchemaDepth = 256;

// The limits that make the validator's stack use bounded (ADR-0043). The validator and the
// compiler recurse: validation once per instance level and once per in-place applicator (`$ref`,
// `allOf`, `anyOf`, `not`, `if`, ...) between two levels; compiling once per level of JSON.
// - An instance nests at most the document limit plus the levels an envelope adds.
// - Applicators that apply to the same value chain at most kMaxInPlaceChain deep; real schemas
//   have a handful. The schema handed to the library has up to kRewriteHops more (the wrapper that
//   moves a default, the `allOf` of a uuid pattern), and a `$ref` to a meta-schema counts as
//   kMetaHops.
// - Work that could use more than a small stack runs on a thread with a stack sized for it:
//   kStackBase + (instance levels + 1) x (kLevelBytes + (chain + kRewriteHops) x kHopBytes), and
//   for compiling kStackBase + JSON levels x kCompileBytesPerLevel. The constants are twice the
//   worst cost measured in a Debug build over every keyword that recurses (ADR-0043): at most 4.7
//   KiB a level and 2.8 KiB a hop for validation (a meta-schema: 6.1 KiB a level), 12.7 KiB a JSON
//   level for compiling.
// - Work that needs at most kInlineBytes stays on the caller's thread: the tests run the worst
//   such case on a caller with 256 KiB, half of what a secondary thread has on macOS.
constexpr int kMaxInstanceDepth = kMaxNestingDepth + 8;
constexpr int kMaxInPlaceChain = 32;
constexpr int kRewriteHops = 2;
constexpr int kMetaHops = 4;
constexpr std::size_t kLevelBytes = 4 * 1024;
constexpr std::size_t kHopBytes = 6 * 1024;
constexpr std::size_t kStackBase = 512 * 1024;
constexpr std::size_t kInlineBytes = 192 * 1024;
constexpr std::size_t kCompileBytesPerLevel = 28 * 1024;
constexpr int kInlineSchemaDepth = 8;

// What the tests may change (set_stack_tuning_for_testing).
std::atomic<std::size_t> g_inline_bytes{kInlineBytes};
std::atomic<int> g_stack_percent{100};
std::atomic<int> g_inline_schema_depth{kInlineSchemaDepth};
std::atomic<bool> g_thread_failure{false};

// Runs `task` on a thread with `bytes` of stack (see run_on_thread_with_stack).
void on_sized_thread(std::size_t bytes, const std::function<void()>& task) {
    if (g_thread_failure.load()) {
        throw ThreadStartError("injected failure to start a thread");
    }
    run_on_thread_with_stack(bytes, task);
}

std::size_t stack_for_validation(int deepest, int chain) {
    return static_cast<std::size_t>(deepest + 1) *
           (kLevelBytes + static_cast<std::size_t>(chain + kRewriteHops) * kHopBytes);
}

std::size_t scaled(std::size_t bytes) {
    return bytes / 100 * static_cast<std::size_t>(g_stack_percent.load());
}

[[noreturn]] void schema_violation(const std::string& message, const std::string& pointer = "") {
    SourceLocation location;
    location.pointer = pointer;
    throw Error(ErrorCode::SCHEMA_VIOLATION, message, location);
}

// What the reference walk cannot follow or finish; becomes a SCHEMA_VIOLATION of the schema.
struct WalkRefusal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct NestingTooDeep : std::runtime_error {
    NestingTooDeep()
        : std::runtime_error("the instance nests deeper than " + std::to_string(kMaxInstanceDepth) +
                             " levels, which the validator does not accept") {}
};

const char* dialect_name(Dialect dialect) {
    return dialect == Dialect::draft07 ? "draft-07" : "2020-12";
}

// The one place that says which `$schema` values are which dialect: draft-07 with or without the
// empty fragment, 2020-12 exactly.
std::optional<Dialect> dialect_from_uri(std::string_view uri) {
    if (uri == kDraft07 || (uri.size() == kDraft07.size() + 1 &&
                            uri.compare(0, kDraft07.size(), kDraft07) == 0 && uri.back() == '#')) {
        return Dialect::draft07;
    }
    if (uri == kDraft202012) {
        return Dialect::draft202012;
    }
    return std::nullopt;
}

// The dialect a schema declares. Without `$schema` it is draft-07, the dialect Praenora schemas
// have always had (ADR-0043). Anything but draft-07 and 2020-12 is refused.
Dialect dialect_of(const nlohmann::json& schema, const std::string& subject) {
    if (!schema.is_object()) {
        return Dialect::draft07;
    }
    const auto declared = schema.find("$schema");
    if (declared == schema.end()) {
        return Dialect::draft07;
    }
    if (declared->is_string()) {
        if (const auto dialect = dialect_from_uri(declared->get_ref<const std::string&>())) {
            return *dialect;
        }
    }
    schema_violation(subject + " declares $schema " + declared->dump() +
                         "; Praenora uses JSON Schema draft-07 (" + std::string(kDraft07) +
                         "#) or 2020-12 (" + std::string(kDraft202012) + ")",
                     "/$schema");
}

// Dialect of a schema document that is not the one being compiled (an embedded one).
Dialect declared_dialect(const nlohmann::json& schema) {
    if (schema.is_object()) {
        const auto declared = schema.find("$schema");
        if (declared != schema.end() && declared->is_string()) {
            if (const auto dialect = dialect_from_uri(declared->get_ref<const std::string&>())) {
                return *dialect;
            }
        }
    }
    return Dialect::draft07;
}

jsch::evaluation_options options_for(Dialect default_dialect) {
    // Formats are assertions in both dialects, as ADR-0008 had them (2020-12 treats them as
    // annotations unless asked); formats the library does not know are accepted.
    return jsch::evaluation_options{}
        .default_version(default_dialect == Dialect::draft07 ? jsch::schema_version::draft7()
                                                             : jsch::schema_version::draft202012())
        .require_format_validation(true);
}

// nlohmann/json to jsoncons, node by node (no text in between). A number keeps its kind, and a
// number that is not finite, which JSON cannot hold, becomes null as it did in text.
JcJson to_jsoncons(const nlohmann::json& value, int depth = 0, int* deepest = nullptr) {
    if (depth > kMaxInstanceDepth) {
        throw NestingTooDeep();
    }
    if (deepest != nullptr && depth > *deepest) {
        *deepest = depth;
    }
    switch (value.type()) {
    case nlohmann::json::value_t::boolean:
        return JcJson(value.get<bool>());
    case nlohmann::json::value_t::number_integer:
        return JcJson(value.get<std::int64_t>());
    case nlohmann::json::value_t::number_unsigned:
        return JcJson(value.get<std::uint64_t>());
    case nlohmann::json::value_t::number_float: {
        const double number = value.get<double>();
        return std::isfinite(number) ? JcJson(number) : JcJson::null();
    }
    case nlohmann::json::value_t::string:
        return JcJson(value.get_ref<const std::string&>());
    case nlohmann::json::value_t::array: {
        JcJson array(jsoncons::json_array_arg);
        array.reserve(value.size());
        for (const auto& element : value) {
            array.push_back(to_jsoncons(element, depth + 1, deepest));
        }
        return array;
    }
    case nlohmann::json::value_t::object: {
        JcJson object(jsoncons::json_object_arg);
        for (const auto& item : value.items()) {
            object.insert_or_assign(item.key(), to_jsoncons(item.value(), depth + 1, deepest));
        }
        return object;
    }
    default:
        return JcJson::null();
    }
}

nlohmann::json from_jsoncons(const JcJson& value) {
    if (value.is_bool()) {
        return value.as<bool>();
    }
    if (value.is_int64()) {
        return value.as<std::int64_t>();
    }
    if (value.is_uint64()) {
        return value.as<std::uint64_t>();
    }
    if (value.is_double()) {
        return value.as<double>();
    }
    if (value.is_string()) {
        return value.as<std::string>();
    }
    if (value.is_array()) {
        nlohmann::json array = nlohmann::json::array();
        for (const auto& element : value.array_range()) {
            array.push_back(from_jsoncons(element));
        }
        return array;
    }
    if (value.is_object()) {
        nlohmann::json object = nlohmann::json::object();
        for (const auto& member : value.object_range()) {
            object[std::string(member.key())] = from_jsoncons(member.value());
        }
        return object;
    }
    return nullptr;
}

// The text of an embedded Praenora schema named by a urn:praenora:schema:<kind>:<major>.<minor>
// reference, or nullopt when `location` names none (or no such schema).
std::optional<nlohmann::json> load_urn(const std::string& location) {
    if (location.compare(0, kUrnPrefix.size(), kUrnPrefix) != 0) {
        return std::nullopt;
    }
    const std::string rest = location.substr(kUrnPrefix.size());
    const auto colon = rest.rfind(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    try {
        const SemVer version = parse_semver(rest.substr(colon + 1) + ".0");
        if (!version.prerelease.empty() || !version.build.empty()) {
            return std::nullopt;
        }
        const auto text = SchemaRegistry::builtin().text_for_kind(rest.substr(0, colon), version);
        return nlohmann::json::parse(text.begin(), text.end());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------------------------
// Subschemas. The keywords whose values are schemas, shared by the passes below.

std::string child_pointer(const std::string& pointer, const std::string& key) {
    return pointer + "/" + json_pointer_escape(key);
}

struct Child {
    std::string pointer;
    bool in_place; // applies to the same instance value as its parent
};

// The subschemas of the schema `node` at `pointer`. Definitions (`definitions`, `$defs`) are
// listed only with `include_definitions`: they apply only where a reference names them. In
// draft-07 the keywords beside a "$ref" are ignored, so a `$ref` leaves none (unless asked for
// them all, which indexing does).
std::vector<Child> subschemas(const nlohmann::json& node, const std::string& pointer,
                              Dialect dialect, bool include_definitions) {
    std::vector<Child> children;
    if (!node.is_object()) {
        return children;
    }
    if (!include_definitions && dialect == Dialect::draft07) {
        const auto ref = node.find("$ref");
        if (ref != node.end() && ref->is_string()) {
            return children;
        }
    }
    for (const auto& item : node.items()) {
        const std::string& key = item.key();
        const nlohmann::json& value = item.value();
        const std::string base = child_pointer(pointer, key);
        if (key == "not" || key == "if" || key == "then" || key == "else") {
            children.push_back({base, true});
        } else if (key == "allOf" || key == "anyOf" || key == "oneOf") {
            for (std::size_t i = 0; value.is_array() && i < value.size(); ++i) {
                children.push_back({base + "/" + std::to_string(i), true});
            }
        } else if (key == "dependentSchemas" || key == "dependencies") {
            for (const auto& entry : value.items()) {
                if (entry.value().is_object() || entry.value().is_boolean()) {
                    children.push_back({child_pointer(base, entry.key()), true});
                }
            }
        } else if (key == "properties" || key == "patternProperties") {
            for (const auto& entry : value.items()) {
                children.push_back({child_pointer(base, entry.key()), false});
            }
        } else if (include_definitions && (key == "definitions" || key == "$defs")) {
            for (const auto& entry : value.items()) {
                children.push_back({child_pointer(base, entry.key()), false});
            }
        } else if (key == "prefixItems" || key == "items" || key == "additionalItems") {
            if (value.is_array()) {
                for (std::size_t i = 0; i < value.size(); ++i) {
                    children.push_back({base + "/" + std::to_string(i), false});
                }
            } else {
                children.push_back({base, false});
            }
        } else if (key == "additionalProperties" || key == "contains" || key == "propertyNames" ||
                   key == "unevaluatedItems" || key == "unevaluatedProperties") {
            children.push_back({base, false});
        }
    }
    return children;
}

// Calls `fn(node, pointer)` on a schema and, recursively, on every subschema (definitions too).
template <typename Fn>
void visit_schemas(nlohmann::json& node, const std::string& pointer, Dialect dialect, int depth,
                   Fn&& fn) {
    if (depth > kMaxSchemaDepth) {
        throw WalkRefusal("the schema nests deeper than " + std::to_string(kMaxSchemaDepth) +
                          " levels");
    }
    fn(node, pointer);
    // `fn` may replace subschemas but never the node's own keys that name them, so the pointers
    // of the original structure stay valid.
    for (const Child& child : subschemas(node, pointer, dialect, true)) {
        nlohmann::json* target = &node;
        try {
            target = &node.at(nlohmann::json::json_pointer(child.pointer.substr(pointer.size())));
        } catch (const std::exception&) {
            continue;
        }
        visit_schemas(*target, child.pointer, dialect, depth + 1, fn);
    }
}

// ---------------------------------------------------------------------------------------------
// URIs and the graph of references.

bool has_scheme(const std::string& uri) {
    if (uri.empty() || !std::isalpha(static_cast<unsigned char>(uri[0]))) {
        return false;
    }
    for (std::size_t i = 1; i < uri.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(uri[i]);
        if (c == ':') {
            return true;
        }
        if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') {
            return false;
        }
    }
    return false;
}

std::string without_fragment(const std::string& uri) { return uri.substr(0, uri.find('#')); }

// `ref` resolved against `base` (RFC 3986, without dot segments).
std::string resolve_uri(const std::string& ref, const std::string& base) {
    if (has_scheme(ref)) {
        return ref;
    }
    const std::string base_without_fragment = without_fragment(base);
    if (!ref.empty() && ref[0] == '#') {
        return base_without_fragment + ref;
    }
    if (base_without_fragment.empty() || base_without_fragment.compare(0, 4, "urn:") == 0) {
        return ref;
    }
    const auto authority = base_without_fragment.find("://");
    if (ref.compare(0, 2, "//") == 0 && authority != std::string::npos) {
        return base_without_fragment.substr(0, authority + 1) + ref;
    }
    const std::size_t origin_end =
        authority == std::string::npos ? std::string::npos
                                       : base_without_fragment.find('/', authority + 3);
    if (!ref.empty() && ref[0] == '/') {
        return (origin_end == std::string::npos ? base_without_fragment
                                                : base_without_fragment.substr(0, origin_end)) +
               ref;
    }
    const std::string path_free = base_without_fragment.substr(0, base_without_fragment.find('?'));
    const auto last_slash = path_free.rfind('/');
    if (last_slash == std::string::npos || (authority != std::string::npos && last_slash < authority + 3)) {
        return path_free + "/" + ref;
    }
    return path_free.substr(0, last_slash + 1) + ref;
}

std::string percent_decode(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() &&
            std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

bool is_meta_schema_uri(const std::string& uri) {
    return uri == kDraft07 || uri == kDraft202012;
}

// Where the references of a schema lead. Both libraries (and every evaluator of jsoncons)
// overflow the stack on a schema that refers to itself in place, such as {"$ref": "#"}, two
// definitions that refer to each other, an anchor that names its own schema, or a dynamic
// reference to its own anchor, so such schemas are refused before they are compiled. A cycle is
// harmless, and common (trees), when it passes a keyword that descends into the instance. A
// reference the walk cannot resolve is refused too: it could hide a cycle, and the library would
// refuse it, or follow it where the walk cannot.
class SchemaGraph {
public:
    struct Node {
        std::string document, pointer;
        bool operator<(const Node& other) const {
            return std::tie(document, pointer) < std::tie(other.document, other.pointer);
        }
    };

    SchemaGraph(const nlohmann::json& root, Dialect dialect) { add_document("", root, dialect); }

    struct Analysis {
        std::optional<std::string> cycle; // a subschema that reaches itself in place
        int longest_chain = 0;            // most in-place applicators between two instance levels
    };

    // Finds a subschema that reaches itself through in-place applicators alone and, without one,
    // the longest chain of them. Throws WalkRefusal for what it cannot follow, for a schema
    // nested or chained too deep, and for a chain longer than kMaxInPlaceChain.
    Analysis analyze() {
        std::map<Node, std::vector<Node>> successors;
        std::deque<std::pair<Node, int>> queue{{Node{"", ""}, 0}};
        while (!queue.empty()) {
            const std::pair<Node, int> current = queue.front();
            queue.pop_front();
            const Node node = current.first;
            const int depth = current.second;
            if (depth > kMaxSchemaDepth) {
                throw WalkRefusal("the schema nests or chains references deeper than " +
                                  std::to_string(kMaxSchemaDepth) + " levels, at " + describe(node));
            }
            if (successors.count(node) != 0) {
                continue;
            }
            std::vector<Node>& in_place = successors[node];
            const nlohmann::json* schema = at(node);
            if (schema == nullptr || !schema->is_object()) {
                continue;
            }
            const Document& document = documents_.at(node.document);
            const auto edge = [&](const Node& next, bool is_in_place) {
                if (is_in_place) {
                    in_place.push_back(next);
                }
                queue.emplace_back(next, depth + 1);
            };
            const auto ref = schema->find("$ref");
            const bool has_ref = ref != schema->end() && ref->is_string();
            if (has_ref) {
                if (const auto next = resolve(node, ref->get_ref<const std::string&>())) {
                    edge(*next, true);
                }
            }
            if (document.dialect == Dialect::draft202012) {
                const auto dynamic = schema->find("$dynamicRef");
                if (dynamic != schema->end() && dynamic->is_string()) {
                    for (const Node& next : resolve_dynamic(node, dynamic->get_ref<const std::string&>())) {
                        edge(next, true);
                    }
                }
            }
            for (const Child& child : subschemas(*schema, node.pointer, document.dialect, false)) {
                edge(Node{node.document, child.pointer}, child.in_place);
            }
        }
        std::map<Node, int> state; // 1 on the stack, 2 done
        for (const auto& entry : successors) {
            if (state[entry.first] == 0) {
                if (const auto cycle = visit(entry.first, successors, state)) {
                    return Analysis{describe(*cycle), 0};
                }
            }
        }
        // No cycle: the in-place edges form a DAG, so the longest chain is finite.
        std::map<Node, int> longest;
        Analysis result;
        for (const auto& entry : successors) {
            result.longest_chain = std::max(result.longest_chain, chain_from(entry.first, successors, longest));
        }
        return result;
    }

    // The `default` that the property schema at `from` (a `$ref`) inherits from the schema it
    // points at, following references, or nullopt. The previous validator applied such a default
    // to an absent property; jsoncons does not, so a second compilation moves it to the property.
    std::optional<nlohmann::json> default_through(const Node& from, const std::string& ref) {
        std::string current = ref;
        Node here = from;
        for (int hop = 0; hop < 8; ++hop) {
            std::optional<Node> next;
            try {
                next = resolve(here, current);
            } catch (const WalkRefusal&) {
                return std::nullopt;
            }
            if (!next) {
                return std::nullopt;
            }
            const nlohmann::json* schema = at(*next);
            if (schema == nullptr || !schema->is_object()) {
                return std::nullopt;
            }
            if (const auto found = schema->find("default"); found != schema->end()) {
                return *found;
            }
            const auto onward = schema->find("$ref");
            if (onward == schema->end() || !onward->is_string()) {
                return std::nullopt;
            }
            here = *next;
            current = onward->get<std::string>();
        }
        return std::nullopt;
    }

private:
    struct Document {
        nlohmann::json schema;
        Dialect dialect = Dialect::draft07;
        std::map<std::string, std::string> bases;     // subschema pointer -> its base URI
        std::map<std::string, std::string> resources; // base URI -> pointer of the resource
        std::map<std::pair<std::string, std::string>, std::string> anchors; // (base, name) -> pointer
        std::map<std::string, std::vector<std::string>> dynamic_anchors;    // name -> pointers
    };

    static std::string describe(const Node& node) {
        if (node.document.empty()) {
            return node.pointer.empty() ? "(root)" : node.pointer;
        }
        return node.document + "#" + node.pointer;
    }

    // Records, for every subschema, its base URI, the resources (`$id`) and the anchors.
    void index(Document& document, const nlohmann::json& node, const std::string& pointer,
               std::string base, int depth) {
        if (depth > kMaxSchemaDepth) {
            throw WalkRefusal("the schema nests deeper than " + std::to_string(kMaxSchemaDepth) +
                              " levels");
        }
        if (node.is_object()) {
            const auto ref_here = node.find("$ref");
            const bool ignores_id = document.dialect == Dialect::draft07 && ref_here != node.end() &&
                                    ref_here->is_string(); // draft-07 reads nothing beside a $ref
            const auto id = ignores_id ? node.end() : node.find("$id");
            if (id != node.end() && id->is_string()) {
                const std::string& value = id->get_ref<const std::string&>();
                if (!value.empty() && value[0] == '#') {
                    document.anchors[{base, value.substr(1)}] = pointer; // draft-07 plain name
                } else if (!value.empty()) {
                    base = without_fragment(resolve_uri(value, base));
                    document.resources[base] = pointer;
                }
            }
            for (const char* keyword : {"$anchor", "$dynamicAnchor"}) {
                const auto anchor = node.find(keyword);
                if (anchor != node.end() && anchor->is_string()) {
                    document.anchors[{base, anchor->get<std::string>()}] = pointer;
                    if (std::string(keyword) == "$dynamicAnchor") {
                        document.dynamic_anchors[anchor->get<std::string>()].push_back(pointer);
                    }
                }
            }
        }
        document.bases[pointer] = base;
        for (const Child& child : subschemas(node, pointer, document.dialect, true)) {
            try {
                index(document, node.at(nlohmann::json::json_pointer(child.pointer.substr(pointer.size()))),
                      child.pointer, base, depth + 1);
            } catch (const nlohmann::json::exception&) {
            }
        }
    }

    void add_document(const std::string& key, const nlohmann::json& schema, Dialect dialect) {
        Document& document = documents_[key];
        document.schema = schema;
        document.dialect = dialect;
        index(document, document.schema, "", "", 0);
        document.resources[""] = "";
    }

    const nlohmann::json* at(const Node& node) const {
        const auto document = documents_.find(node.document);
        if (document == documents_.end()) {
            return nullptr;
        }
        try {
            const nlohmann::json::json_pointer pointer(node.pointer);
            if (!document->second.schema.contains(pointer)) {
                return nullptr;
            }
            return &document->second.schema.at(pointer);
        } catch (const std::exception&) {
            return nullptr;
        }
    }

    // The node a reference names; nullopt for a reference that ends in a meta-schema, whose own
    // recursion goes through the instance. Throws WalkRefusal for what cannot be followed.
    std::optional<Node> resolve(const Node& from, const std::string& ref) {
        const Document& source = documents_.at(from.document);
        const auto base = source.bases.find(from.pointer);
        const std::string absolute = resolve_uri(ref, base == source.bases.end() ? "" : base->second);
        const std::string location = without_fragment(absolute);
        const auto hash = absolute.find('#');
        const std::string fragment = hash == std::string::npos ? "" : percent_decode(absolute.substr(hash + 1));
        if (is_meta_schema_uri(location)) {
            meta_references_.insert(from);
            return std::nullopt;
        }
        std::optional<Node> resource = find_resource(location);
        if (!resource) {
            if (const auto loaded = load_urn(location)) {
                add_document(location, *loaded, declared_dialect(*loaded));
                resource = find_resource(location);
            }
        }
        if (!resource) {
            throw WalkRefusal("the reference '" + ref + "' at " + describe(from) +
                              " names no schema that is part of this one or embedded in the runtime");
        }
        Node target = *resource;
        if (!fragment.empty()) {
            if (fragment[0] == '/') {
                target.pointer += fragment;
            } else {
                const Document& found = documents_.at(target.document);
                const auto anchor = found.anchors.find({location, fragment});
                if (anchor == found.anchors.end()) {
                    throw WalkRefusal("the reference '" + ref + "' at " + describe(from) +
                                      " names an anchor that this schema does not define");
                }
                target.pointer = anchor->second;
            }
        }
        if (at(target) == nullptr) {
            throw WalkRefusal("the reference '" + ref + "' at " + describe(from) +
                              " points at nothing in the schema");
        }
        return target;
    }

    std::optional<Node> find_resource(const std::string& location) const {
        for (const auto& entry : documents_) {
            const auto found = entry.second.resources.find(location);
            if (found != entry.second.resources.end()) {
                return Node{entry.first, found->second};
            }
        }
        return std::nullopt;
    }

    using Resource = std::pair<std::string, std::string>; // (document, base URI)

    Resource resource_of(const Node& node) const {
        const Document& document = documents_.at(node.document);
        const auto base = document.bases.find(node.pointer);
        return {node.document, base == document.bases.end() ? "" : base->second};
    }

    // The resources that lead to one another by `$ref`, and those reachable from the root: the
    // candidates of a dynamic reference. Built when the first dynamic reference needs it.
    void build_resource_graph() {
        if (resources_built_) {
            return;
        }
        resources_built_ = true;
        std::vector<std::string> keys;
        for (const auto& entry : documents_) {
            keys.push_back(entry.first);
        }
        for (std::size_t k = 0; k < keys.size(); ++k) {
            const Document snapshot = documents_.at(keys[k]);
            for (const auto& entry : snapshot.bases) {
                const nlohmann::json* schema = at(Node{keys[k], entry.first});
                if (schema == nullptr || !schema->is_object()) {
                    continue;
                }
                const auto ref = schema->find("$ref");
                if (ref == schema->end() || !ref->is_string()) {
                    continue;
                }
                try {
                    if (const auto target = resolve(Node{keys[k], entry.first}, ref->get_ref<const std::string&>())) {
                        links_[resource_of(Node{keys[k], entry.first})].insert(resource_of(*target));
                    }
                } catch (const WalkRefusal&) {
                    // Reported when the walk reaches it.
                }
            }
            for (const auto& entry : documents_) {
                if (std::find(keys.begin(), keys.end(), entry.first) == keys.end()) {
                    keys.push_back(entry.first);
                }
            }
        }
        std::deque<Resource> queue{resource_of(Node{"", ""})};
        from_root_.insert(queue.front());
        while (!queue.empty()) {
            const Resource current = queue.front();
            queue.pop_front();
            for (const Resource& next : links_[current]) {
                if (from_root_.insert(next).second) {
                    queue.push_back(next);
                }
            }
        }
    }

    // A dynamic reference goes to the outermost dynamic anchor of its name that is in the dynamic
    // scope, which a static walk cannot know. It counts the anchors of that name in the resource
    // that holds the reference and in the resources that lead to it from the root (the outer
    // scopes it could have been reached through); anchors in resources the root never reaches, or
    // that never lead to it, are not in its scope.
    std::vector<Node> resolve_dynamic(const Node& from, const std::string& ref) {
        const auto hash = ref.find('#');
        const std::string fragment = hash == std::string::npos ? "" : ref.substr(hash + 1);
        std::vector<Node> targets;
        if (!fragment.empty() && fragment[0] != '/') {
            build_resource_graph();
            const Resource here = resource_of(from);
            std::set<Resource> leading{here}; // resources from which `here` can be reached
            std::deque<Resource> queue{here};
            while (!queue.empty()) {
                const Resource current = queue.front();
                queue.pop_front();
                for (const auto& link : links_) {
                    if (link.second.count(current) != 0 && leading.insert(link.first).second) {
                        queue.push_back(link.first);
                    }
                }
            }
            for (const auto& entry : documents_) {
                const auto found = entry.second.dynamic_anchors.find(fragment);
                if (found == entry.second.dynamic_anchors.end()) {
                    continue;
                }
                for (const std::string& pointer : found->second) {
                    const Resource anchor = resource_of(Node{entry.first, pointer});
                    if (anchor == here || (from_root_.count(anchor) != 0 && leading.count(anchor) != 0)) {
                        targets.push_back(Node{entry.first, pointer});
                    }
                }
            }
        }
        if (targets.empty()) {
            if (const auto next = resolve(from, ref)) {
                targets.push_back(*next);
            }
        }
        return targets;
    }

    // The number of in-place edges on the longest path from `node` (the graph has no cycle).
    int chain_from(const Node& node, const std::map<Node, std::vector<Node>>& successors,
                   std::map<Node, int>& memo) {
        const auto known = memo.find(node);
        if (known != memo.end()) {
            return known->second;
        }
        int longest = meta_references_.count(node) != 0 ? kMetaHops : 0;
        const auto found = successors.find(node);
        if (found != successors.end()) {
            for (const Node& next : found->second) {
                longest = std::max(longest, 1 + chain_from(next, successors, memo));
                if (longest > kMaxInPlaceChain) {
                    throw WalkRefusal("applicators that apply to the same value (`$ref`, `allOf`, "
                                      "`anyOf`, `not`, `if`, ...) chain more than " +
                                      std::to_string(kMaxInPlaceChain) + " deep, at " + describe(node));
                }
            }
        }
        memo[node] = longest;
        return longest;
    }

    // Depth-first search over the in-place edges; returns a node on a cycle.
    static std::optional<Node> visit(const Node& root, const std::map<Node, std::vector<Node>>& successors,
                                     std::map<Node, int>& state) {
        struct Frame {
            Node node;
            std::size_t next;
        };
        std::vector<Frame> stack{{root, 0}};
        state[root] = 1;
        const std::vector<Node> none;
        while (!stack.empty()) {
            Frame& frame = stack.back();
            const auto found = successors.find(frame.node);
            const std::vector<Node>& next = found == successors.end() ? none : found->second;
            if (frame.next == next.size()) {
                state[frame.node] = 2;
                stack.pop_back();
                continue;
            }
            const Node successor = next[frame.next++];
            const int seen = state[successor];
            if (seen == 1) {
                return successor;
            }
            if (seen == 0) {
                state[successor] = 1;
                stack.push_back({successor, 0});
            }
        }
        return std::nullopt;
    }

    std::map<std::string, Document> documents_;
    std::set<Node> meta_references_; // nodes with a `$ref` to a meta-schema
    bool resources_built_ = false;
    std::map<Resource, std::set<Resource>> links_; // resource -> resources it refers to
    std::set<Resource> from_root_;
};

// ---------------------------------------------------------------------------------------------
// Rewrites of the schema that is handed to jsoncons.

// jsoncons does not check the "uuid" format; the previous validator did. A pattern says the same.
void add_uuid_patterns(nlohmann::json& schema, Dialect dialect) {
    visit_schemas(schema, "", dialect, 0, [&](nlohmann::json& node, const std::string&) {
        if (!node.is_object()) {
            return;
        }
        const auto format = node.find("format");
        if (format == node.end() || !format->is_string() || *format != "uuid") {
            return;
        }
        if (dialect == Dialect::draft07) {
            const auto ref = node.find("$ref");
            if (ref != node.end() && ref->is_string()) {
                return; // draft-07 ignores the format beside a $ref
            }
        }
        if (node.find("pattern") == node.end()) {
            node["pattern"] = std::string(kUuidPattern);
            return;
        }
        auto& all_of = node["allOf"];
        if (!all_of.is_array()) {
            all_of = nlohmann::json::array();
        }
        all_of.push_back({{"pattern", std::string(kUuidPattern)}});
    });
}

// A property whose schema is only a "$ref" gets, for an absent property, the `default` of the
// schema it points at (the behaviour of ADR-0008's validator, which module authors use: a
// definition "fraction" with a default, referenced by several properties). jsoncons applies only
// a `default` in the property's own schema, so a copy of the schema moves the default there,
// {"$ref": X} becoming {"allOf": [{"$ref": X}], "default": D}, and is used to compute defaults
// only; the schema that validates is the original, so messages and their order do not change.
// Draft-07 ignores the other keywords beside a "$ref", so they are dropped; 2020-12 applies
// them, so they stay. Returns whether anything moved.
bool move_reference_defaults(nlohmann::json& schema, Dialect dialect) {
    const nlohmann::json original = schema;
    SchemaGraph graph(original, dialect);
    bool moved_any = false;
    visit_schemas(schema, "", dialect, 0, [&](nlohmann::json& node, const std::string& pointer) {
        const auto properties = node.find("properties");
        if (!node.is_object() || properties == node.end() || !properties->is_object()) {
            return;
        }
        for (auto& entry : properties->items()) {
            nlohmann::json& property = entry.value();
            if (!property.is_object() || property.find("default") != property.end()) {
                continue;
            }
            const auto ref = property.find("$ref");
            if (ref == property.end() || !ref->is_string()) {
                continue;
            }
            const SchemaGraph::Node from{
                "", child_pointer(child_pointer(pointer, "properties"), entry.key())};
            const auto inherited = graph.default_through(from, ref->get<std::string>());
            if (!inherited) {
                continue;
            }
            nlohmann::json moved = nlohmann::json::object();
            if (dialect == Dialect::draft202012) {
                for (const auto& sibling : property.items()) {
                    if (sibling.key() != "$ref") {
                        moved[sibling.key()] = sibling.value();
                    }
                }
            }
            moved["allOf"] = nlohmann::json::array({{{"$ref", *ref}}});
            moved["default"] = *inherited;
            property = std::move(moved);
            moved_any = true;
        }
    });
    return moved_any;
}

// The schema as jsoncons gets it: the schema plus the rewrite of the uuid format, and, for the
// schema that computes defaults, the defaults moved from references.
nlohmann::json prepared(const nlohmann::json& schema, Dialect dialect, bool move_defaults,
                        bool* moved = nullptr) {
    nlohmann::json copy = schema;
    bool did_move = false;
    if (move_defaults) {
        did_move = move_reference_defaults(copy, dialect);
    }
    add_uuid_patterns(copy, dialect);
    if (moved != nullptr) {
        *moved = did_move;
    }
    return copy;
}

// Resolves the external references of a schema while it compiles: embedded Praenora schemas by
// their URN. Everything else is refused (null), so compiling never reads files or the network;
// the meta-schemas of both dialects are built into the library.
JcJson resolve_reference(const jsoncons::uri& uri, bool move_defaults) {
    std::string location = uri.string();
    location = location.substr(0, location.find('#'));
    if (const auto loaded = load_urn(location)) {
        return to_jsoncons(prepared(*loaded, declared_dialect(*loaded), move_defaults));
    }
    return JcJson::null();
}

// ---------------------------------------------------------------------------------------------
// Issues.

// One validation message and its nested ones as flat issues, parent first. jsoncons reports an
// `anyOf` or `oneOf` failure at that node and nests the causes in details(). Two messages are
// refined to point at the property, as ADR-0008 does for the previous validator: a property
// that `additionalProperties` rejects (already reported at the property), and one that fails the
// schema of `additionalProperties` (reported at the object).
struct FlatIssue {
    SchemaIssue issue;
    bool leaf;           // no nested causes
    std::string keyword; // the schema keyword that failed
};

// The texts of jsoncons' messages that the adapter reads. A test pins them (LibraryMessageTexts...),
// so a newer library that words them otherwise fails loudly; the build asks for exactly 1.10.0.
constexpr std::string_view kNotMatchPattern = " does not match pattern '";

bool is_uuid_pattern_message(const std::string& text) {
    return text.find(kNotMatchPattern) != std::string::npos &&
           text.find(kUuidPattern) != std::string::npos;
}

void flatten(const jsch::validation_message& message, std::vector<FlatIssue>& out) {
    // The uuid format is checked by a pattern: report it as the format, and without the `allOf`
    // that wraps it beside a pattern of the schema's own.
    if (message.keyword() == "allOf" && message.details().size() == 1 &&
        is_uuid_pattern_message(message.details().front().message())) {
        flatten(message.details().front(), out);
        return;
    }
    constexpr std::string_view kOpen = "Additional property '";
    constexpr std::string_view kNotAllowed = "' not allowed by schema.";
    constexpr std::string_view kInvalid = "' found but was invalid.";
    SchemaIssue issue{message.instance_location().to_string(), message.message()};
    const std::string& text = message.message();
    const auto ends_with = [&](std::string_view tail) {
        return text.size() >= kOpen.size() + tail.size() &&
               text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
    };
    if (text.compare(0, kOpen.size(), kOpen) == 0) {
        if (ends_with(kNotAllowed)) {
            const std::string key = text.substr(kOpen.size(), text.size() - kOpen.size() - kNotAllowed.size());
            issue.message = "property '" + key + "' is not allowed";
        } else if (ends_with(kInvalid)) {
            const std::string key = text.substr(kOpen.size(), text.size() - kOpen.size() - kInvalid.size());
            issue.pointer += "/" + json_pointer_escape(key);
            issue.message = "property '" + key + "' is invalid";
        }
    }
    if (is_uuid_pattern_message(text)) {
        issue.message = text.substr(0, text.find(kNotMatchPattern)) + " is not a valid UUID";
    }
    out.push_back(FlatIssue{std::move(issue), message.details().empty(), message.keyword()});
    for (const auto& nested : message.details()) {
        flatten(nested, out);
    }
}

// The reference tokens of a JSON pointer, still escaped.
std::vector<std::string> pointer_segments(const std::string& pointer) {
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start < pointer.size()) {
        const std::size_t end = pointer.find('/', start + 1);
        segments.push_back(pointer.substr(start + 1, (end == std::string::npos ? pointer.size() : end) - start - 1));
        if (end == std::string::npos) {
            break;
        }
        start = end;
    }
    return segments;
}

bool is_number_segment(const std::string& segment) {
    return !segment.empty() &&
           std::all_of(segment.begin(), segment.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// A total order of reference tokens: numbers (by value, as length then digits) before the others
// (by text), so "2" < "10" < "1a".
bool segment_less(const std::string& x, const std::string& y) {
    const bool x_number = is_number_segment(x), y_number = is_number_segment(y);
    if (x_number != y_number) {
        return x_number;
    }
    if (x_number && x.size() != y.size()) {
        return x.size() < y.size();
    }
    return x < y;
}

// Orders pointers segment by segment, a parent before its children.
bool pointer_less(const std::string& a, const std::string& b) {
    const auto left = pointer_segments(a), right = pointer_segments(b);
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(), segment_less);
}

// The issues in an order that does not depend on how a dialect orders its keywords: by pointer (a
// parent first, array indices as numbers); at one pointer, issues without nested causes before
// those that wrap them (an `anyOf` failure), then by keyword and by message; what is still equal
// keeps the order the library reported.
void sort_flat(std::vector<FlatIssue>& issues) {
    std::stable_sort(issues.begin(), issues.end(), [](const FlatIssue& a, const FlatIssue& b) {
        if (a.issue.pointer != b.issue.pointer) {
            return pointer_less(a.issue.pointer, b.issue.pointer);
        }
        if (a.leaf != b.leaf) {
            return a.leaf;
        }
        if (a.keyword != b.keyword) {
            return a.keyword < b.keyword;
        }
        return a.issue.message < b.issue.message;
    });
}

// How many levels of objects and arrays `value` nests (a scalar is 0), without recursion.
int nesting_depth(const nlohmann::json& value) {
    int deepest = 0;
    std::vector<std::pair<const nlohmann::json*, int>> stack{{&value, 0}};
    while (!stack.empty()) {
        const auto [node, depth] = stack.back();
        stack.pop_back();
        deepest = std::max(deepest, depth);
        if (node->is_structured()) {
            for (const auto& child : *node) {
                stack.emplace_back(&child, depth + 1);
            }
        }
    }
    return deepest;
}

// The meta-schema of a dialect, compiled once; a `$ref` to it needs no resolver.
const jsch::json_schema<JcJson>& meta_schema(Dialect dialect) {
    static const jsch::json_schema<JcJson> draft07 = jsch::make_json_schema(
        JcJson::parse(R"({"$ref": "http://json-schema.org/draft-07/schema#"})"),
        options_for(Dialect::draft07));
    static const jsch::json_schema<JcJson> draft202012 = jsch::make_json_schema(
        JcJson::parse(R"({"$ref": "https://json-schema.org/draft/2020-12/schema"})"),
        options_for(Dialect::draft202012));
    return dialect == Dialect::draft07 ? draft07 : draft202012;
}

} // namespace

struct CompiledSchema::Impl {
    jsch::json_schema<JcJson> compiled;
    // The schema with the defaults moved from references, only when something moved.
    std::unique_ptr<jsch::json_schema<JcJson>> with_defaults;
    // The longest chain of in-place applicators (the stack use of a validation grows with it).
    int chain = 0;
    explicit Impl(jsch::json_schema<JcJson>&& schema) : compiled(std::move(schema)) {}
};

CompiledSchema::CompiledSchema(const nlohmann::json& schema, std::string name)
    : name_(std::move(name)) {
    const std::string subject = "schema '" + name_ + "'";
    // Documents are read with this nesting limit; compiling recurses once per level, and the
    // meta-schema check does too.
    const int depth = nesting_depth(schema);
    if (depth > kMaxNestingDepth) {
        schema_violation(subject + " nests deeper than " + std::to_string(kMaxNestingDepth) +
                         " levels of objects and arrays");
    }
    if (depth <= g_inline_schema_depth.load()) {
        build(schema);
        return;
    }
    // A deep schema compiles on a thread whose stack is sized for it (kCompileBytesPerLevel a
    // level). A thread that cannot be started is a resource error, not a defect of the schema.
    try {
        on_sized_thread(scaled(kStackBase + static_cast<std::size_t>(depth) * kCompileBytesPerLevel),
                        [&] { build(schema); });
    } catch (const ThreadStartError& error) {
        throw Error(ErrorCode::RESOURCE_EXHAUSTED, subject + " cannot be compiled: " + error.what());
    }
}

void CompiledSchema::build(const nlohmann::json& schema) {
    const std::string subject = "schema '" + name_ + "'";
    const Dialect dialect = dialect_of(schema, subject);

    JcJson converted;
    try {
        converted = to_jsoncons(schema);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        schema_violation(subject + " cannot be read as a JSON Schema: " + e.what());
    }

    // The schema is checked against the meta-schema of its dialect; the first problem that has no
    // nested cause is reported, with its place in the schema.
    std::vector<FlatIssue> meta_issues;
    try {
        meta_schema(dialect).validate(converted, [&](const jsch::validation_message& message) {
            flatten(message, meta_issues);
            return jsch::walk_state::advance;
        });
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        schema_violation(subject + " cannot be checked against the " +
                         std::string(dialect_name(dialect)) + " meta-schema: " + e.what());
    }
    if (!meta_issues.empty()) {
        const auto first = std::find_if(meta_issues.begin(), meta_issues.end(),
                                        [](const FlatIssue& issue) { return issue.leaf; });
        const SchemaIssue& problem = (first == meta_issues.end() ? meta_issues.front() : *first).issue;
        std::string message = subject + " is not a valid JSON Schema (" + dialect_name(dialect) +
                              "): " + (problem.pointer.empty() ? std::string("(root)") : problem.pointer) +
                              ": " + problem.message;
        if (meta_issues.size() > 1) {
            message += " (and " + std::to_string(meta_issues.size() - 1) + " more)";
        }
        schema_violation(message, problem.pointer);
    }

    nlohmann::json plain, moved_schema;
    bool moved = false;
    int chain = 0;
    try {
        const SchemaGraph::Analysis analysis = SchemaGraph(schema, dialect).analyze();
        if (analysis.cycle) {
            schema_violation(subject + " refers to itself without examining the instance, at " + *analysis.cycle +
                                 " (a schema that does, such as a tree, is fine)",
                             analysis.cycle->empty() || (*analysis.cycle)[0] != '/' ? "" : *analysis.cycle);
        }
        chain = analysis.longest_chain;
        plain = prepared(schema, dialect, false);
        moved_schema = prepared(schema, dialect, true, &moved);
    } catch (const WalkRefusal& refusal) {
        schema_violation(subject + ": " + refusal.what());
    }

    try {
        impl_ = std::make_unique<Impl>(jsch::make_json_schema(
            to_jsoncons(plain),
            [](const jsoncons::uri& uri) { return resolve_reference(uri, false); },
            options_for(dialect)));
        impl_->chain = chain;
        if (moved) {
            impl_->with_defaults = std::make_unique<jsch::json_schema<JcJson>>(jsch::make_json_schema(
                to_jsoncons(moved_schema),
                [](const jsoncons::uri& uri) { return resolve_reference(uri, true); },
                options_for(dialect)));
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        schema_violation(subject + " cannot be compiled: " + e.what());
    }
}

CompiledSchema::~CompiledSchema() = default;
CompiledSchema::CompiledSchema(CompiledSchema&&) noexcept = default;
CompiledSchema& CompiledSchema::operator=(CompiledSchema&&) noexcept = default;

std::vector<SchemaIssue> CompiledSchema::validate(const nlohmann::json& instance) const {
    std::vector<SchemaIssue> issues;
    run(instance, issues, nullptr);
    return issues;
}

nlohmann::json CompiledSchema::apply_defaults(const nlohmann::json& instance,
                                              std::vector<SchemaIssue>& issues) const {
    nlohmann::json patch;
    run(instance, issues, &patch);
    if (!patch.is_array() || patch.empty()) {
        return instance;
    }
    try {
        return instance.patch(patch);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        issues.push_back(SchemaIssue{"", std::string("cannot apply defaults: ") + e.what()});
        return instance;
    }
}

void CompiledSchema::run(const nlohmann::json& instance, std::vector<SchemaIssue>& issues,
                         nlohmann::json* patch) const {
    std::vector<FlatIssue> flat;
    try {
        int deepest = 0;
        const JcJson converted = to_jsoncons(instance, 0, &deepest);
        const auto validate_now = [&] {
            const auto report = [&](const jsch::validation_message& message) {
                flatten(message, flat);
                return jsch::walk_state::advance;
            };
            if (patch == nullptr) {
                // A valid instance, the common case on the per-message path, is settled without
                // collecting issues (the library stops at the first failure); only an invalid one
                // is validated again to report every issue.
                if (!impl_->compiled.is_valid(converted)) {
                    impl_->compiled.validate(converted, report);
                }
                return;
            }
            JcJson changes(jsoncons::json_array_arg);
            if (impl_->with_defaults) {
                impl_->compiled.validate(converted, report);
                impl_->with_defaults->validate(
                    converted, [](const jsch::validation_message&) { return jsch::walk_state::advance; },
                    changes);
            } else {
                impl_->compiled.validate(converted, report, changes);
            }
            *patch = from_jsoncons(changes);
        };
        // The stack a validation needs grows with the instance's depth and with the chain of
        // applicators between two levels; a big one runs on a thread sized for it (the common,
        // shallow case stays on the caller's thread).
        const std::size_t need = stack_for_validation(deepest, impl_->chain);
        if (need <= g_inline_bytes.load()) {
            validate_now();
        } else {
            on_sized_thread(scaled(kStackBase + need), validate_now);
        }
    } catch (const ThreadStartError& e) {
        // A resource error, like running out of memory: not an issue of the instance.
        throw Error(ErrorCode::RESOURCE_EXHAUSTED, std::string("cannot validate: ") + e.what());
    } catch (const NestingTooDeep& e) {
        flat.push_back(FlatIssue{SchemaIssue{"", e.what()}, true, ""});
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        flat.push_back(FlatIssue{SchemaIssue{"", std::string("validation failed: ") + e.what()}, true, ""});
    }
    sort_flat(flat);
    issues.clear();
    issues.reserve(flat.size());
    for (auto& entry : flat) {
        issues.push_back(std::move(entry.issue));
    }
}

const std::string& CompiledSchema::name() const { return name_; }

std::size_t CompiledSchema::stack_estimate(const nlohmann::json& instance) const {
    int deepest = 0;
    try {
        static_cast<void>(to_jsoncons(instance, 0, &deepest));
    } catch (const NestingTooDeep&) {
        return 0;
    }
    return stack_for_validation(deepest, impl_->chain);
}

void set_stack_tuning_for_testing(std::size_t inline_bytes, int percent, int inline_schema_depth) {
    g_inline_bytes = inline_bytes;
    g_stack_percent = percent;
    g_inline_schema_depth = inline_schema_depth;
}

void set_thread_failure_for_testing(bool fail) { g_thread_failure = fail; }

void set_stack_percent_for_testing(int percent) { g_stack_percent = percent; }

std::size_t stack_inline_budget_for_testing() { return g_inline_bytes.load(); }

void reset_stack_tuning_for_testing() {
    set_stack_tuning_for_testing(kInlineBytes, 100, kInlineSchemaDepth);
}

namespace {

// Walks the subschemas of `node` (a schema) and records those with keywords beside "$ref".
void find_ref_siblings(const nlohmann::json& node, const std::string& pointer,
                       std::vector<std::string>& found, int depth) {
    if (!node.is_object() || depth > kMaxSchemaDepth) {
        return;
    }
    const auto ref = node.find("$ref");
    if (ref != node.end() && ref->is_string()) {
        std::string ignored;
        for (const auto& item : node.items()) {
            const std::string& key = item.key();
            if (key != "$ref" && key != "description" && key != "title" && key != "$comment" &&
                key != "examples" && key != "$id" && key != "$schema") {
                ignored += (ignored.empty() ? "" : ", ") + key;
            }
        }
        if (!ignored.empty()) {
            found.push_back((pointer.empty() ? "(root)" : pointer) + " (" + ignored + ")");
        }
        return; // draft-07 reads nothing else here
    }
    for (const Child& child : subschemas(node, pointer, Dialect::draft202012, true)) {
        try {
            find_ref_siblings(node.at(nlohmann::json::json_pointer(child.pointer.substr(pointer.size()))),
                              child.pointer, found, depth + 1);
        } catch (const nlohmann::json::exception&) {
        }
    }
}

} // namespace

std::vector<std::string> ignored_ref_siblings(const nlohmann::json& schema) {
    std::vector<std::string> found;
    if (declared_dialect(schema) == Dialect::draft07) {
        find_ref_siblings(schema, "", found, 0);
    }
    return found;
}

namespace {

// The `$ref` strings anywhere in `node`.
void collect_refs(const nlohmann::json& node, std::vector<std::string>& refs) {
    if (node.is_object()) {
        for (const auto& item : node.items()) {
            if (item.key() == "$ref" && item.value().is_string()) {
                refs.push_back(item.value().get<std::string>());
            }
            collect_refs(item.value(), refs);
        }
    } else if (node.is_array()) {
        for (const auto& item : node) {
            collect_refs(item, refs);
        }
    }
}

// The pointers of the entries of the root `$defs` of `schema` that a reference reaches, from
// outside `$defs` or from an entry that is reached ("#/$defs/<name>", also into the entry). The
// export inlines what is referenced and erases `$defs`, so the rest never appears in a document.
std::set<std::string> reachable_defs(const nlohmann::json& schema) {
    std::set<std::string> reached;
    const auto defs = schema.find("$defs");
    if (defs == schema.end() || !defs->is_object()) {
        return reached;
    }
    std::vector<std::string> pending;
    for (const auto& item : schema.items()) {
        if (item.key() != "$defs") {
            collect_refs(item.value(), pending);
        }
    }
    constexpr std::string_view kPrefix = "#/$defs/";
    while (!pending.empty()) {
        const std::string ref = std::move(pending.back());
        pending.pop_back();
        if (ref.compare(0, kPrefix.size(), kPrefix) != 0) {
            continue;
        }
        const std::size_t end = ref.find('/', kPrefix.size());
        const std::string token =
            ref.substr(kPrefix.size(), end == std::string::npos ? end : end - kPrefix.size());
        std::string name = token;
        for (std::size_t at; (at = name.find("~1")) != std::string::npos;) {
            name.replace(at, 2, "/");
        }
        for (std::size_t at; (at = name.find("~0")) != std::string::npos;) {
            name.replace(at, 2, "~");
        }
        const auto entry = defs->find(name);
        if (entry != defs->end() && reached.insert("/$defs/" + json_pointer_escape(name)).second) {
            collect_refs(*entry, pending);
        }
    }
    return reached;
}

// Walks the subschemas of a 2020-12 `node` and records what AsyncAPI 3.0.0 cannot express. At the
// root, `reachable` limits the entries of `$defs` that are looked at.
void find_beyond_asyncapi(const nlohmann::json& node, const std::string& pointer,
                          std::vector<std::string>& found, int depth,
                          const std::set<std::string>* reachable = nullptr) {
    if (!node.is_object()) {
        return;
    }
    const std::string where = pointer.empty() ? "(root)" : pointer;
    if (depth > kMaxSchemaDepth) {
        found.push_back(where + " (nested deeper than " + std::to_string(kMaxSchemaDepth) +
                        " levels)");
        return;
    }
    static const char* const kUnsupported[] = {
        "prefixItems",      "unevaluatedItems", "unevaluatedProperties", "dependentRequired",
        "dependentSchemas", "$dynamicRef",      "$dynamicAnchor",        "$anchor",
        "minContains",      "maxContains"};
    for (const char* keyword : kUnsupported) {
        if (node.contains(keyword)) {
            found.push_back(where + " (" + keyword + ")");
        }
    }
    // The export inlines a schema into a document that has other schemas: an identifier other
    // than the root's would change what its references resolve against.
    if (depth > 0 && node.contains("$id")) {
        found.push_back(where + " ($id)");
    }
    // The AsyncAPI export follows a "$ref" and drops the keywords beside it.
    if (node.contains("$ref")) {
        std::string beside;
        for (const auto& item : node.items()) {
            const std::string& key = item.key();
            if (key != "$ref" && key != "description" && key != "title" && key != "$comment" &&
                key != "examples" && key != "$schema" && key != "$id") {
                beside += (beside.empty() ? "" : ", ") + key;
            }
        }
        if (!beside.empty()) {
            found.push_back(where + " (" + beside + " beside $ref)");
        }
    }
    for (const Child& child : subschemas(node, pointer, Dialect::draft202012, true)) {
        if (depth == 0 && reachable != nullptr && child.pointer.rfind("/$defs/", 0) == 0 &&
            reachable->count(child.pointer) == 0) {
            continue;
        }
        try {
            find_beyond_asyncapi(
                node.at(nlohmann::json::json_pointer(child.pointer.substr(pointer.size()))),
                child.pointer, found, depth + 1);
        } catch (const nlohmann::json::exception&) {
        }
    }
}

} // namespace

std::vector<std::string> beyond_asyncapi_subset(const nlohmann::json& schema) {
    std::vector<std::string> found;
    if (declared_dialect(schema) == Dialect::draft202012) {
        const std::set<std::string> reachable = reachable_defs(schema);
        find_beyond_asyncapi(schema, "", found, 0, &reachable);
    }
    return found;
}

} // namespace praenora::detail
