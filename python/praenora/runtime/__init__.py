"""Praenora runtime for Python.

The functions validate workflow configurations and documents, list, inspect and check modules,
list service providers, and show workflow graphs, the protocol catalog and the document schemas;
Workflow runs a workflow. These are the features of the praenora command-line tool, with the same
results.

- Results are decoded JSON (dicts, lists, strings and numbers) with the keys of the tool's --json
  output.
- Failures raise PraenoraError, which carries the runtime's error code, name and category, a
  message, details and the location of the problem.
- Paths are str or os.PathLike objects. module_paths, provider_paths and catalog_paths are lists
  of paths, searched before PRAENORA_MODULE_PATH, PRAENORA_PROVIDER_PATH and PRAENORA_CATALOG_PATH
  from the environment (and, for modules and service providers, before the installation's module
  and provider directories); None adds no paths of its own.
- Calls into the runtime release the GIL: while one thread runs a workflow, another may call its
  status(), metrics() or request_stop().

praenora is a namespace package; this package is praenora.runtime.
"""

import json
import operator
import os

from . import _bindings

__all__ = [
    "PraenoraError",
    "Workflow",
    "catalog_entry",
    "check_module",
    "graph",
    "inspect_module",
    "list_catalog",
    "list_modules",
    "list_providers",
    "list_schemas",
    "read_status",
    "schema",
    "validate",
    "validate_document",
    "version",
]

# The prefix of the messages that carry an error of the runtime (Error::to_json()).
_ERROR_PREFIX = "PRAENORA_ERROR_JSON:"
# The largest integer a JSON number holds exactly: tick counts go no higher.
_MAX_COUNT = 2**53 - 1


class PraenoraError(Exception):
    """An error of the Praenora runtime; str() gives "NAME: message".

    Attributes:
        code: the error code (int), for example 203.
        name: its stable name, for example "PARAMETERS_INVALID".
        category: its category, for example "validation".
        message: what went wrong.
        details: details that depend on the code (dict). A validation failure holds the whole
            report, {"ok": False, "diagnostics": [...]}, each diagnostic with its own "details"
            when it has any (for SERVICE_UNAVAILABLE, the service and the providers considered);
            a failed contract check the check report.
        location: where the problem is (dict with any of "file", "line", "column" and
            "pointer").
    """

    def __init__(self, message="", *, code=1, name="UNKNOWN", category="internal", details=None,
                 location=None):
        super().__init__(message)
        self.code = code
        self.name = name
        self.category = category
        self.message = message
        self.details = {} if details is None else details
        self.location = {} if location is None else location

    def __str__(self):
        return f"{self.name}: {self.message}"


def _error_from(payload):
    """The PraenoraError of an error document ({"code", "name", "category", "message",
    "location", "details"})."""
    details = payload.get("details")
    if not isinstance(details, dict):
        # Details that are not a JSON object are kept as text.
        details = {} if details is None else {
            "text": details if isinstance(details, str) else json.dumps(details)}
    location = payload.get("location")
    return PraenoraError(str(payload.get("message", "")),
                         code=int(payload.get("code", 1)),
                         name=str(payload.get("name", "UNKNOWN")),
                         category=str(payload.get("category", "internal")),
                         details=details,
                         location=location if isinstance(location, dict) else {})


def _call(function, *arguments):
    """Calls `function` of the extension; an error of the runtime raises PraenoraError."""
    try:
        return function(*arguments)
    except RuntimeError as error:
        text = str(error)
        if not text.startswith(_ERROR_PREFIX):
            raise
        raise _error_from(json.loads(text[len(_ERROR_PREFIX):])) from None


def _decoded(function, *arguments):
    """The JSON result of `function` of the extension, decoded."""
    return json.loads(_call(function, *arguments))


def _path(value):
    """A path as a str: a str or an os.PathLike object whose path is a str."""
    path = os.fspath(value)
    if not isinstance(path, str):
        raise TypeError(f"a path must be a str or os.PathLike, not {type(value).__name__}")
    return path


