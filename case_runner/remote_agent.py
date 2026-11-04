#!/usr/bin/env python3
"""Run one foreground command; stop its process group when controller contact is lost."""

import base64
import json
import os
from pathlib import Path
import selectors
import signal
import stat
import subprocess
import sys
import time

HEARTBEAT_TIMEOUT = 15
POLL_SECONDS = 0.1
STOP_GRACE_SECONDS = 2
CONTROL_FAILURE = 125
CONTROL_CHUNK = 4096
OUTPUT_CHUNK = 65536
FILE_ANCHOR_BYTES = 64


def emit(kind, **fields):
    print(json.dumps({"type": kind, **fields}), flush=True)


class FileLog:
    """Follow a pathname, keeping a bounded match window and skipping pre-run bytes."""

    def __init__(self, spec):
        self.spec = spec
        self.pattern = spec["contains"].encode()
        self.file = None
        self.identity = None
        self.overlap = b""
        self.anchor = b""
        self.ready = False
        self.open_current(skip_existing=True)

    def close(self):
        if self.file is not None:
            self.file.close()
            self.file = None

    def open_current(self, skip_existing=False):
        try:
            fd = os.open(self.spec["path"], os.O_RDONLY | os.O_NONBLOCK)
        except FileNotFoundError:
            return
        metadata = os.fstat(fd)
        if not stat.S_ISREG(metadata.st_mode):
            os.close(fd)
            raise ValueError(f"wait_log requires a regular file: {self.spec['path']}")
        self.close()
        self.file = os.fdopen(fd, "rb", buffering=0)
        self.identity = (metadata.st_dev, metadata.st_ino)
        self.overlap = b""
        self.anchor = b""
        if skip_existing:
            self.file.seek(0, os.SEEK_END)
            self.remember_anchor()

    def remember_anchor(self):
        position = self.file.tell()
        self.file.seek(max(0, position - FILE_ANCHOR_BYTES))
        self.anchor = self.file.read(position - self.file.tell())
        self.file.seek(position)

    def read_chunk(self):
        position = self.file.tell()
        self.file.seek(position - len(self.anchor))
        overwritten = self.file.read(len(self.anchor)) != self.anchor
        self.file.seek(position)
        if os.fstat(self.file.fileno()).st_size < position or overwritten:
            self.file.seek(0)
            self.overlap = b""
        data = self.file.read(OUTPUT_CHUNK)
        if not data:
            return False
        window = self.overlap + data
        if self.pattern in window:
            self.ready = True
            emit("file_ready", id=self.spec["id"])
        retained = len(self.pattern) - 1
        self.overlap = window[-retained:] if retained else b""
        self.remember_anchor()
        return True

    def poll(self):
        if self.ready:
            return
        if self.file is None:
            self.open_current()
        if self.file is not None and self.read_chunk():
            return
        try:
            metadata = os.stat(self.spec["path"])
        except FileNotFoundError:
            return
        if self.identity != (metadata.st_dev, metadata.st_ino):
            self.open_current()
            if self.file is not None:
                self.read_chunk()


def stop_group(process):
    """Also terminate descendants when the foreground script has already exited."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + STOP_GRACE_SECONDS
    while time.monotonic() < deadline:
        process.poll()
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            return
        time.sleep(POLL_SECONDS)
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def read_config():
    # Read the startup frame without buffering away subsequent heartbeat bytes.
    frame = bytearray()
    while not frame.endswith(b"\n"):
        data = os.read(sys.stdin.fileno(), 1)
        if not data:
            raise EOFError("controller disconnected before startup")
        frame.extend(data)
    return json.loads(frame)


def supervise(process, watchers):
    interrupted = False

    def interrupt(_signum, _frame):
        nonlocal interrupted
        interrupted = True

    signal.signal(signal.SIGTERM, interrupt)
    signal.signal(signal.SIGINT, interrupt)
    last_contact = time.monotonic()
    with selectors.DefaultSelector() as selector:
        selector.register(sys.stdin, selectors.EVENT_READ, "control")
        selector.register(process.stdout, selectors.EVENT_READ, "output")
        while process.poll() is None:
            if interrupted or time.monotonic() - last_contact > HEARTBEAT_TIMEOUT:
                return CONTROL_FAILURE
            for key, _ in selector.select(POLL_SECONDS):
                data = os.read(key.fd, OUTPUT_CHUNK if key.data == "output" else CONTROL_CHUNK)
                if key.data == "control":
                    if not data:
                        return CONTROL_FAILURE
                    last_contact = time.monotonic()
                elif data:
                    emit("output", data=base64.b64encode(data).decode("ascii"))
                else:
                    selector.unregister(process.stdout)
            for watcher in watchers:
                watcher.poll()
    return process.returncode if process.returncode >= 0 else 1


def main():
    config = read_config()
    environment = os.environ.copy()
    environment.update(config["env"])
    for directory in config.get("directories", []):
        Path(directory).mkdir(parents=True, exist_ok=True, mode=0o700)
    watchers = []
    process = None
    try:
        for spec in config.get("file_waits", []):
            watchers.append(FileLog(spec))
        process = subprocess.Popen(
            config["command"], cwd=config["cwd"], env=environment,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        return supervise(process, watchers)
    except Exception as exc:
        emit("error", message=str(exc))
        return CONTROL_FAILURE
    finally:
        if process is not None:
            stop_group(process)
            # Drain shutdown output without waiting forever for escaped descendants.
            os.set_blocking(process.stdout.fileno(), False)
            while True:
                data = process.stdout.read(OUTPUT_CHUNK)
                if not data:
                    break
                emit("output", data=base64.b64encode(data).decode("ascii"))
            process.stdout.close()
        for watcher in watchers:
            watcher.close()


if __name__ == "__main__":
    sys.exit(main())
