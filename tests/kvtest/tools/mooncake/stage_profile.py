#!/usr/bin/env python3
"""Apply only the reviewed profiler patch to the pinned official Mooncake source."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

BASE = Path(__file__).resolve().parent


def git(source, *args):
    return subprocess.check_output(['git', '-C', str(source), *args], text=True).strip()


def update(source, action='apply', bundle=BASE):
    source = Path(source).resolve()
    manifest = json.loads((bundle / 'stage_profile_manifest.json').read_text())
    patch = bundle / 'stage_profile.patch'
    if hashlib.sha256(patch.read_bytes()).hexdigest() != manifest['patch_sha256']:
        raise ValueError('profiler patch/manifest mismatch')
    if git(source, 'rev-parse', 'HEAD') != manifest['revision']:
        raise ValueError('profiler requires the official v0.3.13.post1 commit')
    if git(source, 'diff', '--cached', '--name-only'):
        raise ValueError('staged source changes found; profiler will not modify the index')
    unexpected = set(git(source, 'diff', 'HEAD', '--name-only').splitlines()) - set(manifest['files'])
    if unexpected:
        raise ValueError('unrelated tracked changes found: ' + ', '.join(sorted(unexpected)))
    states = []
    for name, hashes in manifest['files'].items():
        path = source / name
        path.resolve().relative_to(source)
        if path.is_symlink():
            raise ValueError('profiler source must not be a symlink: ' + name)
        digest = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None
        if digest == hashes['profile']:
            states.append('profile')
        elif digest == hashes['base']:
            states.append('base')
        else:
            raise ValueError('source differs from both pristine and reviewed profiler content: ' + name)
    if len(set(states)) != 1:
        raise ValueError('partial profiler patch found; inspect the source before continuing')
    state = states[0]
    if action == 'check' or (action == 'apply' and state == 'profile') or (action == 'remove' and state == 'base'):
        return state
    args = ['git', '-C', str(source), 'apply']
    if action == 'remove':
        args.append('--reverse')
    subprocess.run([*args, '--check', str(patch)], check=True)
    subprocess.run([*args, str(patch)], check=True)
    return 'profile' if action == 'apply' else 'base'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--action', choices=('apply', 'check', 'remove'), default='apply')
    args = parser.parse_args()
    try:
        state = update(args.source, args.action)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    print('Mooncake stage profiler source state:', state)


if __name__ == '__main__':
    main()
