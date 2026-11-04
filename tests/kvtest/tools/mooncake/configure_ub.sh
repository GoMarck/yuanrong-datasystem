#!/usr/bin/env bash
# Shared configuration, sourced by the TE and Store build entry points.
set -euo pipefail
base=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
profile=$1
shift
[[ "$profile" == te || "$profile" == store ]] || { echo "Invalid build profile: $profile" >&2; exit 2; }
source "$base/proxy_env.sh"
source "$base/build_log.sh"
source "$base/source_profile.sh"
if (( $# < 3 || $# > 4 )); then
    echo "Usage: bash $(basename "$0") /path/to/Mooncake /dir/containing/urma_api.h /path/to/liburma.so [jobs]" >&2
    exit 2
fi
src=$1
urma_include=$2
urma_lib=$3
jobs=${4:-8}
use_ub=${MOONCAKE_USE_UB:-ON}
use_tent=${MOONCAKE_USE_TENT:-OFF}
event_completion=${MOONCAKE_USE_EVENT_DRIVEN_COMPLETION:-OFF}
[[ "$use_ub" == ON || "$use_ub" == OFF ]] || {
    echo 'MOONCAKE_USE_UB must be ON or OFF' >&2; exit 2;
}
[[ "$use_tent" == ON || "$use_tent" == OFF ]] || {
    echo 'MOONCAKE_USE_TENT must be ON or OFF' >&2; exit 2;
}
[[ "$event_completion" == ON || "$event_completion" == OFF ]] || {
    echo 'MOONCAKE_USE_EVENT_DRIVEN_COMPLETION must be ON or OFF' >&2; exit 2;
}
expected=$mooncake_source_revision
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'jobs must be positive' >&2; exit 2; }
missing=0
if [[ ! -f "$src/CMakeLists.txt" ]]; then
    echo "Missing Mooncake source: $src/CMakeLists.txt" >&2
    missing=1
fi
if [[ "$use_ub" == ON && ! -f "$urma_include/urma_api.h" ]]; then
    echo "Missing URMA header: $urma_include/urma_api.h" >&2
    missing=1
fi
if [[ "$use_ub" == ON && ! -f "$urma_lib" ]]; then
    echo "Missing URMA library: $urma_lib" >&2
    missing=1
fi
if (( missing )); then
    echo 'Locate the installed files, then pass their actual paths to build_store_ub.sh.' >&2
    exit 2
fi
src=$(realpath "$src")
urma_args=()
if [[ "$use_ub" == ON ]]; then
    urma_include=$(realpath "$urma_include")
    urma_lib=$(realpath "$urma_lib")
    urma_args+=("-DURMA_SYSTEM_INCLUDE_DIR=$urma_include" "-DURMA_LIBRARY=$urma_lib")
fi
actual=$(git -C "$src" rev-parse HEAD)
[[ "$actual" == "$expected" ]] || {
    echo "Expected source commit $expected; found $actual. No checkout/reset performed." >&2; exit 2;
}
if [[ -n "$(git -C "$src" status --porcelain --untracked-files=no)" ]]; then
    echo 'Tracked source changes found; use a clean dedicated checkout for reproducibility.' >&2
    exit 2
fi
submodule_status=$(git -C "$src" submodule status --recursive)
if printf '%s\n' "$submodule_status" | grep -Eq '^[-+U]'; then
    echo 'Submodules are not at the commits recorded by Mooncake:' >&2
    printf '%s\n' "$submodule_status" | grep -E '^[-+U]' >&2
    echo '  - = not initialized; + = checkout differs from recorded commit; U = conflict.' >&2
    echo "After checking for local submodule changes, run: git -C '$src' submodule update --init --recursive" >&2
    exit 2
fi
if [[ -z "${glog_DIR:-}" ]]; then
    glog_config=$(find "$base/.deps/glog-install" -type f \( -name 'glog-config.cmake' -o -name 'glogConfig.cmake' \) -print -quit 2>/dev/null || true)
    [[ -z "$glog_config" ]] || export glog_DIR="$(dirname "$glog_config")"
fi
if [[ -z "${gflags_DIR:-}" ]]; then
    gflags_config=$(find "$base/.deps/gflags-install" -type f -name 'gflags-config.cmake' -print -quit 2>/dev/null || true)
    [[ -z "$gflags_config" ]] || export gflags_DIR="$(dirname "$gflags_config")"
fi
for dep in glog-install gflags-install; do
    for libdir in lib64 lib; do
        candidate="$base/.deps/$dep/$libdir"
        if [[ -d "$candidate" && ":${LD_LIBRARY_PATH:-}:" != *":$candidate:"* ]]; then
            export LD_LIBRARY_PATH="$candidate${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        fi
    done
done
dependency_args=()
if [[ -n "${GLOG_INCLUDE_DIR:-}" ]]; then
    [[ -f "$GLOG_INCLUDE_DIR/glog/logging.h" ]] || {
        echo "Missing glog header: $GLOG_INCLUDE_DIR/glog/logging.h" >&2; exit 2;
    }
    dependency_args+=("-DGLOG_INCLUDE_DIR:PATH=$GLOG_INCLUDE_DIR")
fi
if [[ -n "${GLOG_LIBRARY:-}" ]]; then
    [[ -f "$GLOG_LIBRARY" ]] || {
        echo "Missing glog library: $GLOG_LIBRARY" >&2; exit 2;
    }
    dependency_args+=("-DGLOG_LIBRARY:FILEPATH=$GLOG_LIBRARY")
fi
if [[ -n "${glog_DIR:-}" ]]; then
    [[ -f "$glog_DIR/glog-config.cmake" || -f "$glog_DIR/glogConfig.cmake" ]] || {
        echo "No glog CMake config in: $glog_DIR" >&2; exit 2;
    }
    dependency_args+=("-Dglog_DIR:PATH=$glog_DIR")
fi
if [[ -n "${gflags_DIR:-}" ]]; then
    [[ -f "$gflags_DIR/gflags-config.cmake" || -f "$gflags_DIR/gflagsConfig.cmake" ]] || {
        echo "No gflags CMake config in: $gflags_DIR" >&2; exit 2;
    }
    gflags_prefix=$(realpath "$gflags_DIR/../../..")
    [[ -f "$gflags_prefix/include/gflags/gflags.h" ]] || {
        echo "No gflags header in: $gflags_prefix/include/gflags/gflags.h" >&2; exit 2;
    }
    # rdma_twosided includes rdma_context.h (which includes gflags.h), but its
    # CMake target does not link gflags::gflags, so no include path propagates.
    export CPLUS_INCLUDE_PATH="$gflags_prefix/include${CPLUS_INCLUDE_PATH:+:$CPLUS_INCLUDE_PATH}"
    dependency_args+=("-Dgflags_DIR:PATH=$gflags_DIR")
fi
# Always set launcher values, including empty ones, to clear an older cache setting.
launcher=""
case "${MOONCAKE_CCACHE:-auto}" in
    0) ;;
    auto|1)
        launcher=$(command -v ccache || true)
        if [[ -z "$launcher" ]]; then
            if [[ "${MOONCAKE_CCACHE:-auto}" == 1 ]]; then
                echo 'MOONCAKE_CCACHE=1 requires ccache in PATH' >&2
                exit 2
            fi
            echo 'ccache unavailable; compiling without compiler cache' >&2
        fi
        ;;
    *) echo 'MOONCAKE_CCACHE must be auto, 0 or 1' >&2; exit 2 ;;
