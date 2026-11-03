# SPDX-License-Identifier: Apache-2.0
"""Host-only regression checks; never imports torch, ACL or either native SDK."""
import ctypes
import os
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import buffer_validation as validation
import yuanrong_multi_buffer_bench as yr
import mooncake_multi_buffer_bench as mc
import mooncake_bench_http as http


def args(operation="put"):
    return SimpleNamespace(operation=operation, keys_per_batch=2, key_bytes=64,
                           blocks_per_key=4, seed=42, verify_data=True,
                           device_id=0, num_keys=4)


def buffer():
    memory = ctypes.create_string_buffer(128)
    address = ctypes.addressof(memory)
    return SimpleNamespace(memory=memory, torch=None, address=address,
                           size=128, registration_size=128,
                           ptrs=[[address, address + 32], [address + 64, address + 96]],
                           sizes=[[32, 32], [32, 32]])


class Store:
    def __init__(self, corrupt=False):
        self.data = {}
        self.corrupt = corrupt
        self.reads = 0

    def mset_d2h_from_multi_buffers(self, keys, ptrs, sizes, param):
        for key, addresses, lengths in zip(keys, ptrs, sizes):
            self.data[key] = b"".join(ctypes.string_at(p, n) for p, n in zip(addresses, lengths))

    def mget_h2d_from_multi_buffers(self, keys, ptrs, sizes, timeout):
        self.reads += 1
        for key, addresses, lengths in zip(keys, ptrs, sizes):
            payload = self.data[key]
            if self.corrupt and self.reads == 1:
                payload = bytes([payload[0] ^ 1]) + payload[1:]
            offset = 0
            for p, n in zip(addresses, lengths):
                ctypes.memmove(p, payload[offset:offset + n], n)
                offset += n
        return []

    def set_device(self):
        pass

    def batch_put_from_multi_buffers(self, keys, ptrs, sizes, config):
        self.mset_d2h_from_multi_buffers(keys, ptrs, sizes, config)
        return [0] * len(keys)

    def batch_get_into_multi_buffers(self, keys, ptrs, sizes, flag):
        self.mget_h2d_from_multi_buffers(keys, ptrs, sizes, flag)
        return [sum(row) for row in sizes]


