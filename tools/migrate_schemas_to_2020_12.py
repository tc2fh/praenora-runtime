#!/usr/bin/env python3
r"""Rewrites the runtime-owned JSON Schemas from draft-07 to JSON Schema 2020-12.

The runtime-owned schemas are the document schemas in ``spec/schemas/*.schema.json`` and the data
schema of every message type in ``spec/catalog/messages/*/*.json`` (the ``schema`` member of a
``message-type`` entry). Schemas that modules supply (``parameters_schema``) and the test fixtures
that stand for them keep the dialect they declare; this script does not touch them.

Only a schema that is draft-07 is rewritten: one that declares draft-07 in ``$schema``, or declares
nothing (draft-07 is the default of the runtime, ADR-0043), in which case the 2020-12 ``$schema`` is
added as its first member. A schema that declares 2020-12 is left byte for byte as it is; any other
dialect stops the script (exit status 2).

The rewrite is structural: it walks a schema only through the keywords that hold subschemas, so a
property that happens to be called ``definitions`` or ``items`` is left alone, and so are the values
of ``const``, ``enum``, ``default`` and ``examples``. It applies these rules:

- ``$schema`` becomes ``https://json-schema.org/draft/2020-12/schema``.
- ``definitions`` becomes ``$defs`` wherever it is the keyword, and every local ``$ref`` is
  rewritten to follow what it points at, whatever the depth: ``#/definitions/a`` and
  ``#/properties/p/definitions/a`` become ``#/$defs/a`` and ``#/properties/p/$defs/a``; a pointer
  into tuple ``items``, ``additionalItems`` or ``dependencies`` follows the renamed keyword too.
- ``items`` that is an array becomes ``prefixItems``, and ``additionalItems`` becomes ``items``
  (``additionalItems`` beside an ``items`` that is not an array did nothing and is dropped).
- ``dependencies`` is split: array values go to ``dependentRequired``, schema values to
  ``dependentSchemas``; a schema that already has one of those two keywords stops the script.
- ``$id`` loses an empty fragment (``...#``), which 2020-12 does not allow.

Draft-07 ignores the keywords beside a ``$ref``; 2020-12 applies them. A ``$ref`` that has siblings is
therefore only accepted when all of them are ``title``, ``description``, ``$comment`` or ``examples``
(the annotations that the runtime's AsyncAPI export and its check for ignored siblings also let
through); anything else stops the script with exit status 2 and the migration of that file is not
written, because the meaning would change. So does a ``$ref`` that cannot be resolved.

Keys keep their position, files keep their formatting (two-space indent, UTF-8, ``\n`` line ends,
a final newline), and a file that is already 2020-12 is left as it is, so the script can be run
again. ``--check`` compares bytes, so a file whose line endings differ from the script's output also
counts as changed.

Only the Python standard library is used. From the repository root::

    python3 tools/migrate_schemas_to_2020_12.py            # rewrite the files in place
    python3 tools/migrate_schemas_to_2020_12.py --check    # change nothing; exit 1 if a file would change
    python3 tools/migrate_schemas_to_2020_12.py PATH...    # files or directories instead of the defaults
"""

import argparse
import json
import sys
from pathlib import Path

DRAFT07 = "http://json-schema.org/draft-07/schema"
DRAFT2020 = "https://json-schema.org/draft/2020-12/schema"

# Allowed beside a $ref: the annotations that the runtime treats as such (schema.cpp).
ANNOTATIONS = {"title", "description", "$comment", "examples"}

# Keywords whose value is one subschema, a map of subschemas, or an array of subschemas.
SCHEMA_KEYWORDS = (
    "additionalProperties", "additionalItems", "contains", "propertyNames", "not", "if", "then",
    "else", "unevaluatedItems", "unevaluatedProperties", "contentSchema",
)
MAP_KEYWORDS = ("properties", "patternProperties", "definitions", "$defs", "dependentSchemas")
ARRAY_KEYWORDS = ("allOf", "anyOf", "oneOf", "prefixItems")


class Unsafe(Exception):
    """A rewrite that would change what the schema accepts, or a file the script will not touch."""


def unescape(segment):
    return segment.replace("~1", "/").replace("~0", "~")


def escape(segment):
    return segment.replace("~", "~0").replace("/", "~1")


