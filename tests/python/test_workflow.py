"""Tests of praenora.runtime.Workflow: opening, starting, ticking and running a workflow, its
status, metrics, outputs and run record, its options, its failures, and calls from a second
thread while a run is in progress.

CTest runs them (python.unittest) against the package in the build tree, with the environment that
support.py describes. Configurations of the test workflows are written to temporary directories,
and so is everything a run writes.
"""

import json
import os
import pathlib
import shutil
import tempfile
import threading
import time
import unittest

import praenora.runtime as pr
from support import (BAD_PARAMETERS_CONFIG, EXAMPLE_CONFIG, EXAMPLE_MODULES, FIXTURE_PATHS,
                     FIXTURES, SERVICE_CONFIG, TEST_CATALOG, TEST_ECHO_PROVIDER, TEST_MODULES,
                     needs_examples, with_captured_stderr)

# How long a thread waits for the run it watches before it gives up.
PATIENCE_S = 60


def step(name, module, parameters="{}"):
    """A step block of a configuration: step `name` bound to fixture module `module`."""
    return (f"  {name}:\n"
            f'    module: {{ name: {module}, version: "0.1.0" }}\n'
            f"    parameters: {parameters}\n")


def write_config(directory, workflow, run, steps):
    """Writes <directory>/source.json and <directory>/workflow.yaml, a configuration of the test
    workflow `workflow` with the YAML blocks `run` and `steps`, and returns its path."""
    directory = pathlib.Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "source.json").write_text('{"value": 42}\n', encoding="utf-8", newline="\n")
    config = directory / "workflow.yaml"
    config.write_text("kind: workflow-config\n"
                      'format_version: "0.1.0"\n'
                      f'workflow: {{ name: {workflow}, version: "0.1.0" }}\n'
                      f"{run}"
                      "inputs:\n"
                      "  source: { source: json-document, path: source.json }\n"
                      f"steps:\n{steps}",
                      encoding="utf-8", newline="\n")
    return config


