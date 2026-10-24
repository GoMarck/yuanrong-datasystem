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


#include "datasystem/worker/object_cache/worker_worker_oc_service_impl.h"

#include <algorithm>

#include "datasystem/common/flags/common_flags.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/log/access_recorder.h"
#include "datasystem/common/log/latency_phase.h"
#include "datasystem/common/log/trace.h"
#include "datasystem/common/metrics/kv_metrics.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/strings_util.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/worker/object_cache/worker_oc_service_impl.h"
#include "datasystem/worker/object_cache/worker_worker_peer_state_codec.h"
#include "datasystem/worker/rpc_tenant_resolver.h"

DS_DECLARE_string(worker_address);
DS_DECLARE_int32(oc_worker_worker_direct_port);

namespace datasystem {
namespace object_cache {
namespace {
#define DS_WW_RPC_CONSTANT
#define DS_WW_RPC_HELPERS
#include "datasystem/worker/object_cache/worker_worker_oc_service_body.inc"
#undef DS_WW_RPC_HELPERS
#undef DS_WW_RPC_CONSTANT
}  // namespace

#define DS_WW_RPC_ENTRY(name, ...) \
    name##WithTenantResolver(__VA_ARGS__, const worker::RpcTenantResolver &resolver)
#define DS_WW_RPC_FORWARD(name, ...) name##WithTenantResolver(__VA_ARGS__, resolver)
#define DS_WW_RPC_VERIFY(manager, request) (static_cast<void>(request), resolver.Verify(*manager))
#define DS_WW_RPC_GET
#define DS_WW_RPC_POLICY
#define DS_WW_RPC_BATCH
#include "datasystem/worker/object_cache/worker_worker_oc_service_body.inc"
#undef DS_WW_RPC_BATCH
#undef DS_WW_RPC_POLICY
#undef DS_WW_RPC_GET
#undef DS_WW_RPC_VERIFY
#undef DS_WW_RPC_FORWARD
#undef DS_WW_RPC_ENTRY
}  // namespace object_cache
}  // namespace datasystem