def _optional_path(value):
    """A path as a str, "" for None."""
    return "" if value is None else _path(value)


def _paths(values):
    """A list of paths as strs, [] for None; a lone path is refused (not split into
    characters)."""
    if values is None:
        return []
    if isinstance(values, (str, bytes, os.PathLike)):
        raise TypeError("expected a list of paths, not one path")
    return [_path(value) for value in values]


def _count(value, what):
    """A whole number from 0 to 2**53 - 1, as int; anything else raises PraenoraError
    INVALID_ARGUMENT."""
    if not isinstance(value, bool):
        try:
            number = operator.index(value)
        except TypeError:
            pass
        else:
            if 0 <= number <= _MAX_COUNT:
                return number
    raise PraenoraError(f"{what} {value!r} is not a whole number from 0 to {_MAX_COUNT}",
                        code=2, name="INVALID_ARGUMENT", category="usage")


def version():
    """The versions of the runtime, the module SDK, the protocol (PMP) and the module ABI, the
    format version of each document kind, and the platform, compiler and build type:
    {"runtime", "sdk", "pmp", "abi": {"major", "minor"}, "formats", "platform", "compiler",
    "build_type"}."""
    return _decoded(_bindings.version_json)


def validate(config, module_paths=None, catalog_paths=None, load=False, *, provider_paths=None):
    """Validates the workflow configuration `config`: everything a run checks before its first
    tick, without running anything; with `load`, the service providers of the workflow's services
    are started, and the module libraries loaded and their identities checked, too (each where
    its steps run: in this process, or in a module host for a step in process isolation).
    `provider_paths` are searched for service providers first.

    Returns the report, {"ok": True, "diagnostics": [...]}, whose diagnostics are warnings and
    notes. When the report holds an error, raises PraenoraError with the first error's code and
    location, the message "<config>: validation failed: N error(s)" (<config> written '' when it
    is empty) and the report in `details`.
    """
    return _decoded(_bindings.validate_json, _path(config), _paths(module_paths),
                    _paths(catalog_paths), _paths(provider_paths), bool(load))


def validate_document(path):
    """Validates any Praenora document (JSON or YAML) against the schema of its kind; returns the
    report or raises as validate() does."""
    return _decoded(_bindings.validate_document_json, _path(path))


def list_modules(module_paths=None):
    """Every module manifest on the module search paths, in search order, other platforms' too:
    [{"name", "version", "manifest_path", "library_path", "os", "arch", "capabilities"}]."""
    return _decoded(_bindings.list_modules_json, _paths(module_paths))


def list_providers(provider_paths=None):
    """Every service provider manifest on the provider search paths, in search order, other
    platforms' and other runtime versions' too: [{"name", "version", "manifest", "library",
    "platform": {"os", "arch"}, "services": [{"name", "major", "minor"}], "engine", "usable"}],
    with "problem", why this runtime cannot use it, for a provider that is not usable. A manifest
    shadowed by an earlier one of the same name and platform is never used: it is listed unusable,
    its problem naming the manifest that shadows it. Nothing is loaded."""
    return _decoded(_bindings.list_providers_json, _paths(provider_paths))


def inspect_module(name, version=None, load=False, module_paths=None):
    """The module `name` at `version` (by default its highest version for this platform) as
    discovery resolves it, an entry like those of list_modules(). With `load`, its library is
    loaded (and released again) and the entry also holds "describe", what the module's
    describe() reports, and "library_sha256". Inspecting a module loads no service provider."""
    return _decoded(_bindings.inspect_module_json, name, "" if version is None else version,
                    bool(load), _paths(module_paths))


