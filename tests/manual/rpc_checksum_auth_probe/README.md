# BRPC checksum authentication feasibility probe

Standalone, manual, loopback-only experiment against the BRPC and Protobuf already compiled in an existing DataSystem
build cache. It does not change the product authentication path or join the default UT/ST build.

Configure with `cmake -S tests/manual/rpc_checksum_auth_probe -B build/rpc-checksum-auth-probe -DDS_MAIN_BUILD="$PWD/build"`,
then build that directory and run its CTest. Export a verified `DS_OPENSOURCE_DIR` for all commands. The CMake helper
reads dependency paths from the existing main `CMakeCache.txt`; its explicit cache layout is for this experiment only.
Do not export the probe's cached OpenSSL library path to CMake/CTest themselves: system LDAP may require symbols absent
from that OpenSSL build. CTest sets the probe child environment separately; direct execution needs the library path
from the generated `runtime-path.txt` only for that child process.

The sender counting wrapper delegates directly to the generated request serializer; it is test instrumentation, not a
proposed product request type. The server uses the generated V1 request. The extra server reencoding is a test oracle
that reproduces the old signing bug, not part of the proposed authentication path.

The checksum handler hashes segmented IOBuf blocks, places only small credentials in native RpcMeta user fields, and
captures the received digest in Controller-local state before business PB parsing. The auth service rejects missing
local capture, unsupported checksum types, and invalid HMACs. Test checksum IDs and keys are private to the probe.
`test_skip_capture` and `verified` are deliberate adversarial test inputs, never production fields.

The bad-body case changes the digest used for signing, not network bytes; it tests rejection when body and signature
disagree. This is functional evidence, not a latency/throughput benchmark, actual historical-binary rolling upgrade,
product Worker ST, stream-frame validation, IAM integration, retry/cancellation validation, or a multi-Protobuf matrix.

## Disabled-path performance experiment

`run_bench.py --binary build/rpc-checksum-auth-probe/rpc_checksum_auth_probe --output <new-log-directory>` runs five
paired rounds for 64-byte synchronous single-client, 1-KiB asynchronous eight-client, and 1-MiB synchronous single-client
requests. Each fresh process warms up for two seconds and measures for thirty seconds; A/B order alternates. It uses
one shared TCP connection and empty responses over loopback. `DS_OPENSOURCE_DIR` must remain exported. The default
affinity mask is CPUs 0-15; select a valid mask with `--cpus`.

Baseline registers only the plain service and sends directly. Disabled mode registers the actual probe checksum
handler, the auth service, and a lazily used local-state factory, then sends the same business request after one runtime
mode branch. The compiler must retain that branch: inspect the noinline `OffPathStub::WithModeChoice` assembly. No
counting-request wrapper, server reencoding oracle, credential scope, or HMAC runs in the measured plain request path.
Both modes use the same executable, method, response, Controller setup, and measurement instrumentation. Consequently
this experiment does not compare binary code-size/layout changes from a future full product build.

Each result includes total process CPU per operation (client plus in-process server), QPS, all-request p50/p99, errors,
and zero-auth-work counters. Warmup, startup, and percentile sorting are outside the measured CPU interval. Every caller
keeps one outstanding RPC; async callers use an actual BRPC async done plus Join. Per-call monotonic clock reads and
latency recording are common benchmark overhead. RSS is whole-process peak, not per-request allocation profiling.

The driver preserves per-arm logs, commands, host pressure snapshots, schedule, exact Git commit, and progress JSON.
It fails on any RPC/auth-counter error and will not overwrite an existing output directory. This measures the proposed
mechanism in isolation, not a product SDK/Worker performance regression test. It excludes real token/tenant policies,
stream frames, multi-node transport, and a signed/background workload interfering with unsigned traffic.
