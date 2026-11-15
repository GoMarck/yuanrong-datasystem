#!/usr/bin/env python3
"""Package plumbing tests with small real archives, not DS/third-party compatibility tests."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


def command(*args):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise AssertionError(f"Command {args} failed:\n{result.stdout}")
    return result.stdout


class PrivateSdkPackage(unittest.TestCase):
    def test_relocated_package_ignores_host_targets_and_links_shared(self):
        with tempfile.TemporaryDirectory(prefix="ds-private-package-") as directory:
            root = pathlib.Path(directory)
            source = root / "source"
            source.mkdir()
            dependencies = root / "dependencies"
            (dependencies / "include").mkdir(parents=True)
            (source / "dep.cpp").write_text("int private_dependency() { return 42; }\n")
            command("c++", "-fPIC", "-c", str(source / "dep.cpp"), "-o", str(dependencies / "dep.o"))
            command("ar", "rcs", str(dependencies / "libdependency.a"), str(dependencies / "dep.o"))
            (source / "client.cpp").write_text("int private_dependency(); int package_entry() { return private_dependency(); }\n")
            (source / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.20)
project(PackageFixture LANGUAGES CXX C)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(DS_SOURCE_DIR "{REPO}")
set(DS_BINARY_DIR "${{CMAKE_BINARY_DIR}}")
set(DATASYSTEM_VERSION 0.9.3)
set_property(GLOBAL PROPERTY DS_PRIVATE_SDK_PREFIXES "{dependencies}")
add_library(protobuf::libprotobuf STATIC IMPORTED)
set_target_properties(protobuf::libprotobuf PROPERTIES IMPORTED_LOCATION "{dependencies}/libdependency.a"
    INTERFACE_INCLUDE_DIRECTORIES "{dependencies}/include")
add_library(client STATIC client.cpp)
find_package(Threads REQUIRED)
target_link_libraries(client PRIVATE protobuf::libprotobuf Threads::Threads "{dependencies}/libdependency.a")
include("{REPO}/cmake/private-sdk-package.cmake")
ds_install_private_sdk(client)
""")
            build = root / "build"
            prefix = root / "installed"
            command("cmake", "-S", str(source), "-B", str(build), f"-DCMAKE_INSTALL_PREFIX={prefix}")
            command("cmake", "--build", str(build))
            command("cmake", "--install", str(build))
            relocated = root / "relocated"
            prefix.rename(relocated)
            shutil.rmtree(source)
            shutil.rmtree(build)
            shutil.rmtree(dependencies)
            self.assertEqual(len(list((relocated / "lib/datasystem-private").glob("*.a"))), 2)
            config = (relocated / "lib/cmake/DatasystemPrivate/DatasystemPrivateTargets.cmake").read_text()
            self.assertNotIn("protobuf::libprotobuf", config)
            self.assertNotIn(str(source), config)
            self.assertNotIn(str(REPO), config)
            self.assertNotIn(str(build), config)
            consumer = root / "consumer"
            consumer.mkdir()
            (consumer / "bridge.cpp").write_text("int package_entry(); extern \"C\" int fixture_bridge() { return package_entry(); }\n")
            (consumer / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.20)
project(Consumer LANGUAGES CXX)
add_library(protobuf::libprotobuf INTERFACE IMPORTED)
set_property(TARGET protobuf::libprotobuf PROPERTY INTERFACE_LINK_LIBRARIES poison_host_library)
add_library(gRPC::grpc++ INTERFACE IMPORTED)
find_package(DatasystemPrivate CONFIG REQUIRED PATHS "{relocated}/lib/cmake/DatasystemPrivate" NO_DEFAULT_PATH)
add_library(bridge SHARED bridge.cpp)
target_link_libraries(bridge PRIVATE DatasystemPrivate::client)
if(UNIX AND NOT APPLE)
    target_link_options(bridge PRIVATE -Wl,--no-undefined)
endif()
""")
            consumer_build = root / "consumer-build"
            command("cmake", "-S", str(consumer), "-B", str(consumer_build))
            command("cmake", "--build", str(consumer_build))
            self.assertTrue(list(consumer_build.glob("*bridge*")))

    def test_static_dependency_policy(self):
        with tempfile.TemporaryDirectory(prefix="ds-private-policy-") as directory:
            script = pathlib.Path(directory) / "policy.cmake"
            script.write_text(f"""cmake_minimum_required(VERSION 3.20)
set(DS_SOURCE_DIR \"{REPO}\")
include(\"{REPO}/cmake/private-sdk-dependency-options.cmake\")
set(ARG_CONF_OPTIONS -DBUILD_SHARED_LIBS:BOOL=ON)
set(ARG_CXX_FLAGS \"\")
set(ARG_C_FLAGS \"\")
ds_private_sdk_dependency_options(gRPC)
if(NOT \"${{ARG_CONF_OPTIONS}}\" MATCHES \"BUILD_SHARED_LIBS:BOOL=OFF\" OR NOT ARG_C_FLAGS MATCHES \"fPIC\")
    message(FATAL_ERROR \"Static/PIC policy missing\")
endif()
set(ARG_TOOLCHAIN configure)
set(ARG_CONF_OPTIONS shared enable-ssl3)
ds_private_sdk_dependency_options(OpenSSL)
if(\"shared\" IN_LIST ARG_CONF_OPTIONS OR NOT \"no-shared\" IN_LIST ARG_CONF_OPTIONS)
    message(FATAL_ERROR \"OpenSSL policy did not disable shared linkage\")
endif()
""")
            command("cmake", "-P", str(script))

    def test_shared_dependency_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="ds-private-reject-") as directory:
            root = pathlib.Path(directory)
            (root / "lib.cpp").write_text("int fixture() { return 0; }\n")
            (root / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.20)
project(Reject LANGUAGES CXX)
set(DS_SOURCE_DIR "{REPO}")
set(DS_BINARY_DIR "${{CMAKE_BINARY_DIR}}")
set(DATASYSTEM_VERSION 0.9.3)
add_library(shared SHARED lib.cpp)
add_library(client STATIC lib.cpp)
target_link_libraries(client PRIVATE shared)
include("{REPO}/cmake/private-sdk-package.cmake")
ds_install_private_sdk(client)
""")
            result = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cannot contain shared target", result.stdout)

    def test_unmanaged_library_search_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="ds-private-search-") as directory:
            script = pathlib.Path(directory) / "reject.cmake"
            for dependency in ("-lprotobuf", "-Wl,-lssl", "$<$<BOOL:1>:-lgrpc++>",
                               "$<$<BOOL:1>:/outside/libprotobuf.a>"):
                with self.subTest(dependency=dependency):
                    script.write_text(f"""cmake_minimum_required(VERSION 3.20)
include("{REPO}/cmake/private-sdk-package.cmake")
ds_private_sdk_link("{dependency}" output)
""")
                    result = subprocess.run(["cmake", "-P", str(script)], text=True,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertRegex(result.stdout, "unmanaged library|Non-relocatable")


if __name__ == "__main__":
    unittest.main()
