// A compiled JSON Schema (draft-07 or 2020-12), backed by jsoncons (the jsonschema extension).
#ifndef PRAENORA_RUNTIME_DOCUMENT_SCHEMA_HPP
#define PRAENORA_RUNTIME_DOCUMENT_SCHEMA_HPP

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace praenora::detail {

// One validation problem: where (a JSON pointer into the instance; "" is the whole instance) and
// what, in the validator's words.
struct SchemaIssue {
    std::string pointer, message;
};

class CompiledSchema {
public:
    // Compiles `schema`; `name` identifies it in messages (for example "workflow-config@0.1").
    // Throws praenora::Error(SCHEMA_VIOLATION) when `schema` is not a valid schema of its dialect:
    // `$schema` names draft-07 or 2020-12 (draft-07 when absent; anything else is refused), the
    // schema is checked against that dialect's meta-schema, a `$ref` (or `$dynamicRef`) that leads
    // back to itself without looking at the instance is refused, and every `$ref` must resolve
    // to something the reference walk can follow (pointers, anchors, nested `$id`s, embedded
    // schemas); a schema nested deeper than 128 levels, or whose in-place applicators chain more
    // than 32 deep, is refused. External
    // references may name embedded Praenora schemas (urn:praenora:schema:<kind>:<major>.<minor>)
    // or the meta-schema of a dialect, nothing else. `format` is asserted in both dialects for the
    // formats the library knows; other formats are accepted.
    CompiledSchema(const nlohmann::json& schema, std::string name);
    ~CompiledSchema();
    CompiledSchema(CompiledSchema&&) noexcept;
    CompiledSchema& operator=(CompiledSchema&&) noexcept;
    CompiledSchema(const CompiledSchema&) = delete;
    CompiledSchema& operator=(const CompiledSchema&) = delete;

    // Every problem of `instance`; empty when it is valid. In pointer order (a parent before its
    // children, array indices as numbers); at one pointer the issues without nested causes come
    // before those that wrap them, then by keyword and message. The tests compare the two dialects
    // over the rules that they name differently and find the same first issue; the order at one
    // pointer is otherwise the keyword names'. An instance nested deeper than 136 levels gives one
    // issue saying so. Safe to call from several threads.
    std::vector<SchemaIssue> validate(const nlohmann::json& instance) const;

    // Validates `instance`, replaces the content of `issues` with its problems, and returns the
    // instance with the schema's `default` values added for missing properties of the objects
    // that are present.
    nlohmann::json apply_defaults(const nlohmann::json& instance,
                                  std::vector<SchemaIssue>& issues) const;

    const std::string& name() const;

    // The stack, in bytes, that validating `instance` is expected to need (what sizes the thread of
    // a deep validation); exposed for tests and benchmarks.
    std::size_t stack_estimate(const nlohmann::json& instance) const;

private:
    // Validates and fills `issues`; with `patch`, also stores there the JSON patch that adds the
    // defaults.
    void run(const nlohmann::json& instance, std::vector<SchemaIssue>& issues,
             nlohmann::json* patch) const;

    // Compiles `schema` into impl_ (on the caller's thread or one with a bigger stack).
    void build(const nlohmann::json& schema);

    struct Impl;
    std::string name_;
    std::unique_ptr<Impl> impl_;
};

// The places in a draft-07 `schema` where keywords stand beside a "$ref": draft-07 ignores them (a
// `default` there is never applied; 2020-12 applies them), so each is reported as
// "<pointer> (<keywords>)". Annotations (description, title, $comment, examples) are not reported.
// Empty for a schema that declares 2020-12.
std::vector<std::string> ignored_ref_siblings(const nlohmann::json& schema);

// For tests only: the byte budget of a validation that stays on the caller's thread, the percentage
// of the computed size given to a thread started for deep work (100 is the real one), and the
// schema depth up to which a schema compiles on the caller's thread. reset restores the defaults.
void set_stack_tuning_for_testing(std::size_t inline_bytes, int percent, int inline_schema_depth);
void reset_stack_tuning_for_testing();
// For tests only: make the start of the thread of a deep validation or compilation fail.
void set_thread_failure_for_testing(bool fail);
// For tests only: only the percentage of the computed stack size given to started threads.
void set_stack_percent_for_testing(int percent);
// For tests only: the byte budget of a validation that stays on the caller's thread.
std::size_t stack_inline_budget_for_testing();

// What a 2020-12 `schema` uses that AsyncAPI 3.0.0 (draft-07 JSON Schema) cannot express, as
// "<pointer> (<keywords>)": prefixItems, unevaluatedItems, unevaluatedProperties,
// dependentRequired, dependentSchemas, $dynamicRef, $dynamicAnchor, $anchor, minContains,
// maxContains, an `$id` other than the root's, nesting deeper than 256 levels, and keywords other
// than title, description, $comment and examples beside a "$ref" (the export drops them). Entries
// of the root `$defs` that no reference reaches are not looked at: the export inlines references and
// erases `$defs`, so they never appear in a document. The schemas of message types and the
// envelope are embedded in AsyncAPI documents, so the catalog refuses a message type whose schema
// is in this list. Empty for a schema that is not 2020-12.
std::vector<std::string> beyond_asyncapi_subset(const nlohmann::json& schema);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_DOCUMENT_SCHEMA_HPP
