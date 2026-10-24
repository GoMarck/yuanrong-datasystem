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

#include "datasystem/worker/object_cache/worker_oc_service_impl.h"

#include <algorithm>
#include <iterator>

#include "datasystem/common/flags/common_flags.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/log/access_recorder.h"
#include "datasystem/common/metrics/kv_metrics.h"
#include "datasystem/common/perf/perf_manager.h"
#include "datasystem/common/util/format.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/strings_util.h"
#include "datasystem/common/util/timer.h"
#include "datasystem/worker/client_manager/client_manager.h"
#include "datasystem/worker/object_cache/get_hash_ring_response.h"
#include "datasystem/worker/object_cache/service/worker_oc_service_crud_common_api.h"
#include "datasystem/worker/object_cache/worker_oc_service_rpc_helpers.h"
#include "datasystem/worker/rpc_tenant_resolver.h"

DS_DECLARE_string(master_address);

namespace datasystem {
namespace object_cache {
#define DS_OC_WRITE_ENTRY(name, ...) \
    name##WithTenantResolver(__VA_ARGS__, const worker::RpcTenantResolver &resolver)
#define DS_OC_WRITE_FORWARD(name, ...) name##WithTenantResolver(__VA_ARGS__, resolver)
#define DS_OC_WRITE_REQUEST_TENANT(manager, request, tenant) \
    resolver.Request(manager, request.tenant_id(), request.token(), tenant)
#define DS_OC_WRITE_SESSION_TENANT(manager, request, tenant) resolver.Resolve(manager, request, tenant)
#define DS_OC_WRITE_VERIFY(manager, request) resolver.Verify(*manager)
#define DS_OC_WRITE_KNOWN_TENANT(manager, request, knownTenant, tenant) \
    resolver.Request(manager, knownTenant, request.token(), tenant)
#define DS_OC_WRITE_MULTIPUBLISH_BODY
#define DS_OC_WRITE_HEALTH_BODY
#define DS_OC_WRITE_MIGRATION_BODY
#define DS_OC_WRITE_SHM_REFERENCE_BODY
#define DS_OC_WRITE_INVALIDATE_BODY
#define DS_OC_WRITE_HASH_RING_BODY
#define DS_OC_WRITE_PUBLISH_BODY
#define DS_OC_WRITE_GREF_BODY
#define DS_OC_WRITE_DELETE_BODY
#define DS_OC_WRITE_QUERY_REF_BODY
#define DS_OC_WRITE_EXPIRE_BODY
#define DS_OC_WRITE_CREATE_BODY
#define DS_OC_WRITE_READ_INFO_BODY
#define DS_OC_WRITE_META_INFO_BODY
#include "datasystem/worker/object_cache/worker_oc_service_body.inc"
#undef DS_OC_WRITE_HASH_RING_BODY
#undef DS_OC_WRITE_INVALIDATE_BODY
#undef DS_OC_WRITE_SHM_REFERENCE_BODY
#undef DS_OC_WRITE_MIGRATION_BODY
#undef DS_OC_WRITE_HEALTH_BODY
#undef DS_OC_WRITE_META_INFO_BODY
#undef DS_OC_WRITE_READ_INFO_BODY
#undef DS_OC_WRITE_CREATE_BODY
#undef DS_OC_WRITE_EXPIRE_BODY
#undef DS_OC_WRITE_QUERY_REF_BODY
#undef DS_OC_WRITE_DELETE_BODY
#undef DS_OC_WRITE_GREF_BODY
#undef DS_OC_WRITE_PUBLISH_BODY
#undef DS_OC_WRITE_MULTIPUBLISH_BODY
#undef DS_OC_WRITE_KNOWN_TENANT
#undef DS_OC_WRITE_VERIFY
#undef DS_OC_WRITE_SESSION_TENANT
#undef DS_OC_WRITE_REQUEST_TENANT
#undef DS_OC_WRITE_FORWARD
#undef DS_OC_WRITE_ENTRY
}  // namespace object_cache
}  // namespace datasystem
