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


#ifndef DATASYSTEM_MASTER_OBJECT_CACHE_MASTER_OC_RPC_DISPATCHER_H
#define DATASYSTEM_MASTER_OBJECT_CACHE_MASTER_OC_RPC_DISPATCHER_H

#include <utility>

#include "datasystem/common/rpc/brpc_auth.h"
#include "datasystem/master/object_cache/master_oc_service_impl.h"

namespace datasystem {
namespace master {
class MasterOCServiceRpcDispatcher final {
public:
    explicit MasterOCServiceRpcDispatcher(MasterOCServiceImpl &service) : service_(service) {}
    ~MasterOCServiceRpcDispatcher() = default;
    MasterOCServiceRpcDispatcher(const MasterOCServiceRpcDispatcher &) = delete;
    MasterOCServiceRpcDispatcher &operator=(const MasterOCServiceRpcDispatcher &) = delete;

#define DS_MASTER_RPC_AUTH_METHOD(name) \
    template <typename... Args> \
    Status name(const RpcRequestProof &proof, Args &&...args) \
    { \
        return service_.name##WithRpcAuth(std::forward<Args>(args)..., proof); \
    }
#define DS_MASTER_RPC_DIRECT_METHOD(name) \
    template <typename... Args> \
    Status name(const RpcRequestProof &, Args &&...args) \
    { \
        return service_.name(std::forward<Args>(args)...); \
    }
    DS_MASTER_RPC_AUTH_METHOD(CreateMeta)
    DS_MASTER_RPC_AUTH_METHOD(CreateCopyMeta)
    DS_MASTER_RPC_AUTH_METHOD(CreateMultiCopyMeta)
    DS_MASTER_RPC_AUTH_METHOD(QueryMeta)
    DS_MASTER_RPC_AUTH_METHOD(RemoveMeta)
    DS_MASTER_RPC_AUTH_METHOD(DeleteAllCopyMeta)
    DS_MASTER_RPC_AUTH_METHOD(UpdateMeta)
    DS_MASTER_RPC_AUTH_METHOD(GetObjectLocations)
    DS_MASTER_RPC_AUTH_METHOD(GIncreaseRef)
    DS_MASTER_RPC_AUTH_METHOD(GDecreaseRef)
    DS_MASTER_RPC_AUTH_METHOD(QueryGlobalRefNum)
    DS_MASTER_RPC_DIRECT_METHOD(IncrSeqNoMeta)
    DS_MASTER_RPC_DIRECT_METHOD(GetSeqNoMeta)
    DS_MASTER_RPC_DIRECT_METHOD(DelSeqNoMeta)
    DS_MASTER_RPC_AUTH_METHOD(PushMetaToMaster)
    DS_MASTER_RPC_AUTH_METHOD(RollbackSeal)
    DS_MASTER_RPC_AUTH_METHOD(GIncNestedRef)
    DS_MASTER_RPC_AUTH_METHOD(GDecNestedRef)
    DS_MASTER_RPC_AUTH_METHOD(ReconcileMembershipChange)
    DS_MASTER_RPC_AUTH_METHOD(MigrateMetadata)
    DS_MASTER_RPC_AUTH_METHOD(GIncreaseMasterAppRef)
    DS_MASTER_RPC_AUTH_METHOD(ReleaseGRefs)
    DS_MASTER_RPC_AUTH_METHOD(ReleaseGRefsOfRemoteClientId)
    DS_MASTER_RPC_AUTH_METHOD(CreateMultiMeta)
    DS_MASTER_RPC_AUTH_METHOD(ReportResource)
    DS_MASTER_RPC_AUTH_METHOD(ReportRebalanceResult)
    DS_MASTER_RPC_AUTH_METHOD(StartEvictionPolicyUpdate)
    DS_MASTER_RPC_AUTH_METHOD(GetEvictionPolicyUpdateProgress)
    DS_MASTER_RPC_AUTH_METHOD(LivenessCheck)
    DS_MASTER_RPC_AUTH_METHOD(PutP2PMeta)
    DS_MASTER_RPC_AUTH_METHOD(SubscribeReceiveEvent)
    DS_MASTER_RPC_AUTH_METHOD(GetP2PMeta)
    DS_MASTER_RPC_AUTH_METHOD(SendRootInfo)
    DS_MASTER_RPC_AUTH_METHOD(RecvRootInfo)
    DS_MASTER_RPC_AUTH_METHOD(GetDataInfo)
    DS_MASTER_RPC_AUTH_METHOD(AckRecvFinish)
    DS_MASTER_RPC_AUTH_METHOD(RemoveP2PLocation)
    DS_MASTER_RPC_AUTH_METHOD(ReleaseMetaData)
    DS_MASTER_RPC_AUTH_METHOD(ReplacePrimary)
    DS_MASTER_RPC_AUTH_METHOD(PureQueryMeta)
    DS_MASTER_RPC_AUTH_METHOD(RollbackMultiMeta)
    DS_MASTER_RPC_AUTH_METHOD(Expire)
    DS_MASTER_RPC_AUTH_METHOD(GetMetaInfo)
    DS_MASTER_RPC_AUTH_METHOD(CheckObjectDataLocation)
#undef DS_MASTER_RPC_DIRECT_METHOD
#undef DS_MASTER_RPC_AUTH_METHOD
private:
    MasterOCServiceImpl &service_;
};
}  // namespace master
}  // namespace datasystem
#endif  // DATASYSTEM_MASTER_OBJECT_CACHE_MASTER_OC_RPC_DISPATCHER_H
