"""Tests of tools/migrate_schemas_to_2020_12.py: the committed schemas, a draft-07 fixture, and the
rules that stop it. tests/tools/fixtures/draft07.schema.json is a draft-07 schema with a tuple
`items`, `additionalItems`, schema-valued and array-valued `dependencies`, an `$id` with an empty
fragment, a property called `definitions` that has `definitions` of its own, and references into all
of them; expected-2020-12.schema.json is what the script must write for it, byte for byte."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

SOURCE_DIR = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = SOURCE_DIR / "tools" / "migrate_schemas_to_2020_12.py"
FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"
D07 = "http://json-schema.org/draft-07/schema#"
D2020 = "https://json-schema.org/draft/2020-12/schema"


def run_script(*args):
    return subprocess.run([sys.executable, "-B", str(SCRIPT), *args], capture_output=True, text=True,
                          check=False)


def dump(document):
    return json.dumps(document, indent=2, ensure_ascii=False) + "\n"


class CommittedSchemasTest(unittest.TestCase):
    def test_the_runtime_owned_schemas_are_what_the_script_writes(self):
        result = run_script("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 file(s) would change", result.stdout)

    def test_every_document_schema_and_message_type_is_2020_12(self):
        schemas = sorted((SOURCE_DIR / "spec" / "schemas").glob("*.json"))
        self.assertEqual(len(schemas), 19)
        for path in schemas:
            self.assertTrue(path.name.endswith(".schema.json"), path)
            self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["$schema"], D2020, path)
        messages = sorted((SOURCE_DIR / "spec" / "catalog" / "messages").glob("*/*.json"))
        self.assertEqual(len(messages), 7)
        for path in messages:
            document = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(document["schema"]["$schema"], D2020, path)


class FixtureTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = pathlib.Path(self.tmp.name)

    def write(self, document, name="x.0.1.schema.json", newline="\n"):
        path = self.dir / name
        path.write_bytes(dump(document).replace("\n", newline).encode("utf-8"))
        return path

    def test_the_draft07_fixture_becomes_the_expected_file(self):
        path = self.dir / "fixture.schema.json"
        path.write_bytes((FIXTURES / "draft07.schema.json").read_bytes())
        result = run_script(str(path))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(path.read_bytes(), (FIXTURES / "expected-2020-12.schema.json").read_bytes())

    def test_a_second_run_changes_nothing(self):
        path = self.dir / "fixture.schema.json"
        path.write_bytes((FIXTURES / "draft07.schema.json").read_bytes())
        self.assertEqual(run_script(str(path)).returncode, 0)
        before = path.read_bytes()
        result = run_script(str(path))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 file(s) rewritten", result.stdout)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(run_script("--check", str(path)).returncode, 0)

    def test_check_changes_nothing_and_fails_for_a_draft07_file(self):
        path = self.dir / "fixture.schema.json"
        original = (FIXTURES / "draft07.schema.json").read_bytes()
        path.write_bytes(original)
        result = run_script("--check", str(path))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("would change", result.stdout)
        self.assertEqual(path.read_bytes(), original)

    def test_check_catches_line_ending_drift(self):
        expected = (FIXTURES / "expected-2020-12.schema.json").read_bytes()
        path = self.dir / "crlf.schema.json"
        path.write_bytes(expected.replace(b"\n", b"\r\n"))
        result = run_script("--check", str(path))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(path.read_bytes(), expected.replace(b"\n", b"\r\n"))
        # Without --check the file is rewritten with the script's line ends.
        self.assertEqual(run_script(str(path)).returncode, 0)
        self.assertEqual(path.read_bytes(), expected)

    def test_a_message_type_entry_changes_only_its_data_schema(self):
        entry = {"kind": "message-type", "id": "a.b", "schema": {"$schema": D07, "definitions": {}},
                 "x": {"$schema": D07}}
        path = self.write(entry, "0.1.0.json")
        self.assertEqual(run_script(str(path)).returncode, 0)
        migrated = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(migrated["schema"], {"$schema": D2020, "$defs": {}})
        self.assertEqual(migrated["x"], {"$schema": D07})

    def test_other_json_files_are_left_alone(self):
        path = self.write({"kind": "capability-definition", "$schema": D07}, "0.1.0.json")
        before = path.read_bytes()
        self.assertEqual(run_script(str(path)).returncode, 0)
        self.assertEqual(path.read_bytes(), before)


class DialectTest(unittest.TestCase):
    """Only a draft-07 schema is rewritten, and its dialect never stays draft-07."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = pathlib.Path(self.tmp.name)

    def write(self, document, name="x.0.1.schema.json"):
        path = self.dir / name
        path.write_bytes(dump(document).encode("utf-8"))
        return path

    def test_a_schema_without_dollar_schema_is_draft07_and_gets_the_2020_12_one(self):
        path = self.write({"type": "object", "properties": {"p": {"$ref": "#/definitions/n"}},
                           "definitions": {"n": {"type": "number"}}})
        self.assertEqual(run_script(str(path)).returncode, 0)
        migrated = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(migrated, {"$schema": D2020, "type": "object",
                                    "properties": {"p": {"$ref": "#/$defs/n"}},
                                    "$defs": {"n": {"type": "number"}}})
        self.assertEqual(list(migrated)[0], "$schema")
        self.assertEqual(run_script("--check", str(path)).returncode, 0)

    def test_a_message_type_data_schema_without_dollar_schema_too(self):
        path = self.write({"kind": "message-type", "schema": {"definitions": {}}}, "0.1.0.json")
        self.assertEqual(run_script(str(path)).returncode, 0)
        self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["schema"],
                         {"$schema": D2020, "$defs": {}})

    def test_a_2020_12_schema_is_left_alone_even_with_old_spellings(self):
        path = self.write({"$schema": D2020, "definitions": {"n": {}}, "dependencies": {"a": ["b"]}})
        before = path.read_bytes()
        self.assertEqual(run_script(str(path)).returncode, 0)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(run_script("--check", str(path)).returncode, 0)

    def test_another_dialect_is_refused(self):
        for dialect in ("http://json-schema.org/draft-04/schema#",
                        "https://json-schema.org/draft/2019-09/schema"):
            path = self.write({"$schema": dialect, "definitions": {}})
            before = path.read_bytes()
            result = run_script(str(path))
            self.assertEqual(result.returncode, 2, dialect)
            self.assertIn("neither draft-07 nor 2020-12", result.stderr)
            self.assertEqual(path.read_bytes(), before)


