#!/usr/bin/env python3
"""Aggregate cumulative KVTest stage snapshots without adding nested timers."""

import argparse
import csv
import json
from pathlib import Path
import sys

MARKER = '[KVTEST_STAGE_PROFILE] '
BUCKET_NS = [100, 250, 500, 1000, 2000, 4000, 8000, 16000, 32000, 64000,
             128000, 256000, 512000, 1000000, 2000000, 4000000, 8000000,
             16000000, 64000000, 256000000, 1000000000, 2**64 - 1]
COUNTERS = ('count', 'sum_ns', 'requested_bytes', 'failed', 'unknown')


def log_files(paths):
    for path in paths:
        root = Path(path)
        if root.is_dir():
            yield from sorted(p for p in root.rglob('*') if p.is_file() and
                              (p.suffix in ('.log', '.jsonl', '.out') or
                               '.INFO.' in p.name or '.log.' in p.name))
        else:
            yield root


def process_identity(instance):
    return instance.rsplit(':', 1)[0]


def read_snapshots(paths):
    snapshots = {}
    for file in log_files(paths):
        with file.open(errors='replace') as stream:
            for number, line in enumerate(stream, 1):
                if MARKER not in line:
                    continue
                try:
                    row = json.loads(line.split(MARKER, 1)[1])
                    if row['schema'] != 1 or not row['phase'].startswith(('ds.', 'mc.')):
                        raise ValueError('unsupported schema/phase')
                    for name in (*COUNTERS, 'seq', 'max_ns'):
                        if type(row[name]) is not int or row[name] < 0:
                            raise ValueError('invalid counter: ' + name)
                    if len(row['buckets']) != len(BUCKET_NS) or any(
                            type(value) is not int or value < 0 for value in row['buckets']):
                        raise ValueError('invalid histogram')
                    if sum(row['buckets']) != row['count']:
                        raise ValueError('histogram count mismatch')
                    key = (row['instance'], row['phase'], row['seq'])
                    if key in snapshots and snapshots[key] != row:
                        raise ValueError('conflicting duplicate snapshot')
                    snapshots[key] = row
                except (ValueError, KeyError, TypeError) as error:
                    raise ValueError(f'{file}:{number}: {error}') from error
    if not snapshots:
        raise ValueError('no stage snapshots found; check build, runtime environment and collection paths')
    return list(snapshots.values())


def percentile(buckets, count, percent):
    rank = (count * percent + 99) // 100
    total = 0
    for bound, value in zip(BUCKET_NS, buckets):
        total += value
        if total >= rank:
            return None if bound == BUCKET_NS[-1] else bound / 1000
    raise ValueError('invalid histogram count')


def summarize(snapshots, start_seq=0, group_by='role'):
    client_instances = {process_identity(r['instance']) for r in snapshots if '.api.' in r['phase']}
    by_phase = {}
    for row in snapshots:
        by_phase.setdefault((row['instance'], row['phase']), []).append(row)
    groups, warnings = {}, []
    for (instance, phase), rows in by_phase.items():
        rows.sort(key=lambda r: r['seq'])
        last = rows[-1]
        base = next((r for r in reversed(rows) if r['seq'] <= start_seq), None)
        if last['seq'] <= start_seq:
            continue
        values = {k: last[k] - (base[k] if base else 0) for k in COUNTERS}
        buckets = [v - (base['buckets'][i] if base else 0) for i, v in enumerate(last['buckets'])]
        if any(value < 0 for value in (*values.values(), *buckets)):
            raise ValueError(f'nonmonotonic cumulative counters: {instance} {phase}')
        if not last['final']:
            warnings.append(f'{instance}: latest snapshot is periodic; final tail may be missing')
        if values['count'] == 0:
            continue
        role = 'client' if process_identity(instance) in client_instances else 'service'
        key = (phase, instance if group_by == 'instance' else role)
        group = groups.setdefault(key, dict(phase=phase, source=key[1],
                                          **{k: 0 for k in COUNTERS},
                                          buckets=[0] * len(BUCKET_NS), max_ns=0, sample_intervals=set()))
        group['sample_intervals'].add(last.get('detail_sample_every', 1) if '.detail.' in phase else 1)
        for name, value in values.items():
            group[name] += value
        group['buckets'] = [a + b for a, b in zip(group['buckets'], buckets)]
        group['max_ns'] = max(group['max_ns'], last['max_ns'])
    result = []
    for key in sorted(groups):
        row = groups[key]
        count = row['count']
        intervals = row.pop('sample_intervals')
        row['kind'] = 'counter' if row['phase'].endswith(('.count', '.calls')) else 'duration'
        row['sample_every'] = 1 if row['kind'] == 'counter' else (
            next(iter(intervals)) if len(intervals) == 1 else None)
        row['mean_us'] = row['sum_ns'] / count / 1000
        for p in (50, 95, 99):
            row[f'p{p}_upper_us'] = percentile(row['buckets'], count, p)
        # A cumulative maximum cannot be subtracted when excluding warmup.
        row['max_lifetime_us'] = row.pop('max_ns') / 1000
        row.pop('buckets')
        result.append(row)
    if not result:
        raise ValueError('no observations after the selected start sequence')
    return result, sorted(set(warnings))


