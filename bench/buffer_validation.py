# SPDX-License-Identifier: Apache-2.0
"""Bounded, deterministic full-content verification for benchmark buffers."""

import ctypes
import hashlib
import json

CHUNK_BYTES = 1024 * 1024
INVERT = bytes.maketrans(bytes(range(256)), bytes(reversed(range(256))))


def chunks(keys, key_bytes, seed):
    for slot, key in enumerate(keys):
        for offset in range(0, key_bytes, CHUNK_BYTES):
            size = min(CHUNK_BYTES, key_bytes - offset)
            identity = json.dumps([seed, key, offset], ensure_ascii=True).encode()
            yield key, offset, slot * key_bytes + offset, hashlib.shake_256(identity).digest(size)


def write_bytes(buffer, offset, payload):
    if buffer.torch is None:
        ctypes.memmove(buffer.address + offset, payload, len(payload))
    else:
        torch = buffer.torch
        source = torch.frombuffer(bytearray(payload), dtype=torch.uint8)
        buffer.memory[offset:offset + len(payload)].copy_(source)


def fill(buffer, keys, args, poison=False):
    for _, _, offset, expected in chunks(keys, args.key_bytes, args.seed):
        write_bytes(buffer, offset, expected.translate(INVERT) if poison else expected)
    if buffer.torch is not None:
        buffer.torch.npu.synchronize()


def verify(buffer, keys, args):
    if buffer.torch is not None:
        buffer.torch.npu.synchronize()
    for key, key_offset, offset, expected in chunks(keys, args.key_bytes, args.seed):
        if buffer.torch is None:
            actual = ctypes.string_at(buffer.address + offset, len(expected))
        else:
            actual = buffer.memory[offset:offset + len(expected)].cpu().numpy().tobytes()
        if actual != expected:
            mismatch = next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b)
            byte = key_offset + mismatch
            block_size = args.key_bytes // args.blocks_per_key
            raise RuntimeError(f"Data mismatch: key={key} block={byte // block_size} "
                               f"byte={byte} expected={expected[mismatch]} actual={actual[mismatch]}")


def annotate(stats, args):
    stats["verification"] = dict(enabled=args.verify_data,
                                 verified_keys=stats["keys"] if args.verify_data else 0,
                                 pattern="shake256-key-offset-v1" if args.verify_data else "constant-01",
                                 wall_includes_validation=args.verify_data)
    return stats
