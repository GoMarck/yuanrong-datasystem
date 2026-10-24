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

#ifndef DATASYSTEM_WORKER_RPC_TENANT_RESOLVER_H
#define DATASYSTEM_WORKER_RPC_TENANT_RESOLVER_H

#include <memory>
#include <string>

#include "datasystem/utils/status.h"

namespace datasystem {
class AkSkManager;
struct RpcRequestProof;
namespace worker {

// The resolver borrows proof for synchronous admission; queued work must own its proof.
class RpcTenantResolver {
public:
    explicit RpcTenantResolver(const RpcRequestProof &proof) : proof_(proof) {}
    RpcTenantResolver(RpcRequestProof &&) = delete;
    RpcTenantResolver(const RpcRequestProof &&) = delete;
    RpcTenantResolver(const RpcTenantResolver &) = delete;
    RpcTenantResolver &operator=(const RpcTenantResolver &) = delete;

    Status Request(const std::shared_ptr<AkSkManager> &manager, const std::string &requestTenant,
                    const std::string &token, std::string &tenant) const;
    Status Session(const std::shared_ptr<AkSkManager> &manager, const std::string &requestTenant,
                    const std::string &token, const std::string &clientId, std::string &tenant) const;

    template <typename Request>
    Status Resolve(const std::shared_ptr<AkSkManager> &manager, const Request &request, std::string &tenant) const
    {
        return Session(manager, request.tenant_id(), request.token(), request.client_id(), tenant);
    }

private:
    const RpcRequestProof &proof_;
};
}  // namespace worker
}  // namespace datasystem
#endif  // DATASYSTEM_WORKER_RPC_TENANT_RESOLVER_H
