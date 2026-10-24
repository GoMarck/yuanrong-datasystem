/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#ifndef DATASYSTEM_WORKER_STREAM_CACHE_CLIENT_WORKER_SC_SERVICE_RPC_DISPATCHER_H
#define DATASYSTEM_WORKER_STREAM_CACHE_CLIENT_WORKER_SC_SERVICE_RPC_DISPATCHER_H

#include <utility>

#include "datasystem/common/rpc/brpc_auth.h"
#include "datasystem/worker/stream_cache/client_worker_sc_service_impl.h"
#include "datasystem/worker/rpc_tenant_resolver.h"

namespace datasystem {
namespace worker {
namespace stream_cache {
class ClientWorkerSCServiceRpcDispatcher final {
public:
    explicit ClientWorkerSCServiceRpcDispatcher(ClientWorkerSCServiceImpl &service) : service_(service) {}
    ~ClientWorkerSCServiceRpcDispatcher() = default;
    ClientWorkerSCServiceRpcDispatcher(const ClientWorkerSCServiceRpcDispatcher &) = delete;
    ClientWorkerSCServiceRpcDispatcher &operator=(const ClientWorkerSCServiceRpcDispatcher &) = delete;

#define DS_CLIENT_SC_RPC_AUTH_METHOD(name) \
    template <typename... Args> \
    Status name(const RpcRequestProof &proof, Args &&...args) \
    { \
        const RpcTenantResolver resolver(proof); \
        return service_.name##WithTenantResolver(std::forward<Args>(args)..., resolver); \
    }
#define DS_CLIENT_SC_RPC_DIRECT_METHOD(name) \
    template <typename... Args> \
    Status name(const RpcRequestProof &, Args &&...args) \
    { \
        return service_.name(std::forward<Args>(args)...); \
    }
    DS_CLIENT_SC_RPC_AUTH_METHOD(CreateProducer)
    DS_CLIENT_SC_RPC_AUTH_METHOD(CloseProducer)
    DS_CLIENT_SC_RPC_AUTH_METHOD(Subscribe)
    DS_CLIENT_SC_RPC_AUTH_METHOD(CloseConsumer)
    DS_CLIENT_SC_RPC_AUTH_METHOD(GetDataPage)
    DS_CLIENT_SC_RPC_AUTH_METHOD(CreateShmPage)
    DS_CLIENT_SC_RPC_AUTH_METHOD(DeleteStream)
    DS_CLIENT_SC_RPC_AUTH_METHOD(QueryGlobalProducersNum)
    DS_CLIENT_SC_RPC_AUTH_METHOD(QueryGlobalConsumersNum)
    DS_CLIENT_SC_RPC_AUTH_METHOD(UnblockProducer)
    DS_CLIENT_SC_RPC_AUTH_METHOD(BlockProducer)
    DS_CLIENT_SC_RPC_AUTH_METHOD(GetLastAppendCursor)
    DS_CLIENT_SC_RPC_AUTH_METHOD(ResetStreams)
    DS_CLIENT_SC_RPC_AUTH_METHOD(ResumeStreams)
    DS_CLIENT_SC_RPC_AUTH_METHOD(AllocBigShmMemory)
    DS_CLIENT_SC_RPC_AUTH_METHOD(ReleaseBigShmMemory)
#undef DS_CLIENT_SC_RPC_DIRECT_METHOD
#undef DS_CLIENT_SC_RPC_AUTH_METHOD
private:
    ClientWorkerSCServiceImpl &service_;
};
}  // namespace stream_cache
}  // namespace worker
}  // namespace datasystem
#endif  // DATASYSTEM_WORKER_STREAM_CACHE_CLIENT_WORKER_SC_SERVICE_RPC_DISPATCHER_H
