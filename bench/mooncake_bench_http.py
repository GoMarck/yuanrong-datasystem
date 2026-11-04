# SPDX-License-Identifier: Apache-2.0
"""Single-run HTTP control plane for the two-node Mooncake benchmark.

Only parameters/status/results cross HTTP. All data-plane calls use Store.
"""

import copy
import hmac
import json
import os
import signal
import socket
import sys
import threading
import time
import uuid
from contextlib import ExitStack
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

import mooncake_multi_buffer_bench as bench


TERMINAL = {"succeeded", "failed"}
WORKLOAD_FIELDS = (
    "key_prefix", "num_keys", "key_bytes", "blocks_per_key",
    "put_keys_per_batch", "get_keys_per_batch", "warmup", "seed", "verify_data",
)


class ControlError(Exception):
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


class Session:
    def __init__(self, store, args):
        self.store, self.args = store, args
        self.lock = threading.RLock()
        self.runs = {}
        self.active = None
        self.draining = False
        self.worker = None
        self.segment = store.get_hostname()
        self.instance_id = uuid.uuid4().hex

    def status(self):
        with self.lock:
            return dict(state="draining" if self.draining else
                        "busy" if self.active else "storage_ready",
                        instance_id=self.instance_id, segment=self.segment,
                        master=self.args.master, protocol=self.args.protocol,
                        metadata_server=self.args.metadata_server,
                        concurrency=self.args.concurrency,
                        segment_bytes=self.args.segment_bytes, active_run=self.active)

    def validate(self, spec):
        if not isinstance(spec, dict) or set(spec) != set(WORKLOAD_FIELDS) | {
                "run_id", "a_segment", "master", "protocol", "metadata_server", "scenario"}:
            raise ControlError("Unexpected or missing run fields")
        for field in ("run_id", "key_prefix", "a_segment"):
            if not isinstance(spec[field], str) or not spec[field].strip() or len(spec[field]) > 200:
                raise ControlError(f"Invalid {field}")
        if any(c not in "0123456789abcdef" for c in spec["run_id"]) or len(spec["run_id"]) != 32:
            raise ControlError("run_id must be a UUID hex string")
        for field in ("num_keys", "key_bytes", "blocks_per_key", "warmup",
                      "put_keys_per_batch", "get_keys_per_batch"):
            if type(spec[field]) is not int or spec[field] <= 0:
                raise ControlError(f"{field} must be a positive integer")
        if type(spec["verify_data"]) is not bool:
            raise ControlError("verify_data must be a boolean")
        if type(spec["seed"]) is not int:
            raise ControlError("seed must be an integer")
        if (spec["key_bytes"] > 2**31 - 1 or
                spec["key_bytes"] % spec["blocks_per_key"]):
            raise ControlError("Invalid key/block layout")
        if (spec["num_keys"] % spec["put_keys_per_batch"] or
                spec["num_keys"] % spec["get_keys_per_batch"]):
            raise ControlError("num_keys must be divisible by put_keys_per_batch and get_keys_per_batch")
        if spec["num_keys"] // spec["get_keys_per_batch"] < max(self.args.concurrency):
            raise ControlError("Dataset must contain at least one batch per B reader worker")
        if spec["scenario"] not in ("local", "random"):
            raise ControlError("scenario must be local or random")
        for field in ("master", "protocol", "metadata_server"):
            if spec[field] != getattr(self.args, field):
                raise ControlError(f"Writer/server {field} mismatch")
        if spec["a_segment"] == self.segment:
            raise ControlError("HTTP mode requires distinct A/B segments")

    def create(self, spec):
        self.validate(spec)
        with self.lock:
            run_id = spec["run_id"]
            if run_id in self.runs:
                if self.runs[run_id]["spec"] != spec:
                    raise ControlError("run_id already used with different parameters", 409)
                return copy.deepcopy(self.runs[run_id])
            if self.active or self.draining:
                raise ControlError("Server busy or stopping", 409)
            if len(self.runs) >= 64:
                raise ControlError("64-run history limit reached; restart idle server", 409)
            self.runs[run_id] = dict(run_id=run_id, state="reserved", spec=copy.deepcopy(spec),
                                     results=[], error=None)
            self.active = run_id
            return copy.deepcopy(self.runs[run_id])

    def get(self, run_id):
        with self.lock:
            if run_id not in self.runs:
                raise ControlError("Unknown run (server may have restarted)", 404)
            return copy.deepcopy(self.runs[run_id])

    def start(self, run_id):
        with self.lock:
            if run_id not in self.runs:
                raise ControlError("Unknown run", 404)
            run = self.runs[run_id]
            if run["state"] == "reserved":
                run["state"] = "reading"
                self.worker = threading.Thread(target=self.read, args=(run_id,), daemon=False)
                try:
                    self.worker.start()
                except Exception as exc:
                    run["state"], run["error"] = "failed", repr(exc)
            # Duplicate requests never launch another read.
            return copy.deepcopy(run)

    def complete_cleanup(self, run_id):
        with self.lock:
            if run_id not in self.runs:
                raise ControlError("Unknown run", 404)
            run = self.runs[run_id]
            if run["state"] == "reading":
                raise ControlError("Reader is still running", 409)
            if run["state"] != "cleaned":
                run["read_state"] = run["state"]
                run["state"] = "cleaned"
                self.active = None
                if self.args.exit_after_get:
                    self.draining = True
            return copy.deepcopy(run)

    def read(self, run_id):
        run = self.get(run_id)
        spec = run["spec"]
        args = copy.copy(self.args)
        for field in WORKLOAD_FIELDS:
            setattr(args, field, spec[field])
        args.keys_per_batch = spec["get_keys_per_batch"]
        results = []
        try:
            self.store.set_device()
            for concurrency in args.concurrency:
                keysets = bench.prefix_keysets(args, concurrency, True)
                placement = bench.inspect_placement(
                    self.store, keysets, spec["a_segment"], spec["scenario"] == "local")
                with ExitStack() as stack:
                    buffers = []
                    for _ in range(concurrency):
                        buffer = bench.Buffers(self.store, args, 0)
                        stack.callback(buffer.close)
                        buffers.append((buffer, buffer))
                    warm_keys = [[batches[i % len(batches)] for i in range(args.warmup)]
                                 for batches in keysets]
                    bench.phase(self.store, args, None, buffers, warm_keys, "get")
                    stats = bench.phase(self.store, args, None, buffers, keysets, "get")
                    if not args.verify_data and any(not target.has_fill(1) for _, target in buffers):
                        raise RuntimeError("Last read batch content mismatch")
                local_keys = placement.get(self.segment, 0)
                row = dict(operation="get", scenario=spec["scenario"], concurrency=concurrency,
                           placement=placement, local_keys=local_keys,
                           remote_keys=args.num_keys - local_keys, **stats)
                results.append(row)
                bench.print_rows([row])
            state, error = "succeeded", None
        except BaseException as exc:
            state, error = "failed", repr(exc)
        result = dict(run, state=state, error=error, results=results)
        output = Path(self.args.output)
        output = output.with_name(f"{output.stem}.{run_id}{output.suffix or '.json'}")
        try:
            output.write_text(json.dumps(dict(server=self.status(), run=result), indent=2) + "\n")
        except Exception as exc:
            state, error = "failed", f"{error or ''} Result persistence failed: {exc!r}"
        # Terminal state is published only after synchronous reads and buffer cleanup end.
        with self.lock:
            self.runs[run_id].update(state=state, error=error, results=results)


