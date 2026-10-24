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


#include <memory>
#include <type_traits>

#include <gtest/gtest.h>

#include "tests/common/binmock/binmock.h"
#include "datasystem/cluster/runtime/topology_snapshot_state.h"
#include "datasystem/common/flags/common_flags.h"
#include "datasystem/common/rpc/brpc_auth.h"
#include "datasystem/common/rpc/brpc_server_unary_writer_reader.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/timer.h"
#include "datasystem/protos/stream_posix.brpc.auth.pb.h"
#include "datasystem/protos/worker_stream.brpc.auth.pb.h"
#include "datasystem/worker/authenticate.h"
#include "datasystem/worker/client_manager/client_manager.h"
#include "datasystem/worker/worker_health_check.h"
#include "datasystem/worker/stream_cache/client_worker_sc_rpc_dispatcher.h"
#include "datasystem/worker/stream_cache/master_worker_sc_rpc_dispatcher.h"
#include "datasystem/worker/stream_cache/worker_worker_sc_rpc_dispatcher.h"

namespace datasystem {
namespace test {
namespace {
using namespace worker::stream_cache;
class ScRpcAdmissionTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(provider_.SnapshotClientKey(key_).IsOk());
        ASSERT_TRUE(manager_->SetServerAkSk(AkSkType::SYSTEM, key_.accessKey, key_.secretKey).IsOk());
        client_ = std::make_unique<ClientWorkerSCServiceImpl>(HostPort(), HostPort(), nullptr, manager_, nullptr,
                                                               route_, membership_);
        proof_.captured = true;
        proof_.timestampUs = GetSystemClockTimeStampUs();
        proof_.accessKey = key_.accessKey;
        ASSERT_TRUE(Signature::GetSignatureFromDigest(key_, proof_.digest, proof_.signature).IsOk());
    }
    template <typename Service, typename Request, typename Response, typename Native, typename Auth>
    void ReadBeforeAuth(Service &service, Request &request, Response &response, Native native, Auth auth,
                        const worker::RpcTenantResolver &resolver, StatusCode suppliedStatus)
    {
        for (bool supplied : {false, true}) {
            for (bool useRpc : {false, true}) {
                brpc::Controller controller;
                auto api = std::make_shared<BrpcServerUnaryWriterReader<Response, Request>>(
                    &controller, supplied ? &request : nullptr, &response, nullptr, "ScAdmission");
                const auto rc = useRpc ? (service.*auth)(api, resolver) : (service.*native)(api);
                EXPECT_EQ(rc.GetCode(), supplied ? suppliedStatus : K_RUNTIME_ERROR) << rc.ToString();
            }
        }
    }
    cluster::TopologySnapshotState snapshots_;
    cluster::MembershipEndpointView membership_{snapshots_};
    worker::MetadataRouteResolver route_{nullptr, worker::MetadataRouteOptions{}};
    Signature provider_{"test-only-sc-access", SensitiveValue("test-only-sc-secret")};
    AkSkData key_;
    std::shared_ptr<AkSkManager> manager_ = std::make_shared<AkSkManager>();
    std::unique_ptr<ClientWorkerSCServiceImpl> client_;
    RpcRequestProof proof_;
};
}  // namespace