esac
export CCACHE_COMPILERCHECK=content
with_store=OFF
examples=ON
configure_log=configure.log
profile_args=()
if [[ "$profile" == store ]]; then
    with_store=ON
    examples=OFF
    configure_log=store-configure.log
    profile_args+=("-DCMAKE_PROJECT_mooncake_INCLUDE:FILEPATH=$base/store_build_compat.cmake"
                   "-DPYTHON_EXECUTABLE:FILEPATH=$(command -v python3)"
                   "-DPython3_EXECUTABLE:FILEPATH=$(command -v python3)")
else
    profile_args+=("-U" "CMAKE_PROJECT_mooncake_INCLUDE" "-DSKBUILD:BOOL=OFF")
fi
mkdir -p "$src/build-ub"
cmake_args=( \
    -DUSE_UB:BOOL="$use_ub" -DUSE_TENT:BOOL="$use_tent" -DUSE_CUDA:BOOL=OFF \
    -DUSE_EVENT_DRIVEN_COMPLETION:BOOL="$event_completion" \
    -DUSE_ASCEND:BOOL=OFF -DUSE_ASCEND_DIRECT:BOOL=OFF -DUSE_UBSHMEM:BOOL=OFF \
    -DWITH_TE:BOOL=ON -DWITH_STORE:BOOL="$with_store" -DWITH_STORE_RUST:BOOL=OFF \
    -DBUILD_UNIT_TESTS:BOOL=OFF -DBUILD_EXAMPLES:BOOL="$examples" -DBUILD_BENCHMARK:BOOL="$examples" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER:STRING="$launcher" \
    -DCMAKE_CXX_COMPILER_LAUNCHER:STRING="$launcher" \
    "${urma_args[@]}" "${dependency_args[@]}" "${profile_args[@]}" )
# Store lets the upstream PEP 517 backend own configure/build/install.
if [[ "$profile" == te ]]; then
    run_logged_command "$src/build-ub/$configure_log" \
        cmake -S "$src" -B "$src/build-ub" "${cmake_args[@]}"
fi
