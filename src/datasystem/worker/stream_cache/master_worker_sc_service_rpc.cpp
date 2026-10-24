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


#include "datasystem/worker/stream_cache/master_worker_sc_service_impl.h"

#include "datasystem/common/util/format.h"
#include "datasystem/common/log/log_helper.h"
#include "datasystem/common/util/strings_util.h"

#include "datasystem/worker/rpc_tenant_resolver.h"


namespace datasystem {
namespace worker {
namespace stream_cache {
#define DS_MASTER_WORKER_SC_RPC_ENTRY(name, ...) \
    name##WithTenantResolver(__VA_ARGS__, const ::datasystem::worker::RpcTenantResolver &resolver)
#define DS_MASTER_WORKER_SC_RPC_VERIFY(manager, request) (static_cast<void>(request), resolver.Verify(*manager))
#define DS_MASTER_WORKER_SC_RPC_GROUP_0
#define DS_MASTER_WORKER_SC_RPC_GROUP_1
#define DS_MASTER_WORKER_SC_RPC_GROUP_2
#include "datasystem/worker/stream_cache/master_worker_sc_service_body.inc"
#undef DS_MASTER_WORKER_SC_RPC_GROUP_2
#undef DS_MASTER_WORKER_SC_RPC_GROUP_1
#undef DS_MASTER_WORKER_SC_RPC_GROUP_0
#undef DS_MASTER_WORKER_SC_RPC_VERIFY
#undef DS_MASTER_WORKER_SC_RPC_ENTRY
}  // namespace stream_cache
}  // namespace worker
}  // namespace datasystem
