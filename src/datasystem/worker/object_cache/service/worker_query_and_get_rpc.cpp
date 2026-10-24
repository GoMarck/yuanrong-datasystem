/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
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

#include "datasystem/worker/object_cache/service/worker_query_and_get_impl.h"

#include <chrono>

#include "datasystem/common/log/access_recorder.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/worker/rpc_tenant_resolver.h"

namespace datasystem {
namespace object_cache {
namespace {
#define DS_OC_QUERY_CLOCK_BODY
#include "datasystem/worker/object_cache/service/worker_query_and_get_body.inc"
#undef DS_OC_QUERY_CLOCK_BODY
}  // namespace

#define DS_OC_QUERY_ENTRY(name, ...) \
    name##WithTenantResolver(__VA_ARGS__, const worker::RpcTenantResolver &resolver)
#define DS_OC_QUERY_FORWARD(name, ...) name##WithTenantResolver(__VA_ARGS__, resolver)
#define DS_OC_QUERY_VERIFY(manager, request) resolver.Verify(*manager)
#define DS_OC_QUERY_METHOD_BODY
#include "datasystem/worker/object_cache/service/worker_query_and_get_body.inc"
#undef DS_OC_QUERY_METHOD_BODY
#undef DS_OC_QUERY_VERIFY
#undef DS_OC_QUERY_FORWARD
#undef DS_OC_QUERY_ENTRY
}  // namespace object_cache
}  // namespace datasystem
