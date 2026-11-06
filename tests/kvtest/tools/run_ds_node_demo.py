#!/usr/bin/env python3
"""Run DataSystem node-count cases with a recording-friendly terminal display."""
import argparse
import contextlib
import copy
from datetime import datetime, timezone
from decimal import Decimal
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from deploy_compare_ub_rdma import Run, check_memory_budget, component_resources, node_addresses, plan_from_scenario
from kvtest_demo import Console, Progress, read_result, save_summary
from kvtest_scenario import NODE_NAME

COUNTS = (2, 4, 8, 16, 32, 64, 128, 256, 512)


def load_nodes(path, source):
    if path:
        text = path.read_text(encoding='utf-8-sig')
        try:
            value = json.loads(text)
        except json.JSONDecodeError:
            value = [line.strip() for line in text.splitlines() if line.strip() and not line.lstrip().startswith('#')]
        if isinstance(value, dict) and isinstance(value.get('items'), list):
            value = sorted(item['metadata']['name'] for item in value['items'])
    else:
        value = list(source.get('deployment', {}).get('clients', {})) or source.get('nodes')
    if (not isinstance(value, list) or not 2 <= len(value) <= 32
            or any(not isinstance(node, str) or not NODE_NAME.fullmatch(node) for node in value)
            or len(set(value)) != len(value)):
        raise ValueError('需要2到32个不同节点名，请通过 --nodes-file 提供清单')
    return value


def make_scenarios(source, nodes, control, whl, duration, counts=COUNTS):
    base = json.loads((ROOT / 'comparison/scenario.2-nodes-1clients.json').read_text(encoding='utf-8'))
    base.setdefault('artifacts', {}).update(source.get('artifacts', {}))
    base['artifacts']['datasystem_whl'] = str(whl)
    base['ub_resource'] = source.get('ub_resource', base['ub_resource'])
    base['datasystem_deployment'] = {'mode': 'dscli'}
    base.update(systems=['datasystem'], control_node=control, logical_hosts=True,
                sizes=['2MB'], target_qps={'2MB': [17]}, writer_threads=4, reader_threads=8,
                notify_count=1, reads_per_notify=1, notify_include_self=False, client_numa_node=None)
    base.pop('writes_per_client', None)
    base['memory_gib']['datasystem_pod'] = 80
    ds = base['components']['datasystem']
    ds['worker']['config']['host_id_env_name'] = 'HOST_IP'
    config = ds['client']['config']
    config.pop('request_timeout_seconds', None)
    config.update(host_id_env_name='HOST_IP', service_discovery='coordinator', connect_timeout_ms=1000,
                  request_timeout_ms=20, enable_local_cache=False, enable_cross_node_connection=True,
                  data_placement_policy='PREFERRED_META_OWNER', fast_transport_mem_size='512MB')
    ds['client'].setdefault('env', {}).update(DATASYSTEM_UB_GET_DATA_SIZE_BYTES='10485760',
                                             DATASYSTEM_ENABLE_LOAD_AWARE_SCHEDULER='true')
    ds['worker']['resources'] = {'cpu': 32, 'requests_cpu': 8, 'memory_gib': 80, 'requests_memory_gib': 64}
    ds['client']['resources'] = {'cpu': 32, 'requests_cpu': 4, 'memory_gib': 20, 'requests_memory_gib': 4}
    scenarios = []
    for count in counts:
        chosen = nodes[:min(count, len(nodes))]
        per_host, extra = divmod(count, len(chosen))
        workers = {node: per_host + int(index < extra) for index, node in enumerate(chosen)}
        clients = {node: number * 2 for node, number in workers.items()}
        scenario = copy.deepcopy(base)
        scenario.update(nodes=chosen, clients_per_node=next(iter(clients.values())))
        scenario['deployment'] = {'mode': 'kubernetes', 'clients': clients,
                                  'datasystem': {'coordinator': {control: 1}, 'worker': workers}}
        scenario['run'] = {'duration_seconds': duration, 'cleanup_on_success': True,
                           'command_timeout_floor_seconds': max(900, count * 8)}
        plan = plan_from_scenario(scenario)
        check_memory_budget(plan, scenario, duration)
        scenarios.append(scenario)
    return scenarios


