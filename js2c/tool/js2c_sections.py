#!/usr/bin/env python3
"""
Derive a top-level "sections" schema from a project's authored config schema.

A project's config is assembled from fragments owned by the components that
consume them: wifi_manager owns the wifi section, mqtt_manager the mqtt section.
Each of those components generates its own parser and its own C type from its own
fragment, which is what lets a component be used without the project it came from.

That leaves the top level. It cannot be generated from the authored schema
directly: json_schema_to_c would emit a second definition of every section's
struct (ObjectGenerator always declares, there is no "declared elsewhere" mode),
colliding with the one the owning component already generates.

So this script rewrites every top-level property that is a cross-file $ref into
{"js2cType": "raw"}, which makes the generated parser record that section's byte
offset and length instead of parsing it. The project then hands each slice to the
parser belonging to the component that owns it, in place and without copying.

The point is that there is only ever one authored file. The old arrangement kept
an aggregate schema for documentation and a hand-written jsmn walker for the
firmware, and they drifted four separate ways -- a section the walker read that
no schema declared, a section the schema declared that no struct held, and a
section the schema placed at the root that the walker looked for one level down.
Here the walker *is* the schema, mechanically reduced, so it cannot disagree.

Emits three things:
  - the sections schema, to feed to json_schema_to_c
  - a header defining <ID>_SECTIONS(X), an X-macro over the section names, so a
    section added to the schema but not dispatched in C fails to compile
  - a token budget, embedded in the schema, see below
"""

import argparse
import copy
import json
import os
import sys
from collections import OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from js2c.codegen.base import SchemaError          # noqa: E402
from js2c.codegen.root import RootGenerator        # noqa: E402
from js2c.schema import load_schema                # noqa: E402
from js2c.settings import Settings                 # noqa: E402


def die(message):
    print(f"js2c_sections: {message}", file=sys.stderr)
    sys.exit(1)


def is_cross_file_ref(node):
    """A section is a property whose value is a reference into another file."""
    if not isinstance(node, dict) or "$ref" not in node:
        return False
    # "#/$defs/x" stays in this file and is a normal in-file reference; a section
    # is something owned by another component, which means another file.
    return not node["$ref"].startswith("#")


def load_pointer(target, pointer, ref, authorized_paths):
    """Load a fragment and walk into it, if the $ref carried a JSON pointer."""
    # load_schema resolves the fragment's own in-file references.
    schema = load_schema(target, authorized_paths)

    for part in [p for p in pointer.split("/") if p]:
        if not isinstance(schema, dict) or part not in schema:
            die(f"$ref '{ref}': no such member '{part}'")
        schema = schema[part]

    return schema


def parse_component_roots(pairs):
    """--component-root name=dir, repeated. Maps a section's first path segment
    to the directory of the component that owns it."""
    roots = {}
    for pair in pairs or []:
        name, sep, directory = pair.partition("=")
        if not sep or not name:
            die(f"--component-root expects name=dir, got '{pair}'")
        roots[name] = directory
    return roots


def resolve_section(ref, base_dir, authorized_paths, component_roots):
    """
    Load the schema a section's $ref points at.

    Resolution is done here rather than by json_schema_to_c's loader so that a
    section may reference a fragment's *root* -- "wifi_config_schema.json" with
    no fragment pointer. That is the form a component can generate its own parser
    from directly, and the form VS Code resolves for editor validation, but it is
    the one form the generator's own loader cannot follow: it requires a JSON
    pointer, and the empty pointer "#/" makes it look up "" as a key and crash.

    A pointer is still honoured if one is given, so both spellings work.
    """
    path, _, pointer = ref.partition("#")
    if not path:
        die(f"section $ref '{ref}' points into the same file; a section is owned by "
            f"another component, so it must name that component's schema file")

    # A section names its fragment by the component that owns it:
    # "wifi_manager/wifi_config_schema.json". The first segment is resolved to
    # that component's directory, so the reference says nothing about where the
    # component lives -- in components/, in managed_components/ under a namespaced
    # directory name, or anywhere else EXTRA_COMPONENT_DIRS points.
    #
    # Resolving through the component name rather than a staged copy also makes
    # this independent of the order components are configured in, which a staging
    # step is not: ESP-IDF gives `main` an implicit dependency on every component
    # but still configures it before most of them.
    roots = [base_dir]
    segment = path.split("/", 1)[0]
    if segment in component_roots and "/" in path:
        candidate = os.path.abspath(
            os.path.join(component_roots[segment], path.split("/", 1)[1]))
        if os.path.exists(candidate):
            return load_pointer(candidate, pointer, ref, authorized_paths)
    roots += [os.path.abspath(p) for p in authorized_paths]
    target = None
    for root in roots:
        candidate = os.path.abspath(os.path.join(root, path))
        # Containment check per root, so a "../.." in a $ref cannot escape.
        root_abs = os.path.abspath(root)
        if not (candidate == root_abs or candidate.startswith(root_abs + os.sep)):
            continue
        if os.path.exists(candidate):
            target = candidate
            break

    if target is None:
        searched = "\n  ".join(os.path.abspath(r) for r in roots)
        known = ", ".join(sorted(component_roots)) or "(none)"
        die(f"$ref '{ref}' names a file that does not exist. Searched:\n  {searched}\n"
            f"A section's first path segment must be the name of the component that "
            f"owns the fragment. Components in this build: {known}")

    return load_pointer(target, pointer, ref, authorized_paths)


