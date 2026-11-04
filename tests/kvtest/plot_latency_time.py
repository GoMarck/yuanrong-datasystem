#!/usr/bin/env python3
"""Plot DS/MC latency windows from collected latency_timeseries.csv files."""
import argparse
from collections import defaultdict
import csv
from dataclasses import dataclass, field
from datetime import datetime, timezone
import fnmatch
import json
import math
from pathlib import Path
import sys


METRICS = {'avg': 'avg_ms', 'p90': 'p90_ms', 'p99': 'p99_ms', 'p99.9': 'p99.9_ms',
           'p99.99': 'p99.99_ms', 'max': 'max_ms'}
DEFAULT_METRICS = ('avg', 'p99', 'p99.99', 'max')
OPERATIONS = {
    'datasystem': {'setStringView': 'write', 'setBuffer': 'write', 'getBuffer': 'read'},
    'mooncake': {'put': 'write', 'get_buffer': 'read', 'mooncakePut': 'write', 'mooncakeGet': 'read',
                'setBuffer': 'write', 'getBuffer': 'read'},
}
LABELS = {'datasystem': 'DS', 'mooncake': 'MC'}
COLORS = {'datasystem': '#2563eb', 'mooncake': '#d45426'}
STYLES = {'datasystem': '-', 'mooncake': '--'}


@dataclass
class Window:
    backend: str
    client: str
    source: str
    timestamp: str
    time: datetime
    operation: str
    role: str
    count: int
    values: dict = field(default_factory=dict)
    elapsed: float = 0.0


def parse_time(value):
    stamp = datetime.fromisoformat(value.replace('Z', '+00:00'))
    # Naive source clocks have no zone; preserve their wall-time differences.
    return stamp.replace(tzinfo=timezone.utc) if stamp.tzinfo is None else stamp.astimezone(timezone.utc)


def load_csv(path, backend, client):
    rows = []
    with path.open(newline='', encoding='utf-8-sig') as stream:
        reader = csv.DictReader(stream)
        required = {'timestamp', 'op', 'count', *METRICS.values()}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError(f'{path}: missing time-series columns; raw api_latency_samples.csv has no timestamps')
        for line_number, raw in enumerate(reader, start=2):
            operation = raw.get('op')
            if operation not in OPERATIONS[backend]:
                continue
            try:
                count = int(raw['count'])
                if count < 0:
                    raise ValueError('negative count')
                if count == 0:
                    continue
                values = {name: float(raw[column]) for name, column in METRICS.items()}
                if any(not math.isfinite(value) or value < 0 for value in values.values()):
                    raise ValueError('latencies must be finite and nonnegative')
                stamp = raw['timestamp'].strip()
                rows.append(Window(backend, client, str(path), stamp, parse_time(stamp), operation,
                                   OPERATIONS[backend][operation], count, values))
            except (ValueError, TypeError, KeyError, AttributeError) as error:
                raise ValueError(f'{path}:{line_number}: {error}') from error
    return rows


def source_files(root, backend):
    root = Path(root).resolve()
    if root.is_file():
        return [(root, root.parent.parent.name if root.parent.name.startswith('metrics') else root.parent.name)]
    if (root / backend).is_dir():
        root = root / backend
    collected = root / 'collected'
    if not collected.is_dir():
        raise ValueError(f'{root}: expected {backend}/collected or collected directory')
    files = sorted(collected.rglob('latency_timeseries.csv'))
    if not files:
        raise ValueError(f'{collected}: no latency_timeseries.csv found')
    sources = [(path, path.relative_to(collected).parts[0]) for path in files]
    clients = [client for _, client in sources]
    if len(set(clients)) != len(clients):
        raise ValueError(f'{collected}: multiple time-series files for one client; select one run per client')
    return sources


def load_sources(sources):
    rows = []
    for backend, root in sources.items():
        backend_rows = []
        for path, client in source_files(root, backend):
            records = load_csv(path, backend, client)
            if not records:
                raise ValueError(f'{path}: no nonempty SDK latency windows')
            backend_rows.extend(records)
        for role in ('write', 'read'):
            operations = {row.operation for row in backend_rows if row.role == role}
            if len(operations) > 1:
                raise ValueError(f'{backend}: mixed {role} APIs {sorted(operations)}; plot each API/run separately')
        origin = min(row.time for row in backend_rows)
        for row in backend_rows:
            row.elapsed = (row.time - origin).total_seconds()
        rows.extend(backend_rows)
    if not rows:
        raise ValueError('no latency windows found')
    return rows


def load_case(root):
    root = Path(root).resolve()
    sources = {backend: root / backend for backend in LABELS if (root / backend).is_dir()}
    if not sources and root.name in LABELS:
        sources = {root.name: root}
    if not sources:
        raise ValueError(f'{root}: expected a comparison result or datasystem/mooncake result directory')
    return load_sources(sources)


