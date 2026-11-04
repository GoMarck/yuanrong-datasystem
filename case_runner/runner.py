#!/usr/bin/env python3
"""Standard-library-only multi-node, Docker-aware test case orchestration."""

import argparse
import asyncio
import base64
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import signal
import sys
import tarfile
import tempfile
import time
import uuid

DEFAULT_TIMEOUT = 600
CASE_TIMEOUT = 3600
COLLECTION_TIMEOUT = 120
COLLECTION_CONCURRENCY = 4
IO_CHUNK = 65536
FRAME_LIMIT = IO_CHUNK * 2
HEARTBEAT_SECONDS = 1
CLEANUP_TIMEOUT = 20
POLL_SECONDS = 0.05
SSH_CONNECT_TIMEOUT = 10
NAME = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]*\Z")
AGENT_NAME = ".case_runner_agent.py"
RESERVED_ENV = {"CASE_DIR", "CASE_RUN_DIR", "CASE_WORK_DIR", "CASE_LOG_DIR", "CASE_ARTIFACT_DIR"}


def fields(value, allowed, required=()):
    if not isinstance(value, dict):
        raise ValueError("expected an object")
    if set(value) - set(allowed) or set(required) - set(value):
        raise ValueError(f"invalid fields: expected {allowed}, required {required}; got {list(value)}")


def name(value):
    if not isinstance(value, str) or not NAME.fullmatch(value):
        raise ValueError(f"invalid identifier: {value!r}")


def seconds(value, allow_zero=False):
    if (type(value) not in (int, float) or not math.isfinite(value)
            or value < 0 or (value == 0 and not allow_zero)):
        raise ValueError(f"invalid timeout: {value!r}")


def shared_directories(case, directory):
    declared = case.get("shared_dirs", {})
    if not isinstance(declared, dict):
        raise ValueError("shared_dirs must map directory aliases to source paths")
    directory = directory.resolve()
    sources = {}
    for alias, value in declared.items():
        name(alias)
        destination = directory / alias
        if alias in ("case.json", AGENT_NAME) or destination.exists() or destination.is_symlink():
            raise ValueError(f"shared directory alias conflicts with case files: {alias}")
        if not isinstance(value, str) or not value.strip() or "\0" in value:
            raise ValueError(f"shared directory source must be a nonempty path: {alias}")
        source = directory / value
        if any(path.is_symlink() for path in (source, *source.parents)):
            raise ValueError(f"shared directory source must not traverse symlinks: {alias}")
        source = source.resolve()
        if not source.is_dir():
            raise ValueError(f"shared directory source is missing or not a directory: {alias}")
        if source == directory or source in directory.parents or directory in source.parents:
            raise ValueError(f"shared directory source must not overlap the case directory: {alias}")
        sources[alias] = source
    return sources


def validate_source_tree(directory):
    for path in directory.rglob("*"):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError(f"source directory must contain only regular files/directories: {path.name}")


