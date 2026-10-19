# BRPC checksum authentication feasibility probe

Standalone, manual, loopback-only experiment against the BRPC and Protobuf already compiled in an existing DataSystem
build cache. It does not change the product authentication path or join the default UT/ST build.

Configure with `cmake -S tests/manual/rpc_checksum_auth_probe -B build/rpc-checksum-auth-probe -DDS_MAIN_BUILD="$PWD/build"`,
then build that directory and run its CTest. Export a verified `DS_OPENSOURCE_DIR` for all commands. The CMake helper
reads dependency paths from the existing main `CMakeCache.txt`; its explicit cache layout is for this experiment only.

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
