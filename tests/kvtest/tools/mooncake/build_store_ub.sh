#!/usr/bin/env bash
# Build the official upstream wheel, then install it through pip.
set -euo pipefail
base=$(cd "$(dirname "$0")" && pwd -P)
source "$base/pip_env.sh"
# Report inherited pip controls without printing potentially authenticated URLs.
printf 'Pip environment: timeout=%s retries=%s no_cache=%s verbose=%s version_check_disabled=%s\n' \
    "$PIP_TIMEOUT" "$PIP_RETRIES" "${PIP_NO_CACHE_DIR:-unset}" "$PIP_VERBOSE" "$PIP_DISABLE_PIP_VERSION_CHECK"
if [[ "${MOONCAKE_PIP_OFFLINE:-0}" == 1 ]]; then
    echo "Pip offline: enabled; wheelhouse=${MOONCAKE_PIP_WHEELHOUSE:-/opt/mooncake-wheelhouse}"
fi
source "$base/configure_ub.sh" store "$@"
[[ -f "$src/pyproject.toml" ]] || { echo 'Missing upstream pyproject.toml' >&2; exit 2; }

# Match earlier builds' generator so the existing native object cache is reusable.
export CMAKE_GENERATOR="Unix Makefiles"
export CMAKE_BUILD_PARALLEL_LEVEL="$jobs"
wheel_options=("--config-settings=build-dir=$src/build-ub"
    "--config-settings=build.targets=mooncake_master;mooncake_client;store;engine;_fast_copy")
# Preserve typed CMake definitions and spaces in dependency paths.
cmake_settings=$(IFS=';'; printf '%s' "${cmake_args[*]}")
wheel_options+=("--config-settings=cmake.args=$cmake_settings")
protocol=ub
[[ "$use_ub" == ON ]] || protocol=tcp
mkdir -p "$src/dist/$protocol"
wheel_dir=$(mktemp -d "$src/dist/$protocol/build-XXXXXXXX")
run_logged_command "$src/build-ub/store-wheel.log" \
    python3 -m pip wheel --verbose --progress-bar off --no-deps \
    --wheel-dir "$wheel_dir" "${wheel_options[@]}" "$src"
shopt -s nullglob
wheels=("$wheel_dir"/mooncake_transfer_engine-*.whl)
[[ ${#wheels[@]} == 1 ]] || { echo "Expected one Mooncake wheel in $wheel_dir" >&2; exit 2; }
# Replace an older same-version build; resolve missing runtime dependencies without
# force-upgrading every dependency when rebuilding the native package.
run_logged_command "$src/build-ub/store-install.log" \
    python3 -m pip install --progress-bar off --no-deps --force-reinstall "${wheels[0]}"
run_logged_command "$src/build-ub/store-dependencies.log" \
    python3 -m pip install --progress-bar off "${wheels[0]}" "numpy>=1.24"
python3 - "${wheels[0]}" "$actual" "$protocol" <<'PY'
import hashlib
from importlib.metadata import version
import json
from pathlib import Path
import platform
import os
import sys
# Upstream declares NumPy only as a build dependency, but _fast_copy imports it.
import mooncake._fast_copy
from mooncake.store import MooncakeDistributedStore
wheel = Path(sys.argv[1])
digest = hashlib.sha256()
with wheel.open("rb") as stream:
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(chunk)
metadata = {
    "distribution": "mooncake-transfer-engine",
    "version": version("mooncake-transfer-engine"),
    "source_revision": sys.argv[2],
    "stage_profile": os.environ.get("MOONCAKE_STAGE_PROFILE", "OFF") == "ON",
    "use_ub": sys.argv[3] == "ub",
    "python": platform.python_version(),
    "machine": platform.machine(),
    "wheel": wheel.name,
    "sha256": digest.hexdigest(),
}
wheel.with_suffix(".build.json").write_text(json.dumps(metadata, indent=2) + "\n")
print("Mooncake Store import: OK; installed version:", metadata["version"])
print("Wheel:", wheel)
PY
printf '%s\n' "$actual" > "$src/build-ub/source_revision.txt"
echo "Store master: $src/build-ub/mooncake-store/src/mooncake_master"
