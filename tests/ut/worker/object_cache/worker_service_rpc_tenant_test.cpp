/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "datasystem/cluster/model/topology_snapshot.h"
#include "datasystem/common/rpc/brpc_auth.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/timer.h"
#include "datasystem/worker/authenticate.h"
#include "datasystem/worker/rpc_tenant_resolver.h"
#include "datasystem/worker/worker_service_impl.h"
#include "ut/common.h"
#include "../../../common/binmock/binmock.h"

using ::testing::_;

namespace datasystem::worker {
class WorkerServiceRpcTenantTest : public ut::CommonTest {};

TEST_F(WorkerServiceRpcTenantTest, HeartbeatKeepsOneSessionSnapshotAndOriginalFdBoundary)
{
    const bool skip = FLAGS_skip_authenticate;
    FLAGS_skip_authenticate = false;
    Raii restore([skip] {
        FLAGS_skip_authenticate = skip;
        EXPECT_TRUE(TenantAuthManager::Instance()->Init(false).IsOk());
    });
    Raii releaseStubs([] { RELEASE_STUBS });
    ASSERT_TRUE(TenantAuthManager::Instance()->Init(false).IsOk());
    auto manager = std::make_shared<AkSkManager>();
    ASSERT_TRUE(manager->SetServerAkSk(AkSkType::SYSTEM, "test-only-access",
                                       SensitiveValue("test-only-secret")).IsOk());
    Signature provider("test-only-access", SensitiveValue("test-only-secret"));
    AkSkData key;
    ASSERT_TRUE(provider.SnapshotClientKey(key).IsOk());
    RpcRequestProof proof;
    proof.captured = true;
    proof.accessKey = key.accessKey;
    proof.timestampUs = GetSystemClockTimeStampUs();
    ASSERT_TRUE(Signature::GetSignatureFromDigest(key, proof.digest, proof.signature).IsOk());
    RpcTenantResolver resolver(proof);
    cluster::TopologySnapshotState snapshots;
    cluster::MembershipEndpointView membership(snapshots);
    std::atomic<bool> exiting{false};
    WorkerServiceImpl service(HostPort("127.0.0.1", 1), HostPort("127.0.0.1", 2), 1.0,
                              nullptr, manager, "", membership, exiting);
    unsigned phase = 0;
    unsigned lookups = 0;
    unsigned updates = 0;
    unsigned fdCleanups = 0;
    BINEXPECT_CALL(&ClientManager::GetAuthTenantIdByClientId, (_, _))
        .WillRepeatedly([&](const ClientKey &, bool &exist) {
            ++lookups;
            exist = phase != 1;
            return std::string("test-session-tenant");
        });
    BINEXPECT_CALL(&ClientManager::UpdateLastHeartbeat, (_, _))
        .WillRepeatedly([&](const ClientKey &, bool) {
            ++updates;
            return phase == 0 ? Status(K_RUNTIME_ERROR, "test session removed") : Status::OK();
        });
    BINEXPECT_CALL(&ClientManager::DelClientId2WorkerFdMap, (_, _))
        .WillRepeatedly([&](const ClientKey &, const std::vector<int> &) { ++fdCleanups; });
    BINEXPECT_CALL(&WorkerServiceImpl::GetExpiredFdsForClient, (_))
        .WillRepeatedly([](const ClientKey &) { return std::set<int>{}; });
    HeartbeatReqPb request;
    request.set_client_id("test-only-client");
    request.add_released_worker_fds(1);
    HeartbeatRspPb response;
    ASSERT_TRUE(service.HeartbeatWithTenantResolver(request, response, resolver).IsOk());
    EXPECT_TRUE(response.client_removed());
    EXPECT_EQ(fdCleanups, 1u);
    proof.signature.front() ^= 1;
    phase = 1;
    response.Clear();
    ASSERT_TRUE(service.HeartbeatWithTenantResolver(request, response, resolver).IsOk());
    EXPECT_FALSE(response.client_removed());
    EXPECT_EQ(fdCleanups, 1u);
    phase = 2;
    EXPECT_EQ(service.HeartbeatWithTenantResolver(request, response, resolver).GetCode(), K_NOT_AUTHORIZED);
    EXPECT_EQ(lookups, 3u);
    EXPECT_EQ(updates, 2u);
    EXPECT_EQ(fdCleanups, 1u);
}
TEST_F(WorkerServiceRpcTenantTest, RegistrationAndSocketPathKeepOriginalPolicyOrder)
{
    const bool skip = FLAGS_skip_authenticate;
    FLAGS_skip_authenticate = false;
    Raii restore([skip] {
        FLAGS_skip_authenticate = skip;
        EXPECT_TRUE(TenantAuthManager::Instance()->Init(false).IsOk());
    });
    ASSERT_TRUE(TenantAuthManager::Instance()->Init(false).IsOk());
    auto manager = std::make_shared<AkSkManager>();
    const RpcRequestProof missing;
    RpcTenantResolver resolver(missing);
    cluster::TopologySnapshotState snapshots;
    cluster::MembershipEndpointView membership(snapshots);
    std::atomic<bool> exiting{true};
    WorkerServiceImpl service(HostPort("127.0.0.1", 1), HostPort("127.0.0.1", 2), 1.0,
                              nullptr, manager, "", membership, exiting);
    RegisterClientReqPb registration;
    registration.set_client_id("test-only-client");
    RegisterClientRspPb registered;
    EXPECT_EQ(service.RegisterClientWithTenantResolver(registration, registered, resolver).GetCode(), K_NOT_READY);
    ASSERT_TRUE(manager->SetServerAkSk(AkSkType::SYSTEM, "test-only-access",
                                       SensitiveValue("test-only-secret")).IsOk());
    EXPECT_EQ(service.RegisterClientWithTenantResolver(registration, registered, resolver).GetCode(), K_RUNTIME_ERROR);
    GetSocketPathReqPb request;
    GetSocketPathRspPb response;
    EXPECT_EQ(service.GetSocketPathWithTenantResolver(request, response, resolver).GetCode(), K_RUNTIME_ERROR);
    FLAGS_skip_authenticate = true;
    request.set_tenant_id("test-only-tenant");
    EXPECT_EQ(service.GetSocketPathWithTenantResolver(request, response, resolver).GetCode(), K_INVALID);
    request.clear_tenant_id();
    EXPECT_TRUE(service.GetSocketPathWithTenantResolver(request, response, resolver).IsOk());
}
}  // namespace datasystem::worker
