#!/usr/bin/env python3
"""Audit an installed private SDK and its trusted Linux shared-library consumer."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

SYSTEM_DSO = re.compile(r"^(?:lib(?:c|m|dl|rt|pthread|resolv|atomic|stdc\+\+|gcc_s|util)\.so(?:\..*)?|ld-linux[^/]*\.so(?:\..*)?)$")
SYSTEM_ARCHIVE = {"libgcc.a", "libgcc_eh.a", "libc_nonshared.a", "libpthread_nonshared.a"}


def tool(*args):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(f"{args[0]}: {result.stderr.strip()}")
    return result.stdout


def audit(prefix, library, link_map):
    prefix = prefix.resolve()
    archives = sorted((prefix / "lib/datasystem-private").glob("*.a"))
    errors = []
    if not archives:
        errors.append("No private SDK archives installed")
    inventory = {}
    for archive in archives:
        with archive.open("rb") as stream:
            if stream.read(8) != b"!<arch>\n":
                errors.append(f"Not a full static archive: {archive.name}")
        digest = hashlib.sha256()
        with archive.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        inventory[archive.name] = digest.hexdigest()
    for path in (prefix / "lib").rglob("*"):
        if path.is_file() and re.search(r"\.so(?:\.|$)|\.dylib$", path.name):
            errors.append(f"Unexpected packaged shared library: {path.relative_to(prefix)}")
    needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", tool("readelf", "-d", str(library)))
    for name in needed:
        if not SYSTEM_DSO.fullmatch(name):
            errors.append(f"Non-system DT_NEEDED dependency: {name}")
    symbols = tool("readelf", "--dyn-syms", "--wide", str(library))
    versions = sorted(set(re.findall(r"(?:GLIBCXX|GLIBC|CXXABI)_[0-9.]+",
                                    tool("readelf", "--version-info", "--wide", str(library)))))
    if "TEXTREL" in tool("readelf", "-d", str(library)):
        errors.append("Shared consumer has text relocations")
    mapping = link_map.read_text(errors="replace")
    selected = set()
    for name, member in re.findall(r"(?:^|\s)([^\s()]+\.a)\(([^)]+)\)", mapping, re.MULTILINE):
        archive = pathlib.Path(name)
        if not archive.is_absolute():
            archive = link_map.parent / archive
        archive = archive.resolve()
        selected.add(str(archive))
        if not archive.is_relative_to(prefix / "lib/datasystem-private") and archive.name not in SYSTEM_ARCHIVE:
            errors.append(f"Archive selected outside installed SDK: {archive}")
    if not selected:
        errors.append("No selected archive members found in link map")
    return {"prefix": str(prefix), "library": str(library.resolve()), "archive_sha256": inventory,
            "selected_archives": sorted(selected), "dt_needed": needed,
            "dynamic_symbol_table_inspected": bool(symbols), "required_symbol_versions": versions, "errors": errors,
            "scope": "Static/PIC linkage evidence only; does not prove Worker behavior or final bridge symbol isolation."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=pathlib.Path, required=True)
    parser.add_argument("--library", type=pathlib.Path, required=True)
    parser.add_argument("--link-map", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        report = audit(args.prefix, args.library, args.link_map)
    except (OSError, RuntimeError) as error:
        parser.exit(2, str(error) + "\n")
    print(json.dumps(report, indent=2))
    return bool(report["errors"])


if __name__ == "__main__":
    raise SystemExit(main())
