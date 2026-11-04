#!/usr/bin/env python3
"""Run the six approved two-node cases with reference configs and whl deployment."""
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
        '<!doctype html><meta charset="utf-8"><title>六组对比</title>'
        '<h1>六组 DataSystem / Mooncake 对比</h1><ul>' + ''.join(rows) + '</ul>',
        encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-scenario', type=Path,
                        help='reuse only artifacts from an existing scenario, excluding workload and JF settings')
    parser.add_argument('--ds-whl', type=Path, required=True)
    parser.add_argument('--mc-image', default='mooncake-store-kvtest:official-v0.3.13.post1-rdma-event')
    parser.add_argument('--duration', type=int, default=180, help='seconds per system in each case')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
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

    tag = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:6]
    output = (args.output or ROOT / 'results' / ('compare-six-cases-whl-' + tag)).resolve()
    configs = output / 'scenarios'
    output.mkdir(parents=True, exist_ok=False)
    configs.mkdir()
    nodes = ('worker02', 'worker03')
    jobs = []
    for index, (size, qps, clients) in enumerate(CASES, 1):
        name = f'{index:02d}-{size.lower()}-{qps}qps-{clients}clients-per-node'
        scenario = copy.deepcopy(base)
        scenario.update(control_node='master', nodes=list(nodes), clients_per_node=clients,
                        systems=['datasystem', 'mooncake'], sizes=[size], target_qps={size: [qps]},
                        writer_threads=4, reader_threads=8, notify_count=1, reads_per_notify=1,
                        notify_include_self=False, client_numa_node=None)
        scenario['deployment'] = {
            'mode': 'kubernetes', 'clients': dict.fromkeys(nodes, clients),
            'datasystem': {'coordinator': {'master': 1}, 'worker': dict.fromkeys(nodes, 1)},
            'mooncake': {'master': {'master': 1}, 'store': dict.fromkeys(nodes, 1)}}
        scenario.pop('writes_per_client', None)
        path = configs / (name + '.json')
        path.write_text(json.dumps(scenario, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        jobs.append((name, path))
    write_index(output, jobs)
    print('部署模式: whl + dscli，无 JF standalone', flush=True)
    print('client: 4写/8读，随机NUMA，1写1读，接口保持原样', flush=True)
    print('结果与配置目录:', output, flush=True)
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
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit(str(error))
