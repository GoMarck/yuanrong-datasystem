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

/**
 * Description: Implement the stream cache services on the master.
 */
#ifndef DATASYSTEM_MASTER_STREAM_CACHE_MASTER_SC_RPC_DISPATCHER_H
#define DATASYSTEM_MASTER_STREAM_CACHE_MASTER_SC_RPC_DISPATCHER_H

#include <utility>

#include "datasystem/common/rpc/brpc_auth.h"
#include "datasystem/master/stream_cache/master_sc_service_impl.h"

namespace datasystem {
namespace master {
class MasterSCServiceRpcDispatcher final {
public:
    explicit MasterSCServiceRpcDispatcher(MasterSCServiceImpl &service) : service_(service) {}
    ~MasterSCServiceRpcDispatcher() = default;
    MasterSCServiceRpcDispatcher(const MasterSCServiceRpcDispatcher &) = delete;
    MasterSCServiceRpcDispatcher &operator=(const MasterSCServiceRpcDispatcher &) = delete;

#define DS_MASTER_SC_RPC_METHOD(name) \
    template <typename... Args> \
    Status name(const RpcRequestProof &proof, Args &&...args) \
    { \
        return service_.name##WithRpcAuth(std::forward<Args>(args)..., proof); \
    }
    DS_MASTER_SC_RPC_METHOD(CreateProducer)
    DS_MASTER_SC_RPC_METHOD(CloseProducer)
    DS_MASTER_SC_RPC_METHOD(Subscribe)
    DS_MASTER_SC_RPC_METHOD(CloseConsumer)
    DS_MASTER_SC_RPC_METHOD(DeleteStream)
    DS_MASTER_SC_RPC_METHOD(QueryGlobalProducersNum)
    DS_MASTER_SC_RPC_METHOD(QueryGlobalConsumersNum)
    DS_MASTER_SC_RPC_METHOD(MigrateSCMetadata)
#undef DS_MASTER_SC_RPC_METHOD
private:
    MasterSCServiceImpl &service_;
};
}  // namespace master
}  // namespace datasystem
#endif  // DATASYSTEM_MASTER_STREAM_CACHE_MASTER_SC_RPC_DISPATCHER_H
