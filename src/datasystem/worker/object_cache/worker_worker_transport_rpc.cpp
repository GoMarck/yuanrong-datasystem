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


#include "datasystem/worker/object_cache/worker_worker_transport_service_impl.h"
#include "datasystem/worker/rpc_tenant_resolver.h"

namespace datasystem {
namespace object_cache {
#define DS_WWT_RPC_ENTRY(name, ...) \
    name##WithTenantResolver(__VA_ARGS__, const worker::RpcTenantResolver &resolver)
#define DS_WWT_RPC_VERIFY(manager, request) resolver.Verify(*manager)
#include "datasystem/worker/object_cache/worker_worker_transport_body.inc"
#undef DS_WWT_RPC_VERIFY
#undef DS_WWT_RPC_ENTRY
}  // namespace object_cache
}  // namespace datasystem
