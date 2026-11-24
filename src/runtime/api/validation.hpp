// The validation sequence: everything a run checks before tick 0, without loading any module
// code. It turns a configuration file into a ValidatedWorkflow (resolved steps, input sources,
// the reproducibility section and the run fingerprint) or into diagnostics.
#ifndef PRAENORA_RUNTIME_API_VALIDATION_HPP
#define PRAENORA_RUNTIME_API_VALIDATION_HPP

#include "catalog/catalog.hpp"
#include "config/workflow_config.hpp"
#include "discovery/manifest.hpp"
#include "discovery/module_index.hpp"
#include "graph/graph.hpp"
#include "instance/services.hpp"
#include "io/input_source.hpp"
#include "isolation/child_settings.hpp"
#include "provider/provider_index.hpp"
#include "provider/provider_manifest.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace praenora::detail {

class LogHub;

// A step of the workflow with everything needed to run it.
struct ResolvedStep {
    std::string id;
    const CapabilityDef* capability = nullptr; // in the context's catalog
    const ModuleManifest* manifest = nullptr;  // owned by ValidatedWorkflow::manifests
    nlohmann::json parameters;                 // with the parameter schema's defaults filled in
    std::uint64_t seed = 0;                    // derive_step_seed(run seed, id)
    Timeouts timeouts;
    std::optional<LogLevel> log_level;
    std::string library_sha256; // of the library file when it was validated
    // The keys ("<name>@<major>") of ValidatedWorkflow::services that the step's module may use:
    // the services its manifest declares that resolved for the run, required or optional; sorted.
    std::vector<std::string> services;
    // Where the step's module instance runs: in this process or in a module host.
    Isolation isolation;
};

// A runtime service that a provider serves to a run.
struct ResolvedService {
    ServiceVersion service; // its name and major, and the minor the provider offers
    std::shared_ptr<const ProviderManifest> provider;
    std::string library_sha256; // of the provider's library file when it was validated
};

// One provider that a run loads, with the services the run gets from it.
struct ProviderLoad {
    std::shared_ptr<const ProviderManifest> provider;
    std::string library_sha256; // as validated
    std::vector<ServiceVersion> services; // name, major and offered minor, in key order
    std::vector<std::string> keys;        // their keys ("<name>@<major>")
};

struct ValidatedWorkflow {
    // The configuration with the caller's overrides applied (see read_workflow_config()), and
    // `record` filled in with every workflow output when the file lists none.
    WorkflowConfig config;
    const WorkflowDef* definition = nullptr; // in the context's catalog
    WorkflowGraph graph;
    std::vector<ResolvedStep> steps;                             // in schedule order
    std::map<std::string, std::unique_ptr<InputSource>> sources; // by workflow input
    // The run record's reproducibility section (the run-record schema's shape):
    //   versions {runtime, abi {major, minor}, pmp},
    //   workflow {name, version, digest},
    //   catalog {"<id>@<version>": digest} for every capability and message type the workflow
    //     uses (keys become "<kind>:<id>@<version>" for an id@version that two kinds share),
    //   time {unit, start, period}, seed,
    //   ticks (null when the run lasts until its replay inputs are exhausted),
    //   timeouts {invoke, lifecycle, cancel_grace} (the run's),
    //   inputs {<input>: {source, digest[, columns]}},
    //   services {"<name>@<major>": {minor (offered), provider {name, version, manifest_sha256,
    //                                library_sha256}, engine}} for every service of `services`,
    //   steps {<step>: {capability {id, version},
    //                   module {name, version, manifest_sha256, library_sha256, sdk_version},
    //                   parameters, seed, timeouts {invoke, lifecycle, cancel_grace},
    //                   services [the step's service keys, sorted]}}.
    // It holds no paths, no wall-clock time and nothing from `outputs` or `observability`.
    nlohmann::json reproducibility;
    std::string fingerprint; // sha256_hex(canonical_json(reproducibility))
    // The run record's execution.manifests, outside the hashed section: where each manifest of the
    // run was found, without an absolute path (ManifestLocation, discovery/search_paths.hpp):
    //   steps {<step>: location}, services {"<name>@<major>": location} (the provider's manifest),
    // each location {search_path {origin, index (absent for the installation's directory)},
    // file (relative to the search path, '/'-separated)}.
    nlohmann::json manifest_locations;
    // The run record's execution.isolation, outside the hashed section too: every step's resolved
    // isolation settings, {<step>: {"mode": "in-process" | "process", "share_compiled_models",
    // "sandbox": "none" | "restricted"}}.
    nlohmann::json isolation;
    // The run record's execution.inputs, outside the hashed section as well: the counts that the
    // replay sources found in their data, {<input>: {"rows_total", "rows_before_start"}}, for each
    // input whose source has rows (an observation-csv); none for a json-document.
    nlohmann::json input_counts;
    // The module search paths that resolved the steps' modules, in search order, with their
    // origins and indexes: those of the context's index (RuntimeContext::modules), or the list
    // rebuilt with the configuration's modules.search_paths when it names any.
    std::vector<SearchPath> module_search_paths;
    // The runtime services that providers serve to the run, by "<name>@<major>": every service
    // that some step's module declares and that resolved. One provider serves a name and major to
    // the whole run.
    std::map<std::string, ResolvedService> services;
    // The manifests of the steps' modules, one per manifest file.
    std::vector<std::shared_ptr<const ModuleManifest>> manifests;
    // Every message type the workflow's ports carry (its inputs and outputs and the ports of its
    // steps' capabilities), by "<id>@<version>"; in the context's catalog.
    std::map<std::string, const MessageTypeDef*> message_types;
};