def make_handler(session, token):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def setup(self):
            super().setup()
            self.connection.settimeout(10)

        def reply(self, status, data):
            body = json.dumps(data).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def dispatch(self):
            try:
                if token and not hmac.compare_digest(
                        self.headers.get("Authorization", ""), "Bearer " + token):
                    raise ControlError("Unauthorized", 401)
                parts = self.path.strip("/").split("/")
                if self.command == "GET":
                    if parts == ["status"]:
                        return self.reply(200, session.status())
                    if len(parts) == 2 and parts[0] == "runs":
                        return self.reply(200, session.get(parts[1]))
                if self.command == "POST":
                    try:
                        length = int(self.headers.get("Content-Length", "0"))
                    except ValueError:
                        raise ControlError("Invalid Content-Length")
                    if not 0 < length <= 65536:
                        raise ControlError("Request body must be 1..65536 bytes", 413)
                    try:
                        body = json.loads(self.rfile.read(length))
                    except (ValueError, UnicodeError):
                        raise ControlError("Invalid JSON")
                    if parts == ["runs"]:
                        return self.reply(200, session.create(body))
                    if len(parts) == 3 and parts[0] == "runs":
                        if body != {}:
                            raise ControlError("Action body must be {}")
                        if parts[2] == "start-get":
                            return self.reply(202, session.start(parts[1]))
                        if parts[2] == "cleanup-complete":
                            return self.reply(200, session.complete_cleanup(parts[1]))
                raise ControlError("Unknown endpoint", 404)
            except ControlError as exc:
                self.reply(exc.status, {"error": str(exc)})
            except (BrokenPipeError, ConnectionResetError, TimeoutError):
                pass
            except Exception as exc:
                self.reply(500, {"error": repr(exc)})

        do_GET = dispatch
        do_POST = dispatch
    return Handler


