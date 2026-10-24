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
#include <chrono>
#include <future>
#include <memory>
#include <type_traits>

#include <gtest/gtest.h>
#include <brpc/server.h>

#include "tests/common/binmock/binmock.h"
#include "datasystem/cluster/membership/membership_endpoint_view.h"
#include "datasystem/cluster/runtime/topology_snapshot_state.h"
#include "datasystem/common/ak_sk/ak_sk_manager.h"
#include "datasystem/common/rpc/brpc_auth.h"
#include "datasystem/common/rpc/brpc_server_unary_writer_reader.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/thread_pool.h"
#include "datasystem/common/util/timer.h"
#include "datasystem/master/metadata_manager_holder.h"
#include "datasystem/master/stream_cache/master_sc_rpc_dispatcher.h"
#include "datasystem/protos/master_stream.brpc.auth.pb.h"
#include "datasystem/protos/master_object.brpc.auth.pb.h"
#define private public
#include "datasystem/master/object_cache/master_oc_service_impl.h"
#undef private
#include "datasystem/master/object_cache/master_oc_rpc_dispatcher.h"

namespace datasystem {
namespace test {
namespace {
class MasterRpcAdmissionTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(provider_.SnapshotClientKey(key_).IsOk());
        ASSERT_TRUE(manager_->SetServerAkSk(AkSkType::SYSTEM, key_.accessKey, key_.secretKey).IsOk());
        service_ = std::make_unique<master::MasterOCServiceImpl>(
            HostPort(), nullptr, manager_, &holder_, nullptr, membership_, "", nullptr);
        proof_.captured = true;
        proof_.timestampUs = GetSystemClockTimeStampUs();
        proof_.accessKey = key_.accessKey;
        ASSERT_TRUE(Signature::GetSignatureFromDigest(key_, proof_.digest, proof_.signature).IsOk());
    }
    cluster::TopologySnapshotState snapshots_;
    cluster::MembershipEndpointView membership_{snapshots_};
    MetadataManagerHolder holder_;
    Signature provider_{"test-only-master-access", SensitiveValue("test-only-master-secret")};
    AkSkData key_;
    std::shared_ptr<AkSkManager> manager_ = std::make_shared<AkSkManager>();
    std::unique_ptr<master::MasterOCServiceImpl> service_;
    RpcRequestProof proof_;
};
}  // namespace