// What validation runs against: the catalog, the index of the module search paths, the index of
// the provider search paths, and the runtime's built-in services.
//
// Invariant: `modules` is ModuleIndex::scan(module_search_paths(module_paths, {},
// use_environment)). validate_workflow() relies on it: when a configuration names module search
// paths of its own, it rebuilds the search list from `module_paths`, those paths and
// `use_environment` (the caller's paths, the configuration's, PRAENORA_MODULE_PATH, the
// installation's directory), so a context whose fields disagree would drop or misplace the
// caller's paths. Build contexts with make_runtime_context(), which keeps them consistent.
struct RuntimeContext {
    Catalog catalog;
    ModuleIndex modules;
    ProviderIndex providers;
    ServiceRegistry services; // built in: served to every module, whatever its manifest declares
    std::vector<std::string> module_paths;
    bool use_environment = true;
};

// The runtime context for `options`:
// - catalog: the built-in entries, then each of options.catalog_paths (UTF-8; relative ones
//   against the current directory; each must be a readable directory, else IO_ERROR), then, when
//   options.use_environment, the entries of PRAENORA_CATALOG_PATH (':'-separated, ';' on Windows;
//   relative ones against the current directory; entries that are not directories are skipped,
//   as for PRAENORA_MODULE_PATH). A directory named more than once is loaded once.
// - modules: ModuleIndex::scan(module_search_paths(options.module_paths, {},
//   options.use_environment)), with module_paths and use_environment copied from `options`.
// - providers: ProviderIndex::scan(provider_search_paths(options.provider_paths,
//   options.use_environment)): the caller's provider paths, PRAENORA_PROVIDER_PATH (with
//   use_environment) and the installation's provider directory.
// - services: empty.
// Throws what Catalog::load() throws.
RuntimeContext make_runtime_context(const RuntimeOptions& options);

