#!/usr/bin/env python3
"""Bounded, read-only cgroup/RDMA sampling inside Pods and host-side collection."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import sys
import threading
import time


HW_COUNTERS = ('roce_adp_retrans', 'roce_adp_retrans_to', 'local_ack_timeout_err',
               'packet_seq_err', 'req_cqe_error', 'resp_cqe_error',
               'rx_read_requests', 'rx_write_requests', 'np_ecn_marked_roce_packets', 'np_cnp_sent')


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def validate_host_rdma(mapping, nodes):
    if not isinstance(mapping, dict) or set(mapping) != set(nodes):
        raise ValueError('Host RDMA mapping must cover exactly the selected storage nodes')
    for node, target in mapping.items():
        if not re.fullmatch(r'[a-z0-9][a-z0-9.-]*', node):
            raise ValueError(f'Invalid host RDMA node: {node!r}')
        if not isinstance(target, dict) or set(target) != {'ssh', 'device'}:
            raise ValueError(f'Host RDMA {node} requires ssh and device')
        destination, device = target['ssh'], target['device']
        if not isinstance(destination, str) or not re.fullmatch(
                r'(?:[A-Za-z0-9_.-]+@)?[A-Za-z0-9][A-Za-z0-9_.-]*', destination):
            raise ValueError(f'Invalid SSH destination for {node}')
        if not isinstance(device, str) or not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]*', device):
            raise ValueError(f'Invalid RDMA device for {node}')
    return mapping


def sample_rdma(device='', infiniband=Path('/sys/class/infiniband')):
    rdma, errors = {}, []

    def read(path):
        try:
            return path.read_text().strip()
        except OSError as error:
            errors.append(f'{path}: {error}')
            return None

    def integer(path):
        value = read(path)
        try:
            return int(value) if value is not None else None
        except ValueError as error:
            errors.append(f'{path}: {error}')
            return None

    if device:
        port = infiniband / device / 'ports' / '1'
        for direction, counter in (('xmit', 'port_xmit_data'), ('rcv', 'port_rcv_data')):
            value = integer(port / 'counters' / counter)
            rdma[f'port_{direction}_bytes'] = None if value is None else value * 4
        for counter in HW_COUNTERS:
            rdma[counter] = integer(port / 'hw_counters' / counter)
    return {'timestamp': utc_now(), 'monotonic': time.monotonic(), 'cpu': {},
            'device': device, 'rdma': rdma,
            'port_state': read(port / 'state') if device else None,
            'port_rate': read(port / 'rate') if device else None,
            'numa_node': integer(infiniband / device / 'device' / 'numa_node') if device else None,
            'errors': errors}


def resolve_cgroup(membership, mountinfo, controller):
    for line in membership.splitlines():
        _, controllers, member = line.split(':', 2)
        if controllers and controller not in controllers.split(','):
            continue
        for mount in mountinfo.splitlines():
            fields, separator, fs = mount.partition(' - ')
            if not separator:
                continue
            filesystem = fs.split()
            if filesystem[0] not in ('cgroup', 'cgroup2'):
                continue
            if filesystem[0] == 'cgroup' and controller not in filesystem[2].split(','):
                continue
            root, point = fields.split()[3:5]
            if '..' in PurePosixPath(member).parts:
                continue
            if member == '/':
                return Path(point)
            try:
                relative = PurePosixPath(member).relative_to(root)
                return Path(point) / str(relative)
            except ValueError:
                continue
    raise ValueError(f'Cannot resolve {controller} cgroup from process membership and mountinfo')


def sample(device='', cgroup=Path('/sys/fs/cgroup'), infiniband=Path('/sys/class/infiniband')):
    errors = []

    def read(path):
        try:
            return path.read_text().strip()
        except OSError as error:
            errors.append(f'{path}: {error}')
            return None

    def integer(path):
        value = read(path)
        try:
            return int(value) if value is not None else None
        except ValueError:
            errors.append(f'{path}: invalid integer {value!r}')
            return None

    cpu = {k: None for k in ('usage_usec', 'nr_periods', 'nr_throttled', 'throttled_usec', 'quota_cores')}
    acct = None
    if cgroup == Path('/sys/fs/cgroup'):
        try:
            membership = Path('/proc/self/cgroup').read_text()
            mounts = Path('/proc/self/mountinfo').read_text()
            base = resolve_cgroup(membership, mounts, 'cpu')
            acct = resolve_cgroup(membership, mounts, 'cpuacct')
        except (OSError, ValueError) as error:
            errors.append(str(error))
            base = cgroup / 'unresolved-process-cgroup'
        v2 = (base / 'cgroup.controllers').exists() or (base / 'cpu.max').exists()
    else:
        v2 = (cgroup / 'cpu.stat').exists()
        base = cgroup if v2 else next((cgroup / n for n in ('cpu', 'cpu,cpuacct', 'cpuacct,cpu')
                                     if (cgroup / n / 'cpu.stat').exists()), cgroup / 'cpu')
    stat = read(base / 'cpu.stat')
    if stat:
        for line in stat.splitlines():
            key, value = line.split()
            if key in cpu:
                cpu[key] = int(value)
            if key == 'throttled_time':
                cpu['throttled_usec'] = int(value) / 1000
    if v2:
        quota = read(base / 'cpu.max')
        if quota:
            amount, period = quota.split()
            cpu['quota_cores'] = None if amount == 'max' else int(amount) / int(period)
    else:
        acct = acct or next((p for p in (base, cgroup / 'cpuacct') if (p / 'cpuacct.usage').exists()), base)
        usage = integer(acct / 'cpuacct.usage')
        cpu['usage_usec'] = None if usage is None else usage / 1000
        quota, period = integer(base / 'cpu.cfs_quota_us'), integer(base / 'cpu.cfs_period_us')
        if quota is not None and quota > 0 and period:
            cpu['quota_cores'] = quota / period
    result = sample_rdma(device, infiniband)
    result.update(cpu=cpu, cgroup_path=str(base), errors=errors + result['errors'])
    return result


def interval(before, after):
    elapsed = after['monotonic'] - before['monotonic']

    def delta(group, key):
        a, b = before.get(group, {}).get(key), after.get(group, {}).get(key)
        return b - a if elapsed > 0 and a is not None and b is not None and b >= a else None

    usage = delta('cpu', 'usage_usec')
    periods, throttled = delta('cpu', 'nr_periods'), delta('cpu', 'nr_throttled')
    rates = {'seconds': max(elapsed, 0), 'cpu_cores': None if usage is None else usage / elapsed / 1e6,
             'throttled_period_pct': None if not periods or throttled is None else 100 * throttled / periods,
             'cpu_delta': {k: delta('cpu', k) for k in ('usage_usec', 'nr_periods', 'nr_throttled', 'throttled_usec')},
             'rdma_delta': {k: delta('rdma', k) for k in after.get('rdma', {})}}
    for direction, key in (('tx', 'port_xmit_bytes'), ('rx', 'port_rcv_bytes')):
        value = delta('rdma', key)
        rates[f'{direction}_gbps'] = None if value is None else value * 8 / elapsed / 1e9
    return rates


def summarize_file(path):
    records, invalid = [], 0
    for line in path.read_text(errors='replace').splitlines():
        try:
            record = json.loads(line)
            if 'monotonic' not in record:
                raise ValueError('not a sample')
            records.append(record)
        except (ValueError, TypeError):
            invalid += 1
    rates = [interval(a, b) for a, b in zip(records, records[1:])]
    result = {'samples': len(records), 'intervals': len(rates), 'invalid_lines': invalid,
              'first_sample': records[0].get('timestamp') if records else None,
              'last_sample': records[-1].get('timestamp') if records else None,
              'errors': sorted({e for record in records for e in record.get('errors', [])})}
    for key in ('port_state', 'port_rate', 'numa_node'):
        result[key] = records[-1].get(key) if records else None
    for key in ('cpu_cores', 'tx_gbps', 'rx_gbps'):
        valid = [(r[key], r['seconds']) for r in rates if r[key] is not None]
        result[key + '_avg'] = sum(v * t for v, t in valid) / sum(t for _, t in valid) if valid else None
        result[key + '_peak'] = max((v for v, _ in valid), default=None)
        result[key + '_coverage_seconds'] = sum(t for _, t in valid)
    for group in ('cpu_delta', 'rdma_delta'):
        keys = {k for r in rates for k in r[group]}
        result[group] = {k: sum(values) if values else None for k in keys
                         for values in [[r[group][k] for r in rates if r[group].get(k) is not None]]}
    periods = result['cpu_delta'].get('nr_periods')
    throttled = result['cpu_delta'].get('nr_throttled')
    result['throttled_period_pct'] = 100 * throttled / periods if periods and throttled is not None else None
    return result


def probe_main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', default='')
    parser.add_argument('--interval', type=float, default=5)
    parser.add_argument('--max-seconds', type=float, default=1200)
    parser.add_argument('--rdma-only', action='store_true')
    args = parser.parse_args(argv)
    if args.interval <= 0 or args.max_seconds <= 0:
        parser.error('interval and max-seconds must be positive')
    if args.rdma_only and not args.device:
        parser.error('rdma-only requires a device')
    stopped = threading.Event()

    def watch_input():
        try:
            while os.read(sys.stdin.fileno(), 4096):
                pass
        finally:
            stopped.set()

    threading.Thread(target=watch_input, daemon=True).start()
    deadline = time.monotonic() + args.max_seconds
    sampler = sample_rdma if args.rdma_only else sample
    while True:
        print(json.dumps(sampler(args.device)), flush=True)
        remaining = deadline - time.monotonic()
        if remaining <= 0 or stopped.wait(min(args.interval, remaining)):
            break
    print(json.dumps(sampler(args.device)), flush=True)


class MonitorSession:
    def __init__(self, namespace, case_dir, max_seconds, interval_seconds=5, host_rdma=None):
        self.namespace, self.case_dir = namespace, case_dir
        self.max_seconds, self.interval_seconds = max_seconds, interval_seconds
        self.entries = []
        self.host_rdma = host_rdma

    def start(self):
        pods = json.loads((self.case_dir / 'pods.json').read_text())['items']
        devices = json.loads((self.case_dir / 'rdma-devices.json').read_text())
        if self.host_rdma is not None:
            nodes = {p['spec']['nodeName'] for p in pods if p['metadata']['name'].startswith('mc-store-')}
            validate_host_rdma(self.host_rdma, nodes)
            (self.case_dir / 'host-rdma.json').write_text(json.dumps(self.host_rdma, indent=2) + '\n', encoding='utf-8')
        directory = self.case_dir / 'monitoring'
        directory.mkdir(exist_ok=False)
        source = Path(__file__).read_text(encoding='utf-8')
        launches = []
        for pod in pods:
            name = pod['metadata']['name']
            device = devices.get(name, '') if name.startswith('mc-store-') and not self.host_rdma else ''
            argv = ['kubectl', '-n', self.namespace, 'exec', '-i', name, '--', 'python3', '-u',
                    '-c', source, '--interval', str(self.interval_seconds),
                    '--max-seconds', str(self.max_seconds), '--device', device]
            entry = {'pod': name, 'node': pod['spec'].get('nodeName'), 'device': device,
                     'started_at': utc_now(), 'data_file': name + '.jsonl', 'error_file': name + '.stderr'}
            launches.append((entry, argv))
        for node, target in sorted((self.host_rdma or {}).items()):
            remote = ['python3', '-u', '-c', source, '--rdma-only', '--device', target['device'],
                      '--interval', str(self.interval_seconds), '--max-seconds', str(self.max_seconds)]
            argv = ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
                    '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=3',
                    target['ssh'], shlex.join(remote)]
            entry = {'host': node, 'node': node, 'device': target['device'], 'ssh': target['ssh'],
                     'started_at': utc_now(), 'data_file': f'host-rdma-{node}.jsonl',
                     'error_file': f'host-rdma-{node}.stderr'}
            launches.append((entry, argv))
        for entry, argv in launches:
            self.entries.append(entry)
            print(f"Monitor: {entry.get('pod', entry.get('host'))} "
                  + ('CPU ' if 'pod' in entry else 'SSH host ')
                  + (f"shared RDMA port {entry['device']}" if entry['device'] else ''), flush=True)
            out = (directory / entry['data_file']).open('w', encoding='utf-8')
            err = (directory / entry['error_file']).open('w', encoding='utf-8')
            entry['_handles'] = (out, err)
            try:
                entry['_process'] = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=out, stderr=err)
            except OSError as error:
                entry['error'] = str(error)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            pending = [e for e in self.entries if e.get('_process') and e['_process'].poll() is None
                       and not (directory / e['data_file']).stat().st_size]
            if not pending:
                break
            time.sleep(0.2)
        for entry in self.entries:
            if not (directory / entry['data_file']).stat().st_size:
                entry['error'] = entry.get('error', 'No baseline sample received before client deployment')

    def stop(self):
        directory = self.case_dir / 'monitoring'
        if not directory.is_dir():
            return
        for entry in self.entries:
            process = entry.get('_process')
            if process and process.stdin:
                try:
                    process.stdin.close()
                except OSError:
                    pass
        deadline = time.monotonic() + 15
        for entry in self.entries:
            process = entry.pop('_process', None)
            if process:
                try:
                    process.wait(timeout=max(0.1, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                    entry['error'] = 'Remote monitor did not exit on EOF, probe has a hard time limit'
                entry['exit_code'] = process.returncode
            for handle in entry.pop('_handles', ()):
                handle.close()
            entry.update(summarize_file(directory / entry['data_file']))
        summary = {'started_before_clients': True, 'finished_at': utc_now(),
                   'scope': 'CPU is container cgroup. RDMA is shared HCA port, sampled once per storage node, '
                            'via SSH host probes when configured, otherwise inside store Pods. '
                            'Includes client deployment, write phase and shutdown. Missing counters are null. '
                            'Port data counters use 4-byte units. Values are not isolated wire bandwidth.',
                   'pods': [e for e in self.entries if 'pod' in e],
                   'hosts': [e for e in self.entries if 'host' in e]}
        (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
        for entry in self.entries:
            cpu = (f"CPU avg={entry['cpu_cores_avg']} cores, throttled periods={entry['throttled_period_pct']}%, "
                   if 'pod' in entry else '')
            print(f"Monitor {entry.get('pod', entry.get('host'))}: " + cpu +
                  f"TX/RX avg={entry['tx_gbps_avg']}/{entry['rx_gbps_avg']} Gbit/s "
                  f"samples={entry['samples']} exit={entry.get('exit_code')}"
                  + (' (see errors in summary.json)' if entry.get('error') or entry['errors'] else ''), flush=True)


if __name__ == '__main__':
    probe_main()
