#!/usr/bin/env python3
"""Keep plugin JSON Schemas honest against the plugin source.

Each plugin that ships plugins/<kind>/<kind>.schema.json is checked both ways:

  * every property named anywhere in the schema must be read by the plugin's
    source (a renamed or removed key fails), and
  * every string key the source reads with value("k") / contains("k") /
    at("k") / find("k") / ["k"] must be a schema property or listed in the
    schema's "x-not-config" array (keys of events it parses or builds, JSON
    responses from servers, etc.), so a new config key can't ship undocumented.

The extraction is lexical, so "x-not-config" is where non-config keys are named
explicitly. That list is the price of catching drift automatically.

Usage:
  tools/check_plugin_schemas.py              check every plugin with a schema (exit 1 on drift)
  tools/check_plugin_schemas.py --dump KIND  print each key the source reads, with file:line
  tools/check_plugin_schemas.py --kinds A,B  check only these
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PLUGINS = ROOT / "plugins"
SOURCE_SUFFIXES = {".cpp", ".cc", ".hpp", ".h", ".mm"}
KEY_RE = re.compile(
    r"""(?:\.(?:value|contains|at|find|count)\s*\(\s*|\[\s*)"([A-Za-z_][A-Za-z0-9_]*)"\s*[,)\]]"""
)


def plugin_sources(kind: str) -> list[pathlib.Path]:
    base = PLUGINS / kind
    return sorted(
        p for p in base.rglob("*")
        if p.suffix in SOURCE_SUFFIXES and "tests" not in p.relative_to(base).parts
    )


def source_keys(kind: str) -> dict[str, list[str]]:
    """key -> ["file:line  text", ...]"""
    found: dict[str, list[str]] = {}
    for path in plugin_sources(kind):
        for lineno, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            for m in KEY_RE.finditer(line):
                found.setdefault(m.group(1), []).append(
                    f"{path.relative_to(ROOT)}:{lineno}  {stripped[:110]}"
                )
    return found


def schema_properties(node) -> set[str]:
    names: set[str] = set()
    if isinstance(node, dict):
        for name, sub in (node.get("properties") or {}).items():
            names.add(name)
            names |= schema_properties(sub)
        for key in ("items", "additionalProperties", "patternProperties"):
            sub = node.get(key)
            if isinstance(sub, dict):
                names |= schema_properties(sub if key != "patternProperties" else {"properties": sub})
        for key in ("oneOf", "anyOf", "allOf"):
            for sub in node.get(key) or []:
                names |= schema_properties(sub)
    return names


def check(kind: str) -> list[str]:
    schema_path = PLUGINS / kind / f"{kind}.schema.json"
    try:
        schema = json.loads(schema_path.read_text())
    except json.JSONDecodeError as e:
        return [f"{schema_path.relative_to(ROOT)}: invalid JSON: {e}"]
    problems = []
    if schema.get("x-plugin-kind") != kind:
        problems.append(f"{kind}: x-plugin-kind is {schema.get('x-plugin-kind')!r}, expected {kind!r}")
    if not isinstance(schema.get("x-plugin-version"), str):
        problems.append(f"{kind}: x-plugin-version (string) is required")
    props = schema_properties(schema)
    not_config = set(schema.get("x-not-config") or [])
    keys = source_keys(kind)
    for name in sorted(props - set(keys)):
        problems.append(f"{kind}: schema property '{name}' is never read by the source")
    for name in sorted(set(keys) - props - not_config):
        where = keys[name][0]
        problems.append(f"{kind}: source reads '{name}' but the schema has no such property "
                        f"(add it, or list it in x-not-config)  [{where}]")
    for name in sorted(not_config & props):
        problems.append(f"{kind}: '{name}' is both a schema property and in x-not-config")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", metavar="KIND")
    ap.add_argument("--kinds")
    args = ap.parse_args()

    if args.dump:
        for key, uses in sorted(source_keys(args.dump).items()):
            print(key)
            for u in uses:
                print("    " + u)
        return 0

    kinds = sorted(p.parent.name for p in PLUGINS.glob("*/*.schema.json"))
    if args.kinds:
        wanted = set(args.kinds.split(","))
        kinds = [k for k in kinds if k in wanted]
    problems = [p for k in kinds for p in check(k)]
    for p in problems:
        print(p)
    print(f"checked {len(kinds)} plugin schema(s): {'OK' if not problems else f'{len(problems)} problem(s)'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
