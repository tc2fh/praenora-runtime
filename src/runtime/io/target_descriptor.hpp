// The ode-uq-spec target descriptor (schemas/target.schema.json, ADR-0042): the JSON
// document that names an observation CSV and gives its dialect, column mapping, variables (unit,
// role, bounds) and duplicate policy. An observation-csv input may name one with `descriptor`.
#ifndef PRAENORA_RUNTIME_IO_TARGET_DESCRIPTOR_HPP
#define PRAENORA_RUNTIME_IO_TARGET_DESCRIPTOR_HPP

#include "document/yaml_loader.hpp"
#include "io/csv.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::detail {

// The descriptor schema version the runtime reads (the descriptor's `schema_version`).
inline constexpr std::string_view kTargetDescriptorSchemaVersion = "0.0.0";

struct TargetVariable {
    std::string id, unit;
    std::string role = "response"; // response, covariate, input or validation; absent: response
    std::optional<double> lower_bound, upper_bound;
};

struct TargetDescriptor {
    std::filesystem::path file; // as given (absolute for a configuration's descriptor)
    std::string id;             // the descriptor's id
    std::string schema_version;
    std::string href;                // data.href as written
    bool href_remote = false;        // href has a URI scheme: the runtime cannot read it
    std::filesystem::path data_path; // the href resolved against the descriptor's directory;
                                     // empty when href_remote
    std::optional<std::string> sha256; // data.sha256, lower case
    CsvDialect dialect;
    std::optional<std::string> missing_token;
    std::map<std::string, std::string> columns; // observation column -> CSV header
    std::optional<std::string> time_unit;
    std::optional<std::string> duplicate_policy; // "error" or "require_observation_id"
    std::vector<TargetVariable> variables;       // in the descriptor's order
    // sha256 of the canonical JSON of the parts that decide what the runtime does with the data
    // (the hashed paths below, with defaults filled in); part of the run fingerprint.
    std::string digest;
    LoadedDocument document; // for locating diagnostics
};

// Reads the descriptor in `file` and checks everything that does not need the CSV, in this order:
// the file is read (TARGET_DATA_UNAVAILABLE) and parsed as strict JSON, whatever its name
// (UNSUPPORTED for anything else, YAML included: descriptors are JSON); schema_version is "0.0.0"
// (FORMAT_VERSION_UNSUPPORTED naming it); the document satisfies the embedded ode-uq-spec schema,
// as published (SCHEMA_VIOLATION at the first problem, naming how many more); the dialect's
// delimiter and quote are each one byte of ASCII (UNSUPPORTED otherwise), not a line break and
// different from each other (SCHEMA_VIOLATION, at the key that was set); no variable id appears
// twice and no variable has lower_bound > upper_bound (TARGET_DATA_INVALID, located at the second
// one); duplicate_policy is not aggregate_mean or aggregate_median (UNSUPPORTED: they would change
// the data). Every error is located in the file.
TargetDescriptor load_target_descriptor(const std::filesystem::path& file);

// The property paths of the descriptor schema whose values go into TargetDescriptor::digest, and
// those that do not. Only leaf properties are listed. Paths are dot separated; "[]" marks the
// items of an array ("variables[].id"); "x-*" stands for any property beginning with "x-". Every
// leaf property of the embedded schema is in exactly one list, which a test checks, so a field
// added by a later version of the spec has to be classified.
const std::vector<std::string>& descriptor_hashed_paths();
const std::vector<std::string>& descriptor_excluded_paths();

// The embedded descriptor schema, as published by ode-uq-spec (resource ode-uq/target.0.0.schema.json).
const nlohmann::json& embedded_target_descriptor_schema();

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_IO_TARGET_DESCRIPTOR_HPP
