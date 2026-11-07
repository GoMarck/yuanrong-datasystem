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


def read_snapshots(paths):
    snapshots = {}
    for path in paths:
        root = Path(path)
        files = sorted(p for p in root.rglob('*') if p.is_file() and
                       (p.suffix in ('.log', '.jsonl', '.out') or '.INFO.' in p.name or '.log.' in p.name)) \
            if root.is_dir() else [root]
        for file in files:
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
    client_instances = {r['instance'] for r in snapshots if '.api.' in r['phase']}
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
        role = 'client' if instance in client_instances else 'service'
        key = (phase, instance if group_by == 'instance' else role)
        group = groups.setdefault(key, dict(phase=phase, source=key[1],
                                          **{k: 0 for k in COUNTERS},
                                          buckets=[0] * len(BUCKET_NS), max_ns=0))
        for name, value in values.items():
            group[name] += value
        group['buckets'] = [a + b for a, b in zip(group['buckets'], buckets)]
        group['max_ns'] = max(group['max_ns'], last['max_ns'])
    result = []
    for key in sorted(groups):
        row = groups[key]
        count = row['count']
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='+', type=Path)
    parser.add_argument('--start-seq', type=int, default=0,
                        help='subtract cumulative snapshots through this sequence (e.g. 1 skips the first tick)')
    parser.add_argument('--group-by', choices=('role', 'instance'), default='role')
    parser.add_argument('--json', type=Path)
    parser.add_argument('--csv', type=Path)
    args = parser.parse_args()
    if args.start_seq < 0:
        parser.error('--start-seq must be nonnegative')
    try:
        rows, warnings = summarize(read_snapshots(args.paths), args.start_seq, args.group_by)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    if args.json:
        args.json.write_text(json.dumps({'stages': rows, 'warnings': warnings}, indent=2) + '\n')
    if args.csv:
        with args.csv.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    print('| Source | Phase | Count | Mean us | P50 ≤us | P95 ≤us | P99 ≤us |')
    print('|---|---|---:|---:|---:|---:|---:|')
    for row in rows:
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
