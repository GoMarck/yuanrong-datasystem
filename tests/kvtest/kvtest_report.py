#!/usr/bin/env python3
"""Render an offline HTML report from saved deploy_compare_ub_rdma results."""
import argparse
from collections import Counter, defaultdict
from datetime import datetime
from html import escape
import json
import shlex
from pathlib import Path
from urllib.parse import quote


LABELS = {'datasystem': 'DataSystem UB', 'mooncake': 'Mooncake RDMA'}
ROLES = ('Coordinator', 'Worker', 'Master', 'Store', 'Client', 'JF')
PREFIXES = {'ds-coordinator-': 'Coordinator', 'ds-worker-': 'Worker',
            'ds-client-': 'Client', 'mc-master-': 'Master',
            'mc-store-': 'Store', 'mc-client-': 'Client', 'ds-jf-': 'JF'}


def api_operations(metrics):
    if 'put' in metrics:
        return ('put', 'get_buffer')
    return ('setStringView', 'getBuffer') if 'setStringView' in metrics else ('setBuffer', 'getBuffer')


def api_label(backend, operation):
    if backend == 'mooncake':
        return {'setBuffer': 'put', 'getBuffer': 'get_buffer'}.get(operation, operation)
    if operation == 'setStringView':
        return 'Set'
    return operation


def api_boundary_note(backend, metrics):
    if backend == 'mooncake':
        return ('Mooncake put 使用已有 payload，get_buffer 返回持有所有权的数据缓冲区，'
                'put 不实现 10 秒 TTL。')
    if api_operations(metrics)[0] == 'setStringView':
        return 'DataSystem Set(key, StringView(data), param) 计时包含该 SDK 调用内部的写入处理，读取使用 Get(Buffer)。'
    return '旧 DataSystem setBuffer 样本仅计时 Set(Buffer)，不含 Create，memoryCopy 为 no-op。'


def read_json(path, default=None):
    return json.loads(path.read_text(encoding='utf-8')) if path.is_file() else default


def table(headers, rows):
    return ('<div class="scroll"><table><thead><tr>'
            + ''.join(f'<th>{escape(str(value))}</th>' for value in headers)
            + '</tr></thead><tbody>'
            + ''.join('<tr>' + ''.join(f'<td>{escape(str(value))}</td>' for value in row)
                      + '</tr>' for row in rows) + '</tbody></table></div>')


def render_matrix_report(output, state):
    parts = ['<!doctype html><html lang="zh-CN"><meta charset="utf-8">',
             '<meta name="viewport" content="width=device-width,initial-scale=1">',
             '<title>KVTest · 多档位对比</title><style>', CSS, '</style>',
             '<header><h1>KVTest 多档位对比</h1></header><main>',
             '<p>每个数据大小和 QPS 档位独立部署、计时与采集，以下分位数分别合并该档位全部 client 的原始样本。</p>',
             '<p>状态：' + escape(state['status']) + '</p>']
    rows = []
    links = []
    for trial in state['trials']:
        directory = output / trial['directory']
        for backend in LABELS:
            aggregate = read_json(directory / backend / 'api_latency_summary.json')
            if not aggregate:
                continue
            write_op, read_op = api_operations(aggregate)
            counts = read_json(directory / backend / 'fanout.json', {})
            rows.append([trial['size'], trial['qps_per_client'], LABELS[backend], trial['total_clients'],
                         trial['writer_threads'], trial['reader_threads'], trial['notify_count'],
                         trial.get('effective_notify_count', trial['notify_count']),
                         trial.get('reads_per_notify', 1), trial.get('reads_per_write', trial['notify_count']),
                         '是' if trial.get('notify_include_self', False) else '否',
                         f"{aggregate[write_op]['avg_us'] / 1000:.3f}",
                         f"{aggregate[write_op]['p99_us'] / 1000:.3f}",
                         f"{aggregate[read_op]['avg_us'] / 1000:.3f}",
                         f"{aggregate[read_op]['p99_us'] / 1000:.3f}",
                         f"{counts.get('observed_get_per_set', 0):.3f}"])
        link = trial['directory'] + '/report.html'
        links.append('<li>' + escape(f"{trial['size']} / {trial['qps_per_client']} QPS/client / {trial['status']}")
                     + ' · <a href="' + quote(link) + '">完整报告、部署形态与配置</a>'
                     + (' · ' + escape(str(trial['error'])) if trial.get('error') else '') + '</li>')
    parts.append(table(['大小', 'QPS/client', '系统', 'client总数', '写线程', '读线程', '通知client数上限', '有效通知client数', '每通知读次数', '目标读/写', '候选含自身',
                        '写Avg(ms)', '写P99(ms)', '读Avg(ms)', '读P99(ms)', '实测读/写'], rows))
    parts += ['<h2>逐档位证据</h2><ul>', *links, '</ul>',
              '<p><a href="scenario-source.json">原始配置</a> · <a href="scenario-effective.json">生效配置</a> · '
              '<a href="state.json">执行状态</a></p></main></html>']
    (output / 'report.html').write_text('\n'.join(parts), encoding='utf-8')