class UnsafeTest(unittest.TestCase):
    """What stops the script: the file is not written and the status is 2."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = pathlib.Path(self.tmp.name)

    def refused(self, document, message):
        path = self.dir / "x.0.1.schema.json"
        path.write_bytes(dump(document).encode("utf-8"))
        before = path.read_bytes()
        result = run_script(str(path))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn(message, result.stderr)
        self.assertEqual(path.read_bytes(), before)

    def test_keywords_beside_a_ref_are_refused(self):
        for keyword, value in (("default", 1), ("maximum", 1), ("deprecated", True),
                               ("readOnly", True), ("writeOnly", True)):
            with self.subTest(keyword):
                self.refused({"$schema": D07, "definitions": {"n": {"type": "number"}},
                              "properties": {"p": {"$ref": "#/definitions/n", keyword: value}}},
                             "beside $ref")

    def test_a_non_string_ref_with_siblings_is_refused(self):
        self.refused({"$schema": D07, "properties": {"p": {"$ref": 5, "maximum": 1}}}, "beside $ref")

    def test_a_ref_that_does_not_resolve_is_refused(self):
        self.refused({"$schema": D07, "properties": {"p": {"$ref": "#/definitions/missing"}}},
                     "does not resolve")

    def test_dependencies_beside_a_dependent_keyword_are_refused(self):
        for existing in ("dependentRequired", "dependentSchemas"):
            with self.subTest(existing):
                value = {"x": ["y"]} if existing == "dependentRequired" else {"x": {"required": ["y"]}}
                document = {"$schema": D07, "dependencies": {"a": ["b"], "c": {"required": ["d"]}}}
                document[existing] = value
                self.refused(document, "both dependencies and " + existing)

    def test_a_file_in_another_format_is_refused(self):
        path = self.dir / "y.0.1.schema.json"
        path.write_text('{"$schema":"' + D07 + '"}', encoding="utf-8")
        result = run_script(str(path))
        self.assertEqual(result.returncode, 2)
        self.assertIn("canonical", result.stderr)


if __name__ == "__main__":
    unittest.main()