def summarize_details(paths, snapshots, start_seq=0, group_by='role'):
    samples, statuses = {}, {}
    for file in log_files(paths):
        with file.open(errors='replace') as stream:
            for number, line in enumerate(stream, 1):
                marker = next((m for m in ('[KVTEST_DETAIL_SAMPLE] ', '[KVTEST_DETAIL_STATUS] ') if m in line), None)
                if marker is None:
                    continue
                try:
                    row = json.loads(line.split(marker, 1)[1])
                    if row['schema'] != 1 or type(row['seq']) is not int or row['seq'] < 0:
                        raise ValueError('invalid detail schema/sequence')
                    if 'STATUS' in marker:
                        if type(row['dropped_lifetime']) is not int or row['dropped_lifetime'] < 0:
                            raise ValueError('invalid dropped count')
                        key = (row['instance'], row['seq'])
                        target = statuses
                    else:
                        for key in ('id', 'start_ns', 'duration_ns'):
                            if type(row[key]) is not int or row[key] < 0:
                                raise ValueError('invalid sample counter: ' + key)
                        if not row['group'].startswith(('ds.detail.', 'mc.detail.')):
                            raise ValueError('invalid sample group')
                        if type(row['overflow']) is not bool or len(row['steps']) > 32:
                            raise ValueError('invalid sample steps/overflow')
                        for step in row['steps']:
                            if (not step['phase'].startswith(('ds.detail.', 'mc.detail.')) or
                                    type(step['duration_ns']) is not int or step['duration_ns'] < 0):
                                raise ValueError('invalid sample step')
                        total = sum(s['duration_ns'] for s in row['steps'])
                        if total > row['duration_ns'] or (row['steps'] and not row['overflow'] and
                                                         total != row['duration_ns']):
                            raise ValueError('sample checkpoints do not reconcile')
                        key = (row['instance'], row['id'])
                        target = samples
                    if key in target and target[key] != row:
                        raise ValueError('conflicting duplicate detail record')
                    target[key] = row
                except (ValueError, KeyError, TypeError) as error:
                    raise ValueError(f'{file}:{number}: {error}') from error
    clients = {process_identity(r['instance']) for r in snapshots if '.api.' in r['phase']}
    groups, warnings = {}, []
    for sample in samples.values():
        if sample['seq'] <= start_seq:
            continue
        if sample['overflow']:
            warnings.append('detail frame overflow: ' + sample['group'])
            continue
        if not sample['steps']:
            continue
        source = sample['instance'] if group_by == 'instance' else (
            'client' if process_identity(sample['instance']) in clients else 'service')
        group = groups.setdefault((source, sample['group']), dict(count=0, total=0, steps={}))
        group['count'] += 1
        group['total'] += sample['duration_ns']
        for step in sample['steps']:
            item = group['steps'].setdefault(step['phase'], [0, 0])
            item[0] += step['duration_ns']
            item[1] += 1
    rows = []
    for (source, name), group in sorted(groups.items()):
        for phase, (ns, visits) in sorted(group['steps'].items(), key=lambda p: -p[1][0]):
            rows.append(dict(source=source, group=name, phase=phase, sampled_frames=group['count'],
                             phase_visits=visits, mean_group_us=group['total'] / group['count'] / 1000,
                             mean_per_frame_us=ns / group['count'] / 1000,
                             mean_per_visit_us=ns / visits / 1000,
                             share_of_group_pct=ns / group['total'] * 100 if group['total'] else 0))
    for status in statuses.values():
        if status['seq'] > start_seq and status['dropped_lifetime']:
            warnings.append(status['instance'] + ': bounded detail sample buffer dropped records; '
                            'checkpoint samples are partial, aggregate stage counters are retained')
    rates = {r.get('detail_sample_every') for r in snapshots if '.detail.' in r['phase']}
    if len(rates) > 1:
        warnings.append('detail sampling intervals differ between sources: ' + str(sorted(rates, key=str)))
    if not rates:
        warnings.append('no third-level detail counters; check SDK, worker, master and Mooncake/TE artifacts')
    if not samples:
        warnings.append('no checkpoint samples; set KVTEST_DETAIL_SAMPLES=1 to inspect same-scope time accounting')
    return rows, list(samples.values()), sorted(set(warnings))


