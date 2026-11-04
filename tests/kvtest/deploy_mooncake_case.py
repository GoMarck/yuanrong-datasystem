#!/usr/bin/env python3
"""Start, run with optional monitoring, collect, and delete one Mooncake RDMA case."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

import deploy_compare_ub_rdma as compare
from kvtest_monitor import MonitorSession, utc_now, validate_host_rdma
from kvtest_thread_cpu import capture_client as capture_client_thread_cpu

ROOT = compare.ROOT
TELEMETRY = {'MC_TE_METRIC': '1', 'MC_TE_METRIC_INTERVAL_SECONDS': '5',
             'MC_STORE_CLIENT_METRIC': '1', 'MC_STORE_CLIENT_METRIC_INTERVAL': '5'}


def save_state(output, state):
    target = output / 'state.json'
    temporary = target.with_suffix('.tmp')
    temporary.write_text(json.dumps(state, indent=2) + '\n', encoding='utf-8')
    temporary.replace(target)


def resume_run(output):
    run = compare.Run.__new__(compare.Run)
    run.output, run.commands = output, output / 'commands'
    run.commands.mkdir(exist_ok=True)
    run.step = max((int(p.name.split('-')[0]) for p in run.commands.glob('[0-9]*-*.log')), default=0)
    return run


@contextmanager
def phase_lock(output):
    lock = output.with_name(output.name + '.lock')
    lock.parent.mkdir(parents=True, exist_ok=True)
    try:
        handle = lock.open('x', encoding='utf-8')
    except FileExistsError:
        raise RuntimeError(f'Case phase lock exists: {lock}, check for another active command') from None
    try:
        with handle:
            handle.write(str(os.getpid()) + '\n')
        yield
    finally:
        lock.unlink()


def monitor_template(resource):
    env = ''.join(f'        - name: {key}\n          value: "{value}"\n' for key, value in TELEMETRY.items())
    return compare.pod_template(resource, ipc_lock=bool(resource)).replace('      env:\n', '      env:\n' + env)


def verify_namespace(run, state):
    if not state.get('namespace_uid'):
        raise RuntimeError('Namespace UID was not recorded, inspect the namespace manually before cleanup')
    current = run.kube_json('get', 'namespace', state['namespace'])
    if current['metadata']['uid'] != state['namespace_uid']:
        raise RuntimeError('Namespace UID changed, refusing to operate on a replacement namespace')


def start_case(run, state, scenario, plan, image, duration):
    compare.check_memory_budget(plan, scenario, duration)
    addresses = compare.node_addresses(run, plan, scenario, ('mooncake',))
    run.addresses = addresses
    run.binary_path = compare.backend_binary(scenario, 'mooncake')
    compare.ensure_binary(run, image)
    compare.ensure_mooncake_rdma_image(run, image)
    effective = dict(scenario)
    effective.pop('writes_per_client', None)
    effective['execution'] = {'duration_seconds': duration, 'systems': ['mooncake'],
                              'write_limit': 'rc44 external wall-time stop', 'pipelines_by_system': {
                                  'mooncake': {'writer': ['put'], 'reader': ['get_buffer']}}}
    effective['monitoring'] = {'enabled': False, 'profile_client_cpu': False,
                               'interval_seconds': 5, 'environment': TELEMETRY,
                               'rdma_scope': 'shared HCA port, one store per node'}
    compare.write_json(run.output / 'scenario-effective.json', effective)
    compare.write_json(run.output / 'images.json', {'mooncake': image, 'pod_images': {'mooncake': image}})
    templates = run.output / 'templates'
    templates.mkdir()
    for name, resource in (('control', None), ('mooncake', scenario['mooncake_rdma']['resource'])):
        (templates / f'{name}.yaml').write_text(monitor_template(resource), encoding='utf-8')
    ns = state['namespace']
    run.command(['kubectl', 'create', 'namespace', ns])
    state['namespace_uid'] = run.kube_json('get', 'namespace', ns)['metadata']['uid']
    state['retained_namespaces'] = [ns]
    save_state(run.output, state)
    case = run.output / 'mooncake'
    case.mkdir()
    masters = compare.deploy_component(run, scenario, plan, ns, 'mooncake', 'master', image, templates)
    port = scenario['components']['mooncake']['master'].get('args', {}).get('rpc_port', 50051)
    service = next(iter(masters.values()))['status']['podIP'] + f':{port}'
    run.command([sys.executable, ROOT / 'deploy_mooncake.py', 'start', '--role', 'master',
                 '-n', ns, '-p', 'mc-master-', '--port', str(port), '--timeout', '120',
                 *compare.mooncake_master_args(scenario, case)], timeout=180)
    memory = scenario['memory_gib']
    stores = compare.deploy_component(run, scenario, plan, ns, 'mooncake', 'store', image, templates)
    clients = compare.deploy_component(run, scenario, plan, ns, 'mooncake', 'client', image, templates)
    devices = {}
    for name in [*stores, *clients]:
        run.command(['kubectl', '-n', ns, 'cp', ROOT / 'tools' / 'mooncake' / 'rdma_device.py',
                     name + ':/tmp/rdma_device.py'], timeout=60)
        result = run.command(['kubectl', '-n', ns, 'exec', name, '--', 'python3',
                              '/tmp/rdma_device.py', '--device-name=' + compare.rdma_device_preference(scenario, (stores | clients)[name]),
                              '--require-ipc-lock'],
                             capture=True, timeout=30)
        devices[name] = result.strip().splitlines()[-1]
    compare.write_json(case / 'rdma-devices.json', devices)
    run.command([sys.executable, ROOT / 'deploy_mooncake.py', 'start', '--role', 'store',
                 '-n', ns, '-p', 'mc-store-', '--master', service, '--protocol', 'rdma',
                 '--segment-size', str(memory['mooncake_store'] * 1024**3),
                 '--local-buffer-size', str(memory['mooncake_client_buffer'] * 1024**3),
                 '--devices-json', case / 'rdma-devices.json', '--timeout', '180',
                 *compare.mooncake_store_env_args(scenario, case)], timeout=240)
    deploy, _ = compare.configure_clients(run, scenario, plan, ns, 'mc-client-', 'mooncake',
                                          case, service, duration, devices)
    data = json.loads(deploy.read_text())
    data['kvtest_binary_path'] = str(run.binary_path)
    compare.write_json(deploy, data)
    compare.collect_pod_snapshot(run, ns, case)
    compare.collect_startup_snapshot(run, ns, case)


def run_case(run, state, plan):
    verify_namespace(run, state)
    case = run.output / 'mooncake'
    session = (MonitorSession(state['namespace'], case, max_seconds=1800, host_rdma=state.get('host_rdma'))
               if state.get('monitor_enabled', False) else None)
    try:
        if session is not None:
            session.start()
        else:
            print('External CPU/RDMA monitoring disabled (use --monitor to enable)', flush=True)
        state['client_deploy_started_at'] = utc_now()
        save_state(run.output, state)
        run.command([sys.executable, ROOT / 'deploy_client.py', 'deploy', case / 'deploy.json',
                     case / 'config.json'], cwd=case, timeout=900)
        client_started = compare.time.monotonic()
        if state.get('profile_client_cpu'):
            capture_client_thread_cpu(run, state['namespace'], case)
        state['writer_wait_started_at'] = utc_now()
        save_state(run.output, state)
        compare.wait_pipeline_writers(case, plan, client_started)
        state['client_stop_started_at'] = utc_now()
        save_state(run.output, state)
        run.command([sys.executable, ROOT / 'deploy_client.py', 'stop', case / 'deploy.json',
                     case / 'config.json', '--stop-timeout', '60'], cwd=case, timeout=180)
    finally:
        if session is not None:
            session.stop()


def collect_case(run, state, plan):
    verify_namespace(run, state)
    case, ns = run.output / 'mooncake', state['namespace']
    run.command([sys.executable, ROOT / 'deploy_client.py', 'collect', case / 'deploy.json',
                 case / 'config.json', '--summary-timeout', '0', '-o', case / 'collected'],
                cwd=case, timeout=300)
    for role in ('master', 'store'):
        run.command([sys.executable, ROOT / 'deploy_mooncake.py', 'collect', '--role', role,
                     '-n', ns, '-p', f'mc-{role}-', '-o', case / f'{role}-logs'], timeout=180)
    evidence = case / 'mooncake-internal-metrics.log'
    with evidence.open('w', encoding='utf-8') as target:
        logs = sorted((case / 'collected').rglob('run.log')) + sorted((case / 'store-logs').glob('*.log'))
        for log in logs:
            with log.open(encoding='utf-8', errors='replace') as source:
                for line in source:
                    if any(marker in line for marker in ('[Metrics]', 'Client Metrics', 'client metrics',
                                                         'metrics enabled', 'Metrics reporting')):
                        target.write(str(log.relative_to(case)) + ': ' + line)
    summaries = compare.collect_and_check(case, plan)
    state['cases'] = [{'backend': 'mooncake', 'namespace': ns,
                       'summaries': [str(p.relative_to(run.output)) for p in summaries]}]


def delete_case(run, state):
    verify_namespace(run, state)
    run.command(['kubectl', 'delete', 'namespace', state['namespace'], '--wait=true', '--timeout=300s'], timeout=330)
    state['retained_namespaces'] = []


def execute(args, parser):
    output = args.output.resolve()
    if args.action == 'start':
        if output.exists():
            parser.error('start requires a new output directory, use another --output')
        if not args.mc_image or not 0 < args.duration <= 60:
            parser.error('start requires --mc-image and duration between 1 and 60 seconds')
        scenario = json.loads(args.scenario.read_text(encoding='utf-8'))
        if args.client_cpu_limit is not None:
            scenario['client_cpu_limit'] = args.client_cpu_limit
        try:
            if args.target_qps is not None:
                scenario['target_qps'] = {compare.scenario_size(scenario): [args.target_qps]}
            compare.client_cpu_resources(scenario)
            plan = compare.plan_from_scenario(scenario)
        except ValueError as error:
            parser.error(str(error))
        output.mkdir(parents=True)
        compare.write_json(output / 'scenario-input.json', scenario)
        state = {'status': 'starting', 'namespace': 'kvtest-mc-' + uuid.uuid4().hex[:10],
                 'duration_seconds': args.duration, 'cases': [], 'retained_namespaces': [],
                 'monitor_enabled': False, 'profile_client_cpu': False}
        save_state(output, state)
    else:
        state = json.loads((output / 'state.json').read_text())
        scenario = json.loads((output / 'scenario-input.json').read_text())
        plan = compare.plan_from_scenario(scenario)
        if args.action == 'run' and state['status'] != 'started':
            parser.error('run requires a successfully started, unused case, create a new case for another measurement')
        if args.action == 'run' and args.host_rdma_json is not None:
            state['host_rdma'] = validate_host_rdma(
                json.loads(args.host_rdma_json.read_text(encoding='utf-8')), plan.nodes)
        if args.action == 'collect' and state['status'] not in ('ran', 'collected', 'failed'):
            parser.error('collect requires a finished or failed run')
        if args.action == 'delete' and state['status'] == 'deleted':
            print('Already deleted:', state['namespace'])
            return 0
    run = resume_run(output)
    try:
        if args.action == 'start':
            start_case(run, state, scenario, plan, args.mc_image, args.duration)
        elif args.action == 'run':
            state['status'] = 'running'
            state['monitor_enabled'] = args.monitor or args.host_rdma_json is not None
            state['profile_client_cpu'] = args.profile_client_cpu
            save_state(output, state)
            effective_path = output / 'scenario-effective.json'
            if effective_path.is_file():
                effective = json.loads(effective_path.read_text(encoding='utf-8'))
                effective.setdefault('monitoring', {}).update(
                    enabled=state['monitor_enabled'], profile_client_cpu=state['profile_client_cpu'])
                compare.write_json(effective_path, effective)
            run_case(run, state, plan)
        elif args.action == 'collect':
            collect_case(run, state, plan)
        else:
            delete_case(run, state)
        state['status'] = {'start': 'started', 'run': 'ran', 'collect': 'collected', 'delete': 'deleted'}[args.action]
        state.pop('error', None)
        save_state(output, state)
        if args.action == 'collect':
            compare.render_report(output, plan, ('mooncake',))
            print('HTML report:', output / 'report.html')
        print(f"{args.action}: OK\nNamespace: {state['namespace']}\nResults: {output}")
        return 0
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired, KeyboardInterrupt) as error:
        state['status'], state['error'] = 'failed', str(error)
        save_state(output, state)
        print(f'Failed: {error}\nPods retained. Results: {output}', file=sys.stderr)
        return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('start', 'run', 'collect', 'delete'))
    parser.add_argument('--output', type=Path, required=True, help='same result directory for all four commands')
    parser.add_argument('--scenario', type=Path, default=compare.SCENARIO)
    parser.add_argument('--mc-image', help='required for start')
    parser.add_argument('--duration', type=int, default=60)
    parser.add_argument('--target-qps', type=int,
                        help='start only: override per-client write QPS (positive integer)')
    parser.add_argument('--client-cpu-limit', type=int,
                        help='start only: client CPU limit, retaining scenario cpus.client as the CPU request')
    parser.add_argument('--monitor', action='store_true',
                        help='run only: enable external CPU/cgroup and shared RDMA sampling (default: off)')
    parser.add_argument('--profile-client-cpu', action='store_true',
                        help='run only: sample one live client for 10 seconds and save thread-cpu.json')
    parser.add_argument('--host-rdma-json', type=Path,
                        help='run only: host RDMA mapping, implies --monitor')
    args = parser.parse_args(argv)
    if args.target_qps is not None and args.action != 'start':
        parser.error('--target-qps applies only to start, use a new case to change workload')
    if args.client_cpu_limit is not None and args.action != 'start':
        parser.error('--client-cpu-limit applies only to start, use a new case to change Pod resources')
    if args.profile_client_cpu and args.action != 'run':
        parser.error('--profile-client-cpu applies only to run')
    if args.monitor and args.action != 'run':
        parser.error('--monitor applies only to run')
    if args.host_rdma_json is not None and args.action != 'run':
        parser.error('--host-rdma-json applies only to run')
    try:
        with phase_lock(args.output.resolve()):
            return execute(args, parser)
    except (OSError, RuntimeError, ValueError) as error:
        print(f'Failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
