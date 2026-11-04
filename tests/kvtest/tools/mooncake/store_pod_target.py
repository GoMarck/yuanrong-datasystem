#!/usr/bin/env python3
"""Keep a Mooncake Store memory segment alive inside a Kubernetes Pod."""
import argparse
import os
import signal
import sys
import threading
from pathlib import Path

READY = Path('/tmp/mooncake-store-ready')


def setup_target(store, master, segment_size, buffer_size, protocol, device, pod_ip):
    if not pod_ip:
        raise ValueError('POD_IP is required')
    if protocol in ('ub', 'rdma') and not device:
        raise ValueError(protocol.upper() + ' protocol requires device name')
    if protocol == 'ub':
        if device == 'auto':
            devices = sorted(p.name for p in Path('/dev/uburma').glob('bonding_dev_*')
                             if p.is_char_device())
            if len(devices) != 1:
                raise RuntimeError('UB device allocation: expected one bonding device in '
                                   '/dev/uburma, found %s; check device-plugin allocation, '
                                   'or set device_name explicitly for multiple devices' % devices)
            device = devices[0]
        print('UB_DEVICE selected=' + device, flush=True)
    elif protocol == 'rdma':
        print('RDMA_DEVICE selected=' + device, flush=True)
    result = store.setup(pod_ip, 'P2PHANDSHAKE', segment_size,
                         buffer_size, protocol, device if protocol in ('ub', 'rdma') else '',
                         master)
    if result:
        raise RuntimeError('Mooncake Store target setup failed: %s' % result)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--master', required=True)
    parser.add_argument('--segment-size', type=int, required=True)
    parser.add_argument('--local-buffer-size', type=int, required=True)
    parser.add_argument('--protocol', choices=('ub', 'tcp', 'rdma'), required=True)
    parser.add_argument('--device-name', default='')
    args = parser.parse_args(argv)
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    READY.unlink(missing_ok=True)
    from mooncake.store import MooncakeDistributedStore
    store = MooncakeDistributedStore()
    try:
        setup_target(store, args.master, args.segment_size, args.local_buffer_size,
                     args.protocol, args.device_name, os.environ.get('POD_IP'))
        READY.write_text('ready\n', encoding='ascii')
        print('STORE_READY', flush=True)
        stop.wait()
    finally:
        READY.unlink(missing_ok=True)
        if hasattr(store, 'close'):
            store.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