def deployment_rows(output, backends):
    counts = defaultdict(Counter)
    resources = []
    for backend in backends:
        snapshot = read_json(output / backend / 'pods.json', {})
        for pod in snapshot.get('items', []):
            name = pod['metadata']['name']
            role = next((role for prefix, role in PREFIXES.items() if name.startswith(prefix)), 'Other')
            spec = pod.get('spec', {})
            node = spec.get('nodeName', '(未调度)')
            counts[(backend, node)][role] += 1
            for container in spec.get('containers', []):
                resource = container.get('resources', {})
                resources.append([LABELS[backend], node, name, role, container.get('image', ''),
                                  json.dumps(resource.get('requests', {}), ensure_ascii=False),
                                  json.dumps(resource.get('limits', {}), ensure_ascii=False)])
    rows = [[LABELS[backend], node, *(count[role] for role in ROLES)]
            for (backend, node), count in sorted(counts.items())]
    return rows, resources


def config_files(output, backends):
    """Include archived config sources, without duplicating large metric samples."""
    candidates = list(output.glob('*.json')) + list((output / 'image-config').glob('*.json'))
    candidates += list((output / 'templates').glob('*'))
    for backend in backends:
        for name in ('config.json', 'deploy.json', 'pods.json', 'rdma-devices.json',
                     'service-startup.json', 'thread-cpu.json', 'host-rdma.json', 'latency-source.json',
                     'execution.json', 'service-discovery.json'):
            candidates.append(output / backend / name)
        candidates += list((output / backend / 'collected').rglob('config*.json'))
        candidates += list((output / backend / 'collected').rglob('deploy*.json'))
        candidates += list((output / backend / 'worker-logs').rglob('*config*'))
        candidates += list((output / backend / 'coordinator-logs').rglob('*config*'))
    return sorted({path for path in candidates if path.is_file()})