class WorkflowTestCase(unittest.TestCase):
    """Gives each test a temporary directory, self.directory."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = pathlib.Path(directory.name)

    def chain_config(self, ticks, sleep_ms):
        """test.chain: fx-echo, then fx-slow sleeping `sleep_ms` in each of `ticks` ticks and
        stopping when cancelled."""
        return write_config(
            self.directory / "chain", "test.chain", f"run: {{ ticks: {ticks} }}\n",
            step("a", "fx-echo") +
            step("b", "fx-slow", f"{{ sleep_ms: {sleep_ms}, cooperative: true }}"))


class ExampleWorkflowTest(WorkflowTestCase):
    """Runs of the example workflow. Its configuration sets the predict step's log level to
    debug, which outranks the run's level (and so PRAENORA_LOG_LEVEL): each test runs with the
    process's standard error, where the runtime logs, sent to a temporary file
    (with_captured_stderr), so the records stay out of the test output."""

    @needs_examples
    @with_captured_stderr
    def test_workflow_context_manager_runs_three_ticks(self):
        with pr.Workflow(EXAMPLE_CONFIG, module_paths=[EXAMPLE_MODULES]) as workflow:
            self.assertEqual(workflow.state, "created")
            self.assertEqual(workflow.latest_outputs(), {})
            workflow.start()
            self.assertEqual(workflow.state, "ready")
            ticks = [workflow.tick() for _ in range(3)]
            self.assertEqual(set(ticks[0]), {"tick", "logical_time", "outputs", "finished"})
            self.assertEqual([tick["tick"] for tick in ticks], [0, 1, 2])
            self.assertEqual([tick["logical_time"] for tick in ticks], [1.0, 2.0, 3.0])
            self.assertEqual([sorted(tick["outputs"]) for tick in ticks],
                             [["predictions", "state"]] * 3)
            self.assertEqual([tick["finished"] for tick in ticks], [False] * 3)
            self.assertEqual(workflow.state, "running")
            self.assertEqual(sorted(workflow.latest_outputs()), ["predictions", "state"])
            self.assertEqual(workflow.status()["workflow"]["ticks_completed"], 3)
        # Leaving the block closes the workflow: the run ends as stopped and control calls are
        # refused, while queries keep working.
        self.assertEqual(workflow.state, "stopped")
        self.assertEqual(workflow.status()["workflow"]["state"], "stopped")
        with self.assertRaises(pr.PraenoraError) as caught:
            workflow.tick()
        self.assertEqual(caught.exception.name, "INVALID_STATE")
        workflow.close()  # idempotent

    @needs_examples
    @with_captured_stderr
    def test_status_and_metrics_shapes(self):
        with pr.Workflow(EXAMPLE_CONFIG, module_paths=[EXAMPLE_MODULES]) as workflow:
            summary = workflow.run(ticks=2)
            self.assertEqual(summary, {"state": "running", "ticks_completed": 2,
                                       "fingerprint": workflow.fingerprint})

            status = workflow.status()
            self.assertEqual(set(status), {"kind", "format_version", "generated_at",
                                           "runtime_version", "pid", "workflow", "steps"})
            self.assertEqual(status["kind"], "status-snapshot")
            self.assertEqual(status["pid"], os.getpid())
            self.assertEqual(status["workflow"]["name"], "example.estimate-predict")
            self.assertEqual(status["workflow"]["state"], "running")
            self.assertEqual(status["workflow"]["ticks_completed"], 2)
            self.assertEqual(status["workflow"]["fingerprint"], workflow.fingerprint)
            self.assertEqual([step["step"] for step in status["steps"]], ["estimate", "predict"])
            self.assertEqual([step["invocations"] for step in status["steps"]], [2, 2])

            metrics = workflow.metrics()
            self.assertEqual(set(metrics), {"metrics"})
            for point in metrics["metrics"]:
                self.assertTrue({"name", "unit", "kind", "attributes"} <= set(point), point)
            values = {point["name"]: point["value"] for point in metrics["metrics"]
                      if point["kind"] == "counter" and "outcome" not in point["attributes"]}
            self.assertEqual(values, {"praenora.workflow.ticks": 2})
            succeeded = sorted((point["attributes"]["step"], point["value"])
                               for point in metrics["metrics"]
                               if point["attributes"].get("outcome") == "succeeded")
            self.assertEqual(succeeded, [("estimate", 2), ("predict", 2)])

            summary = workflow.run()
            self.assertEqual((summary["state"], summary["ticks_completed"]), ("completed", 12))
            self.assertEqual(workflow.status()["workflow"]["state"], "completed")

    @needs_examples
    @with_captured_stderr
    def test_run_record_fingerprint_is_64_hex(self):
        with pr.Workflow(EXAMPLE_CONFIG, module_paths=[EXAMPLE_MODULES]) as workflow:
            self.assertRegex(workflow.fingerprint, r"^[0-9a-f]{64}$")
            self.assertIsNone(workflow.run_record())
            summary = workflow.run()
            record = workflow.run_record()
        self.assertEqual(record["kind"], "run-record")
        self.assertEqual(record["fingerprint"], workflow.fingerprint)
        self.assertEqual(summary["fingerprint"], workflow.fingerprint)
        self.assertEqual(record["execution"]["status"], "completed")
        # The same configuration gives the same fingerprint; another seed another one.
        with pr.Workflow(EXAMPLE_CONFIG, [EXAMPLE_MODULES]) as again:
            self.assertEqual(again.fingerprint, workflow.fingerprint)
        with pr.Workflow(EXAMPLE_CONFIG, [EXAMPLE_MODULES], seed=7) as other:
            self.assertNotEqual(other.fingerprint, workflow.fingerprint)

    @needs_examples
    @with_captured_stderr
    def test_run_options(self):
        output_dir = self.directory / "run"
        status_file = self.directory / "status" / "status.json"
        log_file = self.directory / "logs" / "run.log"
        log_file.parent.mkdir()
        with pr.Workflow(EXAMPLE_CONFIG, [EXAMPLE_MODULES], output_dir, 7, "Debug", ticks=3,
                         trace_messages=True, status_file=status_file, log_format="JSON",
                         log_file=log_file) as workflow:
            summary = workflow.run()
        self.assertEqual(summary, {"state": "completed", "ticks_completed": 3,
                                   "fingerprint": workflow.fingerprint})
        record = json.loads((output_dir / "run-record.json").read_text(encoding="utf-8"))
        self.assertEqual(record["fingerprint"], workflow.fingerprint)
        self.assertEqual(record["reproducibility"]["seed"], 7)
        self.assertTrue((output_dir / "trace" / "messages.jsonl").is_file())
        snapshot = pr.read_status(status_file)
        self.assertEqual(snapshot["kind"], "status-snapshot")
        self.assertEqual(snapshot["workflow"]["state"], "completed")
        self.assertEqual(snapshot["workflow"]["fingerprint"], workflow.fingerprint)
        events = [json.loads(line) for line in log_file.read_text(encoding="utf-8").splitlines()]
        self.assertIn("DEBUG", {event["severity_text"] for event in events})
        # The explicit level and format win over PRAENORA_LOG_LEVEL=off, on standard error too:
        # the engine's own debug records are there (the example's predict step logs at debug by
        # its own configuration, whatever the run's level).
        records = [json.loads(line) for line in self.stderr().splitlines()
                   if line.startswith("{")]
        self.assertIn(("DEBUG", "praenora.engine", "tick.completed"),
                      {(record["severity_text"], record["attributes"].get("logger"),
                        record["attributes"].get("event")) for record in records})

        # An output directory that holds an earlier run is refused, unless the run may
        # overwrite it.
        with self.assertRaises(pr.PraenoraError) as caught:
            pr.Workflow(EXAMPLE_CONFIG, [EXAMPLE_MODULES], output_dir)
        self.assertEqual(caught.exception.name, "IO_ERROR")
        with pr.Workflow(EXAMPLE_CONFIG, [EXAMPLE_MODULES], output_dir, overwrite=True,
                         ticks=1) as workflow:
            self.assertEqual(workflow.run()["ticks_completed"], 1)


class FixtureWorkflowTest(WorkflowTestCase):
    def test_paths_with_spaces_and_non_ascii(self):
        # The configuration, module and output directories have names with spaces and non-ASCII
        # characters; fx-echo (its manifest names its library relative to itself) is copied.
        modules = self.directory / "mödules ü"
        shutil.copytree(pathlib.Path(TEST_MODULES, "fx-echo"), modules / "fx-echo")
        config = write_config(self.directory / "cönfig dir", "test.single", "run: { ticks: 2 }\n",
                              step("a", "fx-echo"))
        output_dir = self.directory / "öutput ñ"
        paths = {"module_paths": [modules], "catalog_paths": [TEST_CATALOG]}
        self.assertTrue(pr.validate(config, load=True, **paths)["ok"])
        self.assertEqual(pr.graph(config, **paths)["schedule"], ["a"])
        info = pr.inspect_module("fx-echo", module_paths=[modules])
        self.assertEqual(pathlib.Path(info["manifest_path"]).resolve(),
                         (modules / "fx-echo" / "fx-echo.pmm.json").resolve())
        with pr.Workflow(config, output_dir=output_dir, **paths) as workflow:
            self.assertEqual(workflow.run()["state"], "completed")
        record = json.loads((output_dir / "run-record.json").read_text(encoding="utf-8"))
        self.assertEqual(record["execution"]["ticks_completed"], 2)


class ServiceWorkflowTest(WorkflowTestCase):
    def test_provider_paths(self):
        # fx-service requires test.echo@0: without its provider the configuration is invalid.
        with self.assertRaises(pr.PraenoraError) as caught:
            pr.Workflow(SERVICE_CONFIG, **FIXTURE_PATHS)
        error = caught.exception
        self.assertEqual((error.name, error.code), ("SERVICE_UNAVAILABLE", 407))
        self.assertEqual(error.details["diagnostics"][0]["details"]["service"], "test.echo")
        with self.assertRaises(TypeError):
            pr.Workflow(SERVICE_CONFIG, provider_paths=TEST_ECHO_PROVIDER, **FIXTURE_PATHS)

        # With test-echo on a provider path, the run echoes its input through the service, and
        # its run record names the service and its provider.
        with pr.Workflow(SERVICE_CONFIG, provider_paths=[TEST_ECHO_PROVIDER],
                         **FIXTURE_PATHS) as workflow:
            self.assertEqual(workflow.run()["state"], "completed")
            self.assertEqual(workflow.latest_outputs()["result"]["data"],
                             {"echo": {"value": 42}, "found": {"test.echo@0": True}})
            reproducibility = workflow.run_record()["reproducibility"]
        service = reproducibility["services"]["test.echo@0"]
        self.assertEqual((service["provider"]["name"], service["minor"]), ("test-echo", 0))
        self.assertEqual(reproducibility["steps"]["a"]["services"], ["test.echo@0"])


@unittest.skipIf(os.name == "nt", "steps cannot run in module hosts on Windows")
class IsolationWorkflowTest(WorkflowTestCase):
    """Steps in module hosts (process isolation): the keyword isolation sets every step's mode."""

    def test_isolation_keyword(self):
        config = write_config(self.directory / "config", "test.chain", "run: { ticks: 3 }\n",
                              step("a", "fx-echo") + step("b", "fx-lifecycle"))
        records, outputs = {}, {}
        for mode in ("in-process", "process"):
            with self.subTest(mode=mode):
                out = self.directory / mode
                with pr.Workflow(config, **FIXTURE_PATHS, output_dir=out,
                                 isolation=mode) as workflow:
                    self.assertEqual(workflow.run()["state"], "completed")
                    records[mode] = workflow.run_record()
                outputs[mode] = (out / "outputs" / "result.jsonl").read_bytes()
                execution = records[mode]["execution"]
                self.assertEqual(execution["isolation"],
                                 {"a": {"mode": mode, "share_compiled_models": False,
                                        "sandbox": "none"},
                                  "b": {"mode": mode, "share_compiled_models": False,
                                        "sandbox": "none"}})
                self.assertEqual(execution["overrides"], {"isolation": mode})
        # The same results, and the same fingerprint, in either place.
        self.assertEqual(outputs["in-process"], outputs["process"])
        self.assertEqual(records["in-process"]["fingerprint"], records["process"]["fingerprint"])
        self.assertEqual(records["in-process"]["reproducibility"],
                         records["process"]["reproducibility"])

    def test_crash_in_a_module_host(self):
        # fx-crash ends its module host in its second invocation; the run fails, its step is
        # crashed, and the Python process goes on.
        config = FIXTURES / "configs" / "cli" / "isolation-crash" / "workflow.yaml"
        with pr.Workflow(config, **FIXTURE_PATHS) as workflow:
            with self.assertRaises(pr.PraenoraError) as caught:
                workflow.run()
            error = caught.exception
            self.assertEqual((error.name, error.code, error.category),
                             ("MODULE_CRASHED", 706, "execution"))
            self.assertEqual((error.details["phase"], error.details["signal_name"]),
                             ("invoke", "SIGSEGV"))
            status = workflow.status()
            self.assertEqual(status["steps"][0]["state"], "crashed")
            self.assertEqual(status["workflow"]["health"], "unhealthy")
            self.assertEqual(workflow.state, "failed")

    def test_validate_loads_in_a_module_host(self):
        config = write_config(self.directory, "test.single",
                              "run: { ticks: 1, isolation: { mode: process } }\n",
                              step("a", "fx-echo"))
        self.assertTrue(pr.validate(config, load=True, **FIXTURE_PATHS)["ok"])


