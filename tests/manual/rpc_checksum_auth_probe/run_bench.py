"""Sequential paired runs; source and scripts must reach validation hosts through Git."""
import argparse
import json
import os
from pathlib import Path
import random
import subprocess
import time


def snapshot():
    result = {"load": list(os.getloadavg())}
    for name in ("/proc/stat", "/proc/pressure/cpu", "/proc/pressure/memory"):
        path = Path(name)
        if path.exists():
            result[name] = path.read_text()
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpus", default="0-15")
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--pairs", type=int, default=5)
    args = parser.parse_args()
    if not os.environ.get("DS_OPENSOURCE_DIR"):
        parser.error("verified DS_OPENSOURCE_DIR is required")
    if args.output.exists():
        parser.error("output directory already exists; preserve prior results")
    args.output.mkdir(parents=True)
    binary = args.binary.resolve()
    env = os.environ.copy()
    env.pop("CHECKSUM_PROBE_AUTH_ENABLED", None)
    env["LD_LIBRARY_PATH"] = (binary.parent / "runtime-path.txt").read_text().strip()
    cases = [(64, 1, "sync"), (1024, 8, "async"), (1048576, 1, "sync")]
    schedule = []
    rng = random.Random(20261019)
    for case in cases:
        first = rng.choice(["baseline", "disabled"])
        for pair in range(args.pairs):
            order = [first, "disabled" if first == "baseline" else "baseline"]
            if pair % 2:
                order.reverse()
            for mode in order:
                schedule.append((*case, pair + 1, mode))
    metadata = {"commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                "arguments": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                "schedule": schedule, "start": snapshot()}
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2))
    results = []
    for index, (size, clients, style, pair, mode) in enumerate(schedule, 1):
        command = ["taskset", "-c", args.cpus, str(binary), "--bench", mode, str(size), str(clients),
                   style, str(args.seconds), str(args.warmup)]
        log = args.output / f"{size}-{clients}-{style}-pair{pair}-{mode}.log"
        before = snapshot()
        started = time.time()
        with log.open("w") as output:
            completed = subprocess.run(command, env=env, stdout=output, stderr=subprocess.STDOUT,
                                       timeout=args.seconds + args.warmup + 30)
        lines = [line[6:] for line in log.read_text().splitlines() if line.startswith("BENCH ")]
        if completed.returncode != 0 or len(lines) != 1:
            raise RuntimeError(f"benchmark failed: exit={completed.returncode}, log={log}")
        row = json.loads(lines[0])
        row.update(pair=pair, sequence=index, log=log.name, started=started, command=command,
                   environment_before=before, environment_after=snapshot())
        results.append(row)
        (args.output / "results.json").write_text(json.dumps(results, indent=2))
        status = {"completed": index, "total": len(schedule), "last": {
            k: row[k] for k in ("bytes", "clients", "style", "pair", "mode", "qps", "p50_us", "p99_us",
                               "cpu_us_per_op", "errors")}}
        (args.output / "status.json").write_text(json.dumps(status))
        print(json.dumps(status), flush=True)
    metadata["finish"] = snapshot()
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
