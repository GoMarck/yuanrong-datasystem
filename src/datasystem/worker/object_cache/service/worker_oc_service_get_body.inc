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

// Compile the same body in separate translation units to preserve ordinary-path optimization.
    PerfPoint point(PerfKey::WORKER_GET_OBJECT);
    ScopedRequestContext ctx;
    Timer timer;
    const bool traceEnabled = ShouldCollectLatencyTrace(GetServerLatencyTraceConfig());
    if (traceEnabled) {
        Trace::Instance().AddLatencyTick(LatencyTickKey::WORKER_GET_START);
    }
    auto healthProvider = std::atomic_load(&ubHealthSummaryProvider_);
    auto request =
        std::make_shared<GetRequest>(AccessRecorderKey::DS_POSIX_GET, localAddress_, ubAdmission_,
                                     healthProvider == nullptr ? UbHealthSummaryProvider{} : *healthProvider);
    INJECT_POINT("WorkerOCServiceImpl.Get.Retry",
                 [&serverApi]() { return serverApi->SendStatus(Status(K_TRY_AGAIN, "test get retry")); });
    GetReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    auto clientId = ClientKey::Intern(req.client_id());
    auto inflightGauge =
        metrics::GetGauge(static_cast<uint16_t>(metrics::KvMetricId::WORKER_INFLIGHT_REMOTE_GET_REQUEST));
    VLOG(1) << FormatString("[Get] Receive, clientId: %s, serverApiReadCost: %.3fms, inflightRemoteGet: %d", clientId,
                            timer.ElapsedMilliSecond(), inflightGauge.Get());
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_RESOLVE_GET_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");

    int64_t subTimeout = req.sub_timeout();
    int64_t remainingUs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTimeUs();
    auto dispatchTime = std::chrono::steady_clock::now();
    INJECT_POINT("WorkerOCServiceImpl.Get.Timeout", [&remainingUs](int64_t changedRemainingUs) {
        remainingUs = changedRemainingUs;
        return Status::OK();
    });
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(
        remainingUs > 0, K_RPC_DEADLINE_EXCEEDED,
        FormatString("RPC deadline exceeded before dispatch, remaining %ld us.", remainingUs));
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(Validator::IsInNonNegativeInt32(subTimeout), K_RUNTIME_ERROR,
                                         "SubTimeout is out of range.");

    PerfPoint p(PerfKey::WORKER_GET_REQUEST_INIT);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(request->Init(tenantId, req, memoryRefTable_, serverApi, threadPool_, clientId),
                                     "GetRequest Init failed");
    p.Record();

    if (serverApi->EnableMsgQ()) {
        const std::chrono::steady_clock::time_point submitToPool = std::chrono::steady_clock::now();
        auto traceContext = Trace::Instance().GetContext();
        auto cost = GetWorkerTimeCost();
        threadPool_->Execute([=]() mutable {
            ScopedRequestContext ctx;
            GetWorkerTimeCost() = cost;
            TraceGuard traceGuard = Trace::Instance().SetTraceContext(traceContext);
            const std::chrono::steady_clock::time_point poolThreadStart = std::chrono::steady_clock::now();
            {
                const uint64_t qUs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(poolThreadStart - submitToPool).count());
                metrics::GetHistogram(static_cast<uint16_t>(metrics::KvMetricId::WORKER_GET_THREADPOOL_QUEUE_LATENCY))
                    .Observe(qUs);
            }
            auto initRc = InitTimeoutsFromDispatch(remainingUs, dispatchTime);
            if (initRc.IsError()) {
                LOG(ERROR) << initRc.GetMsg();
                LOG_IF_ERROR(serverApi->SendStatus(initRc), "Send status failed");
            } else {
                int64_t currentRemainingUs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTimeUs();
                VLOG(1) << FormatString("[Get] Receive, clientId: %s, objects: %s, threadPool: %s, remainingUs: %ld",
                                        clientId, VectorToString(request->GetRawObjectKeys()),
                                        threadPool_->GetStatistics(), currentRemainingUs);
                // subTimeout is the client's subscribe budget; the actual wait is capped by
                // std::min(subTimeout, remainingTimeMs) in ProcessGetObjectRequest.
                auto newSubTimeout = subTimeout > 0 ? subTimeout : 0;
                const std::chrono::steady_clock::time_point processStart = std::chrono::steady_clock::now();
                LOG_IF_ERROR(ProcessGetObjectRequest(newSubTimeout, request), "Process Get failed");
                {
                    const uint64_t execUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                      std::chrono::steady_clock::now() - processStart)
                                                                      .count());
                    const uint64_t e2EUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                     std::chrono::steady_clock::now() - submitToPool)
                                                                     .count());
                    metrics::GetHistogram(
                        static_cast<uint16_t>(metrics::KvMetricId::WORKER_GET_THREADPOOL_EXEC_LATENCY))
                        .Observe(execUs);
                    metrics::GetHistogram(static_cast<uint16_t>(metrics::KvMetricId::WORKER_PROCESS_GET_LATENCY))
                        .Observe(e2EUs);
                }
                auto getElapsedMs = timer.ElapsedMilliSecond();
                GetWorkerTimeCost().Append("ProcessGetObjectRequest", getElapsedMs);
                auto inflightGaugeGet =
                    metrics::GetGauge(static_cast<uint16_t>(metrics::KvMetricId::WORKER_INFLIGHT_REMOTE_GET_REQUEST));
                auto config = GetServerLatencyTraceConfig();
                SLOW_LOG_IF_OR_VLOG(
                    INFO,
                    (config.processSlowerThanUs > 0
                        && static_cast<uint64_t>(getElapsedMs * US_PER_MS) >= config.processSlowerThanUs)
                        || FLAGS_enable_perf_trace_log,
                    1,
                    FormatString("[Get] Done, clientId: %s, objects: %zu, transferPath: %s, totalCost: %.3fms, "
                                 "inflightRemoteGet: %d %s",
                                 clientId, request->GetRawObjectKeys().size(),
                                 IsUrmaEnabled() ? "UB" : (IsUcpEnabled() ? "RDMA" : "TCP"), getElapsedMs,
                                 inflightGauge.Get(), GetWorkerTimeCost().GetInfo()));
            }
        });
    } else {
        int64_t currentRemainingUs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTimeUs();
        if (currentRemainingUs <= 0) {
            LOG(ERROR) << "RPC deadline exceeded, remaining " << currentRemainingUs << " us";
            return Status(K_RPC_DEADLINE_EXCEEDED,
                          FormatString("RPC deadline exceeded, remaining %ld us.", currentRemainingUs));
        }
        // subTimeout is the client's subscribe budget; the actual wait is capped by
        // std::min(subTimeout, remainingTimeMs) in ProcessGetObjectRequest.
        auto adjustedSubTimeout = subTimeout > 0 ? subTimeout : 0;
        {
            const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            LOG_IF_ERROR(ProcessGetObjectRequest(adjustedSubTimeout, request), "Process Get failed");
            {
                const uint64_t e2EUs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
                        .count());
                metrics::GetHistogram(static_cast<uint16_t>(metrics::KvMetricId::WORKER_GET_THREADPOOL_EXEC_LATENCY))
                    .Observe(e2EUs);
                metrics::GetHistogram(static_cast<uint16_t>(metrics::KvMetricId::WORKER_PROCESS_GET_LATENCY))
                    .Observe(e2EUs);
            }
        }
        auto getElapsedMs = timer.ElapsedMilliSecond();
        GetWorkerTimeCost().Append("ProcessGetObjectRequest", getElapsedMs);
        auto inflightGaugeGet =
            metrics::GetGauge(static_cast<uint16_t>(metrics::KvMetricId::WORKER_INFLIGHT_REMOTE_GET_REQUEST));
        auto config = GetServerLatencyTraceConfig();
        SLOW_LOG_IF_OR_VLOG(
            INFO,
            (config.processSlowerThanUs > 0
                && static_cast<uint64_t>(getElapsedMs * US_PER_MS) >= config.processSlowerThanUs)
                || FLAGS_enable_perf_trace_log,
            1,
            FormatString("[Get] Done, clientId: %s, objects: %zu, transferPath: %s, totalCost: %.3fms, "
                         "inflightRemoteGet: %d %s",
                         clientId, request->GetRawObjectKeys().size(),
                         IsUrmaEnabled() ? "UB" : (IsUcpEnabled() ? "RDMA" : "TCP"), getElapsedMs, inflightGauge.Get(),
                         GetWorkerTimeCost().GetInfo()));
    }
    return Status::OK();
