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

#ifndef DATASYSTEM_WORKER_OBJECT_CACHE_WORKER_OC_SERVICE_RPC_DISPATCHER_H
#define DATASYSTEM_WORKER_OBJECT_CACHE_WORKER_OC_SERVICE_RPC_DISPATCHER_H

#include <utility>

#include "datasystem/worker/object_cache/worker_oc_service_impl.h"
#include "datasystem/worker/rpc_tenant_resolver.h"

namespace datasystem {
namespace object_cache {
class WorkerOCServiceRpcDispatcher final {
public:
    explicit WorkerOCServiceRpcDispatcher(WorkerOCServiceImpl &service) : service_(service) {}
    ~WorkerOCServiceRpcDispatcher() = default;
    WorkerOCServiceRpcDispatcher(const WorkerOCServiceRpcDispatcher &) = delete;
    WorkerOCServiceRpcDispatcher &operator=(const WorkerOCServiceRpcDispatcher &) = delete;

#define DS_OC_RPC_TENANT_METHOD(name) \
    template <typename... Args> \
    Status name(const RpcRequestProof &proof, Args &&...args) \
    { \
        const worker::RpcTenantResolver resolver(proof); \
        return service_.name##WithTenantResolver(std::forward<Args>(args)..., resolver); \
    }
#define DS_OC_RPC_DIRECT_METHOD(name) \
    template <typename... Args> \
    Status name(const RpcRequestProof &, Args &&...args) \
    { \
        return service_.name(std::forward<Args>(args)...); \
    }

    DS_OC_RPC_TENANT_METHOD(HealthCheck)
    DS_OC_RPC_TENANT_METHOD(Create)
    DS_OC_RPC_TENANT_METHOD(Publish)
    DS_OC_RPC_TENANT_METHOD(Get)
    DS_OC_RPC_TENANT_METHOD(QueryAndGet)
    DS_OC_RPC_TENANT_METHOD(DecreaseReference)
    DS_OC_RPC_TENANT_METHOD(ReconcileShmRef)
    DS_OC_RPC_TENANT_METHOD(GIncreaseRef)
    DS_OC_RPC_TENANT_METHOD(GDecreaseRef)
    DS_OC_RPC_TENANT_METHOD(DeleteAllCopy)
    DS_OC_RPC_TENANT_METHOD(InvalidateBuffer)
    DS_OC_RPC_TENANT_METHOD(ReleaseGRefs)
    DS_OC_RPC_DIRECT_METHOD(IncrSeqNo)
    DS_OC_RPC_DIRECT_METHOD(GetSeqNo)
    DS_OC_RPC_DIRECT_METHOD(DelSeqNo)
    DS_OC_RPC_DIRECT_METHOD(Lpush)
    DS_OC_RPC_DIRECT_METHOD(Lpop)
    DS_OC_RPC_DIRECT_METHOD(Lindex)
    DS_OC_RPC_DIRECT_METHOD(Llen)
    DS_OC_RPC_DIRECT_METHOD(Hget)
    DS_OC_RPC_DIRECT_METHOD(Hset)
    DS_OC_RPC_DIRECT_METHOD(Hdel)
    DS_OC_RPC_DIRECT_METHOD(Hgetall)
    DS_OC_RPC_TENANT_METHOD(QueryGlobalRefNum)
    DS_OC_RPC_TENANT_METHOD(PublishDeviceObject)
    DS_OC_RPC_TENANT_METHOD(GetDeviceObject)
    DS_OC_RPC_TENANT_METHOD(MultiPublish)
    DS_OC_RPC_TENANT_METHOD(PutP2PMeta)
    DS_OC_RPC_TENANT_METHOD(SubscribeReceiveEvent)
    DS_OC_RPC_TENANT_METHOD(GetP2PMeta)
    DS_OC_RPC_TENANT_METHOD(SendRootInfo)
    DS_OC_RPC_TENANT_METHOD(RecvRootInfo)
    DS_OC_RPC_TENANT_METHOD(GetDataInfo)
    DS_OC_RPC_TENANT_METHOD(AckRecvFinish)
    DS_OC_RPC_DIRECT_METHOD(RemoveP2PLocation)
    DS_OC_RPC_TENANT_METHOD(GetObjMetaInfo)
    DS_OC_RPC_TENANT_METHOD(MultiCreate)
    DS_OC_RPC_TENANT_METHOD(QuerySize)
    DS_OC_RPC_TENANT_METHOD(Exist)
    DS_OC_RPC_TENANT_METHOD(Expire)
    DS_OC_RPC_TENANT_METHOD(GetMetaInfo)
    DS_OC_RPC_TENANT_METHOD(GetHashRing)

#undef DS_OC_RPC_DIRECT_METHOD
#undef DS_OC_RPC_TENANT_METHOD
private:
    WorkerOCServiceImpl &service_;
};
}  // namespace object_cache
}  // namespace datasystem
#endif  // DATASYSTEM_WORKER_OBJECT_CACHE_WORKER_OC_SERVICE_RPC_DISPATCHER_H