class ValidationTests(unittest.TestCase):
    def test_patterns_roundtrip_across_chunk_boundary(self):
        b, a = buffer(), args()
        with patch.object(validation, "CHUNK_BYTES", 17):
            validation.fill(b, ["a", "b"], a)
            validation.verify(b, ["a", "b"], a)
            with self.assertRaisesRegex(RuntimeError, "key=b"):
                validation.verify(b, ["b", "a"], a)

    def test_poison_detects_no_copy(self):
        b, a = buffer(), args()
        validation.fill(b, ["a", "b"], a, poison=True)
        with self.assertRaisesRegex(RuntimeError, "byte=0"):
            validation.verify(b, ["a", "b"], a)

    def test_late_byte_and_block_corruption(self):
        b, a = buffer(), args()
        validation.fill(b, ["a", "b"], a)
        ctypes.c_ubyte.from_address(b.address + 127).value ^= 1
        with self.assertRaisesRegex(RuntimeError, "key=b block=3 byte=63"):
            validation.verify(b, ["a", "b"], a)

    def test_pattern_independent_of_batch_slot(self):
        a = args()
        one, two = buffer(), buffer()
        validation.fill(one, ["a", "b"], a)
        validation.fill(two, ["b", "a"], a)
        self.assertEqual(one.memory.raw[:64], two.memory.raw[64:128])

    def test_seed_mismatch(self):
        b, a = buffer(), args()
        validation.fill(b, ["a", "b"], a)
        a.seed += 1
        with self.assertRaises(RuntimeError):
            validation.verify(b, ["a", "b"], a)

    def phase(self, backend, corrupt=False, verify=True):
        a, b, store = args(), buffer(), Store(corrupt)
        a.verify_data = verify
        keysets = [[["a", "b"], ["c", "d"]]]
        if backend == "yr":
            torch = SimpleNamespace(npu=SimpleNamespace(set_device=lambda _: None))
            stats = yr.phase(store, torch, a, {"get_sub_timeout_ms": 0}, None, [b], keysets, [])
        else:
            stats = mc.phase(store, a, None, [(b, b)], keysets, "put", [])
        return stats, store

    def test_yuanrong_verifies_every_put(self):
        stats, store = self.phase("yr")
        self.assertEqual(store.reads, 2)
        self.assertEqual(stats["verification"]["verified_keys"], 4)

    def test_yuanrong_detects_first_batch_corruption(self):
        with self.assertRaisesRegex(RuntimeError, "Data mismatch"):
            self.phase("yr", corrupt=True)

    def test_mooncake_verifies_every_put(self):
        stats, store = self.phase("mc")
        self.assertEqual(store.reads, 2)
        self.assertTrue(stats["verification"]["wall_includes_validation"])

    def test_mooncake_detects_first_batch_corruption(self):
        with self.assertRaisesRegex(RuntimeError, "Data mismatch"):
            self.phase("mc", corrupt=True)

    def test_disabled_verification_has_no_readback(self):
        for backend in ("yr", "mc"):
            stats, store = self.phase(backend, verify=False)
            self.assertEqual(store.reads, 0)
            self.assertFalse(stats["verification"]["enabled"])

    def test_yuanrong_default_placement_and_put_registration(self):
        a = yr.parse_args(["--operation", "put", "--key-prefix", "x", "--worker-addr", "worker:19491"])
        config = yr.load_config(a)
        self.assertEqual(config["data_placement_policy"], "PREFERRED_META_OWNER")
        self.assertFalse(config["enable_remote_h2d"])
        calls = []
        store = SimpleNamespace(pre_register_device_memory=lambda *values: calls.append(values))
        b = buffer()
        yr.register_buffers(store, a, config, [b])
        self.assertEqual(calls, [([b.address], [b.registration_size])])

    def test_get_hixl_registration_ignores_legacy_opt_out(self):
        a = yr.parse_args(["--operation", "get", "--key-prefix", "x", "--worker-addr", "worker:19491",
                           "--enable-remote-h2d", "--no-enable-dev-mem-pregister"])
        calls = []
        yr.register_buffers(SimpleNamespace(pre_register_device_memory=lambda *x: calls.append(x)),
                            a, yr.load_config(a), [buffer()])
        self.assertEqual(len(calls), 1)

    def test_env_and_explicit_placement(self):
        a = yr.parse_args(["--operation", "put", "--key-prefix", "x", "--worker-addr", "worker:19491",
                           "--data-placement-policy", "REQUIRED_SAME_NODE", "--local-ip", "sdk"])
        config = yr.load_config(a)
        self.assertEqual(config["data_placement_policy"], "REQUIRED_SAME_NODE")
        with patch.dict(os.environ, {"DS_RH2D_HCCS_BUFFER_POOL": "8:8"}, clear=True):
            yr.configure_hixl(config)
            self.assertEqual(os.environ["DS_RH2D_LINK_TYPE"], "HCCS")
            self.assertEqual(os.environ["DS_RH2D_HCCS_BUFFER_POOL"], "8:8")
            self.assertEqual(os.environ["DS_HIXL_D2RH_LOCAL_IP"], "sdk")

    def test_get_checks_early_batches_for_both_backends(self):
        for backend in ("yr", "mc"):
            store, b, a = Store(corrupt=True), buffer(), args("get")
            keysets = [[["a", "b"], ["c", "d"]]]
            for keys in keysets[0]:
                validation.fill(b, keys, a)
                store.mset_d2h_from_multi_buffers(keys, b.ptrs, b.sizes, None)
            with self.assertRaisesRegex(RuntimeError, "Data mismatch"):
                if backend == "yr":
                    torch = SimpleNamespace(npu=SimpleNamespace(set_device=lambda _: None))
                    yr.phase(store, torch, a, {"get_sub_timeout_ms": 0}, None, [b], keysets, [])
                else:
                    mc.phase(store, a, None, [(b, b)], keysets, "get")

    def test_block_reorder(self):
        b, a = buffer(), args()
        validation.fill(b, ["a", "b"], a)
        payload = b.memory.raw[:64]
        ctypes.memmove(b.address, payload[32:] + payload[:32], 64)
        with self.assertRaisesRegex(RuntimeError, "key=a block=0"):
            validation.verify(b, ["a", "b"], a)

    def test_put_rejects_unsupported_fabric(self):
        a = yr.parse_args(["--operation", "put", "--key-prefix", "x", "--worker-addr", "worker:19491",
                           "--enable-fabric-mem"])
        with self.assertRaisesRegex(ValueError, "requires HIXL"):
            yr.load_config(a)

    def test_yuanrong_aligned_arena_covers_all_blob_ranges(self):
        class Tensor:
            def __init__(self, owner, offset=0):
                self.owner, self.offset = owner, offset
            def data_ptr(self):
                return ctypes.addressof(self.owner) + self.offset
            def __getitem__(self, span):
                return Tensor(self.owner, self.offset + span.start)
            def fill_(self, value):
                pass
        torch = SimpleNamespace(uint8="uint8", empty=lambda size, **kw: Tensor(ctypes.create_string_buffer(size)))
        a = args()
        b = yr.DeviceBuffer(torch, a)
        self.assertEqual(b.address % (2 * 1024 * 1024), 0)
        for addresses, lengths in zip(b.ptrs, b.sizes):
            for address, length in zip(addresses, lengths):
                self.assertGreaterEqual(address, b.address)
                self.assertLessEqual(address + length, b.address + b.registration_size)
        self.assertLessEqual(b.address + b.registration_size,
                             b.allocation.data_ptr() + len(b.allocation.owner))

    def test_expected_generated_once_per_batch_and_old_format_compatible(self):
        b, a = buffer(), args()
        keys = ["a", "b"]
        expected = b"".join(part[3] for part in validation.chunks(keys, a.key_bytes, a.seed))
        with patch.object(validation, "chunks", wraps=validation.chunks) as generated:
            validation.fill(b, keys, a)
            self.assertEqual(ctypes.string_at(b.address, 128), expected)
            validation.fill(b, keys, a, poison=True)
            ctypes.memmove(b.address, expected, len(expected))
            validation.verify(b, keys, a)
            self.assertEqual(generated.call_count, 1)
            validation.fill(b, ["c", "d"], a)
            self.assertEqual(generated.call_count, 2)

    def test_device_batch_uses_one_upload_and_one_download(self):
        calls = dict(upload=0, download=0, invert=0)
        class Tensor:
            def __init__(self, data, start=0, end=None):
                self.data, self.start, self.end = data, start, len(data) if end is None else end
            def __getitem__(self, span):
                return Tensor(self.data, self.start + (span.start or 0), self.start + span.stop)
            def copy_(self, source):
                calls["upload"] += 1
                self.data[self.start:self.end] = source
            def bitwise_not_(self):
                calls["invert"] += 1
                self.data[self.start:self.end] = self.data[self.start:self.end].translate(validation.INVERT)
            def cpu(self):
                calls["download"] += 1
                return self
            def numpy(self):
                return self
            def tobytes(self):
                return bytes(self.data[self.start:self.end])
        memory = Tensor(bytearray(128))
        torch = SimpleNamespace(uint8="uint8", frombuffer=lambda value, **kw: value,
                                npu=SimpleNamespace(synchronize=lambda: None))
        b, a = SimpleNamespace(torch=torch, memory=memory), args()
        validation.fill(b, ["a", "b"], a)
        saved = bytes(memory.data)
        validation.fill(b, ["a", "b"], a, poison=True)
        self.assertNotEqual(bytes(memory.data), saved)
        memory.data[:] = saved
        validation.verify(b, ["a", "b"], a)
        self.assertEqual(calls, dict(upload=1, download=1, invert=1))

    def test_progress_reports_first_and_final_batch(self):
        import contextlib
        import io
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            progress = validation.Progress("put", 0, 575)
            progress.update(1, "prepare")
            progress.update(1, "readback")
            progress.update(575, "done")
        self.assertIn("batch=1/575 stage=prepare", output.getvalue())
        self.assertIn("batch=1/575 stage=readback", output.getvalue())
        self.assertIn("batch=575/575 stage=done", output.getvalue())

    def test_http_passes_verification_flag(self):
        self.assertIn("verify_data", http.WORKLOAD_FIELDS)
        session = http.Session(SimpleNamespace(get_hostname=lambda: "reader"),
                               SimpleNamespace(master="master", protocol="ascend",
                                               metadata_server="P2PHANDSHAKE", concurrency=[1]))
        spec = dict(run_id="a" * 32, key_prefix="x", a_segment="writer", master="master",
                    protocol="ascend", metadata_server="P2PHANDSHAKE", scenario="local",
                    num_keys=4, key_bytes=64, blocks_per_key=4, warmup=1, seed=42,
                    put_keys_per_batch=2, get_keys_per_batch=2, verify_data=True)
        self.assertTrue(session.create(spec)["spec"]["verify_data"])
        spec["verify_data"] = 1
        with self.assertRaisesRegex(http.ControlError, "boolean"):
            session.validate(spec)


if __name__ == "__main__":
    unittest.main()
