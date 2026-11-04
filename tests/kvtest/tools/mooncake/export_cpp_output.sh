#!/usr/bin/env bash
# Copy only the prebuilt C++ inputs KVTest needs. The Python copier verifies
# existing files so an interrupted multi-GiB copy can safely resume.
set -euo pipefail

base=$(cd "$(dirname "$0")/../.." && pwd -P)
image=${1:-mooncake-store-kvtest:tcp-rdma}
output=${2:-$base/mooncake-output}
script=$base/tools/mooncake/export_cpp_output.py

command -v nerdctl >/dev/null 2>&1 || { echo 'nerdctl is required' >&2; exit 2; }
nerdctl -n k8s.io image inspect "$image" >/dev/null
mkdir -p "$output"
output=$(cd "$output" && pwd -P)

nerdctl -n k8s.io run --rm --net=none --user 0 \
    -v "$output:/kvtest-mooncake-output" \
    -v "$script:/export_cpp_output.py:ro" \
    --entrypoint python3 "$image" -u /export_cpp_output.py \
    /opt /kvtest-mooncake-output "$image"

echo "Exported prebuilt Mooncake C++ output: $output"