def serve(store, args, report):
    session = Session(store, args)
    class ControlServer(ThreadingHTTPServer):
        # Wait for cleanup-complete's response (and other active responses) to
        # finish before closing Store. ThreadingHTTPServer defaults to daemon
        # handlers, which could otherwise be cut off during automatic exit.
        daemon_threads = False
        block_on_close = True

    server_type = ControlServer
    if ":" in args.local_hostname:
        class IPv6Server(ControlServer):
            address_family = socket.AF_INET6
        server_type = IPv6Server
    token = os.getenv("MC_BENCH_HTTP_TOKEN", "")
    server = server_type((args.local_hostname, args.http_listen_port), make_handler(session, token))
    server.timeout = 0.5
    previous = {}
    def stop(*_):
        with session.lock:
            session.draining = True
        print("HTTP server draining: waiting for active run cleanup before exit.", flush=True)
    try:
        for sig in (signal.SIGINT, signal.SIGTERM):
            previous[sig] = signal.signal(sig, stop)
        print(f"STORAGE_READY: HTTP {server.server_address}; segment={session.segment}", flush=True)
        if not token:
            print("HTTP authentication disabled; use only on a trusted private test network.",
                  file=sys.stderr)
        while True:
            server.handle_request()
            with session.lock:
                if session.draining and session.active is None:
                    break
        if args.exit_after_get:
            print("GET server exiting after run cleanup.", flush=True)
    finally:
        server.server_close()
        if session.worker is not None:
            session.worker.join()
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        with session.lock:
            report["http_runs"] = copy.deepcopy(session.runs)


class Client:
    def __init__(self, args):
        url = urlsplit(args.http_server)
        if (url.scheme != "http" or not url.hostname or url.username or url.password or
                url.path not in ("", "/") or url.query or url.fragment):
            raise ValueError("--http-server must be http://host:port without path or credentials")
        self.base = args.http_server.rstrip("/")
        self.timeout = args.http_timeout
        self.token = os.getenv("MC_BENCH_HTTP_TOKEN", "")

    def request(self, path, body=None):
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        request = Request(self.base + path, data=None if body is None else json.dumps(body).encode(),
                          headers=headers, method="GET" if body is None else "POST")
        for attempt in range(3):
            try:
                with urlopen(request, timeout=self.timeout) as response:
                    return json.load(response)
            except HTTPError as exc:
                raise ControlError(f"HTTP {exc.code}: {exc.read(4096).decode(errors='replace')}",
                                   exc.code) from exc
            except (URLError, TimeoutError, ConnectionError):
                if attempt == 2:
                    raise
                time.sleep(1)

    def wait_terminal(self, run_id):
        warned = False
        while True:
            try:
                result = self.request(f"/runs/{run_id}")
                if result["state"] in TERMINAL:
                    return result
            except (URLError, TimeoutError, ConnectionError, ControlError) as exc:
                if not warned:
                    print(f"Cannot confirm reader completion: {exc}. Keeping A data alive; "
                          "will keep querying. Do not stop A while B may be reading.",
                          file=sys.stderr)
                    warned = True
            time.sleep(1)


