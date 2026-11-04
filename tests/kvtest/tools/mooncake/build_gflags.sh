#!/usr/bin/env bash
# Build gflags v2.2.2 locally when distro development headers are unavailable.
set -euo pipefail
base=$(cd "$(dirname "$0")" && pwd -P)
source "$base/proxy_env.sh"
source_dir="$base/.deps/gflags-source"
build_dir="$base/.deps/gflags-build"
prefix=${1:-$base/.deps/gflags-install}
jobs=${2:-8}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'jobs must be positive' >&2; exit 2; }
mkdir -p "$prefix"
prefix=$(realpath "$prefix")
if [[ ! -d "$source_dir/.git" ]]; then
    mkdir -p "$(dirname "$source_dir")"
    git clone --branch v2.2.2 --depth 1 https://github.com/gflags/gflags.git "$source_dir"
fi
tag=$(git -C "$source_dir" describe --tags --exact-match 2>/dev/null || true)
[[ "$tag" == v2.2.2 ]] || {
    echo "Expected clean gflags v2.2.2 source, found tag: ${tag:-none}" >&2; exit 2;
}
[[ -z "$(git -C "$source_dir" status --porcelain --untracked-files=no)" ]] || {
    echo 'gflags tracked source changes found; resolve before building.' >&2; exit 2;
}
cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DBUILD_TESTING=OFF
cmake --build "$build_dir" -j "$jobs"
cmake --install "$build_dir"
config=$(find "$prefix" -type f -name 'gflags-config.cmake' -print -quit)
[[ -n "$config" ]] || { echo "gflags CMake config missing under $prefix" >&2; exit 2; }
[[ -f "$prefix/include/gflags/gflags.h" ]] || {
    echo "gflags header missing under $prefix" >&2; exit 2;
}
echo "gflags installed under: $prefix"
echo "For Mooncake build: export gflags_DIR=$(dirname "$config")"
echo "For runtime: export LD_LIBRARY_PATH=$prefix/lib64:$prefix/lib:\${LD_LIBRARY_PATH:-}"
