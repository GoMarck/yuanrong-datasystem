#!/usr/bin/env python3
"""Parser-only audit regressions; mocked tool output is not ELF/ABI evidence."""
import importlib.util
import pathlib
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("client_linkage", REPO / "scripts/verify_client_linkage.py")
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class LinkageAudit(unittest.TestCase):
    def audit(self, mapping, shared=None, duplicate_symbol=False):
        with tempfile.TemporaryDirectory(prefix="ds-link-audit-") as directory:
            root = pathlib.Path(directory)
            binary = root / "consumer"
            link_map = root / "consumer.map"
            report = root / "targets.txt"
            link_map.write_text(mapping)
            report.write_text("datasystem_client|STATIC_LIBRARY|common_rpc_zmq_client|\n")

            def tool(*args):
                if args[0] == "readelf":
                    return ""
                if args[0] == "ldd":
                    return f"{shared} => {root / shared} (0x1000)\n" if shared else ""
                if args[0] == "nm":
                    return "00000001 T google::FlagRegisterer::FlagRegisterer()\n" if duplicate_symbol else ""
                raise AssertionError(args)

            with mock.patch.object(AUDIT, "run", side_effect=tool):
                return AUDIT.inspect(binary, link_map, report)

    def test_repeated_archive_and_unused_load_are_allowed(self):
        result = self.audit("one/libgflags.a(flags.o)\none/libgflags.a(flags.o)\nLOAD two/libgflags.a\n")
        self.assertEqual(result["errors"], [])
        self.assertEqual(len(result["selected_common_providers"]["gflags"]), 1)

    def test_distinct_selected_archives_are_reported(self):
        result = self.audit("one/libgflags.a(flags.o)\ntwo/libgflags.a(flags.o)\n")
        self.assertTrue(any("Multiple selected gflags" in error for error in result["errors"]))

    def test_duplicate_loaded_strong_symbol_is_reported(self):
        result = self.audit("", shared="libcustom.so", duplicate_symbol=True)
        self.assertTrue(result["duplicate_strong_symbols"])

    def test_transitive_private_core_dso_is_reported(self):
        result = self.audit("", shared="libdatasystem.so")
        self.assertTrue(any("Unexpected transitive" in error for error in result["errors"]))


if __name__ == "__main__":
    unittest.main()