def validate(case, directory):
    fields(case, ("version", "nodes", "targets", "steps", "timeout",
                  "failure_collect_seconds", "collection_timeout", "shared_dirs"),
           ("version", "nodes", "targets", "steps"))
    if type(case["version"]) is not int or case["version"] != 1:
        raise ValueError("only version 1 is supported")
    shared = shared_directories(case, directory)
    for source in (directory, *shared.values()):
        validate_source_tree(source)
    seconds(case.get("timeout", CASE_TIMEOUT))
    seconds(case.get("collection_timeout", COLLECTION_TIMEOUT))
    seconds(case.get("failure_collect_seconds", 0), allow_zero=True)
    nodes = case["nodes"]
    if not isinstance(nodes, dict) or not nodes:
        raise ValueError("nodes must be a nonempty node mapping")
    for key, node in nodes.items():
        name(key)
        fields(node, ("ssh", "local"))
        if node.get("local") is True and "ssh" not in node:
            continue
        if set(node) != {"ssh"} or not isinstance(node["ssh"], str) or not NAME.fullmatch(node["ssh"]):
            raise ValueError("node must contain local: true or ssh: <SSH config alias>")
    if not isinstance(case["targets"], dict) or not case["targets"]:
        raise ValueError("targets must be a nonempty mapping")
    for key, target in case["targets"].items():
        name(key)
        fields(target, ("node", "container"), ("node",))
        name(target["node"])
        if target["node"] not in nodes:
            raise ValueError(f"unknown node in target {key}")
        if "container" in target:
            name(target["container"])
    if not isinstance(case["steps"], list) or not case["steps"]:
        raise ValueError("steps must be a nonempty list")
    jobs = {}
    for step in case["steps"]:
        fields(step, ("run", "start", "wait_log", "timeout"))
        actions = set(step) - {"timeout"}
        if len(actions) != 1:
            raise ValueError("each step needs exactly one of run/start/wait_log")
        seconds(step.get("timeout", DEFAULT_TIMEOUT))
        action = next(iter(actions))
        if action == "start" and "timeout" in step:
            raise ValueError("start does not wait; use wait_log with timeout for readiness")
        if action == "wait_log":
            wait = step[action]
            fields(wait, ("job", "contains", "source", "path"), ("job", "contains"))
            name(wait["job"])
            if wait["job"] not in jobs or jobs[wait["job"]] != "start":
                raise ValueError("wait_log must reference a previously started service")
            if not isinstance(wait["contains"], str) or not wait["contains"]:
                raise ValueError("contains must be a nonempty literal string")
            source = wait.get("source", "console")
            if source not in ("console", "file"):
                raise ValueError("wait_log source must be console or file")
            if source == "console" and "path" in wait:
                raise ValueError("console wait_log does not accept path")
            if source == "file":
                path = wait.get("path")
                if (not isinstance(path, str) or not path or "\0" in path
                        or ".." in PurePosixPath(path).parts):
                    raise ValueError("file wait_log requires path without '..'")
            continue
        if not isinstance(step[action], list) or not step[action]:
            raise ValueError(f"{action} must be a nonempty job list")
        for job in step[action]:
            fields(job, ("id", "target", "script", "command", "args", "env"), ("id", "target"))
            if ("script" in job) == ("command" in job):
                raise ValueError("each job requires exactly one of script or command")
            name(job["id"])
            if job["id"] in jobs:
                raise ValueError(f"duplicate job id: {job['id']}")
            jobs[job["id"]] = action
            name(job["target"])
            if job["target"] not in case["targets"]:
                raise ValueError(f"unknown target: {job['target']}")
            if "command" in job:
                command = job["command"]
                if not isinstance(command, str) or not command.strip() or "\0" in command:
                    raise ValueError("command must be a nonempty shell command string without NUL")
                if "args" in job:
                    raise ValueError("args is only supported with script; put command arguments in command")
            else:
                script = job["script"]
                if not isinstance(script, str) or not script:
                    raise ValueError("script must be a relative path")
                relative = PurePosixPath(script)
                if relative.is_absolute() or ".." in relative.parts:
                    raise ValueError("scripts must stay inside the case directory")
                if relative.parts and relative.parts[0] in shared:
                    path = shared[relative.parts[0]].joinpath(*relative.parts[1:])
                else:
                    path = directory / script
                if not path.is_file() or not os.access(path, os.X_OK):
                    raise ValueError(f"script missing or not executable: {script}")
            if not isinstance(job.get("args", []), list) or not all(
                    isinstance(arg, str) and "\0" not in arg for arg in job.get("args", [])):
                raise ValueError("args must be a string array")
            env = job.get("env", {})
            if not isinstance(env, dict) or not all(
                    isinstance(k, str) and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", k)
                    and isinstance(v, str) and "\0" not in v for k, v in env.items()):
                raise ValueError("env must map environment names to strings")
            if RESERVED_ENV.intersection(env):
                raise ValueError("CASE_* directory variables are owned by the runner")
    if (directory / AGENT_NAME).exists():
        raise ValueError(f"reserved case filename: {AGENT_NAME}")


