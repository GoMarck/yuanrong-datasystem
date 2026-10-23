/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <brpc/closure_guard.h>
#include <brpc/server.h>
#include <google/protobuf/descriptor.pb.h>
#include <atomic>

#include "datasystem/common/rpc/brpc_client_stream_impl.h"
#include "datasystem/common/rpc/brpc_server_stream_impl.h"
#include "datasystem/protos/utils.pb.h"

namespace datasystem {
namespace test {
namespace {
class SingleReadService final : public BrpcSingleStreamService {
public:
    explicit SingleReadService(bool readUntilEnd = false, bool fail = false, bool emptyReply = false)
        : readUntilEnd_(readUntilEnd), fail_(fail), emptyReply_(emptyReply), pool_(google::protobuf::DescriptorPool::generated_pool())
    {
        google::protobuf::FileDescriptorProto file;
        file.set_name("client_stream_baseline_test.proto");
        file.set_package("client_stream_baseline_test");
        file.set_syntax("proto3");
        file.add_dependency(HostPortPb::descriptor()->file()->name());
        auto *service = file.add_service();
        service->set_name("SingleRead");
        auto *method = service->add_method();
        method->set_name("Echo");
        method->set_input_type("." + HostPortPb::descriptor()->full_name());
        method->set_output_type("." + HostPortPb::descriptor()->full_name());
        method->set_client_streaming(true);
        const auto *descriptor = pool_.BuildFile(file);
        descriptor_ = descriptor == nullptr ? nullptr : descriptor->service(0);
    }

    const google::protobuf::ServiceDescriptor *GetDescriptor() override { return descriptor_; }
    const google::protobuf::Message &GetRequestPrototype(const google::protobuf::MethodDescriptor *) const override
    {
        return HostPortPb::default_instance();
    }
    const google::protobuf::Message &GetResponsePrototype(const google::protobuf::MethodDescriptor *) const override
    {
        return HostPortPb::default_instance();
    }
    void CallMethod(const google::protobuf::MethodDescriptor *, google::protobuf::RpcController *base,
                    const google::protobuf::Message *initial, google::protobuf::Message *,
                    google::protobuf::Closure *done) override
    {
        brpc::ClosureGuard guard(done);
        auto *controller = static_cast<brpc::Controller *>(base);
        const auto *request = static_cast<const HostPortPb *>(initial);
        auto reader = std::make_shared<BrpcServerReaderImpl<HostPortPb>>(controller, *request, "SingleRead.Echo");
        const auto init = reader->Init(*controller);
        if (init.IsError()) {
            controller->SetFailed(init.ToString());
            return;
        }
        CallScope scope(*this, reader);
        if (!scope.IsActive()) {
            reader->Close();
            controller->SetFailed("Service stopping");
            return;
        }
        guard.release()->Run();
        HostPortPb requestPb;
        HostPortPb response;
        auto status = reader->Read(requestPb);
        while (status.IsOk()) {
            response.set_host(requestPb.host());
            response.set_port(response.port() + 1);
            if (!readUntilEnd_) {
                break;
            }
            status = reader->Read(requestPb);
        }
        lastRead.store(status.GetCode());
        if (status.GetCode() == K_RPC_STREAM_END) {
            status = Status::OK();
        }
        if (fail_) {
            status = Status(K_NOT_AUTHORIZED, "test-only-denied");
        }
        if (emptyReply_) {
            response.Clear();
        }
        (void)reader->Complete(response, status);
    }