def check_module(manifest, vectors=None, *, catalog_paths=None, provider_paths=None):
    """Runs the contract checks of the module whose manifest is `manifest`, with the test vectors
    in `vectors` (by default the ones the manifest names); `catalog_paths` adds catalog entries,
    such as the capabilities and message types the module uses that the runtime does not have
    built in. The services the manifest declares are resolved on the provider search paths,
    `provider_paths` first, and their providers started for the checks.

    Returns the module-check-report document ({"kind", "format_version", "passed", "checks"})
    when no check failed. When one failed, raises PraenoraError FAILED, with the report in
    `details`.
    """
    return _decoded(_bindings.check_module_json, _path(manifest), _optional_path(vectors),
                    _paths(catalog_paths), _paths(provider_paths))


def graph(config=None, workflow=None, format="json", *, module_paths=None, catalog_paths=None):
    """The graph of the workflow definition `workflow` ("<name>@<version>") or of the one that
    the configuration `config` pins (give one of them), in `format`: "json" (returned as a dict),
    "text", "dot" (Graphviz) or "asyncapi" (an AsyncAPI 3.0.0 document), each returned as a
    str."""
    text = _call(_bindings.graph, _optional_path(config), "" if workflow is None else workflow,
                 format, _paths(module_paths), _paths(catalog_paths))
    return json.loads(text) if format == "json" else text


def list_catalog(kind=None, *, catalog_paths=None):
    """The entries of the protocol catalog (the built-in ones and those of the catalog paths),
    sorted by kind, id and version: [{"kind", "id", "version", "status", "summary"}]. `kind`
    ("workflows", "capabilities" or "messages") selects the entries of one kind."""
    return _decoded(_bindings.list_catalog_json, "" if kind is None else kind,
                    _paths(catalog_paths))


def catalog_entry(id, version, *, catalog_paths=None):
    """The document of the catalog entry id@version."""
    return _decoded(_bindings.catalog_entry_json, id, version, _paths(catalog_paths))


def list_schemas():
    """The embedded document schemas, ["<kind>@<major>.<minor>", ...], sorted."""
    return _decoded(_bindings.list_schemas_json)


def schema(kind, format_version=None):
    """The embedded JSON Schema of documents of `kind` in `format_version` ("major.minor" or a
    whole version; by default the highest)."""
    return _decoded(_bindings.schema_json, kind,
                    "" if format_version is None else format_version)


def read_status(path):
    """The status file that a run keeps (its status_file): the status-snapshot document. Its error
    records name the run's directories by placeholders such as <output> and <home>."""
    return _decoded(_bindings.read_status_json, _path(path))