def monitoring_section(output, backends):
    parts = []
    for backend in backends:
        threads = read_json(output / backend / 'thread-cpu.json')
        thread_section = ('<details><summary>Client 线程 CPU 采样 · 100% 为一个逻辑核</summary><pre>'
                          + escape(json.dumps(threads, ensure_ascii=False, indent=2)) + '</pre></details>'
                          if threads is not None else '')
        summary = read_json(output / backend / 'monitoring' / 'summary.json')
        if summary is None:
            if backend == 'mooncake' and read_json(output / 'state.json', {}).get('monitor_enabled') is False:
                parts += ['<h2>运行监控 · Mooncake</h2>',
                          '<p>基础 CPU/RDMA 周期采样未启用；Mooncake 原生 metrics 日志保留。</p>']
            parts.append(thread_section)
            continue
        parts += ['<h2>运行监控 · ' + escape(LABELS[backend]) + '</h2>',
                  '<p>监控覆盖 client 部署、写入和停止阶段，平均值不是纯稳态性能。'
                  'CPU 单位为核；节流比例为被节流周期数 / 总周期数。RDMA 为共享端口计数器差分，'
                  '包含其他使用该 HCA 的流量，每节点仅采一份；不能直接当作本次测试的独占线速。'
                  '缺失指标显示“未采集”。采样与内部指标日志会增加诊断开销。</p>',
                  '<p>' + escape(summary.get('scope', '')) + '</p>']

        def value(item, key):
            v = item.get(key)
            return '未采集' if v is None else f'{v:.3f}' if isinstance(v, float) else v

        keys = ('cpu_cores_avg', 'cpu_cores_peak', 'throttled_period_pct',
                'tx_gbps_avg', 'tx_gbps_peak', 'rx_gbps_avg', 'rx_gbps_peak', 'samples')
        parts.append(table(['Pod', 'CPU Avg(核)', 'CPU Peak(核)', '节流周期(%)',
                            'TX Avg(Gbit/s)', 'TX Peak', 'RX Avg(Gbit/s)', 'RX Peak', '样本数'],
                           [[p['pod'], *(value(p, k) for k in keys)] for p in summary.get('pods', [])]))
        hosts = summary.get('hosts', [])
        if hosts:
            parts.append('<h3>宿主机 RDMA · 每节点一份共享端口采样</h3>')
            parts.append(table(['节点', '设备', 'TX Avg(Gbit/s)', 'TX Peak',
                                'RX Avg(Gbit/s)', 'RX Peak', '样本数'],
                               [[h['node'], h['device'], *(value(h, k) for k in keys[3:])]
                                for h in hosts]))
        for host in hosts:
            parts.append('<details><summary>' + escape(host['node'])
                         + ' · 宿主机端口原始指标与采集状态</summary><pre>'
                         + escape(json.dumps(host, ensure_ascii=False, indent=2)) + '</pre>')
            for key in ('data_file', 'error_file'):
                if host.get(key):
                    link = f"{backend}/monitoring/{host[key]}"
                    parts.append('<p><a href="' + quote(link) + '">' + escape(host[key]) + '</a></p>')
            parts.append('</details>')
        for pod in summary.get('pods', []):
            parts += ['<details><summary>' + escape(pod['pod']) + ' · 原始指标与采集状态</summary><pre>'
                      + escape(json.dumps(pod, ensure_ascii=False, indent=2)) + '</pre>']
            for key in ('data_file', 'error_file'):
                if pod.get(key):
                    link = f"{backend}/monitoring/{pod[key]}"
                    parts.append('<p><a href="' + quote(link) + '">' + escape(pod[key]) + '</a></p>')
            parts.append('</details>')
        parts.append(f'<p><a href="{backend}/monitoring/summary.json">监控汇总 JSON</a> · '
                     f'<a href="{backend}/mooncake-internal-metrics.log">Mooncake 内部指标日志索引</a></p>')
        parts.append(thread_section)
    return '\n'.join(parts)