// Validates the configuration in `config`, adding every problem to `report`, in this order:
//   1-2. the configuration is read and checked (read_workflow_config(), with `options` as the
//        overrides);
//   3. its workflow definition is in the catalog (WORKFLOW_NOT_FOUND), and so are the message
//      types and capabilities the definition uses (CATALOG_ENTRY_NOT_FOUND);
//   4. the definition's graph is valid (graph/graph.hpp);
//   5. every step is bound and no other (BINDING_INVALID), every workflow input is bound and no
//      other (INPUT_BINDING_INVALID), and outputs.record names workflow outputs
//      (CONFIG_INVALID);
//   6. each step's module resolves on the search paths (MODULE_NOT_FOUND, PLATFORM_MISMATCH),
//      is compatible (compat/compat.hpp), each service its manifest requires resolves on the
//      provider search paths (SERVICE_UNAVAILABLE, located at /requires/services/<i> of the
//      manifest, with ProviderIndex::unresolved_details() as details; a service it can use
//      (optional) that does not resolve is simply not used), and its library file exists and
//      matches its pinned digest (LIBRARY_NOT_FOUND, LIBRARY_DIGEST_MISMATCH, IO_ERROR); then the
//      library file of each provider the run uses is checked the same way
//      (check_provider_library_file(), located in the provider's manifest, reported for the first
//      step in schedule order that uses the provider). A service name and major that several
//      steps declare is served by one provider: the one the highest minor any of them asks for
//      resolved to. Then each step that runs in a module host (isolation mode "process") is
//      checked: UNSUPPORTED where the platform has no process isolation (Windows in this
//      version), located at the field that set the step's mode (/steps/<step>/isolation/mode or
//      /run/isolation/mode), or nowhere when the caller's override set it; its module's services
//      are served as any step's, by providers loaded in its module host. A step that runs in the
//      runtime's process with share_compiled_models true or sandbox "restricted" is refused
//      (CONFIG_INVALID, located at the field that set it, /steps/<step>/isolation/<field> or
//      /run/isolation/<field>), and so is a step in a module host with sandbox "restricted" where
//      the platform has no restricted profile (UNSUPPORTED, anything but macOS in this version,
//      located at that field too). Nothing is loaded;
//   7. each step's parameters satisfy its module's parameter schema (PARAMETERS_INVALID, located
//      at /steps/<step>/parameters<pointer> in the configuration), which then fills in defaults;
//   8. each input's data file exists, is a file and has a status that can be read
//      (TARGET_DATA_UNAVAILABLE, located at /inputs/<name>/path in the configuration) and its
//      source can be built (io/input_source.hpp);
//   9. the reproducibility section and the fingerprint are computed. The steps' isolation is not
//      part of them (results do not depend on it): it goes to ValidatedWorkflow::isolation.
// Every problem is collected; a check is skipped only when it depends on one that failed (no
// definition: nothing after step 3; an unknown capability or message type: the graph, and the
// steps and inputs that use it; an error in an input's path or columns: its source). Diagnostics
// from the scans of the module and provider search paths are added as warnings. Returns nullopt
// when any error was reported.
std::optional<ValidatedWorkflow> validate_workflow(const std::filesystem::path& config,
                                                   const RuntimeContext& context,
                                                   const WorkflowOptions& options,
                                                   ValidationReport& report);

// SERVICE_UNAVAILABLE for service requirement `index` of `manifest`, which did not resolve on
// `providers` (`resolution`): the message names the service and why no provider serves it,
// located at /requires/services/<index> of the manifest, with ProviderIndex::unresolved_details()
// as the details and the module (name@version) set; the step is left empty.
Diagnostic service_unavailable(const ModuleManifest& manifest, std::size_t index,
                               const ProviderIndex& providers,
                               const ServiceResolution& resolution);

// The providers that serve the services `keys` (keys of workflow.services, "<name>@<major>"), each
// once, with the services of `keys` it serves: in the order of the keys of workflow.services, a
// provider coming where its first service does. The providers of one step are
// provider_loads(workflow, step.services); a module host loads those for a step in it.
std::vector<ProviderLoad> provider_loads(const ValidatedWorkflow& workflow,
                                         const std::vector<std::string>& keys);

// The providers of the services that the steps of `workflow` in isolation mode `mode` may use, as
// above: those a run loads in the runtime's process (IsolationMode::in_process), or those the
// module hosts of its steps load (IsolationMode::process).
std::vector<ProviderLoad> provider_loads(const ValidatedWorkflow& workflow, IsolationMode mode);

// The providers of the services `keys` as the module host of a step loads them
// (ChildSettings::providers): provider_loads(workflow, keys), each with validation's digest and
// its records going to the logger provider.<name> of `logs`, as in this process (none without
// logs).
std::vector<ChildProvider> child_providers(const ValidatedWorkflow& workflow,
                                           const std::vector<std::string>& keys, LogHub* logs);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_API_VALIDATION_HPP
