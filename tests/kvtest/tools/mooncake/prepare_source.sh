#!/usr/bin/env bash
# Download the pinned Mooncake source and its submodules without touching dirty work.
set -euo pipefail
base=$(cd "$(dirname "$0")" && pwd -P)
source "$base/proxy_env.sh"
source "$base/source_profile.sh"
src=${1:-$base/Mooncake}
revision=$mooncake_source_revision

# Retry in the same checkout: completed Git objects/submodules remain reusable.
# Keep HTTP settings local to these commands; never change the user's Git config.
retry_download() {
    local attempt rc
    for attempt in 1 2 3; do
        if git -c http.version=HTTP/1.1 "$@"; then
            return 0
        else
            rc=$?
        fi
        if [[ "$attempt" == 3 ]]; then
            echo "Source download failed after 3 attempts; rerun to retry: $src" >&2
            return "$rc"
        fi
        echo "Source download interrupted (attempt $attempt/3); retrying in $((attempt * 3))s." >&2
        sleep "$((attempt * 3))"
    done
}

if [[ ! -e "$src" ]]; then
    git init --quiet "$src"
    git -C "$src" remote add origin "$mooncake_source_repo"
fi
if [[ ! -e "$src/.git" ]]; then
    echo "Not a dedicated Git checkout: $src" >&2
    exit 2
fi
git -C "$src" rev-parse --is-inside-work-tree >/dev/null || {
    echo "Not a Git checkout: $src" >&2; exit 2;
}
if [[ -n "$(git -C "$src" status --porcelain --untracked-files=no)" ]]; then
    echo "Tracked Mooncake source changes found in $src; not switching revisions." >&2
    exit 2
fi
git -C "$src" submodule foreach --quiet --recursive \
    'test -z "$(git status --porcelain --untracked-files=no)" || { echo "Dirty submodule: $name" >&2; exit 1; }'
if ! git -C "$src" cat-file -e "$revision^{commit}" 2>/dev/null; then
    retry_download -C "$src" fetch --depth=1 --no-tags "$mooncake_source_repo" "$revision"
fi
if [[ "$(git -C "$src" rev-parse --verify HEAD 2>/dev/null || true)" != "$revision" ]]; then
    git -C "$src" checkout --detach "$revision"
fi
retry_download -C "$src" submodule update --init --recursive --depth=1
echo "Prepared Mooncake source: $src ($revision)"