def rewrite_pointer(root, pointer, where):
    """The 2020-12 spelling of the JSON pointer `pointer` into the draft-07 schema `root`.

    The pointer is followed through the original schema, keyword by keyword, so a segment is
    renamed only where it is the keyword: a property called "definitions" keeps its name.
    """
    if pointer == "":
        return ""
    if not pointer.startswith("/"):
        raise Unsafe(f"{where}: cannot follow the reference fragment {pointer!r}")
    node = root
    context = "schema"  # what `node` is: a schema, a map of schemas, an array of schemas, other
    out = []
    for raw in pointer[1:].split("/"):
        segment = unescape(raw)
        if context == "schema":
            if not isinstance(node, dict) or segment not in node:
                raise Unsafe(f"{where}: the reference does not resolve at {segment!r}")
            value = node[segment]
            if segment == "definitions" and isinstance(value, dict):
                out.append("$defs")
                context = "map"
            elif segment == "dependencies" and isinstance(value, dict):
                # Each entry is renamed with the kind of its value; the next segment names it.
                out.append(None)
                context = "dependencies"
            elif segment == "items" and isinstance(value, list):
                out.append("prefixItems")
                context = "array"
            elif segment == "additionalItems" and isinstance(node.get("items"), list):
                out.append("items")
                context = "schema"
            elif segment in MAP_KEYWORDS and isinstance(value, dict):
                out.append(raw)
                context = "map"
            elif segment in ARRAY_KEYWORDS and isinstance(value, list):
                out.append(raw)
                context = "array"
            elif segment in SCHEMA_KEYWORDS or segment == "items":
                out.append(raw)
                context = "schema"
            else:
                out.append(raw)
                context = "other"
            node = value
        elif context == "map":
            if not isinstance(node, dict) or segment not in node:
                raise Unsafe(f"{where}: the reference does not resolve at {segment!r}")
            out.append(raw)
            node = node[segment]
            context = "schema"
        elif context == "array":
            try:
                node = node[int(segment)]
            except (ValueError, IndexError, TypeError):
                raise Unsafe(f"{where}: the reference does not resolve at {segment!r}") from None
            out.append(raw)
            context = "schema"
        elif context == "dependencies":
            if not isinstance(node, dict) or segment not in node:
                raise Unsafe(f"{where}: the reference does not resolve at {segment!r}")
            node = node[segment]
            kind = "dependentRequired" if isinstance(node, list) else "dependentSchemas"
            out[-1] = kind
            out.append(raw)
            context = "other" if isinstance(node, list) else "schema"
        else:
            out.append(raw)
            if isinstance(node, dict) and segment in node:
                node = node[segment]
            elif isinstance(node, list):
                try:
                    node = node[int(segment)]
                except (ValueError, IndexError):
                    node = None
            else:
                node = None
    if out and out[-1] is None:
        raise Unsafe(f"{where}: a reference to the whole of dependencies cannot be migrated")
    return "/" + "/".join(out)


def fix_ref(ref, root, where):
    if not ref.startswith("#"):
        return ref  # another document: not ours to rewrite
    return "#" + rewrite_pointer(root, ref[1:], where)