TEST_F(ScRpcAdmissionTest, ClientSessionAndBlockingKeepTheirOriginalAdmissionPoints)
{
    using namespace ::testing;
    const bool skip = FLAGS_skip_authenticate;
    FLAGS_skip_authenticate = false;
    Raii restore([skip] { FLAGS_skip_authenticate = skip; });
    bool ready = false;
    size_t lookups = 0;
    BINEXPECT_CALL(&::datasystem::IsHealthy, ()).WillRepeatedly([&] { return ready; });
    BINEXPECT_CALL(&worker::ClientManager::GetClientInfo, (_))
        .WillRepeatedly([&](const ClientKey &client) {
            EXPECT_EQ(client.ToString(), "test-only-sc-client");
            ++lookups;
            return std::shared_ptr<worker::ClientInfo>{};
        });
    Raii releaseStubs([] { RELEASE_STUBS });
    ClientWorkerSCServiceRpcDispatcher dispatcher(*client_);
    ClientWorkerSCServiceBrpcAuthAdapter<ClientWorkerSCServiceRpcDispatcher> adapter(dispatcher);
    EXPECT_EQ(adapter.GetDescriptor()->method_count(), 16);
    const RpcRequestProof missing;
    worker::RpcTenantResolver rejected(missing);
    CreateProducerReqPb create;
    create.set_client_id("test-only-sc-client");
    CreateProducerRspPb created;
    {
        brpc::Controller controller;
        auto api = std::make_shared<BrpcServerUnaryWriterReader<CreateProducerRspPb, CreateProducerReqPb>>(
            &controller, nullptr, &created, nullptr, "ScAdmissionState");
        EXPECT_EQ(client_->CreateProducer(api).GetCode(), K_NOT_READY);
        EXPECT_EQ(client_->CreateProducerWithTenantResolver(api, rejected).GetCode(), K_NOT_READY);
        EXPECT_EQ(lookups, 0);
    }
    ready = true;
    ReadBeforeAuth(*client_, create, created, &ClientWorkerSCServiceImpl::CreateProducer,
                   &ClientWorkerSCServiceImpl::CreateProducerWithTenantResolver, rejected, K_CLIENT_WORKER_DISCONNECT);
    SubscribeReqPb subscribe;
    subscribe.set_client_id(create.client_id());
    SubscribeRspPb subscribed;
    ReadBeforeAuth(*client_, subscribe, subscribed, &ClientWorkerSCServiceImpl::Subscribe,
                   &ClientWorkerSCServiceImpl::SubscribeWithTenantResolver, rejected, K_CLIENT_WORKER_DISCONNECT);
    GetDataPageReqPb get;
    get.set_client_id(create.client_id());
    GetDataPageRspPb got;
    ReadBeforeAuth(*client_, get, got, &ClientWorkerSCServiceImpl::GetDataPage,
                   &ClientWorkerSCServiceImpl::GetDataPageWithTenantResolver, rejected, K_CLIENT_WORKER_DISCONNECT);
    CreateLobPageReqPb allocate;
    allocate.set_client_id(create.client_id());
    CreateLobPageRspPb allocated;
    ReadBeforeAuth(*client_, allocate, allocated, &ClientWorkerSCServiceImpl::AllocBigShmMemory,
                   &ClientWorkerSCServiceImpl::AllocBigShmMemoryWithTenantResolver, rejected, K_CLIENT_WORKER_DISCONNECT);
    ResetOrResumeStreamsReqPb reset;
    reset.set_client_id(create.client_id());
    ResetOrResumeStreamsRspPb resetResponse;
    ReadBeforeAuth(*client_, reset, resetResponse, &ClientWorkerSCServiceImpl::ResetStreams,
                   &ClientWorkerSCServiceImpl::ResetStreamsWithTenantResolver, rejected, K_CLIENT_WORKER_DISCONNECT);
    ReadBeforeAuth(*client_, reset, resetResponse, &ClientWorkerSCServiceImpl::ResumeStreams,
                   &ClientWorkerSCServiceImpl::ResumeStreamsWithTenantResolver, rejected, K_CLIENT_WORKER_DISCONNECT);
    EXPECT_EQ(lookups, 12);
    BlockProducerReqPb block;
    BlockProducerRspPb blocked;
    ReadBeforeAuth(*client_, block, blocked, &ClientWorkerSCServiceImpl::BlockProducer,
                   &ClientWorkerSCServiceImpl::BlockProducerWithTenantResolver, rejected, K_NOT_AUTHORIZED);
    UnblockProducerReqPb unblock;
    UnblockProducerRspPb unblocked;
    ReadBeforeAuth(*client_, unblock, unblocked, &ClientWorkerSCServiceImpl::UnblockProducer,
                   &ClientWorkerSCServiceImpl::UnblockProducerWithTenantResolver, rejected, K_NOT_AUTHORIZED);
    ASSERT_TRUE(provider_.GenerateSignature(block).IsOk());
    worker::RpcTenantResolver accepted(proof_);
    ReadBeforeAuth(*client_, block, blocked, &ClientWorkerSCServiceImpl::BlockProducer,
                   &ClientWorkerSCServiceImpl::BlockProducerWithTenantResolver, accepted, K_INVALID);
    EXPECT_EQ(lookups, 12);
}

