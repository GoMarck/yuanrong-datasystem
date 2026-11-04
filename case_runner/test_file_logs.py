"""Deterministic file-following and archive boundary tests."""

import asyncio
import io
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

from remote_agent import FileLog, OUTPUT_CHUNK
from runner import extract_results


class FileLogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "server.log"
        self.emit = mock.patch("remote_agent.emit").start()
        self.addCleanup(mock.patch.stopall)
        self.addCleanup(self.temp.cleanup)

    def watcher(self):
        watcher = FileLog({"id": "ready", "path": str(self.path), "contains": "READY"})
        self.addCleanup(watcher.close)
        return watcher

    def test_missing_file_and_split_pattern_without_newline(self):
        watcher = self.watcher()
        watcher.poll()
        self.path.write_bytes(b"x" * (OUTPUT_CHUNK - 2) + b"READY")
        watcher.poll()
        self.assertFalse(watcher.ready)
        watcher.poll()
        self.assertTrue(watcher.ready)
        watcher.poll()
        self.emit.assert_called_once_with("file_ready", id="ready")

    def test_preexisting_ready_is_not_a_new_run_signal(self):
        self.path.write_bytes(b"READY")
        watcher = self.watcher()
        watcher.poll()
        self.assertFalse(watcher.ready)
        with self.path.open("ab") as log:
            log.write(b"\nREADY")
        watcher.poll()
        self.assertTrue(watcher.ready)

    def test_rotation_does_not_join_different_files(self):
        watcher = self.watcher()
        self.path.write_bytes(b"RE")
        watcher.poll()
        self.path.rename(self.path.with_suffix(".old"))
        self.path.write_bytes(b"ADY")
        watcher.poll()
        self.assertFalse(watcher.ready)
        with self.path.open("ab") as log:
            log.write(b"\nREADY")
        watcher.poll()
        self.assertTrue(watcher.ready)

    def test_truncate_resets_match_window(self):
        watcher = self.watcher()
        self.path.write_bytes(b"x" * 100 + b"RE")
        watcher.poll()
        self.path.write_bytes(b"ADY")
        watcher.poll()
        self.assertFalse(watcher.ready)
        self.path.write_bytes(b"READY")
        watcher.poll()
        self.assertTrue(watcher.ready)

    def test_truncate_and_regrow_uses_changed_anchor(self):
        watcher = self.watcher()
        self.path.write_bytes(b"x" * 100)
        watcher.poll()
        self.path.write_bytes(b"READY" + b"y" * 200)
        watcher.poll()
        self.assertTrue(watcher.ready)

    def test_permission_error_is_not_treated_as_absent(self):
        with mock.patch("remote_agent.os.open", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                self.watcher()

    def test_nonregular_files_are_rejected_without_blocking(self):
        os.mkfifo(self.path)
        with self.assertRaisesRegex(ValueError, "regular file"):
            self.watcher()


class ArchiveTests(unittest.IsolatedAsyncioTestCase):
    def archive(self, name, kind=tarfile.REGTYPE, link=""):
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w") as tar:
            info = tarfile.TarInfo(name)
            info.type = kind
            info.linkname = link
            content = b"result" if kind == tarfile.REGTYPE else b""
            info.size = len(content)
            tar.addfile(info, io.BytesIO(content))
        stream.seek(0)
        return stream

    async def test_regular_file_is_extracted_without_preserving_remote_permissions(self):
        with tempfile.TemporaryDirectory() as temp:
            await extract_results(self.archive("logs/job/test.log"), Path(temp))
            path = Path(temp) / "logs/job/test.log"
            self.assertEqual(path.read_bytes(), b"result")
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    async def test_unsafe_members_never_escape_output(self):
        cases = [
            ("../escape", tarfile.REGTYPE, ""),
            ("/logs/escape", tarfile.REGTYPE, ""),
            ("logs/../../escape", tarfile.REGTYPE, ""),
            ("case/script.py", tarfile.REGTYPE, ""),
            ("logs/link", tarfile.SYMTYPE, "../../escape"),
            ("logs/link", tarfile.LNKTYPE, "logs/other"),
            ("logs/fifo", tarfile.FIFOTYPE, ""),
        ]
        for name, kind, link in cases:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp:
                destination = Path(temp) / "output"
                destination.mkdir()
                with self.assertRaisesRegex(ValueError, "unsafe"):
                    await extract_results(self.archive(name, kind, link), destination)
                self.assertFalse((Path(temp) / "escape").exists())

    async def test_cancelled_extraction_remains_cancellable_between_chunks(self):
        with tempfile.TemporaryDirectory() as temp:
            task = asyncio.create_task(extract_results(self.archive("logs/test.log"), Path(temp)))
            await asyncio.sleep(0)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task


if __name__ == "__main__":
    unittest.main()
