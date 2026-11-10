cmake_minimum_required(VERSION 3.14)
project(kvtest C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Use -std=c++17 (not gnu++17) to avoid GNU extension conflicts with the SDK's
# spdlog/logging initialization in ServiceDiscovery::Init().
set(CMAKE_CXX_EXTENSIONS OFF)

set(DATASYSTEM_SDK_DIR "" CACHE PATH "Path to datasystem SDK installation")
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to yuanrong-datasystem repo root (for third-party cmake + patches)")

# Read version from VERSION file
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" BUILD_VERSION LIMIT_COUNT 1)
add_definitions(-DBUILD_VERSION="${BUILD_VERSION}")

# Embed git commit hash at compile time
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE BUILD_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT BUILD_COMMIT)
    set(BUILD_COMMIT "unknown")
endif()
add_definitions(-DBUILD_COMMIT="${BUILD_COMMIT}")

# ---------------------------------------------------------------------------
# Backend selection: brpc+bthread (default, ON) or httplib+std::thread (OFF).
# When ON, the main repo's third-party cmake scripts (cmake/external_libs/*.cmake)
# are reused to download/build brpc+protobuf+gflags+absl+leveldb+openssl+zlib
# into $DS_OPENSOURCE_DIR (cached; first build ~5-10min, subsequent seconds).
# kvtest then links libbrpc.so/libprotobuf.so/etc directly — no SDK packaging
# changes needed, no SDK symbol-export changes. The user need NOT pre-install
# any third-party dev packages; only the pre-installed datasystem SDK (for
# libdatasystem.so + datasystem headers) is required.
# ---------------------------------------------------------------------------
option(KVTEST_ENABLE_MOONCAKE "Link the prebuilt Mooncake C++ SDK" OFF)
set(MOONCAKE_OUTPUT_DIR "" CACHE PATH "Mooncake SDK export directory")

option(KVTEST_USE_BRPC "Use brpc control plane + bthread workers (ON) or httplib + std::thread (OFF)" ON)

# Common sources (compiled in both backends).
set(KVTEST_COMMON_SOURCES
    src/main.cpp
    src/common/config.cpp
    src/common/jemalloc_prof.cpp
    src/metrics/metrics.cpp
    src/pipeline/pipeline.cpp
    src/pipeline/cuda_workload.cpp
    src/pipeline/kv_worker.cpp
    src/pipeline/cache_reader.cpp
    src/pipeline/stop.cpp
    src/rpc/notify_dispatcher.cpp
    src/rpc/peer_client.cpp
    src/benchmark/interface_benchmark.cpp
    src/benchmark/benchmark_runner.cpp)

