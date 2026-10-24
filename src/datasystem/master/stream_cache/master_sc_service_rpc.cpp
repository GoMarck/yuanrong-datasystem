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

/**
 * Description: Implement the stream cache services on the master.
 */
#include "datasystem/common/util/uuid_generator.h"
#include "datasystem/master/stream_cache/master_sc_service_impl.h"

#include <chrono>
#include <utility>

#include "datasystem/common/log/log_helper.h"
#include "datasystem/common/stream_cache/util.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/master/metadata_manager_holder.h"
#include "datasystem/master/stream_cache/sc_metadata_manager.h"
#include "datasystem/master/stream_cache/sc_migrate_metadata_manager.h"

#include "datasystem/common/rpc/brpc_auth.h"

namespace datasystem {
namespace master {
#define DS_MASTER_SC_HELPERS
#include "datasystem/master/stream_cache/master_sc_service_body.inc"
#undef DS_MASTER_SC_HELPERS

#define DS_MASTER_SC_ENTRY(name, ...) \
    name##WithRpcAuth(__VA_ARGS__, const RpcRequestProof &proof)
#define DS_MASTER_SC_VERIFY(manager, request) (static_cast<void>(request), VerifyRpcRequest(*manager, &proof))
#define DS_MASTER_SC_FORWARD(name, ...) name##WithRpcAuth(__VA_ARGS__, proof)
#define DS_MASTER_SC_GROUP_0
#define DS_MASTER_SC_GROUP_1
#include "datasystem/master/stream_cache/master_sc_service_body.inc"
#undef DS_MASTER_SC_GROUP_1
#undef DS_MASTER_SC_GROUP_0
#undef DS_MASTER_SC_FORWARD
#undef DS_MASTER_SC_VERIFY
#undef DS_MASTER_SC_ENTRY
}  // namespace master
}  // namespace datasystem
