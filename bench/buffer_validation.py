# SPDX-License-Identifier: Apache-2.0
"""Bounded, deterministic full-content verification for benchmark buffers."""

import ctypes
import hashlib
import json
import time

CHUNK_BYTES = 1024 * 1024
INVERT = bytes.maketrans(bytes(range(256)), bytes(reversed(range(256))))


def chunks(keys, key_bytes, seed):
    for slot, key in enumerate(keys):
        for offset in range(0, key_bytes, CHUNK_BYTES):
            size = min(CHUNK_BYTES, key_bytes - offset)
            identity = json.dumps([seed, key, offset], ensure_ascii=True).encode()
            yield key, offset, slot * key_bytes + offset, hashlib.shake_256(identity).digest(size)


class BatchVerifier:
    def __init__(self, buffer, keys, args):
        self.keys = tuple(keys)
        self.key_bytes = args.key_bytes
        self.block_bytes = args.key_bytes // args.blocks_per_key
        self.identity = (self.keys, args.key_bytes, args.blocks_per_key, args.seed)
        self.expected = bytearray().join(part[3] for part in chunks(keys, args.key_bytes, args.seed))
        self.source = (buffer.torch.frombuffer(self.expected, dtype=buffer.torch.uint8)
                       if buffer.torch is not None else None)
        self.has_expected = False

    def fill(self, buffer, poison=False):
        if buffer.torch is None:
            payload = self.expected.translate(INVERT) if poison else self.expected
            ctypes.memmove(buffer.address, bytes(payload), len(payload))
        else:
            if not poison or not self.has_expected:
                buffer.memory[:len(self.expected)].copy_(self.source)
            if poison:
                buffer.memory[:len(self.expected)].bitwise_not_()
            buffer.torch.npu.synchronize()
        self.has_expected = not poison

    def verify(self, buffer):
        if buffer.torch is None:
            actual = ctypes.string_at(buffer.address, len(self.expected))
        else:
            actual = buffer.memory[:len(self.expected)].cpu().numpy().tobytes()
        if len(actual) != len(self.expected):
            raise RuntimeError(f"Data size mismatch: expected={len(self.expected)} actual={len(actual)}")
        if actual != self.expected:
            offset = next(i for i, (a, b) in enumerate(zip(actual, self.expected)) if a != b)
            slot, byte = divmod(offset, self.key_bytes)
            raise RuntimeError(f"Data mismatch: key={self.keys[slot]} block={byte // self.block_bytes} "
                               f"byte={byte} expected={self.expected[offset]} actual={actual[offset]}")
        self.has_expected = True


def prepared(buffer, keys, args):
    identity = (tuple(keys), args.key_bytes, args.blocks_per_key, args.seed)
    batch = getattr(buffer, "verification_batch", None)
    if batch is None or batch.identity != identity:
        # Drop the previous batch before generating the next; memory is bounded per worker.
        buffer.verification_batch = None
        batch = None
        batch = BatchVerifier(buffer, keys, args)
        buffer.verification_batch = batch
    return batch


def fill(buffer, keys, args, poison=False):
    prepared(buffer, keys, args).fill(buffer, poison)


def verify(buffer, keys, args):
    prepared(buffer, keys, args).verify(buffer)


class Progress:
    def __init__(self, operation, worker, total):
        self.operation, self.worker, self.total = operation, worker, total
        self.last_log = 0.0

    def update(self, batch, stage):
        now = time.monotonic()
        if batch == 1 or now - self.last_log >= 5 or (batch == self.total and stage == "done"):
            print(f"VERIFY_PROGRESS operation={self.operation} worker={self.worker} "
                  f"batch={batch}/{self.total} stage={stage}", flush=True)
            self.last_log = now


def annotate(stats, args):
    stats["verification"] = dict(enabled=args.verify_data,
                                 verified_keys=stats["keys"] if args.verify_data else 0,
                                 verified_bytes=stats["payload_bytes"] if args.verify_data else 0,
                                 verified_blocks=stats["keys"] * args.blocks_per_key if args.verify_data else 0,
                                 pattern="shake256-key-offset-v1" if args.verify_data else "constant-01",
                                 wall_includes_validation=args.verify_data)
    return stats


class ApiCalls:
    def __init__(self):
        self.data = dict(attempts=0, successes=0, errors=0, requested_keys=0,
                         successful_keys=0, requested_payload_bytes=0,
                         successful_payload_bytes=0, latencies_ns=[])

    def invoke(self, keys, payload_bytes, call, validate=None):
        data = self.data
        data["attempts"] += 1
        data["requested_keys"] += len(keys)
        data["requested_payload_bytes"] += payload_bytes
        start = time.perf_counter_ns()
        try:
            result = call()
        except BaseException:
            data["latencies_ns"].append(time.perf_counter_ns() - start)
            data["errors"] += 1
            raise
        elapsed = time.perf_counter_ns() - start
        data["latencies_ns"].append(elapsed)
        try:
            if validate is not None:
                validate(result)
        except BaseException:
            data["errors"] += 1
            raise
        data["successes"] += 1
        data["successful_keys"] += len(keys)
        data["successful_payload_bytes"] += payload_bytes
        return result, elapsed


def require_no_failed_keys(failed):
    if failed:
        raise RuntimeError(f"Read failed keys: {failed}")


def check_blob_layout(result, keys, args):
    failed, metadata = result
    require_no_failed_keys(failed)
    if len(metadata) != len(keys):
        raise RuntimeError(f"Metadata count mismatch: expected={len(keys)} actual={len(metadata)}")
    expected = [args.key_bytes // args.blocks_per_key] * args.blocks_per_key
    for key, info in zip(keys, metadata):
        actual = list(info.blob_size_list)
        if actual != expected:
            raise RuntimeError(f"Blob layout mismatch: key={key} expected={expected} actual={actual}")


def finalize_api_calls(report, aggregate=True):
    for phase in report.get("phases", []):
        for worker in phase["workers"]:
            for data in worker.values():
                values = sorted(value / 1e6 for value in data["latencies_ns"])
                def percentile(percent):
                    position = (len(values) - 1) * percent / 100
                    lower = int(position)
                    upper = min(lower + 1, len(values) - 1)
                    return values[lower] + (values[upper] - values[lower]) * (position - lower)
                data["latency_ms"] = dict(
                    sum=sum(values), avg=sum(values) / len(values) if values else 0,
                    max=max(values, default=0),
                    **{f"p{p}": percentile(p) if values else 0 for p in (50, 90, 99)})

        if not aggregate:
            continue
        grouped = {}
        for worker in phase["workers"]:
            for name, data in worker.items():
                group = grouped.setdefault(name, dict(attempts=0, successes=0, errors=0,
                                                      requested_keys=0, successful_keys=0,
                                                      requested_payload_bytes=0, successful_payload_bytes=0,
                                                      latencies_ns=[]))
                for field in group:
                    if field == "latencies_ns":
                        group[field].extend(data[field])
                    else:
                        group[field] += data[field]
        # Reuse the same latency reducer for the aggregated call roles.
        if grouped:
            finalize_api_calls(dict(phases=[dict(workers=[grouped])]), aggregate=False)
            phase["summary"] = {name: {key: value for key, value in data.items() if key != "latencies_ns"}
                                for name, data in grouped.items()}