class Workflow:
    """One run of a workflow configuration.

    The constructor validates the configuration, with the arguments as overrides, and prepares
    the run; nothing is loaded yet. It raises PraenoraError when the configuration is invalid
    (the first error's code and location, the report in `details`) or the output directory or
    the log file cannot be used. Leaving a `with` block closes the run.

    Arguments:
        config: the workflow configuration file.
        module_paths, provider_paths, catalog_paths: lists of paths searched first (see the
            package).
        output_dir: where the run record, the outputs and the trace go, instead of the
            configuration's outputs.directory.
        seed, ticks: the run seed and the number of ticks, whole numbers from 0 to 2**53 - 1.
        log_level: trace, debug, info, warn, error, critical or off; log_format: text or json
            (names in any case); log_file: also log to this file ("" for none). They win over
            PRAENORA_LOG_LEVEL, PRAENORA_LOG_FORMAT and the configuration.
        overwrite: replace an earlier run in the output directory.
        trace_messages: whether every message, and the runtime services each call into a
            module looks up, are traced to <output_dir>/trace/messages.jsonl; None keeps the
            configuration's setting.
        status_file: keep a status file there (read_status() reads it).
        isolation: where every step's module instance runs, in place of the configuration's
            (run.isolation and steps.<id>.isolation): "in-process" (in the Python process) or
            "process" (in a module host, a process of its own, so that a crash of the module ends
            only its step). The run record lists it with the other overrides. A wheel installation
            has no module host: a step in a module host then fails to load with
            MODULE_HOST_UNAVAILABLE.

    The control calls (start, tick, run and close) are made one at a time. The queries (state,
    fingerprint, status, metrics, latest_outputs and run_record) and request_stop() may be called
    from any thread at any time, also while a control call runs.
    """

    def __init__(self, config, module_paths=None, output_dir=None, seed=None, log_level=None, *,
                 ticks=None, overwrite=False, trace_messages=None, status_file=None,
                 log_format=None, log_file=None, catalog_paths=None, provider_paths=None,
                 isolation=None):
        options = {
            "module_paths": None if module_paths is None else _paths(module_paths),
            "provider_paths": None if provider_paths is None else _paths(provider_paths),
            "catalog_paths": None if catalog_paths is None else _paths(catalog_paths),
            "output_dir": None if output_dir is None else _path(output_dir),
            "seed": seed,
            "ticks": ticks,
            "overwrite": bool(overwrite),
            "trace_messages": None if trace_messages is None else bool(trace_messages),
            "status_file": None if status_file is None else _path(status_file),
            "log_level": log_level,
            "log_format": log_format,
            "log_file": None if log_file is None else _path(log_file),
            "isolation": isolation,
        }
        # Only the options given; integers of other types (numpy's, say) as ints.
        options_json = json.dumps({key: value for key, value in options.items()
                                   if value is not None}, default=operator.index)
        self._handle = _call(_bindings.WorkflowHandle, _path(config), options_json)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    def start(self):
        """Loads and starts the service providers of the run's services, loads the modules, then
        creates and initializes an instance for each step: the state becomes "ready"
        ("completed" for a run of no ticks)."""
        _call(self._handle.start)

    def tick(self):
        """Runs the next tick and returns its number, its logical time, the workflow outputs it
        produced and whether the run is now finished: {"tick", "logical_time", "outputs",
        "finished"}."""
        return _decoded(self._handle.tick)

    def run(self, ticks=None):
        """Starts the run when it has not started, then runs ticks until it is finished, or until
        `ticks` more ticks have run, and returns the summary: {"state", "ticks_completed",
        "fingerprint"}. It returns while the run goes on and for a completed or stopped run; a
        failed run raises its PraenoraError, also on every later call."""
        max_ticks = -1 if ticks is None else _count(ticks, "ticks")
        return _decoded(self._handle.run, max_ticks)

    def status(self):
        """The run's status: the status-snapshot document, its error records with absolute
        paths."""
        return _decoded(self._handle.status)

    def metrics(self):
        """The run's metric points: {"metrics": [{"name", "unit", "kind", "attributes", ...}]}."""
        return _decoded(self._handle.metrics)

    def latest_outputs(self):
        """The newest envelope of each workflow output, {"<output>": envelope}; {} at first."""
        return _decoded(self._handle.latest_outputs)

    def run_record(self):
        """The run record as last written, the same document as its file; None before the run
        starts. Its error records name the run's directories by placeholders such as <output>
        and <home>."""
        return _decoded(self._handle.run_record)

    def diagnostics(self):
        """What validation found when the run was opened and did not refuse it: the report of
        validate(), {"ok": True, "diagnostics": [...]}, whose diagnostics are warnings (and
        notes), for example data rows before the run's start; the praenora tool prints the same
        ones. An error would have raised in the constructor."""
        return _decoded(self._handle.diagnostics)

    def request_stop(self):
        """Asks the run to stop: the module call in progress is asked to cancel, no further call
        starts, and the run ends as "stopped". It may be called from any thread."""
        _call(self._handle.request_stop)

    def close(self):
        """Ends the run, if it is ready or running, as "stopped"; finalizes, destroys and unloads
        its modules; then refuses every further control call. Idempotent; the queries keep
        working. While another thread is in a control call, it only asks the run to stop."""
        _call(self._handle.close)

    @property
    def state(self):
        """The run's state: created, starting, ready, running, stopping, completed, stopped or
        failed."""
        return _call(self._handle.state)

    @property
    def fingerprint(self):
        """The run fingerprint (SHA-256, 64 hex digits)."""
        return _call(self._handle.fingerprint)
