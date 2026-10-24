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


#ifndef DATASYSTEM_WORKER_STREAM_CACHE_WORKER_WORKER_SC_SERVICE_RPC_DISPATCHER_H
#define DATASYSTEM_WORKER_STREAM_CACHE_WORKER_WORKER_SC_SERVICE_RPC_DISPATCHER_H

#include <utility>

#include "datasystem/common/rpc/brpc_auth.h"
#include "datasystem/worker/stream_cache/worker_worker_sc_service_impl.h"
#include "datasystem/worker/rpc_tenant_resolver.h"

namespace datasystem {
namespace worker {
namespace stream_cache {
class WorkerWorkerSCServiceRpcDispatcher final {
public:
    explicit WorkerWorkerSCServiceRpcDispatcher(WorkerWorkerSCServiceImpl &service) : service_(service) {}
    ~WorkerWorkerSCServiceRpcDispatcher() = default;
    WorkerWorkerSCServiceRpcDispatcher(const WorkerWorkerSCServiceRpcDispatcher &) = delete;
    WorkerWorkerSCServiceRpcDispatcher &operator=(const WorkerWorkerSCServiceRpcDispatcher &) = delete;

#define DS_PEER_SC_RPC_AUTH_METHOD(name) \
    template <typename... Args> \
    Status name(const RpcRequestProof &proof, Args &&...args) \
    { \
        const RpcTenantResolver resolver(proof); \
        return service_.name##WithTenantResolver(std::forward<Args>(args)..., resolver); \
    }
#define DS_PEER_SC_RPC_DIRECT_METHOD(name) \
    template <typename... Args> \
    Status name(const RpcRequestProof &, Args &&...args) \
    { \
        return service_.name(std::forward<Args>(args)...); \
    }
    DS_PEER_SC_RPC_AUTH_METHOD(PushElementsCursors)
    DS_PEER_SC_RPC_AUTH_METHOD(PushSharedPageCursors)
#undef DS_PEER_SC_RPC_DIRECT_METHOD
#undef DS_PEER_SC_RPC_AUTH_METHOD
private:
    WorkerWorkerSCServiceImpl &service_;
};
}  // namespace stream_cache
}  // namespace worker
}  // namespace datasystem
#endif  // DATASYSTEM_WORKER_STREAM_CACHE_WORKER_WORKER_SC_SERVICE_RPC_DISPATCHER_H
