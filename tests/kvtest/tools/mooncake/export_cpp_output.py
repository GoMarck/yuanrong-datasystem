#!/usr/bin/env python3
"""Resume-safe copy of prebuilt Mooncake C++ inputs from an image's /opt tree."""

import hashlib
import os
import shutil
import sys
import tempfile
from pathlib import Path


HEADER_DIRS = (
    'Mooncake/mooncake-common/etcd',
    'Mooncake/mooncake-common/k8s-lease',
    'Mooncake/mooncake-common/include',
    'Mooncake/mooncake-common/src',
    'Mooncake/mooncake-transfer-engine/include',
    'Mooncake/mooncake-store/include',
    'Mooncake/build-ub/mooncake-store/include',
    'Mooncake/build-ub/_deps/yalantinglibs-src/include',
    'mooncake-bench/.deps/gflags-install/include',
    'mooncake-bench/.deps/glog-install/include',
    'mooncake-bench/.deps/msgpack-install/include',
)
HEADER_SUFFIXES = {'.h', '.hpp', '.hxx', '.inc', '.inl', '.ipp', '.tcc'}
REQUIRED_FILES = (
    'Mooncake/mooncake-store/include/real_client.h',
    'Mooncake/build-ub/source_revision.txt',
    'Mooncake/build-ub/CMakeCache.txt',
    'Mooncake/build-ub/mooncake-store/src/libmooncake_store.a',
    'Mooncake/build-ub/mooncake-transfer-engine/src/libtransfer_engine.a',
    'Mooncake/build-ub/mooncake-transfer-engine/src/common/base/libbase.a',
    'Mooncake/build-ub/mooncake-store/src/local_ssd/libmooncake_local_ssd.a',
    'Mooncake/build-ub/mooncake-store/src/cachelib_memory_allocator/libcachelib_memory_allocator.a',
    'Mooncake/build-ub/mooncake-common/src/libmooncake_common.a',
    'Mooncake/build-ub/mooncake-common/libasio.so',
    'Mooncake/build-ub/mooncake-store/src/mooncake_master',
    'mooncake-bench/.deps/msgpack-install/include/msgpack.hpp',
)
SHARED_DIRS = (
    'mooncake-bench/.deps/gflags-install/lib',
    'mooncake-bench/.deps/gflags-install/lib64',
    'mooncake-bench/.deps/glog-install/lib',
    'mooncake-bench/.deps/glog-install/lib64',
)


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def source_files(source):
    paths = set(REQUIRED_FILES)
    # Native client's flags/link closure are useful when upgrading SDK releases.
    for name in ('flags.make', 'link.txt'):
        relative = 'Mooncake/build-ub/mooncake-store/src/CMakeFiles/mooncake_client.dir/' + name
        if (source / relative).is_file():
            paths.add(relative)
    for relative in HEADER_DIRS:
        directory = source / relative
        if directory.is_dir():
            for path in directory.rglob('*'):
                if path.is_file() and path.suffix.lower() in HEADER_SUFFIXES:
                    paths.add(path.relative_to(source).as_posix())
    for relative in SHARED_DIRS:
        directory = source / relative
        if directory.is_dir():
            for path in directory.glob('lib*.so*'):
                if path.is_file():
                    paths.add(path.relative_to(source).as_posix())
    missing = [name for name in REQUIRED_FILES if not (source / name).is_file()]
    if missing:
        raise ValueError('Mooncake image output missing: ' + ', '.join(missing))
    if not any('gflags-install' in p and '/libgflags.so' in p for p in paths):
        raise ValueError('Mooncake image output missing libgflags.so')
    if not any('glog-install' in p and '/libglog.so' in p for p in paths):
        raise ValueError('Mooncake image output missing libglog.so')
    return sorted(paths)


def atomic_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile('w', dir=path.parent, prefix='.export-',
                                     delete=False, encoding='utf-8') as stream:
        temporary = Path(stream.name)
        stream.write(text)
    os.replace(temporary, path)


def copy_with_progress(original, temporary, name, size):
    copied = 0
    next_report = 128 * 1024 * 1024
    with original.open('rb') as reader, temporary.open('wb') as writer:
        for block in iter(lambda: reader.read(4 * 1024 * 1024), b''):
            writer.write(block)
            copied += len(block)
            if size >= 256 * 1024 * 1024 and copied >= next_report:
                print(f'  {name}: {copied / 2**20:.0f}/{size / 2**20:.0f} MiB',
                      flush=True)
                next_report += 128 * 1024 * 1024
    shutil.copymode(original, temporary)


def export(source, output, image):
    source, output = Path(source), Path(output)
    if not source.is_dir():
        raise ValueError('source /opt tree does not exist: ' + str(source))
    image_marker = output / 'source_image.txt'
    if image_marker.is_file() and image_marker.read_text().strip() != image:
        raise ValueError('output belongs to another source image')
    for name in ('Mooncake/build-ub/source_revision.txt',
                 'Mooncake/build-ub/CMakeCache.txt'):
        current = output / name
        original = source / name
        if current.exists() and original.exists() and digest(current) != digest(original):
            raise ValueError('output has a different revision or CMake configuration: ' + name)
    paths = source_files(source)
    copied = 0
    checksums = {}
    for name in paths:
        original, current = source / name, output / name
        size = original.stat().st_size
        is_library = name.endswith('.a') or '.so' in Path(name).name
        if is_library or size >= 64 * 1024 * 1024:
            print(f'Checking {name} ({size / 2**20:.1f} MiB)', flush=True)
        same = current.is_file() and current.stat().st_size == size
        if same:
            source_hash = digest(original)
            same = digest(current) == source_hash
            if same and is_library:
                checksums[name] = source_hash
        if same:
            continue
        if is_library or size >= 64 * 1024 * 1024:
            print(f'Copying  {name} ({size / 2**20:.1f} MiB)', flush=True)
        current.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=current.parent, prefix='.export-',
                                         delete=False) as stream:
            temporary = Path(stream.name)
        try:
            copy_with_progress(original, temporary, name, size)
            os.replace(temporary, current)
        finally:
            temporary.unlink(missing_ok=True)
        copied += 1
        if copied % 1000 == 0:
            print(f'Copied {copied} files', flush=True)
    for name in paths:
        if name.endswith('.a') or '.so' in Path(name).name:
            if name not in checksums:
                print(f'Hashing  {name}', flush=True)
                checksums[name] = digest(output / name)
    atomic_text(output / 'libraries.sha256', ''.join(
        f'{checksums[name]}  {name}\n' for name in sorted(checksums)))
    atomic_text(image_marker, image + '\n')
    print(f'Export complete: {len(paths)} files, {copied} copied', flush=True)


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit('usage: export_cpp_output.py SOURCE_ROOT OUTPUT_DIR IMAGE')
    export(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3])