def quantity(value):
    match = re.fullmatch(r'([0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)([KMGTPE]i|[numkKMGTPE])?', str(value))
    if not match:
        raise ValueError(f'unsupported Kubernetes resource quantity: {value}')
    suffix = match[2] or ''
    if suffix.endswith('i'):
        multiplier = Decimal(1024) ** ('KMGTPE'.index(suffix[0]) + 1)
    else:
        powers = {'n': -9, 'u': -6, 'm': -3, '': 0, 'k': 3, 'K': 3, 'M': 6, 'G': 9, 'T': 12, 'P': 15, 'E': 18}
        multiplier = Decimal(10) ** powers[suffix]
    return Decimal(match[1]) * multiplier


def pod_request(pod, resource):
    spec = pod.get('spec', {})
    def request(container):
        resources = container.get('resources', {})
        return quantity(resources.get('requests', {}).get(resource, resources.get('limits', {}).get(resource, 0)))
    regular = sum((request(c) for c in spec.get('containers', [])), Decimal(0))
    init = max((request(c) for c in spec.get('initContainers', [])), default=Decimal(0))
    return max(regular, init) + quantity(spec.get('overhead', {}).get(resource, 0))


def preflight(run, scenario):
    plan = plan_from_scenario(scenario)
    node_addresses(run, plan, scenario, ('datasystem',))
    nodes = json.loads((run.output / 'nodes.json').read_text(encoding='utf-8'))
    pods = run.kube_json('get', 'pods', '-A')['items']
    for name, node in nodes.items():
        if node.get('spec', {}).get('unschedulable') or any(
                taint.get('effect') in ('NoSchedule', 'NoExecute') for taint in node.get('spec', {}).get('taints', [])):
            raise RuntimeError(f'{name}: 节点不可调度或有未容忍的调度污点')
        needed = {'cpu': 0, 'memory': 0, 'pods': 0}
        for role in ('coordinator', 'worker', 'client'):
            count = dict(plan.placement('datasystem', role)).get(name, 0)
            resources = component_resources(scenario, 'datasystem', role)
            needed['cpu'] += count * resources['requests_cpu']
            needed['memory'] += count * resources['requests_memory_gib'] * 1024**3
            needed['pods'] += count
        current = [p for p in pods if p.get('spec', {}).get('nodeName') == name
                   and p.get('status', {}).get('phase') not in ('Succeeded', 'Failed')]
        for resource in needed:
            used = len(current) if resource == 'pods' else sum((pod_request(p, resource) for p in current), Decimal(0))
            available = quantity(node['status']['allocatable'].get(resource, 0)) - used
            if available < needed[resource]:
                raise RuntimeError(f'{name}: {resource} free={available}, need={needed[resource]}')


def stop_controller(process):
    if process.poll() is not None:
        return
    if os.name == 'posix':
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGINT)
    else:
        process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        if os.name == 'posix':
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGTERM)
        else:
            process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            if os.name == 'posix':
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
            process.wait()