class WorkflowErrorTest(WorkflowTestCase):
    def test_invalid_options_raise(self):
        config = write_config(self.directory, "test.single", "run: { ticks: 1 }\n",
                              step("a", "fx-echo"))
        for options in ({"log_level": "loud"}, {"log_format": "xml"}, {"log_level": 3},
                        {"seed": -1}, {"seed": 2**53}, {"seed": True}, {"seed": 1.5},
                        {"ticks": "3"}, {"isolation": "thread"}, {"isolation": "Process"},
                        {"isolation": 1}):
            with self.subTest(options=options):
                with self.assertRaises(pr.PraenoraError) as caught:
                    pr.Workflow(config, **FIXTURE_PATHS, **options)
                self.assertEqual(caught.exception.name, "INVALID_ARGUMENT")
        with self.assertRaises(TypeError):
            pr.Workflow(config, **FIXTURE_PATHS, output_dir=5)

        # The facade refuses what the Python layer never sends: an unknown option, a document
        # that is not a JSON object.
        prefix = "PRAENORA_ERROR_JSON:"
        for options in ({**FIXTURE_PATHS, "colour": "blue"}, [1, 2],
                        {**FIXTURE_PATHS, "provider_paths": TEST_ECHO_PROVIDER}):
            with self.subTest(options=options):
                with self.assertRaises(RuntimeError) as caught:
                    pr._bindings.WorkflowHandle(str(config), json.dumps(options))
                message = str(caught.exception)
                self.assertTrue(message.startswith(prefix), message)
                self.assertEqual(json.loads(message[len(prefix):])["name"], "INVALID_ARGUMENT")

        # Level and format names in any case; run() takes a whole number of ticks, or None.
        with pr.Workflow(config, **FIXTURE_PATHS, log_level="WARN", log_format="Text",
                         seed=2**53 - 1) as workflow:
            for ticks in (-1, True, 1.5, "1", 2**53):
                with self.subTest(ticks=ticks):
                    with self.assertRaises(pr.PraenoraError) as caught:
                        workflow.run(ticks=ticks)
                    self.assertEqual(caught.exception.name, "INVALID_ARGUMENT")
            self.assertEqual(workflow.run(ticks=0)["ticks_completed"], 0)
            self.assertEqual(workflow.run(), {"state": "completed", "ticks_completed": 1,
                                              "fingerprint": workflow.fingerprint})

    def test_open_failure_raises(self):
        with self.assertRaises(pr.PraenoraError) as caught:
            pr.Workflow(BAD_PARAMETERS_CONFIG, **FIXTURE_PATHS)
        error = caught.exception
        self.assertEqual((error.name, error.category), ("PARAMETERS_INVALID", "validation"))
        self.assertEqual(error.message, "workflow validation failed: 1 error(s)")
        self.assertEqual(error.location["pointer"], "/steps/a/parameters/sleep_ms")
        self.assertEqual(error.details["diagnostics"][0]["name"], "PARAMETERS_INVALID")

    def test_failed_run_raises(self):
        config = FIXTURES / "configs" / "cli" / "module-failure" / "workflow.yaml"
        with pr.Workflow(config, **FIXTURE_PATHS) as workflow:
            with self.assertRaises(pr.PraenoraError) as caught:
                workflow.run()
            self.assertEqual((caught.exception.name, caught.exception.category),
                             ("STEP_FAILED", "execution"))
            self.assertEqual(workflow.state, "failed")
            last_error = workflow.status()["workflow"]["last_error"]
            self.assertEqual(last_error["name"], "STEP_FAILED")
            # The status's error record holds the error's details (and its location: none here).
            self.assertEqual(last_error["details"], caught.exception.details)
            self.assertEqual(last_error["details"]["step"], "a")
            self.assertEqual(caught.exception.location, {})
            self.assertIsNone(last_error["location"])
            # A failed run throws its failure again.
            with self.assertRaises(pr.PraenoraError) as again:
                workflow.run()
            self.assertEqual(again.exception.message, caught.exception.message)

    def test_failed_run_files_name_directories_by_placeholders(self):
        # A file where the run's outputs directory goes: the outputs cannot be written.
        out = self.directory / "out"
        out.mkdir()
        (out / "outputs").write_text("a file, not a directory\n", encoding="utf-8")
        status_file = self.directory / "status.json"
        config = write_config(self.directory / "config", "test.single", "run: { ticks: 1 }\n",
                              step("a", "fx-echo"))
        outputs = str(out / "outputs")
        named = "<output>" + outputs[len(str(out)):]
        with pr.Workflow(config, **FIXTURE_PATHS, output_dir=out,
                         status_file=status_file) as workflow:
            with self.assertRaises(pr.PraenoraError) as caught:
                workflow.run()
            self.assertEqual(caught.exception.name, "IO_ERROR")
            # What stays in the process names the directory.
            self.assertEqual(caught.exception.location["file"], outputs)
            self.assertEqual(workflow.status()["workflow"]["last_error"]["location"]["file"],
                             outputs)
            # The run record names it by its placeholder, as its file does.
            recorded = workflow.run_record()["execution"]["error"]
            self.assertEqual(recorded["location"]["file"], named)
            self.assertIn(f"'{named}'", recorded["message"])
            self.assertNotIn(str(self.directory), json.dumps(workflow.run_record()))
        with open(out / "run-record.json", encoding="utf-8") as file:
            self.assertEqual(json.load(file)["execution"]["error"], recorded)
        # So does the status file, written for the last time when the workflow was closed.
        self.assertEqual(pr.read_status(status_file)["workflow"]["last_error"], recorded)


