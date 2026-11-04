# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Randomized release-wheel E2E with access-log proof of HiXL WRITE/READ coverage."""

import argparse
from collections import Counter, defaultdict, deque
import ctypes
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import time
import traceback
import uuid


ACCESS_LOG_WAIT_SECONDS = 60
WRITE_API = "DS_HETERO_CLIENT_MSETD2H"
READ_API = "DS_HETERO_CLIENT_MGETH2D"
WRITE_INTERFACES = ("mset_d2h_from_multi_buffers", "mset_d2h")
ACCESS_RECORD = re.compile(
    r"(?:^|\|)\s*(-?\d+)\s*\|\s*(DS_HETERO_CLIENT_MSETD2H|DS_HETERO_CLIENT_MGETH2D)"
    r"\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\{[^}]*\})")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def acl_ok(code, operation):
    require(code == 0, "{} failed: ACL {}".format(operation, code))


def check_bound_worker_locality(log_dir, worker_host, worker_port):
    snapshots = []
    for path in log_dir.iterdir():
        if path.is_file():
            with path.open(encoding="utf-8", errors="replace") as stream:
                snapshots.extend(line.rstrip() for line in stream if "[Routing] worker snapshot hostIdMap" in line)
    require(snapshots, "No SDK routing snapshot found; keep SDK INFO logging enabled for the dual-mode preflight")
    snapshot = max(snapshots)
    endpoint = "{}:{}".format(worker_host, worker_port)
    sdk_host = re.search(r"\bsdkHostId=([^,]*)", snapshot)
    bound_host = re.search(r"\{worker=" + re.escape(endpoint) + r", hostId=([^,}]+), status=([A-Z_]+)", snapshot)
    require(sdk_host and sdk_host.group(1) and bound_host and bound_host.group(1) == sdk_host.group(1)
            and bound_host.group(2) == "ACTIVE",
            "Bound Worker {} is not confirmed same-host. Start Workers with --host_id_env_name DS_TEST_HOST_ID "
            "and distinct per-host DS_TEST_HOST_ID values; ensure the SDK can access Worker A's UDS/SHM. "
            "Otherwise local keys may enter HIXL and fail with duplicate device IP on the same NPU. "
            "Snapshot: {}".format(endpoint, snapshot))
    print("ROUTING OK: bound Worker {} is same-host, hostId={}".format(endpoint, sdk_host.group(1)), flush=True)


def parse_access_record(line):
    match = ACCESS_RECORD.search(line)
    if match is None:
        return None
    code, operation, elapsed, size, fields = match.groups()
    key = re.search(r"\bObject_key:([^,}]+)", fields)
    if key is None:
        return None
    transport = re.search(r"\btransportType:([^,}]+)", fields)
    prefix = line[:match.start()].split("|")
    return {
        "key": key.group(1), "operation": operation, "code": int(code),
        "elapsed_us": int(elapsed), "bytes": int(size),
        "transport": transport.group(1) if transport else None,
        "trace_id": prefix[5].strip() if len(prefix) > 5 else "",
    }


class AccessEvidence:
    def __init__(self, log_path):
        self.log_path = log_path
        self.cursor = 0
        self.requests = []
        self.pending = defaultdict(deque)

    def expect(self, key, operation, stage, elapsed_ms):
        entry = {"key": key, "operation": operation, "stage": stage, "sdk_elapsed_ms": elapsed_ms}
        self.requests.append(entry)
        self.pending[(key, operation)].append(entry)

    def drain(self):
        if not self.log_path.exists():
            return
        with self.log_path.open("rb") as stream:
            stream.seek(self.cursor)
            while True:
                raw = stream.readline()
                if not raw or not raw.endswith(b"\n"):
                    break
                self.cursor = stream.tell()
                record = parse_access_record(raw.decode("utf-8", errors="replace"))
                if record is None:
                    continue
                identity = (record["key"], record["operation"])
                pending = self.pending.get(identity)
                if pending:
                    pending.popleft().update(record)
                    if not pending:
                        del self.pending[identity]

    def collect(self):
        print("Collecting access evidence once after sampling (up to {}s): {}".format(
            ACCESS_LOG_WAIT_SECONDS, self.log_path), flush=True)
        deadline = time.monotonic() + ACCESS_LOG_WAIT_SECONDS
        previous_missing = None
        while True:
            self.drain()
            missing = sum(len(items) for items in self.pending.values())
            if missing != previous_missing:
                print("Access evidence: {}/{} records".format(len(self.requests) - missing, len(self.requests)),
                      flush=True)
                previous_missing = missing
            if missing == 0 or time.monotonic() >= deadline:
                return
            time.sleep(0.1)


