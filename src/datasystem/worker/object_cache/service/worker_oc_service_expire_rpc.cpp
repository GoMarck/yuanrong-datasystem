/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
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

#include "datasystem/worker/object_cache/service/worker_oc_service_expire_impl.h"

#include <cstdint>
#include <utility>


#include "datasystem/common/flags/common_flags.h"
#include "datasystem/common/iam/tenant_auth_manager.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/log/access_recorder.h"
#include "datasystem/common/log/log.h"
#include "datasystem/common/perf/perf_manager.h"
#include "datasystem/common/util/format.h"
#include "datasystem/common/util/meta_route_tool.h"
#include "datasystem/common/util/rpc_util.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/protos/master_object.pb.h"
#include "datasystem/utils/status.h"
#include "datasystem/worker/authenticate.h"
#include "datasystem/worker/object_cache/service/service_execution_policy.h"

using namespace datasystem::master;
#include "datasystem/worker/rpc_tenant_resolver.h"

namespace datasystem {
namespace object_cache {
#define DS_OC_PROC_ENTRY(name, ...) \
    name##WithTenantResolver(__VA_ARGS__, const worker::RpcTenantResolver &resolver)
#define DS_OC_PROC_FORWARD(name, ...) name##WithTenantResolver(__VA_ARGS__, resolver)
#define DS_OC_REQUEST_TENANT(manager, request, tenant) \
    resolver.Request(manager, request.tenant_id(), request.token(), tenant)
#define DS_OC_SESSION_TENANT(manager, request, tenant) resolver.Resolve(manager, request, tenant)
#include "datasystem/worker/object_cache/service/worker_oc_service_expire_body.inc"
#undef DS_OC_SESSION_TENANT
#undef DS_OC_REQUEST_TENANT
#undef DS_OC_PROC_FORWARD
#undef DS_OC_PROC_ENTRY
}  // namespace object_cache
}  // namespace datasystem
