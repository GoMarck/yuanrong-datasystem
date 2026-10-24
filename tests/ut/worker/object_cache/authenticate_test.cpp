/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2023. All rights reserved.
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

/**
 * Description: Test SpillRequestHandler and SpillFileManager.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>

#include "ut/common.h"
#include "datasystem/common/ak_sk/ak_sk_manager.h"
#include "datasystem/common/ak_sk/signature.h"
#include "datasystem/common/constants.h"
#include "datasystem/common/eventloop/timer_queue.h"
#include "datasystem/common/httpclient/curl_http_client.h"
#include "datasystem/common/iam/tenant_auth_manager.h"
#include "datasystem/common/iam/yuanrong_iam.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/util/rpc_util.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/protos/share_memory.pb.h"
#include "datasystem/protos/ut_object.pb.h"
#include "datasystem/utils/sensitive_value.h"
#include "datasystem/utils/status.h"
#include "datasystem/worker/authenticate.h"
#include "datasystem/worker/rpc_tenant_resolver.h"
#include "datasystem/common/rpc/brpc_auth.h"
#include "datasystem/protos/object_posix.pb.h"
#include "datasystem/common/util/raii.h"
#include "../../../common/binmock/binmock.h"

DS_DECLARE_string(spill_directory);
DS_DECLARE_string(iam_kit);
DS_DECLARE_string(yuanrong_iam_url);

using namespace ::testing;

namespace datasystem {
namespace ut {

class AuthenticateTest : public CommonTest {
public:
    void SetUp() override
    {
        FLAGS_v = 1;
        FLAGS_iam_kit = "token_oauth";
        ASSERT_TRUE(TimerQueue::GetInstance()->Initialize());
        datasystem::inject::Set("CurlHttpClient.Send.ReleaseCurlHandle", "100*call");
    }

    Status RegisterReqCheck(std::shared_ptr<AkSkManager> akSkManager, const std::string &akSkTenantId,
                            const SensitiveValue &token, std::string &tenantId)
    {
        tenantId.clear();
        RegisterClientReqPb req;
        req.set_client_id("clientid");
        if (!token.Empty()) {
            req.set_token(token.GetData(), token.GetSize());
        }

        if (!akSkTenantId.empty()) {
            req.set_tenant_id(akSkTenantId);
            akSkManager->GenerateSignature(req);
        }
        auto serializedStr = req.SerializeAsString();
        RETURN_IF_NOT_OK(GetRequestContext()->serializedMessage.CopyBuffer(
            serializedStr.c_str(), serializedStr.size()));

        return worker::AuthenticateRequest(akSkManager, req, req.tenant_id(), tenantId);
    }

    template <typename Req>
    Status GenerateSignature(std::shared_ptr<AkSkManager> akSkManager, Req &req)
    {
        RETURN_IF_NOT_OK(akSkManager->GenerateSignature(req));
        auto serializedStr = req.SerializeAsString();
        return GetRequestContext()->serializedMessage.CopyBuffer(serializedStr.c_str(), serializedStr.size());
    }

    template <typename Req>
    Status VerifySignature(std::shared_ptr<AkSkManager> akSkManager, Req &req, std::string &tenantId)
    {
        return worker::AuthenticateRequest(akSkManager, req, req.tenant_id(), tenantId);
    }

protected:
    std::string accessKey_ = "QTWAOYTTINDUT2QVKYUC";
    std::string secretKey_ = "MFyfvK41ba2giqM7**********KGpownRZlmVmHc";
};

TEST_F(AuthenticateTest, RpcTenantResolverUsesOwnedProofAndPreservesTokenPriority)
{
    const bool skip = FLAGS_skip_authenticate;
    const auto iamUrl = FLAGS_yuanrong_iam_url;
    FLAGS_skip_authenticate = false;
    FLAGS_yuanrong_iam_url = "http://127.0.0.1:1";
    Raii restore([skip, iamUrl] {
        FLAGS_skip_authenticate = skip;
        FLAGS_yuanrong_iam_url = iamUrl;
        EXPECT_TRUE(TenantAuthManager::Instance()->Init(false).IsOk());
    });
    auto manager = std::make_shared<AkSkManager>();
    ASSERT_TRUE(manager->SetServerAkSk(AkSkType::SYSTEM, "test-only-access",
                                       SensitiveValue("test-only-secret")).IsOk());
    Signature provider("test-only-access", SensitiveValue("test-only-secret"));
    AkSkData key;
    ASSERT_TRUE(provider.SnapshotClientKey(key).IsOk());
    RpcRequestProof proof;
    proof.captured = true;
    proof.timestampUs = GetSystemClockTimeStampUs();
    proof.accessKey = key.accessKey;
    ASSERT_TRUE(Signature::GetSignatureFromDigest(key, proof.digest, proof.signature).IsOk());
    worker::RpcTenantResolver resolver(proof);
    ASSERT_TRUE(TenantAuthManager::Instance()->Init(false).IsOk());
    std::string tenant;
    ASSERT_TRUE(resolver.Request(manager, "test-tenant", "", tenant).IsOk());
    EXPECT_EQ(tenant, "test-tenant");
    proof.signature.front() ^= 1;
    EXPECT_EQ(resolver.Request(manager, "test-tenant", "", tenant).GetCode(), K_NOT_AUTHORIZED);

    ASSERT_TRUE(TenantAuthManager::Instance()->Init(true, manager).IsOk());
    ASSERT_TRUE(inject::Set("worker.auth", "return(test-only-token,test-token-tenant)").IsOk());
    ASSERT_TRUE(resolver.Request(manager, "test-tenant", "test-only-token", tenant).IsOk());
    EXPECT_EQ(tenant, "test-token-tenant");

    proof.signature.front() ^= 1;
    Raii releaseStubs([] { RELEASE_STUBS });
    BINEXPECT_CALL(&TenantAuthManager::TenantTokenAuth, (_, _))
        .WillOnce([](const SensitiveValue &, std::string &) {
            return Status(K_NOT_AUTHORIZED, "test token rejected");
        });
    EXPECT_EQ(resolver.Request(manager, "test-tenant", "test-rejected-token", tenant).GetCode(), K_NOT_AUTHORIZED);
    const RpcRequestProof missing;
    worker::RpcTenantResolver missingResolver(missing);
    EXPECT_EQ(missingResolver.Request(manager, "test-tenant", "", tenant).GetCode(), K_RUNTIME_ERROR);

    auto tenantManager = std::make_shared<AkSkManager>();
    ASSERT_TRUE(tenantManager->SetServerAkSk(AkSkType::TENANT, key.accessKey, key.secretKey).IsOk());
    ASSERT_TRUE(TenantAuthManager::Instance()->Init(true, tenantManager).IsOk());
    BINEXPECT_CALL(&TenantAuthManager::TenantAkAuth, (_, _, _))
        .WillOnce([](const std::string &, const std::string &requested, std::string &resolved) {
            resolved = requested;
            return Status::OK();
        })
        .WillOnce([](const std::string &, const std::string &, std::string &resolved) {
            resolved = "test-only-other-tenant";
            return Status::OK();
        });
    ASSERT_TRUE(resolver.Request(tenantManager, "test-tenant", "", tenant).IsOk());
    EXPECT_EQ(tenant, "test-tenant");
    EXPECT_EQ(resolver.Request(tenantManager, "test-tenant", "", tenant).GetCode(), K_NOT_AUTHORIZED);
}

TEST_F(AuthenticateTest, RpcTenantResolverKeepsSkipAndSessionErrorOrder)
{
    const bool skip = FLAGS_skip_authenticate;
    Raii restore([skip] {
        FLAGS_skip_authenticate = skip;
        EXPECT_TRUE(TenantAuthManager::Instance()->Init(false).IsOk());
    });
    const RpcRequestProof missing;
    worker::RpcTenantResolver resolver(missing);
    auto manager = std::make_shared<AkSkManager>();
    ASSERT_TRUE(TenantAuthManager::Instance()->Init(false).IsOk());
    FLAGS_skip_authenticate = false;
    std::string tenant;
    ASSERT_TRUE(resolver.Request(manager, "test-tenant", "", tenant).IsOk());
    EXPECT_EQ(tenant, "test-tenant");
    const std::string client = "test-only-missing-client";
    EXPECT_EQ(resolver.Session(nullptr, "", "", client, tenant).GetCode(), K_CLIENT_WORKER_DISCONNECT);
    const auto explicitClient = ClientKey::Intern(client);
    EXPECT_EQ(resolver.SessionForClient(nullptr, "", "", explicitClient, tenant).GetCode(), K_CLIENT_WORKER_DISCONNECT);
    Raii releaseStubs([] { RELEASE_STUBS });
    auto clientInfo = std::make_shared<worker::ClientInfo>(-1, ClientKey::Intern("test-only-session"), true,
                                                         false, "test-session-tenant");
    BINEXPECT_CALL(&worker::ClientManager::GetClientInfo, (_))
        .WillOnce([clientInfo](const ClientKey &) { return clientInfo; })
        .WillOnce([clientInfo](const ClientKey &) { return clientInfo; })
        .WillOnce([clientInfo](const ClientKey &) { return clientInfo; })
        .WillOnce([clientInfo](const ClientKey &client) {
            EXPECT_EQ(client, ClientKey::Intern("test-only-session"));
            return clientInfo;
        });
    ASSERT_TRUE(resolver.Session(manager, "", "", "test-only-session", tenant).IsOk());
    EXPECT_EQ(tenant, "test-session-tenant");
    ASSERT_TRUE(resolver.Session(manager, "test-request-tenant", "", "test-only-session", tenant).IsOk());
    EXPECT_EQ(tenant, "test-request-tenant");
    EXPECT_EQ(resolver.Session(nullptr, "", "", "test-only-session", tenant).GetCode(), K_INVALID);
    ASSERT_TRUE(resolver.SessionForClient(manager, "", "", ClientKey::Intern("test-only-session"), tenant).IsOk());
    EXPECT_EQ(tenant, "test-session-tenant");
    FLAGS_skip_authenticate = true;
    EXPECT_TRUE(resolver.Session(nullptr, "", "", client, tenant).IsOk());
    EXPECT_TRUE(resolver.SessionForClient(nullptr, "", "", explicitClient, tenant).IsOk());
    EXPECT_EQ(resolver.Session(nullptr, "test-tenant", "", client, tenant).GetCode(), K_INVALID);
}

TEST_F(AuthenticateTest, TestEnableAll)
{
    auto akSkManager = std::make_shared<AkSkManager>();
    akSkManager->SetClientAkSk(accessKey_, secretKey_);
    akSkManager->SetServerAkSk(AkSkType::TENANT, accessKey_, secretKey_);
    DS_ASSERT_OK(inject::Set("worker.auth", "return(token1,tenant1)"));
    TenantAuthManager::Instance()->Init(true);
    std::string akSkTenantId = "test_tenant";
    std::string token = "token1";
    std::string tenantId;
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, akSkTenantId, token, tenantId));
    ASSERT_EQ(tenantId, "tenant1");
    DS_ASSERT_NOT_OK(RegisterReqCheck(akSkManager, akSkTenantId, "", tenantId));
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, "", token, tenantId));
    ASSERT_EQ(tenantId, "tenant1");
    DS_ASSERT_NOT_OK(RegisterReqCheck(akSkManager, "", "", tenantId));
}

TEST_F(AuthenticateTest, TestEnableAKSK)
{
    auto akSkManager = std::make_shared<AkSkManager>();
    akSkManager->SetClientAkSk(accessKey_, secretKey_);
    akSkManager->SetServerAkSk(AkSkType::TENANT, accessKey_, secretKey_);
    DS_ASSERT_OK(inject::Set("worker.auth", "return(token1,tenant1)"));
    TenantAuthManager::Instance()->Init(false);

    std::string akSkTenantId = "test_tenant";
    std::string token = "token1";
    std::string tenantId;

    DS_ASSERT_OK(RegisterReqCheck(akSkManager, akSkTenantId, token, tenantId));
    ASSERT_EQ(tenantId, akSkTenantId);
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, akSkTenantId, "", tenantId));
    ASSERT_EQ(tenantId, akSkTenantId);
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, "", token, tenantId));
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, "", "", tenantId));
}

TEST_F(AuthenticateTest, TestSystemAKSK)
{
    auto akSkManager = std::make_shared<AkSkManager>();
    akSkManager->SetClientAkSk(accessKey_, secretKey_);
    akSkManager->SetServerAkSk(AkSkType::SYSTEM, accessKey_, secretKey_);
    DS_ASSERT_OK(inject::Set("worker.auth", "return(token1,tenant1)"));
    TenantAuthManager::Instance()->Init(true);

    std::string akSkTenantId = "test_tenant";
    std::string token = "token1";
    std::string tenantId;

    DS_ASSERT_OK(RegisterReqCheck(akSkManager, akSkTenantId, token, tenantId));
    ASSERT_EQ(tenantId, "tenant1");
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, akSkTenantId, "", tenantId));
    ASSERT_EQ(tenantId, akSkTenantId);
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, "", token, tenantId));
    ASSERT_EQ(tenantId, "tenant1");
    DS_ASSERT_NOT_OK(RegisterReqCheck(akSkManager, "", "", tenantId));
}

TEST_F(AuthenticateTest, TestDisableAll)
{
    auto akSkManager = std::make_shared<AkSkManager>();
    akSkManager->SetClientAkSk(accessKey_, secretKey_);

    DS_ASSERT_OK(inject::Set("worker.auth", "return(token1,tenant1)"));
    TenantAuthManager::Instance()->Init(false);

    std::string akSkTenantId = "test_tenant";
    std::string token = "token1";
    std::string tenantId;

    DS_ASSERT_OK(RegisterReqCheck(akSkManager, akSkTenantId, token, tenantId));
    ASSERT_EQ(tenantId, akSkTenantId);
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, akSkTenantId, "", tenantId));
    ASSERT_EQ(tenantId, akSkTenantId);
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, "", token, tenantId));
    ASSERT_EQ(tenantId, "");
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, "", "", tenantId));
    ASSERT_EQ(tenantId, "");
}

TEST_F(AuthenticateTest, TestProtobufFilterSensitiveInfo)
{
    PublishReqPb req;
    req.set_client_id("clientId");
    req.set_token("1111111111111");
    req.set_signature("1111111111111");
    std::string filterReq = LogHelper::IgnoreSensitive(req);
    auto reqPos = filterReq.find("token: \"***\"");
    ASSERT_TRUE(reqPos != std::string::npos);
    reqPos = filterReq.find("signature: \"***\"");
    ASSERT_TRUE(reqPos != std::string::npos);

    auto originalReq = req.ShortDebugString().find("token: \"1111111111111\"");
    ASSERT_TRUE(originalReq != std::string::npos);

    PublishRspPb resp;
    std::string filterResp = LogHelper::IgnoreSensitive(resp);
    auto respPos = filterResp.find("**");
    ASSERT_TRUE(respPos == std::string::npos);
}

TEST_F(AuthenticateTest, TestEnableInYuanrong)
{
    FLAGS_iam_kit = "yuanrong_iam";
    const std::string accessKey = "accesskey";
    const std::string secretKey = "secretkey";
    auto akSkManager = std::make_shared<AkSkManager>();
    akSkManager->SetClientAkSk(accessKey, secretKey);
    akSkManager->SetServerAkSk(AkSkType::TENANT, accessKey_, secretKey_);
    DS_ASSERT_OK(inject::Set("worker.akauth", "return(accesskey,secretkey,test_tenant)"));
    TenantAuthManager::Instance()->Init(true, akSkManager);
    std::string akSkTenantId = "test_tenant";
    std::string tenantId;
    DS_ASSERT_NOT_OK(RegisterReqCheck(akSkManager, "tenant1", "", tenantId));
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, akSkTenantId, "", tenantId));
    ASSERT_EQ(tenantId, akSkTenantId);
    DS_ASSERT_NOT_OK(RegisterReqCheck(akSkManager, "", "", tenantId));
}

TEST_F(AuthenticateTest, TestSystemRoleAuth)
{
    BINEXPECT_CALL(&CurlHttpClient::SendRequest, (_, _, _))
        .WillOnce([](const std::shared_ptr<HttpRequest> &req, std::shared_ptr<HttpResponse> &rsp, CURL *curlHandle) {
            (void)req;
            (void)curlHandle;
            rsp->SetStatus(HTTP_OK);
            std::string rspStr = R"({
                "accessKey": "accesskey",
                "dataKey": "dataKey",
                "expiredTimeSpan": "0",
                "expiredTimeStamp": "0",
                "role": "system",
                "secretKey": "secretkey",
                "tenantID": "faas"
            })";
            rsp->GetBody()->write(rspStr.c_str(), rspStr.length());
            return Status::OK();
        });
    FLAGS_iam_kit = "yuanrong_iam";
    FLAGS_yuanrong_iam_url = "http://iam-server.default.svc.cluster.local:31218";
    const std::string accessKey = "accesskey";
    const std::string secretKey = "secretkey";
    auto akSkManager = std::make_shared<AkSkManager>();
    akSkManager->SetClientAkSk(accessKey, secretKey);
    akSkManager->SetServerAkSk(AkSkType::TENANT, accessKey_, secretKey_);
    TenantAuthManager::Instance()->Init(true, akSkManager);
    std::string reqTenantId = "tenant1";
    std::string tenantId;
    // The system role does not verify tenantId.
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, reqTenantId, "", tenantId));
    ASSERT_EQ(tenantId, reqTenantId);
    // Hit clientAkTable_
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, reqTenantId, "", tenantId));
    ASSERT_EQ(tenantId, reqTenantId);
}

TEST_F(AuthenticateTest, TestOnlySystemAKSK)
{
    auto akSkManager = std::make_shared<AkSkManager>();
    akSkManager->SetClientAkSk(accessKey_, secretKey_);
    akSkManager->SetServerAkSk(AkSkType::SYSTEM, accessKey_, secretKey_);
    DS_ASSERT_OK(inject::Set("worker.auth", "return(token1,tenant1)"));
    TenantAuthManager::Instance()->Init(false);

    std::string akSkTenantId = "test_tenant";
    std::string token = "token1";
    std::string tenantId;

    DS_ASSERT_OK(RegisterReqCheck(akSkManager, akSkTenantId, token, tenantId));
    ASSERT_EQ(tenantId, akSkTenantId);
    DS_ASSERT_NOT_OK(RegisterReqCheck(akSkManager, "", token, tenantId));
    DS_ASSERT_OK(RegisterReqCheck(akSkManager, akSkTenantId, "", tenantId));
    ASSERT_EQ(tenantId, akSkTenantId);
    akSkManager->SetClientAkSk("accesskey", "secretKey");
    DS_ASSERT_NOT_OK(RegisterReqCheck(akSkManager, akSkTenantId, token, tenantId));
    akSkManager->SetClientAkSk("", "");
    DS_ASSERT_NOT_OK(RegisterReqCheck(akSkManager, akSkTenantId, token, tenantId));
}

TEST_F(AuthenticateTest, TestAKSKCompatibility)
{
    FLAGS_iam_kit = "yuanrong_iam";
    const std::string accessKey = "accesskey";
    const std::string secretKey = "secretkey";
    auto akSkManager = std::make_shared<AkSkManager>();
    akSkManager->SetClientAkSk(accessKey, secretKey);
    akSkManager->SetServerAkSk(AkSkType::TENANT, accessKey_, secretKey_);
    DS_ASSERT_OK(inject::Set("worker.akauth", "return(accesskey,secretkey,test_tenant)"));
    TenantAuthManager::Instance()->Init(true, akSkManager);
    std::string akSkTenantId = "test_tenant";

    TestReqPbV2 req;
    req.set_filed1(1); // set filed1 as 1.
    req.set_field2("xxx");
    req.set_field3(2);  // set filed3 as 2.
    req.set_tenant_id(akSkTenantId);
    int count = 3;
    for (int i = 0; i < count; ++i) {
        req.mutable_filed4()->Add(std::to_string(i));
    }
    req.set_filed5(1);
    DS_ASSERT_OK(GenerateSignature(akSkManager, req));

    TestReqPbV1 req1;
    ASSERT_TRUE(req1.ParseFromString(req.SerializeAsString()));
    std::string tenantId;
    DS_ASSERT_OK(VerifySignature(akSkManager, req1, tenantId));
    ASSERT_EQ(tenantId, akSkTenantId);
}
}  // namespace ut
}  // namespace datasystem
