#!/usr/bin/env python3
"""Run fixed two-node comparison suites with reference configs and whl deployment."""
import argparse
import copy
from datetime import datetime, timezone
from html import escape
import json
from pathlib import Path
import subprocess
import sys
import uuid


ROOT = Path(__file__).resolve().parents[1]
CASES = (('1B', 17, 2), ('1KB', 17, 2), ('8KB', 17, 2),
         ('8KB', 105, 1), ('256KB', 17, 2), ('1MB', 17, 2))
SCALING_CASES = (('8MB', 17, 1, 2), ('8KB', 1000, 1, 1), ('8KB', 1000, 4, 4),
                 ('8KB', 1000, 8, 8), ('8KB', 1000, 16, 16))
LOGICAL_HOST_CASES = tuple(('4MB', 17, workers, workers * 2) for workers in (2, 4, 8, 16))


def write_index(output, jobs):
    rows = []
    for name, _ in jobs:
        label = escape(name)
        if (output / name / 'report.html').is_file():
            label = f'<a href="{name}/report.html">{label}</a>'
        else:
            label += '（尚无报告）'
        rows.append('<li>' + label + '</li>')
    (output / 'index.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>批量对比</title>'
        f'<h1>{len(jobs)}组 DataSystem / Mooncake 对比</h1><ul>' + ''.join(rows) + '</ul>',
        encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', choices=('six', 'worker-scaling', 'logical-hosts'), default='six')
    parser.add_argument('--source-scenario', type=Path,
                        help='reuse only artifacts from an existing scenario, excluding workload and JF settings')
    parser.add_argument('--ds-whl', type=Path, required=True)
    parser.add_argument('--mc-image', default='mooncake-store-kvtest:official-v0.3.13.post1-rdma-event')
    parser.add_argument('--duration', type=int, help='seconds per system: default 180, worker-scaling defaults to 150')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    scaling = args.suite == 'worker-scaling'
    logical_hosts = args.suite == 'logical-hosts'
    if args.duration is None:
        args.duration = 150 if scaling else 180
    whl = args.ds_whl.resolve()
    if not whl.is_file():
        parser.error('whl not found: ' + str(whl))
    if args.duration <= 0:
        parser.error('--duration must be positive')

    base = json.loads((ROOT / 'comparison/scenario.2-nodes-1clients.json').read_text(encoding='utf-8'))
    if args.source_scenario:
        artifacts = json.loads(args.source_scenario.read_text(encoding='utf-8')).get('artifacts', {})
        if not isinstance(artifacts, dict):
            parser.error('source scenario artifacts must be an object')
        base.setdefault('artifacts', {}).update(artifacts)
    base.setdefault('artifacts', {})['datasystem_whl'] = str(whl)
    base['datasystem_deployment'] = {'mode': 'dscli'}
    ds = base['components']['datasystem']
    for role in ('worker', 'coordinator'):
        if not ds[role].get('preserve_config_template'):
            parser.error(f'{role} must preserve the reference config template')
    options = ds['client'].setdefault('config', {})
    options.pop('request_timeout_seconds', None)
    options.update(service_discovery='coordinator', connect_timeout_ms=1000, request_timeout_ms=20,
                   enable_local_cache=False, enable_cross_node_connection=True,
                   data_placement_policy='PREFERRED_META_OWNER', fast_transport_mem_size='512MB')
    ds['client'].setdefault('env', {}).update(DATASYSTEM_UB_GET_DATA_SIZE_BYTES='10485760',
                                             DATASYSTEM_ENABLE_LOAD_AWARE_SCHEDULER='true')
    if logical_hosts:
        ds['worker']['config']['host_id_env_name'] = 'HOST_IP'
        options['host_id_env_name'] = 'HOST_IP'
    if scaling or logical_hosts:
        base['memory_gib'].update(mooncake_store=base['memory_gib']['datasystem_worker'],
                                  datasystem_pod=80, mooncake_pod=80)
        for backend, role in (('datasystem', 'worker'), ('mooncake', 'store')):
            base['components'][backend][role]['resources'] = {
                'cpu': 32 if logical_hosts else 16, 'requests_cpu': 8, 'memory_gib': 80, 'requests_memory_gib': 64}
            base['components'][backend]['client']['resources'] = {
                'cpu': 32, 'requests_cpu': 4, 'memory_gib': 20, 'requests_memory_gib': 4}

    tag = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:6]
    prefix = ('compare-logical-hosts-whl-' if logical_hosts else
              'compare-worker-scaling-whl-' if scaling else 'compare-six-cases-whl-')
    output = (args.output or ROOT / 'results' / (prefix + tag)).resolve()
    configs = output / 'scenarios'
    output.mkdir(parents=True, exist_ok=False)
    configs.mkdir()
    nodes = ('worker02', 'worker03')
    cases = SCALING_CASES if scaling else tuple((size, qps, 1, clients) for size, qps, clients in CASES)
    if logical_hosts:
        cases = LOGICAL_HOST_CASES
    jobs = []
    for index, (size, qps, workers, clients) in enumerate(cases, 1):
        name = f'{index:02d}-{size.lower()}-{qps}qps-{clients}clients-per-node'
        if scaling or logical_hosts:
            name += f'-{workers}workers-per-node'
        if logical_hosts:
            name += f'-{workers * len(nodes)}logical-hosts'
        scenario = copy.deepcopy(base)
        scenario.update(control_node='master', nodes=list(nodes), clients_per_node=clients,
                        systems=['datasystem', 'mooncake'], sizes=[size], target_qps={size: [qps]},
                        writer_threads=4, reader_threads=8, notify_count=1, reads_per_notify=1,
                        notify_include_self=False, client_numa_node=None)
        scenario['deployment'] = {
            'mode': 'kubernetes', 'clients': dict.fromkeys(nodes, clients),
            'datasystem': {'coordinator': {'master': 1}, 'worker': dict.fromkeys(nodes, workers)},
            'mooncake': {'master': {'master': 1}, 'store': dict.fromkeys(nodes, workers)}}
        scenario.pop('writes_per_client', None)
        if logical_hosts:
            scenario['logical_hosts'] = True
        path = configs / (name + '.json')
        path.write_text(json.dumps(scenario, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        jobs.append((name, path))
    write_index(output, jobs)
    print('部署模式: whl + dscli，无 JF standalone', flush=True)
    print('client: 4写/8读，随机NUMA，1写1读，接口保持原样', flush=True)
    print('每组每系统运行秒数:', args.duration, flush=True)
    print('结果与配置目录:', output, flush=True)
    if scaling or logical_hosts:
        sys.path.insert(0, str(ROOT))
        from deploy_compare_ub_rdma import Run, check_memory_budget, node_addresses, plan_from_scenario
        for _, path in jobs:
            scenario = json.loads(path.read_text(encoding='utf-8'))
            check_memory_budget(plan_from_scenario(scenario), scenario, args.duration)
        storage_cpu = 32 if logical_hosts else 16
        print(f'worker/store: 每实例60GiB池，Pod CPU request/limit=8/{storage_cpu}，内存request/limit=64/80GiB', flush=True)
        print('client: Pod CPU request/limit=4/32，内存request/limit=4/20GiB', flush=True)
        if logical_hosts:
            print('2台物理机模拟4/8/16/32个逻辑节点，每组1worker/store+2client；保留原有传输策略', flush=True)
            print('DS同组HOST_IP=node-XX，host_id_env_name=HOST_IP；MC仅匹配实例数量，不注入DS主机标识', flush=True)
            print('最大档每节点需要48份UB/RDMA空闲配额，存储与client合计请求256CPU、1152GiB内存', flush=True)
        else:
            print('最大档每节点需要32份UB/RDMA空闲配额，存储与client合计请求192CPU、1088GiB内存', flush=True)
        node_addresses(Run(output / 'preflight'), plan_from_scenario(scenario), scenario)
    for name, scenario in jobs:
        print('\n===== ' + name + ' =====', flush=True)
        command = [sys.executable, '-u', str(ROOT / 'deploy_compare_ub_rdma.py'),
                   '--scenario', str(scenario), '--systems', 'both', '--duration', str(args.duration),
                   '--ds-whl', str(whl), '--mc-image', args.mc_image, '--output', str(output / name)]
        result = subprocess.run(command, cwd=ROOT)
        write_index(output, jobs)
        if result.returncode:
            print(f'失败，停止后续测试并保留现场: {output / name}', file=sys.stderr, flush=True)
            return result.returncode if result.returncode > 0 else 1
    print('\n全部完成，报告入口:', output / 'index.html', flush=True)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        raise SystemExit(str(error))
