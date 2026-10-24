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

#include "datasystem/worker/rpc_tenant_resolver.h"

#include "datasystem/common/rpc/brpc_auth.h"
#include "datasystem/worker/authenticate.h"

namespace datasystem {
namespace worker {

Status RpcTenantResolver::Verify(AkSkManager &manager) const
{
    return VerifyRpcRequest(manager, &proof_);
}

Status RpcTenantResolver::Request(const std::shared_ptr<AkSkManager> &manager, const std::string &requestTenant,
                                   const std::string &token, std::string &tenant) const
{
    if (FLAGS_skip_authenticate) {
        return CheckTenantId(requestTenant);
    }
    CHECK_FAIL_RETURN_STATUS(manager != nullptr, K_INVALID, "AK/SK manager has not been initialized.");
    const bool tenantEnabled = TenantAuthManager::Instance()->AuthEnabled();
    if (tenantEnabled) {
        if (!token.empty()) {
            RETURN_IF_NOT_OK_PRINT_ERROR_MSG(TenantAuthManager::Instance()->TenantTokenAuth(token, tenant),
                                             "worker authenticate token failed");
            return Status::OK();
        } else if (!proof_.signature.empty() && manager->IsTenantAk(proof_.accessKey)) {
            RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
                TenantAuthManager::Instance()->TenantAkAuth(proof_.accessKey, requestTenant, tenant),
                "worker authenticate accesskey failed");
            CHECK_FAIL_RETURN_STATUS(tenant == requestTenant, K_NOT_AUTHORIZED, "Authentication failed.");
            RETURN_IF_NOT_OK_PRINT_ERROR_MSG(VerifyRpcRequest(*manager, &proof_),
                                             "tenant authenticate AK/SK signature failed");
            return Status::OK();
        }
    }
    const bool systemEnabled = manager->SystemAuthEnabled();
    if (systemEnabled && !proof_.signature.empty()) {
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(VerifyRpcRequest(*manager, &proof_), "AK/SK failed");
    } else if (tenantEnabled || systemEnabled) {
        RETURN_STATUS(K_RUNTIME_ERROR, "AK/SK or token not provide");
    }
    tenant = requestTenant;
    return Status::OK();
}

Status RpcTenantResolver::Session(const std::shared_ptr<AkSkManager> &manager, const std::string &requestTenant,
                                   const std::string &token, const std::string &clientId, std::string &tenant) const
{
    if (FLAGS_skip_authenticate) {
        return CheckTenantId(requestTenant);
    }
    return LookupSession(manager, requestTenant, token, ClientKey::Intern(clientId), tenant);
}

Status RpcTenantResolver::SessionForClient(const std::shared_ptr<AkSkManager> &manager,
                                           const std::string &requestTenant, const std::string &token,
                                           const ClientKey &client, std::string &tenant) const
{
    if (FLAGS_skip_authenticate) {
        return CheckTenantId(requestTenant);
    }
    return LookupSession(manager, requestTenant, token, client, tenant);
}

Status RpcTenantResolver::LookupSession(const std::shared_ptr<AkSkManager> &manager, const std::string &requestTenant,
                                        const std::string &token, const ClientKey &client, std::string &tenant) const
{
    Timer timer;
    std::string effectiveTenant = requestTenant;
    auto info = ClientManager::Instance().GetClientInfo(client);
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(info != nullptr, K_CLIENT_WORKER_DISCONNECT,
                                         FormatString("The client id (%s) does not exist.", client));
    if (effectiveTenant.empty()) {
        effectiveTenant = info->GetTenantId();
    }
    RETURN_IF_NOT_OK(Request(manager, effectiveTenant, token, tenant));
    GetWorkerTimeCost().Append("Authenticate", timer.ElapsedMilliSecond());
    return Status::OK();
}
}  // namespace worker
}  // namespace datasystem
