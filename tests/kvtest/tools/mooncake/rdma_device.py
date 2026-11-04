#!/usr/bin/env python3
"""Resolve the verbs HCA actually made available to one benchmark Pod."""
import argparse
from pathlib import Path
import re
import sys


def require_ipc_lock(status_path=Path('/proc/self/status')):
    """CAP_IPC_LOCK lets verbs register buffers beyond the small memlock ulimit."""
    for line in status_path.read_text(encoding='ascii').splitlines():
        if line.startswith('CapEff:'):
            if int(line.split()[1], 16) & (1 << 14):
                return
            break
    raise RuntimeError('CAP_IPC_LOCK is not effective in this Pod; RDMA memory registration may fail')


def select_device(requested='auto', dev_root=Path('/dev/infiniband'),
                  verbs_root=Path('/sys/class/infiniband_verbs'),
                  hca_root=Path('/sys/class/infiniband')):
    if not isinstance(requested, str) or not re.fullmatch(r'[A-Za-z0-9_.:-]+', requested):
        raise ValueError('invalid RDMA device name')
    active = set()
    for node in dev_root.glob('uverbs*'):
        if not node.is_char_device():
            continue
        try:
            hca = (verbs_root / node.name / 'ibdev').read_text(encoding='ascii').strip()
            if not re.fullmatch(r'[A-Za-z0-9_.:-]+', hca):
                continue
            ports = hca_root / hca / 'ports'
            if any(state.read_text(encoding='ascii').strip().startswith('4:')
                   for state in ports.glob('*/state')):
                active.add(hca)
        except OSError:
            continue
    if not active:
        raise RuntimeError('no active allocated RDMA verbs HCA; check device plugin and port state')
    if requested == 'auto':
        if len(active) != 1:
            raise RuntimeError('multiple active allocated RDMA HCAs: ' + ', '.join(sorted(active))
                               + '; set mooncake_rdma.device_name explicitly')
        return next(iter(active))
    if requested not in active:
        raise RuntimeError('requested RDMA HCA ' + requested + ' is not active and allocated; available: '
                           + ', '.join(sorted(active)))
    return requested


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device-name', default='auto')
    parser.add_argument('--require-ipc-lock', action='store_true')
    args = parser.parse_args()
    try:
        if args.require_ipc_lock:
            require_ipc_lock()
        print(select_device(args.device_name))
    except (OSError, ValueError, RuntimeError) as error:
        print('RDMA device selection failed: ' + str(error), file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
