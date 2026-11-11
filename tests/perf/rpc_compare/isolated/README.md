# TCP RPC isolation benchmark

This standalone diagnostic does not initialize DataSystem workers, Mooncake master/store, SHM or TE. The handler only copies a prebuilt response body; handler time is returned and reported separately. The measured native interval includes serialization, scheduling and TCP. It is not CPU-only framework time, and must not be blindly subtracted from live Set/Get at different load.

The two backends use byte-for-byte identical `bench_common.h`. CLI workload, warmup, pacing, sample bounds, failure handling and quantiles are shared. Message sizes specify logical body bytes; protobuf and struct_pack wire headers differ. Mean/P99 retain failed requests; any connection failure, sample overflow or failed call makes the command return nonzero.

Build the brpc target with the existing dependency prefixes, not a system protobuf:

```sh
cmake -S tests/perf/rpc_compare/isolated -B build-rpc-isolation \
  -DCMAKE_BUILD_TYPE=Release -DBRPC_ROOT=<cached-brpc> \
  -DCMAKE_PREFIX_PATH='<cached-protobuf>;<cached-absl>;<cached-gflags>'
cmake --build build-rpc-isolation --target brpc_isolation_bench
```

The corresponding Mooncake target is standalone at `mooncake-store/benchmarks/rpc_isolation`; configure it with `-DYLT_INCLUDE_DIR=<existing-yalantinglibs>/include`. Use the same compiler/optimization flags, server threads, client IO threads and workload settings for both harnesses. Record the compiler and linked library versions; linking an existing library does not change how it was compiled.

```sh
brpc_isolation_bench --mode server --host <server-ip> --port <port> --response-bytes 256 --server-threads 4
brpc_isolation_bench --mode client --host <server-ip> --port <port> \
  --request-bytes 128 --response-bytes 256 --threads 4 --io-threads 4 \
  --warmup-seconds 10 --measure-seconds 60 --rate-per-thread 51
```

Use `coro_rpc_isolation_bench` with the same options for the other backend. Set rate to zero only for the explicitly labeled closed-loop saturation control. Each thread owns one connection/client. For brpc, `--io-threads` sets bthread worker concurrency before client creation; for coro_rpc it sets the Asio IO-pool size. These are different native execution models, so record actual process thread counts as well. Server-thread settings configure native server execution. Report native-call latency and full call-envelope latency separately; the latter includes per-call object setup, validation and destruction. Results are emitted as one `[RPC_ISOLATION_RESULT]` JSON line with exact nearest-rank P99.

Production instrumentation uses `ds.rpc_native.*` for the literal `Channel::CallMethod` interval and `mc.rpc_native.*` for library client-pool dispatch/send/response wait, excluding Mooncake metric updates. These intervals still contain server processing and data where the production RPC carries it. Sampled `ds.detail.rpc.stub.*`, server Read/Write, attachment encode/decode, deferred completion and `mc.detail.rpc.server.*` separate product adapters, protobuf copying, metrics and business work. `rpc_method` identifies Create=1, Publish=2, QueryAndGet=3, Release=4, CreateMeta=5, PutStart=6, PutEnd=7, GetReplicaList=8. DataSystem byte attributes report protobuf body size and response attachment bytes before trailer removal, not TCP wire size.
