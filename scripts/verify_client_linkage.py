#!/usr/bin/env python3
"""Inspect a trusted Linux consumer, its link map and DS target graph.

This audit finds visible duplicate implementations; it cannot prove ABI or exclude
hidden symbols embedded inside third-party DSOs. Review unified dependency builds too.
"""
import argparse
import collections
import json
import pathlib
import re
import subprocess

FAMILIES = {
    "gflags": re.compile(r"libgflags(?:[._-]|$)"),
    "brpc": re.compile(r"libbrpc(?:[._-]|$)"),
    "protobuf": re.compile(r"libprotobuf(?:[._-]|$)"),
    "grpc": re.compile(r"libgrpc\+\+(?:[._-]|$)"),
}
PRIVATE_CORE = re.compile(r"lib(?:datasystem(?:_worker)?|common_flags|rpc_option_protos|etcdapi_proto)\.so")
SYMBOL = re.compile(r"(?:google::(?:FlagRegisterer|SetCommandLineOption)|brpc::(?:Channel|Server)::|google::protobuf::MessageLite::)")


def run(*command):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(f"{command[0]} failed: {result.stderr.strip()}")
    return result.stdout


def needed(path):
    return re.findall(r"\(NEEDED\).*?\[(.*?)\]", run("readelf", "-d", str(path)))


def inspect(binary, link_map, targets):
    errors = []
    mapping = link_map.read_text(errors="replace")
    target_graph = targets.read_text()
    if "datasystem_client|STATIC_LIBRARY|" not in target_graph:
        errors.append("Target report does not contain the static Client")
    if "--whole-archive" in target_graph:
        errors.append("Unexpected whole-archive in DS target graph")
    direct_needed = needed(binary)
    for name in direct_needed:
        if PRIVATE_CORE.search(name) or re.search(r"cann_hixl|ascendcl|metadef", name):
            errors.append(f"Unexpected Client-only runtime dependency: {name}")
    loaded = {binary.resolve()}
    ldd = run("ldd", str(binary))
    if "not found" in ldd:
        errors.append("ldd reports unresolved runtime dependencies")
    for line in ldd.splitlines():
        match = re.search(r"(?:=>\s*)?(/[^\s]+)\s+\(", line)
        if match:
            loaded.add(pathlib.Path(match.group(1)).resolve())
    # Archive members in the map show selected objects, unlike repeated .a inputs.
    archive_members = set(re.findall(r"(?:^|\s)([^\s()]+\.a)\(([^)]+)\)", mapping, re.MULTILINE))
    providers = collections.defaultdict(set)
    for path in loaded:
        for family, pattern in FAMILIES.items():
            if pattern.search(path.name):
                providers[family].add(str(path))
    for archive, member in archive_members:
        path = pathlib.Path(archive)
        if not path.is_absolute():
            path = link_map.parent / path
        path = path.resolve()
        for family, pattern in FAMILIES.items():
            if pattern.search(path.name):
                providers[family].add(str(path))
    for family, paths in providers.items():
        if len(paths) > 1:
            errors.append(f"Multiple selected {family} providers: {sorted(paths)}")
    for path in loaded:
        if PRIVATE_CORE.search(path.name) or re.search(r"lib(?:cann_hixl|ascendcl|metadef)", path.name):
            errors.append(f"Unexpected transitive Client-only runtime library: {path}")
    definitions = collections.defaultdict(set)
    for path in loaded:
        args = ("nm", "-C", "--defined-only", str(path)) if path == binary.resolve() else (
            "nm", "-D", "-C", "--defined-only", str(path))
        symbols = run(*args)
        for line in symbols.splitlines():
            match = re.match(r"^[0-9a-fA-F]+\s+[TDB]\s+(.+)$", line)
            if match and SYMBOL.search(match.group(1)):
                definitions[match.group(1)].add(str(path))
    duplicates = {symbol: sorted(paths) for symbol, paths in definitions.items() if len(paths) > 1}
    if duplicates:
        errors.append("Common strong symbols are defined in multiple loaded artifacts")
    return {"binary": str(binary.resolve()), "dt_needed": direct_needed,
            "loaded_artifacts": sorted(map(str, loaded)),
            "selected_common_providers": {key: sorted(value) for key, value in providers.items()},
            "selected_archive_members": len(archive_members),
            "duplicate_strong_symbols": duplicates, "errors": errors,
            "limits": "Does not establish ABI/runtime correctness or exclude hidden third-party symbols."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=pathlib.Path)
    parser.add_argument("--link-map", required=True, type=pathlib.Path)
    parser.add_argument("--targets-report", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        result = inspect(args.binary, args.link_map, args.targets_report)
    except (OSError, RuntimeError) as error:
        parser.exit(2, str(error) + "\n")
    print(json.dumps(result, indent=2))
    return bool(result["errors"])


if __name__ == "__main__":
    raise SystemExit(main())