def report_coverage(evidence, samples, args, arena_size, report_path):
    issues = []
    for entry in evidence.requests:
        if "code" not in entry:
            issues.append("Missing {} {} record for {}".format(entry["stage"], entry["operation"], entry["key"]))
        elif (entry["code"] != 0 or entry["bytes"] != arena_size
              or entry["transport"] not in ("HIXL", "SHM", "TCP", "UB")):
            issues.append("Invalid access evidence: {}".format(entry))
    fresh_writes = {item["key"]: item for item in evidence.requests if item["stage"] == "fresh_write"}
    first_reads = {item["key"]: item for item in evidence.requests if item["stage"] == "first_read"}
    d2rh = set()
    rh2d = set()
    for sample in samples:
        write = fresh_writes[sample["key"]]
        read = first_reads[sample["key"]]
        sample["write_transport"] = write.get("transport", "MISSING")
        sample["read_transport"] = read.get("transport", "MISSING")
        if write.get("code") == 0 and write.get("transport") == "HIXL" and write.get("bytes") == arena_size:
            d2rh.add(sample["key"])
        if read.get("code") == 0 and read.get("transport") == "HIXL" and read.get("bytes") == arena_size:
            rh2d.add(sample["key"])
        print("Sample {}: WRITE={} READ={} data=verified interface={} key={}".format(
            sample["index"], sample["write_transport"], sample["read_transport"],
            sample["write_interface"], sample["key"]), flush=True)
    joint = d2rh & rh2d
    covered = joint if args.enable_remote_h2d else d2rh
    if len(covered) < args.min_hixl_samples:
        issues.append("Need {} distinct {} samples; observed {}. Check Worker topology/configuration or "
                      "increase --rounds to sample more placements.".format(
                          args.min_hixl_samples, "HIXL WRITE+READ on the same key" if args.enable_remote_h2d
                          else "HIXL WRITE", len(covered)))
    interfaces = {name: sum(sample["write_interface"] == name and sample["key"] in covered for sample in samples)
                  for name in WRITE_INTERFACES}
    for name, count in interfaces.items():
        if count == 0:
            issues.append("No qualifying HIXL sample for write interface {}".format(name))
    summary = {
        "passed": not issues, "mode": "D2RH+RH2D" if args.enable_remote_h2d else "D2RH",
        "samples": len(samples), "min_hixl_samples": args.min_hixl_samples,
        "d2rh_samples": len(d2rh), "rh2d_samples": len(rh2d), "same_key_bidirectional_samples": len(joint),
        "qualifying_samples_by_interface": interfaces,
        "fresh_write_transports": dict(Counter(item["write_transport"] for item in samples)),
        "first_read_transports": dict(Counter(item["read_transport"] for item in samples)),
        "issues": issues, "sample_results": samples, "access_records": evidence.requests,
    }
    report_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print("Coverage: D2RH={} RH2D={} same-key-bidirectional={} / {} samples; report={}".format(
        len(d2rh), len(rh2d), len(joint), len(samples), report_path), flush=True)
    require(not issues, "Coverage failed: {}. See {}".format("; ".join(issues[:3]), report_path))


