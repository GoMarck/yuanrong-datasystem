// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "bench_common.h"
#include "rpc_isolation.pb.h"
#include <brpc/channel.h>
#include <brpc/controller.h>
#include <brpc/server.h>

namespace rpc_isolation {
class EchoServiceImpl final : public EchoService {
public:
    explicit EchoServiceImpl(size_t size) : response_(size, 'r') {}
    void Echo(google::protobuf::RpcController *, const Request *, Reply *response,
              google::protobuf::Closure *done) override
    {
        brpc::ClosureGuard guard(done);
        const auto begin = NowNs();
        response->set_payload(response_);
        response->set_handler_ns(NowNs() - begin);
    }
private:
    const std::string response_;
};

class Client {
public:
    explicit Client(const Config &c) : expectedSize_(c.responseBytes)
    {
        brpc::ChannelOptions opts;
        opts.timeout_ms = c.timeoutMs;
        opts.max_retry = 0;
        opts.connection_type = "single";
        if (channel_.Init((c.host + ":" + std::to_string(c.port)).c_str(), &opts) != 0)
            throw std::runtime_error("brpc channel initialization failed");
        request_.set_payload(std::string(c.requestBytes, 'q'));
    }
    CallResult Call()
    {
        brpc::Controller controller;
        Reply reply;
        const auto begin = NowNs();
        channel_.CallMethod(EchoService::descriptor()->method(0), &controller, &request_, &reply, nullptr);
        const auto elapsed = NowNs() - begin;
        const bool valid = !controller.Failed() && reply.IsInitialized()
                           && reply.payload().size() == expectedSize_
                           && (expectedSize_ == 0 || reply.payload().front() == 'r');
        return {valid, elapsed, valid ? reply.handler_ns() : 0};
    }
private:
    brpc::Channel channel_;
    Request request_;
    size_t expectedSize_;
};
}  // namespace rpc_isolation

int main(int argc, char **argv)
{
    using namespace rpc_isolation;
    try {
        const auto config = Parse(argc, argv);
        if (config.mode == "server") {
            EchoServiceImpl echo(config.responseBytes);
            brpc::Server server;
            if (server.AddService(&echo, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) return 1;
            brpc::ServerOptions opts;
            opts.num_threads = config.serverThreads;
            if (server.Start((config.host + ":" + std::to_string(config.port)).c_str(), &opts) != 0) return 1;
            std::cout << "RPC_ISOLATION_READY" << std::endl;
            server.RunUntilAskedToQuit();
            return 0;
        }
        return Run(config, "brpc", [&](int) { return std::make_unique<Client>(config); });
    } catch (const std::exception &e) { std::cerr << e.what() << std::endl; return 1; }
}
