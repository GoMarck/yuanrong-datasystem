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
#include <mutex>

#include "datasystem/common/rpc/brpc_client_stream_impl.h"
#include "datasystem/common/rpc/brpc_server_stream_impl.h"
#include "datasystem/protos/utils.pb.h"

namespace datasystem {
namespace test {
namespace {
class SingleReadService final : public google::protobuf::Service {
public:
    SingleReadService() : pool_(google::protobuf::DescriptorPool::generated_pool())
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
                    const google::protobuf::Message *, google::protobuf::Message *,
                    google::protobuf::Closure *done) override
    {
        brpc::ClosureGuard guard(done);
        auto *controller = static_cast<brpc::Controller *>(base);
        auto reader = std::make_shared<BrpcServerReaderImpl<HostPortPb>>(controller, "SingleRead.Echo");
        reader->Init();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reader_ = reader;
            if (cancelled_) {
                reader->on_idle_timeout(brpc::INVALID_STREAM_ID);
            }
        }
        HostPortPb request;
        auto status = reader->Read(request);
        if (status.IsOk()) {
            status = reader->WritePb(request);
        }
        (void)reader->SendStatus(status);
        reader->Close();
        std::lock_guard<std::mutex> lock(mutex_);
        reader_.reset();
    }

    void CancelPendingRead()
    {
        std::shared_ptr<BrpcServerReaderImpl<HostPortPb>> reader;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
            reader = reader_;
        }
        if (reader) {
            reader->on_idle_timeout(brpc::INVALID_STREAM_ID);
        }
    }
private:
    google::protobuf::DescriptorPool pool_;
    const google::protobuf::ServiceDescriptor *descriptor_ = nullptr;
    std::mutex mutex_;
    bool cancelled_ = false;
    std::shared_ptr<BrpcServerReaderImpl<HostPortPb>> reader_;
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
        service.CancelPendingRead();
        EXPECT_TRUE(writer.Finish().IsOk());
    }
    server.Stop(0);
    server.Join();
}
}  // namespace test
}  // namespace datasystem