if(KVTEST_USE_BRPC)
    # Build all third-party libs (brpc/protobuf/gflags/absl/openssl/zlib) as
    # STATIC archives and link them into the kvtest binary. This avoids vtable
    # interposition: libdatasystem.so exports _ZTV* (all C++ vtables via
    # export_datasystem_symbols.lds); if kvtest dynamically links libprotobuf.so,
    # the dynamic linker resolves google::protobuf::* vtable symbols to
    # libdatasystem.so's internal protobuf → crash. With static linking, vtable
    # references are direct (link-time resolved, no GOT) → no interposition.
    # This mirrors bazel mode (self-contained binary, static brpc/protobuf).
    set(KVTEST_BUILD_STATIC ON)

    # The main repo's external_libs/*.cmake reference ${CMAKE_SOURCE_DIR}/third_party/patches/
    # to locate patch files. When kvtest is the top-level project, CMAKE_SOURCE_DIR is
    # tests/kvtest/, not the repo root — so save/restore CMAKE_SOURCE_DIR around the
    # includes so the included .cmake files see the repo root for patch resolution.
    set(_KVTEST_SAVED_CMAKE_SOURCE_DIR ${CMAKE_SOURCE_DIR})
    set(CMAKE_SOURCE_DIR ${REPO_ROOT})

    # Include order mirrors cmake/dependency.cmake's relevant subset (brpc depends on
    # gflags/leveldb/protobuf/openssl/zlib/absl; protobuf depends on absl/zlib).
    include(${REPO_ROOT}/cmake/util.cmake)
    include(${REPO_ROOT}/cmake/external_libs/absl.cmake)
    include(${REPO_ROOT}/cmake/external_libs/zlib.cmake)
    include(${REPO_ROOT}/cmake/external_libs/openssl.cmake)
    include(${REPO_ROOT}/cmake/external_libs/leveldb.cmake)
    include(${REPO_ROOT}/cmake/external_libs/gflags.cmake)
    # Use protobuf 28.3 (matching bazel-built SDK's internal protobuf version).
    set(protobuf_VERSION 28.3)
    include(${REPO_ROOT}/cmake/external_libs/protobuf.cmake)
    include(${REPO_ROOT}/cmake/external_libs/brpc.cmake)

    set(CMAKE_SOURCE_DIR ${_KVTEST_SAVED_CMAKE_SOURCE_DIR})

    # Generate kvtest_control.pb.cc/.h from the proto using the project's protoc.
    # protoc is dynamically linked against libprotoc.so.<ver> which lives in
    # ${Protobuf_ROOT}/lib[64]; inject LD_LIBRARY_PATH via `cmake -E env`.
    if (EXISTS ${Protobuf_ROOT}/lib64)
        set(_KVTEST_PROTOC_LIB_DIR ${Protobuf_ROOT}/lib64)
    else()
        set(_KVTEST_PROTOC_LIB_DIR ${Protobuf_ROOT}/lib)
    endif()
    set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/kvtest_control.proto)
    set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)
    file(MAKE_DIRECTORY ${GEN_DIR})
    set(PB_CC ${GEN_DIR}/kvtest_control.pb.cc)
    set(PB_H  ${GEN_DIR}/kvtest_control.pb.h)
    add_custom_command(
        OUTPUT ${PB_CC} ${PB_H}
        COMMAND ${CMAKE_COMMAND} -E env "LD_LIBRARY_PATH=${_KVTEST_PROTOC_LIB_DIR}:$ENV{LD_LIBRARY_PATH}"
                ${Protobuf_ROOT}/bin/protoc
                --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
                --cpp_out=${GEN_DIR}
                ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}
        COMMENT "Generating kvtest_control.pb.cc/.h via main repo's protoc")

    add_executable(kvtest
        ${KVTEST_COMMON_SOURCES}
        src/rpc/brpc_server.cpp
        ${PB_CC})

    # Force brpc_server.cpp.o and peer_client.cpp.o to wait for the proto
    # header to be generated (CMake's Make generator doesn't know about the
    # generated header dependency on first build).
    set_source_files_properties(src/rpc/brpc_server.cpp src/rpc/peer_client.cpp PROPERTIES
        OBJECT_DEPENDS "${PB_H}")

    target_include_directories(kvtest PRIVATE
        ${DATASYSTEM_SDK_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${GEN_DIR})

    target_compile_definitions(kvtest PRIVATE KVTEST_USE_BRPC)

    # Statically link all third-party archives + dynamically link libdatasystem.so.
    # BRPC_LIBRARIES carries libbrpc.a + libgflags.a + libleveldb.a + protobuf::libprotobuf
    # + Threads. OpenSSL::SSL/Crypto and ZLIB::ZLIB are cmake imported targets that
    # handle transitive deps. -Wl,--whole-archive for absl ensures all small .a
    # components are pulled in (absl has many fine-grained static libs; without
    # whole-archive, the linker may miss template/inline-defined symbols).
    target_link_libraries(kvtest PRIVATE
        ${DATASYSTEM_SDK_DIR}/lib/libdatasystem.so
        -Wl,--whole-archive
        ${BRPC_LIBRARY}
        $<$<NOT:$<BOOL:${KVTEST_ENABLE_MOONCAKE}>>:${gflags_LIBRARY}>
        ${leveldb_LIBRARY}
        -Wl,--no-whole-archive
        protobuf::libprotobuf
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        Threads::Threads
        pthread
        dl
        rt)

    # Only libdatasystem.so is dynamic — BUILD_RPATH points at the SDK lib dir;
    # INSTALL_RPATH uses $ORIGIN/lib for portable deployment (patchelf in
    # `make package` rewrites the binary's RPATH at packaging time).
    set_target_properties(kvtest PROPERTIES
        BUILD_RPATH "${DATASYSTEM_SDK_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/lib")
else()
    # httplib backend — header-only, no third-party deps; std::thread workers
    # (KVTEST_USE_BRPC undefined → kvtest::thread == std::thread, httplib path
    # in main.cpp/peer_client.cpp is selected at compile time).
    add_executable(kvtest
        ${KVTEST_COMMON_SOURCES}
        src/rpc/http_server.cpp)

    target_include_directories(kvtest PRIVATE
        ${DATASYSTEM_SDK_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

    set_target_properties(kvtest PROPERTIES
        BUILD_RPATH "${DATASYSTEM_SDK_DIR}/lib"
        INSTALL_RPATH "${DATASYSTEM_SDK_DIR}/lib")

    target_link_libraries(kvtest PRIVATE
        ${DATASYSTEM_SDK_DIR}/lib/libdatasystem.so
        pthread ${CMAKE_DL_LIBS})
endif()

# Optional NUMA support — link libnuma if available (header detection by __has_include)
find_library(LIBNUMA_LIB numa)
if(LIBNUMA_LIB)
    target_compile_definitions(kvtest PRIVATE HAS_LIBNUMA=1)
    target_link_libraries(kvtest PRIVATE ${LIBNUMA_LIB})
    message(STATUS "NUMA support enabled: ${LIBNUMA_LIB}")
else()
    message(STATUS "NUMA not found — numa_node config will be ignored")
endif()

# Statically link libstdc++ and libgcc so the binary runs on systems with
# older GCC (e.g. openEuler 22.04 with GCC 10 / GLIBCXX_3.4.29).
if(NOT KVTEST_ENABLE_MOONCAKE)
    target_link_options(kvtest PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)
else()
    target_link_options(kvtest PRIVATE -Wl,--exclude-libs,ALL)
endif()

# ---------------------------------------------------------------------------
# coordinator_test: standalone Coordinator deployment test program
# Links against libdatasystem_coordinator.so from the SDK.
# ---------------------------------------------------------------------------
add_executable(coordinator_test src/coordinator_main.cpp src/common/jemalloc_prof.cpp)
target_include_directories(coordinator_test PRIVATE
    ${DATASYSTEM_SDK_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(coordinator_test PRIVATE
    ${DATASYSTEM_SDK_DIR}/lib/libdatasystem_coordinator.so
    pthread)
set_target_properties(coordinator_test PROPERTIES
    BUILD_RPATH "${DATASYSTEM_SDK_DIR}/lib"
    INSTALL_RPATH "$ORIGIN/lib")
target_link_options(coordinator_test PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)

# ---------------------------------------------------------------------------
# worker_test: standalone Worker deployment test program
# Links against libdatasystem_worker.so from the SDK.
# ---------------------------------------------------------------------------
add_executable(worker_test src/data_worker_main.cpp src/common/jemalloc_prof.cpp)
target_include_directories(worker_test PRIVATE
    ${DATASYSTEM_SDK_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(worker_test PRIVATE DATASYSTEM_VERSION="${BUILD_VERSION}")
target_link_libraries(worker_test PRIVATE
    ${DATASYSTEM_SDK_DIR}/lib/libdatasystem_worker.so
    pthread)
set_target_properties(worker_test PROPERTIES
    BUILD_RPATH "${DATASYSTEM_SDK_DIR}/lib"
    INSTALL_RPATH "$ORIGIN/lib")
target_link_options(worker_test PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)

# Mooncake is compiled separately by the user. Opting in links its prebuilt
# C++ SDK into the same kvtest executable and enables runtime backend choice.
if(KVTEST_ENABLE_MOONCAKE)
    target_sources(kvtest PRIVATE src/pipeline/mooncake_client.cpp src/pipeline/mooncake_pipeline.cpp)
    if(NOT MOONCAKE_OUTPUT_DIR)
        message(FATAL_ERROR "KVTEST_ENABLE_MOONCAKE requires MOONCAKE_OUTPUT_DIR")
    endif()
    set(MC_ROOT "${MOONCAKE_OUTPUT_DIR}/Mooncake")
    set(MC_BUILD "${MC_ROOT}/build-ub")
    set(MC_DEPS "${MOONCAKE_OUTPUT_DIR}/mooncake-bench/.deps")
    foreach(required
            "${MC_ROOT}/mooncake-store/include/real_client.h"
            "${MC_BUILD}/mooncake-store/src/libmooncake_store.a"
            "${MC_BUILD}/mooncake-transfer-engine/src/libtransfer_engine.a"
            "${MC_BUILD}/mooncake-transfer-engine/src/common/base/libbase.a"
            "${MC_BUILD}/mooncake-store/src/local_ssd/libmooncake_local_ssd.a"
            "${MC_BUILD}/mooncake-store/src/cachelib_memory_allocator/libcachelib_memory_allocator.a"
            "${MC_BUILD}/mooncake-common/src/libmooncake_common.a"
            "${MC_BUILD}/mooncake-common/libasio.so"
            "${MC_DEPS}/msgpack-install/include/msgpack.hpp"
            "${DATASYSTEM_SDK_DIR}/lib/libdatasystem.so")
        if(NOT EXISTS "${required}")
            message(FATAL_ERROR "Mooncake KVTest build input is missing: ${required}")
        endif()
    endforeach()

    unset(MC_GFLAGS CACHE)
    unset(MC_GFLAGS)
    unset(MC_GLOG CACHE)
    unset(MC_GLOG)
    find_library(MC_GFLAGS NAMES gflags PATHS "${MC_DEPS}/gflags-install/lib" "${MC_DEPS}/gflags-install/lib64" NO_DEFAULT_PATH)
    find_library(MC_GLOG NAMES glog PATHS "${MC_DEPS}/glog-install/lib64" "${MC_DEPS}/glog-install/lib" NO_DEFAULT_PATH)
    if(NOT MC_GFLAGS OR NOT MC_GLOG)
        message(FATAL_ERROR "Mooncake output must include the built gflags and glog libraries")
    endif()
    foreach(lib ibverbs curl rt crypto zstd jsoncpp yaml-cpp xxhash numa)
        string(REPLACE "-" "_" var "MC_SYSTEM_${lib}")
        find_library(${var} NAMES ${lib})
        if(NOT ${var})
            message(FATAL_ERROR "Mooncake KVTest build requires system library: ${lib}")
        endif()
    endforeach()
    # The prebuilt Store archive references io_uring even for in-memory workloads.
    find_library(MC_SYSTEM_uring NAMES uring)
    if(NOT MC_SYSTEM_uring)
        message(FATAL_ERROR "Mooncake Store requires liburing: install liburing-devel (or liburing-dev), then rebuild KVTest")
    endif()
    find_package(Threads REQUIRED)

    target_compile_features(kvtest PRIVATE cxx_std_20)
    target_compile_definitions(kvtest PRIVATE KVTEST_WITH_MOONCAKE
        ASIO_DYN_LINK ASIO_SEPARATE_COMPILATION CONFIG_ERDMA GFLAGS_IS_A_DLL=0
        GLOG_USE_GLOG_EXPORT HAVE_OSS_ADAPTER HAVE_S3_ADAPTER
        MOONCAKE_GLOG_HAS_IS_INITIALIZED=1 USE_HTTP USE_TCP WITH_METRICS
        YLT_ENABLE_DL YLT_ENABLE_IBV)
    set(MC_COMPLETION_MODE "not recorded")
    if(EXISTS "${MC_BUILD}/CMakeCache.txt")
        file(STRINGS "${MC_BUILD}/CMakeCache.txt" MC_COMPLETION_ENTRY
            REGEX "^USE_EVENT_DRIVEN_COMPLETION:BOOL=")
        if(MC_COMPLETION_ENTRY)
            string(REPLACE "USE_EVENT_DRIVEN_COMPLETION:BOOL=" ""
                MC_COMPLETION_MODE "${MC_COMPLETION_ENTRY}")
            if(MC_COMPLETION_MODE STREQUAL "ON")
                target_compile_definitions(kvtest PRIVATE USE_EVENT_DRIVEN_COMPLETION)
            elseif(NOT MC_COMPLETION_MODE STREQUAL "OFF")
                message(FATAL_ERROR "Invalid Mooncake event completion setting: ${MC_COMPLETION_MODE}")
            endif()
        endif()
    endif()
    message(STATUS "Mooncake event-driven completion: ${MC_COMPLETION_MODE} (prebuilt SDK)")
    target_include_directories(kvtest PRIVATE
        "${DATASYSTEM_SDK_DIR}/include" "${CMAKE_SOURCE_DIR}/src"
        "${MC_ROOT}/mooncake-common/etcd"
        "${MC_ROOT}/mooncake-common/k8s-lease"
        "${MC_ROOT}/mooncake-common/include"
        "${MC_ROOT}/mooncake-common/src"
        "${MC_ROOT}/mooncake-transfer-engine/include"
        "${MC_ROOT}/mooncake-store/include/mooncake-store/proto"
        "${MC_ROOT}/mooncake-store/include"
        "${MC_ROOT}/mooncake-store/include/cachelib_memory_allocator"
        "${MC_ROOT}/mooncake-store/include/cachelib_memory_allocator/include"
        "${MC_ROOT}/mooncake-store/include/cachelib_memory_allocator/fake_include"
        "${MC_BUILD}/mooncake-store/include"
        "${MC_BUILD}/_deps/yalantinglibs-src/include/ylt/thirdparty")
    target_include_directories(kvtest SYSTEM PRIVATE
        "${MC_BUILD}/_deps/yalantinglibs-src/include"
        "${MC_BUILD}/_deps/yalantinglibs-src/include/ylt/standalone"
        "${MC_DEPS}/glog-install/include"
        "${MC_DEPS}/msgpack-install/include")
    target_link_libraries(kvtest PRIVATE
        "${MC_BUILD}/mooncake-store/src/libmooncake_store.a"
        "${MC_BUILD}/mooncake-transfer-engine/src/libtransfer_engine.a"
        "${MC_BUILD}/mooncake-transfer-engine/src/common/base/libbase.a"
        "${MC_SYSTEM_ibverbs}" "${MC_SYSTEM_curl}" "${MC_SYSTEM_rt}"
        "${MC_GFLAGS}"
        "${MC_BUILD}/mooncake-store/src/local_ssd/libmooncake_local_ssd.a"
        "${MC_BUILD}/mooncake-store/src/cachelib_memory_allocator/libcachelib_memory_allocator.a"
        "${MC_GLOG}" "${MC_SYSTEM_curl}" "${MC_SYSTEM_crypto}"
        "${MC_BUILD}/mooncake-common/src/libmooncake_common.a"
        "${MC_BUILD}/mooncake-common/libasio.so"
        Threads::Threads "${CMAKE_DL_LIBS}" "${MC_SYSTEM_ibverbs}"
        "${MC_SYSTEM_zstd}" "${MC_SYSTEM_jsoncpp}"
        "${MC_SYSTEM_yaml_cpp}" "${MC_SYSTEM_xxhash}" "${MC_SYSTEM_numa}")
    target_link_libraries(kvtest PRIVATE "${MC_SYSTEM_uring}")
    set_target_properties(kvtest PROPERTIES BUILD_RPATH
        "${DATASYSTEM_SDK_DIR}/lib;${MC_DEPS}/gflags-install/lib;${MC_DEPS}/glog-install/lib64;${MC_BUILD}/mooncake-common;/opt/mooncake-bench/.deps/gflags-install/lib;/opt/mooncake-bench/.deps/glog-install/lib64;/opt/Mooncake/build-ub/mooncake-common")
    message(STATUS "Mooncake C++ KVTest enabled from ${MOONCAKE_OUTPUT_DIR}")
endif()


# Diagnostic executable; not part of default builds or the production benchmark pipeline.
add_executable(kvtest_table_probe EXCLUDE_FROM_ALL src/benchmark/table_probe.cpp src/common/config.cpp)
foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES LINK_OPTIONS BUILD_RPATH)
    get_target_property(probe_${property} kvtest ${property})
    if(probe_${property})
        set_property(TARGET kvtest_table_probe PROPERTY ${property} "${probe_${property}}")
    endif()
endforeach()
if(KVTEST_ENABLE_MOONCAKE)
    target_sources(kvtest_table_probe PRIVATE src/pipeline/mooncake_client.cpp)
    set_property(TARGET kvtest_table_probe PROPERTY CXX_STANDARD 20)
endif()
