#!/usr/bin/env bash
# Export settings so pip's isolated build-dependency installer inherits them too.
export PIP_INDEX_URL="${MOONCAKE_PIP_INDEX_URL:-${PIP_INDEX_URL:-https://pypi.tuna.tsinghua.edu.cn/simple}}"
export PIP_DEFAULT_TIMEOUT="${MOONCAKE_PIP_TIMEOUT:-${PIP_TIMEOUT:-${PIP_DEFAULT_TIMEOUT:-600}}}"
export PIP_TIMEOUT="$PIP_DEFAULT_TIMEOUT"
export PIP_RETRIES="${MOONCAKE_PIP_RETRIES:-${PIP_RETRIES:-5}}"
export PIP_VERBOSE="${MOONCAKE_PIP_VERBOSE:-${PIP_VERBOSE:-0}}"
export PIP_DISABLE_PIP_VERSION_CHECK="${PIP_DISABLE_PIP_VERSION_CHECK:-1}"
# Preserve standalone pip settings unless the build explicitly selects a mode.
# Do not export PIP_NO_CACHE_DIR=0: pip's negative boolean option is special.
case "${MOONCAKE_PIP_NO_CACHE-}" in
    '') ;;
    0) unset PIP_NO_CACHE_DIR ;;
    1) export PIP_NO_CACHE_DIR=1 ;;
    *) echo 'MOONCAKE_PIP_NO_CACHE must be 0 or 1' >&2; return 2 ;;
esac
case "${MOONCAKE_PIP_OFFLINE:-0}" in
    0) ;;
    1)
        _mooncake_wheelhouse=${MOONCAKE_PIP_WHEELHOUSE:-/opt/mooncake-wheelhouse}
        _mooncake_has_wheel=0
        for _mooncake_wheel in "$_mooncake_wheelhouse"/*.whl; do
            if [[ -f "$_mooncake_wheel" ]]; then _mooncake_has_wheel=1; break; fi
        done
        if [[ "$_mooncake_has_wheel" == 0 ]]; then
            echo "Offline pip wheelhouse is empty or missing: $_mooncake_wheelhouse" >&2
            echo 'Run download_store_wheels.sh or copy matching wheels into wheelhouse/ first.' >&2
            return 2
        fi
        # Use a URI so pip can also handle standalone wheelhouse paths with spaces.
        PIP_FIND_LINKS=$(python3 -c 'from pathlib import Path; import sys; print(Path(sys.argv[1]).resolve().as_uri())' \
            "$_mooncake_wheelhouse") || return
        export PIP_FIND_LINKS PIP_NO_INDEX=1
        unset _mooncake_wheelhouse _mooncake_has_wheel _mooncake_wheel
        ;;
    *) echo 'MOONCAKE_PIP_OFFLINE must be 0 or 1' >&2; return 2 ;;
esac
