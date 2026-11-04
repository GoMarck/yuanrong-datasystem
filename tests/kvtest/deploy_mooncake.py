#!/usr/bin/env python3
"""Manage Mooncake master/store processes in Pods created by deploy_pods.py.

This is the Mooncake service counterpart of deploy_coordinator.py and
deploy_worker.py. The image must already contain the compiled Mooncake output.
"""

import argparse
import json
import re
import shlex
import socket
import subprocess
import sys
import time
from pathlib import Path

from deploy_common import DEFAULT_TIMEOUT, do_for_all_pods, get_pods, kubectl_exec
from kvtest_environment import validate_environment


PID_FILES = {role: f'/tmp/kvtest-mooncake-{role}.pid' for role in ('master', 'store')}
LOG_FILES = {role: f'/tmp/kvtest-mooncake-{role}.log' for role in ('master', 'store')}
STORE_READY = '/tmp/mooncake-store-ready'
MASTER_BINARY = '/opt/Mooncake/build-ub/mooncake-store/src/mooncake_master'
STORE_BINARY = '/opt/mooncake-bench/store_pod_target.py'


def service_command(role, pod, args):
    """Construct one process argv; never put configuration into shell syntax."""
    if role == 'master':
        command = [MASTER_BINARY, f'--rpc_address={pod["ip"]}', f'--rpc_port={args.port}']
        for key, value in getattr(args, 'extra_args', {}).items():
            command.append(f'--{key}={str(value).lower() if isinstance(value, bool) else value}')
        return command
    if role != 'store':
        raise ValueError(f'unknown Mooncake service role: {role}')
    if args.protocol not in ('tcp', 'rdma'):
        raise ValueError('Mooncake KVTest service transport must be tcp or rdma')
    if not args.master or args.segment_size <= 0 or args.local_buffer_size <= 0:
        raise ValueError('store requires master and positive segment/buffer sizes')
    command = ['python3', '-u', STORE_BINARY,
               f'--master={args.master}', f'--segment-size={args.segment_size}',
               f'--local-buffer-size={args.local_buffer_size}', f'--protocol={args.protocol}']
    if args.protocol == 'rdma':
        device = args.device_name
        if args.devices_json:
            devices = json.loads(Path(args.devices_json).read_text(encoding='utf-8'))
            device = devices.get(pod['name'], '')
        if not device or device == 'auto':
            raise ValueError(f'RDMA device must be explicitly allocated for {pod["name"]}')
        command.append(f'--device-name={device}')
    return command


def is_ready(role, pod, args):
    if role == 'store':
        pid_file = PID_FILES['store']
        script = (f'test -f {STORE_READY} && test -f {pid_file} '
                  f'&& pid=$(cat {pid_file}) '
                  '&& kill -0 "$pid" 2>/dev/null '
                  '&& grep -aqF store_pod_target.py /proc/"$pid"/cmdline '
                  '&& test "$(awk \'{print $3}\' /proc/"$pid"/stat)" != Z')
        result = kubectl_exec(pod['name'], args.namespace,
                              script, check=False, timeout=args.timeout)
        return result.returncode == 0
    try:
        with socket.create_connection((pod['ip'], args.port), timeout=2):
            return True
    except OSError:
        return False


def wait_ready(role, pod, args):
    deadline = time.monotonic() + args.timeout
    while True:
        if is_ready(role, pod, args):
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(1)


def start_pod(pod, args):
    role = args.role
    argv = service_command(role, pod, args)
    pid_file = shlex.quote(PID_FILES[role])
    log_file = shlex.quote(LOG_FILES[role])
    prefix = f'POD_IP={shlex.quote(pod["ip"])} ' if role == 'store' else ''
    environment = validate_environment(getattr(args, 'environment', {}), 'service environment', ('POD_IP',))
    prefix += ''.join(f'{key}={shlex.quote(value)} ' for key, value in sorted(environment.items()))
    clear_ready = f'rm -f {STORE_READY}; ' if role == 'store' else ''
    state = "awk '{print $3}'"
    script = (f'if test -f {pid_file} && kill -0 "$(cat {pid_file})" 2>/dev/null '
              f'&& test "$({state} /proc/$(cat {pid_file})/stat 2>/dev/null)" != Z; '
              f'then echo "service already running" >&2; exit 4; fi; '
              f'{clear_ready}{prefix}nohup {shlex.join(argv)} > {log_file} 2>&1 </dev/null & '
              f'echo $! > {pid_file}')
    try:
        result = kubectl_exec(pod['name'], args.namespace, script,
                              check=False, timeout=30)
        if result.returncode != 0:
            print(f'{pod["name"]}: launch failed: {result.stderr.strip()}', file=sys.stderr)
            return False
        if wait_ready(role, pod, args):
            print(f'{pod["name"]}: {role} ready')
            return True
        log = kubectl_exec(pod['name'], args.namespace,
                           f'tail -40 {log_file}', check=False, timeout=15)
        print(f'{pod["name"]}: {role} not ready; {LOG_FILES[role]}\n{log.stdout}',
              file=sys.stderr)
        return False
    except (OSError, subprocess.SubprocessError) as exc:
        print(f'{pod["name"]}: {role} start failed: {exc}', file=sys.stderr)
        return False


