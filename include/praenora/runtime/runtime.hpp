// The entry point of the C++ API: a Runtime validates workflow configurations, lists and inspects
// modules, lists service providers, validates documents and opens workflows
// (praenora/runtime/workflow.hpp). Paths are UTF-8 strings; relative ones are relative to the
// current directory.
#ifndef PRAENORA_RUNTIME_RUNTIME_HPP
#define PRAENORA_RUNTIME_RUNTIME_HPP

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/export.h>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/version.hpp>
#include <praenora/runtime/workflow.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace praenora {

// A module manifest found on the module search paths.
struct ModuleInfo {
    std::string name, version, manifest_path, library_path, os, arch;
    std::vector<std::string> capabilities; // "<id>@<version>"
    // Filled in only when the module was loaded: what its describe() reported (a JSON object) and
    // the SHA-256 of its library.
    std::string describe_json, library_sha256;
};

// A service provider manifest found on the provider search paths
// (docs/specs/runtime-services.md).
struct ProviderInfo {
    std::string name, version, manifest_path, library_path, os, arch;
    std::vector<std::string> services; // the services it offers, "<name>@<major>.<minor>"
    std::string engine_json;           // the manifest's engine object, as JSON text
    // Whether this runtime can use the provider: no earlier manifest of its name and platform
    // shadows it, and its manifest is for this platform, this runtime version and this provider
    // interface major. When it cannot, `problem` says why (shadowed, naming the manifest that
    // shadows it, or else the first of the other rules it breaks); it is empty for a usable
    // provider.
    bool usable = false;
    std::string problem;
};

// One contract check of a module: its id, its status ("pass", "fail" or "skip") and a message.
struct CheckResult {
    std::string id, status, message;
};

struct ModuleCheckReport {
    std::vector<CheckResult> checks;

    // True when no check failed.
    PRAENORA_RUNTIME_API bool passed() const;
    // The report as a module-check-report document: {"kind", "format_version", "passed",
    // "checks": [{"id", "status", "message"}]}.
    PRAENORA_RUNTIME_API std::string to_json() const;
};

// One entry of the protocol catalog: its kind ("message-type", "capability-definition" or
// "workflow-definition"), id, version, status and summary.
struct CatalogEntryInfo {
    std::string kind, id, version, status, summary;
};

#ifdef _MSC_VER
#pragma warning(push)
// The exported class holds a std::shared_ptr, which is not exported itself.
#pragma warning(disable : 4251)
#endif

// The runtime: the protocol catalog (the built-in entries, RuntimeOptions::catalog_paths and, with
// use_environment, PRAENORA_CATALOG_PATH), the index of the module search paths (the options'
// module_paths, then PRAENORA_MODULE_PATH with use_environment, then the installation's module
// directory), the index of the provider search paths (the options' provider_paths, then
// PRAENORA_PROVIDER_PATH with use_environment, then the installation's provider directory) and
// the runtime's logging. Everything is read once, when the Runtime is constructed.
// A Runtime is immutable and thread-safe; copies share one state and are cheap (a move is a copy,
// so a Runtime stays usable whatever happened to it).
//
// Logging. The Runtime's own log records use RuntimeOptions::logging, with PRAENORA_LOG_LEVEL and
// PRAENORA_LOG_FORMAT put over its level and format when use_environment. A workflow resolves each
// setting on its own: level and format come from WorkflowOptions, else the environment variable,
// else the configuration's observability section (when the file sets them), else
// RuntimeOptions::logging; the log file from WorkflowOptions, else the configuration, else
// RuntimeOptions::logging; to_stderr always from RuntimeOptions::logging. Logging never changes
// a run's fingerprint.
class PRAENORA_RUNTIME_API Runtime {
public:
    // Throws praenora::Error: CONFIG_INVALID when PRAENORA_LOG_LEVEL or PRAENORA_LOG_FORMAT holds
    // an invalid value (with use_environment), IO_ERROR when the log file cannot be opened or a
    // catalog path is not a readable directory, and the catalog's own errors (PARSE_ERROR,
    // SCHEMA_VIOLATION, CATALOG_ENTRY_DUPLICATE, ...).
    explicit Runtime(RuntimeOptions options = {});
    ~Runtime();
    Runtime(const Runtime&);
    Runtime& operator=(const Runtime&);

    // Validates a workflow configuration: everything a run checks before tick 0, without running
    // anything, every problem reported (never thrown) with its location. With `load`, it also
    // loads and starts the service providers of the workflow's services, as a run does (they stay
    // loaded for the rest of the process, and their records go to this runtime's logging), then
    // loads each module library once where its steps run, in this process or in a module host
    // (then ended), checks its identity, and releases the libraries.
    ValidationReport validate(const std::string& config_path,
                              const ValidateOptions& options = {}) const;

    // Validates any Praenora document (JSON or YAML) against the schema of its kind: the `kind`
    // field, or for the documents of the module ABI, which have none, the kind whose required
    // fields it has. Problems are reported, never thrown.
    ValidationReport validate_document(const std::string& path) const;

    // Every manifest found on the module search paths, in search order, other platforms' too.
    std::vector<ModuleInfo> list_modules() const;