def read_resources(paths, start_seq=0):
    processes = {}
    for file in log_files(paths):
        with file.open(errors='replace') as stream:
            for number, line in enumerate(stream, 1):
                marker = '[KVTEST_PROFILE_RESOURCE] '
                if marker not in line:
                    continue
                try:
                    row = json.loads(line.split(marker, 1)[1])
                    fields = ('seq', 'monotonic_ns', 'user_cpu_us', 'system_cpu_us', 'voluntary_switches',
                              'involuntary_switches', 'minor_faults', 'major_faults')
                    if row['schema'] != 1 or any(type(row[k]) is not int or row[k] < 0 for k in fields):
                        raise ValueError('invalid resource snapshot')
                    if row['seq'] >= start_seq:
                        processes.setdefault(process_identity(row['instance']), []).append(row)
                except (ValueError, KeyError, TypeError) as error:
                    raise ValueError(f'{file}:{number}: {error}') from error
    result = []
    for process, rows in sorted(processes.items()):
        rows.sort(key=lambda r: r['monotonic_ns'])
        first, last = rows[0], rows[-1]
        elapsed_us = (last['monotonic_ns'] - first['monotonic_ns']) / 1000
        if elapsed_us <= 0:
            continue
        cpu_us = last['user_cpu_us'] + last['system_cpu_us'] - first['user_cpu_us'] - first['system_cpu_us']
        if cpu_us < 0:
            raise ValueError('nonmonotonic process CPU counters: ' + process)
        result.append(dict(process=process, observed_wall_us=elapsed_us, cpu_us=cpu_us,
                           average_cpu_cores=cpu_us / elapsed_us,
                           voluntary_switches=last['voluntary_switches'] - first['voluntary_switches'],
                           involuntary_switches=last['involuntary_switches'] - first['involuntary_switches'],
                           minor_faults=last['minor_faults'] - first['minor_faults'],
                           major_faults=last['major_faults'] - first['major_faults']))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='+', type=Path)
    parser.add_argument('--start-seq', type=int, default=0,
                        help='subtract cumulative snapshots through this sequence (e.g. 1 skips the first tick)')
    parser.add_argument('--group-by', choices=('role', 'instance'), default='role')
    parser.add_argument('--json', type=Path)
    parser.add_argument('--csv', type=Path)
    parser.add_argument('--details-json', type=Path, help='rank checkpoints within each sampled function scope')
    parser.add_argument('--samples-jsonl', type=Path, help='preserve bounded same-scope samples, not a distributed trace')
    parser.add_argument('--require-details', action='store_true')
    args = parser.parse_args()
    if args.start_seq < 0:
        parser.error('--start-seq must be nonnegative')
    try:
        snapshots = read_snapshots(args.paths)
        rows, warnings = summarize(snapshots, args.start_seq, args.group_by)
        details, samples, detail_warnings = summarize_details(args.paths, snapshots, args.start_seq, args.group_by)
        resources = read_resources(args.paths, args.start_seq)
        warnings += detail_warnings
        if args.require_details and not any('.detail.' in r['phase'] for r in rows):
            raise ValueError('required detail counters are missing')
    except (ValueError, OSError) as error:
        parser.error(str(error))
    if args.json:
        args.json.write_text(json.dumps({'stages': rows, 'resources': resources, 'warnings': warnings}, indent=2) + '\n')
    if args.csv:
        with args.csv.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    if args.details_json:
        args.details_json.write_text(json.dumps({'checkpoints': details, 'warnings': detail_warnings,
            'boundary': 'same function scope; nested scopes are not additive; sample buffer may drop records'},
            indent=2) + '\n')
    if args.samples_jsonl:
        args.samples_jsonl.write_text(''.join(json.dumps(r) + '\n' for r in sorted(
            samples, key=lambda r: (r['instance'], r['id'])) if r['seq'] > args.start_seq))
    print('| Source | Phase | Count | Mean us | P50 ≤us | P95 ≤us | P99 ≤us |')
    print('|---|---|---:|---:|---:|---:|---:|')
    for row in rows:
        if row['kind'] == 'counter':
            print(f"| {row['source']} | {row['phase']} | {row['count']} | — | — | — | — |")
            continue
        quantiles = ['overflow' if row[f'p{p}_upper_us'] is None else f"{row[f'p{p}_upper_us']:.3f}"
                     for p in (50, 95, 99)]
        print(f"| {row['source']} | {row['phase']} | {row['count']} | {row['mean_us']:.3f} | "
              + ' | '.join(quantiles) + ' |')
    for warning in warnings:
        print('WARNING: ' + warning, file=sys.stderr)
    print('Phases may nest. RPC includes remote work; percentile columns are bucket upper bounds. '
          'Unknown outcomes are included. Missing phases are not zero.', file=sys.stderr)


if __name__ == '__main__':
    main()
