// The facade that the Python extension praenora.runtime._bindings wraps (praenora_runtime.i): a
// narrow layer over the public C++ API whose parameters and results are strings, integers and
// booleans. JSON documents travel as strings; the pure-Python package praenora.runtime decodes
// them.
//
// - Results. Every function and method returns JSON text, except where it says otherwise. The
//   documents have the keys of the praenora tool's --json results: both front ends take them,
//   and their failures, from praenora::frontend (src/frontend/frontend.hpp).
// - Paths are UTF-8; relative ones are relative to the current directory. An empty string stands
//   for an optional argument that is not given, where a parameter says so.
// - Search paths. module_paths, provider_paths and catalog_paths come first; then, as with
//   RuntimeOptions::use_environment, PRAENORA_MODULE_PATH, PRAENORA_PROVIDER_PATH,
//   PRAENORA_CATALOG_PATH, PRAENORA_LOG_LEVEL and PRAENORA_LOG_FORMAT apply, and the
//   installation's module and provider directories.
// - Errors. A praenora::Error is thrown as std::runtime_error("PRAENORA_ERROR_JSON:" +
//   Error::to_json()); so is any other std::exception except std::bad_alloc, as an INTERNAL error
//   ("internal error: <what>"), as the praenora tool reports it. A mistake in an argument is
//   INVALID_ARGUMENT, the code of the tool's usage errors.
#ifndef PRAENORA_PYTHON_FACADE_HPP
#define PRAENORA_PYTHON_FACADE_HPP

#include <memory>
#include <string>
#include <vector>

namespace praenora {
class Workflow;
} // namespace praenora

namespace praenora::python {

// version_info_json().
std::string version_json();

// The validation report of a workflow configuration ({"ok", "diagnostics"}); with `load`, the
// service providers of the workflow's services are started, and the module libraries loaded and
// their identities checked, too. A report that holds an error is thrown instead, as the praenora
// tool reports it: the first error's code and location, the message "<config>: validation failed:
// N error(s)" (<config> written '' when it is empty) and the report as the details. Warnings and
// notes are returned.
std::string validate_json(const std::string& config, const std::vector<std::string>& module_paths,
                          const std::vector<std::string>& catalog_paths,
                          const std::vector<std::string>& provider_paths, bool load);

// The validation report of any Praenora document, returned or thrown as validate_json() does.
std::string validate_document_json(const std::string& path);

// Every manifest on the module search paths: [{"name", "version", "manifest_path",
// "library_path", "os", "arch", "capabilities"}].
std::string list_modules_json(const std::vector<std::string>& module_paths);

// Every service provider manifest on the provider search paths, shadowed ones included:
// [{"name", "version", "manifest", "library", "platform": {"os", "arch"}, "services": [{"name",
// "major", "minor"}], "engine", "usable"}], with "problem" for one that is not usable.
std::string list_providers_json(const std::vector<std::string>& provider_paths);

// The module `name` at `version` (empty: its highest version for this platform), as one entry of
// list_modules_json(); with `load`, also "describe" (its describe() document) and
// "library_sha256". Inspecting a module loads no service provider.
std::string inspect_module_json(const std::string& name, const std::string& version, bool load,
                                const std::vector<std::string>& module_paths);

// The contract checks of the module whose manifest is `manifest`, with the test vectors in
// `vectors` (empty: the ones the manifest names), the catalog's entries from `catalog_paths` too
// and the services the manifest declares resolved on `provider_paths` first: the
// module-check-report document. A report with a failed check is thrown instead, as the praenora
// tool reports it: FAILED, the message "N of M contract checks failed: <ids>" and the report as
// the details.
std::string check_module_json(const std::string& manifest, const std::string& vectors,
                              const std::vector<std::string>& catalog_paths,
                              const std::vector<std::string>& provider_paths);

// The graph of the workflow definition `workflow` ("<name>@<version>") or of the one the
// configuration `config` pins (give one; the other is empty), in `format`: text, json, dot or
// asyncapi. Returns the text of the graph (JSON text for json and asyncapi).
std::string graph(const std::string& config, const std::string& workflow, const std::string& format,
                  const std::vector<std::string>& module_paths,
                  const std::vector<std::string>& catalog_paths);

// The catalog's entries of `kind` (workflows, capabilities or messages; empty: all), sorted:
// [{"kind", "id", "version", "status", "summary"}].
std::string list_catalog_json(const std::string& kind,
                              const std::vector<std::string>& catalog_paths);

// The catalog entry id@version.
std::string catalog_entry_json(const std::string& id, const std::string& version,
                               const std::vector<std::string>& catalog_paths);

// The embedded document schemas, ["<kind>@<major>.<minor>", ...], sorted.
std::string list_schemas_json();

// The embedded schema of documents of `kind` in `format_version` (empty: the highest).
std::string schema_json(const std::string& kind, const std::string& format_version);

// The status file at `path` (a status-snapshot document).
std::string read_status_json(const std::string& path);

// One run of a workflow configuration: praenora::Workflow. Not copyable.
class WorkflowHandle {
public:
    // Validates the configuration and prepares its run (Runtime::open()). `options_json` is a JSON
    // object with any of the keys module_paths, provider_paths and catalog_paths (arrays of paths),
    // seed and ticks (whole numbers from 0 to 2^53 - 1), output_dir, status_file and log_file
    // (paths; an empty log_file means none), overwrite and trace_messages (booleans), log_level
    // (trace, debug, info, warn, error, critical or off) and log_format (text or json), level and
    // format names in any case, and isolation (in-process or process, exactly so); null stands for
    // a key that is not given. The logging settings apply to the runtime that opens the run too, as
    // the praenora tool's --log-* flags do. An unknown key or a bad value is INVALID_ARGUMENT.
    WorkflowHandle(const std::string& config, const std::string& options_json);
    // Closes the run (Workflow::close()).
    ~WorkflowHandle();
    WorkflowHandle(const WorkflowHandle&) = delete;
    WorkflowHandle& operator=(const WorkflowHandle&) = delete;

    void start();
    // The tick's result: {"tick", "logical_time", "outputs", "finished"}.
    std::string tick();
    // Runs at most `max_ticks` more ticks (-1: until the run is finished) and returns the run's
    // summary: {"state", "ticks_completed", "fingerprint"}. INVALID_ARGUMENT for a count below -1
    // or above 2^53 - 1.
    std::string run(long long max_ticks);
    // The status-snapshot document, the metric points ({"metrics": [...]}), the newest envelope
    // of each workflow output and the run record (null before the run starts).
    std::string status() const;
    std::string metrics() const;
    std::string latest_outputs() const;
    std::string run_record() const;
    // What validation found when the run was opened and did not refuse it: the report of
    // validate, {"ok": true, "diagnostics": [...]}, whose diagnostics are warnings and notes.
    std::string diagnostics() const;
    void request_stop();
    void close();
    // The run's state, for example "running", and its fingerprint (SHA-256, hex): plain text.
    std::string state() const;
    std::string fingerprint() const;

private:
    std::unique_ptr<praenora::Workflow> workflow_;
};

} // namespace praenora::python

#endif // PRAENORA_PYTHON_FACADE_HPP
