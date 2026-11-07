#!/usr/bin/env bash
# Build an independent Mooncake Store benchmark image from a base with URMA.
set -euo pipefail
base=$(cd "$(dirname "$0")" && pwd -P)
source "$base/build_log.sh"
source "$base/source_profile.sh"
if (( $# > 3 )); then
    echo "Usage: bash build_kvtest_store_image.sh [BASE_IMAGE [OUTPUT_TAG [JOBS]]]" >&2
    exit 2
fi
base_image=${1:-docker.io/library/kvcache_base:4d7a51beffc9}
tag=${2:-mooncake-store-kvtest:local}
jobs=${3:-8}
use_ub=${MOONCAKE_USE_UB:-ON}
use_tent=${MOONCAKE_USE_TENT:-OFF}
event_completion=${MOONCAKE_USE_EVENT_DRIVEN_COMPLETION:-OFF}
stage_profile=${MOONCAKE_STAGE_PROFILE:-OFF}
[[ "$stage_profile" == ON || "$stage_profile" == OFF ]] || { echo 'MOONCAKE_STAGE_PROFILE must be ON or OFF' >&2; exit 2; }
repair_only=${MOONCAKE_URMA_REPAIR_ONLY:-0}
[[ "$use_ub" == ON || "$use_ub" == OFF ]] || { echo 'MOONCAKE_USE_UB must be ON or OFF' >&2; exit 2; }
[[ "$use_tent" == ON || "$use_tent" == OFF ]] || { echo 'MOONCAKE_USE_TENT must be ON or OFF' >&2; exit 2; }
[[ "$event_completion" == ON || "$event_completion" == OFF ]] || { echo 'MOONCAKE_USE_EVENT_DRIVEN_COMPLETION must be ON or OFF' >&2; exit 2; }
[[ "$repair_only" == 0 || "$repair_only" == 1 ]] || { echo 'MOONCAKE_URMA_REPAIR_ONLY must be 0 or 1' >&2; exit 2; }
[[ "$repair_only" == 0 || "$use_ub" == ON ]] || { echo 'URMA repair requires MOONCAKE_USE_UB=ON' >&2; exit 2; }
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be positive' >&2; exit 2; }
runtime_work=
cleanup_runtime() {
    if [[ -n "$runtime_work" ]]; then
        rm -rf -- "$runtime_work"
    fi
}
trap cleanup_runtime EXIT

if [[ -n "${CONTAINER_CLI:-}" ]]; then
    cli=$CONTAINER_CLI
elif command -v nerdctl >/dev/null 2>&1; then
    cli=nerdctl
elif command -v docker >/dev/null 2>&1; then
    cli=docker
else
    echo 'Need nerdctl or docker to build the image' >&2
    exit 2
fi

args=()
if [[ "$(basename "$cli")" == nerdctl ]]; then
    args=(-n k8s.io)
    if ! "$cli" "${args[@]}" image inspect "$base_image" >/dev/null 2>&1; then
        echo "Base image is absent from containerd namespace k8s.io: $base_image" >&2
        echo 'List local tags with: nerdctl -n k8s.io images' >&2
        exit 2
    fi
    if [[ "$repair_only" == 0 ]] && ! "$cli" "${args[@]}" run --rm --net=none --entrypoint sh "$base_image" -c '
        if [ "$1" = ON ]; then
            test -f "$2/urma_api.h" && test -f "$3" || exit 1
        fi
        command -v cmake && command -v g++ && command -v python3 && command -v dnf
    ' sh "$use_ub" "${URMA_INCLUDE_DIR:-/usr/include/ub/umdk/urma}" "${URMA_LIBRARY:-/usr/lib64/liburma.so}" >/dev/null; then
        echo "Base image lacks a required URMA file or build tool: $base_image" >&2
        echo 'Required: urma_api.h, liburma.so, cmake, g++, python3, dnf' >&2
        exit 2
    fi
    if [[ -z "${MOONCAKE_BUILDKIT_HOST:-}" ]]; then
        command -v buildkitd >/dev/null 2>&1 || {
            echo 'buildkitd is required to build from a local containerd image' >&2
            exit 2
        }
        buildkit_work=$(mktemp -d "${TMPDIR:-/tmp}/mooncake-buildkit.XXXXXX")
        buildkit_socket="$buildkit_work/buildkitd.sock"
        buildkit_log="$buildkit_work/buildkitd.log"
        buildkit_state=${MOONCAKE_BUILDKIT_ROOT:-${HOME:-/root}/.cache/mooncake-buildkit-k8s}
        mkdir -p "$buildkit_state"
        buildkitd --addr="unix://$buildkit_socket" --root="$buildkit_state" \
            --oci-worker=false --containerd-worker=true \
            --containerd-worker-namespace=k8s.io >"$buildkit_log" 2>&1 &
        buildkit_pid=$!
        cleanup_buildkit() {
            status=$?
            trap - EXIT
            if (( status != 0 )); then
                echo 'BuildKit diagnostic log:' >&2
                tail -n 50 "$buildkit_log" >&2 || true
            fi
            kill "$buildkit_pid" 2>/dev/null || true
            wait "$buildkit_pid" 2>/dev/null || true
            cleanup_runtime
            case "$buildkit_work" in
                "${TMPDIR:-/tmp}"/mooncake-buildkit.*) rm -rf -- "$buildkit_work" ;;
            esac
            exit "$status"
        }
        trap cleanup_buildkit EXIT
        for attempt in $(seq 1 100); do
            if [[ -S "$buildkit_socket" ]]; then
                break
            fi
            if ! kill -0 "$buildkit_pid" 2>/dev/null; then
                echo 'Dedicated BuildKit exited before its socket became ready' >&2
                exit 2
            fi
            sleep 0.1
        done
        [[ -S "$buildkit_socket" ]] || { echo 'Dedicated BuildKit startup timed out' >&2; exit 2; }
        export BUILDKIT_HOST="unix://$buildkit_socket"
    else
        export BUILDKIT_HOST="$MOONCAKE_BUILDKIT_HOST"
    fi
fi
build_log=${MOONCAKE_BUILD_LOG:-$base/kvtest-store-image-build-$(date +%Y%m%d-%H%M%S)-$$.log}
if [[ "$repair_only" == 0 ]]; then
run_logged_command "$build_log" \
    "$cli" "${args[@]}" build --progress=plain -f "$base/kvtest-store.Dockerfile" \
    --build-arg "BASE_IMAGE=$base_image" \
    --build-arg "BUILD_JOBS=$jobs" \
    --build-arg "MOONCAKE_USE_UB=$use_ub" \
    --build-arg "MOONCAKE_USE_TENT=$use_tent" \
    --build-arg "MOONCAKE_USE_EVENT_DRIVEN_COMPLETION=$event_completion" \
    --build-arg "MOONCAKE_STAGE_PROFILE=$stage_profile" \
    --build-arg "MOONCAKE_SOURCE_PROFILE=${MOONCAKE_SOURCE_PROFILE:-legacy}" \
    --build-arg "MOONCAKE_CCACHE=${MOONCAKE_CCACHE:-auto}" \
    --build-arg "URMA_INCLUDE_DIR=${URMA_INCLUDE_DIR:-/usr/include/ub/umdk/urma}" \
    --build-arg "URMA_LIBRARY=${URMA_LIBRARY:-/usr/lib64/liburma.so}" \
    --build-arg "MOONCAKE_PROXY_URL=${MOONCAKE_PROXY_URL-http://141.61.25.219:3128}" \
    --build-arg "MOONCAKE_PIP_INDEX_URL=${MOONCAKE_PIP_INDEX_URL:-https://pypi.tuna.tsinghua.edu.cn/simple}" \
    --build-arg "MOONCAKE_PIP_TIMEOUT=${MOONCAKE_PIP_TIMEOUT:-600}" \
    --build-arg "MOONCAKE_PIP_RETRIES=${MOONCAKE_PIP_RETRIES:-5}" \
    --build-arg "MOONCAKE_PIP_NO_CACHE=${MOONCAKE_PIP_NO_CACHE:-0}" \
    --build-arg "MOONCAKE_PIP_VERBOSE=${MOONCAKE_PIP_VERBOSE:-0}" \
    --build-arg "MOONCAKE_PIP_OFFLINE=${MOONCAKE_PIP_OFFLINE:-0}" \
    -t "$tag" "$base"
fi
if [[ "$use_ub" == ON ]]; then
    runtime_work=$(mktemp -d "${TMPDIR:-/tmp}/mooncake-urma-runtime.XXXXXX")
    # Only helper sources enter this context; URMA libraries come from the image.
    cp "$base/urma_runtime.py" "$base/urma-runtime.Dockerfile" "$runtime_work/"
    runtime_base=$tag
    if [[ "$repair_only" == 1 ]]; then runtime_base=$base_image; fi
    run_logged_command "$build_log.urma.log" \
        "$cli" "${args[@]}" build --progress=plain -f "$runtime_work/urma-runtime.Dockerfile" \
        --build-arg "BASE_IMAGE=$runtime_base" -t "$tag" "$runtime_work"
fi
"$cli" "${args[@]}" image inspect "$tag" >/dev/null
echo "Built image: $tag"