class SecondThreadTest(WorkflowTestCase):
    """The calls into the runtime release the GIL: another Python thread runs, and may query or
    stop the workflow, while run() is in progress."""

    def test_status_from_second_thread_during_run(self):
        config = self.chain_config(ticks=10, sleep_ms=50)
        seen = []  # (state, ticks_completed) of each status() the second thread made
        done = threading.Event()
        with pr.Workflow(config, **FIXTURE_PATHS) as workflow:
            workflow.start()

            def watch():
                while not done.is_set():
                    snapshot = workflow.status()["workflow"]
                    seen.append((snapshot["state"], snapshot["ticks_completed"]))
                    time.sleep(0.005)

            watcher = threading.Thread(target=watch)
            watcher.start()
            try:
                summary = workflow.run()
            finally:
                done.set()
                watcher.join(PATIENCE_S)
        self.assertFalse(watcher.is_alive())
        self.assertEqual((summary["state"], summary["ticks_completed"]), ("completed", 10))
        # Had run() kept the GIL, the watcher would have seen the run only before and after it.
        running = [ticks for state, ticks in seen if state == "running"]
        self.assertGreaterEqual(len(set(running)), 2, seen)

    def test_request_stop_from_thread(self):
        config = self.chain_config(ticks=100, sleep_ms=200)
        with pr.Workflow(config, **FIXTURE_PATHS) as workflow:

            def stop_once_running():
                deadline = time.monotonic() + PATIENCE_S
                while workflow.state in ("created", "starting", "ready"):
                    if time.monotonic() > deadline:
                        return
                    time.sleep(0.01)
                workflow.request_stop()

            stopper = threading.Thread(target=stop_once_running)
            stopper.start()
            try:
                summary = workflow.run()
            finally:
                stopper.join(PATIENCE_S)
            self.assertFalse(stopper.is_alive())
            self.assertEqual(summary["state"], "stopped")
            self.assertLess(summary["ticks_completed"], 100)
            self.assertEqual(workflow.state, "stopped")
            self.assertEqual(workflow.run_record()["execution"]["status"], "stopped")


