#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Ascend NPU D2H/H2D benchmark using the vLLM-Ascend Yuanrong backend APIs."""

import argparse
import importlib.metadata
import json
import os
import platform
import sys
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import buffer_validation as validation

# Shared workload/statistics only: importing this module does not load Mooncake.
from mooncake_multi_buffer_bench import (
    check_results, nonnegative, positive, prefix_keysets, print_rows,
    summarize,
)


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--config", default=os.getenv("YR_CONFIG_PATH"),
                   help="same JSON as YuanrongBackend; defaults to YR_CONFIG_PATH")
    p.add_argument("--local-ip", help="reachable SDK IP for HIXL; overrides DS_HIXL_D2RH_LOCAL_IP")
    p.add_argument("--worker-addr", help="override config worker_addr, host:port")
    p.add_argument("--operation", required=True, choices=["put", "get"])
    p.add_argument("--key-prefix", required=True)
    p.add_argument("--num-keys", type=positive, default=3200)
    p.add_argument("--keys-per-batch", type=positive, default=8)
    p.add_argument("--key-bytes", type=positive, default=7096320)
    p.add_argument("--blocks-per-key", type=positive, default=128)
    p.add_argument("--concurrency", nargs="+", type=positive)
    p.add_argument("--warmup", type=nonnegative, default=5)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--device-id", type=nonnegative, default=0)
    p.add_argument("--verify-data", action="store_true",
                   help="verify every batch with key/offset patterns; wall throughput includes verification")
    p.add_argument("--data-placement-policy", choices=["PREFERRED_SAME_NODE", "REQUIRED_SAME_NODE",
                                                        "PREFERRED_META_OWNER"])
    p.add_argument("--output", default="yuanrong_multi_buffer_results.json")
    for name in ("enable-remote-h2d", "enable-fabric-mem", "enable-dev-mem-pregister"):
        p.add_argument("--" + name, action=argparse.BooleanOptionalAction, default=None)
    p.add_argument("--remote-h2d-transport-backend")
    p.add_argument("--connect-timeout-ms", type=positive)
    p.add_argument("--request-timeout-ms", type=nonnegative)
    p.add_argument("--get-sub-timeout-ms", type=nonnegative)
    args = p.parse_args(argv)
    args.concurrency = args.concurrency or ([1] if args.operation == "put" else [1, 4])
    if not args.key_prefix.strip():
        p.error("--key-prefix must not be blank")
    if args.key_bytes % args.blocks_per_key:
        p.error("--key-bytes must be divisible by --blocks-per-key")
    if args.num_keys % args.keys_per_batch:
        p.error("--num-keys must be divisible by --keys-per-batch")
    if args.num_keys // args.keys_per_batch < max(args.concurrency):
        p.error("dataset must contain at least one full batch per worker")
    if args.keys_per_batch > 30000:
        p.error("Yuanrong multi-buffer API accepts at most 30000 keys per call")
    if args.operation == "put" and len(args.concurrency) != 1:
        p.error("put accepts one concurrency; use a fresh prefix for each write test")
    return args