async def extract_results(archive, destination):
    """Extract only regular files/directories within the two declared result roots."""
    with tarfile.open(fileobj=archive, mode="r|") as tar:
        for member in tar:
            relative = PurePosixPath(member.name)
            if (relative.is_absolute() or ".." in relative.parts or not relative.parts
                    or relative.parts[0] not in ("logs", "artifacts")
                    or not (member.isdir() or member.isfile())):
                raise ValueError(f"unsafe result archive member: {member.name}")
            path = destination.joinpath(*relative.parts)
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True, mode=0o700)
            else:
                path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                with tar.extractfile(member) as source, path.open("xb") as target:
                    os.chmod(path, 0o600)
                    while True:
                        chunk = source.read(IO_CHUNK)
                        if not chunk:
                            break
                        target.write(chunk)
                        await asyncio.sleep(0)
            await asyncio.sleep(0)


class Transport:
    def __init__(self, nodes, targets):
        self.nodes = nodes
        self.targets = targets

    def command(self, target_id, argv, interactive=False):
        target = self.targets[target_id]
        node = self.nodes[target["node"]]
        if "container" in target:
            argv = ["docker", "exec"] + (["-i"] if interactive else []) + [target["container"]] + argv
        if node.get("local"):
            return argv
        return [
            "ssh", "-T", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
            "-o", f"ConnectTimeout={SSH_CONNECT_TIMEOUT}",
            "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3",
            node["ssh"], shlex.join(argv),
        ]


class Job:
    def __init__(self, spec, service, process, output, waits):
        self.spec = spec
        self.service = service
        self.process = process
        self.output = output
        self.ready = {key: asyncio.Event() for key in waits}
        self.patterns = [(wait["contains"].encode(), self.ready[key])
                         for key, wait in waits.items() if wait.get("source", "console") == "console"]
        self.error = None
        self.reader = asyncio.create_task(self.collect())
        self.heartbeat = asyncio.create_task(self.keep_alive())
        self.started = time.time()

    async def collect(self):
        try:
            await self.collect_records()
        except Exception as exc:
            self.error = exc
            # Keep draining so a diagnostic or disk error cannot block remote shutdown.
            while await self.process.stdout.read(IO_CHUNK):
                pass

    async def collect_records(self):
        overlap = b""
        retained = max((len(pattern) for pattern, _ in self.patterns), default=1) - 1
        with self.output.open("wb") as log:
            while True:
                frame = await self.process.stdout.readline()
                if not frame:
                    break
                try:
                    record = json.loads(frame)
                except json.JSONDecodeError as exc:
                    log.write(frame)
                    log.flush()
                    raise RuntimeError(f"{self.spec['id']}: invalid agent response; see {self.output.name}") from exc
                if record["type"] == "file_ready":
                    self.ready[record["id"]].set()
                    continue
                if record["type"] == "error":
                    raise RuntimeError(f"{self.spec['id']}: {record['message']}")
                if record["type"] != "output":
                    raise RuntimeError(f"unknown agent record: {record['type']}")
                data = base64.b64decode(record["data"], validate=True)
                log.write(data)
                log.flush()
                window = overlap + data
                for pattern, event in self.patterns:
                    if pattern in window:
                        event.set()
                overlap = window[-retained:] if retained else b""

    async def keep_alive(self):
        try:
            while self.process.returncode is None:
                self.process.stdin.write(b".\n")
                await self.process.stdin.drain()
                await asyncio.sleep(HEARTBEAT_SECONDS)
        except (BrokenPipeError, ConnectionResetError):
            pass