def token_budget(raw, sections, base_dir, authorized_paths, component_roots):
    """
    How many jsmn tokens the *whole* document needs.

    A raw section costs one token in the generated parser, but the tokenizer still
    has to hold every token of the content being skipped -- so the budget cannot
    come from the sections schema. It is computed from the authored schema with
    every section inlined, using json_schema_to_c's own accounting rather than a
    reimplementation of it, so the two cannot drift apart.

    The result is passed as allow_additional_properties, which the generator adds
    directly to the token buffer size. Over-provisioning costs a little stack;
    under-provisioning fails at runtime with JSMN_ERROR_NOMEM on a config that is
    merely large, which is much the worse way to be wrong.
    """
    inlined = copy.deepcopy(raw)
    for name in sections:
        inlined["properties"][name] = resolve_section(
            raw["properties"][name]["$ref"], base_dir, authorized_paths, component_roots)

    settings = Settings({}, inlined.get("js2cSettings", {}))
    return RootGenerator(inlined, settings).root_generator.max_token_num()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("schema_file", help="the authored aggregate config schema")
    ap.add_argument("out_schema", help="derived sections schema, for json_schema_to_c")
    ap.add_argument("out_header", help="generated header defining the X-macro")
    ap.add_argument("--authorized-paths", type=str, nargs="+", default=None,
                    metavar="path",
                    help="directories the schema may reference across files")
    ap.add_argument("--component-root", type=str, action="append", default=None,
                    metavar="name=dir",
                    help="map a component name to its directory, so a section can "
                         "reference the fragment owned by that component")
    args = ap.parse_args()

    authorized = list(args.authorized_paths or [])
    authorized.append(os.path.dirname(os.path.abspath(args.schema_file)))

    with open(args.schema_file, encoding="utf-8") as handle:
        raw = json.load(handle, object_pairs_hook=OrderedDict)

    schema_id = raw.get("$id")
    if not schema_id:
        die(f"{args.schema_file}: the schema needs an '$id'; it names the generated type")

    properties = raw.get("properties")
    if not isinstance(properties, OrderedDict):
        die(f"{args.schema_file}: expected a top-level 'properties' object")

    sections = [name for name, node in properties.items() if is_cross_file_ref(node)]
    if not sections:
        die(f"{args.schema_file}: no sections found. A section is a top-level property "
            f"whose value is a $ref into another file, e.g. "
            f'"wifi": {{"$ref": "wifi_manager/wifi_config_schema.json"}}')

    # A raw field cannot carry a js2cDefault, so the generator has no value to
    # supply when the key is absent and will refuse to generate at all unless the
    # field is required. Catch it here, where the message can say what to do.
    required = raw.get("required", [])
    missing = [name for name in sections if name not in required]
    if missing:
        die(f"{args.schema_file}: section(s) {', '.join(missing)} must be listed in "
            f"'required'. A raw section has no default, so an absent one would leave "
            f"the slice zeroed rather than reporting a problem. Write the section as "
            f"{{}} in the config file to mean 'all defaults'.")

    base_dir = os.path.dirname(os.path.abspath(args.schema_file))
    component_roots = parse_component_roots(args.component_root)
    try:
        budget = token_budget(raw, sections, base_dir, authorized, component_roots)
    except (ValueError, OSError) as exc:
        die(f"resolving {args.schema_file}: {exc}")
    except SchemaError as exc:
        die(f"{exc}")

    out = copy.deepcopy(raw)
    # Every raw field shares one generated type (<id>_json_ref_t), and the doc
    # comment travels with the type rather than the field -- so a per-section
    # description would be emitted against all of them, naming whichever section
    # happened to come first. One description that is true of every section.
    description = ("Byte offset and length of this section within the input JSON. "
                   "Parsed by the component that owns it.")
    for name in sections:
        out["properties"][name] = OrderedDict((
            ("js2cType", "raw"),
            ("description", description),
        ))

    settings = out.setdefault("js2cSettings", OrderedDict())
    settings["allow_additional_properties"] = budget

    out["$comment"] = (f"Generated by js2c_sections.py from "
                       f"{os.path.basename(args.schema_file)}. Do not edit.")

    os.makedirs(os.path.dirname(os.path.abspath(args.out_schema)), exist_ok=True)
    with open(args.out_schema, "w", encoding="utf-8") as handle:
        json.dump(out, handle, indent=2)
        handle.write("\n")

    macro = f"{schema_id.upper()}_SECTIONS"
    guard = f"{schema_id.upper()}_SECTIONS_H"
    lines = [
        "/* This file was generated by js2c_sections.py.",
        " * Any changes made to it will be lost on regeneration. */",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "/*",
        f" * Every section of {schema_id}, as an X-macro.",
        " *",
        " * Dispatch with this rather than by hand: a section added to the schema",
        " * expands to an X() the compiler cannot resolve until it is wired up, so a",
        " * forgotten section is a build failure instead of a silently zeroed struct.",
        " */",
        f"#define {macro}(X) \\",
    ]
    for index, name in enumerate(sections):
        end = "" if index == len(sections) - 1 else " \\"
        lines.append(f"    X({name}){end}")
    lines += ["", f"#define {macro}_COUNT {len(sections)}", "", f"#endif /* {guard} */", ""]

    os.makedirs(os.path.dirname(os.path.abspath(args.out_header)), exist_ok=True)
    with open(args.out_header, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))

    print(f"js2c_sections: {len(sections)} section(s): {', '.join(sections)}; "
          f"token budget {budget}")


if __name__ == "__main__":
    main()
