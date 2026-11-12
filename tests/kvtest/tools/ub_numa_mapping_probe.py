#!/usr/bin/env python3
"""Reproduce NUMA sampling across the mock's fixed memfd remap; no service needed."""
import ctypes
import ctypes.util
import json
import os


def main():
    libc = ctypes.CDLL(None, use_errno=True)
    numa = ctypes.CDLL(ctypes.util.find_library("numa"), use_errno=True)
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
                          ctypes.c_int, ctypes.c_long]
    libc.mmap.restype = ctypes.c_void_p
    libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    libc.memfd_create.argtypes = [ctypes.c_char_p, ctypes.c_uint]
    libc.memfd_create.restype = ctypes.c_int
    libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_ubyte)]
    numa.numa_move_pages.argtypes = [ctypes.c_int, ctypes.c_ulong,
                                   ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_int),
                                   ctypes.POINTER(ctypes.c_int), ctypes.c_int]
    size, stride = 4 * 1024 * 1024, 1024 * 1024
    page_size = os.sysconf("SC_PAGE_SIZE")
    address = libc.mmap(None, size, 3, 0x22, -1, 0)
    if address == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_errno(), "anonymous mmap")
    fd = None
    try:
        pages = (ctypes.c_void_p * (size // stride))(
            *(address + offset for offset in range(0, size, stride)))

        def touch():
            for offset in range(0, size, page_size):
                ctypes.c_ubyte.from_address(address + offset).value = 0x5A

        def observe():
            status = (ctypes.c_int * len(pages))(*([-999] * len(pages)))
            result = numa.numa_move_pages(0, len(pages), pages, None, status, 0)
            if result != 0:
                raise OSError(ctypes.get_errno(), "numa_move_pages query")
            resident = (ctypes.c_ubyte * (size // page_size))()
            if libc.mincore(address, size, resident) != 0:
                raise OSError(ctypes.get_errno(), "mincore")
            return {"numa_status": list(status),
                    "resident_pages": sum(value & 1 for value in resident)}

        touch()
        before = observe()
        fd = libc.memfd_create(b"ub-numa-remap-probe", 1)
        if fd < 0:
            raise OSError(ctypes.get_errno(), "memfd_create")
        os.ftruncate(fd, size)
        mapped = libc.mmap(address, size, 3, 0x11, fd, 0)
        if mapped != address:
            raise OSError(ctypes.get_errno(), "fixed memfd mmap")
        after = observe()
        touch()
        restored = observe()
        assert all(node >= 0 for node in before["numa_status"]), before
        assert after["numa_status"] == [-2] * len(pages), after
        assert after["resident_pages"] == 0, after
        assert all(node >= 0 for node in restored["numa_status"]), restored
        print(json.dumps({"bytes": size, "sample_stride": stride, "before_remap": before,
                          "after_remap": after, "after_touch": restored, "passed": True}))
    finally:
        libc.munmap(address, size)
        if fd is not None:
            os.close(fd)


if __name__ == "__main__":
    main()