def load_config(args):
    config = dict(
        worker_addr="", local_ip="", enable_remote_h2d=False,
        remote_h2d_transport_backend="HIXL", enable_fabric_mem=False,
        connect_timeout_ms=9000, request_timeout_ms=0, get_sub_timeout_ms=0,
        enable_dev_mem_pregister=False, data_placement_policy="PREFERRED_META_OWNER",
    )
    if args.config:
        supplied = json.loads(Path(args.config).read_text())
        if not isinstance(supplied, dict):
            raise ValueError("Yuanrong config must be a JSON object")
        for key in config:
            if key in supplied:
                config[key] = supplied[key]
    for key in config:
        if getattr(args, key, None) is not None:
            config[key] = getattr(args, key)
    for key in ("enable_remote_h2d", "enable_fabric_mem", "enable_dev_mem_pregister"):
        if not isinstance(config[key], bool):
            raise ValueError(f"{key} must be a JSON boolean")
    for key in ("connect_timeout_ms", "request_timeout_ms", "get_sub_timeout_ms"):
        value = config[key]
        if type(value) is not int or value < (1 if key == "connect_timeout_ms" else 0):
            raise ValueError(f"Invalid {key}: {value!r}")
    if not isinstance(config["remote_h2d_transport_backend"], str):
        raise ValueError("remote_h2d_transport_backend must be a string")
    if config["data_placement_policy"] not in (
            "PREFERRED_SAME_NODE", "REQUIRED_SAME_NODE", "PREFERRED_META_OWNER"):
        raise ValueError("Invalid data_placement_policy")
    if not isinstance(config["local_ip"], str):
        raise ValueError("local_ip must be a string")
    if args.operation == "put" and (
            config["enable_fabric_mem"] or config["remote_h2d_transport_backend"] != "HIXL"):
        raise ValueError("D2RH put requires HIXL with enable_fabric_mem=false")
    split_address(config["worker_addr"])
    return config


def configure_hixl(config):
    if config["remote_h2d_transport_backend"] == "HIXL" and not config["enable_fabric_mem"]:
        os.environ["DS_RH2D_LINK_TYPE"] = "HCCS"
        os.environ.setdefault("DS_RH2D_HCCS_BUFFER_POOL", "4:8")
        os.environ.setdefault("DS_HIXL_CS_ENABLE", "false")
    if config["local_ip"]:
        os.environ["DS_HIXL_D2RH_LOCAL_IP"] = config["local_ip"]


def register_buffers(store, args, config, buffers):
    if (args.operation == "put" or (config["enable_remote_h2d"] and
            config["remote_h2d_transport_backend"] == "HIXL" and not config["enable_fabric_mem"])):
        store.pre_register_device_memory(
            [buffer.address for buffer in buffers], [buffer.registration_size for buffer in buffers])


def split_address(address):
    if not isinstance(address, str):
        raise ValueError("worker_addr must be host:port or [IPv6]:port")
    host, separator, port = address.rpartition(":")
    if host.startswith("[") and host.endswith("]"):
        host = host[1:-1]
    elif ":" in host:
        raise ValueError("IPv6 worker addresses must use [IPv6]:port")
    if not separator or not host or not port.isdigit() or not 0 < int(port) < 65536:
        raise ValueError("worker_addr must be host:port or [IPv6]:port")
    return host, int(port)


class DeviceBuffer:
    def __init__(self, torch, args):
        self.size = args.keys_per_batch * args.key_bytes
        # Keep the tensor alive until after HeteroClient destruction, including
        # when pre_register_device_memory caches a registration.
        self.torch = torch
        alignment = 2 * 1024 * 1024
        self.registration_size = (self.size + alignment - 1) // alignment * alignment
        self.allocation = torch.empty(self.registration_size + alignment - 1,
                                      dtype=torch.uint8, device=f"npu:{args.device_id}")
        offset = (-self.allocation.data_ptr()) % alignment
        self.tensor = self.allocation[offset:offset + self.size]
        self.memory = self.tensor
        self.tensor.fill_(1 if args.operation == "put" else 0)
        self.address = self.tensor.data_ptr()
        block = args.key_bytes // args.blocks_per_key
        self.ptrs = [
            [self.address + k * args.key_bytes + b * block
             for b in range(args.blocks_per_key)]
            for k in range(args.keys_per_batch)
        ]
        self.sizes = [[block] * args.blocks_per_key for _ in range(args.keys_per_batch)]


def require_existence(store, keysets, expected):
    for batches in keysets:
        for keys in batches:
            check_results("key existence", store.batch_is_exist(keys), len(keys), expected)


