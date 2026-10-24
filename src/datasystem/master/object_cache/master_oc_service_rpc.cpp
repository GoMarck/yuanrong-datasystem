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


#include "datasystem/common/perf/perf_manager.h"
#include "datasystem/master/object_cache/master_oc_service_impl.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <type_traits>

#include "datasystem/common/flags/flags.h"
#include "datasystem/common/log/log.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/log/latency_phase.h"
#include "datasystem/common/log/trace.h"

#include "datasystem/common/l2cache/persistence_api.h"
#include "datasystem/common/log/log_helper.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/common/util/strings_util.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/master/object_cache/device/master_dev_oc_manager.h"
#include "datasystem/master/object_cache/oc_metadata_manager.h"
#include "datasystem/master/object_cache/oc_migrate_metadata_manager.h"
#include "datasystem/protos/master_object.service.rpc.pb.h"
#include "datasystem/protos/p2p_subscribe.pb.h"

#include "datasystem/common/rpc/brpc_auth.h"

namespace datasystem {
namespace master {
#define DS_MASTER_RPC_CONSTANT
#define DS_MASTER_RPC_HELPERS
#include "datasystem/master/object_cache/master_oc_service_body.inc"
#undef DS_MASTER_RPC_HELPERS
#undef DS_MASTER_RPC_CONSTANT

#define DS_MASTER_RPC_ENTRY(name, ...) \
    name##WithRpcAuth(__VA_ARGS__, const RpcRequestProof &proof)
#define DS_MASTER_RPC_VERIFY(manager, request) (static_cast<void>(request), VerifyRpcRequest(*manager, &proof))
#define DS_MASTER_RPC_FORWARD(name, ...) name##WithRpcAuth(__VA_ARGS__, proof)
#define DS_MASTER_RPC_TASK_CAPTURE , proof
#define DS_MASTER_RPC_GROUP_0
#define DS_MASTER_RPC_GROUP_1
#define DS_MASTER_RPC_GROUP_2
#define DS_MASTER_RPC_GROUP_3
#include "datasystem/master/object_cache/master_oc_service_body.inc"
#undef DS_MASTER_RPC_GROUP_3
#undef DS_MASTER_RPC_GROUP_2
#undef DS_MASTER_RPC_GROUP_1
#undef DS_MASTER_RPC_GROUP_0
#undef DS_MASTER_RPC_TASK_CAPTURE
#undef DS_MASTER_RPC_FORWARD
#undef DS_MASTER_RPC_VERIFY
#undef DS_MASTER_RPC_ENTRY
}  // namespace master
}  // namespace datasystem
