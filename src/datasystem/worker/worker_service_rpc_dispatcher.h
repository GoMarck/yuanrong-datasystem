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

#ifndef DATASYSTEM_WORKER_WORKER_SERVICE_RPC_DISPATCHER_H
#define DATASYSTEM_WORKER_WORKER_SERVICE_RPC_DISPATCHER_H

#include "datasystem/worker/rpc_tenant_resolver.h"
#include "datasystem/worker/worker_service_impl.h"

namespace datasystem {
namespace worker {
class WorkerServiceRpcDispatcher final {
public:
    explicit WorkerServiceRpcDispatcher(WorkerServiceImpl &service) : service_(service) {}
    ~WorkerServiceRpcDispatcher() = default;

    Status GetClientFd(const RpcRequestProof &proof, const GetClientFdReqPb &request, GetClientFdRspPb &response)
    {
        const RpcTenantResolver resolver(proof);
        return service_.GetClientFdWithTenantResolver(request, response, resolver);
    }

    Status GetSocketPath(const RpcRequestProof &proof, const GetSocketPathReqPb &request, GetSocketPathRspPb &response)
    {
        const RpcTenantResolver resolver(proof);
        return service_.GetSocketPathWithTenantResolver(request, response, resolver);
    }

    Status RegisterClient(const RpcRequestProof &proof, const RegisterClientReqPb &request, RegisterClientRspPb &response)
    {
        const RpcTenantResolver resolver(proof);
        return service_.RegisterClientWithTenantResolver(request, response, resolver);
    }

    Status DisconnectClient(const RpcRequestProof &proof, const DisconnectClientReqPb &request, DisconnectClientRspPb &response)
    {
        const RpcTenantResolver resolver(proof);
        return service_.DisconnectClientWithTenantResolver(request, response, resolver);
    }

    Status Heartbeat(const RpcRequestProof &proof, const HeartbeatReqPb &request, HeartbeatRspPb &response)
    {
        const RpcTenantResolver resolver(proof);
        return service_.HeartbeatWithTenantResolver(request, response, resolver);
    }

private:
    WorkerServiceImpl &service_;
};
}  // namespace worker
}  // namespace datasystem
#endif  // DATASYSTEM_WORKER_WORKER_SERVICE_RPC_DISPATCHER_H