TEST_F(MasterRpcAdmissionTest, KeepsMetadataAvailabilityAndReadBeforeAuthentication)
{
    master::MasterOCServiceRpcDispatcher dispatcher(*service_);
    master::MasterOCServiceBrpcAuthAdapter<master::MasterOCServiceRpcDispatcher> adapter(dispatcher);
    EXPECT_EQ(adapter.GetDescriptor()->method_count(), 44);
    const RpcRequestProof missing;
    master::GIncNestedRefReqPb nested;
    master::GIncNestedRefRspPb incremented;
    EXPECT_EQ(service_->GIncNestedRef(nested, incremented).GetCode(), K_RUNTIME_ERROR);
    EXPECT_EQ(service_->GIncNestedRefWithRpcAuth(nested, incremented, missing).GetCode(), K_RUNTIME_ERROR);
    master::CreateMetaReqPb create;
    master::CreateMetaRspPb created;
    EXPECT_EQ(service_->CreateMeta(create, created).GetCode(), K_NOT_AUTHORIZED);
    EXPECT_EQ(service_->CreateMetaWithRpcAuth(create, created, missing).GetCode(), K_NOT_AUTHORIZED);
    ASSERT_TRUE(provider_.GenerateSignature(create).IsOk());
    EXPECT_EQ(service_->CreateMeta(create, created).GetCode(), K_RUNTIME_ERROR);
    EXPECT_EQ(service_->CreateMetaWithRpcAuth(create, created, proof_).GetCode(), K_RUNTIME_ERROR);
    master::CreateMultiCopyMetaReqPb copies;
    master::CreateMultiCopyMetaRspPb copied;
    EXPECT_EQ(service_->CreateMultiCopyMeta(copies, copied).GetCode(), K_NOT_AUTHORIZED);
    EXPECT_EQ(service_->CreateMultiCopyMetaWithRpcAuth(copies, copied, missing).GetCode(), K_NOT_AUTHORIZED);
    master::RemoveMetaReqPb remove;
    master::RemoveMetaRspPb removed;
    EXPECT_EQ(service_->RemoveMeta(remove, removed).GetCode(), K_NOT_AUTHORIZED);
    EXPECT_EQ(service_->RemoveMetaWithRpcAuth(remove, removed, missing).GetCode(), K_NOT_AUTHORIZED);
    auto read = [&](auto &request, auto &response, auto ordinary, auto authenticated) {
        using Request = std::decay_t<decltype(request)>;
        using Response = std::decay_t<decltype(response)>;
        for (bool supplied : {false, true}) {
            for (bool useRpc : {false, true}) {
                brpc::Controller controller;
                auto api = std::make_shared<BrpcServerUnaryWriterReader<Response, Request>>(
                    &controller, supplied ? &request : nullptr, &response, nullptr, "MasterAdmission");
                const auto rc = useRpc ? (service_.get()->*authenticated)(api, missing)
                                       : (service_.get()->*ordinary)(api);
                EXPECT_EQ(rc.GetCode(), supplied ? K_NOT_AUTHORIZED : K_RUNTIME_ERROR) << rc.ToString();
                if (!supplied) {
                    EXPECT_NE(rc.GetMsg().find("Request protobuf is null"), std::string::npos);
                }
            }
        }
    };
    master::DeleteAllCopyMetaReqPb erase;
    master::DeleteAllCopyMetaRspPb erased;
    using Delete = Status (master::MasterOCServiceImpl::*)(std::shared_ptr<
        ServerUnaryWriterReader<master::DeleteAllCopyMetaRspPb, master::DeleteAllCopyMetaReqPb>>);
    using DeleteAuth = Status (master::MasterOCServiceImpl::*)(std::shared_ptr<
        ServerUnaryWriterReader<master::DeleteAllCopyMetaRspPb, master::DeleteAllCopyMetaReqPb>>, const RpcRequestProof &);
    read(erase, erased, static_cast<Delete>(&master::MasterOCServiceImpl::DeleteAllCopyMeta),
         static_cast<DeleteAuth>(&master::MasterOCServiceImpl::DeleteAllCopyMetaWithRpcAuth));
}

