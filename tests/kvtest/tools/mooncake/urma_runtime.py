#!/usr/bin/env python3
"""Prepare and validate URMA providers using only libraries inside the base image."""
import argparse
import ctypes
import os
from pathlib import Path
import shutil
import sys


CORE = ('liburma.so', 'liburma_common.so')


def provider_files(directory):
    return [p for p in sorted(directory.glob('liburma*.so*'))
            if not p.name.startswith(('liburma.so', 'liburma_common.so'))]


def has_required_providers(files):
    return all(any(kind in p.name and p.is_file() for p in files) for kind in ('udma', 'ubagg'))


def load_providers(lib_dir, files):
    mode = os.RTLD_NOW | ctypes.RTLD_GLOBAL
    handles = [ctypes.CDLL(str(lib_dir / name), mode=mode) for name in reversed(CORE)]
    for path in files:
        handles.append(ctypes.CDLL(str(path), mode=mode))
        print('URMA_PROVIDER_LOADED', path)
    print('URMA_PROVIDERS_LOAD_OK (device enumeration requires a UB Pod)')


def install(lib_dir):
    for name in CORE:
        if not (lib_dir / name).is_file():
            raise ValueError(f'Missing URMA core library in base image: {lib_dir / name}')
    scan_dir = lib_dir / 'urma'
    files = provider_files(scan_dir)
    if not has_required_providers(files):
        # Some SDK images flatten providers beside liburma. Populate the scan
        # directory from that same image; never import host libraries.
        flat_files = provider_files(lib_dir)
        if not has_required_providers(flat_files):
            raise ValueError(f'Missing UDMA or UB bonding provider in base image: '
                             f'{scan_dir} or {lib_dir}; use an image with complete URMA libraries')
        scan_dir.mkdir(parents=True, exist_ok=True)
        for path in flat_files:
            dest = scan_dir / path.name
            # A flat alias may already resolve to the scan-directory file.
            if dest.exists() and path.samefile(dest):
                continue
            if dest.is_symlink():
                dest.unlink()
            shutil.copyfile(path, dest)
            dest.chmod(0o644)
        files = provider_files(scan_dir)
    print(f'URMA_PROVIDER_SOURCE=image (scan directory: {scan_dir})')
    load_providers(lib_dir, files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('install',))
    parser.add_argument('--lib-dir', type=Path, default=Path('/usr/lib64'))
    args = parser.parse_args()
    try:
        install(args.lib_dir)
    except (OSError, ValueError) as error:
        print(f'URMA runtime error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
