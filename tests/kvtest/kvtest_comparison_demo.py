"""Recorded comparison display backed by archived controller results."""
import csv
from html import escape
import json
import re
import shutil
import sys
import time

from kvtest_demo import Console, LATENCIES, Progress, STATES, bar, read_result, terminal_table


LABELS = {'datasystem': 'DataSystem', 'mooncake': '对比系统'}
HEADERS = ('数据大小', 'DataSystem Avg', '对比系统 Avg', '时延降低',
           'DataSystem P99', '对比系统 P99', '时延降低', '状态')


def read_comparison_result(directory, row):
    systems = {backend: read_result(directory, row, backend) for backend in LABELS}
    reductions = {}
    for key in LATENCIES:
        baseline = systems['mooncake'][key]
        reductions[key] = (1 - systems['datasystem'][key] / baseline) * 100 if baseline > 0 else None
    return dict(row, status='complete', systems=systems, reduction_pct=reductions)


def metric_cells(row, operation):
    cells = [row['size']]
    for statistic in ('avg_ms', 'p99_ms'):
        key = f'{operation}_{statistic}'
        for backend in LABELS:
            value = row.get('systems', {}).get(backend, {}).get(key)
            cells.append(f'{value:.3f}' if value is not None else '—')
        reduction = row.get('reduction_pct', {}).get(key)
        cells.append(f'{reduction:+.1f}%' if reduction is not None else '—')
    return [*cells, STATES[row['status']]]


def save_comparison_summary(output, rows, duration, status, error=None):
    summary = {'status': status, 'duration_seconds_per_system': duration, 'qps_per_client': 17,
               'nodes': 2, 'clients_per_node': 2, 'workers_per_node': 1, 'reads_per_write': 1,
               'display_labels': LABELS,
               'latency_sources': {'datasystem': 'sdk_access_log', 'mooncake': 'kvtest_sdk_call'},
               'reduction_formula': '(baseline_ms - datasystem_ms) / baseline_ms * 100',
               'error': error, 'runs': rows}
    temporary = output / 'summary.json.tmp'
    temporary.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(output / 'summary.json')
    fields = ['size', 'status', 'backend', *LATENCIES, *(key + '_reduction_pct' for key in LATENCIES),
              'set_samples', 'get_samples', 'successful_set', 'successful_get', 'set_coverage', 'get_coverage']
    with (output / 'summary.csv').open('w', newline='', encoding='utf-8-sig') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction='ignore')
        writer.writeheader()
        for row in rows:
            for backend in LABELS:
                reductions = {key + '_reduction_pct': value for key, value in row.get('reduction_pct', {}).items()}
                writer.writerow(dict(row.get('systems', {}).get(backend, {}), **reductions,
                                     size=row['size'], status=row['status'], backend=backend))
    tables = []
    for operation, title in (('set', '写入接口时延'), ('get', '读取接口时延')):
        body = []
        for row in rows:
            cells = metric_cells(row, operation)
            rendered = [escape(cell) for cell in cells]
            if (output / row['directory'] / 'report.html').is_file():
                rendered[0] = f'<a href="{escape(row["directory"], quote=True)}/report.html">{rendered[0]}</a>'
            for index, statistic in ((3, 'avg_ms'), (6, 'p99_ms')):
                value = row.get('reduction_pct', {}).get(f'{operation}_{statistic}')
                color = '#166534' if value is not None and value >= 0 else '#b91c1c'
                if value is not None:
                    rendered[index] = f'<span style="color:{color}">{rendered[index]}</span>'
            body.append('<tr>' + ''.join('<td>' + cell + '</td>' for cell in rendered) + '</tr>')
        tables.append('<h2>' + title + '（ms）</h2><table><thead><tr>'
                      + ''.join('<th>' + escape(h) + '</th>' for h in HEADERS)
                      + '</tr></thead><tbody>' + ''.join(body) + '</tbody></table>')
    details = []
    for row in rows:
        for backend, item in row.get('systems', {}).items():
            cells = [row['size'], LABELS[backend], item['set_samples'], item['get_samples'],
                     item['successful_set'], item['successful_get'],
                     f"{item['set_coverage']:.1%}", f"{item['get_coverage']:.1%}"]
            details.append('<tr>' + ''.join('<td>' + escape(str(c)) + '</td>' for c in cells) + '</tr>')
    html = ('<!doctype html><html lang="zh-CN"><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1"><title>DataSystem 性能对比</title>'
            '<style>body{margin:0;background:#f5f7fa;color:#1f2937;font:17px system-ui,sans-serif}'
            'main{max-width:1300px;margin:48px auto;padding:0 24px}h1{font-size:36px}'
            'p,footer{color:#475569}table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}'
            'th,td{padding:14px 10px;text-align:right;border-bottom:1px solid #cbd5e1}'
            'a{color:#1d4ed8}details{margin:24px 0}footer{font-size:14px}</style>'
            '<main><h1>DataSystem 性能对比</h1><p>2节点 · 1写1读 · 4 / 6 / 8 / 16 / 32MB</p>'
            + ''.join(tables)
            + '<p>时延降低 =（对比系统时延 − DataSystem时延）/ 对比系统时延；负值表示时延上升。'
            '百分比为本轮观测，未运行或无有效基准的档位不填值。</p>'
            '<details><summary>样本与实际调用数</summary><table><tr>'
            + ''.join('<th>' + c + '</th>' for c in ('数据大小', '系统', '写样本', '读样本', '成功写', '成功读', '写覆盖率', '读覆盖率'))
            + '</tr>' + ''.join(details) + '</table></details>'
            '<footer>DataSystem 使用 SDK access 日志，对比系统使用 KVTest 接口外侧计时；统计边界有差异。'
            'Avg/P99来自各系统全部client合并样本，P99不做client间平均。实际配置与真实后端名称见各档详细报告。'
            ' <a href="summary.csv">CSV</a> · <a href="summary.json">JSON</a></footer></main></html>')
    (output / 'summary.html').write_text(html, encoding='utf-8')