class DiagnosticsTest(WorkflowTestCase):
    """Workflow.diagnostics(): what validation found when the run was opened and did not refuse."""

    def replay_config(self, rows):
        """A configuration of test.single whose input replays a CSV of `rows` (time, value)."""
        directory = self.directory / "replay"
        directory.mkdir(parents=True, exist_ok=True)
        lines = ["series_id,variable_id,time,value"]
        lines += [f"S1,x,{time},{value}" for time, value in rows]
        (directory / "obs.csv").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        config = directory / "workflow.yaml"
        config.write_text("kind: workflow-config\n"
                          'format_version: "0.1.0"\n'
                          'workflow: { name: test.single, version: "0.1.0" }\n'
                          "run: { ticks: 2 }\n"
                          "inputs:\n"
                          "  source: { source: observation-csv, path: obs.csv }\n"
                          "steps:\n" + step("a", "fx-echo"),
                          encoding="utf-8", newline="\n")
        return config

    def test_warnings_of_the_validation_that_opened_the_run(self):
        config = self.replay_config([(0.5, 1), (-2.0, 2), (-1.0, 3), (1.5, 4)])
        reported = pr.validate(config, **FIXTURE_PATHS)
        self.assertTrue(reported["ok"])
        with pr.Workflow(config, output_dir=self.directory / "out", **FIXTURE_PATHS) as workflow:
            diagnostics = workflow.diagnostics()
            # The same document that validate() gives for the configuration: one warning.
            self.assertEqual(diagnostics, reported)
            self.assertTrue(diagnostics["ok"])
            self.assertEqual(len(diagnostics["diagnostics"]), 1)
            warning = diagnostics["diagnostics"][0]
            self.assertEqual((warning["severity"], warning["name"]),
                             ("warning", "TARGET_DATA_INVALID"))
            self.assertEqual(warning["location"]["line"], 3)
            self.assertIn("2 of 4 rows have a time before run.time.start", warning["message"])
            # The warnings do not stop the run, and its record holds the counts.
            self.assertEqual(workflow.run()["state"], "completed")
            self.assertEqual(workflow.run_record()["execution"]["inputs"],
                             {"source": {"rows_total": 4, "rows_before_start": 2}})
            # A query that still works once the run is over.
            self.assertEqual(workflow.diagnostics(), diagnostics)

    def test_no_warnings_without_early_rows(self):
        config = self.replay_config([(0.5, 1), (1.5, 2)])
        with pr.Workflow(config, **FIXTURE_PATHS) as workflow:
            self.assertEqual(workflow.diagnostics(), {"ok": True, "diagnostics": []})