def coordinate(store, config_type, args, report):
    client = Client(args)
    status = client.request("/status")
    if status["state"] != "storage_ready":
        raise RuntimeError(f"B is not ready: {status['state']}")
    a_segment, b_segment = store.get_hostname(), status["segment"]
    warm = copy.copy(args)
    warm.key_prefix = "mcbench-warm-" + uuid.uuid4().hex
    warm.num_keys = args.concurrency[0] * args.warmup * args.keys_per_batch
    spec = {name: getattr(args, name) for name in WORKLOAD_FIELDS}
    spec.update(run_id=uuid.uuid4().hex, a_segment=a_segment, master=args.master,
                protocol=args.protocol, metadata_server=args.metadata_server,
                scenario=args.scenarios[0])
    report["http_run_id"] = spec["run_id"]
    report["peer"] = status
    client.request("/runs", spec)
    run_path = f"/runs/{spec['run_id']}"
    owned = []
    started = False
    reader_done = False
    try:
        report["http_state"] = "reserved"
        Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
        concurrency = args.concurrency[0]
        keys = bench.prefix_keysets(args, concurrency)
        bench.require_absent(store, keys)
        config = config_type()
        config.replica_num = 1
        config.with_hard_pin = args.hard_pin
        config.prefer_alloc_in_same_node = args.prefer_alloc_in_same_node
        if args.scenarios[0] == "local":
            config.preferred_segment = a_segment
        with ExitStack() as stack:
            buffers = []
            for _ in range(concurrency):
                buffer = bench.Buffers(store, args, 1)
                stack.callback(buffer.close)
                buffers.append((buffer, buffer))
            warm_keys = bench.prefix_keysets(warm, concurrency)
            warm_owned = []
            try:
                bench.require_absent(store, warm_keys)
                bench.phase(store, warm, config, buffers, warm_keys, "put", warm_owned)
                bench.inspect_placement(store, warm_keys, a_segment, args.scenarios[0] == "local")
            finally:
                bench.remove_owned(store, warm, warm_owned)
            stats = bench.phase(store, args, config, buffers, keys, "put", owned)
            placement = bench.inspect_placement(store, keys, a_segment, args.scenarios[0] == "local")
        row = dict(operation="put", scenario=args.scenarios[0], concurrency=concurrency,
                   placement=placement, **stats)
        report["results"].append(row)
        report["http_state"] = "data_ready"
        Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
        bench.print_rows([row])
        print("DATA_READY: all keys written and placement inspected.", flush=True)
        # From this point an ambiguous HTTP error is NOT permission to delete.
        started = True
        try:
            client.request(run_path + "/start-get", {})
        except (URLError, TimeoutError, ConnectionError):
            # Idempotent action may have been accepted despite response loss.
            print("start-get response lost; reconciling with server.", file=sys.stderr)
            while True:
                try:
                    client.request(run_path + "/start-get", {})
                    break
                except (URLError, TimeoutError, ConnectionError):
                    time.sleep(1)
        result = client.wait_terminal(spec["run_id"])
        reader_done = True
        report["reader"] = result
        report["results"].extend(result["results"])
        report["http_state"] = result["state"]
        Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
        bench.print_rows(result["results"])
        if result["state"] == "failed":
            raise RuntimeError(f"B reader failed: {result['error']}")
    finally:
        primary_error = sys.exc_info()[0] is not None
        if started and not reader_done:
            # Even an interrupt/error must not free A's segment under B.
            client.wait_terminal(spec["run_id"])
        try:
            bench.remove_owned(store, args, owned)
            report["cleanup_status"] = "succeeded"
            client.request(run_path + "/cleanup-complete", {})
            print("CLEANUP_COMPLETE: B can accept the next run.", flush=True)
        except Exception as exc:
            report["cleanup_status"] = "failed_or_unconfirmed"
            report["cleanup_error"] = repr(exc)
            if primary_error:
                print(f"Cleanup failed: {exc!r}", file=sys.stderr)
            else:
                raise