    // Every service provider manifest found on the provider search paths, in search order, other
    // platforms' and other runtime versions' too, each saying whether this runtime can use it: a
    // manifest shadowed by an earlier one of the same provider name and platform, which is never
    // used, is listed unusable. Nothing is loaded.
    std::vector<ProviderInfo> list_providers() const;

    // The module `name` at `version` (by default its highest version for this platform), as
    // discovery resolves it. With `load`, the library is loaded and checked (and released again),
    // and describe_json and library_sha256 are filled in. Throws praenora::Error:
    // MODULE_NOT_FOUND or PLATFORM_MISMATCH, INVALID_ARGUMENT for a version that is not a semantic
    // version, and the loader's errors.
    ModuleInfo inspect_module(const std::string& name,
                              const std::optional<std::string>& version = std::nullopt,
                              bool load = false) const;

    // Runs the contract checks of the module whose manifest is `manifest_path`, with the test
    // vectors in `vectors_path` (by default the ones the manifest's tests.vectors names), against
    // this runtime's catalog. The report lists every check, in a fixed order, with its result:
    // manifest.schema, manifest.semantics, library.digest, library.load, library.entry_point,
    // api.header, api.required_functions, describe.schema, describe.identity,
    // lifecycle.create_destroy, lifecycle.full, invoke.result_schema, invoke.expected_outputs,
    // invoke.determinism, invoke.malformed_input, self_test and unload; a check whose
    // prerequisite did not pass is skipped, saying why. Problems of the module, its manifest or
    // the vectors are failed checks. A manifest file that cannot be read (missing, for example)
    // throws praenora::Error(IO_ERROR) instead, and so does a vectors file that cannot be read,
    // which is opened only once the manifest checks have passed (a manifest that fails them is
    // reported, and its vectors are never read); either is thrown before any of the module's code
    // runs. library.load first resolves the services the manifest declares on this runtime's
    // provider search paths and loads and starts their providers, as a run does, and the module's
    // instances are then served them. The module's log messages go to the runtime's logging
    // (logger module.check), and so do the providers' records (logger provider.<name>).
    ModuleCheckReport
    check_module(const std::string& manifest_path,
                 const std::optional<std::string>& vectors_path = std::nullopt) const;

    // The graph of a workflow definition (request.workflow, "<name>@<version>") or of the one a
    // configuration pins (request.config_path), in request.format: text (the schedule and one line
    // per connection), json, dot (Graphviz) or asyncapi (an AsyncAPI 3.0.0 document). A
    // configuration must be readable as one (the file is read and parsed, its kind and
    // format_version are right and it satisfies the workflow-config schema); beyond that only its
    // workflow section matters, and other problems in it are ignored. Throws praenora::Error:
    // INVALID_ARGUMENT unless exactly one of the two is given or for a malformed name@version,
    // WORKFLOW_NOT_FOUND, the configuration's first error that keeps it from being read or that
    // concerns its workflow section (its report in the details), and, for an invalid definition,
    // its first problem, located in its catalog file, with every problem (a validation report) in
    // the details.
    std::string graph(const GraphRequest& request) const;

    // Every catalog entry, sorted by kind, then id, then version.
    std::vector<CatalogEntryInfo> list_catalog() const;
    // The catalog entry id@version as canonical JSON. Throws praenora::Error:
    // CATALOG_ENTRY_NOT_FOUND when there is none, INVALID_ARGUMENT when entries of more than one
    // kind have that id and version (the message names the kinds) or `version` is not a
    // semantic version.
    std::string catalog_entry(const std::string& id, const std::string& version) const;

    // The kinds and versions of the embedded document schemas, each "<kind>@<major>.<minor>",
    // sorted.
    std::vector<std::string> list_schemas() const;
    // The text of the embedded schema of documents of `kind` in `format_version` ("major.minor"
    // or a whole semantic version; the highest version when not given). Throws praenora::Error:
    // FORMAT_VERSION_UNSUPPORTED for an unknown kind or version, INVALID_ARGUMENT for a
    // malformed version.
    std::string schema(const std::string& kind,
                       const std::optional<std::string>& format_version = std::nullopt) const;

    // Validates the configuration (with the overrides in `options`) and prepares its run: the
    // output directory (refusing one that holds an earlier run, unless options.overwrite), the
    // status file and the logging. Nothing is loaded yet. Throws praenora::Error: when validation
    // reports an error, the first error's code and location, the message "workflow validation
    // failed: N error(s)" and the whole report (ValidationReport::to_json()) as details; IO_ERROR
    // when the output directory or the log file cannot be used. A configuration that is valid but
    // has warnings opens; the workflow keeps them (Workflow::diagnostics()).
    std::unique_ptr<Workflow> open(const std::string& config_path,
                                   const WorkflowOptions& options = {}) const;

    // Reads a status file that a run writes (the status-snapshot document, whose error records name
    // the run's directories by placeholders) and returns its text.
    // Throws praenora::Error: IO_ERROR when it cannot be read, PARSE_ERROR when it is not JSON,
    // DOCUMENT_KIND_MISMATCH when its kind is not "status-snapshot".
    static std::string read_status_file(const std::string& path);

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace praenora

#endif // PRAENORA_RUNTIME_RUNTIME_HPP
