#!/usr/bin/env bash
# Download for the same Python/architecture as the build, without changing the base image.
set -euo pipefail
base=$(cd "$(dirname "$0")" && pwd -P)
source "$base/build_log.sh"
if (( $# > 2 )); then
    echo 'Usage: bash download_store_wheels.sh [BASE_IMAGE [WHEELHOUSE]]' >&2
    exit 2
fi
base_image=${1:-docker.io/library/kvcache_base:4d7a51beffc9}
wheelhouse=${2:-$base/wheelhouse}
mkdir -p "$wheelhouse"
wheelhouse=$(cd "$wheelhouse" && pwd -P)
if [[ -n "${CONTAINER_CLI:-}" ]]; then
    cli=$CONTAINER_CLI
elif command -v nerdctl >/dev/null 2>&1; then
    cli=nerdctl
elif command -v docker >/dev/null 2>&1; then
    cli=docker
else
    echo 'Need nerdctl or docker to download wheels in the base image' >&2
    exit 2
fi
args=()
[[ "$(basename "$cli")" != nerdctl ]] || args=(-n k8s.io)
log=${MOONCAKE_DOWNLOAD_LOG:-$base/store-wheel-download-$(date +%Y%m%d-%H%M%S)-$$.log}
run_logged_command "$log" "$cli" "${args[@]}" run --rm --net=host \
    -v "$base:/opt/bench:ro" -v "$wheelhouse:/wheelhouse" \
    -e "MOONCAKE_PROXY_URL=${MOONCAKE_PROXY_URL-http://141.61.25.219:3128}" \
    -e "MOONCAKE_PIP_INDEX_URL=${MOONCAKE_PIP_INDEX_URL:-https://pypi.tuna.tsinghua.edu.cn/simple}" \
    -e "MOONCAKE_PIP_TIMEOUT=${MOONCAKE_PIP_TIMEOUT:-600}" \
    -e "MOONCAKE_PIP_RETRIES=${MOONCAKE_PIP_RETRIES:-5}" \
    --entrypoint bash "$base_image" -ec '
        export MOONCAKE_PIP_OFFLINE=0 PIP_NO_INDEX=0
        source /opt/bench/proxy_env.sh
        source /opt/bench/pip_env.sh
        python3 -m pip download --only-binary=:all: --progress-bar off \
            --find-links /wheelhouse --dest /wheelhouse \
            -r /opt/bench/store-wheel-requirements.txt
    '
echo "Wheels downloaded: $wheelhouse"
echo 'Copy these wheels to the build scripts wheelhouse/ if using another machine or directory.'
echo 'Build with: MOONCAKE_PIP_OFFLINE=1 bash build_kvtest_store_image.sh'
