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

Status WorkerOcServiceCreateImpl::DS_OC_PROC_ENTRY(AuthenticateCreateRequest, const CreateReqPb &req, std::string &tenantId)
{
    CHECK_FAIL_RETURN_STATUS(metadataRouteResolver_ != nullptr, StatusCode::K_NOT_READY,
                             "ETCD cluster manager is not provided.");
    Status rc = req.is_routed() ? DS_OC_REQUEST_TENANT(akSkManager_, req, tenantId)
                                : DS_OC_SESSION_TENANT(akSkManager_, req, tenantId);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(rc, "Authenticate failed.");
    return Status::OK();
}

Status WorkerOcServiceCreateImpl::DS_OC_PROC_ENTRY(Create, const CreateReqPb &req, CreateRspPb &resp)
{
    INJECT_POINT("worker.Create.begin");
    ScopedRequestContext ctx;
    Timer timer;
    PerfPoint point(PerfKey::WORKER_CREATE_OBJECT);
    auto config = GetServerLatencyTraceConfig();
    const bool traceEnabled = ShouldCollectLatencyTrace(config);
    if (traceEnabled) {
        Trace::Instance().AddLatencyTick(LatencyTickKey::WORKER_CREATE_START);
    }
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_CREATE);
    access.ObjectKeyProvider([&req]() -> std::string { return req.object_key(); }).DataSize(req.data_size());
    VLOG(1) << FormatString("Receive create meta request, clientId: %s, objectKey: %s, size: %zu", req.client_id(),
                            req.object_key(), req.data_size());
    int64_t remainingTimeMs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTime();
    INJECT_POINT_NO_RETURN("WorkerOcServiceCreateImpl.Create.timeoutMs",
                           [&remainingTimeMs]() { remainingTimeMs = -1; });
    if (remainingTimeMs <= 0) {
        const auto totalUs = static_cast<uint64_t>(timer.ElapsedMicroSecond());
        FinalizeWorkerLatencyTrace(LatencyTickKey::WORKER_CREATE_END, config, traceEnabled, totalUs, false, resp);
        Status rc = Status(StatusCode::K_RPC_DEADLINE_EXCEEDED,
            FormatString("The create request process time has exceeded the request timeout time (remaining: %lld ms)",
                         remainingTimeMs));
        access.Result(rc).Record();
        return rc;
    }
    std::string tenantId;
    Status authRc = DS_OC_PROC_FORWARD(AuthenticateCreateRequest, req, tenantId);
    if (authRc.IsError()) {
        access.Result(authRc).Record();
        return authRc;
    }
    Status rc = CreateImpl(tenantId, ClientKey::Intern(req.client_id()), req.object_key(), req.data_size(),
                           req.request_timeout(), req.allocation_id(), resp,
                           static_cast<CacheType>(req.cache_type()));
    const auto totalUs = static_cast<uint64_t>(timer.ElapsedMicroSecond());
    FinalizeWorkerLatencyTrace(LatencyTickKey::WORKER_CREATE_END, config, traceEnabled, totalUs, true, resp);
    access.Result(rc).Record();
    point.Record();
    const double totalMs = static_cast<double>(totalUs) / US_PER_MS;
    GetWorkerTimeCost().Append("Total Create", totalMs);
    INJECT_POINT("worker.Create.end");
    SLOW_LOG_IF_OR_VLOG(INFO, config.processSlowerThanUs > 0 && totalUs >= config.processSlowerThanUs, 1,
                        FormatString("Create done, cost: %.3fms, %s", totalMs, GetWorkerTimeCost().GetInfo()));
    return rc;
}

Status WorkerOcServiceCreateImpl::DS_OC_PROC_ENTRY(MultiCreate, const MultiCreateReqPb &req, MultiCreateRspPb &resp)
{
    PerfPoint pointAll(PerfKey::WORKER_MULTI_CREATE_TOTAL);
    PerfPoint point(PerfKey::WORKER_MULTI_CREATE_INPUT_CHECK);
    CHECK_FAIL_RETURN_STATUS(metadataRouteResolver_ != nullptr, StatusCode::K_NOT_READY,
                             "ETCD cluster manager is not provided.");
    std::string tenantId;
    Status authRc = req.is_routed() ? DS_OC_REQUEST_TENANT(akSkManager_, req, tenantId)
                                    : DS_OC_SESSION_TENANT(akSkManager_, req, tenantId);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(authRc, "Authenticate failed.");
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(Validator::IsBatchSizeUnderLimit(req.object_key_size()),
                                         StatusCode::K_INVALID, "invalid object size");
    CHECK_FAIL_RETURN_STATUS(req.object_key_size() == req.data_size_size(), K_INVALID,
                             FormatString("object key count %zu not match with data size count %zu",
                                          req.object_key_size(), req.data_size_size()));
    if (!req.skip_check_existence()) {
        CheckExistence(req, tenantId, resp);
    }
    point.RecordAndReset(PerfKey::WORKER_MULTI_CREATE_IMPL);
    Status rc = MultiCreateImpl(req, tenantId, resp);
    if (rc.IsError()) {
        resp.Clear();
    }
    return rc;
}