def run(args):
    # Configure logging before the installed wheel initializes its native library.
    log_dir = Path(tempfile.mkdtemp(prefix="ds_d2rh_", dir=args.output_dir)).resolve()
    os.environ.update({
        "DATASYSTEM_CLIENT_LOG_DIR": str(log_dir),
        "DATASYSTEM_CLIENT_ACCESS_LOG_NAME": "d2rh_e2e_access",
        "DATASYSTEM_LOG_MONITOR_ENABLE": "true",
        "DATASYSTEM_LOG_ASYNC_ENABLE": "false",
        "DATASYSTEM_LOG_COMPRESS": "false",
        "DS_HIXL_D2RH_LOCAL_IP": args.local_ip,
    })
    if args.enable_remote_h2d:
        os.environ["DS_RH2D_LINK_TYPE"] = "HCCS"
    import acl
    import yr.datasystem.hetero_client as sdk
    from yr.datasystem.kv_client import DataPlacementPolicy, ExistenceOpt, SetParam

    print("SDK module: {}\nNative module: {}\nArtifacts: {}".format(
        sdk.__file__, sdk.ds.__file__, log_dir), flush=True)
    acl_ok(acl.init(), "init")
    acl_ok(acl.rt.set_device(args.device_id), "set_device")
    alignment = 2 * 1024 * 1024
    # Exceed QueryMeta's 512 KiB inline-payload limit so remote reads can exercise RH2D.
    blob_sizes = [alignment, 2 * alignment]
    arena_size = sum(blob_sizes)
    source_allocation, code = acl.rt.malloc(arena_size + alignment - 1, 0)
    acl_ok(code, "malloc source")
    destination_allocation, code = acl.rt.malloc(arena_size + alignment - 1, 0)
    acl_ok(code, "malloc destination")
    source = (source_allocation + alignment - 1) // alignment * alignment
    destination = (destination_allocation + alignment - 1) // alignment * alignment
    print("Arena: bytes={} blobs={} alignment={}".format(arena_size, blob_sizes, alignment), flush=True)
    client = sdk.HeteroClient(
        args.worker_host, args.worker_port, connect_timeout_ms=30000, req_timeout_ms=30000,
        access_key=os.getenv("DS_D2RH_TEST_ACCESS_KEY", ""),
        secret_key=os.getenv("DS_D2RH_TEST_SECRET_KEY", ""),
        tenant_id=os.getenv("DS_D2RH_TEST_TENANT_ID", ""),
        enable_remote_h2d=args.enable_remote_h2d,
        data_placement_policy=DataPlacementPolicy.PREFERRED_META_OWNER,
    )
    client.init()
    if args.enable_remote_h2d:
        check_bound_worker_locality(log_dir, args.worker_host, args.worker_port)
    arenas = [source, destination] if args.enable_remote_h2d else [source]
    client.pre_register_device_memory(arenas, [arena_size] * len(arenas))
    addrs = [[source, source + blob_sizes[0]]]
    sizes = [list(blob_sizes)]
    outputs = [sdk.DeviceBlobList(args.device_id, [
        sdk.Blob(destination, blob_sizes[0]), sdk.Blob(destination + blob_sizes[0], blob_sizes[1]),
    ])]
    inputs = [sdk.DeviceBlobList(args.device_id, [sdk.Blob(ptr, size)
              for ptr, size in zip(addrs[0], blob_sizes)])]
    evidence = AccessEvidence(log_dir / "d2rh_e2e_access.log")
    samples = []

    def upload(round_id):
        payload = b"".join(
            (bytes((round_id * 17 + j * 7 + k) % 256 for k in range(256)) * ((size + 255) // 256))[:size]
            for j, size in enumerate(blob_sizes))
        host = ctypes.create_string_buffer(payload, arena_size)
        acl_ok(acl.rt.memcpy(source, arena_size, ctypes.addressof(host), arena_size, 1), "H2D fill")
        return payload

    def write(keys, param, interface, stage):
        started = time.monotonic()
        if interface == WRITE_INTERFACES[0]:
            client.mset_d2h_from_multi_buffers(keys, addrs, sizes, param)
        else:
            client.mset_d2h(keys, inputs, param)
        elapsed = (time.monotonic() - started) * 1000
        evidence.expect(keys[0], WRITE_API, stage, elapsed)
        return elapsed

    def verify(keys, expected, stage):
        host = ctypes.create_string_buffer(expected.translate(bytes(range(255, -1, -1))), arena_size)
        acl_ok(acl.rt.memcpy(destination, arena_size, ctypes.addressof(host), arena_size, 1), "poison")
        started = time.monotonic()
        failed = client.mget_h2d(keys, outputs, 5000)
        require(not failed, "MGetH2D failed: {}".format(failed))
        elapsed = (time.monotonic() - started) * 1000
        evidence.expect(keys[0], READ_API, stage, elapsed)
        acl_ok(acl.rt.memcpy(ctypes.addressof(host), arena_size, destination, arena_size, 2), "D2H check")
        require(host.raw == expected, "Round-trip bytes differ for {}".format(keys[0]))
        failed, metadata = client.get_meta_info(keys, False)
        require(not failed and len(metadata) == len(keys), "Missing metadata")
        require(all(list(item.blob_size_list) == blob_sizes for item in metadata), "Blob layout differs")
        return elapsed

    for round_id in range(args.rounds):
        key = "d2rh_e2e_{}_{}".format(uuid.uuid4().hex, round_id)
        keys = [key]
        interface = WRITE_INTERFACES[round_id % len(WRITE_INTERFACES)]
        print("Sample {}/{}: {} interface={}".format(round_id + 1, args.rounds, key, interface), flush=True)
        expected = upload(round_id)
        param = SetParam()
        param.ttl_second = 300
        write_ms = write(keys, param, interface, "fresh_write")
        read_ms = verify(keys, expected, "first_read")
        require(client.exist(keys) == [True], "Published key is missing")
        upload(round_id + 1)
        param.existence = ExistenceOpt.NX
        write(keys, param, WRITE_INTERFACES[0], "nx_write")
        verify(keys, expected, "nx_read")
        require(not client.delete(keys), "Delete failed")
        require(client.exist(keys) == [False], "Deleted key remains visible")
        samples.append({"index": round_id + 1, "key": key, "write_interface": interface, "data_verified": True})
        print("DATA OK: WRITE={:.3f} ms READ={:.3f} ms; bytes/layout/NX/delete verified".format(
            write_ms, read_ms), flush=True)

    (log_dir / "verified_samples.json").write_text(json.dumps(samples, indent=2) + "\n", encoding="utf-8")
    evidence.collect()
    report_coverage(evidence, samples, args, arena_size, log_dir / "coverage.json")
    del client
    acl_ok(acl.rt.free(destination_allocation), "free destination")
    acl_ok(acl.rt.free(source_allocation), "free source")
    acl_ok(acl.rt.reset_device(args.device_id), "reset_device")
    acl_ok(acl.finalize(), "finalize")
    print("PASS randomized Python HiXL E2E; logs and coverage: {}".format(log_dir), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker-host", required=True,
                        help="Bound Worker IP; use Worker A for dual-direction coverage")
    parser.add_argument("--worker-port", type=int, required=True)
    parser.add_argument("--local-ip", required=True, help="SDK host's reachable HIXL IP")
    parser.add_argument("--device-id", type=int, default=0)
    parser.add_argument("--rounds", type=int, default=32, help="Number of independent random-key placement samples")
    parser.add_argument("--min-hixl-samples", type=int, default=2,
                        help="Minimum distinct HIXL sample keys; dual mode requires WRITE+READ on the same key")
    parser.add_argument("--enable-remote-h2d", action="store_true",
                        help="Require both D2RH WRITE and RH2D READ coverage")
    parser.add_argument("--output-dir", default=None, help="Existing parent directory for logs")
    args = parser.parse_args()
    require(args.rounds >= len(WRITE_INTERFACES), "Use at least two rounds to cover both Python write interfaces")
    require(0 < args.min_hixl_samples <= args.rounds, "min-hixl-samples must be positive and at most rounds")
    require(os.getenv("DS_RH2D_HCCS_BUFFER_POOL") not in (None, "", "0:0"), "Set DS_RH2D_HCCS_BUFFER_POOL")
    try:
        run(args)
    except BaseException:
        traceback.print_exc()
        print("FAIL: no retry or device-memory cleanup; retaining arenas until process exit. "
              "Created keys have a 300-second TTL.", file=sys.stderr, flush=True)
        sys.stdout.flush()
        # Native teardown cannot establish a DMA fence after an ambiguous transfer error.
        os._exit(1)


if __name__ == "__main__":
    main()