def render_html_report(output, backends=None, workload=None):
    output = Path(output)
    if backends is None:
        backends = tuple(name for name in LABELS
                         if (output / name / 'api_latency_summary.json').is_file()
                         and (output / name / 'metrics.json').is_file())
    if not backends:
        raise ValueError('No completed client metrics found in result directory')
    scenario = read_json(output / 'scenario-effective.json', {})
    workload = workload or scenario
    size = workload.get('size') or ', '.join(scenario.get('sizes', ['8MB']))
    parts = ['<!doctype html><html lang="zh-CN"><meta charset="utf-8">',
             '<meta name="viewport" content="width=device-width,initial-scale=1">',
             '<title>KVTest · 性能对比报告</title><style>', CSS, '</style>',
             '<header><div class="eyebrow">KVTEST / ' + escape(size) + ' / SINGLE RUN</div>',
             '<h1>性能对比与部署记录</h1>',
             '<p>' + escape(' · '.join(LABELS[b] for b in backends)) + '</p></header><main>',
             '<nav><a href="#summary">接口对比</a><a href="#deployment">部署形态</a>'
             '<a href="#clients">Client 明细</a><a href="#configs">完整配置</a></nav>',
             '<p class="muted">结果目录：' + escape(str(output.resolve())) + '<br>生成时间：'
             + datetime.now().astimezone().isoformat(timespec='seconds') + '</p>']
    state = read_json(output / 'state.json', {})
    if state.get('status') == 'failed':
        parts.append('<aside>运行状态为失败；本页仅展示已保存的数据。失败原因：'
                     + escape(str(state.get('error', ''))) + '</aside>')
    config_rows = []
    for backend in backends:
        cfg = read_json(output / backend / 'config.json', {})
        counts = read_json(output / backend / 'fanout.json', {})
        config_rows.append([LABELS[backend], read_json(output / backend / 'execution.json', {}).get('duration_seconds', '未归档'),
                            cfg.get('target_qps', workload.get('qps', '未归档')),
                            cfg.get('num_threads', workload.get('writer_threads', '未归档')),
                            cfg.get('num_total_threads', 0) - cfg.get('num_threads', 0),
                            cfg.get('notify_count', workload.get('fanout', '未归档')),
                            counts.get('effective_notify_count', workload.get('effective_notify_count',
                                       cfg.get('notify_count', workload.get('fanout', '未归档')))),
                            cfg.get('reads_per_notify', workload.get('reads_per_notify', 1)),
                            '是' if cfg.get('notify_include_self', False) else '否',
                            ','.join(cfg.get('pipeline', [])), ','.join(cfg.get('notify_pipeline', []))])
    parts += ['<h2>本次负载</h2>', table(['系统', '控制器运行窗口(s)', '目标写 QPS/client', '写线程/client',
                                        '读线程/client', '通知client数上限', '有效通知client数', '每通知读次数', '候选含自身', '写流水线', '通知流水线'], config_rows),
              '<section id="summary"><h2>接口耗时与实际读写量</h2>',
              '<p>DS 合并原生 SDK access 日志，MC 合并 SDK 外侧原始打点；两者边界不同。'
              '不平均各 client 的 P99。下面的对比单位为 ms。</p>']
    aggregate = {b: read_json(output / b / 'api_latency_summary.json') for b in backends}
    rows = []
    for index, label in [(0, '写入'), (1, '读取')]:
        for key, title in [('avg_us', 'Avg'), ('p50_us', 'P50'), ('p95_us', 'P95'), ('p99_us', 'P99')]:
            rows.append([label + ' ' + title,
                         *(f'{api_label(b, api_operations(aggregate[b])[index])}: '
                           f'{aggregate[b][api_operations(aggregate[b])[index]][key] / 1000:.3f}'
                           for b in backends)])
    parts.append(table(['指标 (ms)', *(LABELS[b] for b in backends)], rows))
    for backend in backends:
        fanout = read_json(output / backend / 'fanout.json', {})
        write_name, read_name = [api_label(backend, op) for op in api_operations(aggregate[backend])]
        ratio = fanout.get('observed_get_per_set', 0)
        parts += [f'<h3>{escape(LABELS[backend])}</h3>',
                  '<div class="cards">' + ''.join(
                      f'<div><small>{label}</small><strong>{escape(str(value))}</strong></div>'
                      for label, value in [('成功 ' + write_name, f"{fanout.get('successful_set', 0):,}"),
                                           ('成功 ' + read_name, f"{fanout.get('successful_get', 0):,}"),
                                           (read_name + ' / ' + write_name, f'{ratio:.3f}')]) + '</div>']
        if abs(ratio - fanout.get('configured_fanout', workload.get('reads_per_write', workload.get('fanout', ratio)))) > 0.001:
            parts.append('<aside>实际读取倍数与配置不同；比较时应同时查看读负载。读取数量不作严格相等校验。</aside>')
        fields = [('total', 'Total'), ('fail', 'Fail'), ('fail_rate_pct', 'FailRate %'),
                  ('min_us', 'Min'), ('avg_us', 'Avg'), ('p50_us', 'P50'), ('p90_us', 'P90'),
                  ('p95_us', 'P95'), ('p99_us', 'P99'), ('p999_us', 'P99.9'),
                  ('p9999_us', 'P99.99'), ('max_us', 'Max')]
        fields += [(f'gt{n}', f'>{n}us') for n in (2000, 5000, 6000, 7000, 8000, 9000, 10000, 20000)]
        parts += ['<p class="muted">完整耗时表单位：µs；阈值列为超过该耗时的调用次数。</p>',
                  table(['接口', *(title for _, title in fields)],
                        [[api_label(backend, op), *(f'{aggregate[backend][op].get(key, 0):,.2f}' if key == 'fail_rate_pct'
                                else f'{aggregate[backend][op].get(key, 0):,.0f}' for key, _ in fields)]
                         for op in api_operations(aggregate[backend])]),
                  f'<p><a href="{backend}/api_latency_summary.csv">下载 {escape(LABELS[backend])} CSV</a></p>']
    source_rows = []
    for backend in backends:
        source = read_json(output / backend / 'latency-source.json', {})
        for client in source.get('clients', []):
            for op, coverage in client.get('coverage', {}).items():
                source_rows.append([LABELS[backend], Path(client['summary']).parent.parent.name,
                    source.get('source', '未归档'), api_label(backend, op), client['samples'][op],
                    client['operations'][op], f'{coverage * 100:.2f}%'])
    parts += ['<h3>统计来源与采样覆盖率</h3>',
              table(['系统', 'Client', '计时来源', '接口', '样本数', '操作数', '覆盖率'], source_rows),
              '<p>DS 使用原生 access 日志，可能受 SDK 采样影响；MC 使用有界异步导出，缺样或未正常关闭会报错。'
              '逐 client 原生 KVTest 指标仍保留，用于核对外侧计时差异。</p>']
    topology, resources = deployment_rows(output, backends)
    if 'datasystem' in backends:
        discovery = read_json(output / 'datasystem/service-discovery.json')
        if discovery:
            jf = discovery.get('jf', {})
            parts += ['<h3>DataSystem 部署与服务发现</h3>',
                      table(['部署模式', 'JF节点', 'JF地址', 'JF服务名', '注册实例', '原生程序包'], [[
                          discovery['mode'], jf.get('node', '—'), discovery.get('jf_address', '—'),
                          jf.get('service', '—'),
                          ', '.join(discovery.get('registration', {}).get('instances', [])),
                          discovery.get('native_package', '—')]])]
    snapshot_times = '；'.join(LABELS[b] + '：' + str(
        read_json(output / b / 'pods.json', {}).get('snapshot_collected_at', '旧结果未记录采集时间'))
        for b in backends)
    parts += ['</section><section id="deployment"><h2>实际部署形态</h2>',
              '<p>按压测启动前保存的 Pod 快照统计；数量和资源取自实际 Pod spec。</p>',
              '<p class="muted">快照时间 ' + escape(snapshot_times) + '</p>',
              table(['系统', '节点', *ROLES], topology) if topology else '<aside>未归档 Pod 快照。</aside>',
              '<details><summary>逐 Pod 镜像与资源配额</summary>',
              table(['系统', '节点', 'Pod', '角色', '镜像', 'Requests（含设备）', 'Limits（含设备）'], resources),
              '</details>']
    rows = []
    parts.append('<h3>实际组件启动参数</h3><p>命令行来自运行时 /proc，client 项包含实际实例配置。'
                 '环境变量仅采集指定的地址与传输设置。</p>')
    for backend in backends:
        startup = read_json(output / backend / 'service-startup.json')
        if startup is None:
            parts.append('<aside>' + escape(LABELS[backend]) + '：旧结果未采集进程启动参数，见下方已归档配置。</aside>')
            continue
        for pod in startup.get('pods', []):
            label = f"{LABELS[backend]} / {pod['node']} / {pod['pod']}"
            parts.append('<details><summary>' + escape(label) + '</summary>')
            if pod.get('capture_error'):
                parts.append('<p>' + escape(pod['capture_error']) + '</p>')
            for process in pod.get('processes', []):
                parts.append('<pre>' + escape(shlex.join(process['argv'])) + '</pre><pre>'
                             + escape(json.dumps(process, ensure_ascii=False, indent=2)) + '</pre>')
            parts.append('</details>')
    parts += ['</section><section id="clients"><h2>逐 Client 指标</h2>',
              '<p>以下 P99 是各 client 自身的统计。QPS 为原有 Total/Uptime 口径，包含启动和排空时间。</p>']
    for backend in backends:
        for name, metrics in read_json(output / backend / 'metrics.json', {}).items():
            for op in api_operations(metrics):
                m = metrics[op]
                rows.append([LABELS[backend], name, api_label(backend, op), m['success'], m['fail'],
                             f"{m['qps']:.2f}", f"{m['avg_ms']:.3f}", f"{m['p99_ms']:.3f}"])
    parts += [table(['系统', 'Client 摘要路径', '接口', 'Success', 'Fail', 'QPS', 'Avg(ms)', 'P99(ms)'], rows),
              '</section>', monitoring_section(output, backends),
              '<h2>统计口径</h2><p>这是单轮观测，没有重复性证据。计时覆盖 SDK 调用，'
              'DS 为原生 SDK access 日志，MC 为调用外侧计时；不含通知排队、写前数据准备和 Get 后校验。'
              '表中 DS Total 是日志样本数，实际操作数见读写量；覆盖率见统计来源。'
              + escape(''.join(api_boundary_note(b, aggregate[b]) for b in backends)) +
              '逻辑处理量不等于网络带宽。本页展示数值，不据此宣称峰值性能。</p>',
              '<section id="configs"><h2>完整归档配置与证据</h2>',
              '<p>下列内容直接嵌入本次已归档文件；未归档的环境参数不作推断。原始文件链接需保留结果目录。</p>',
              '<input id="filter" type="search" placeholder="搜索文件名或参数，如 num_threads、enable_urma">']
    for path in config_files(output, backends):
        name = path.relative_to(output).as_posix()
        parts.append('<details class="config"><summary>' + escape(name) + '</summary>'
                     + f'<a href="{quote(name)}">原始文件</a><pre>'
                     + escape(path.read_text(encoding='utf-8', errors='replace')) + '</pre></details>')
    logs = sorted((output / 'commands').glob('*.log'))
    parts += ['<details><summary>执行日志索引</summary><ul>',
              *[f'<li><a href="commands/{quote(p.name)}">{escape(p.name)}</a></li>' for p in logs],
              '</ul></details></section></main>',
              '<script>document.getElementById("filter").addEventListener("input",function(){'
              'const q=this.value.toLowerCase();document.querySelectorAll(".config").forEach(d=>{'
              'd.hidden=!d.textContent.toLowerCase().includes(q);});});</script></html>']
    target = output / 'report.html'
    target.write_text('\n'.join(parts), encoding='utf-8')
    return target


