#!/usr/bin/env python3
"""Read service command lines and selected transport settings from procfs."""
import json
from pathlib import Path

SERVICES = {'datasystem_worker', 'datasystem_coordinator', 'mooncake_master',
            'store_pod_target.py', 'kvtest'}
ENV_KEYS = {'HOST_IP', 'POD_IP', 'LD_LIBRARY_PATH', 'MC_GID_INDEX', 'MC_IB_PORT',
            'MC_SLICE_SIZE', 'MC_WORKERS_PER_CTX', 'MC_NUM_QP_PER_EP', 'MC_NUM_CQ_PER_CTX', 'MC_MAX_WR',
            'MC_RDMA_DATA_DIRECT', 'MC_DEVICE_NAME', 'DATASYSTEM_UB_GET_DATA_SIZE_BYTES',
            'MC_TE_METRIC', 'MC_TE_METRIC_INTERVAL_SECONDS',
            'MC_STORE_CLIENT_METRIC', 'MC_STORE_CLIENT_METRIC_INTERVAL'}


def capture_processes(proc=Path('/proc')):
    result = []
    for directory in sorted(proc.glob('[0-9]*')):
        record = None
        try:
            argv = directory.joinpath('cmdline').read_bytes().decode(errors='replace').strip('\0').split('\0')
            matches = [Path(arg).name for arg in argv[:4] if Path(arg).name in SERVICES]
            if not matches:
                continue
            record = {'pid': int(directory.name), 'component': matches[0], 'argv': argv}
            env = directory.joinpath('environ').read_bytes().decode(errors='replace').split('\0')
            record['environment'] = {key: value for item in env if '=' in item
                                     for key, value in [item.split('=', 1)] if key in ENV_KEYS}
            try:
                record['affinity'] = {
                    key: value.strip() for line in directory.joinpath('status').read_text().splitlines()
                    if ':' in line for key, value in [line.split(':', 1)]
                    if key in ('Cpus_allowed_list', 'Mems_allowed_list')}
            except OSError as error:
                record['affinity_error'] = str(error)
            if matches[0] == 'kvtest' and argv[-1].endswith('.json'):
                config = Path(argv[-1])
                if not config.is_absolute():
                    config = directory / 'cwd' / config
                record['config_file'] = argv[-1]
                record['config'] = json.loads(config.read_text())
            result.append(record)
        except (OSError, ValueError) as error:
            if record is not None:
                record['capture_error'] = str(error)
                result.append(record)
    return result


if __name__ == '__main__':
    print(json.dumps(capture_processes()))