class Runner:
    def __init__(self, case, directory, output_root):
        self.case = case
        self.directory = directory
        self.shared_dirs = shared_directories(case, directory)
        output_root = output_root.resolve()
        for source in (directory.resolve(), *self.shared_dirs.values()):
            if output_root == source or source in output_root.parents:
                raise ValueError("output must be outside the case and shared source directories")
        self.run_id = uuid.uuid4().hex
        self.output = output_root / directory.name / self.run_id
        self.transport = Transport(case["nodes"], case["targets"])
        self.remote_root = "/tmp/case-runner-" + self.run_id
        self.jobs = {}
        self.events = []
        self.cleanup_errors = []
        self.collection_errors = []
        self.collections = {}
        self.staged = []

    def event(self, message):
        self.events.append({"time": time.time(), "message": message})
        print(message, flush=True)

    async def prepare(self):
        shared = shared_directories(self.case, self.directory)
        if shared != self.shared_dirs:
            raise ValueError("shared directory sources changed before staging")
        for source in (self.directory, *shared.values()):
            validate_source_tree(source)
        with tempfile.TemporaryFile() as archive:
            with tarfile.open(fileobj=archive, mode="w") as tar:
                for path in sorted(self.directory.iterdir()):
                    tar.add(path, arcname=path.name)
                for alias, source in sorted(shared.items()):
                    tar.add(source, arcname=alias)
                tar.add(Path(__file__).with_name("remote_agent.py"), arcname=AGENT_NAME)
            for target in self.case["targets"]:
                archive.seek(0)
                root = self.root(target)
                shell = (
                    f"umask 077; mkdir -p {shlex.quote(self.remote_root)}"
                    f" && mkdir {shlex.quote(root)}"
                    f" && mkdir {shlex.join([root + '/' + part for part in ('case', 'work', 'logs', 'artifacts')])}"
                    f" && tar -xf - -C {shlex.quote(root + '/case')}"
                )
                argv = self.transport.command(target, ["sh", "-c", shell], interactive=True)
                with (self.output / "prepare" / (target + ".log")).open("wb") as log:
                    process = await asyncio.create_subprocess_exec(
                        *argv, stdin=archive, stdout=log, stderr=asyncio.subprocess.STDOUT,
                        start_new_session=True,
                    )
                    try:
                        await asyncio.wait_for(process.wait(), DEFAULT_TIMEOUT)
                    finally:
                        await self.stop_local(process)
                if process.returncode != 0:
                    raise RuntimeError(f"staging {target} failed: {process.returncode}")
                self.staged.append(target)
                self.event(f"prepared {target}: {root}")

    def root(self, target):
        return self.remote_root + "/" + target

    def environment(self, spec):
        root = self.root(spec["target"])
        return {
            **spec.get("env", {}),
            "CASE_RUN_DIR": root,
            "CASE_DIR": root + "/case",
            "CASE_WORK_DIR": root + "/work/" + spec["id"],
            "CASE_LOG_DIR": root + "/logs/" + spec["id"],
            "CASE_ARTIFACT_DIR": root + "/artifacts/" + spec["id"],
        }

    async def launch(self, spec, service):
        root = self.root(spec["target"])
        argv = self.transport.command(
            spec["target"], ["python3", "-u", root + "/case/" + AGENT_NAME], interactive=True)
        waits = {str(index): step["wait_log"] for index, step in enumerate(self.case["steps"])
                 if "wait_log" in step and step["wait_log"]["job"] == spec["id"]}
        env = self.environment(spec)
        file_waits = [
            {"id": key, "contains": wait["contains"],
             "path": str(PurePosixPath(env["CASE_LOG_DIR"]) / wait["path"])}
            for key, wait in waits.items() if wait.get("source") == "file"
        ]
        process = await asyncio.create_subprocess_exec(
            *argv, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT, start_new_session=True, limit=FRAME_LIMIT,
        )
        command = (["sh", "-c", spec["command"]] if "command" in spec
                   else [env["CASE_DIR"] + "/" + spec["script"]] + spec.get("args", []))
        payload = {
            "command": command,
            "cwd": env["CASE_WORK_DIR"], "env": env, "file_waits": file_waits,
            "directories": [env[key] for key in ("CASE_WORK_DIR", "CASE_LOG_DIR", "CASE_ARTIFACT_DIR")],
        }
        process.stdin.write(json.dumps(payload).encode() + b"\n")
        job = Job(spec, service, process, self.output / "console" / (spec["id"] + ".log"), waits)
        self.jobs[spec["id"]] = job
        self.event(f"started {spec['id']} on {spec['target']}")

    def check_health(self):
        for key, job in self.jobs.items():
            if job.error is not None:
                raise job.error
            if job.reader.done():
                job.reader.result()
            if job.heartbeat.done():
                job.heartbeat.result()
            if job.service and job.process.returncode is not None:
                raise RuntimeError(f"service {key} exited unexpectedly: {job.process.returncode}")

    async def wait_until(self, predicate, timeout, description):
        deadline = time.monotonic() + timeout
        while True:
            self.check_health()
            if predicate():
                return
            if time.monotonic() >= deadline:
                raise RuntimeError(f"timeout: {description}")
            await asyncio.sleep(POLL_SECONDS)

    async def execute_steps(self):
        for index, step in enumerate(self.case["steps"]):
            self.check_health()
            self.event(f"step {index + 1}")
            timeout = step.get("timeout", DEFAULT_TIMEOUT)
            if "wait_log" in step:
                wait = step["wait_log"]
                event = self.jobs[wait["job"]].ready[str(index)]
                await self.wait_until(event.is_set, timeout, f"log from {wait['job']}")
                continue
            service = "start" in step
            specs = step["start"] if service else step["run"]
            # launch() only waits for process creation, never for script completion.
            for spec in specs:
                await self.launch(spec, service)
            if service:
                continue

            def completed():
                for spec in specs:
                    code = self.jobs[spec["id"]].process.returncode
                    if code is not None and code != 0:
                        raise RuntimeError(f"job {spec['id']} failed: {code}")
                return all(self.jobs[spec["id"]].process.returncode is not None for spec in specs)

            await self.wait_until(completed, timeout, "run " + ", ".join(s["id"] for s in specs))
        self.check_health()

    @staticmethod
    async def stop_local(process):
        if process.returncode is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        await asyncio.wait_for(process.wait(), CLEANUP_TIMEOUT)

    async def cleanup(self):
        async def stop(key, job):
            job.heartbeat.cancel()
            await asyncio.gather(job.heartbeat, return_exceptions=True)
            job.process.stdin.close()
            try:
                await asyncio.wait_for(job.process.wait(), CLEANUP_TIMEOUT)
            except asyncio.TimeoutError:
                self.cleanup_errors.append(f"{key}: remote cleanup unconfirmed; heartbeat lease will expire")
                try:
                    await self.stop_local(job.process)
                except asyncio.TimeoutError:
                    self.cleanup_errors.append(f"{key}: transport did not close after kill")
            try:
                await asyncio.wait_for(job.reader, CLEANUP_TIMEOUT)
            except (Exception, asyncio.CancelledError) as exc:
                self.cleanup_errors.append(f"{key}: log collection failed: {exc}")
            if job.error is not None:
                self.cleanup_errors.append(f"{key}: log collection failed: {job.error}")

        results = await asyncio.gather(*(stop(key, job) for key, job in self.jobs.items()), return_exceptions=True)
        for key, result in zip(self.jobs, results):
            if isinstance(result, BaseException):
                self.cleanup_errors.append(f"{key}: {result}")

    async def collect_target(self, target):
        argv = self.transport.command(
            target, ["tar", "-cf", "-", "-C", self.root(target), "logs", "artifacts"])
        diagnostics = self.output / "collection" / target
        diagnostics.mkdir(mode=0o700)
        log_path = diagnostics / "transfer.log"
        destination = self.output / "artifacts" / target
        partial = diagnostics / "files.partial"
        with tempfile.TemporaryFile() as archive, log_path.open("wb") as log:
            process = await asyncio.create_subprocess_exec(
                *argv, stdin=asyncio.subprocess.DEVNULL, stdout=archive, stderr=log,
                start_new_session=True,
            )
            try:
                await process.wait()
            finally:
                await self.stop_local(process)
            if process.returncode != 0:
                raise RuntimeError(
                    f"archive command exited {process.returncode}; see {log_path.relative_to(self.output)}")
            archive.seek(0)
            partial.mkdir(mode=0o700)
            await extract_results(archive, partial)
            partial.rename(destination)
        self.collections[target] = {"status": "ok", "path": str(destination.relative_to(self.output))}

    async def collect_results(self):
        limit = asyncio.Semaphore(COLLECTION_CONCURRENCY)

        async def collect(target):
            async with limit:
                try:
                    await asyncio.wait_for(
                        self.collect_target(target), self.case.get("collection_timeout", COLLECTION_TIMEOUT))
                    self.event(f"collected {target}")
                except Exception as exc:
                    error = str(exc) or type(exc).__name__
                    self.collection_errors.append(f"{target}: {error}")
                    self.collections[target] = {"status": "failed", "error": error}
                    self.event(f"collection failed for {target}: {error}")

        for target in self.case["targets"]:
            self.collections[target] = {"status": "not_prepared"}
        await asyncio.gather(*(collect(target) for target in self.staged))

    async def finish_cleanup(self):
        async def finalize():
            await self.cleanup()
            await self.collect_results()

        cleanup = asyncio.create_task(finalize())
        interrupted = False
        while True:
            try:
                await asyncio.shield(cleanup)
                return interrupted
            except asyncio.CancelledError:
                # Interrupts must not abandon remote cleanup or the result file.
                interrupted = True

    async def run(self):
        self.output.mkdir(parents=True, exist_ok=False, mode=0o700)
        for part in ("prepare", "console", "collection", "artifacts"):
            (self.output / part).mkdir(mode=0o700)
        self.event(f"results: {self.output}")
        error = None
        interrupted = False
        try:
            async def workflow():
                await self.prepare()
                await self.execute_steps()
            await asyncio.wait_for(workflow(), self.case.get("timeout", CASE_TIMEOUT))
        except asyncio.CancelledError:
            error, interrupted = "interrupted", True
        except Exception as exc:
            error = str(exc) or type(exc).__name__
        finally:
            if error:
                self.event(f"FAILED: {error}")
                if not interrupted:
                    try:
                        await asyncio.sleep(self.case.get("failure_collect_seconds", 0))
                    except asyncio.CancelledError:
                        interrupted = True
            if await self.finish_cleanup() and error is None:
                error = "interrupted"
            summary = {
                "run_id": self.run_id,
                "success": error is None and not self.cleanup_errors and not self.collection_errors,
                "error": error, "cleanup_errors": self.cleanup_errors,
                "collection_errors": self.collection_errors, "collections": self.collections,
                "remote_directories": {key: self.root(key) for key in self.staged},
                "jobs": {key: {"target": job.spec["target"], "service": job.service,
                               "returncode": job.process.returncode,
                               "log": str(job.output.relative_to(self.output)),
                               "directories": {k: v for k, v in self.environment(job.spec).items()
                                               if k in RESERVED_ENV},
                               "started": job.started}
                         for key, job in self.jobs.items()},
                "events": self.events,
            }
            (self.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        return 0 if summary["success"] else 1


async def run_interruptibly(runner):
    loop = asyncio.get_running_loop()
    task = asyncio.create_task(runner.run())
    stopping = False

    def interrupt():
        nonlocal stopping
        if not stopping:
            stopping = True
            task.cancel()

    for signum in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(signum, interrupt)
    try:
        return await task
    finally:
        for signum in (signal.SIGINT, signal.SIGTERM):
            loop.remove_signal_handler(signum)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case_dir", type=Path)
    parser.add_argument("--output", type=Path, default=Path("case-runs"),
                        help="result root outside case_dir; each run creates case-name/run-id below it")
    parser.add_argument("--check", action="store_true", help="validate without connecting to nodes")
    args = parser.parse_args()
    try:
        directory = args.case_dir.resolve()
        case = json.loads((directory / "case.json").read_text(encoding="utf-8"))
        validate(case, directory)
        if args.check:
            print("Configuration OK")
            return 0
        output = args.output.resolve()
        return asyncio.run(run_interruptibly(Runner(case, directory, output)))
    except (ValueError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
