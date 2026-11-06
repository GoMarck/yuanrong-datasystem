"""Terminal progress and saved summaries for the DataSystem scaling demo."""
import csv
from html import escape
import json
import math
import re
import shutil
import sys
import threading
import time

from kvtest_report import api_operations


HEADERS = ('节点数', '数据大小', 'Set Avg(ms)', 'Set P99(ms)', 'Get Avg(ms)', 'Get P99(ms)', '状态')
STATES = {'pending': '待运行', 'running': '运行中', 'complete': '完成', 'failed': '失败', 'interrupted': '已中断'}
LATENCIES = ('set_avg_ms', 'set_p99_ms', 'get_avg_ms', 'get_p99_ms')


def read_result(directory, row):
    def read(name):
        return json.loads((directory / name).read_text(encoding='utf-8'))

    if read('state.json').get('status') != 'complete':
        raise ValueError('controller did not finish successfully')
    stats = read('datasystem/api_latency_summary.json')
    write_op, read_op = api_operations(stats)
    result = dict(row, status='complete')
    for prefix, op in (('set', write_op), ('get', read_op)):
        item = stats[op]
        for source, target in (('avg_us', 'avg_ms'), ('p99_us', 'p99_ms')):
            value = float(item[source])
            if not math.isfinite(value) or value < 0 or item['total'] <= 0 or item['fail']:
                raise ValueError(f'invalid SDK latency samples: {op}')
            result[f'{prefix}_{target}'] = value / 1000
        result[f'{prefix}_samples'] = item['total']
    fanout = read('datasystem/fanout.json')
    result.update(successful_set=fanout['successful_set'], successful_get=fanout['successful_get'],
                  observed_get_per_set=fanout['observed_get_per_set'])
    for prefix, count in (('set', fanout['successful_set']), ('get', fanout['successful_get'])):
        if count <= 0 or result[f'{prefix}_samples'] > count:
            raise ValueError('invalid latency sample coverage')
        result[f'{prefix}_coverage'] = result[f'{prefix}_samples'] / count
    return result


def result_cells(row):
    return [str(row['nodes']), row['size'],
            *(f"{row[key]:.3f}" if key in row else '—' for key in LATENCIES),
            STATES[row['status']]]


def terminal_table(rows):
    values = [list(HEADERS), *(result_cells(row) for row in rows)]
    def width(text):
        import unicodedata
        return sum(2 if unicodedata.east_asian_width(c) in 'WF' else 1 for c in text)
    widths = [max(width(row[i]) for row in values) for i in range(len(HEADERS))]
    lines = ['  '.join(cell + ' ' * (widths[i] - width(cell)) for i, cell in enumerate(row)) for row in values]
    lines.insert(1, '─' * (sum(widths) + 2 * (len(widths) - 1)))
    return lines


