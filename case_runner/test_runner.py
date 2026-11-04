"""Local process tests; no cluster, Docker daemon, or accelerator required."""

import asyncio
import copy
import json
import os
from pathlib import Path
import shlex
import shutil
import sys
import tempfile
import unittest
from unittest import mock

from runner import Runner, Transport, validate


class RunnerTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.directory = self.base / "case"
        self.directory.mkdir()
        self.case = {
            "version": 1, "timeout": 5,
            "nodes": {"node1": {"local": True}, "node2": {"local": True}},
            "targets": {"first": {"node": "node1"}, "second": {"node": "node2"}},
            "steps": [],
        }
        self.runners = []

    def tearDown(self):
        for runner in self.runners:
            shutil.rmtree(runner.remote_root, ignore_errors=True)
        self.temp.cleanup()

    def script(self, filename, source):
        path = self.directory / filename
        path.write_text(f"#!{sys.executable}\n" + source, encoding="utf-8")
        path.chmod(0o700)
        return filename

    def job(self, key, script, target="first"):
        return {"id": key, "script": script, "target": target}

    async def execute(self):
        validate(self.case, self.directory)
        output = self.base / f"logs-{len(self.runners)}"
        runner = Runner(self.case, self.directory, output)
        self.runners.append(runner)
        code = await runner.run()
        return code, json.loads((runner.output / "summary.json").read_text()), runner.output

    async def test_shared_scripts_commands_and_assets_are_staged_without_local_copy(self):
        common = self.base / "shared scripts"
        common.mkdir()
        (common / "data.txt").write_text("shared data")
        (common / "utils.sh").write_text("helper() { printf 'shared helper'; }\n")
        self.script("check.py", (
            "import os, pathlib, sys\n"
            "data = pathlib.Path(os.environ['CASE_DIR'], 'common/data.txt').read_text()\n"
            "pathlib.Path(os.environ['CASE_ARTIFACT_DIR'], 'result.txt').write_text(data + ':' + sys.argv[1])\n"
        ))
        self.script("service.py", "import time\nprint('READY', flush=True)\ntime.sleep(30)\n")
        for filename in ("check.py", "service.py"):
            (self.directory / filename).rename(common / filename)
        self.case["shared_dirs"] = {"common": "../shared scripts"}
        self.case["steps"] = [
            {"start": [self.job("service", "common/service.py")]},
            {"wait_log": {"job": "service", "contains": "READY"}},
            {"run": [
                {**self.job("check", "common/check.py"), "args": ["argument with spaces"]},
                {"id": "helper", "target": "second",
                 "command": '. "$CASE_DIR/common/utils.sh" && helper > "$CASE_LOG_DIR/helper.log"'},
            ]},
        ]
        original = (common / "check.py").read_bytes()
        code, summary, output = await self.execute()
        self.assertEqual(code, 0, summary)
        self.assertEqual((output / "artifacts/first/artifacts/check/result.txt").read_text(),
                         "shared data:argument with spaces")
        self.assertEqual((output / "artifacts/second/logs/helper/helper.log").read_text(), "shared helper")
        self.assertFalse((self.directory / "common").exists())
        self.assertEqual((common / "check.py").read_bytes(), original)
        for target in self.case["targets"]:
            self.assertTrue((Path(self.runners[-1].root(target)) / "case/common/check.py").is_file())

    def test_shared_directory_validation(self):
        common = self.base / "common"
        common.mkdir()
        script = common / "check.sh"
        script.write_text("#!/bin/sh\ntrue\n")
        script.chmod(0o700)
        self.case["steps"] = [{"run": [self.job("check", "common/check.sh")]}]
        invalid = [
            [], {"common": ""}, {"common": None}, {"common": "bad\0path"},
            {"common": "../missing"}, {"common": "../common/check.sh"},
            {"../common": "../common"}, {"nested/common": "../common"},
            {"case.json": "../common"}, {"common": "."}, {"common": ".."},
        ]
        for declaration in invalid:
            with self.subTest(declaration=declaration), self.assertRaises(ValueError):
                validate({**self.case, "shared_dirs": declaration}, self.directory)
        self.case["shared_dirs"] = {"common": "../common"}
        validate(self.case, self.directory)
        script.chmod(0o600)
        with self.assertRaisesRegex(ValueError, "not executable"):
            validate(self.case, self.directory)
        script.chmod(0o700)
        self.case["steps"][0]["run"][0]["script"] = "common/../check.sh"
        with self.assertRaisesRegex(ValueError, "inside"):
            validate(self.case, self.directory)

    async def test_two_cases_share_source_but_have_isolated_remote_copies(self):
        common = self.base / "common"
        common.mkdir()
        (common / "value.txt").write_text("shared value")
        second = self.base / "case2"
        second.mkdir()
        self.case["shared_dirs"] = {"common": "../common"}
        self.case["steps"] = [{"run": [{
            "id": "check", "target": "first",
            "command": 'cat "$CASE_DIR/common/value.txt" && printf private > "$CASE_DIR/common/value.txt"',
        }]}]
        pair = []
        for directory in (self.directory, second):
            validate(self.case, directory)
            pair.append(Runner(self.case, directory, self.base / "results"))
        self.runners.extend(pair)
        self.assertEqual(await asyncio.gather(*(runner.run() for runner in pair)), [0, 0])
        for runner in pair:
            self.assertEqual((runner.output / "console/check.log").read_text(), "shared value")
            self.assertEqual((Path(runner.root("first")) / "case/common/value.txt").read_text(), "private")
        self.assertNotEqual(pair[0].root("first"), pair[1].root("first"))
        self.assertEqual((common / "value.txt").read_text(), "shared value")

    def test_shared_directories_reject_collisions_symlinks_and_special_files(self):
        common = self.base / "common"
        common.mkdir()
        self.case["steps"] = [{"run": [{"id": "check", "target": "first", "command": "true"}]}]
        self.case["shared_dirs"] = {"common": "../common"}
        (self.directory / "common").write_text("do not overwrite")
        with self.assertRaisesRegex(ValueError, "conflicts"):
            validate(self.case, self.directory)
        (self.directory / "common").unlink()
        (self.directory / "common").mkdir()
        with self.assertRaisesRegex(ValueError, "conflicts"):
            validate(self.case, self.directory)
        (self.directory / "common").rmdir()
        (common / "link").symlink_to(self.directory)
        with self.assertRaisesRegex(ValueError, "regular"):
            validate(self.case, self.directory)
        (common / "link").unlink()
        alias = self.base / "alias"
        alias.symlink_to(common, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlinks"):
            validate({**self.case, "shared_dirs": {"common": "../alias"}}, self.directory)
        os.mkfifo(common / "fifo")
        with self.assertRaisesRegex(ValueError, "regular"):
            validate(self.case, self.directory)

    def test_output_cannot_be_staged_back_into_shared_sources(self):
        common = self.base / "common"
        common.mkdir()
        self.case["shared_dirs"] = {"common": "../common"}
        for output in (common, common / "results", self.directory / "results"):
            with self.subTest(output=output), self.assertRaisesRegex(ValueError, "outside"):
                Runner(self.case, self.directory, output)

    async def test_shared_paths_are_resolved_from_case_directory_in_cli(self):
        common = self.base / "common"
        common.mkdir()
        (common / "data.txt").write_text("shared CLI")
        self.case["shared_dirs"] = {"common": "../common"}
        self.case["steps"] = [{"run": [
            {"id": "check", "target": "first", "command": 'cat "$CASE_DIR/common/data.txt"'}
        ]}]
        (self.directory / "case.json").write_text(json.dumps(self.case))
        runner_path = Path(__file__).with_name("runner.py").resolve()
        output = self.base / "cli-shared-output"
        argv = [sys.executable, str(runner_path), str(self.directory), "--output", str(output)]
        try:
            for extra in (["--check"], []):
                process = await asyncio.create_subprocess_exec(
                    *argv, *extra, cwd=str(common), stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.STDOUT)
                stdout, _ = await asyncio.wait_for(process.communicate(), 3)
                self.assertEqual(process.returncode, 0, stdout.decode())
            result = next(output.glob("case/*/summary.json")).parent
            self.assertEqual((result / "console/check.log").read_text(), "shared CLI")
        finally:
            for result in output.glob("case/*/summary.json"):
                summary = json.loads(result.read_text())
                for path in summary["remote_directories"].values():
                    shutil.rmtree(Path(path).parent, ignore_errors=True)

    async def test_run_command_supports_shell_and_target_environment(self):
        self.case["steps"] = [{"run": [{
            "id": "command", "target": "first",
            "env": {"MESSAGE": "spaces; $(not-executed)"},
            "command": (
                'test "$PWD" = "$CASE_WORK_DIR" && '
                'printf "%s" "$MESSAGE" | cat > "$CASE_ARTIFACT_DIR/result.txt" && '
                'printf "file log" > "$CASE_LOG_DIR/test.log" && '
                'printf "console OK\\n" && printf "stderr OK\\n" >&2'
            ),
        }]}]
        code, summary, output = await self.execute()
        self.assertEqual(code, 0, summary)
        self.assertEqual((output / "artifacts/first/artifacts/command/result.txt").read_text(),
                         "spaces; $(not-executed)")
        self.assertEqual((output / "artifacts/first/logs/command/test.log").read_text(), "file log")
        self.assertIn("console OK", (output / "console/command.log").read_text())
        self.assertIn("stderr OK", (output / "console/command.log").read_text())

    async def test_start_command_supports_both_readiness_sources_and_script_jobs(self):
        script = self.script("bench.py", "print('bench OK')\n")
        self.case["steps"] = [
            {"start": [{
                "id": "service", "target": "first",
                "command": 'printf "READY" > "$CASE_LOG_DIR/server.log" && printf "STARTED\\n" && exec sleep 30',
            }]},
            {"wait_log": {"job": "service", "contains": "STARTED"}},
            {"wait_log": {"job": "service", "source": "file", "path": "server.log", "contains": "READY"}},
            {"run": [self.job("bench", script), {"id": "other", "target": "second", "command": "true"}]},
        ]
        code, summary, output = await self.execute()
        self.assertEqual(code, 0, summary)
        self.assertIn("bench OK", (output / "console/bench.log").read_text())
        self.assertEqual((output / "artifacts/first/logs/service/server.log").read_text(), "READY")
        self.assertIsNotNone(summary["jobs"]["service"]["returncode"])

    async def test_command_failure_blocks_next_step_and_collects_logs(self):
        self.case["steps"] = [
            {"run": [{"id": "fail", "target": "first",
                      "command": 'printf "failed" > "$CASE_LOG_DIR/error.log"; exit 7'}]},
            {"run": [{"id": "later", "target": "first", "command": "true"}]},
        ]
        code, summary, output = await self.execute()
        self.assertEqual(code, 1)
        self.assertIn("fail failed: 7", summary["error"])
        self.assertNotIn("later", summary["jobs"])
        self.assertEqual((output / "artifacts/first/logs/fail/error.log").read_text(), "failed")

    def test_script_command_exclusivity_and_command_validation(self):
        script = self.script("ok.py", "print('ok')\n")
        for action in ("run", "start"):
            for choice in (
                {}, {"script": script, "command": "true"}, {"command": ""},
                {"command": " \n\t"}, {"command": None}, {"command": ["echo", "hello"]},
                {"command": "echo \0"}, {"command": "true", "args": []},
            ):
                with self.subTest(action=action, choice=choice):
                    self.case["steps"] = [{action: [{"id": "job", "target": "first", **choice}]}]
                    with self.assertRaises(ValueError):
                        validate(self.case, self.directory)

    async def test_parallel_ready_and_complete_logs(self):
        service = self.script("service.py", (
            "import os, time\n"
            "os.write(1, b'x' * 100000 + b'RE')\n"
            "os.write(1, b'ADY')\n"
            "time.sleep(30)\n"
        ))
        bench = self.script("bench.py", "import sys\nprint('PASS')\nprint('ERR', file=sys.stderr)\n")
        self.case["steps"] = [
            {"start": [self.job("a", service), self.job("b", service, "second")]},
            {"wait_log": {"job": "a", "contains": "READY"}, "timeout": 2},
            {"wait_log": {"job": "b", "contains": "READY"}, "timeout": 2},
            {"run": [self.job("bench", bench)]},
        ]
        code, summary, output = await self.execute()
        self.assertEqual(code, 0, summary)
        self.assertEqual((output / "console/a.log").read_bytes(), b"x" * 100000 + b"READY")
        self.assertIn(b"ERR", (output / "console/bench.log").read_bytes())
        self.assertTrue(summary["success"])

    async def test_precheck_failure_blocks_later_steps(self):
        fail = self.script("fail.py", "print('failed')\nraise SystemExit(7)\n")
        later = self.script("later.py", "print('must not run')\n")
        slow = self.script("slow.py", "import time\ntime.sleep(30)\n")
        self.case["steps"] = [
            {"run": [self.job("pre", fail), self.job("slow", slow, "second")]},
            {"run": [self.job("later", later)]}]
        code, summary, output = await self.execute()
        self.assertEqual(code, 1)
        self.assertIn("pre failed: 7", summary["error"])
        self.assertIsNotNone(summary["jobs"]["slow"]["returncode"])
        self.assertFalse((output / "console/later.log").exists())

    async def test_service_zero_exit_is_failure_even_after_ready(self):
        service = self.script("service.py", "print('READY', flush=True)\n")
        bench = self.script("bench.py", "import time\ntime.sleep(30)\n")
        self.case["steps"] = [
            {"start": [self.job("service", service)]},
            {"wait_log": {"job": "service", "contains": "READY"}},
            {"run": [self.job("bench", bench)]},
        ]
        code, summary, _ = await self.execute()
        self.assertEqual(code, 1)
        self.assertIn("service service exited unexpectedly: 0", summary["error"])

    async def test_failure_window_keeps_collecting(self):
        service = self.script("service.py", (
            "import time\n"
            "print('READY', flush=True)\n"
            "while True:\n"
            " print('still collecting', flush=True)\n"
            " time.sleep(0.02)\n"
        ))
        fail = self.script("fail.py", "raise SystemExit(3)\n")
        self.case["failure_collect_seconds"] = 0.15
        self.case["steps"] = [
            {"start": [self.job("service", service)]},
            {"wait_log": {"job": "service", "contains": "READY"}},
            {"run": [self.job("bench", fail)]},
        ]
        code, summary, output = await self.execute()
        self.assertEqual(code, 1)
        self.assertIn("bench failed: 3", summary["error"])
        self.assertGreater((output / "console/service.log").read_text().count("still collecting"), 1)

    async def test_log_timeout_stops_service(self):
        service = self.script("service.py", "import time\ntime.sleep(30)\n")
        self.case["steps"] = [
            {"start": [self.job("service", service)]},
            {"wait_log": {"job": "service", "contains": "READY"}, "timeout": 0.1},
        ]
        code, summary, _ = await self.execute()
        self.assertEqual(code, 1)
        self.assertIn("timeout", summary["error"])
        self.assertIsNotNone(summary["jobs"]["service"]["returncode"])

    async def test_run_timeout_and_argument_quoting(self):
        service = self.script("service.py", (
            "import os, sys, time\n"
            "print(sys.argv[1], os.environ['TEST_VALUE'], flush=True)\n"
            "time.sleep(30)\n"
        ))
        job = self.job("job", service)
        job.update(args=["; touch should-not-exist"], env={"TEST_VALUE": "value with spaces"})
        self.case["steps"] = [{"run": [job], "timeout": 0.2}]
        code, summary, output = await self.execute()
        self.assertEqual(code, 1)
        self.assertIn("timeout", summary["error"])
        self.assertIn("; touch should-not-exist value with spaces", (output / "console/job.log").read_text())
        self.assertFalse((Path(self.runners[-1].root("first")) / "should-not-exist").exists())

    async def test_cancel_still_writes_summary_and_stops(self):
        script = self.script("service.py", (
            "import os, pathlib, signal, time\n"
            "def stop(*args):\n"
            " pathlib.Path(os.environ['CASE_LOG_DIR'], 'shutdown.log').write_text('stopped')\n"
            " raise SystemExit(0)\n"
            "signal.signal(signal.SIGTERM, stop)\n"
            "print('READY', flush=True)\n"
            "time.sleep(30)\n"
        ))
        self.case["steps"] = [{"run": [self.job("service", script)]}]
        runner = Runner(self.case, self.directory, self.base / "cancel")
        self.runners.append(runner)
        task = asyncio.create_task(runner.run())
        async def started():
            while (not runner.jobs or not runner.jobs["service"].output.exists()
                   or "READY" not in runner.jobs["service"].output.read_text()):
                await asyncio.sleep(0.01)
        await asyncio.wait_for(started(), 2)
        task.cancel()
        self.assertEqual(await task, 1)
        summary = json.loads((runner.output / "summary.json").read_text())
        self.assertEqual(summary["error"], "interrupted")
        self.assertFalse(summary["cleanup_errors"])
        self.assertEqual((runner.output / "artifacts/first/logs/service/shutdown.log").read_text(), "stopped")

    async def test_total_timeout(self):
        script = self.script("slow.py", "import time\ntime.sleep(30)\n")
        self.case["timeout"] = 0.2
        self.case["steps"] = [{"run": [self.job("slow", script)]}]
        code, summary, _ = await self.execute()
        self.assertEqual(code, 1)
        self.assertEqual(summary["error"], "TimeoutError")

    async def test_log_write_error_fails_case(self):
        script = self.script("slow.py", "import os\nwhile True:\n os.write(1, b'x' * 65536)\n")
        self.case["steps"] = [{"run": [self.job("slow", script)]}]

        async def failed_collector(_self):
            raise OSError("disk full")

        with mock.patch("runner.Job.collect_records", failed_collector):
            code, summary, _ = await self.execute()
        self.assertEqual(code, 1)
        self.assertIn("disk full", summary["error"])
        self.assertTrue(summary["cleanup_errors"])

    async def test_remote_heartbeat_loss_kills_foreground_process(self):
        pid_file = self.base / "pid"
        script = self.script("slow.py", (
            "import os, pathlib, sys, time\n"
            "pathlib.Path(sys.argv[1]).write_text(str(os.getpid()))\n"
            "time.sleep(30)\n"
        ))
        source = (
            "import remote_agent; remote_agent.HEARTBEAT_TIMEOUT = 0.2; "
            "raise SystemExit(remote_agent.main())"
        )
        process = await asyncio.create_subprocess_exec(
            sys.executable, "-c", source, cwd=str(Path(__file__).parent),
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        frame = {"command": [str(self.directory / script), str(pid_file)],
                 "env": {}, "cwd": str(self.directory)}
        process.stdin.write(json.dumps(frame).encode() + b"\n")
        await process.stdin.drain()
        try:
            await asyncio.wait_for(process.wait(), 3)
            self.assertEqual(process.returncode, 125)
            pid = int(pid_file.read_text())
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)
        finally:
            process.stdin.close()
            if process.returncode is None:
                process.kill()
                await process.wait()

    async def test_fake_ssh_docker_round_trip(self):
        binary = self.base / "bin"
        binary.mkdir()
        for filename, body in {
            "ssh": (
                "import os, sys\n"
                "os.environ['FAKE_NODE'] = sys.argv[-2]\n"
                "os.execvp('sh', ['sh', '-c', sys.argv[-1]])\n"
            ),
            "docker": (
                "import os, sys\n"
                "args = sys.argv[2:]\n"
                "if args[0] == '-i': args = args[1:]\n"
                "expected = {'example-node': 'example-container', 'other-node': 'other-container'}\n"
                "assert args[0] == expected[os.environ['FAKE_NODE']]\n"
                "args = args[1:]\n"
                "os.execvp(args[0], args)\n"
            ),
        }.items():
            path = binary / filename
            path.write_text(f"#!{sys.executable}\n" + body)
            path.chmod(0o700)
        self.case["nodes"] = {"node1": {"ssh": "example-node"}, "node2": {"ssh": "other-node"}}
        self.case["targets"] = {
            "first": {"node": "node1", "container": "example-container"},
            "second": {"node": "node2", "container": "other-container"},
        }
        script = self.script("hello.py", (
            "import os, pathlib\n"
            "pathlib.Path(os.environ['CASE_LOG_DIR'], 'remote.log').write_text(os.environ['FAKE_NODE'])\n"
            "pathlib.Path(os.environ['CASE_ARTIFACT_DIR'], 'result.txt').write_text('remote artifact')\n"
            "print('transport OK')\n"
        ))
        common = self.base / "common"
        common.mkdir()
        (self.directory / script).rename(common / script)
        self.case["shared_dirs"] = {"common": "../common"}
        self.case["steps"] = [{"run": [
            self.job("hello", "common/" + script),
            {"id": "hello2", "target": "second", "command": 'exec "$CASE_DIR/common/hello.py"'},
        ]}]
        with mock.patch.dict(os.environ, {"PATH": str(binary) + os.pathsep + os.environ["PATH"]}):
            code, summary, output = await self.execute()
        self.assertEqual(code, 0, summary)
        self.assertIn("transport OK", (output / "console/hello.log").read_text())
        self.assertEqual((output / "artifacts/first/logs/hello/remote.log").read_text(), "example-node")
        self.assertEqual((output / "artifacts/second/logs/hello2/remote.log").read_text(), "other-node")
        self.assertEqual((output / "artifacts/first/artifacts/hello/result.txt").read_text(), "remote artifact")

    async def test_file_readiness_and_isolated_result_directories(self):
        (self.directory / "asset.txt").write_text("asset")
        service = self.script("service.py", (
            "import os, pathlib, time\n"
            "assert pathlib.Path.cwd() == pathlib.Path(os.environ['CASE_WORK_DIR'])\n"
            "assert pathlib.Path(os.environ['CASE_DIR'], 'asset.txt').read_text() == 'asset'\n"
            "pathlib.Path(os.environ['CASE_ARTIFACT_DIR'], 'result.bin').write_bytes(b'\\x00\\xff')\n"
            "with open(pathlib.Path(os.environ['CASE_LOG_DIR'], 'server.log'), 'wb', buffering=0) as log:\n"
            " log.write(b'x' * 100000 + b'RE')\n"
            " log.write(b'ADY')\n"
            "time.sleep(30)\n"
        ))
        bench = self.script("bench.py", "print('bench ran')\n")
        self.case["steps"] = [
            {"start": [self.job("a", service), self.job("b", service, "second")]},
            {"wait_log": {"job": "a", "source": "file", "path": "server.log", "contains": "READY"}},
            {"wait_log": {"job": "b", "source": "file", "path": "server.log", "contains": "READY"}},
            {"run": [self.job("bench", bench)]},
        ]
        code, summary, output = await self.execute()
        self.assertEqual(code, 0, summary)
        for target, job in (("first", "a"), ("second", "b")):
            self.assertEqual((output / f"artifacts/{target}/logs/{job}/server.log").read_bytes(),
                             b"x" * 100000 + b"READY")
            self.assertEqual((output / f"artifacts/{target}/artifacts/{job}/result.bin").read_bytes(), b"\0\xff")
            self.assertEqual((output / f"console/{job}.log").read_bytes(), b"")
        self.assertIn("bench ran", (output / "console/bench.log").read_text())

    async def test_file_wait_timeout_still_returns_logs(self):
        service = self.script("service.py", (
            "import os, pathlib, time\n"
            "pathlib.Path(os.environ['CASE_LOG_DIR'], 'server.log').write_text('not ready')\n"
            "print('STARTED', flush=True)\n"
            "time.sleep(30)\n"
        ))
        self.case["steps"] = [
            {"start": [self.job("service", service)]},
            {"wait_log": {"job": "service", "contains": "STARTED"}},
            {"wait_log": {"job": "service", "source": "file", "path": "server.log", "contains": "READY"},
             "timeout": 0.1},
        ]
        code, summary, output = await self.execute()
        self.assertEqual(code, 1)
        self.assertIn("timeout", summary["error"])
        self.assertEqual((output / "artifacts/first/logs/service/server.log").read_text(), "not ready")

    async def test_file_readiness_does_not_mask_service_exit(self):
        service = self.script("service.py", (
            "import os, pathlib\n"
            "pathlib.Path(os.environ['CASE_LOG_DIR'], 'server.log').write_text('READY')\n"
        ))
        bench = self.script("bench.py", "import time\ntime.sleep(30)\n")
        self.case["steps"] = [
            {"start": [self.job("service", service)]},
            {"wait_log": {"job": "service", "source": "file", "path": "server.log", "contains": "READY"}},
            {"run": [self.job("bench", bench)]},
        ]
        code, summary, output = await self.execute()
        self.assertEqual(code, 1)
        self.assertIn("exited unexpectedly", summary["error"])
        self.assertEqual((output / "artifacts/first/logs/service/server.log").read_text(), "READY")

    async def test_collection_failure_preserves_test_error_and_other_targets(self):
        writer = self.script("writer.py", (
            "import os, pathlib\n"
            "pathlib.Path(os.environ['CASE_LOG_DIR'], 'result.log').write_text('collected')\n"
        ))
        fail = self.script("fail.py", "raise SystemExit(7)\n")
        self.case["steps"] = [
            {"run": [self.job("a", writer), self.job("b", writer, "second")]},
            {"run": [self.job("fail", fail)]},
        ]
        original = Runner.collect_target

        async def unavailable(runner, target):
            if target == "first":
                raise OSError("node unavailable")
            await original(runner, target)

        with mock.patch.object(Runner, "collect_target", unavailable):
            code, summary, output = await self.execute()
        self.assertEqual(code, 1)
        self.assertIn("fail failed: 7", summary["error"])
        self.assertIn("node unavailable", summary["collection_errors"][0])
        self.assertEqual(summary["collections"]["second"]["status"], "ok")
        self.assertEqual((output / "artifacts/second/logs/b/result.log").read_text(), "collected")

    async def test_collection_timeout_fails_successful_case(self):
        script = self.script("ok.py", "print('ok')\n")
        self.case["steps"] = [{"run": [self.job("ok", script)]}]
        self.case["collection_timeout"] = 0.1
        original = Runner.collect_target

        async def stalled(runner, target):
            if target == "first":
                await asyncio.Event().wait()
            await original(runner, target)

        with mock.patch.object(Runner, "collect_target", stalled):
            code, summary, _ = await self.execute()
        self.assertEqual(code, 1)
        self.assertIsNone(summary["error"])
        self.assertEqual(summary["collections"]["first"]["error"], "TimeoutError")
        self.assertEqual(summary["collections"]["second"]["status"], "ok")

    async def test_interrupt_during_collection_finishes_transfer_and_writes_summary(self):
        script = self.script("ok.py", "print('ok')\n")
        self.case["steps"] = [{"run": [self.job("ok", script)]}]
        runner = Runner(self.case, self.directory, self.base / "interrupt-collection")
        self.runners.append(runner)
        entered = asyncio.Event()
        release = asyncio.Event()
        original = Runner.collect_target

        async def paused(instance, target):
            if target == "first":
                entered.set()
                await release.wait()
            await original(instance, target)

        with mock.patch.object(Runner, "collect_target", paused):
            task = asyncio.create_task(runner.run())
            try:
                await asyncio.wait_for(entered.wait(), 2)
                task.cancel()
            finally:
                release.set()
                code = await task
        summary = json.loads((runner.output / "summary.json").read_text())
        self.assertEqual(code, 1)
        self.assertEqual(summary["error"], "interrupted")
        self.assertTrue(all(result["status"] == "ok" for result in summary["collections"].values()))

    async def test_unsafe_result_links_are_reported_without_blocking_other_targets(self):
        script = self.script("link.py", (
            "import os, pathlib\n"
            "pathlib.Path(os.environ['CASE_LOG_DIR'], 'link').symlink_to("
            "pathlib.Path(os.environ['CASE_DIR'], 'link.py'))\n"
        ))
        self.case["steps"] = [{"run": [self.job("link", script)]}]
        code, summary, output = await self.execute()
        self.assertEqual(code, 1)
        self.assertIn("unsafe result archive member", summary["collection_errors"][0])
        self.assertEqual(summary["collections"]["second"]["status"], "ok")
        self.assertFalse((output / "artifacts/first").exists())

    def test_validation_rejects_ambiguous_or_unsafe_config(self):
        script = self.script("ok.py", "print('ok')\n")
        self.case["steps"] = [{"run": [self.job("ok", script)]}]
        bad_cases = []
        bad = copy.deepcopy(self.case)
        bad["steps"].append({"wait_log": {"job": "ok", "contains": "READY"}})
        bad_cases.append(bad)
        bad = copy.deepcopy(self.case)
        bad["steps"][0]["run"][0]["script"] = "../outside.sh"
        bad_cases.append(bad)
        bad = copy.deepcopy(self.case)
        bad["steps"].append(bad["steps"][0])
        bad_cases.append(bad)
        bad = copy.deepcopy(self.case)
        bad["timeout"] = float("nan")
        bad_cases.append(bad)
        bad = copy.deepcopy(self.case)
        bad["steps"][0]["typo"] = []
        bad_cases.append(bad)
        bad = copy.deepcopy(self.case)
        del bad["nodes"]
        bad_cases.append(bad)
        bad = copy.deepcopy(self.case)
        bad["steps"][0]["run"][0]["env"] = {"CASE_LOG_DIR": "/ignored"}
        bad_cases.append(bad)
        for wait in (
            {"source": "file", "contains": "READY"},
            {"source": "file", "path": "../escape", "contains": "READY"},
            {"source": "console", "path": "server.log", "contains": "READY"},
            {"source": "unknown", "contains": "READY"},
        ):
            bad = copy.deepcopy(self.case)
            bad["steps"] = [{"start": [self.job("service", script)]},
                            {"wait_log": {"job": "service", **wait}}]
            bad_cases.append(bad)
        for config in bad_cases:
            with self.subTest(config=config), self.assertRaises(ValueError):
                validate(config, self.directory)
        (self.directory / "link").symlink_to(self.directory / script)
        with self.assertRaisesRegex(ValueError, "regular"):
            validate(self.case, self.directory)

    def test_example_configs(self):
        examples = Path(__file__).parent / "examples"
        for config in examples.glob("case*/case.json"):
            case = json.loads(config.read_text())
            for step in case["steps"]:
                for job in step.get("run", step.get("start", [])):
                    self.script(job["script"], "print('example placeholder')\n")
            validate(case, self.directory)

    async def test_cli_check_and_execute(self):
        self.case["steps"] = [{"run": [{"id": "hello", "target": "first", "command": "printf 'CLI OK\\n'"}]}]
        (self.directory / "case.json").write_text(json.dumps(self.case))
        output = self.base / "cli-output"
        argv = [sys.executable, str(Path(__file__).with_name("runner.py")),
                str(self.directory), "--output", str(output)]
        try:
            for extra in (["--check"], []):
                process = await asyncio.create_subprocess_exec(
                    *argv, *extra, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
                stdout, _ = await asyncio.wait_for(process.communicate(), 3)
                self.assertEqual(process.returncode, 0, stdout.decode())
                if extra:
                    self.assertFalse(output.exists())
            result = next(output.glob("case/*/summary.json")).parent
            self.assertIn("CLI OK", (result / "console/hello.log").read_text())
        finally:
            for result in output.glob("case/*/summary.json"):
                summary = json.loads(result.read_text())
                for path in summary["remote_directories"].values():
                    shutil.rmtree(Path(path).parent, ignore_errors=True)

    def test_transport_uses_strict_host_checking_and_shell_quoting(self):
        transport = Transport({"node": {"ssh": "example"}}, {
            "target": {"node": "node", "container": "container"}})
        argv = transport.command("target", ["echo", "a; b"], interactive=True)
        self.assertIn("StrictHostKeyChecking=yes", argv)
        self.assertEqual(shlex.split(argv[-1]), ["docker", "exec", "-i", "container", "echo", "a; b"])


if __name__ == "__main__":
    unittest.main()