TEST_F(MasterRpcAdmissionTest, ScMasterKeepsFirstRequestAndMigrationAuthenticationBoundaries)
{
    master::MasterSCServiceImpl service(HostPort(), manager_, &holder_, membership_, "", false, false);
    master::MasterSCServiceRpcDispatcher dispatcher(service);
    master::MasterSCServiceBrpcAuthAdapter<master::MasterSCServiceRpcDispatcher> adapter(dispatcher);
    EXPECT_EQ(adapter.GetDescriptor()->method_count(), 8);
    const RpcRequestProof missing;
    auto read = [&](auto &request, auto &response, auto ordinary, auto authenticated) {
        using Request = std::decay_t<decltype(request)>;
        using Response = std::decay_t<decltype(response)>;
        for (bool supplied : {false, true}) {
            for (bool useRpc : {false, true}) {
                brpc::Controller controller;
                auto api = std::make_shared<BrpcServerUnaryWriterReader<Response, Request>>(
                    &controller, supplied ? &request : nullptr, &response, nullptr, "ScMasterAdmission");
                const auto rc = useRpc ? (service.*authenticated)(api, missing) : (service.*ordinary)(api);
                EXPECT_EQ(rc.GetCode(), supplied ? K_NOT_AUTHORIZED : K_RUNTIME_ERROR) << rc.ToString();
            }
        }
    };
    master::CreateProducerReqPb producer;
    master::CreateProducerRspPb produced;
    read(producer, produced, &master::MasterSCServiceImpl::CreateProducer,
         &master::MasterSCServiceImpl::CreateProducerWithRpcAuth);
    master::CloseProducerReqPb close;
    master::CloseProducerRspPb closed;
    read(close, closed, &master::MasterSCServiceImpl::CloseProducer,
         &master::MasterSCServiceImpl::CloseProducerWithRpcAuth);
    master::SubscribeReqPb subscribe;
    master::SubscribeRspPb subscribed;
    read(subscribe, subscribed, &master::MasterSCServiceImpl::Subscribe,
         &master::MasterSCServiceImpl::SubscribeWithRpcAuth);
    master::CloseConsumerReqPb consumer;
    master::CloseConsumerRspPb consumed;
    read(consumer, consumed, &master::MasterSCServiceImpl::CloseConsumer,
         &master::MasterSCServiceImpl::CloseConsumerWithRpcAuth);
    master::MigrateSCMetadataReqPb migration;
    migration.set_topology_version(1);
    migration.add_stream_metas()->add_notifications()->set_id("test-only-notification");
    master::MigrateSCMetadataRspPb migrated;
    EXPECT_EQ(service.MigrateSCMetadata(migration, migrated).GetCode(), K_NOT_AUTHORIZED);
    EXPECT_EQ(service.MigrateSCMetadataWithRpcAuth(migration, migrated, missing).GetCode(), K_NOT_AUTHORIZED);
    auto projected = migration;
    projected.mutable_stream_metas(0)->clear_notifications();
    ASSERT_TRUE(provider_.GenerateSignature(projected).IsOk());
    migration.set_timestamp(projected.timestamp());
    migration.set_signature(projected.signature());
    migration.set_access_key(projected.access_key());
    EXPECT_EQ(service.MigrateSCMetadata(migration, migrated).GetCode(), K_INVALID);
    EXPECT_EQ(service.MigrateSCMetadataWithRpcAuth(migration, migrated, proof_).GetCode(), K_INVALID);

    ASSERT_TRUE(RegisterRpcAuthCapture().IsOk());
    RpcAuthDataFactory factory;
    brpc::Server server;
    ASSERT_EQ(server.AddService(&adapter, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
    brpc::ServerOptions options;
    options.session_local_data_factory = &factory;
    ASSERT_EQ(server.Start("127.0.0.1:0", &options), 0);
    Raii stop([&server] { server.Stop(0); server.Join(); });
    const auto *plain = master::MigrateSCMetadataReqPb::descriptor()->file()->FindServiceByName("MasterSCService");
    ASSERT_NE(plain, nullptr);
    std::shared_ptr<const RpcMethodMap> methods;
    ASSERT_TRUE(RpcMethodMap::Create({{plain, master::MasterSCServiceAuth::descriptor()}}, methods).IsOk());
    auto provider = std::make_shared<Signature>(key_.accessKey, key_.secretKey);
    BrpcChannelConfig config;
    config.endpoint = butil::endpoint2str(server.listen_address()).c_str();
    config.timeout_ms = 1000;
    config.max_retry = 0;
    std::unique_ptr<brpc::Channel> channel;
    ASSERT_TRUE(CreateBrpcAuthChannel(config, {provider, methods}, channel).IsOk());
    brpc::Controller controller;
    channel->CallMethod(plain->FindMethodByName("MigrateSCMetadata"), &controller, &migration, &migrated, nullptr);
    ASSERT_TRUE(controller.Failed());
    EXPECT_NE(controller.ErrorText().find("Incomplete topology migration RPC fence"), std::string::npos);

    const auto timestamp = static_cast<uint64_t>(GetSystemClockTimeStampUs());
    butil::IOBuf originalBytes;
    originalBytes.append(migration.SerializeAsString());
    AkSkDigest digest{};
    const auto *method = plain->FindMethodByName("MigrateSCMetadata");
    ASSERT_TRUE(ComputeRpcRequestDigest(*method, timestamp, originalBytes, digest).IsOk());
    std::string signature;
    ASSERT_TRUE(Signature::GetSignatureFromDigest(key_, digest, signature).IsOk());
    auto native = BrpcChannelFactory::Create(config);
    ASSERT_NE(native, nullptr);
    auto callOriginalSignature = [&](const std::string &expectedReason) {
        brpc::Controller call;
        call.set_request_checksum_type(RPC_AUTH_CHECKSUM_TYPE);
        auto &fields = *call.request_user_fields();
        fields["ds.auth.ak"] = key_.accessKey;
        fields["ds.auth.ts"] = std::to_string(timestamp);
        fields["ds.auth.sig"] = signature;
        native->CallMethod(master::MasterSCServiceAuth::descriptor()->FindMethodByName("MigrateSCMetadata"),
                           &call, &migration, &migrated, nullptr);
        EXPECT_TRUE(call.Failed());
        EXPECT_NE(call.ErrorText().find(expectedReason), std::string::npos) << call.ErrorText();
    };
    migration.mutable_stream_metas(0)->mutable_notifications(0)->set_id("test-only-altered-notification");
    callOriginalSignature("Incomplete topology migration RPC fence");
    migration.set_source_addr("test-only-altered-source");
    callOriginalSignature("AK/SK authorized failed");
}

TEST_F(MasterRpcAdmissionTest, QueuedReconciliationOwnsProofAndVerifiesOnlyWhenDequeued)
{
    using namespace ::testing;
    std::atomic<unsigned> verified{0};
    const auto expectedTimestamp = proof_.timestampUs;
    const auto expectedSignature = proof_.signature;
    BINEXPECT_CALL(&AkSkManager::VerifySignatureFromDigest, (_, _, _, _))
        .WillOnce([&](const std::string &signature, uint64_t timestamp, const std::string &accessKey,
                      const AkSkDigest &digest) {
            EXPECT_EQ(timestamp, expectedTimestamp);
            EXPECT_EQ(signature, expectedSignature);
            EXPECT_EQ(accessKey, key_.accessKey);
            EXPECT_EQ(digest, AkSkDigest{});
            ++verified;
            return Status(K_NOT_AUTHORIZED, "test-only-queued-authentication");
        });
    Raii releaseStubs([] { RELEASE_STUBS });
    class Done final : public google::protobuf::Closure {
    public:
        void Run() override { completion.set_value(); }
        std::promise<void> completion;
    } done;
    auto completed = done.completion.get_future();
    brpc::Controller controller;
    master::ReconciliationQueryPb request;
    master::ReconciliationRspPb response;
    std::promise<void> started;
    auto running = started.get_future();
    std::promise<void> release;
    auto gate = release.get_future();
    bool released = false;
    service_->reconciliationAsyncPool_ = std::make_unique<ThreadPool>(1, 1, "RpcAuthTest");
    Raii drain([&] {
        if (!released) {
            release.set_value();
        }
        service_->reconciliationAsyncPool_.reset();
    });
    auto blocker = service_->reconciliationAsyncPool_->Submit([&] {
        started.set_value();
        gate.wait();
        return Status::OK();
    });
    ASSERT_EQ(running.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    {
        RpcRequestProof temporary = proof_;
        auto api = std::make_shared<BrpcServerUnaryWriterReader<master::ReconciliationRspPb, master::ReconciliationQueryPb>>(
            &controller, &request, &response, &done, "QueuedReconciliation");
        ASSERT_TRUE(service_->ReconcileMembershipChangeWithRpcAuth(api, temporary).IsOk());
        temporary = RpcRequestProof{};
    }
    EXPECT_EQ(verified.load(), 0);
    release.set_value();
    released = true;
    ASSERT_EQ(completed.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(verified.load(), 1);
    EXPECT_TRUE(blocker.get().IsOk());
}
}  // namespace test
}  // namespace datasystem
