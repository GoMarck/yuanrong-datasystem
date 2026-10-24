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


#include "datasystem/common/util/uuid_generator.h"
#include "datasystem/worker/stream_cache/client_worker_sc_service_impl.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <sstream>
#include <utility>

#include "datasystem/common/constants.h"
#include "datasystem/common/flags/flags.h"
#include "datasystem/common/log/log.h"
#include "datasystem/common/util/locks.h"
#include "datasystem/common/iam/tenant_auth_manager.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/log/access_recorder.h"
#include "datasystem/common/log/log_helper.h"
#include "datasystem/common/perf/perf_manager.h"
#include "datasystem/common/rpc/rpc_auth_key_manager.h"
#include "datasystem/common/rpc/rpc_stub_cache_mgr.h"
#include "datasystem/common/stream_cache/stream_fields.h"
#include "datasystem/common/stream_cache/util.h"
#include "datasystem/common/util/format.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/common/util/strings_util.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/common/util/validator.h"
#include "datasystem/master/stream_cache/master_sc_service_impl.h"
#include "datasystem/protos/stream_posix.service.rpc.pb.h"
#include "datasystem/stream/stream_config.h"
#include "datasystem/utils/optional.h"
#include "datasystem/utils/status.h"
#include "datasystem/worker/authenticate.h"
#include "datasystem/worker/cluster_event_type.h"
#include "datasystem/worker/worker_health_check.h"
#include "datasystem/worker/stream_cache/client_worker_sc_service_impl.h"
#include "datasystem/worker/stream_cache/metrics/sc_metrics_monitor.h"
#include "datasystem/worker/stream_cache/stream_manager.h"

#include "datasystem/worker/rpc_tenant_resolver.h"

DS_DECLARE_uint32(page_size);
DS_DECLARE_uint64(client_dead_timeout_s);
DS_DECLARE_int32(sc_thread_num);
DS_DECLARE_uint32(sc_gc_interval_ms);
DS_DECLARE_bool(enable_stream_data_verification);
DS_DECLARE_uint32(sc_shared_page_size_mb);

namespace datasystem {
namespace worker {
namespace stream_cache {
#define DS_CLIENT_SC_RPC_ENTRY(name, ...) \
    name##WithTenantResolver(__VA_ARGS__, const ::datasystem::worker::RpcTenantResolver &resolver)
#define DS_CLIENT_SC_RPC_VERIFY(manager, request) (static_cast<void>(request), resolver.Verify(*manager))
#define DS_CLIENT_SC_RPC_TENANT(manager, request, tenant, client) resolver.Resolve(manager, request, tenant, client)
#define DS_CLIENT_SC_RPC_FORWARD(name, ...) name##WithTenantResolver(__VA_ARGS__, resolver)
#define DS_CLIENT_SC_RPC_GROUP_0
#define DS_CLIENT_SC_RPC_GROUP_1
#define DS_CLIENT_SC_RPC_GROUP_2
#define DS_CLIENT_SC_RPC_GROUP_3
#define DS_CLIENT_SC_RPC_GROUP_4
#define DS_CLIENT_SC_RPC_GROUP_5
#define DS_CLIENT_SC_RPC_GROUP_6
#define DS_CLIENT_SC_RPC_GROUP_7
#define DS_CLIENT_SC_RPC_GROUP_8
#define DS_CLIENT_SC_RPC_GROUP_9
#define DS_CLIENT_SC_RPC_GROUP_10
#define DS_CLIENT_SC_RPC_GROUP_11
#include "datasystem/worker/stream_cache/client_worker_sc_service_body.inc"
#undef DS_CLIENT_SC_RPC_GROUP_11
#undef DS_CLIENT_SC_RPC_GROUP_10
#undef DS_CLIENT_SC_RPC_GROUP_9
#undef DS_CLIENT_SC_RPC_GROUP_8
#undef DS_CLIENT_SC_RPC_GROUP_7
#undef DS_CLIENT_SC_RPC_GROUP_6
#undef DS_CLIENT_SC_RPC_GROUP_5
#undef DS_CLIENT_SC_RPC_GROUP_4
#undef DS_CLIENT_SC_RPC_GROUP_3
#undef DS_CLIENT_SC_RPC_GROUP_2
#undef DS_CLIENT_SC_RPC_GROUP_1
#undef DS_CLIENT_SC_RPC_GROUP_0
#undef DS_CLIENT_SC_RPC_FORWARD
#undef DS_CLIENT_SC_RPC_TENANT
#undef DS_CLIENT_SC_RPC_VERIFY
#undef DS_CLIENT_SC_RPC_ENTRY
}  // namespace stream_cache
}  // namespace worker
}  // namespace datasystem
