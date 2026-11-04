#!/usr/bin/env python3
"""Collect UB enumeration evidence in a temporary Pod cloned from a failed target."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import time
import uuid


PROBE = r'''
import glob
import os
from pathlib import Path
import shutil
import subprocess

def run(args, timeout=15, env=None):
    print('COMMAND', ' '.join(args), flush=True)
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=timeout, env=env)
        output = p.stdout + p.stderr
        if args[0] == 'ldd':
            output = '\n'.join(x for x in output.splitlines()
                               if any(s in x.lower() for s in ('urma', 'udma', 'ubagg', 'not found', 'undefined')))
        print(output[:24000], flush=True)
        print('RETURN_CODE', p.returncode, flush=True)
        return p.returncode
    except (OSError, subprocess.TimeoutExpired) as e:
        print(type(e).__name__, str(e), flush=True)
        return -1

for folder in ('/dev/uburma', '/sys/class/ubcore', '/sys/class/uburma'):
    p = Path(folder)
    names = sorted(x.name for x in p.iterdir()) if p.exists() else []
    print('DEVICE_DIRECTORY', folder, 'count=', len(names), 'first16=', names[:16], flush=True)
for dev in sorted(Path('/dev/uburma').glob('bonding_dev_*')):
    print('ALLOCATED', dev, 'character_device=', dev.is_char_device(), flush=True)
    for folder in ('/sys/class/ubcore', '/sys/class/uburma'):
        p = Path(folder) / dev.name
        print('SYSFS', str(p), 'exists=', p.exists(), 'resolved=', str(p.resolve()), flush=True)

try:
    import mooncake.store
    print('MOONCAKE_MODULE', mooncake.store.__file__, flush=True)
except Exception as e:
    print('MOONCAKE_IMPORT_ERROR', str(e), flush=True)
maps = Path('/proc/self/maps').read_text()
loaded = sorted({line.split()[-1] for line in maps.splitlines()
                 if any(s in line.lower() for s in ('urma', 'udma', 'ubagg'))})
print('MOONCAKE_LOADED_UB_LIBRARIES', loaded, flush=True)
folders = {'/usr/lib64/urma', '/usr/lib/urma', '/usr/local/lib64/urma', '/usr/local/lib/urma'}
folders.update(str(Path(p).parent / 'urma') for p in loaded if 'liburma.so' in p)
for folder in sorted(folders):
    files = sorted(glob.glob(folder + '/*'))
    print('PROVIDER_DIRECTORY', folder, files, flush=True)
    for p in files[:12]:
        if '.so' in p:
            run(['ldd', '-r', p], timeout=8)
if shutil.which('urma_admin'):
    run(['urma_admin', 'show'])
else:
    print('urma_admin not installed', flush=True)

source = r"""
#include <cstdio>
#include <cerrno>
#include <urma_api.h>
int main() {
    urma_init_attr_t attr = {};
    auto rc = urma_init(&attr);
    std::printf("NATIVE_URMA init=%d\n", (int)rc);
    if (rc != URMA_SUCCESS) return 2;
    int count = 0;
    errno = 0;
    auto list = urma_get_device_list(&count);
    std::printf("NATIVE_URMA count=%d list=%p errno=%d\n", count, (void*)list, errno);
    if (list) {
        for (int i=0; i<count; ++i) std::printf("NATIVE_DEVICE %s\n", list[i]->name);
        urma_free_device_list(list);
    }
    urma_uninit();
    return count > 0 ? 0 : 3;
}
"""
Path('/tmp/ub_probe.cpp').write_text(source)
headers = ('/usr/include/ub/umdk/urma', '/usr/include/umdk', '/usr/include/urma')
include = next((p for p in headers if Path(p, 'urma_api.h').is_file()), None)
if include and shutil.which('g++'):
    rc = run(['g++', '/tmp/ub_probe.cpp', '-I' + include, '-lurma', '-o', '/tmp/ub_probe'], timeout=30)
    if rc == 0:
        run(['ldd', '/tmp/ub_probe'])
        run(['/tmp/ub_probe'])
else:
    print('Native probe unavailable: compiler or URMA headers missing', flush=True)
print('DIAGNOSTICS_COLLECTED (not a UB performance test)', flush=True)
'''


def diagnostic_pod(source, name):
    spec = copy.deepcopy(source['spec'])
    if not spec.get('nodeName') or len(spec.get('containers', [])) != 1 or spec.get('initContainers'):
        raise ValueError('Expected a single-container target with nodeName and no init containers')
    container = spec['containers'][0]
    limits = container.get('resources', {}).get('limits', {})
    if not any('ub' in k.lower() and '/' in k and str(v) != '0' for k, v in limits.items()):
        raise ValueError('Target has no UB resource allocation; do not select the master Pod')
    for key in ('args', 'readinessProbe', 'livenessProbe', 'startupProbe', 'lifecycle', 'ports'):
        container.pop(key, None)
    container['command'] = ['python3', '-u', '-c', PROBE]
    spec.update(restartPolicy='Never', activeDeadlineSeconds=180, terminationGracePeriodSeconds=5)
    return dict(apiVersion='v1', kind='Pod', metadata=dict(name=name,
                namespace=source['metadata'].get('namespace', 'mooncake-perf'),
                labels={'app':'mooncake-ub-diagnostic'}), spec=spec)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--manifest', type=Path, help='saved *-target.json (works after target is deleted)')
    source.add_argument('--pod', help='existing failed target Pod name')
    parser.add_argument('--namespace', default='mooncake-perf', help='namespace for --pod')
    parser.add_argument('--image', help='test a repaired image with the saved target allocation')
    args = parser.parse_args(argv)
    if args.pod:
        original = json.loads(subprocess.check_output(
            ['kubectl', '-n', args.namespace, 'get', 'pod', args.pod, '-o', 'json'], text=True))
    else:
        path = args.manifest
        if path is None:
            paths = list(Path('results').rglob('*-target.json'))
            if not paths:
                parser.error('No saved target found; pass --manifest or --pod')
            path = max(paths, key=lambda p: p.stat().st_mtime_ns)
        print('Source target manifest:', path, flush=True)
        original = json.loads(path.read_text(encoding='utf-8'))
    name = 'mc-ub-check-' + uuid.uuid4().hex[:12]
    pod = diagnostic_pod(original, name)
    if args.image:
        pod['spec']['containers'][0]['image'] = args.image
    namespace = pod['metadata']['namespace']
    base = ['kubectl', '-n', namespace]
    log = Path(name + '.log')
    print('Diagnostic Pod:', name, 'node:', pod['spec']['nodeName'], 'log:', log, flush=True)
    try:
        subprocess.run(base + ['create', '-f', '-'], input=json.dumps(pod), text=True, check=True, timeout=30)
        deadline = time.monotonic() + 210
        while time.monotonic() < deadline:
            state = json.loads(subprocess.check_output(base + ['get', 'pod', name, '-o', 'json'], text=True, timeout=15))
            if state['status'].get('phase') in ('Succeeded', 'Failed'):
                break
            time.sleep(2)
        output = subprocess.run(base + ['logs', name], capture_output=True, text=True, timeout=20)
        detail = subprocess.run(base + ['describe', 'pod', name], capture_output=True, text=True, timeout=20)
        text = output.stdout + output.stderr
        # Keep scheduling/device-plugin failures as evidence when no container starts.
        log.write_text(text + '\nPOD_DETAILS\n' + detail.stdout + detail.stderr, encoding='utf-8')
        print(text, flush=True)
        print('Saved diagnostics:', log, flush=True)
        return 0 if 'DIAGNOSTICS_COLLECTED' in text else 1
    finally:
        subprocess.run(base + ['delete', 'pod', name, '--ignore-not-found', '--wait=false'], timeout=30)


if __name__ == '__main__':
    raise SystemExit(main())