def run_case(command, log_path, console, index, progress):
    errors = []
    with log_path.open('w', encoding='utf-8') as log:
        process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding='utf-8', errors='replace',
                                   start_new_session=os.name == 'posix')
        def relay():
            try:
                for line in process.stdout:
                    log.write(line)
                    log.flush()
                    progress.consume(line)
            except Exception as error:
                errors.append(error)
        reader = threading.Thread(target=relay, daemon=True)
        try:
            reader.start()
            while process.poll() is None:
                console.draw(index, progress)
                if errors:
                    raise RuntimeError(f'日志采集失败: {errors[0]}')
                time.sleep(0.25)
            reader.join(timeout=10)
            if reader.is_alive():
                raise RuntimeError('控制器输出未关闭，见运行日志')
            if errors:
                raise RuntimeError(f'日志采集失败: {errors[0]}')
            return process.returncode
        finally:
            if process.poll() is None:
                stop_controller(process)
            if reader.is_alive() and os.name == 'posix':
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(process.pid, signal.SIGTERM)
            if reader.ident is not None:
                reader.join(timeout=5)
            if not reader.is_alive():
                process.stdout.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-scenario', type=Path, default=ROOT / 'comparison/scenario.2-nodes-1clients.json')
    parser.add_argument('--nodes-file', type=Path, help='node names or kubectl get nodes -o json output')
    parser.add_argument('--counts', nargs='+', type=int, choices=COUNTS,
                        help='selected node-count cases, e.g. --counts 2 4')
    parser.add_argument('--control-node')
    parser.add_argument('--ds-whl', type=Path, required=True)
    parser.add_argument('--pod-image', default='mooncake-store-kvtest:official-v0.3.13.post1-rdma-event')
    parser.add_argument('--duration', type=int, default=180)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--plain', action='store_true')
    parser.add_argument('--theme', choices=('light', 'dark'), default='light', help='terminal accent colors (default: light)')
    args = parser.parse_args()
    if args.duration <= 0 or not args.ds_whl.is_file():
        parser.error('duration必须为正数，ds-whl必须是已存在的文件')
    source = json.loads(args.source_scenario.read_text(encoding='utf-8'))
    nodes = load_nodes(args.nodes_file, source)
    if args.counts is None and len(nodes) != 32:
        parser.error('完整九档默认需要32个节点；小规模试跑请指定 --counts 2 4')
    counts = tuple(sorted(args.counts or COUNTS))
    if len(set(counts)) != len(counts):
        parser.error('--counts 不允许重复档位')
    control = args.control_node or source.get('control_node', 'master')
    scenarios = make_scenarios(source, nodes, control, args.ds_whl.resolve(), args.duration, counts)
    tag = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:6]
    output = (args.output or ROOT / 'results' / ('ds-node-demo-' + tag)).resolve()
    output.mkdir(parents=True, exist_ok=False)
    for name in ('scenarios', 'logs'):
        (output / name).mkdir()
    rows = [{'nodes': count, 'size': '2MB', 'status': 'pending', 'directory': f'{index:02d}-nodes-{count:03d}'}
            for index, count in enumerate(counts, 1)]
    for row, scenario in zip(rows, scenarios):
        (output / 'scenarios' / (row['directory'] + '.json')).write_text(
            json.dumps(scenario, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    save_summary(output, rows, args.duration, 17, 'prepared')
    if args.prepare_only:
        print(f'{len(rows)}档配置已生成:', output / 'scenarios')
        return 0
    console = Console(rows, args.duration, 17, args.plain, args.theme)
    index = 0
    status = 'running'
    failure = None
    try:
        console.draw(index, status='资源预检')
        with (output / 'logs/preflight.log').open('w', encoding='utf-8') as log:
            with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
                preflight(Run(output / 'preflight'), scenarios[-1])
        for index, row in enumerate(rows):
            row['status'] = 'running'
            save_summary(output, rows, args.duration, 17, status)
            command = [sys.executable, '-u', str(ROOT / 'deploy_compare_ub_rdma.py'),
                       '--scenario', str(output / 'scenarios' / (row['directory'] + '.json')),
                       '--systems', 'datasystem', '--duration', str(args.duration),
                       '--ds-whl', str(args.ds_whl.resolve()), '--mc-image', args.pod_image,
                       '--output', str(output / row['directory'])]
            code = run_case(command, output / 'logs' / (row['directory'] + '.log'), console, index,
                            Progress(row['nodes'], args.duration))
            if code:
                state_file = output / row['directory'] / 'state.json'
                reason = f'控制器退出码{code}'
                if state_file.is_file():
                    try:
                        reason = json.loads(state_file.read_text(encoding='utf-8')).get('error') or reason
                    except (OSError, ValueError):
                        pass
                raise RuntimeError(f'节点数{row["nodes"]}档失败：{reason}')
            rows[index] = read_result(output / row['directory'], row)
            save_summary(output, rows, args.duration, 17, status)
        status = 'complete'
        return 0
    except KeyboardInterrupt:
        failure = '用户中断'
        if rows[index]['status'] == 'running':
            rows[index].update(status='interrupted', error=failure)
        status = 'interrupted'
        return 130
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        failure = str(error)
        if rows[index]['status'] == 'running':
            rows[index].update(status='failed', error=failure)
        status = 'failed'
        return 1
    finally:
        try:
            save_summary(output, rows, args.duration, 17, status, failure)
            console.draw(index, status={'complete': '全部完成', 'failed': '运行失败', 'interrupted': '已中断'}.get(status, '运行结束'), final=True)
        finally:
            console.close()
        if status != 'complete':
            print('原因:', failure)
        print('汇总报告:', output / 'summary.html')
        print('完整日志:', output / 'logs')


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit(str(error))