def save_summary(output, rows, duration, qps, status, error=None):
    summary = {'status': status, 'size': '2MB', 'qps_per_client': qps,
               'duration_seconds': duration, 'source': 'datasystem_sdk_access_log', 'error': error, 'runs': rows}
    path = output / 'summary.json'
    temporary = path.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)
    fields = ('nodes', 'size', *LATENCIES, 'status', 'set_samples', 'get_samples',
              'successful_set', 'successful_get', 'observed_get_per_set', 'set_coverage', 'get_coverage', 'directory', 'error')
    with (output / 'summary.csv').open('w', newline='', encoding='utf-8-sig') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(rows)
    body = []
    for row in rows:
        cells = result_cells(row)
        report = output / row['directory'] / 'report.html'
        if report.is_file():
            cells[0] = f'<a href="{escape(row["directory"], quote=True)}/report.html">{row["nodes"]}</a>'
        else:
            cells[0] = escape(cells[0])
        body.append('<tr><td>' + cells[0] + '</td>'
                    + ''.join('<td>' + escape(cell) + '</td>' for cell in cells[1:]) + '</tr>')
    details = [[row['nodes'], row.get('set_samples', '—'), row.get('get_samples', '—'),
                f"{row['set_coverage']:.1%}" if 'set_coverage' in row else '—',
                f"{row['get_coverage']:.1%}" if 'get_coverage' in row else '—',
                row.get('successful_set', '—'), row.get('successful_get', '—')] for row in rows]
    html = ('<!doctype html><html lang="zh-CN"><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1"><title>DataSystem 节点扩展测试</title>'
            '<style>body{margin:0;background:#f5f7fa;color:#1f2937;font:17px system-ui,sans-serif}'
            'main{max-width:1180px;margin:64px auto;padding:0 28px}h1{font-size:36px}'
            '.sub{color:#475569}table{width:100%;border-collapse:collapse;margin:28px 0;font-variant-numeric:tabular-nums}'
            'th,td{text-align:right;padding:17px 12px;border-bottom:1px solid #cbd5e1}'
            'th{color:#475569}a{color:#1d4ed8}details{margin:28px 0}footer{color:#475569;font-size:14px}</style>'
            '<main><p class="sub">DATASYSTEM / SCALING BENCHMARK</p><h1>节点扩展测试结果</h1>'
            '<p class="sub">2MB · 1写1读</p>'
            '<table><thead><tr>' + ''.join('<th>' + escape(h) + '</th>' for h in HEADERS)
            + '</tr></thead><tbody>' + ''.join(body) + '</tbody></table>'
            '<details><summary>样本与实际调用数</summary><table><tr>'
            + ''.join('<th>' + h + '</th>' for h in ('节点数', 'Set样本', 'Get样本', 'Set覆盖率', 'Get覆盖率', '成功Set', '成功Get'))
            + '</tr>' + ''.join('<tr>' + ''.join('<td>' + escape(str(c)) + '</td>' for c in r) + '</tr>' for r in details)
            + '</table></details><footer>Avg/P99 来自该档全部 client 合并后的 SDK access 日志样本；'
            '覆盖率见展开项。点击节点数查看完整报告与部署证据。'
            ' <a href="summary.csv">CSV</a> · <a href="summary.json">JSON</a></footer></main></html>')
    (output / 'summary.html').write_text(html, encoding='utf-8')


class Progress:
    def __init__(self, nodes, duration):
        self.nodes = nodes
        self.duration = duration
        self.stage = '准备'
        self.ready = set()
        self.collected = set()
        self.run_started = None
        self.lock = threading.Lock()

    def consume(self, line):
        with self.lock:
            match = re.match(r'pod/(\S+) condition met', line.strip())
            if match:
                self.ready.add(match[1])
            match = re.match(r'\s*(ds-client-\S+) -> \d+ files collected', line)
            if match:
                self.collected.add(match[1])
            stages = (('[1/7]', '预检'), ('[2/7]', '创建环境'), ('[3/7]', '部署服务'),
                      ('[4/7]', '部署客户端'), ('[5/7]', '生成配置'), ('[6/7]', '安装与启动客户端'),
                      ('[7/7]', '清理环境'))
            for marker, stage in stages:
                if line.startswith(marker):
                    self.stage = stage
            if line.startswith('rc44 native pipeline:'):
                self.stage = '压测'
                self.run_started = time.monotonic()
            elif line.startswith('> ') and 'deploy_client.py stop ' in line:
                self.stage = '停止客户端'
            elif line.startswith('> ') and 'deploy_client.py collect ' in line:
                self.stage = '采集日志'
            elif line.startswith('Collect result:'):
                self.stage = '汇总与归档'

    def snapshot(self):
        with self.lock:
            return self.stage, len(self.ready), len(self.collected), self.run_started


def bar(value, total, width=30):
    filled = int(width * min(value / max(total, 1), 1))
    return '[' + '█' * filled + '░' * (width - filled) + ']'


