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

#include <atomic>
#include <future>

#include <brpc/closure_guard.h>
#include <brpc/nonreflectable_message.h>
#include <brpc/server.h>
#include <google/protobuf/descriptor.pb.h>
#include <gtest/gtest.h>

#include "datasystem/common/ak_sk/ak_sk_manager.h"
#include "datasystem/common/util/timer.h"
#include "datasystem/common/rpc/brpc_auth.h"
#include "datasystem/common/rpc/brpc_client_stream_impl.h"
#include "datasystem/common/rpc/brpc_client_stream_writer_reader.h"
#include "datasystem/protos/master_stream.auth.pb.h"
#include "datasystem/protos/master_stream.pb.h"
#include "datasystem/protos/ut_object.auth.pb.h"
#include "datasystem/protos/ut_object.brpc.auth.pb.h"
#include "datasystem/protos/ut_object.pb.h"

namespace datasystem {
namespace test {
namespace {
class CountedRequest final : public brpc::NonreflectableMessage<CountedRequest> {
public:
    CountedRequest() = default;
    explicit CountedRequest(const google::protobuf::Message &message) : message_(&message) {}
    void MergeFrom(const CountedRequest &other) override { message_ = other.message_; }
    size_t ByteSizeLong() const override { return message_->ByteSizeLong(); }
    uint8_t *_InternalSerialize(uint8_t *target, google::protobuf::io::EpsCopyOutputStream *stream) const override
    {
        ++encodes;
        return message_->_InternalSerialize(target, stream);
    }
    mutable unsigned encodes = 0;
private:
    const google::protobuf::Message *message_ = nullptr;
};

class AuthProbe final : public google::protobuf::Service {
public:
    AuthProbe() : pool_(google::protobuf::DescriptorPool::generated_pool())
    {
        google::protobuf::FileDescriptorProto file;
        file.set_name("rpc_byte_auth_test.proto");
        file.set_package("rpc_byte_auth_test");
        file.add_dependency(TestReqPbV1::descriptor()->file()->name());
        for (const auto *name : {"Probe", "ProbeAuth"}) {
            auto *service = file.add_service();
            service->set_name(name);
            auto *method = service->add_method();
            method->set_name("Echo");
            method->set_input_type(".datasystem.TestReqPbV1");
            method->set_output_type(".datasystem.TestRspPbV1");
        }
        file_ = pool_.BuildFile(file);
    }
    const google::protobuf::ServiceDescriptor *Plain() const { return file_->service(0); }
    const google::protobuf::ServiceDescriptor *GetDescriptor() override { return file_->service(1); }
    const google::protobuf::Message &GetRequestPrototype(const google::protobuf::MethodDescriptor *) const override
    {
        return TestReqPbV1::default_instance();
    }
    const google::protobuf::Message &GetResponsePrototype(const google::protobuf::MethodDescriptor *) const override
    {
        return TestRspPbV1::default_instance();
    }
    void CallMethod(const google::protobuf::MethodDescriptor *method, google::protobuf::RpcController *base,
                    const google::protobuf::Message *request, google::protobuf::Message *,
                    google::protobuf::Closure *done) override
    {
        brpc::ClosureGuard guard(done);
        auto *controller = static_cast<brpc::Controller *>(base);
        const auto &typed = static_cast<const TestReqPbV1 &>(*request);
        const auto *capture = GetRpcRequestCapture(*controller);
        if (typed.auth_req() == 1) {
            return;
        }
        const auto status = VerifyRpcRequest(manager, capture);
        if (status.IsError()) {
            controller->SetFailed(status.ToString());
            return;
        }
        butil::IOBuf reencoded;
        reencoded.append(request->SerializeAsString());
        AkSkDigest digest{};
        EXPECT_TRUE(ComputeRpcRequestDigest(*method, capture->timestampUs, reencoded, digest).IsOk());
        EXPECT_NE(digest, capture->digest);
        verified.fetch_add(1);
    }
    AkSkManager manager;
    std::atomic<unsigned> verified{0};
private:
    google::protobuf::DescriptorPool pool_;
    const google::protobuf::FileDescriptor *file_ = nullptr;
};

class GeneratedAuthProbe {
public:
    template <typename... Args>
    Status GetWorkerGRefTable(const RpcRequestProof &, Args &&...) { return Status::OK(); }
    template <typename... Args>
    Status GetMasterGRefTable(const RpcRequestProof &, Args &&...) { return Status::OK(); }
    template <typename... Args>
    Status GetCmNodeTable(const RpcRequestProof &, Args &&...) { return Status::OK(); }
    template <typename... Args>
    Status TestUnaryCompatibilityV1(const RpcRequestProof &proof, Args &&...)
    {
        return VerifyRpcRequest(manager, &proof);
    }
    template <typename... Args>
    Status TestUnaryCompatibilityV2(const RpcRequestProof &proof, Args &&...)
    {
        return VerifyRpcRequest(manager, &proof);
    }
    Status TestStreamCompatibilityV1(
        const RpcRequestProof &proof, const std::shared_ptr<ServerWriterReader<TestRspPbV1, TestReqPbV1>> &stream)
    {
        TestRspPbV1 reply;
        RETURN_IF_NOT_OK(ReadFirstRequest<TestReqPbV1>(proof, stream, reply));
        RETURN_IF_NOT_OK(stream->Write(reply));
        bidiStream.set_value(stream);
        return Status::OK();
    }
    template <typename... Args>
    Status TestStreamCompatibilityV2(const RpcRequestProof &, Args &&...) { return Status::OK(); }
    template <typename R, typename Reader>
    Status ReadFirstRequest(const RpcRequestProof &proof, const Reader &reader, TestRspPbV1 &reply)
    {
        R request;
        RETURN_IF_NOT_OK(reader->Read(request));
        EXPECT_EQ(request.auth_req(), 2);
        RETURN_IF_NOT_OK(VerifyRpcRequest(manager, &proof));
        reply.GetReflection()->MutableUnknownFields(&reply)->AddVarint(7, request.auth_req());
        verified.fetch_add(1);
        return Status::OK();
    }
    Status TestStreamCompatibility1V1(const RpcRequestProof &proof,
                                      const std::shared_ptr<ServerReader<TestReqPbV1>> &reader, TestRspPbV1 &reply)
    {
        return ReadFirstRequest<TestReqPbV1>(proof, reader, reply);
    }
    Status TestStreamCompatibility1V2(const RpcRequestProof &proof,
                                      const std::shared_ptr<ServerReader<TestReqPbV2>> &reader, TestRspPbV1 &reply)
    {
        return ReadFirstRequest<TestReqPbV2>(proof, reader, reply);
    }
    AkSkManager manager;
    std::atomic<unsigned> verified{0};
    std::promise<std::shared_ptr<ServerWriterReader<TestRspPbV1, TestReqPbV1>>> bidiStream;
};

class BrpcAuthTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(RegisterRpcAuthCapture().IsOk());
        ASSERT_TRUE(service_.manager.SetServerAkSk(
            AkSkType::SYSTEM, "test-only-access", SensitiveValue("test-only-secret")).IsOk());
        ASSERT_TRUE(generated_.manager.SetServerAkSk(
            AkSkType::SYSTEM, "test-only-access", SensitiveValue("test-only-secret")).IsOk());
        ASSERT_EQ(server_.AddService(&service_, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        ASSERT_EQ(server_.AddService(&adapter_, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        brpc::ServerOptions options;
        options.session_local_data_factory = &factory_;
        ASSERT_EQ(server_.Start("127.0.0.1:0", &options), 0);
        ASSERT_TRUE(RpcMethodMap::Create({{service_.Plain(), service_.GetDescriptor()}}, methods_).IsOk());
        provider_ = std::make_shared<Signature>("test-only-access", SensitiveValue("test-only-secret"));
        BrpcChannelConfig config;
        config.endpoint = butil::endpoint2str(server_.listen_address()).c_str();
        config.timeout_ms = 1000;
        config.max_retry = 0;
        ASSERT_TRUE(CreateBrpcAuthChannel(config, {provider_, methods_}, channel_).IsOk());
    }
    void TearDown() override
    {
        server_.Stop(0);
        adapter_.StopAndWait();
        server_.Join();
    }
    RpcAuthDataFactory factory_;
    AuthProbe service_;
    GeneratedAuthProbe generated_;
    UtOCServiceBrpcAuthAdapter<GeneratedAuthProbe> adapter_{generated_};
    brpc::Server server_;
    std::shared_ptr<Signature> provider_;
    std::shared_ptr<const RpcMethodMap> methods_;
    std::unique_ptr<brpc::Channel> channel_;
};
}  // namespace

TEST(BrpcAuthDescriptorTest, GeneratedCompanionPreservesMethodsAndRejectsInvalidMappings)
{
    const auto *plain = TestReqPbV1::descriptor()->file()->FindServiceByName("UtOCService");
    const auto *auth = UtOCServiceAuth::descriptor();
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(auth, nullptr);
    std::shared_ptr<const RpcMethodMap> methods;
    ASSERT_TRUE(RpcMethodMap::Create({{plain, auth}}, methods).IsOk());
    for (int i = 0; i < plain->method_count(); ++i) {
        SCOPED_TRACE(plain->method(i)->full_name());
        const auto *mapped = methods->Find(plain->method(i));
        ASSERT_EQ(mapped, auth->method(i));
        EXPECT_EQ(mapped->name(), plain->method(i)->name());
        EXPECT_EQ(mapped->input_type(), plain->method(i)->input_type());
        EXPECT_EQ(mapped->output_type(), plain->method(i)->output_type());
        EXPECT_EQ(mapped->client_streaming(), plain->method(i)->client_streaming());
        EXPECT_EQ(mapped->server_streaming(), plain->method(i)->server_streaming());
    }
    EXPECT_FALSE(RpcMethodMap::Create({{plain, plain}}, methods).IsOk());
    EXPECT_FALSE(RpcMethodMap::Create({{plain, auth}, {plain, auth}}, methods).IsOk());
    EXPECT_EQ(google::protobuf::DescriptorPool::generated_pool()->FindServiceByName(
                  "datasystem.UtOCServiceAuthAuth"), nullptr);
}

TEST_F(BrpcAuthTest, EncodesOnceAndVerifiesRawBytesAcrossSchemas)
{
    TestReqPbV2 request;
    request.set_filed1(1);
    request.set_filed5(5);
    request.set_auth_req(2);
    unsigned verified = 0;
    for (const auto compression : {brpc::COMPRESS_TYPE_NONE, brpc::COMPRESS_TYPE_GZIP}) {
        SCOPED_TRACE(compression);
        CountedRequest counted(request);
        TestRspPbV1 response;
        brpc::Controller controller;
        controller.set_request_compress_type(compression);
        channel_->CallMethod(service_.Plain()->method(0), &controller, &counted, &response, nullptr);
        EXPECT_FALSE(controller.Failed()) << controller.ErrorText();
        EXPECT_EQ(counted.encodes, 1);
        EXPECT_EQ(service_.verified.load(), ++verified);
    }
}

TEST_F(BrpcAuthTest, ScMigrationRejectsCompressionBeforeEncoding)
{
    const auto *plain = master::MigrateSCMetadataReqPb::descriptor()->file()->FindServiceByName("MasterSCService");
    ASSERT_NE(plain, nullptr);
    std::shared_ptr<const RpcMethodMap> methods;
    ASSERT_TRUE(RpcMethodMap::Create({{plain, master::MasterSCServiceAuth::descriptor()}}, methods).IsOk());
    BrpcChannelConfig config;
    config.endpoint = butil::endpoint2str(server_.listen_address()).c_str();
    config.timeout_ms = 1000;
    config.max_retry = 0;
    std::unique_ptr<brpc::Channel> channel;
    ASSERT_TRUE(CreateBrpcAuthChannel(config, {provider_, methods}, channel).IsOk());
    const auto *method = plain->FindMethodByName("MigrateSCMetadata");
    ASSERT_NE(method, nullptr);
    master::MigrateSCMetadataReqPb request;
    master::MigrateSCMetadataRspPb response;
    CountedRequest counted(request);
    brpc::Controller controller;
    controller.set_request_compress_type(brpc::COMPRESS_TYPE_GZIP);
    channel->CallMethod(method, &controller, &counted, &response, nullptr);
    ASSERT_TRUE(controller.Failed());
    EXPECT_NE(controller.ErrorText().find("SC metadata authentication requires uncompressed PB"), std::string::npos);
    EXPECT_EQ(counted.encodes, 0);
}

TEST_F(BrpcAuthTest, GeneratedAdapterOwnsProofAfterFirstStreamHandshake)
{
    const auto *plain = TestReqPbV1::descriptor()->file()->FindServiceByName("UtOCService");
    std::shared_ptr<const RpcMethodMap> methods;
    ASSERT_TRUE(RpcMethodMap::Create({{plain, UtOCServiceAuth::descriptor()}}, methods).IsOk());
    BrpcChannelConfig config;
    config.endpoint = butil::endpoint2str(server_.listen_address()).c_str();
    config.timeout_ms = 1000;
    config.max_retry = 0;
    std::unique_ptr<brpc::Channel> channel;
    ASSERT_TRUE(CreateBrpcAuthChannel(config, {provider_, methods}, channel).IsOk());
    TestReqPbV1 request;
    request.set_auth_req(2);
    CountedRequest counted(request);
    const auto *method = plain->FindMethodByName("TestStreamCompatibility1V1");
    BrpcClientWriterImpl<CountedRequest> writer(channel.get(), method, 1000);
    ASSERT_TRUE(writer.Write(counted).IsOk());
    TestRspPbV1 response;
    ASSERT_TRUE(writer.Read(response).IsOk());
    const auto &fields = response.GetReflection()->GetUnknownFields(response);
    ASSERT_EQ(fields.field_count(), 1);
    EXPECT_EQ(fields.field(0).number(), 7);
    EXPECT_EQ(fields.field(0).varint(), 2);
    ASSERT_TRUE(writer.Finish().IsOk());
    EXPECT_EQ(counted.encodes, 1);
    EXPECT_EQ(generated_.verified.load(), 1);

    CountedRequest bidiCounted(request);
    auto bidi = std::make_shared<BrpcClientStreamWriterReader<CountedRequest, TestRspPbV1>>(
        channel.get(), plain->FindMethodByName("TestStreamCompatibilityV1"), 1000);
    ASSERT_TRUE(bidi->Write(bidiCounted).IsOk());
    TestRspPbV1 bidiResponse;
    ASSERT_TRUE(bidi->Read(bidiResponse).IsOk());
    const auto &bidiFields = bidiResponse.GetReflection()->GetUnknownFields(bidiResponse);
    ASSERT_EQ(bidiFields.field_count(), 1);
    EXPECT_EQ(bidiFields.field(0).number(), 7);
    EXPECT_EQ(bidiFields.field(0).varint(), 2);
    auto serverStream = generated_.bidiStream.get_future().get();
    ASSERT_TRUE(serverStream->Finish().IsOk());
    serverStream.reset();
    ASSERT_TRUE(bidi->Finish().IsOk());
    EXPECT_EQ(bidiCounted.encodes, 1);
    EXPECT_EQ(generated_.verified.load(), 2);
}

TEST_F(BrpcAuthTest, MissingProofOnlyFailsWhenTheMethodRequiresIt)
{
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.timeout_ms = 1000;
    options.max_retry = 0;
    ASSERT_EQ(channel.Init(server_.listen_address(), &options), 0);
    TestReqPbV1 request;
    TestRspPbV1 response;
    for (unsigned branch : {1U, 2U}) {
        request.set_auth_req(branch);
        brpc::Controller controller;
        channel.CallMethod(service_.GetDescriptor()->method(0), &controller, &request, &response, nullptr);
        EXPECT_EQ(controller.Failed(), branch == 2U);
    }
    for (unsigned fault : {0U, 1U, 2U, 3U, 4U}) {
        TestReqPbV1 payload;
        payload.set_auth_req(2);
        const auto now = static_cast<uint64_t>(GetSystemClockTimeStampUs());
        const auto timestamp = fault == 4 ? now - 301000000ULL : now;
        butil::IOBuf bytes;
        bytes.append(payload.SerializeAsString());
        AkSkDigest digest{};
        ASSERT_TRUE(ComputeRpcRequestDigest(*service_.Plain()->method(0), timestamp, bytes, digest).IsOk());
        AkSkData key;
        ASSERT_TRUE(provider_->SnapshotClientKey(key).IsOk());
        std::string signature;
        ASSERT_TRUE(Signature::GetSignatureFromDigest(key, digest, signature).IsOk());
        brpc::Controller controller;
        auto &fields = *controller.request_user_fields();
        fields["ds.auth.ak"] = fault == 2 ? "test-only-unknown" : key.accessKey;
        fields["ds.auth.ts"] = std::to_string(timestamp + (fault == 1 ? 1 : 0));
        if (fault == 0) {
            signature.front() ^= 1;
        }
        fields["ds.auth.sig"] = signature;
        if (fault == 3) {
            payload.set_field2("tampered-body");
        }
        controller.set_request_checksum_type(RPC_AUTH_CHECKSUM_TYPE);
        channel.CallMethod(service_.GetDescriptor()->method(0), &controller, &payload, &response, nullptr);
        EXPECT_TRUE(controller.Failed()) << "fault=" << fault;
    }
    class ForeignFactory final : public brpc::DataFactory {
    public:
        void *CreateData() const override { return new uint64_t(0); }
        void DestroyData(void *data) const override { delete static_cast<uint64_t *>(data); }
        bool ResetData(void *data) const override { *static_cast<uint64_t *>(data) = 0; return true; }
    } foreign;
    const brpc::DataFactory *factories[] = {nullptr, &foreign};
    for (const auto *factory : factories) {
        brpc::Server other;
        ASSERT_EQ(other.AddService(&service_, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        brpc::ServerOptions serverOptions;
        serverOptions.session_local_data_factory = factory;
        ASSERT_EQ(other.Start("127.0.0.1:0", &serverOptions), 0);
        BrpcChannelConfig config;
        config.endpoint = butil::endpoint2str(other.listen_address()).c_str();
        config.timeout_ms = 1000;
        config.max_retry = 0;
        std::unique_ptr<brpc::Channel> signedChannel;
        ASSERT_TRUE(CreateBrpcAuthChannel(config, {provider_, methods_}, signedChannel).IsOk());
        request.set_auth_req(2);
        brpc::Controller controller;
        signedChannel->CallMethod(service_.Plain()->method(0), &controller, &request, &response, nullptr);
        EXPECT_TRUE(controller.Failed());
        other.Stop(0);
        other.Join();
    }

}

TEST_F(BrpcAuthTest, NativeCompletionPreservesAsyncOwnershipAndCancellation)
{
    class Done final : public google::protobuf::Closure {
    public:
        explicit Done(brpc::Controller *controller) : controller_(controller) {}
        void Run() override
        {
            failed = controller_->Failed();
            delete controller_;
            ++calls;
        }
        bool failed = false;
        std::atomic<unsigned> calls{0};
    private:
        brpc::Controller *controller_;
    };
    for (unsigned scenario : {0U, 1U, 2U}) {
        TestReqPbV2 request;
        request.set_filed5(5);
        request.set_auth_req(2);
        CountedRequest counted(request);
        TestRspPbV1 response;
        auto *controller = new brpc::Controller;
        const auto call = controller->call_id();
        Done done(controller);
        if (scenario == 1) {
            brpc::StartCancel(call);
        }
        const auto *method = scenario == 0 ? service_.GetDescriptor()->method(0) : service_.Plain()->method(0);
        channel_->CallMethod(method, controller, &counted, &response, &done);
        brpc::Join(call);
        EXPECT_EQ(done.failed, scenario != 2);
        EXPECT_EQ(done.calls.load(), 1);
        EXPECT_EQ(counted.encodes, scenario == 2 ? 1 : 0);
    }
}
}  // namespace test
}  // namespace datasystem