class ComparisonProgress(Progress):
    def __init__(self, duration):
        super().__init__(2, duration)
        self.backend = 'datasystem'

    def consume(self, line):
        super().consume(line)
        with self.lock:
            if line.lstrip().startswith('[2/7]'):
                self.ready.clear()
                self.collected.clear()
                self.run_started = None
                self.backend = 'mooncake' if 'kvtest-mo-' in line else 'datasystem'
            if re.match(r'\s*mc-client-\S+ -> \d+ files collected', line):
                self.collected.add(line.strip().split()[0])

    def snapshot(self):
        with self.lock:
            return self.stage, len(self.ready), len(self.collected), self.run_started, self.backend


class ComparisonConsole(Console):
    def draw(self, index, progress=None, status='运行中', final=False):
        completed = sum(row['status'] == 'complete' for row in self.rows)
        stage, ready, collected, run_started, backend = (
            progress.snapshot() if progress else (status, 0, 0, None, 'datasystem'))
        elapsed = int(time.monotonic() - self.started)
        lines = ['DATASYSTEM  |  接口性能对比', '2节点  ·  1写1读', '',
                 f'总进度  {bar(completed, len(self.rows))}  {completed}/{len(self.rows)}',
                 f'当前数据  {self.rows[index]["size"]}    系统  {LABELS[backend] if progress else "—"}    阶段  {stage}',
                 f'总用时  {elapsed // 60:02d}:{elapsed % 60:02d}']
        if stage == '压测' and run_started is not None:
            used = min(time.monotonic() - run_started, self.duration)
            lines.append(f'压测进度  {bar(used, self.duration)}  {int(used)}/{self.duration}s')
        elif stage in ('部署服务', '部署客户端'):
            lines.append(f'就绪进度  {bar(ready, 7)}  {ready}/7')
        elif stage == '采集日志':
            lines.append(f'采集进度  {bar(collected, 4)}  {collected}/4')
        else:
            lines.append('阶段状态  ' + ('已结束' if final else '◐◓◑◒'[int(time.monotonic() * 4) % 4] + ' 处理中…'))
        for operation, title in (('set', '写入接口时延（ms）'), ('get', '读取接口时延（ms）')):
            lines += ['', title, *terminal_table(self.rows, HEADERS, lambda row: metric_cells(row, operation))]
        lines += ['', '时延降低为负值表示时延上升  |  单轮实测  |  样本与计时口径见汇总报告']
        if self.tty:
            lines[0] = '\x1b[1m' + self.colored(lines[0], 'title') + '\x1b[0m'
            lines[3] = self.colored(lines[3], 'running')
            for i, line in enumerate(lines[7:], 7):
                lines[i] = re.sub(r'[+-]\d+\.\d+%', lambda match: self.colored(
                    match[0], 'failed' if match[0].startswith('-') else 'complete'), line)
            if shutil.get_terminal_size((140, 40)).columns < 120:
                lines.append('建议终端宽度至少120列，完整显示两组结果。')
            clear = '' if self.frame_shown else '\x1b[2J'
            sys.stdout.write('\x1b[?25l\x1b[H' + clear + '\x1b[K\n'.join(lines) + '\x1b[K\n\x1b[J')
            sys.stdout.flush()
            self.frame_shown = True
        else:
            event = (index, backend, stage, int((time.monotonic() - run_started) // 10) if run_started else None)
            if final or event != self.last_event:
                print('\n'.join(lines if final or completed != self.last_completed else lines[:7]), flush=True)
                self.last_event, self.last_completed = event, completed