class Console:
    def __init__(self, rows, duration, qps, plain=False, theme='light'):
        self.rows = rows
        self.duration = duration
        self.qps = qps
        self.tty = sys.stdout.isatty() and not plain
        self.last_event = None
        self.last_completed = 0
        self.frame_shown = False
        self.started = time.monotonic()
        self.colors = ({'title': '#1e40af', 'running': '#1d4ed8', 'complete': '#166534',
                        'failed': '#b91c1c', 'interrupted': '#92400e'} if theme == 'light' else
                       {'title': '#7dd3fc', 'running': '#38bdf8', 'complete': '#86efac',
                        'failed': '#fca5a5', 'interrupted': '#fde68a'})

    def colored(self, text, role):
        color = self.colors[role].lstrip('#')
        rgb = ';'.join(str(int(color[i:i + 2], 16)) for i in (0, 2, 4))
        return '\x1b[38;2;' + rgb + 'm' + text + '\x1b[0m'

    def draw(self, index, progress=None, status='运行中', final=False):
        completed = sum(row['status'] == 'complete' for row in self.rows)
        state = progress.snapshot() if progress else (status, 0, 0, None)
        stage, ready, collected, run_started = state
        elapsed = int(time.monotonic() - self.started)
        current = self.rows[index] if index < len(self.rows) else self.rows[-1]
        lines = ['DATASYSTEM  |  节点扩展性能测试',
                 '2MB  ·  1写1读', '',
                 f'总进度  {bar(completed, len(self.rows))}  {completed}/{len(self.rows)}',
                 f'当前节点数  {current["nodes"]}    阶段  {stage}    总用时  {elapsed // 60:02d}:{elapsed % 60:02d}']
        if stage == '压测' and run_started is not None:
            used = min(time.monotonic() - run_started, self.duration)
            lines.append(f'压测进度  {bar(used, self.duration)}  {int(used)}/{self.duration}s  剩余 {max(0, self.duration - int(used))}s')
        elif stage in ('部署服务', '部署客户端'):
            total = current['nodes'] * 3 + 1
            lines.append(f'就绪进度  {bar(ready, total)}  {ready}/{total}')
        elif stage == '采集日志':
            total = current['nodes'] * 2
            lines.append(f'采集进度  {bar(collected, total)}  {collected}/{total}')
        else:
            spinner = '◐◓◑◒'[int(time.monotonic() * 4) % 4]
            lines.append('阶段状态  ' + ('已结束' if final else spinner + ' 处理中…'))
        table_row_start = len(lines) + 3
        lines += ['', *terminal_table(self.rows), '', '时延单位：ms  |  SDK access 日志样本统计']
        if self.tty:
            columns = shutil.get_terminal_size((120, 30)).columns
            lines[0] = '\x1b[1m' + self.colored(lines[0], 'title')
            lines[3] = self.colored(lines[3], 'running')
            for offset, row in enumerate(self.rows, table_row_start):
                if row['status'] in ('complete', 'running', 'failed', 'interrupted'):
                    lines[offset] = self.colored(lines[offset], row['status'])
            if columns < 96:
                lines.append('建议终端宽度至少96列，以完整显示结果表。')
            clear = '' if self.frame_shown else '\x1b[2J'
            sys.stdout.write('\x1b[?25l\x1b[H' + clear + '\x1b[K\n'.join(lines) + '\x1b[K\n\x1b[J')
            sys.stdout.flush()
            self.frame_shown = True
        else:
            event = (index, stage, int((time.monotonic() - run_started) // 10) if stage == '压测' and run_started else None)
            if final or event != self.last_event:
                print('\n'.join(lines if final or completed != self.last_completed else lines[:7]), flush=True)
                self.last_event = event
                self.last_completed = completed

    def close(self):
        if self.tty:
            sys.stdout.write('\x1b[?25h')
            sys.stdout.flush()