def cleanup(store, keys):
    errors = []
    # HeteroClient.delete has a 10,000-key limit, independent of batch size.
    for start in range(0, len(keys), 10000):
        try:
            failed = store.delete(keys[start:start + 10000])
            if failed:
                errors.append(f"failed keys: {failed}")
        except Exception as exc:
            errors.append(repr(exc))
    if errors:
        raise RuntimeError(f"Yuanrong cleanup failed: {errors}")


def phase(store, torch, args, config, param, buffers, keysets, attempted):
    started = []
    gate = threading.Barrier(
        len(keysets) + 1, action=lambda: started.append(time.perf_counter()))

    def worker(index):
        try:
            # ACL context is thread-local; setting only the main thread is not enough.
            torch.npu.set_device(args.device_id)
            buffer = buffers[index]
            samples = []
            gate.wait()
            progress = validation.Progress(args.operation, index, len(keysets[index])) if args.verify_data else None
            for batch_index, keys in enumerate(keysets[index], 1):
                if args.operation == "put":
                    # A raised mset can already have published some keys. The
                    # exclusive, preflight-checked range is ours to clean.
                    attempted.extend(keys)
                if args.verify_data:
                    progress.update(batch_index, "prepare")
                    validation.fill(buffer, keys, args, poison=args.operation == "get")
                    progress.update(batch_index, args.operation)
                start = time.perf_counter_ns()
                if args.operation == "put":
                    store.mset_d2h_from_multi_buffers(keys, buffer.ptrs, buffer.sizes, param)
                    failed = []
                else:
                    failed = store.mget_h2d_from_multi_buffers(
                        keys, buffer.ptrs, buffer.sizes, config["get_sub_timeout_ms"])
                end = time.perf_counter_ns()
                if failed:
                    raise RuntimeError(f"mget_h2d_from_multi_buffers failed keys: {failed}")
                samples.append(end - start)
                if args.verify_data:
                    if args.operation == "put":
                        progress.update(batch_index, "readback")
                        validation.fill(buffer, keys, args, poison=True)
                        failed = store.mget_h2d_from_multi_buffers(
                            keys, buffer.ptrs, buffer.sizes, config["get_sub_timeout_ms"])
                        if failed:
                            raise RuntimeError(f"Verification read failed keys: {failed}")
                    progress.update(batch_index, "compare")
                    validation.verify(buffer, keys, args)
                    progress.update(batch_index, "done")
            return samples, time.perf_counter()
        except BaseException:
            gate.abort()  # also unblock startup if set_device fails in one worker
            raise

    with ThreadPoolExecutor(max_workers=len(keysets)) as pool:
        futures = [pool.submit(worker, i) for i in range(len(keysets))]
        try:
            gate.wait()
        except threading.BrokenBarrierError:
            # Surface the device/API exception rather than only "broken barrier".
            for future in futures:
                if future.done() and future.exception() is not None:
                    raise future.exception()
            raise
        results = [future.result() for future in futures]
    elapsed = max(end for _, end in results) - started[0]
    return validation.annotate(summarize([x for samples, _ in results for x in samples], elapsed, args), args)


