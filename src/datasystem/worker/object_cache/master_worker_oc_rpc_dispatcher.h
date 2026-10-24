/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
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


#ifndef DATASYSTEM_WORKER_OBJECT_CACHE_MASTER_WORKER_OC_RPC_DISPATCHER_H
#define DATASYSTEM_WORKER_OBJECT_CACHE_MASTER_WORKER_OC_RPC_DISPATCHER_H

#include <utility>

#include "datasystem/worker/object_cache/master_worker_oc_service_impl.h"
#include "datasystem/worker/rpc_tenant_resolver.h"

namespace datasystem {
namespace object_cache {
class MasterWorkerOCServiceRpcDispatcher final {
public:
    explicit MasterWorkerOCServiceRpcDispatcher(MasterWorkerOCServiceImpl &service) : service_(service) {}
    ~MasterWorkerOCServiceRpcDispatcher() = default;
    MasterWorkerOCServiceRpcDispatcher(const MasterWorkerOCServiceRpcDispatcher &) = delete;
    MasterWorkerOCServiceRpcDispatcher &operator=(const MasterWorkerOCServiceRpcDispatcher &) = delete;

#define DS_MW_RPC_AUTH_METHOD(name) \
    template <typename... Args> \
    Status name(const RpcRequestProof &proof, Args &&...args) \
    { \
        const worker::RpcTenantResolver resolver(proof); \
        return service_.name##WithTenantResolver(std::forward<Args>(args)..., resolver); \
    }
#define DS_MW_RPC_DIRECT_METHOD(name) \
    template <typename... Args> \
    Status name(const RpcRequestProof &, Args &&...args) \
    { \
        return service_.name(std::forward<Args>(args)...); \
    }
    DS_MW_RPC_AUTH_METHOD(PublishMeta)
    DS_MW_RPC_DIRECT_METHOD(ClearData)
    DS_MW_RPC_DIRECT_METHOD(MetaChange)
    DS_MW_RPC_AUTH_METHOD(UpdateNotification)
    DS_MW_RPC_AUTH_METHOD(DeleteNotification)
    DS_MW_RPC_AUTH_METHOD(DeletePersistenceObject)
    DS_MW_RPC_AUTH_METHOD(QueryGlobalRefNumOnWorker)
    DS_MW_RPC_AUTH_METHOD(PushMetaToWorker)
    DS_MW_RPC_AUTH_METHOD(RequestMetaFromWorker)
    DS_MW_RPC_AUTH_METHOD(ChangePrimaryCopy)
    DS_MW_RPC_AUTH_METHOD(NotifyMasterIncNestedRefs)
    DS_MW_RPC_AUTH_METHOD(NotifyMasterDecNestedRefs)
#undef DS_MW_RPC_DIRECT_METHOD
#undef DS_MW_RPC_AUTH_METHOD
private:
    MasterWorkerOCServiceImpl &service_;
};
}  // namespace object_cache
}  // namespace datasystem
#endif  // DATASYSTEM_WORKER_OBJECT_CACHE_MASTER_WORKER_OC_RPC_DISPATCHER_H