def stop_pod(pod, args):
    pid_file = shlex.quote(PID_FILES[args.role])
    state = "awk '{print $3}'"
    marker = 'mooncake_master' if args.role == 'master' else 'store_pod_target.py'
    script = (f'if test -f {pid_file}; then '
              f'pid=$(cat {pid_file}); '
              f'if kill -0 "$pid" 2>/dev/null && '
              f'test "$({state} /proc/$pid/stat 2>/dev/null)" != Z && '
              f'! grep -aqF {marker} /proc/$pid/cmdline 2>/dev/null; '
              f'then echo "PID belongs to another process" >&2; exit 5; fi; '
              f'kill -TERM "$pid" 2>/dev/null || true; '
              f'i=0; while kill -0 "$pid" 2>/dev/null && '
              f'test "$({state} /proc/$pid/stat 2>/dev/null)" != Z '
              f'&& test "$i" -lt 15; '
              f'do sleep 1; i=$((i+1)); done; '
              f'if kill -0 "$pid" 2>/dev/null && '
              f'test "$({state} /proc/$pid/stat 2>/dev/null)" != Z; '
              f'then exit 1; fi; '
              f'rm -f {pid_file}; fi')
    try:
        result = kubectl_exec(pod['name'], args.namespace, script,
                              check=False, timeout=25)
        return result.returncode == 0
    except (OSError, subprocess.SubprocessError) as exc:
        print(f'{pod["name"]}: stop failed: {exc}', file=sys.stderr)
        return False


def kubectl_cp_from(pod, namespace, source, target, timeout=DEFAULT_TIMEOUT):
    subprocess.run(['kubectl', 'cp', f'{namespace}/{pod}:{source}', target],
                   check=True, capture_output=True, text=True, timeout=timeout)


def collect_pod(pod, args):
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    target = str(output / f'{pod["name"]}.log')
    try:
        kubectl_cp_from(pod['name'], args.namespace, LOG_FILES[args.role], target,
                        timeout=args.timeout)
        print(f'{pod["name"]}: {target}')
        return True
    except (OSError, subprocess.SubprocessError) as exc:
        print(f'{pod["name"]}: log collection failed: {exc}', file=sys.stderr)
        return False


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('start', 'check', 'stop', 'collect'))
    parser.add_argument('--role', choices=('master', 'store'), required=True)
    parser.add_argument('--namespace', '-n', default='default')
    parser.add_argument('--prefix', '-p', required=True)
    parser.add_argument('--port', type=int, default=50051,
                        help='master RPC port (default: 50051)')
    parser.add_argument('--master', help='master host:port, required for store start')
    parser.add_argument('--segment-size', type=int, default=1073741824)
    parser.add_argument('--local-buffer-size', type=int, default=536870912)
    parser.add_argument('--protocol', choices=('tcp', 'rdma'), default='tcp')
    parser.add_argument('--device-name', default='', help='RDMA device shared by selected Pods')
    parser.add_argument('--devices-json', help='JSON mapping Pod names to allocated RDMA devices')
    parser.add_argument('--env-json', type=Path, help='start only: JSON environment map for the service process')
    parser.add_argument('--args-json', type=Path, help='master start only: extra scalar flags excluding RPC address and port')
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--output', '-o', default='mooncake-service-logs')
    args = parser.parse_args(argv)
    args.extra_args = {}
    if args.args_json is not None:
        if args.command != 'start' or args.role != 'master':
            parser.error('--args-json applies only to master start')
        try:
            args.extra_args = json.loads(args.args_json.read_text(encoding='utf-8'))
            if not isinstance(args.extra_args, dict) or any(
                    not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', key) or key in ('rpc_address', 'rpc_port')
                    or not isinstance(value, (str, int, float, bool)) or '\0' in str(value)
                    for key, value in args.extra_args.items()):
                raise ValueError('master args require scalar flags and cannot override RPC address/port')
        except (OSError, ValueError) as error:
            parser.error(str(error))
    if args.env_json is not None and args.command != 'start':
        parser.error('--env-json applies only to start')
    try:
        args.environment = validate_environment(
            json.loads(args.env_json.read_text(encoding='utf-8')) if args.env_json else {},
            'service environment', ('POD_IP',))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if args.timeout <= 0 or not 1 <= args.port <= 65535:
        parser.error('--timeout must be positive and --port must be in 1..65535')
    pods = get_pods(args.namespace, [args.prefix])
    if not pods:
        parser.error(f'no running Pods matched {args.prefix!r} in {args.namespace}')
    actions = {'start': start_pod,
               'check': lambda pod, a: is_ready(a.role, pod, a),
               'stop': stop_pod,
               'collect': collect_pod}
    return do_for_all_pods(pods, lambda pod: actions[args.command](pod, args),
                           f'{args.command} Mooncake {args.role}')


if __name__ == '__main__':
    sys.exit(main())
