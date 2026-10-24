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

#include "datasystem/worker/worker_service_impl.h"

#include <unistd.h>

#include "datasystem/common/constants.h"
#include "datasystem/common/flags/common_flags.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/log/log_sampler.h"
#include "datasystem/common/os_transport_pipeline/os_transport_pipeline_worker_api.h"
#include "datasystem/common/shared_memory/allocator.h"
#include "datasystem/common/util/compatibility_manager.h"
#include "datasystem/common/util/fd_pass.h"
#include "datasystem/common/util/file_util.h"
#include "datasystem/common/util/format.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/strings_util.h"
#include "datasystem/common/util/uuid_generator.h"
#include "datasystem/common/util/version.h"
#include "datasystem/worker/client_manager/client_manager.h"
#include "datasystem/worker/rpc_tenant_resolver.h"
#include "datasystem/worker/worker_health_check.h"

DS_DECLARE_bool(ipc_through_shared_memory);

namespace datasystem {
namespace worker {
#define DS_WORKER_RPC_ENTRY(name, ...) name##WithTenantResolver(__VA_ARGS__, const RpcTenantResolver &resolver)
#define DS_WORKER_RPC_FORWARD(name, ...) name##WithTenantResolver(__VA_ARGS__, resolver)
#define DS_RESOLVE_WORKER_TENANT(manager, request, requested, resolved) \
    resolver.Request(manager, requested, request.token(), resolved)
#include "datasystem/worker/worker_service_body.inc"
#undef DS_RESOLVE_WORKER_TENANT
#undef DS_WORKER_RPC_FORWARD
#undef DS_WORKER_RPC_ENTRY
}  // namespace worker
}  // namespace datasystem
