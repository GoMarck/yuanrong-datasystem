# URMA mock

The default backend uses local UDS and memory-file mappings. To exercise the existing URMA manager and completion flow
between physical hosts, build with `BUILD_WITH_URMA_MOCK=ON` and set a different
`URMA_MOCK_TCP_ENDPOINT=<private-IPv4>:<unused-port>` in every worker and SDK process. The option also works with
`WITH_TESTS=OFF`, which avoids enabling injection hooks in performance measurements.

The mock encodes that endpoint in its segment EID; existing segment exchange carries it to peers. A post performs a
bounded TCP read/write and pushes the existing send completion. The service accepts only currently registered local
segment tokens and address ranges. Each transfer is limited to 16 MiB, with one-second socket/connect timeouts.
Use independent processes; forking an already started TCP listener is unsupported. Unset the variable to retain UDS.
There is no connection pool or transport performance guarantee. `ds.mock_urma.post` measures the fake post interface;
it is nested within URMA submission and must not be counted again as business logic. Completion waiting is separate.

Build `urma_mock_tcp_probe` to expose a 4096-byte registered region for read/write, range, token, and disconnect checks.
Run `python3 transport/tcp_probe_test.py <server-IPv4>:<port> '<JSON-line-from-probe>'` on the peer host.
It prints the region metadata as JSON and exits after 45 seconds. Run real Set/Get integration with data verification
before using this backend for workflow attribution; require no payload fallback and keep SHM/non-SHM results separate.
