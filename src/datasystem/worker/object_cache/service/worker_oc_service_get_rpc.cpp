/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.
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

#include "datasystem/worker/object_cache/service/worker_oc_service_get_impl.h"

#include <atomic>
#include <chrono>

#include "datasystem/common/flags/common_flags.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/log/latency_phase.h"
#include "datasystem/common/log/trace.h"
#include "datasystem/common/metrics/kv_metrics.h"
#include "datasystem/common/metrics/metrics.h"
#include "datasystem/common/perf/perf_manager.h"
#include "datasystem/common/rpc/api_deadline.h"
#include "datasystem/common/rdma/fast_transport_base.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/common/util/timer.h"
#include "datasystem/common/util/format.h"
#include "datasystem/common/util/strings_util.h"
#include "datasystem/common/util/validator.h"
#include "datasystem/common/device/device_helper.h"
#include "datasystem/common/iam/tenant_auth_manager.h"
#include "datasystem/common/log/access_recorder.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/worker/object_cache/worker_master_oc_api.h"
#include "datasystem/worker/rpc_tenant_resolver.h"

DS_DECLARE_bool(authorization_enable);

using namespace datasystem::worker;
using namespace datasystem::master;
namespace datasystem {
namespace object_cache {
namespace {
constexpr double US_PER_MS = 1000.0;
}  // namespace

Status WorkerOcServiceGetImpl::GetWithTenantResolver(
    std::shared_ptr<ServerUnaryWriterReader<GetRspPb, GetReqPb>> &serverApi, const worker::RpcTenantResolver &resolver)
{
#define DS_RESOLVE_GET_TENANT(manager, request, tenant) resolver.Resolve(manager, request, tenant)
#include "datasystem/worker/object_cache/service/worker_oc_service_get_body.inc"
#undef DS_RESOLVE_GET_TENANT
}
#define DS_OC_READ_ENTRY(name, ...) \
    name##WithTenantResolver(__VA_ARGS__, const worker::RpcTenantResolver &resolver)
#define DS_OC_READ_FORWARD(name, ...) name##WithTenantResolver(__VA_ARGS__, resolver)
#define DS_OC_READ_REQUEST_TENANT(manager, request, tenant) \
    resolver.Request(manager, request.tenant_id(), request.token(), tenant)
#define DS_OC_READ_SESSION_TENANT(manager, request, tenant) resolver.Resolve(manager, request, tenant)
#define DS_OC_READ_VERIFY(manager, request) (static_cast<void>(request), resolver.Verify(*manager))
#define DS_OC_READ_META_AUTH_BODY
#define DS_OC_READ_OBJECT_INFO_BODY
#define DS_OC_READ_EXIST_INFO_BODY
#include "datasystem/worker/object_cache/service/worker_oc_service_get_info.inc"
#undef DS_OC_READ_EXIST_INFO_BODY
#undef DS_OC_READ_OBJECT_INFO_BODY
#undef DS_OC_READ_META_AUTH_BODY
#undef DS_OC_READ_VERIFY
#undef DS_OC_READ_SESSION_TENANT
#undef DS_OC_READ_REQUEST_TENANT
#undef DS_OC_READ_FORWARD
#undef DS_OC_READ_ENTRY
}  // namespace object_cache
}  // namespace datasystem