def select_rows(rows, clients=(), start=None, end=None):
    selected = [row for row in rows
                if (not clients or any(fnmatch.fnmatchcase(row.client, pattern) for pattern in clients))
                and (start is None or row.elapsed >= start) and (end is None or row.elapsed <= end)]
    if not selected:
        raise ValueError('no windows match the client/time filters')
    return selected


def weighted_average(rows, backend, role):
    groups = defaultdict(list)
    for row in rows:
        if (row.backend, row.role) == (backend, role):
            groups[row.elapsed].append(row)
    result = []
    for elapsed, group in sorted(groups.items()):
        count = sum(row.count for row in group)
        result.append({'elapsed_s': elapsed, 'value_ms': math.fsum(row.values['avg'] * row.count for row in group) / count,
                       'count': count, 'clients': len({row.client for row in group})})
    return result


def export_points(rows, output, metrics):
    fields = ['system', 'client', 'api', 'role', 'timestamp', 'elapsed_s', 'count',
              *METRICS.values(), 'source']
    with (output / 'latency_points.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(dict(system=row.backend, client=row.client, api=row.operation, role=row.role,
                                 timestamp=row.timestamp, elapsed_s=row.elapsed, count=row.count, source=row.source,
                                 **{METRICS[name]: value for name, value in row.values.items()}))
    fields = ['system', 'role', 'elapsed_s', 'avg_ms', 'count', 'clients']
    with (output / 'weighted_avg.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fields)
        writer.writeheader()
        for backend in LABELS:
            for role in ('write', 'read'):
                for point in weighted_average(rows, backend, role):
                    writer.writerow(dict(system=backend, role=role, elapsed_s=point['elapsed_s'], avg_ms=point['value_ms'],
                                         count=point['count'], clients=point['clients']))
    fields = ['system', 'api', 'client', 'metric', 'latency_ms', 'timestamp', 'elapsed_s', 'count', 'source']
    with (output / 'latency_peaks.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fields)
        writer.writeheader()
        for backend in LABELS:
            for role in ('write', 'read'):
                candidates = [row for row in rows if (row.backend, row.role) == (backend, role)]
                for metric in metrics:
                    if not candidates:
                        continue
                    row = max(candidates, key=lambda value: value.values[metric])
                    writer.writerow(dict(system=backend, api=row.operation, client=row.client, metric=metric,
                                         latency_ms=row.values[metric], timestamp=row.timestamp,
                                         elapsed_s=row.elapsed, count=row.count, source=row.source))
                    print(f'{LABELS[backend]} {row.operation} peak client-window {metric}={row.values[metric]:.3f} ms '
                          f'at +{row.elapsed:.1f}s ({row.timestamp}), client={row.client}')


def render(rows, output, metrics, formats, title, log_y=False):
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        from matplotlib.lines import Line2D
    except ImportError as error:
        raise ValueError('Matplotlib required: python3 -m pip install matplotlib') from error
    figure, axes = plt.subplots(len(metrics), 2, figsize=(15, 3.1 * len(metrics) + 1.5),
                                sharex=True, sharey='row', squeeze=False)
    for index, metric in enumerate(metrics):
        for column, role in enumerate(('write', 'read')):
            axis = axes[index][column]
            present = []
            for backend in LABELS:
                candidates = [row for row in rows if (row.backend, row.role) == (backend, role)]
                if not candidates:
                    continue
                present.append(backend)
                if metric == 'avg':
                    points = weighted_average(candidates, backend, role)
                    axis.plot([point['elapsed_s'] for point in points],
                              [point['value_ms'] if not log_y or point['value_ms'] > 0 else math.nan for point in points],
                              color=COLORS[backend], linestyle=STYLES[backend], linewidth=1.8,
                              marker='.', markersize=2.5)
                else:
                    groups = defaultdict(list)
                    for row in candidates:
                        groups[row.client].append(row)
                    for client_rows in groups.values():
                        ordered = sorted(client_rows, key=lambda row: row.elapsed)
                        axis.plot([row.elapsed for row in ordered],
                                  [row.values[metric] if not log_y or row.values[metric] > 0 else math.nan for row in ordered],
                                  color=COLORS[backend], linestyle=STYLES[backend], linewidth=0.9,
                                  alpha=0.6, marker='.', markersize=2)
            subtitle = 'call-count weighted windows' if metric == 'avg' else 'one trace per client; no pooling'
            axis.set_title(f'{role.upper()} / {metric.upper()} — {subtitle}', fontsize=10, loc='left')
            axis.set_ylabel('Latency (ms)')
            axis.grid(alpha=0.2)
            axis.spines[['top', 'right']].set_visible(False)
            if log_y:
                axis.set_yscale('log')
            if present:
                handles = [Line2D([0], [0], color=COLORS[backend], linestyle=STYLES[backend], label=LABELS[backend])
                           for backend in present]
                axis.legend(handles=handles, loc='upper right', framealpha=0.8, fontsize=9)
            else:
                axis.text(0.5, 0.5, 'No samples', transform=axis.transAxes, ha='center')
        if not log_y:
            axes[index][0].set_ylim(bottom=0)
    for axis in axes[-1]:
        axis.set_xlabel('Elapsed seconds from each system\'s first nonempty API window')
    figure.suptitle(title, fontsize=17, fontweight='bold')
    footer = ('CSV timestamps are window-end wall times; systems ran sequentially and are aligned independently.\n'
              'Percentiles and Max are client-window statistics, not pooled percentiles. All calls (including failures) are included.\n'
              'No smoothing, outlier clipping or zero filling. Log view omits zero-valued points.' if log_y else
              'CSV timestamps are window-end wall times; systems ran sequentially and are aligned independently.\n'
              'Percentiles and Max are client-window statistics, not pooled percentiles. All calls (including failures) are included.\n'
              'No smoothing, outlier clipping or zero filling.')
    figure.text(0.02, 0.012, footer, fontsize=8, color='#555555')
    figure.tight_layout(rect=(0, 0.065, 1, 0.96))
    try:
        for extension in formats:
            path = output / f'latency_time.{extension}'
            figure.savefig(path, dpi=180, bbox_inches='tight')
            print('Plot:', path)
    finally:
        plt.close(figure)


def case_jobs(root):
    root = Path(root).resolve()
    state_path = root / 'state.json'
    state = json.loads(state_path.read_text(encoding='utf-8')) if state_path.is_file() else {}
    if 'trials' not in state:
        return [(root.name, root)]
    jobs = []
    for trial in state['trials']:
        directory = (root / trial['directory']).resolve()
        if not directory.is_relative_to(root) or not directory.is_dir():
            raise ValueError(f'invalid trial directory: {directory}')
        jobs.append((directory.name, directory))
    if not jobs:
        raise ValueError('matrix has no trial results')
    return jobs


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results', nargs='?', type=Path, help='comparison result, backend result or matrix root')
    parser.add_argument('--ds', type=Path, help='DataSystem result directory or one time-series CSV')
    parser.add_argument('--mc', type=Path, help='Mooncake result directory or one time-series CSV')
    parser.add_argument('--output', type=Path, help='output directory; defaults to <result>/latency-plots')
    parser.add_argument('--metrics', default=','.join(DEFAULT_METRICS), help='comma-separated avg,p90,p99,p99.9,p99.99,max')
    parser.add_argument('--client', action='append', default=[], help='client folder name or quoted wildcard, repeatable')
    parser.add_argument('--start', type=float, help='first elapsed second to include')
    parser.add_argument('--end', type=float, help='last elapsed second to include')
    parser.add_argument('--log-y', action='store_true', help='log latency axis; zero points are omitted')
    parser.add_argument('--formats', default='png,svg', help='comma-separated png,svg,pdf')
    parser.add_argument('--title', default='KVTest DS / MC — latency over time')
    args = parser.parse_args(argv)
    try:
        if bool(args.results) == bool(args.ds or args.mc):
            raise ValueError('provide either a result path or --ds/--mc paths')
        metrics = tuple(name.strip() for name in args.metrics.split(','))
        formats = tuple(name.strip() for name in args.formats.split(','))
        if not metrics or len(set(metrics)) != len(metrics) or set(metrics) - METRICS.keys():
            raise ValueError('unsupported or duplicate metrics')
        if not formats or len(set(formats)) != len(formats) or set(formats) - {'png', 'svg', 'pdf'}:
            raise ValueError('formats must be unique choices of png,svg,pdf')
        if any(value is not None and (not math.isfinite(value) or value < 0) for value in (args.start, args.end)):
            raise ValueError('start/end must be finite nonnegative seconds')
        if args.start is not None and args.end is not None and args.end <= args.start:
            raise ValueError('end must be greater than start')
        if args.results:
            jobs = [(name, load_case(root)) for name, root in case_jobs(args.results)]
            base = args.output or args.results / 'latency-plots'
        else:
            sources = {backend: path for backend, path in (('datasystem', args.ds), ('mooncake', args.mc)) if path}
            jobs = [('comparison', load_sources(sources))]
            base = args.output or Path.cwd() / 'latency-plots'
        for name, windows in jobs:
            selected = select_rows(windows, args.client, args.start, args.end)
            output = (base / name if len(jobs) > 1 else base).resolve()
            output.mkdir(parents=True, exist_ok=True)
            print(f'{name}: {len(selected)} client/API windows, '
                  f'{len({(row.backend, row.client) for row in selected})} clients')
            render(selected, output, metrics, formats, args.title + (' / ' + name if len(jobs) > 1 else ''), args.log_y)
            export_points(selected, output, metrics)
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    raise SystemExit(main())
