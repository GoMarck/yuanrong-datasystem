#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Ascend NPU multi-buffer benchmark for Mooncake v0.3.11.post1 (optional Host mode)."""

import argparse
import copy
import ctypes
import importlib.metadata
import json
import os
import platform
import random
import signal
import statistics
import sys
import threading
import time
import uuid
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from pathlib import Path

import buffer_validation as validation


def positive(value):
    value = int(value)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def nonnegative(value):
    value = int(value)
    if value < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return value


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--local-hostname", required=True, help="reachable local IP")
    p.add_argument("--master", required=True, help="master IP:port")
    p.add_argument("--metadata-server", default="P2PHANDSHAKE")
    p.add_argument("--protocol", choices=["ascend", "rdma", "tcp"], default="ascend",
                   help="ascend uses NPU buffers; rdma/tcp use Host buffers")
    p.add_argument("--device-id", type=nonnegative, default=0)
    p.add_argument("--prefer-alloc-in-same-node", action=argparse.BooleanOptionalAction,
                   default=None, help="default true for ascend, false for Host mode")
    p.add_argument("--hard-pin", action=argparse.BooleanOptionalAction, default=False,
                   help="prevent eviction (default false, matching backend defaults)")
    p.add_argument("--rdma-devices", default="")
    p.add_argument("--segment-bytes", type=nonnegative, default=None)
    p.add_argument("--local-buffer-bytes", type=nonnegative, default=None)
    p.add_argument("--operation", choices=["put", "get", "put-get"], required=True,
                   help="put/get use HTTP; put-get writes then reads in one process")
    p.add_argument("--http-listen-port", type=positive,
                   help="get server: bind local-hostname and this HTTP port")
    p.add_argument("--exit-after-get", action="store_true",
                   help="get only: exit after one run and the put client's cleanup acknowledgement")
    p.add_argument("--http-server", help="put coordinator: http://B_IP:PORT")
    p.add_argument("--http-timeout", type=positive, default=10,
                   help="timeout in seconds for each HTTP request")
    p.add_argument("--key-prefix", help="put/put-get: PREFIX_1 through PREFIX_N")
    p.add_argument("--num-keys", type=positive, help="put/put-get; default 3200")
    p.add_argument("--seed", type=int, help="put/put-get; reader shuffle seed, default 42")
    p.add_argument("--scenarios", nargs="+", choices=["random", "local"],
                   default=None)
    p.add_argument("--concurrency", nargs="+", type=positive, default=None)
    p.add_argument("--put-keys-per-batch", type=positive, help="put/put-get; write batch size, default 8")
    p.add_argument("--get-keys-per-batch", type=positive, help="put/put-get; read batch size, default 8")
    p.add_argument("--key-bytes", type=positive, help="put/put-get; default 7096320")
    p.add_argument("--blocks-per-key", type=positive, help="put/put-get; default 128")
    p.add_argument("--warmup", type=positive,
                   help="put/put-get; unmeasured batches per worker for each API, default 5")
    p.add_argument("--verify-data", action="store_true",
                   help="verify every batch; HTTP get inherits this switch from put")
    p.add_argument("--output", default="mooncake_multi_buffer_results.json")
    args = p.parse_args(argv)
    if args.exit_after_get and args.operation != "get":
        p.error("--exit-after-get requires --operation get")
    if args.operation == "get":
        if not args.http_listen_port or args.http_listen_port > 65535 or args.http_server:
            p.error("get requires --http-listen-port <= 65535 and forbids --http-server")
        fields = ("key_prefix", "num_keys", "put_keys_per_batch", "get_keys_per_batch",
                  "key_bytes", "blocks_per_key", "warmup", "seed", "scenarios")
        forbidden = ["--" + name.replace("_", "-") for name in fields
                     if getattr(args, name) is not None]
        if forbidden:
            p.error("get receives workload parameters from put; forbidden: " + ", ".join(forbidden))
    else:
        if args.operation == "put" and (not args.http_server or args.http_listen_port):
            p.error("put requires --http-server and forbids --http-listen-port")
        if args.operation == "put-get" and (args.http_server or args.http_listen_port):
            p.error("put-get does not use HTTP; omit --http-server and --http-listen-port")
        defaults = dict(num_keys=3200, put_keys_per_batch=8, get_keys_per_batch=8,
                        key_bytes=7096320, blocks_per_key=128, warmup=5, seed=42,
                        scenarios=["local"])
        for name, value in defaults.items():
            if getattr(args, name) is None:
                setattr(args, name, value)
        # Internal batch helpers operate on the effective batch size for this role.
        args.keys_per_batch = args.put_keys_per_batch
    if args.prefer_alloc_in_same_node is None:
        args.prefer_alloc_in_same_node = args.protocol == "ascend"
    if args.local_buffer_bytes is None:
        args.local_buffer_bytes = (
            1024**3 if args.protocol == "ascend" else 64 * 1024**2)
    args.concurrency = args.concurrency or (
        [1, 4] if args.operation == "get" else [1])
    if args.segment_bytes is None:
        args.segment_bytes = 32 * 1024**3
    if args.operation == "get" and args.segment_bytes == 0:
        p.error("HTTP get server must mount a nonzero --segment-bytes")
    if args.operation in ("put", "put-get"):
        if not args.key_prefix or not args.key_prefix.strip():
            p.error("put/put-get requires a nonempty --key-prefix")
        if args.num_keys % args.put_keys_per_batch or args.num_keys % args.get_keys_per_batch:
            p.error("--num-keys must be divisible by both --put-keys-per-batch and --get-keys-per-batch")
        if args.num_keys // args.keys_per_batch < max(args.concurrency):
            p.error("dataset must contain at least one batch per put worker")
        if (args.operation == "put-get" and
                args.num_keys // args.get_keys_per_batch < max(args.concurrency)):
            p.error("dataset must contain at least one batch per get worker")
        if len(args.concurrency) != 1 or len(args.scenarios) != 1:
            p.error("put/put-get requires exactly one concurrency and one scenario; "
                    "use a fresh prefix for each write test")
        if args.key_bytes % args.blocks_per_key:
            p.error("--key-bytes must be divisible by --blocks-per-key")
        # The version's get binding returns vector<int>, not int64_t.
        if args.key_bytes > 2**31 - 1:
            p.error("--key-bytes must fit a signed 32-bit get return value")
    if args.operation in ("put", "put-get") and "local" in args.scenarios:
        needed = max(args.num_keys, args.concurrency[0] * args.warmup *
                     args.keys_per_batch) * args.key_bytes
        if args.segment_bytes < needed * 1.2:
            p.error(f"local needs at least {int(needed * 1.2) + 1} segment bytes "
                    "(payload plus 20% minimum headroom)")
    return args


def check_results(operation, results, count, expected):
    if len(results) != count or any(x != expected for x in results):
        raise RuntimeError(f"{operation}: expected {count} x {expected}; "
                           f"got {list(results)}")


class StoreRuntime:
    """Own TE + Store, and mirror the three vLLM-Ascend registration paths."""

    def __init__(self, store, args):
        self.store = store
        self.args = args
        self.torch = None
        self.engine = None
        self.path = "host"
        self.live_buffers = {}

    def __getattr__(self, name):
        return getattr(self.store, name)

    def set_device(self):
        if self.torch is not None:
            self.torch.npu.set_device(self.args.device_id)

    def setup(self):
        args = self.args
        hostname = args.local_hostname
        local_buffer = args.local_buffer_bytes
        kwargs = {}
        if args.protocol == "ascend":
            import torch
            import torch_npu  # noqa: F401 - registers the NPU device backend

            self.torch = torch
            self.set_device()
            fabric = os.getenv("ASCEND_ENABLE_USE_FABRIC_MEM", "0") == "1"
            independent = bool(os.getenv("ASCEND_GLOBAL_RESOURCE_CONFIG")) and not fabric
            if fabric or independent:
                self.path = "fabric" if fabric else "store_independent_te"
                local_buffer = 0
            else:
                from mooncake.engine import TransferEngine

                self.path = "shared_te"
                self.engine = TransferEngine()
                # vLLM's global TE uses P2PHANDSHAKE independently of Store metadata.
                rc = self.engine.initialize(hostname, "P2PHANDSHAKE", "ascend", "")
                if rc != 0:
                    raise RuntimeError(f"TransferEngine.initialize failed: {rc}")
                hostname = f"{hostname}:{self.engine.get_rpc_port()}"
                kwargs["engine"] = self.engine.get_engine()
        self.effective_local_buffer_bytes = local_buffer
        rc = self.store.setup(
            local_hostname=hostname, metadata_server=args.metadata_server,
            global_segment_size=args.segment_bytes, local_buffer_size=local_buffer,
            protocol=args.protocol, rdma_devices=args.rdma_devices,
            master_server_addr=args.master, **kwargs)
        if rc != 0:
            raise RuntimeError(f"setup failed: {rc}")

    def register_allocation(self, buffer):
        if self.path == "fabric":
            rc = 0  # matches backend: no explicit buffer registration
        elif self.path == "shared_te":
            rc = self.engine.register_memory(buffer.address, buffer.size)
        else:
            rc = self.store.register_buffer(buffer.address, buffer.size)
        if rc != 0:
            raise RuntimeError(f"buffer registration ({self.path}) failed: {rc}")
        self.live_buffers[buffer.address] = buffer

    def unregister_allocation(self, buffer):
        if self.path == "fabric":
            rc = 0
        elif self.path == "shared_te":
            rc = self.engine.unregister_memory(buffer.address)
        else:
            rc = self.store.unregister_buffer(buffer.address)
        if rc != 0:
            # Retain memory until the owning transport is closed.
            raise RuntimeError(f"buffer unregistration ({self.path}) failed: {rc}")
        self.live_buffers.pop(buffer.address, None)

    def close(self):
        self.set_device()
        rc = self.store.close()
        if rc not in (None, 0):
            raise RuntimeError(f"Store.close failed: {rc}")
        self.engine = None  # Store must stop using the external TE first.
        self.live_buffers.clear()


class Buffers:
    """One registered contiguous allocation, exposed as K x B distinct slices."""

    def __init__(self, store, args, fill):
        self.store = store
        self.size = args.keys_per_batch * args.key_bytes
        self.fill = fill
        self.torch = store.torch
        if store.torch is not None:
            self.memory = store.torch.full(
                (self.size,), fill, dtype=store.torch.uint8,
                device=f"npu:{args.device_id}")
            self.address = self.memory.data_ptr()
            store.torch.npu.synchronize()  # initialization is outside API timing
        else:
            self.memory = ctypes.create_string_buffer(self.size)
            self.address = ctypes.addressof(self.memory)
            ctypes.memset(self.address, fill, self.size)
        block = args.key_bytes // args.blocks_per_key
        self.ptrs = [
            [self.address + k * args.key_bytes + b * block
             for b in range(args.blocks_per_key)]
            for k in range(args.keys_per_batch)
        ]
        self.sizes = [[block] * args.blocks_per_key
                      for _ in range(args.keys_per_batch)]
        store.register_allocation(self)

    def has_fill(self, value):
        if self.store.torch is not None:
            self.store.torch.npu.synchronize()
            return bool((self.memory.cpu() == value).all().item())
        return self.memory.raw == bytes([value]) * self.size

    def close(self):
        self.store.unregister_allocation(self)


def percentile(sorted_values, percent):
    index = (len(sorted_values) - 1) * percent / 100
    lo = int(index)
    hi = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (index - lo)


def summarize(latencies_ns, elapsed, args):
    values = sorted(x / 1e6 for x in latencies_ns)
    batches = len(values)
    size = batches * args.keys_per_batch * args.key_bytes
    return {
        "batches": batches, "keys": batches * args.keys_per_batch,
        "keys_per_batch": args.keys_per_batch,
        "payload_bytes": size, "wall_seconds": elapsed,
        "batch_per_second": batches / elapsed,
        "keys_per_second": batches * args.keys_per_batch / elapsed,
        "GB_per_second": size / elapsed / 1e9,
        "GiB_per_second": size / elapsed / 1024**3,
        "latency_ms": {
            "avg": statistics.mean(values),
            **{f"p{p}": percentile(values, p) for p in (50, 90, 99)},
            "max": max(values),
        },
        "latencies_ns": latencies_ns,
    }


def phase(store, args, config, buffers, keysets, operation, owned=None):
    """Closed-loop concurrency; one synchronous API call in flight per worker."""
    calls = [{name: validation.ApiCalls() for name in ("measured", "verification_readback")}
             for _ in buffers]
    started = []
    gate = threading.Barrier(
        len(buffers) + 1, action=lambda: started.append(time.perf_counter()))

    def worker(index):
        try:
            store.set_device()  # ACL context is thread-local.
        except BaseException:
            gate.abort()
            raise
        source, target = buffers[index]
        buffer = source if operation == "put" else target
        samples = []
        try:
            gate.wait()
        except threading.BrokenBarrierError:
            return [], time.perf_counter()
        progress = validation.Progress(operation, index, len(keysets[index])) if args.verify_data else None
        for batch_index, keys in enumerate(keysets[index], 1):
            if args.verify_data:
                progress.update(batch_index, "prepare")
                validation.fill(buffer, keys, args, poison=operation == "get")
                progress.update(batch_index, operation)
            def measured_call():
                if operation == "put":
                    return store.batch_put_from_multi_buffers(keys, buffer.ptrs, buffer.sizes, config)
                return store.batch_get_into_multi_buffers(keys, buffer.ptrs, buffer.sizes, False)
            def validate_measured(result):
                if owned is not None and operation == "put":
                    owned.extend(key for key, rc in zip(keys, result) if rc == 0)
                check_results(operation, result, len(keys), 0 if operation == "put" else args.key_bytes)
            _, elapsed = calls[index]["measured"].invoke(
                keys, len(keys) * args.key_bytes, measured_call, validate_measured)
            samples.append(elapsed)
            if args.verify_data:
                if operation == "put":
                    progress.update(batch_index, "readback")
                    validation.fill(buffer, keys, args, poison=True)
                    calls[index]["verification_readback"].invoke(
                        keys, len(keys) * args.key_bytes,
                        lambda: store.batch_get_into_multi_buffers(keys, buffer.ptrs, buffer.sizes, False),
                        lambda result: check_results("verification read", result, len(keys), args.key_bytes))
                progress.update(batch_index, "compare")
                validation.verify(buffer, keys, args)
                progress.update(batch_index, "done")
        return samples, time.perf_counter()

    with ThreadPoolExecutor(max_workers=len(buffers)) as pool:
        futures = [pool.submit(worker, i) for i in range(len(buffers))]
        try:
            gate.wait()
        except threading.BrokenBarrierError:
            # Workers that bound a device successfully exit the broken barrier;
            # result() surfaces the original device initialization error.
            for future in futures:
                future.result()
            raise
        results = [f.result() for f in futures]
    elapsed = max(end for _, end in results) - started[0]
    workers = [{name: metric.data for name, metric in worker.items()} for worker in calls]
    phase_info = dict(workers=workers)
    validation.finalize_api_calls(dict(phases=[phase_info]))
    stats = validation.annotate(summarize([x for samples, _ in results for x in samples], elapsed, args), args)
    stats["api_calls"] = workers
    stats["api_metrics"] = phase_info["summary"]
    stats["verification"]["size_validation"] = (
        "api_returned_key_bytes" if operation == "get" or args.verify_data else "not_checked")
    return stats


def inspect_placement(store, keysets, local_endpoint, local_only):
    counts = Counter()
    for worker_keys in keysets:
        for keys in worker_keys:
            descs = store.batch_get_replica_desc(keys)
            for key in keys:
                replicas = descs.get(key, [])
                if len(replicas) != 1 or not replicas[0].is_memory_replica():
                    raise RuntimeError(f"Expected one memory replica for {key}")
                endpoint = replicas[0].get_memory_descriptor().buffer_descriptor.transport_endpoint
                counts[endpoint] += 1
                if local_only and endpoint != local_endpoint:
                    raise RuntimeError(f"Local placement violated: {key} -> {endpoint}, "
                                       f"expected {local_endpoint}")
    return dict(counts)


def prefix_keysets(args, concurrency, shuffle=False, prefix=None, count=None):
    keys = [f"{prefix or args.key_prefix}_{i}"
            for i in range(1, (count or args.num_keys) + 1)]
    if shuffle:
        random.Random(args.seed).shuffle(keys)
    batches = [keys[i:i + args.keys_per_batch]
               for i in range(0, len(keys), args.keys_per_batch)]
    return [batches[w::concurrency] for w in range(concurrency)]


def require_absent(store, keysets):
    # Check the entire range before writing anything. A prefix must have only
    # one writer: this preflight is not a distributed lock.
    for worker_keys in keysets:
        for keys in worker_keys:
            results = store.batch_is_exist(keys)
            check_results("prefix preflight (keys must not exist)",
                          results, len(keys), 0)


def remove_owned(store, args, keys):
    errors = []
    for i in range(0, len(keys), args.keys_per_batch):
        batch = keys[i:i + args.keys_per_batch]
        try:
            check_results("cleanup", store.batch_remove(batch, True), len(batch), 0)
        except Exception as exc:
            errors.append(repr(exc))
    if errors:
        raise RuntimeError(f"Owned key cleanup failed: {errors}")


def run_put_get(store, config_type, args, report):
    """One Store instance; each phase uses its own batch layout and buffers."""
    concurrency = args.concurrency[0]
    endpoint = store.get_hostname()
    config = config_type()
    config.replica_num = 1
    config.with_hard_pin = args.hard_pin
    config.prefer_alloc_in_same_node = args.prefer_alloc_in_same_node
    if args.scenarios[0] == "local":
        config.preferred_segment = endpoint
    owned = []
    try:
        write_keys = prefix_keysets(args, concurrency)
        require_absent(store, write_keys)
        with ExitStack() as stack:
            buffers = []
            for _ in range(concurrency):
                buffer = Buffers(store, args, 1)
                stack.callback(buffer.close)
                buffers.append((buffer, buffer))
            warm_keys = prefix_keysets(
                args, concurrency, prefix="mcbench-warm-" + uuid.uuid4().hex,
                count=concurrency * args.warmup * args.put_keys_per_batch)
            warm_owned = []
            try:
                require_absent(store, warm_keys)
                phase(store, args, config, buffers, warm_keys, "put", warm_owned)
            finally:
                primary_error = sys.exc_info()[0] is not None
                try:
                    remove_owned(store, args, warm_owned)
                except Exception as exc:
                    if primary_error:
                        print(f"Warmup cleanup failed: {exc!r}", file=sys.stderr)
                    else:
                        raise
            written = phase(store, args, config, buffers, write_keys, "put", owned)
            placement = inspect_placement(
                store, write_keys, endpoint, args.scenarios[0] == "local")
        # Release source buffers before allocating potentially much larger read buffers.
        row = dict(operation="put", scenario=args.scenarios[0], concurrency=concurrency,
                   placement=placement, **written)
        report["results"].append(row)
        print_rows([row])
        read_args = copy.copy(args)
        read_args.keys_per_batch = args.get_keys_per_batch
        read_keys = prefix_keysets(read_args, concurrency, True)
        placement = inspect_placement(
            store, read_keys, endpoint, args.scenarios[0] == "local")
        with ExitStack() as stack:
            buffers = []
            for _ in range(concurrency):
                buffer = Buffers(store, read_args, 0)
                stack.callback(buffer.close)
                buffers.append((buffer, buffer))
            warm_keys = [[batches[i % len(batches)] for i in range(args.warmup)]
                         for batches in read_keys]
            phase(store, read_args, None, buffers, warm_keys, "get")
            read = phase(store, read_args, None, buffers, read_keys, "get")
            if not args.verify_data and any(not target.has_fill(1) for _, target in buffers):
                raise RuntimeError("Last read batch content mismatch")
        local_keys = placement.get(endpoint, 0)
        row = dict(operation="get", scenario=args.scenarios[0], concurrency=concurrency,
                   placement=placement, local_keys=local_keys,
                   remote_keys=args.num_keys - local_keys, **read)
        report["results"].append(row)
        print_rows([row])
    finally:
        primary_error = sys.exc_info()[0] is not None
        try:
            remove_owned(store, args, owned)
            report["cleanup_status"] = "succeeded"
        except Exception as exc:
            report["cleanup_status"] = "failed"
            report["cleanup_error"] = repr(exc)
            if primary_error:
                print(f"Cleanup failed: {exc!r}", file=sys.stderr)
            else:
                raise


def print_rows(rows):
    for row in rows:
        lat = row["latency_ms"]
        values = " | ".join(f"{lat[k]:.3f}" for k in
                            ("avg", "p50", "p90", "p99", "max"))
        print(f"| {row['scenario']} | {row['concurrency']} | {row['operation']} | "
              f"{values} | {row['batch_per_second']:.2f} | "
              f"{row['GB_per_second']:.3f} |", flush=True)


def wait_for_reader(ready_message="writes and placement checks complete"):
    """SIGINT/SIGTERM stop normally after READY; no inter-node coordination."""
    stopped = threading.Event()
    previous = {}
    try:
        for sig in (signal.SIGINT, signal.SIGTERM):
            previous[sig] = signal.signal(sig, lambda *_: stopped.set())
        print(f"READY: {ready_message}. Run reader now. "
              "Keep this process alive; Ctrl+C or SIGTERM cleans up and exits.",
              flush=True)
        stopped.wait()
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def detect_mooncake_version():
    """Importable source/custom builds need not have wheel metadata."""
    distribution = "mooncake-transfer-engine"
    try:
        return importlib.metadata.version(distribution), distribution
    except importlib.metadata.PackageNotFoundError:
        # Use an installed package's declared mapping, never guess wheel names.
        candidates = importlib.metadata.packages_distributions().get("mooncake", [])
        if len(candidates) == 1:
            distribution = candidates[0]
            try:
                return importlib.metadata.version(distribution), distribution
            except importlib.metadata.PackageNotFoundError:
                pass
    return None, None


def validate_store_api(store_type):
    required = (
        "setup", "close", "get_hostname", "batch_is_exist", "batch_remove",
        "batch_get_replica_desc", "batch_put_from_multi_buffers",
        "batch_get_into_multi_buffers",
    )
    missing = [name for name in required if not callable(getattr(store_type, name, None))]
    if missing:
        raise RuntimeError("Installed Mooncake Store lacks required APIs: " + ", ".join(missing))


def main(argv=None):
    args = parse_args(argv)
    if args.verify_data:
        print("VERIFY_DATA: API latency excludes validation, wall throughput includes it", flush=True)
    import mooncake.store as store_module
    from mooncake.store import MooncakeDistributedStore, ReplicateConfig

    validate_store_api(MooncakeDistributedStore)
    version, distribution = detect_mooncake_version()
    if version is not None and version != "0.3.11.post1":
        raise RuntimeError(f"Expected Mooncake 0.3.11.post1, got {version} ({distribution})")
    if version is None:
        print(
            "WARNING: Mooncake is importable but its version metadata is unavailable. "
            "Continuing with required API checks only; version 0.3.11.post1 is NOT verified. "
            f"Loaded module: {getattr(store_module, '__file__', None)}",
            file=sys.stderr,
        )
    report = {
        "version": version, "version_verified": version == "0.3.11.post1",
        "version_distribution": distribution,
        "module_path": getattr(store_module, "__file__", None),
        "args": vars(args), "platform": platform.platform(),
        "memory": "npu" if args.protocol == "ascend" else "host",
        "run_id": "mcbench-" + uuid.uuid4().hex,
        "status": "running", "results": [],
    }
    store = StoreRuntime(MooncakeDistributedStore(), args)
    try:
        store.setup()
        report["setup_path"] = store.path
        report["effective_local_buffer_bytes"] = store.effective_local_buffer_bytes
        report["local_endpoint"] = store.get_hostname()
        print("| scenario | concurrency | API | avg ms | p50 | p90 | p99 | max | batch/s | GB/s |")
        print("|---|---:|---|---:|---:|---:|---:|---:|---:|---:|", flush=True)
        if args.operation == "put-get":
            run_put_get(store, ReplicateConfig, args, report)
        elif args.operation == "get":
            from mooncake_bench_http import serve

            serve(store, args, report)
        else:
            from mooncake_bench_http import coordinate

            coordinate(store, ReplicateConfig, args, report)
        report["status"] = "ok"
    except BaseException as exc:
        report["status"] = "failed"
        report["error"] = repr(exc)
        raise
    finally:
        primary_error = sys.exc_info()[0] is not None
        try:
            store.close()
        except Exception as exc:
            report["status"] = "failed"
            report["close_error"] = repr(exc)
            if primary_error:
                print(f"Store cleanup failed: {exc!r}", file=sys.stderr)
            else:
                raise
        finally:
            Path(args.output).write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
