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

#ifndef DATASYSTEM_WORKER_OBJECT_CACHE_WORKER_OC_SERVICE_RPC_HELPERS_H
#define DATASYSTEM_WORKER_OBJECT_CACHE_WORKER_OC_SERVICE_RPC_HELPERS_H

#include "datasystem/worker/object_cache/worker_oc_service_impl.h"

namespace datasystem {
namespace object_cache {
namespace {
template <typename Response>
void AttachPublishedSelfUbHealth(const WorkerOCServiceImpl &service, Response &rsp)
{
    auto summary = service.GetPublishedSelfUbHealthProto();
    if (summary != nullptr) {
        rsp.mutable_ub_health_summary()->CopyFrom(*summary);
    }
}
}  // namespace

uint64_t PayloadBytes(const std::vector<RpcMessage> &payloads);
void RecordMultiPublishTransportMetrics(const MultiPublishReqPb &req, uint64_t payloadBytes, bool clientShmEnabled);
}  // namespace object_cache
}  // namespace datasystem
#endif
