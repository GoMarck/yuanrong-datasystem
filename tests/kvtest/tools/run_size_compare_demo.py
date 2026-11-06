#!/usr/bin/env python3
"""Record five two-node size comparisons with measured latency reductions."""
import argparse
import copy
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from deploy_compare_ub_rdma import check_memory_budget, plan_from_scenario
from ds_config_template import render_ds_config
from kvtest_comparison_demo import ComparisonConsole, ComparisonProgress, read_comparison_result, save_comparison_summary
from kvtest_scenario import NODE_NAME
from run_ds_node_demo import run_case


SIZES = ('4MB', '6MB', '8MB', '16MB', '32MB')


def make_scenarios(source, nodes, control, whl, image, duration, *, reference_dir):
    if (len(nodes) != 2 or len(set(nodes)) != 2
            or any(not isinstance(n, str) or not NODE_NAME.fullmatch(n) for n in [*nodes, control])):
        raise ValueError('请配置两个不同发压节点与有效控制节点名称')
    base = json.loads((ROOT / 'comparison/scenario.2-nodes-1clients.json').read_text(encoding='utf-8'))
    base.setdefault('artifacts', {}).update(source.get('artifacts', {}))
    base['artifacts'].update(datasystem_whl=str(whl), pod_image=image,
                             pod_images={'datasystem': image, 'mooncake': image})
    base['ub_resource'] = source.get('ub_resource', base['ub_resource'])
    base['mooncake_rdma'] = copy.deepcopy(source.get('mooncake_rdma', base['mooncake_rdma']))
    base['datasystem_deployment'] = {'mode': 'dscli'}
    base.update(control_node=control, nodes=nodes, clients_per_node=2,
                systems=['datasystem', 'mooncake'], writer_threads=4, reader_threads=8,
                notify_count=1, reads_per_notify=1, notify_include_self=False, client_numa_node=None)
    base.pop('writes_per_client', None)
    base['memory_gib'].update(datasystem_worker=256, mooncake_store=256, datasystem_pod=288, mooncake_pod=288)
    worker = base['components']['datasystem']['worker']
    reference_text = (ROOT / worker['config_file']).read_bytes().decode('utf-8')
    reference = json.loads(reference_text)
    value = reference['shared_memory_size_mb']['value']
    reference['shared_memory_size_mb']['value'] = str(262144) if isinstance(value, str) else 262144
    reference_dir.mkdir(parents=True, exist_ok=True)
    worker_reference = (reference_dir / 'worker.reference.json').resolve()
    worker_reference.write_bytes(render_ds_config(reference_text, reference, {'shared_memory_size_mb'}).encode('utf-8'))
    worker['config_file'] = str(worker_reference)
    options = base['components']['datasystem']['client']['config']
    options.pop('request_timeout_seconds', None)
    options.update(connect_timeout_ms=1000, request_timeout_ms=20, enable_local_cache=False,
                   enable_cross_node_connection=True, data_placement_policy='PREFERRED_META_OWNER',
                   fast_transport_mem_size='512MB')
    for backend, storage in (('datasystem', 'worker'), ('mooncake', 'store')):
        base['components'][backend][storage]['resources'] = {
            'cpu': 32, 'requests_cpu': 32, 'memory_gib': 288, 'requests_memory_gib': 272}
        base['components'][backend]['client']['resources'] = {
            'cpu': 32, 'requests_cpu': 4, 'memory_gib': 20, 'requests_memory_gib': 4}
    base['deployment'] = {'mode': 'kubernetes', 'clients': dict.fromkeys(nodes, 2),
                          'datasystem': {'coordinator': {control: 1}, 'worker': dict.fromkeys(nodes, 1)},
                          'mooncake': {'master': {control: 1}, 'store': dict.fromkeys(nodes, 1)}}
    base['run'] = {'duration_seconds': duration, 'cleanup_on_success': True}
    scenarios = []
    for size in SIZES:
        scenario = copy.deepcopy(base)
        scenario.update(sizes=[size], target_qps={size: [17]})
        check_memory_budget(plan_from_scenario(scenario), scenario, duration)
        scenarios.append(scenario)
    return scenarios


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-scenario', type=Path, default=ROOT / 'comparison/scenario.2-nodes-1clients.json')
    parser.add_argument('--nodes', nargs=2)
    parser.add_argument('--control-node')
    parser.add_argument('--ds-whl', type=Path, required=True)
    parser.add_argument('--mc-image', default='mooncake-store-kvtest:official-v0.3.13.post1-rdma-event')
    parser.add_argument('--duration', type=int, default=120)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--plain', action='store_true')
    parser.add_argument('--theme', choices=('light', 'dark'), default='light')
    args = parser.parse_args()
    if args.duration <= 0 or not args.ds_whl.is_file() or not args.mc_image.strip():
        parser.error('duration必须为正数，ds-whl必须是已存在的文件，镜像名不能为空')
    source = json.loads(args.source_scenario.read_text(encoding='utf-8'))
    tag = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:6]
    output = (args.output or ROOT / 'results' / ('size-compare-demo-' + tag)).resolve()
    output.mkdir(parents=True, exist_ok=False)
    for name in ('scenarios', 'logs'):
        (output / name).mkdir()
    scenarios = make_scenarios(source, args.nodes or source['nodes'],
                               args.control_node or source.get('control_node', 'master'),
                               args.ds_whl.resolve(), args.mc_image, args.duration, reference_dir=output / 'scenarios')
    rows = [{'size': size, 'status': 'pending', 'directory': f'{index:02d}-{size.lower()}'}
            for index, size in enumerate(SIZES, 1)]
    for row, scenario in zip(rows, scenarios):
        (output / 'scenarios' / (row['directory'] + '.json')).write_text(
            json.dumps(scenario, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    save_comparison_summary(output, rows, args.duration, 'prepared')
    if args.prepare_only:
        print('5档配置已生成:', output / 'scenarios')
        return 0
    console = ComparisonConsole(rows, args.duration, 17, args.plain, args.theme)
    index, status, failure = 0, 'running', None
    try:
        for index, row in enumerate(rows):
            row['status'] = 'running'
            save_comparison_summary(output, rows, args.duration, status)
            command = [sys.executable, '-u', str(ROOT / 'deploy_compare_ub_rdma.py'),
                       '--scenario', str(output / 'scenarios' / (row['directory'] + '.json')),
                       '--systems', 'both', '--duration', str(args.duration),
                       '--ds-whl', str(args.ds_whl.resolve()), '--mc-image', args.mc_image,
                       '--output', str(output / row['directory'])]
            code = run_case(command, output / 'logs' / (row['directory'] + '.log'), console, index,
                            ComparisonProgress(args.duration))
            if code:
                state_file = output / row['directory'] / 'state.json'
                reason = f'控制器退出码{code}'
                if state_file.is_file():
                    reason = json.loads(state_file.read_text(encoding='utf-8')).get('error') or reason
                raise RuntimeError(reason)
            rows[index] = read_comparison_result(output / row['directory'], row)
            save_comparison_summary(output, rows, args.duration, status)
        status = 'complete'
        return 0
    except KeyboardInterrupt:
        failure, status = '用户中断', 'interrupted'
        rows[index].update(status=status, error=failure)
        return 130
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        failure, status = str(error), 'failed'
        rows[index].update(status=status, error=failure)
        return 1
    finally:
        try:
            save_comparison_summary(output, rows, args.duration, status, failure)
            console.draw(index, status={'complete': '全部完成', 'failed': '运行失败',
                                        'interrupted': '已中断'}.get(status, '运行结束'), final=True)
        finally:
            console.close()
        if failure:
            print('本档未完成，原因与保留现场见完整日志。')
        print('汇总报告:', output / 'summary.html')
        print('完整日志:', output / 'logs')


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit(str(error))