def migrate_schema(node, where, stats, root):
    """Rewrites one schema (a mapping or a boolean) and returns the new one."""
    if not isinstance(node, dict):
        return node

    if "$ref" in node:
        extra = set(node) - {"$ref"} - ANNOTATIONS
        if extra:
            raise Unsafe(f"{where}: keywords beside $ref change meaning in 2020-12: {sorted(extra)}")

    out = {}
    for key, value in node.items():
        here = f"{where}/{key}"
        if key == "$schema" and isinstance(value, str):
            new = DRAFT2020 if value.rstrip("#") == DRAFT07 else value
            stats["$schema"] += new != value
            out[key] = new
        elif key == "$id" and isinstance(value, str) and value.endswith("#"):
            stats["$id"] += 1
            out[key] = value[:-1]
        elif key == "$ref" and isinstance(value, str):
            new = fix_ref(value, root, here)
            stats["$ref"] += new != value
            out[key] = new
        elif key == "definitions" and isinstance(value, dict):
            if "$defs" in node:
                raise Unsafe(f"{where}: both definitions and $defs")
            stats["definitions"] += 1
            out["$defs"] = {name: migrate_schema(sub, f"{here}/{name}", stats, root) for name, sub in value.items()}
        elif key == "items" and isinstance(value, list):
            if "prefixItems" in node:
                raise Unsafe(f"{where}: both tuple items and prefixItems")
            stats["items"] += 1
            out["prefixItems"] = [migrate_schema(sub, f"{here}/{i}", stats, root) for i, sub in enumerate(value)]
        elif key == "additionalItems":
            if isinstance(node.get("items"), list):
                stats["additionalItems"] += 1
                out["items"] = migrate_schema(value, here, stats, root)
            else:
                stats["additionalItems dropped"] += 1  # ignored by draft-07 without a tuple
        elif key == "dependencies" and isinstance(value, dict):
            required = {k: v for k, v in value.items() if isinstance(v, list)}
            schemas = {k: v for k, v in value.items() if not isinstance(v, list)}
            for new_key, part in (("dependentRequired", required), ("dependentSchemas", schemas)):
                if not part:
                    continue
                if new_key in node:
                    raise Unsafe(f"{where}: both dependencies and {new_key}")
                stats["dependencies"] += 1
                if new_key == "dependentRequired":
                    out[new_key] = part
                else:
                    out[new_key] = {k: migrate_schema(v, f"{here}/{k}", stats, root) for k, v in part.items()}
        elif key == "items":
            out[key] = migrate_schema(value, here, stats, root)
        elif key in SCHEMA_KEYWORDS:
            out[key] = migrate_schema(value, here, stats, root)
        elif key in MAP_KEYWORDS and isinstance(value, dict):
            out[key] = {name: migrate_schema(sub, f"{here}/{name}", stats, root) for name, sub in value.items()}
        elif key in ARRAY_KEYWORDS and isinstance(value, list):
            out[key] = [migrate_schema(sub, f"{here}/{i}", stats, root) for i, sub in enumerate(value)]
        else:
            out[key] = value
    return out


def migrate_root(schema, where, stats):
    """Migrates a whole schema document: only a draft-07 one (declared or by default)."""
    if not isinstance(schema, dict):
        return schema
    declared = schema.get("$schema")
    if declared is None:
        new = migrate_schema(schema, where, stats, schema)
        stats["$schema"] += 1
        return {"$schema": DRAFT2020, **new}
    if not isinstance(declared, str):
        raise Unsafe(f"{where}: $schema is not a string")
    if declared.rstrip("#") == DRAFT07:
        return migrate_schema(schema, where, stats, schema)
    if declared == DRAFT2020:
        return schema
    raise Unsafe(f"{where}: $schema is {declared!r}, neither draft-07 nor 2020-12")


def dump(document):
    return json.dumps(document, indent=2, ensure_ascii=False) + "\n"


def migrate_file(path, stats):
    """Returns (text, new text) of `path`, or None when it is not a runtime-owned schema."""
    text = path.read_bytes().decode("utf-8")  # bytes, not universal newlines: line ends count
    document = json.loads(text)
    if not isinstance(document, dict):
        return None
    if path.name.endswith(".schema.json"):
        new = migrate_root(document, "", stats)
    elif document.get("kind") == "message-type" and isinstance(document.get("schema"), dict):
        new = dict(document)
        new["schema"] = migrate_root(document["schema"], "/schema", stats)
    else:
        return None
    if dump(document) != text.replace("\r\n", "\n"):
        raise Unsafe(f"{path}: the file is not in the canonical two-space form; it would be reformatted")
    return text, dump(new)


def files_under(paths):
    for path in paths:
        if path.is_dir():
            yield from sorted(path.rglob("*.json"))
        else:
            yield path


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("paths", nargs="*", type=Path, help="files or directories (default: spec/schemas and spec/catalog/messages)")
    parser.add_argument("--check", action="store_true", help="change nothing; exit 1 if a file would change")
    args = parser.parse_args()
    paths = args.paths or [root / "spec" / "schemas", root / "spec" / "catalog" / "messages"]

    changed = []
    stats = {name: 0 for name in ("$schema", "$id", "$ref", "definitions", "items", "additionalItems",
                                   "additionalItems dropped", "dependencies")}
    try:
        for path in files_under(paths):
            result = migrate_file(path, stats)
            if result is not None and result[0] != result[1]:
                changed.append((path, result[1]))
    except Unsafe as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    for path, new in changed:
        if not args.check:
            path.write_bytes(new.encode("utf-8"))
        print(("would change " if args.check else "rewrote ") + str(path))
    summary = ", ".join(f"{name} {count}" for name, count in stats.items() if count)
    print(f"{len(changed)} file(s) {'would change' if args.check else 'rewritten'}" + (f" ({summary})" if summary else ""))
    return 1 if (args.check and changed) else 0


if __name__ == "__main__":
    sys.exit(main())
