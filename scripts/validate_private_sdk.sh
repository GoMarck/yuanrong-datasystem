#!/usr/bin/env bash
set -euo pipefail
: "${DS_OPENSOURCE_DIR:?Set DS_OPENSOURCE_DIR to a verified target-local dependency cache}"
source_dir=$(cd "$(dirname "$0")/.." && pwd)
validation_dir=${1:?usage: validate_private_sdk.sh <validation-directory> [worker-host worker-port]}
mkdir -p "$validation_dir"
validation_dir=$(cd "$validation_dir" && pwd)
log_dir="$validation_dir/logs"
mkdir -p "$log_dir"
jobs=${DS_VALIDATION_JOBS:-4}
run_logged() {
    local label=$1
    shift
    local logfile="$log_dir/$label.log"
    local status=0
    "$@" >"$logfile" 2>&1 || status=$?
    echo "step=$label status=$status log=$logfile"
    if test "$status" -ne 0; then
        tail -60 "$logfile"
        return "$status"
    fi
}
if test -e "$validation_dir/relocated-sdk"; then
    echo "Use a fresh validation directory: relocated-sdk already exists" >&2
    exit 1
fi
run_logged configure cmake -S "$source_dir" -B "$validation_dir/sdk-build" \
    -DDS_BUILD_STATIC_SDK=ON -DDS_OPENSOURCE_DIR="$DS_OPENSOURCE_DIR" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$validation_dir/sdk"
run_logged build cmake --build "$validation_dir/sdk-build" -j "$jobs"
run_logged install cmake --install "$validation_dir/sdk-build"
mv "$validation_dir/sdk" "$validation_dir/relocated-sdk"
run_logged consumer-configure cmake -S "$source_dir/example/cpp/private_static_sdk" \
    -B "$validation_dir/consumer-build" -DCMAKE_PREFIX_PATH="$validation_dir/relocated-sdk"
run_logged consumer-build cmake --build "$validation_dir/consumer-build" -j "$jobs"
run_logged linkage python3 "$source_dir/scripts/verify_private_sdk.py" \
    --prefix "$validation_dir/relocated-sdk" \
    --library "$validation_dir/consumer-build/libds_private_sdk_consumer.so" \
    --link-map "$validation_dir/consumer-build/ds_private_sdk_consumer.map"
if test "$#" -eq 3; then
    run_logged runtime "$validation_dir/consumer-build/ds_private_sdk_smoke" "$2" "$3"
else
    echo "runtime=NOT_RUN reason=no_worker_endpoint"
fi
