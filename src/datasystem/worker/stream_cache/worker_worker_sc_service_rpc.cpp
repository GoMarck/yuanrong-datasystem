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


#include "datasystem/worker/stream_cache/worker_worker_sc_service_impl.h"

#include <functional>
#include <numeric>
#include <utility>

#include "datasystem/worker/stream_cache/stream_manager.h"
#include "datasystem/worker/stream_cache/usage_monitor.h"

#include "datasystem/worker/rpc_tenant_resolver.h"

DS_DECLARE_int32(sc_regular_socket_num);
DS_DECLARE_uint64(sc_local_cache_memory_size_mb);

namespace datasystem {
namespace worker {
namespace stream_cache {
#define DS_PEER_SC_RPC_ENTRY(name, ...) \
    name##WithTenantResolver(__VA_ARGS__, const ::datasystem::worker::RpcTenantResolver &resolver)
#define DS_PEER_SC_RPC_VERIFY(manager, request) (static_cast<void>(request), resolver.Verify(*manager))
#define DS_PEER_SC_RPC_GROUP_0
#define DS_PEER_SC_RPC_GROUP_1
#include "datasystem/worker/stream_cache/worker_worker_sc_service_body.inc"
#undef DS_PEER_SC_RPC_GROUP_1
#undef DS_PEER_SC_RPC_GROUP_0
#undef DS_PEER_SC_RPC_VERIFY
#undef DS_PEER_SC_RPC_ENTRY
}  // namespace stream_cache
}  // namespace worker
}  // namespace datasystem