CSS = """
:root{color-scheme:light;--ink:#182a3a;--muted:#596b7a;--border:#dde5eb}
*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:#f4f7fa;color:var(--ink);font:15px/1.65 system-ui,sans-serif}
header{background:#142e42;color:white;padding:42px max(24px,calc((100vw - 1280px)/2))}h1{font-size:34px;margin:8px 0}.eyebrow{letter-spacing:2px;font-size:12px;color:#92cbd9}
main{max-width:1330px;margin:auto;padding:24px}nav{display:flex;gap:24px;flex-wrap:wrap;padding:12px 0}a{color:#086c87}h2{margin:36px 0 12px;font-size:23px}h3{margin-top:28px}
.muted,small{color:var(--muted)}.scroll{overflow:auto;background:white;border:1px solid var(--border);border-radius:9px}table{border-collapse:collapse;width:100%;font-size:13px}th,td{text-align:left;padding:11px 14px;border-bottom:1px solid var(--border);white-space:nowrap;font-variant-numeric:tabular-nums}th{background:#eaf1f5;color:#28485d;position:sticky;top:0}tr:last-child td{border-bottom:0}tbody tr:hover{background:#f2f8fa}
.cards{display:flex;gap:14px;flex-wrap:wrap}.cards>div{flex:1;min-width:180px;background:white;padding:18px 22px;border:1px solid var(--border);border-radius:9px}.cards strong{display:block;font-size:27px;color:#12677d}aside{background:#fff4d9;border-left:4px solid #b68115;padding:12px 18px;margin:16px 0}
details{background:white;border:1px solid var(--border);border-radius:8px;padding:13px 18px;margin:12px 0}summary{cursor:pointer;font-weight:600;overflow-wrap:anywhere}pre{white-space:pre-wrap;overflow-wrap:anywhere;font:12px/1.6 ui-monospace,monospace;background:#f2f5f7;padding:16px;border-radius:6px;max-height:650px;overflow:auto}input{font:inherit;width:100%;padding:12px;border:1px solid #b9cbd7;border-radius:8px}section{scroll-margin-top:18px}[hidden]{display:none!important}
@media print{header{padding:20px}.scroll,pre{overflow:visible;max-height:none}nav,input{display:none}body{background:white}}
"""


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results', type=Path, help='Existing result directory; no deployment or test is run')
    args = parser.parse_args()
    print(render_html_report(args.results))
