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

namespace {
void FillReadLoadFeedback(const Status &rc, PublishRspPb &resp)
{
    // READ_BW_WRH_P2C_PUBLISH_FEEDBACK
    if (rc.IsOk()) {
        const auto bw = scheduling::WorkerReadBandwidthTracker::Instance().GetSnapshot();
        resp.set_read_load_p50(bw.p50Ns);
        resp.set_read_load_p99(bw.p99Ns);
        resp.set_read_load_sample_version(bw.latencyVersion);
    }
}
}  // namespace

Status WorkerOcServicePublishImpl::DS_OC_PROC_ENTRY(PublishImpl, const PublishReqPb &req, PublishRspPb &resp,
                                               std::vector<RpcMessage> &payloads)
{
    // Step1: Add namespace for objectKeys.
    VLOG(1) << FormatString("[ObjectKey %s] is being publishing [Sz: %zu].", req.object_key(), req.data_size());
    std::string tenantId;
    Status authRc = req.is_routed() ? DS_OC_REQUEST_TENANT(akSkManager_, req, tenantId)
                                    : DS_OC_SESSION_TENANT(akSkManager_, req, tenantId);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(authRc, "Authenticate failed.");
    std::vector<ShmKey> shmUnits = std::vector<ShmKey>{ ShmKey::Intern(req.shm_id()) };
    RETURN_IF_NOT_OK(WorkerOcServiceCrudCommonApi::CheckShmUnitByTenantId(tenantId, ClientKey::Intern(req.client_id()),
                                                                          shmUnits, memoryRefTable_));
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_key());
    PerfPoint point(PerfKey::WORKER_SEAL_OBJECT);
    auto nestedObjectKeys = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.nested_keys());

    // Step 2: Do create-or-get logic on object table, and get both locks.
    std::future<Status> future;
    auto shmUnitId = ShmKey::Intern(req.shm_id());
    auto clientId = ClientKey::Intern(req.client_id());
    Status rc =
        RetryWhenDeadlock([this, &namespaceUri, &req, &clientId, &shmUnitId, &nestedObjectKeys, &payloads, &future] {
            return PublishObjectWithLock(namespaceUri, req, clientId, shmUnitId, nestedObjectKeys, payloads, future);
        });

    // If worker diable shared-memory transfer but the request still carries shmUnitId, client-to-worker data transfer
    // is through UB; we need clean memoryRefTable to avoid memory leak.
    if (!shmUnitId.Empty() && !(ShmEnable() && ClientShmEnabled(clientId))) {
        memoryRefTable_->RemoveShmUnit(clientId, shmUnitId);
    }

    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(rc, FormatString("[ObjectKey %s] Publish failed.", namespaceUri));
    Status futureRc = Status::OK();
    INJECT_POINT("worker.async_send_wait_future", [&future, &futureRc]() {
        futureRc = future.get();
        LOG(INFO) << "Inject point async_send_wait, futureRc: " << futureRc;
        return Status::OK();
    });
    RETURN_IF_NOT_OK(futureRc);

    // record the published object size.
    (void)resp;
    point.Record();
    if (req.is_seal()) {
        INJECT_POINT("worker.seal_failure");
    } else {
        INJECT_POINT("worker.publish_failure");
    }
    VLOG(1) << "Put success";
    INJECT_POINT("worker.after_publish");
    return Status::OK();
}

Status WorkerOcServicePublishImpl::DS_OC_PROC_ENTRY(Publish, const PublishReqPb &req, PublishRspPb &resp,
                                           std::vector<RpcMessage> &payloads)
{
    ScopedRequestContext ctx;
    Timer timer;
    auto config = GetServerLatencyTraceConfig();
    const bool traceEnabled = ShouldCollectLatencyTrace(config);
    if (traceEnabled) {
        Trace::Instance().AddLatencyTick(LatencyTickKey::WORKER_PUBLISH_START);
    }
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_PUBLISH);
    access.ObjectKeyProvider([&req]() -> std::string { return req.object_key(); }).DataSize(req.data_size());
    if (req.nested_keys_size() > 0) {
        access.NestedKeyProvider([&req] { return objectKeysToString(req.nested_keys()); });
    }
    access.Keep(req.keep())
        .WriteMode(req.write_mode())
        .ConsistencyType(req.consistency_type())
        .IsSeal(req.is_seal())
        .IsRetry(req.is_retry())
        .TtlSecond(req.ttl_second())
        .Existence(req.existence())
        .CacheType(req.cache_type());
    Status rc = DS_OC_PROC_FORWARD(PublishImpl, req, resp, payloads);
    FillReadLoadFeedback(rc, resp);
    if (traceEnabled) {
        Trace::Instance().AddLatencyTick(LatencyTickKey::WORKER_PUBLISH_END);
    }
    const auto totalPublishUs = static_cast<uint64_t>(timer.ElapsedMicroSecond());
    if (ShouldPrintLatencySummary(totalPublishUs, config)) {
        PhaseDurationResult result = ComputePhaseDurations(
            Trace::Instance().GetLatencyTicks(), Trace::Instance().GetLatencyTickCount(),
            Trace::Instance().GetLatencyTickDroppedCount());
        bool hasDownstream = Trace::Instance().GetDownstreamPhases().count > 0;
        MergeDownstreamPhases(result);
        bool gateHit = CheckPhaseGate(result, config);
        if (gateHit) {
            Trace::Instance().SetLatencySummary(FormatLatencySummary(result));
        }
        if (gateHit || hasDownstream) {
            EncodePhaseProto(resp, result);
        }
    }
    access.Result(rc).Record();
    const double totalPublishMs = static_cast<double>(totalPublishUs) / US_PER_MS;
    GetWorkerTimeCost().Append("Total Publish", totalPublishMs);
    SLOW_LOG_IF_OR_VLOG(INFO, config.processSlowerThanUs > 0 && totalPublishUs >= config.processSlowerThanUs, 1,
        FormatString("Publish done, cost: %.3fms, %s", totalPublishMs, GetWorkerTimeCost().GetInfo()));
    return rc;
}
