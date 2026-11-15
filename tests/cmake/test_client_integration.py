#!/usr/bin/env python3
"""Configuration-only contracts. Fixtures never establish real dependency/ABI compatibility."""
import pathlib
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
DEPENDENCIES = {
    "gflags::gflags": "2.2.2", "brpc::brpc": "1.12.1",
    "protobuf::libprotobuf": "3.21.12", "protobuf::libprotoc": "3.21.12",
    "gRPC::grpc++": "1.51.1", "absl::symbolize": "20230802.1",
    "absl::failure_signal_handler": "20230802.1", "OpenSSL::SSL": "3.2.1",
    "OpenSSL::Crypto": "3.2.1", "leveldb::leveldb": "1.23", "ZLIB::ZLIB": "1.3.1",
    "re2::re2": "2023-07-01", "CURL::libcurl": "8.6.0", "libzmq": "4.3.5",
    "c-ares::cares": "1.26.0", "nlohmann_json::nlohmann_json": "3.11.3",
    "TBB::tbb": "2020.3", "securec::securec": "1.1.16",
    "ds_spdlog::spdlog": "1.12.0", "fixture_jemalloc": "5.3.0",
}


class ClientIntegration(unittest.TestCase):
    def configure(self, omit=None, extra="", protoc_version="3.21.12", source_targets=False):
        with tempfile.TemporaryDirectory(prefix="ds-cmake-contract-") as directory:
            root = pathlib.Path(directory)
            (root / "fixture.cpp").write_text("int fixture() { return 0; }\n")
            protoc = root / "fixture-protoc"
            protoc.write_text(f"#!/bin/sh\necho 'libprotoc {protoc_version}'\n")
            protoc.chmod(0o755)
            lines = ["cmake_minimum_required(VERSION 3.20)", "project(Host LANGUAGES C CXX)"]
            for target, version in DEPENDENCIES.items():
                if target != omit:
                    if source_targets and target in {"gflags::gflags", "protobuf::libprotobuf", "gRPC::grpc++"}:
                        real = "host_" + target.replace("::", "_").replace("+", "p")
                        lines += [f"add_library({real} STATIC fixture.cpp)",
                                  f"add_library({target} ALIAS {real})",
                                  f"set_property(TARGET {real} PROPERTY DS_DEPENDENCY_VERSION {version})"]
                    else:
                        lines += [f"add_library({target} INTERFACE IMPORTED GLOBAL)",
                                  f"set_property(TARGET {target} PROPERTY DS_DEPENDENCY_VERSION {version})"]
            lines += [
                "add_library(token INTERFACE)", "add_library(rpc_generator INTERFACE)",
                "set_property(TARGET brpc::brpc PROPERTY DS_BRPC_LOGGING RENAMED_BUTIL)",
                "set_property(TARGET TBB::tbb PROPERTY DS_TBB_BTHREAD_AWARE_YIELD TRUE)",
                "set_property(TARGET brpc::brpc PROPERTY DS_BRPC_NO_PTHREAD_MUTEX_HOOK TRUE)",
                "set_property(TARGET brpc::brpc PROPERTY DS_BRPC_USES_HOST_DEPS TRUE)",
                "add_executable(protobuf::protoc IMPORTED GLOBAL)",
                f'set_property(TARGET protobuf::protoc PROPERTY IMPORTED_LOCATION "{protoc}")',
                "add_executable(gRPC::grpc_cpp_plugin IMPORTED GLOBAL)",
                "set_property(TARGET gRPC::grpc_cpp_plugin PROPERTY DS_DEPENDENCY_VERSION 1.51.1)",
                f'set_property(TARGET gRPC::grpc_cpp_plugin PROPERTY IMPORTED_LOCATION "{protoc}")',
                "set(DS_BUILD_CLIENT_ONLY ON CACHE BOOL \"\" FORCE)",
                "set(DS_USE_EXTERNAL_DEPS ON CACHE BOOL \"\" FORCE)",
                "set(DS_PRIVATE_JEMALLOC_TARGET fixture_jemalloc)",
                "set(CMAKE_CXX_FLAGS \"-DHOST_SENTINEL\")",
                "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY \"${CMAKE_BINARY_DIR}/host-bin\")",
                "set(CMAKE_PREFIX_PATH \"${CMAKE_BINARY_DIR}/host-prefix\")",
                "set(_saved_ld \"$ENV{LD_LIBRARY_PATH}\")",
                "set(_saved_pkg \"$ENV{PKG_CONFIG_PATH}\")",
                "set(WITH_TESTS ON)", "set(BUILD_HETERO ON)", "set(BUILD_PYTHON_API ON)",
                "set(BUILD_SHARED_LIBS ON)", "set(ASCEND_HIXL_HCCS_SUPPORTED ON)", extra,
                f'add_subdirectory("{REPO}" ds)',
                "if(NOT TARGET Datasystem::client)", "message(FATAL_ERROR \"Missing public target\")", "endif()",
                "get_target_property(_type Datasystem::client TYPE)",
                "if(NOT _type STREQUAL STATIC_LIBRARY)", "message(FATAL_ERROR \"Client must be static\")", "endif()",
                "foreach(_forbidden datasystem_worker_bin datasystem_worker_shared coordinator_service_impl dsbench_cpp pybind_api common_metastore_service datasystem ds_router_client)",
                "if(TARGET ${_forbidden})", "message(FATAL_ERROR \"Unexpected service/package target: ${_forbidden}\")", "endif()", "endforeach()",
                "foreach(_target etcdapi_proto rpc_option_protos common_flags)",
                "get_target_property(_type ${_target} TYPE)",
                "if(NOT _type STREQUAL STATIC_LIBRARY)", "message(FATAL_ERROR \"Private DS DSO remains: ${_target}\")", "endif()", "endforeach()",
                "if(NOT CMAKE_CXX_FLAGS STREQUAL \"-DHOST_SENTINEL\" OR NOT WITH_TESTS OR NOT BUILD_HETERO OR NOT BUILD_PYTHON_API)",
                "message(FATAL_ERROR \"DS changed host compilation/options\")", "endif()",
                "if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY STREQUAL \"${CMAKE_BINARY_DIR}/host-bin\" OR NOT CMAKE_PREFIX_PATH STREQUAL \"${CMAKE_BINARY_DIR}/host-prefix\")",
                "message(FATAL_ERROR \"DS changed host output/search paths\")", "endif()",
                "if(NOT \"$ENV{LD_LIBRARY_PATH}\" STREQUAL \"${_saved_ld}\" OR NOT \"$ENV{PKG_CONFIG_PATH}\" STREQUAL \"${_saved_pkg}\")",
                "message(FATAL_ERROR \"DS leaked environment changes\")", "endif()",
                f'add_executable(consumer "{REPO}/example/cpp/source_client/main.cpp")',
                "target_link_libraries(consumer PRIVATE Datasystem::client)",
            ]
            (root / "CMakeLists.txt").write_text("\n".join(lines) + "\n")
            result = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build"),
                                     "-DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON" if omit == "CURL::libcurl" else "-Wno-dev"],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if result.returncode == 0:
                commands = "\n".join(path.read_text() for path in (root / "build").rglob("build.make"))
                self.assertRegex(commands, r"fixture-protoc.*--cpp_out")
                self.assertNotIn("LD_LIBRARY_PATH=", commands)
                report = (root / "build/ds/client-targets.txt").read_text()
                self.assertIn("worker_transport_api|STATIC_LIBRARY|", report)
                self.assertNotIn("|SHARED_LIBRARY|", report)
                self.assertNotIn("libprotobuf.a", report)
                self.assertNotIn("--whole-archive", report)
            return result.returncode, result.stdout

    def test_default_options_remain_off(self):
        with tempfile.TemporaryDirectory(prefix="ds-options-") as directory:
            script = pathlib.Path(directory) / "options.cmake"
            script.write_text(f'include("{REPO}/cmake/options.cmake")\n'
                              'if(DS_BUILD_CLIENT_ONLY OR DS_USE_EXTERNAL_DEPS OR DS_BUILD_STATIC_SDK)\n'
                              'message(FATAL_ERROR "Standalone defaults changed")\nendif()\n')
            result = subprocess.run(["cmake", "-P", str(script)], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_parent_targets_and_static_consumer_graph(self):
        code, output = self.configure()
        self.assertEqual(code, 0, output)

    def test_host_source_targets_are_reused(self):
        code, output = self.configure(source_targets=True)
        self.assertEqual(code, 0, output)

    def test_mapping_to_different_canonical_implementation_fails(self):
        code, output = self.configure(extra="set(DS_PROTOBUF_TARGET fixture_jemalloc)")
        self.assertNotEqual(code, 0)
        self.assertRegex(output, r"different\s+implementations")

    def test_cross_build_requires_host_tools(self):
        code, output = self.configure(extra="set(CMAKE_CROSSCOMPILING TRUE)")
        self.assertNotEqual(code, 0)
        self.assertIn("explicit imported host tool", output)

    def test_missing_dependency_fails_closed(self):
        code, output = self.configure(omit="CURL::libcurl")
        self.assertNotEqual(code, 0)
        self.assertIn("External dependency CURL is missing", output)
        self.assertNotIn("Download ", output)

    def test_unknown_mapping_is_rejected(self):
        code, output = self.configure(extra="set(DS_BRPC_TARGET nonexistent)")
        self.assertNotEqual(code, 0)
        self.assertIn("not an existing CMake target", output)

    def test_unpatched_brpc_is_rejected(self):
        code, output = self.configure(extra="set_property(TARGET brpc::brpc PROPERTY DS_BRPC_LOGGING BUTIL)")
        self.assertNotEqual(code, 0)
        self.assertIn("DS_BRPC_LOGGING", output)

    def test_unpatched_tbb_is_rejected(self):
        code, output = self.configure(extra="set_property(TARGET TBB::tbb PROPERTY DS_TBB_BTHREAD_AWARE_YIELD FALSE)")
        self.assertNotEqual(code, 0)
        self.assertIn("DS_TBB_BTHREAD_AWARE_YIELD", output)

    def test_old_protobuf_is_rejected(self):
        code, output = self.configure(extra="set(DS_PROTOBUF_VERSION 3.14.0)")
        self.assertNotEqual(code, 0)
        self.assertIn("below the integration floor", output)

    def test_wrong_protoc_is_rejected(self):
        code, output = self.configure(protoc_version="25.5")
        self.assertNotEqual(code, 0)
        self.assertIn("expected 3.21.12", output)

    def test_wrong_grpc_plugin_is_rejected(self):
        code, output = self.configure(extra="set(DS_HOST_GRPC_PLUGIN_VERSION 1.65.4)")
        self.assertNotEqual(code, 0)
        self.assertIn("must match gRPC", output)

    def test_glog_contract_configures(self):
        code, output = self.configure(extra="set_property(TARGET brpc::brpc PROPERTY DS_BRPC_LOGGING GLOG)")
        self.assertEqual(code, 0, output)


if __name__ == "__main__":
    unittest.main()
