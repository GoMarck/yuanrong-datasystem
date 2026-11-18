#include <brpc/channel.h>
#include <butil/config.h>
#include <butil/logging.h>
#include <gflags/gflags.h>
#include <google/protobuf/stubs/common.h>
#include <grpcpp/grpcpp.h>
#include <openssl/crypto.h>

#include <iostream>
#include <string>

#if DS_CLIENT_BRPC_GLOG != BRPC_WITH_GLOG
#error "Host brpc logging contract disagrees with its installed headers"
#endif

namespace brpc {
DECLARE_int32(max_connection_pool_size);
}

int main()
{
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    if (grpc::Version() != DS_CLIENT_GRPC_VERSION) {
        std::cerr << "gRPC runtime does not match the selected generator/runtime version\n";
        return 1;
    }
    if (OpenSSL_version_num() != OPENSSL_VERSION_NUMBER) {
        std::cerr << "OpenSSL runtime does not match selected headers\n";
        return 1;
    }
    brpc::Channel channel;
    (void)channel;
    if (brpc::FLAGS_max_connection_pool_size < 0) {
        return 1;
    }
#if !BRPC_WITH_GLOG
    (void)logging::FLAGS_brpc_v;
    const char *const loggingFlags[]{"brpc_v", "brpc_vmodule", "brpc_minloglevel"};
    for (const auto *flag : loggingFlags) {
        gflags::CommandLineFlagInfo info;
        if (!gflags::GetCommandLineFlagInfo(flag, &info)) {
            std::cerr << "Missing renamed brpc flag: " << flag << '\n';
            return 1;
        }
    }
#endif
    const char *const rpcFlags[]{"baidu_std_protocol_deliver_timeout_ms", "max_body_size",
                                "circuit_breaker_max_isolation_duration_ms", "event_dispatcher_num"};
    for (const auto *flag : rpcFlags) {
        gflags::CommandLineFlagInfo info;
        if (!gflags::GetCommandLineFlagInfo(flag, &info)) {
            std::cerr << "Missing required brpc flag: " << flag << '\n';
            return 1;
        }
    }
    return 0;
}
