#!/usr/bin/env bash
# Preserve command failures through tee and repeat useful diagnostics after warning floods.
run_logged_command() (
    set -o pipefail
    local log=$1
    shift
    mkdir -p "$(dirname "$log")" || return
    : > "$log" || return
    echo "Full command log: $log"
    if "$@" 2>&1 | tee "$log"; then
        return 0
    else
        status=$?
    fi
    echo "===== Build failed (exit $status): first error contexts =====" >&2
    if ! grep -n -B 4 -A 8 -m 5 -E \
        'fatal error:|(^|[[:space:]])error:|undefined reference|collect2:|Killed signal|No rule to make target|CMake Error|FAILED:' \
        "$log" >&2; then
        echo 'No primary compiler error found in captured output; last 30 lines follow.' >&2
        tail -n 30 "$log" >&2
    fi
    echo "Full command log: $log" >&2
    return "$status"
)