    std::atomic<StatusCode> lastRead{K_OK};
private:
    bool readUntilEnd_;
    bool fail_;
    bool emptyReply_;
    google::protobuf::DescriptorPool pool_;
    const google::protobuf::ServiceDescriptor *descriptor_ = nullptr;

};
}  // namespace

TEST(BrpcClientStreamTest, FirstWriteAndReadCompleteBeforeFinish)
{
    SingleReadService service;
    ASSERT_NE(service.GetDescriptor(), nullptr);
    brpc::Server server;
    ASSERT_EQ(server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
    ASSERT_EQ(server.Start("127.0.0.1:0", nullptr), 0);
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.timeout_ms = 1000;
    options.max_retry = 0;
    ASSERT_EQ(channel.Init(server.listen_address(), &options), 0);
    {
        BrpcClientWriterImpl<HostPortPb> writer(&channel, service.GetDescriptor()->method(0), 1000);
        HostPortPb request;
        request.set_host("first-stream-message");
        const auto write = writer.Write(request);
        EXPECT_TRUE(write.IsOk()) << write.ToString();
        if (write.IsOk()) {
            HostPortPb response;
            const auto read = writer.Read(response);
            EXPECT_TRUE(read.IsOk()) << read.ToString();
            EXPECT_EQ(response.host(), request.host());
        }
        EXPECT_TRUE(writer.Finish().IsOk());
    }
    server.Stop(0);
    service.StopAndWait();
    server.Join();
}
TEST(BrpcClientStreamTest, MultipleWritesAndEmptyFirstMessageEndAtRead)
{
    SingleReadService service(true);
    brpc::Server server;
    ASSERT_EQ(server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
    ASSERT_EQ(server.Start("127.0.0.1:0", nullptr), 0);
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.timeout_ms = 1000;
    options.max_retry = 0;
    ASSERT_EQ(channel.Init(server.listen_address(), &options), 0);
    {
        BrpcClientWriterImpl<HostPortPb> writer(&channel, service.GetDescriptor()->method(0), 1000);
        HostPortPb request;
        EXPECT_TRUE(writer.Write(request).IsOk());
        EXPECT_TRUE(writer.Write(request).IsOk());
        request.set_host("last-message");
        EXPECT_TRUE(writer.Write(request).IsOk());
        HostPortPb response;
        EXPECT_TRUE(writer.Read(response).IsOk());
        EXPECT_EQ(response.host(), request.host());
        EXPECT_EQ(response.port(), 3);
        EXPECT_TRUE(writer.Finish().IsOk());
        EXPECT_TRUE(writer.Finish().IsOk());
    }
    server.Stop(0);
    service.StopAndWait();
    server.Join();
}

TEST(BrpcClientStreamTest, FinalStatusAndFinishBeforeRead)
{
    for (bool fail : {false, true}) {
        SingleReadService service(true, fail, true);
        brpc::Server server;
        ASSERT_EQ(server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        ASSERT_EQ(server.Start("127.0.0.1:0", nullptr), 0);
        brpc::Channel channel;
        brpc::ChannelOptions options;
        options.timeout_ms = 1000;
        options.max_retry = 0;
        ASSERT_EQ(channel.Init(server.listen_address(), &options), 0);
        {
            BrpcClientWriterImpl<HostPortPb> writer(&channel, service.GetDescriptor()->method(0), 1000);
            HostPortPb request;
            EXPECT_TRUE(writer.Write(request).IsOk());
            EXPECT_EQ(writer.Finish().GetCode(), fail ? K_NOT_AUTHORIZED : K_OK);
            HostPortPb response;
            EXPECT_EQ(writer.Read(response).GetCode(), fail ? K_NOT_AUTHORIZED : K_OK);
            EXPECT_TRUE(response.host().empty());
        }
        server.Stop(0);
        service.StopAndWait();
        server.Join();
    }
}

TEST(BrpcClientStreamTest, StopDrainsAcceptedBusinessAndRejectsNewCalls)
{
    SingleReadService service(true);
    brpc::Server server;
    ASSERT_EQ(server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
    ASSERT_EQ(server.Start("127.0.0.1:0", nullptr), 0);
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.timeout_ms = 1000;
    options.max_retry = 0;
    ASSERT_EQ(channel.Init(server.listen_address(), &options), 0);
    {
        BrpcClientWriterImpl<HostPortPb> abandoned(&channel, service.GetDescriptor()->method(0), 1000);
        HostPortPb request;
        EXPECT_TRUE(abandoned.Write(request).IsOk());
    }
    service.Wait();
    EXPECT_EQ(service.lastRead.load(), K_RPC_CANCELLED);
    {
        BrpcClientWriterImpl<HostPortPb> writer(&channel, service.GetDescriptor()->method(0), 1000);
        HostPortPb request;
        EXPECT_TRUE(writer.Write(request).IsOk());
        service.StopAndWait();
        service.StopAndWait();
        BrpcClientWriterImpl<HostPortPb> rejected(&channel, service.GetDescriptor()->method(0), 1000);
        EXPECT_TRUE(rejected.Write(request).IsError());
        server.Stop(0);
        server.Join();
        HostPortPb response;
        EXPECT_TRUE(writer.Read(response).IsError());
    }
}
}  // namespace test
}  // namespace datasystem