class DescriptorTest(WorkflowTestCase):
    """An observation-csv input named by an ode-uq-spec target descriptor (ADR-0042), through the
    Python package: the same validation, warnings, fingerprint and run record as the C++ API."""

    def descriptor_config(self, unit="mmol"):
        """A configuration of test.single whose input is described by meta/target.json: the CSV has
        a row of the response x and one of the covariate age. Returns the configuration's path."""
        directory = self.directory / "descriptor"
        (directory / "meta").mkdir(parents=True, exist_ok=True)
        (directory / "data").mkdir(parents=True, exist_ok=True)
        (directory / "data" / "obs.csv").write_text(
            "series_id,variable_id,time,value\nS1,x,0.5,1\nS1,age,0.5,60\nS1,x,1.5,2\n",
            encoding="utf-8", newline="\n")
        descriptor = {
            "schema_version": "0.0.0", "id": "py-descriptor", "name": "Python test data",
            "data": {"id": "obs", "href": "../data/obs.csv"},
            "columns": {"series_id": "series_id", "variable_id": "variable_id", "time": "time",
                        "value": "value"},
            "time_unit": "d",
            "variables": [
                {"id": "x", "name": "X", "unit": unit, "value_type": "scalar"},
                {"id": "age", "name": "Age", "unit": "y", "value_type": "scalar",
                 "role": "covariate"}],
        }
        (directory / "meta" / "target.json").write_text(json.dumps(descriptor), encoding="utf-8",
                                                          newline="\n")
        config = directory / "workflow.yaml"
        config.write_text("kind: workflow-config\n"
                          'format_version: "0.1.0"\n'
                          'workflow: { name: test.single, version: "0.1.0" }\n'
                          "run: { ticks: 2, time: { unit: d } }\n"
                          "inputs:\n"
                          "  source: { source: observation-csv, descriptor: meta/target.json }\n"
                          "steps:\n" + step("a", "fx-echo"),
                          encoding="utf-8", newline="\n")
        return config

    def test_the_descriptor_is_read_and_its_roles_are_warned_about(self):
        config = self.descriptor_config()
        reported = pr.validate(config, **FIXTURE_PATHS)
        self.assertTrue(reported["ok"])
        with pr.Workflow(config, output_dir=self.directory / "out", **FIXTURE_PATHS) as workflow:
            diagnostics = workflow.diagnostics()
            self.assertEqual(diagnostics, reported)
            self.assertEqual(len(diagnostics["diagnostics"]), 1)
            warning = diagnostics["diagnostics"][0]
            self.assertEqual((warning["severity"], warning["name"]),
                             ("warning", "TARGET_DATA_INVALID"))
            self.assertEqual(warning["location"]["line"], 3)
            self.assertIn("1 of 3 rows belong to variables whose role is not response",
                          warning["message"])
            self.assertEqual(workflow.run()["state"], "completed")
            record = workflow.run_record()
            self.assertEqual(record["execution"]["inputs"], {"source": {
                "rows_total": 3, "rows_before_start": 0, "rows_not_delivered_by_role": 1,
                "descriptor_id": "py-descriptor"}})
            entry = record["reproducibility"]["inputs"]["source"]
            self.assertEqual(entry["descriptor"]["schema_version"], "0.0.0")
            self.assertEqual(len(entry["descriptor"]["digest"]), 64)
            self.assertEqual(record["fingerprint"], workflow.fingerprint)

    def test_the_descriptor_changes_the_fingerprint_only_where_it_decides(self):
        config = self.descriptor_config()
        with pr.Workflow(config, **FIXTURE_PATHS) as workflow:
            reference = workflow.fingerprint
        target = config.parent / "meta" / "target.json"
        descriptor = json.loads(target.read_text(encoding="utf-8"))
        descriptor["name"] = "Another name"
        descriptor["x-note"] = "words"
        target.write_text(json.dumps(descriptor), encoding="utf-8", newline="\n")
        with pr.Workflow(config, **FIXTURE_PATHS) as workflow:
            self.assertEqual(workflow.fingerprint, reference)
        descriptor["variables"][1]["role"] = "response"
        target.write_text(json.dumps(descriptor), encoding="utf-8", newline="\n")
        with pr.Workflow(config, **FIXTURE_PATHS) as workflow:
            self.assertNotEqual(workflow.fingerprint, reference)

    def test_a_descriptor_that_breaks_the_rows_is_refused(self):
        config = self.descriptor_config()
        data = config.parent / "data" / "obs.csv"
        data.write_text("series_id,variable_id,time,value\nS1,nope,0.5,1\n", encoding="utf-8",
                        newline="\n")
        # validate() raises for a configuration that is not valid, with the report in the details.
        with self.assertRaises(pr.PraenoraError) as caught:
            pr.validate(config, **FIXTURE_PATHS)
        error = caught.exception
        self.assertEqual(error.name, "TARGET_DATA_INVALID")
        self.assertEqual(error.details["diagnostics"][0]["name"], "TARGET_DATA_INVALID")
        self.assertEqual(error.details["diagnostics"][0]["location"]["line"], 2)
        with self.assertRaises(pr.PraenoraError) as opened:
            pr.Workflow(config, **FIXTURE_PATHS)
        self.assertEqual(opened.exception.name, "TARGET_DATA_INVALID")


if __name__ == "__main__":
    unittest.main()