def run(store, torch, args, config, param, buffers, report):
    attempted = []
    try:
        for concurrency in args.concurrency:
            keysets = prefix_keysets(args, concurrency, args.operation == "get")
            require_existence(store, keysets, 0 if args.operation == "put" else 1)
            if args.warmup:
                if args.operation == "put":
                    warm_keys = prefix_keysets(
                        args, concurrency, prefix="yrbench-warm-" + uuid.uuid4().hex,
                        count=concurrency * args.warmup * args.keys_per_batch)
                    require_existence(store, warm_keys, 0)
                    warm_attempted = []
                    try:
                        phase(store, torch, args, config, param, buffers,
                              warm_keys, warm_attempted)
                    finally:
                        primary_error = sys.exc_info()[0] is not None
                        try:
                            cleanup(store, warm_attempted)
                        except Exception as exc:
                            if primary_error:
                                print(str(exc), file=sys.stderr)
                            else:
                                raise
                else:
                    warm_keys = [[batches[i % len(batches)] for i in range(args.warmup)]
                                 for batches in keysets]
                    phase(store, torch, args, config, param, buffers, warm_keys, [])
            stats = phase(store, torch, args, config, param, buffers, keysets, attempted)
            # Sync/check only outside measured calls; the SDK APIs are synchronous.
            torch.npu.synchronize()
            if args.operation == "get" and not args.verify_data:
                for buffer in buffers[:concurrency]:
                    if not bool((buffer.tensor.cpu() == 1).all().item()):
                        raise RuntimeError("Last read batch content mismatch")
            row = dict(scenario="worker_managed", operation=args.operation,
                       concurrency=concurrency, **stats)
            report["results"].append(row)
            print_rows([row])
        if args.operation == "put":
            # Host objects belong to the worker, not this client's NPU buffers.
            # Retain the committed dataset for a separately launched get process.
            report["put_data_retained"] = True
            print("PUT_COMPLETE: data retained in worker; exiting. "
                  "Run get with the same prefix and dataset layout.", flush=True)
    except BaseException:
        try:
            cleanup(store, attempted)
        except Exception as exc:
            print(str(exc), file=sys.stderr)
        raise


_FAILED_RUNTIMES = []


def main(argv=None):
    args = parse_args(argv)
    config = load_config(args)
    if args.verify_data:
        print("VERIFY_DATA: every batch; API latency excludes validation, wall throughput includes it", flush=True)
    configure_hixl(config)
    report = dict(backend="yuanrong", args=vars(args), config=config,
                  platform=platform.platform(), memory="npu",
                  status="running", results=[])
    store = None
    buffers = []
    try:
        import torch
        import torch_npu  # noqa: F401 - registers the NPU device backend
        from yr.datasystem.hetero_client import HeteroClient
        from yr.datasystem.kv_client import DataPlacementPolicy, ExistenceOpt, SetParam
        from yr.datasystem.object_client import WriteMode

        report["version"] = importlib.metadata.version("openyuanrong-datasystem")
        torch.npu.set_device(args.device_id)
        host, port = split_address(config["worker_addr"])
        store = HeteroClient(
            host, port, connect_timeout_ms=config["connect_timeout_ms"],
            req_timeout_ms=config["request_timeout_ms"],
            enable_remote_h2d=config["enable_remote_h2d"],
            data_placement_policy=DataPlacementPolicy[config["data_placement_policy"]])
        store.init()
        for method in ("pre_register_device_memory", "mset_d2h_from_multi_buffers", "mget_h2d_from_multi_buffers",
                       "batch_is_exist", "delete"):
            if not callable(getattr(store, method, None)):
                raise RuntimeError(f"Installed HeteroClient lacks {method}; upgrade the SDK")
        param = SetParam()
        param.write_mode = WriteMode.NONE_L2_CACHE_EVICT
        param.existence = ExistenceOpt.NX
        buffers = [DeviceBuffer(torch, args) for _ in range(max(args.concurrency))]
        torch.npu.synchronize()
        register_buffers(store, args, config, buffers)
        print("| scenario | concurrency | API | avg ms | p50 | p90 | p99 | max | batch/s | GB/s |")
        print("|---|---:|---|---:|---:|---:|---:|---:|---:|---:|", flush=True)
        run(store, torch, args, config, param, buffers, report)
        report["status"] = "ok"
    except BaseException as exc:
        report["status"] = "failed"
        report["error"] = repr(exc)
        # Failed native DMA has no proven fence; keep client and arenas until process exit.
        _FAILED_RUNTIMES.append((store, buffers))
        raise
    finally:
        # HeteroClient exposes no public close/unregister API in the reference
        # SDK. Keep buffers alive through client destruction and process exit.
        store = None
        Path(args.output).write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
