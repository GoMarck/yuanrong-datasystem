#!/usr/bin/env bash
# Build glog v0.7.1 locally when no distro development package is available.
set -euo pipefail
base=$(cd "$(dirname "$0")" && pwd -P)
source "$base/proxy_env.sh"
source_dir="$base/.deps/glog-source"
prefix=${1:-$base/.deps/glog-install}
jobs=${2:-8}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'jobs must be positive' >&2; exit 2; }
mkdir -p "$prefix"
prefix=$(realpath "$prefix")
if [[ ! -d "$source_dir/.git" ]]; then
    mkdir -p "$(dirname "$source_dir")"
    git clone --branch v0.7.1 --depth 1 https://github.com/google/glog.git "$source_dir"
fi
tag=$(git -C "$source_dir" describe --tags --exact-match 2>/dev/null || true)
[[ "$tag" == v0.7.1 ]] || {
    echo "Expected clean glog v0.7.1 source, found tag: ${tag:-none}" >&2; exit 2;
}
[[ -z "$(git -C "$source_dir" status --porcelain --untracked-files=no)" ]] || {
    echo 'glog tracked source changes found; resolve before building.' >&2; exit 2;
}
cmake -S "$source_dir" -B "$source_dir/build" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF \
    -DWITH_GTEST=OFF -DWITH_GFLAGS=OFF -DWITH_UNWIND=none
cmake --build "$source_dir/build" -j "$jobs"
cmake --install "$source_dir/build"
config=$(find "$prefix" -type f \( -name 'glog-config.cmake' -o -name 'glogConfig.cmake' \) -print -quit)
[[ -n "$config" ]] || { echo "glog CMake config missing under $prefix" >&2; exit 2; }
echo "glog installed under: $prefix"
echo "For Mooncake build: export glog_DIR=$(dirname "$config")"
echo "For runtime: export LD_LIBRARY_PATH=$prefix/lib64:$prefix/lib:\${LD_LIBRARY_PATH:-}"
