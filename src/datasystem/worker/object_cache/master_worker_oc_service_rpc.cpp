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


#include "datasystem/worker/object_cache/master_worker_oc_service_impl.h"

#include <limits>
#include <sstream>
#include <vector>

#include "datasystem/common/log/log.h"
#include "datasystem/common/iam/tenant_auth_manager.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/log/trace.h"
#include "datasystem/common/rpc/api_deadline.h"
#include "datasystem/common/util/format.h"
#include "datasystem/common/log/log_helper.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/common/util/strings_util.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/protos/master_object.pb.h"
#include "datasystem/utils/status.h"

#include "datasystem/worker/object_cache/object_kv.h"
#include "datasystem/worker/object_cache/service/worker_oc_service_crud_common_api.h"

#include "datasystem/worker/rpc_tenant_resolver.h"

namespace datasystem {
namespace object_cache {
#define DS_MW_RPC_ENTRY(name, ...) \
    name##WithTenantResolver(__VA_ARGS__, const worker::RpcTenantResolver &resolver)
#define DS_MW_RPC_VERIFY(manager, request) (static_cast<void>(request), resolver.Verify(*manager))
#define DS_MW_RPC_GROUP_0
#define DS_MW_RPC_GROUP_1
#define DS_MW_RPC_GROUP_2
#define DS_MW_RPC_GROUP_3
#define DS_MW_RPC_GROUP_4
#include "datasystem/worker/object_cache/master_worker_oc_service_body.inc"
#undef DS_MW_RPC_GROUP_4
#undef DS_MW_RPC_GROUP_3
#undef DS_MW_RPC_GROUP_2
#undef DS_MW_RPC_GROUP_1
#undef DS_MW_RPC_GROUP_0
#undef DS_MW_RPC_VERIFY
#undef DS_MW_RPC_ENTRY
}  // namespace object_cache
}  // namespace datasystem