TEST_F(ScRpcAdmissionTest, PeerPayloadAndMasterNotificationsKeepOriginalAuthenticationScope)
{
    MasterWorkerSCServiceImpl master(HostPort(), HostPort(), client_.get(), manager_);
    WorkerWorkerSCServiceImpl peer(client_.get(), manager_);
    MasterWorkerSCServiceRpcDispatcher masterDispatcher(master);
    WorkerWorkerSCServiceRpcDispatcher peerDispatcher(peer);
    MasterWorkerSCServiceBrpcAuthAdapter<MasterWorkerSCServiceRpcDispatcher> masterAdapter(masterDispatcher);
    WorkerWorkerSCServiceBrpcAuthAdapter<WorkerWorkerSCServiceRpcDispatcher> peerAdapter(peerDispatcher);
    EXPECT_EQ(masterAdapter.GetDescriptor()->method_count(), 7);
    EXPECT_EQ(peerAdapter.GetDescriptor()->method_count(), 2);
    const RpcRequestProof missing;
    worker::RpcTenantResolver rejected(missing);
    SyncPubNodeReqPb publisher;
    SyncPubNodeRspPb published;
    EXPECT_EQ(master.SyncPubNode(publisher, published).GetCode(), K_NOT_AUTHORIZED);
    EXPECT_EQ(master.SyncPubNodeWithTenantResolver(publisher, published, rejected).GetCode(), K_NOT_AUTHORIZED);
    DelStreamContextReqPb erase;
    DelStreamContextRspPb erased;
    EXPECT_EQ(master.DelStreamContext(erase, erased).GetCode(), K_NOT_AUTHORIZED);
    EXPECT_EQ(master.DelStreamContextWithTenantResolver(erase, erased, rejected).GetCode(), K_NOT_AUTHORIZED);
    ClearRemoteInfoReqPb clear;
    ClearRemoteInfoRspPb cleared;
    EXPECT_TRUE(master.ClearAllRemoteConsumer(clear, cleared).IsOk());
    EXPECT_TRUE(masterDispatcher.ClearAllRemoteConsumer(missing, clear, cleared).IsOk());
    PushReqPb push;
    PushRspPb pushed;
    ReadBeforeAuth(peer, push, pushed, &WorkerWorkerSCServiceImpl::PushElementsCursors,
                   &WorkerWorkerSCServiceImpl::PushElementsCursorsWithTenantResolver, rejected, K_NOT_AUTHORIZED);
    SharedPagePushReqPb pages;
    ReadBeforeAuth(peer, pages, pushed, &WorkerWorkerSCServiceImpl::PushSharedPageCursors,
                   &WorkerWorkerSCServiceImpl::PushSharedPageCursorsWithTenantResolver, rejected, K_NOT_AUTHORIZED);
    for (bool useRpc : {false, true}) {
        brpc::Controller controller;
        controller.request_attachment().append("x", 1);
        auto api = std::make_shared<BrpcServerUnaryWriterReader<PushRspPb, PushReqPb>>(
            &controller, &push, &pushed, nullptr, "ScPayloadAdmission");
        const auto rc = useRpc ? peer.PushElementsCursorsWithTenantResolver(api, rejected) : peer.PushElementsCursors(api);
        EXPECT_EQ(rc.GetCode(), K_RUNTIME_ERROR);
        EXPECT_NE(rc.GetMsg().find("Malformed payload"), std::string::npos);
    }
}
}  // namespace test
}  // namespace datasystem
