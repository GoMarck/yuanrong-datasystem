#!/usr/bin/env python3
"""Bounded read-only thread CPU sampling for a live KVTest client Pod."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def parse_task_stat(text):
    fields = text[text.rfind(')') + 2:].split()
    return int(fields[11]), int(fields[12]), int(fields[19])


def snapshot(pid, proc=Path('/proc')):
    tasks = {}
    for task in (proc / str(pid) / 'task').glob('[0-9]*'):
        try:
            user, system, start = parse_task_stat((task / 'stat').read_text())
            tasks[int(task.name)] = {'start_ticks': start, 'user_ticks': user, 'system_ticks': system,
                                     'name': (task / 'comm').read_text().strip()}
        except (OSError, ValueError, IndexError):
            continue
        try:
            tasks[int(task.name)]['wchan'] = (task / 'wchan').read_text().strip()
        except OSError:
            tasks[int(task.name)]['wchan'] = 'unavailable'
        try:
            for line in (task / 'status').read_text().splitlines():
                if line.startswith('Cpus_allowed_list:'):
                    tasks[int(task.name)]['allowed_cpus'] = line.split(':', 1)[1].strip()
        except OSError:
            pass
    return tasks


def thread_deltas(before, after, elapsed, ticks_per_second):
    rows = []
    for tid, b in after.items():
        a = before.get(tid)
        if not a or a['start_ticks'] != b['start_ticks']:
            continue
        user, system = b['user_ticks'] - a['user_ticks'], b['system_ticks'] - a['system_ticks']
        if min(user, system) < 0:
            continue
        scale = 100 / ticks_per_second / elapsed
        rows.append({'tid': tid, 'name': b['name'], 'cpu_pct': (user + system) * scale,
                     'user_pct': user * scale, 'system_pct': system * scale,
                     'end_wchan': b.get('wchan'), 'allowed_cpus': b.get('allowed_cpus')})
    return sorted(rows, key=lambda row: row['cpu_pct'], reverse=True)


def sample_threads(seconds):
    pids = []
    for proc in Path('/proc').glob('[0-9]*'):
        try:
            if (proc / 'comm').read_text().strip() == 'kvtest':
                pids.append(int(proc.name))
        except OSError:
            continue
    if len(pids) != 1:
        raise RuntimeError(f'Expected one live kvtest process, found {pids}, sample while the test is running')
    pid = pids[0]
    start_identity = parse_task_stat(Path(f'/proc/{pid}/stat').read_text())[2]
    started = datetime.now(timezone.utc).isoformat()
    before = snapshot(pid)
    begin = time.monotonic()
    time.sleep(seconds)
    after = snapshot(pid)
    elapsed = time.monotonic() - begin
    if not after or parse_task_stat(Path(f'/proc/{pid}/stat').read_text())[2] != start_identity:
        raise RuntimeError('KVTest exited or restarted during thread sampling')
    rows = thread_deltas(before, after, elapsed, os.sysconf('SC_CLK_TCK'))
    image_options = {}
    try:
        numa0_cpus = Path('/sys/devices/system/node/node0/cpulist').read_text().strip()
    except OSError:
        numa0_cpus = None
    try:
        for line in Path('/opt/Mooncake/build-ub/CMakeCache.txt').read_text().splitlines():
            for key in ('USE_EVENT_DRIVEN_COMPLETION', 'USE_UB', 'USE_TENT'):
                if line.startswith(key + ':') and '=' in line:
                    image_options[key] = line.split('=', 1)[1]
    except OSError:
        pass
    return {'pid': pid, 'started_at': started, 'elapsed_seconds': elapsed, 'threads_before': len(before),
            'threads_after': len(after), 'sampled_cpu_cores': sum(r['cpu_pct'] for r in rows) / 100,
            'image_build_options': image_options,
            'numa0_cpus': numa0_cpus,
            'cpu_allowed_lists': dict(Counter(task.get('allowed_cpus') or 'unavailable' for task in after.values())),
            'scope': '100% means one logical CPU. Threads present at both snapshots only. '
                     'Names and end_wchan do not identify user-space call stacks.', 'threads': rows}


def print_report(result, top=20):
    print(f"Thread CPU: PID={result['pid']} window={result['elapsed_seconds']:.2f}s "
          f"sampled cores={result['sampled_cpu_cores']:.2f} threads={result['threads_after']}")
    print('Image build options:', result.get('image_build_options', {}), '(not proof of linked SDK build options)')
    if 'cpu_allowed_lists' in result:
        print('NUMA 0 CPUs:', result.get('numa0_cpus'))
        print('Thread CPU masks (mask -> thread count):', result['cpu_allowed_lists'])
    print('TID       CPU%    User%  System%  Name / end_wchan')
    for row in result['threads'][:top]:
        print(f"{row['tid']:<8} {row['cpu_pct']:7.2f} {row['user_pct']:7.2f} {row['system_pct']:7.2f}  "
              f"{row['name']} / {row['end_wchan']}")
    print(result['scope'])


def capture_client(run, namespace, case_dir):
    pods = json.loads((case_dir / 'pods.json').read_text())['items']
    pod = sorted(p['metadata']['name'] for p in pods if p['metadata']['name'].startswith('mc-client-'))[0]
    record = {'pod': pod, 'diagnostic': True}
    try:
        source = Path(__file__).read_text(encoding='utf-8')
        result = run.command(['kubectl', '-n', namespace, 'exec', pod, '--', 'python3', '-c', source,
                              '--probe', '--seconds', '10'], capture=True, timeout=30,
                             display=f'kubectl -n {namespace} exec {pod} -- python3 [10s thread CPU probe]')
        record.update(json.loads(result))
        print_report(record)
    except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        record['error'] = str(error)
        print(f'Thread CPU probe unavailable: {error}', flush=True)
    (case_dir / 'thread-cpu.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-n', '--namespace')
    parser.add_argument('-p', '--pod')
    parser.add_argument('--seconds', type=int, default=10)
    parser.add_argument('--probe', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if not 1 <= args.seconds <= 30:
        parser.error('--seconds must be between 1 and 30')
    try:
        if args.probe:
            print(json.dumps(sample_threads(args.seconds)))
        else:
            if not args.namespace or not args.pod:
                parser.error('--namespace and --pod are required')
            source = Path(__file__).read_text(encoding='utf-8')
            result = subprocess.run(['kubectl', '-n', args.namespace, 'exec', args.pod, '--', 'python3',
                                     '-c', source, '--probe', '--seconds', str(args.seconds)],
                                    capture_output=True, text=True, timeout=args.seconds + 20)
            if result.returncode:
                raise RuntimeError(result.stderr or result.stdout)
            print_report(json.loads(result.stdout))
        return 0
    except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        print(f'Thread CPU probe failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
