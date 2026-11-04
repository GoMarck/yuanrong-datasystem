#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software distributed under the License is
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and limitations under the License.
set -uo pipefail

: "${WORKER_HOST:?Set WORKER_HOST to the bound Worker IP}"
: "${SDK_LOCAL_IP:?Set SDK_LOCAL_IP to this SDK host IP}"
export DS_RH2D_HCCS_BUFFER_POOL="${DS_RH2D_HCCS_BUFFER_POOL:-4:8}"
export DS_HIXL_CS_ENABLE="${DS_HIXL_CS_ENABLE:-false}"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || exit 1
run_dir="$(mktemp -d "${TMPDIR:-/tmp}/ds_hixl_multi.XXXXXX")" || exit 1
IFS=',' read -r -a devices <<< "${SDK_DEVICE_IDS:-0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}"
for device in "${devices[@]}"; do
    if [[ ! "$device" =~ ^[0-9]+$ ]]; then
        printf 'Invalid SDK device ID: %s\n' "$device" >&2
        exit 1
    fi
done

pids=()
printf 'Logs: %s\n' "$run_dir"
for device in "${devices[@]}"; do
    "${PYTHON:-python3}" "$script_dir/hixl_d2rh_release_e2e.py" \
        --worker-host "$WORKER_HOST" --worker-port "${WORKER_PORT:-31501}" \
        --local-ip "$SDK_LOCAL_IP" --device-id "$device" \
        --rounds "${ROUNDS:-32}" --min-hixl-samples "${MIN_HIXL_SAMPLES:-2}" \
        --enable-remote-h2d --output-dir "$run_dir" \
        > "$run_dir/device_${device}.log" 2>&1 &
    pids+=("$!")
done

result=0
for index in "${!pids[@]}"; do
    if wait "${pids[$index]}"; then
        printf 'PASS SDK device %s\n' "${devices[$index]}"
    else
        printf 'FAIL SDK device %s; see %s/device_%s.log\n' \
            "${devices[$index]}" "$run_dir" "${devices[$index]}" >&2
        result=1
    fi
done
exit "$result"
