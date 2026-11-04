#!/usr/bin/env bash
# Install the header-only MessagePack C++ dependency without distro RPM packages.
set -euo pipefail
base=$(cd "$(dirname "$0")" && pwd -P)
source "$base/proxy_env.sh"
source_dir="$base/.deps/msgpack-source"
build_dir="$base/.deps/msgpack-build"
prefix=${1:-$base/.deps/msgpack-install}
revision=8c602e8579c7e7d65d6f9c6703c9699db3fb0488 # cpp-6.1.0
mkdir -p "$prefix"
prefix=$(realpath "$prefix")
if [[ ! -d "$source_dir/.git" ]]; then
    mkdir -p "$(dirname "$source_dir")"
    git clone --branch cpp-6.1.0 --depth 1 https://github.com/msgpack/msgpack-c.git "$source_dir"
fi
[[ "$(git -C "$source_dir" rev-parse HEAD)" == "$revision" ]] || {
    echo "Expected MessagePack cpp-6.1.0 commit $revision" >&2; exit 2;
}
[[ -z "$(git -C "$source_dir" status --porcelain --untracked-files=no)" ]] || {
    echo 'MessagePack tracked source changes found; resolve before building.' >&2; exit 2;
}
# No library binary is needed. Disable upstream tests/examples/docs and their dependencies.
cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DMSGPACK_CXX20=ON \
    -DMSGPACK_USE_BOOST=OFF -DMSGPACK_BUILD_TESTS=OFF \
    -DMSGPACK_BUILD_EXAMPLES=OFF -DMSGPACK_BUILD_DOCS=OFF
cmake --install "$build_dir"
[[ -f "$prefix/include/msgpack.hpp" ]] || {
    echo "MessagePack C++ header missing under $prefix" >&2; exit 2;
}
echo "MessagePack C++ headers installed under: $prefix/include"
