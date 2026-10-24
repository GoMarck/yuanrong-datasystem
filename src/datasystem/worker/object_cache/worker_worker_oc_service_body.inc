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


#ifdef DS_WW_RPC_CONSTANT
constexpr double US_PER_MS = 1000.0;
#endif

#ifdef DS_WW_RPC_HELPERS
template <typename Response>
void AttachPublishedSelfUbHealth(const std::shared_ptr<WorkerOCServiceImpl> &service, Response &rsp)
{
    if (service == nullptr) {
        return;
    }
    auto summary = service->GetPublishedSelfUbHealthProto();
    if (summary != nullptr) {
        rsp.mutable_ub_health_summary()->CopyFrom(*summary);
    }
}

#ifdef DS_WW_RPC_NATIVE_DIRECT
void MovePayload(std::vector<RpcMessage> &src, std::vector<RpcMessage> &dst)
{
    dst.insert(dst.end(), std::make_move_iterator(src.begin()), std::make_move_iterator(src.end()));
}

#endif

std::string GetRemoteAddressForLog(const GetObjectRemoteReqPb &req)
{
    if (req.has_urma_info()) {
        return FormatString("%s:%d", req.urma_info().request_address().host(),
                            req.urma_info().request_address().port());
    }
    if (req.has_ucp_info()) {
        return FormatString("%s:%d", req.ucp_info().remote_ip_addr().host(), req.ucp_info().remote_ip_addr().port());
    }
    return "";
}

const char *GetRemoteGetTransportName(const GetObjectRemoteReqPb &req)
{
    if (req.has_urma_info()) {
        return "UB";
    }
    if (req.has_ucp_info()) {
        return "RDMA";
    }
    return "TCP";
}

const char *GetRemoteGetTransportName(const BatchGetObjectRemoteReqPb &req)
{
    return req.requests_size() == 0 ? "UNKNOWN" : GetRemoteGetTransportName(req.requests(0));
}

std::string GetBatchRemoteGetObjectKeys(const BatchGetObjectRemoteReqPb &req)
{
    std::string objectKeys = "[";
    uint64_t totalKeySize = 0;
    uint64_t keyCount = 0;
    for (const auto &request : req.requests()) {
        if (keyCount >= LOG_TOTAL_KEYS_SIZE_LIMIT) {
            return objectKeys.append("***,total:").append(std::to_string(req.requests_size())).append("]");
        }
        const auto &key = request.object_key();
        if (totalKeySize > 0 && key.size() > LOG_OBJECT_KEY_SIZE_LIMIT - totalKeySize) {
            return objectKeys.append("total:").append(std::to_string(req.requests_size())).append("]");
        }
        ++keyCount;
        const size_t displaySize = std::min<uint64_t>(key.size(), LOG_OBJECT_KEY_SIZE_LIMIT);
        totalKeySize = std::min<uint64_t>(totalKeySize + key.size(), LOG_OBJECT_KEY_SIZE_LIMIT);
        objectKeys.append(key.data(), displaySize).append(",");
    }
    if (objectKeys.size() > 1) {
        objectKeys.pop_back();
    }
    return objectKeys.append("]");
}

uint64_t GetRemoteGetDataSize(const GetObjectRemoteRspPb &rsp)
{
    return rsp.data_size() > 0 ? static_cast<uint64_t>(rsp.data_size()) : 0;
}

uint64_t GetBatchRemoteGetDataSize(const BatchGetObjectRemoteRspPb &rsp)
{
    uint64_t totalDataSize = 0;
    for (const auto &response : rsp.responses()) {
        if (response.error().error_code() == K_OK && response.data_size() > 0) {
            totalDataSize += static_cast<uint64_t>(response.data_size());
        }
    }
    return totalDataSize;
}

// UB data-transfer logs describe the RPC request direction (src=requester, dst=this worker), which reads
// inverted against the data flow direction (worker pushes to the SDK). Append explicit roles.
std::string AppendRequesterProviderForLog(const std::string &prefix, const std::string &requesterAddr,
                                          const std::string &providerAddr)
{
    return prefix + AppendSrcDstForLog(requesterAddr, providerAddr)
           + FormatString(", requester=%s, provider=%s", requesterAddr.c_str(), providerAddr.c_str());
}

Status GetRemoteAddressFromBatchGetReq(const BatchGetObjectRemoteReqPb &req, HostPort &requestAddress)
{
    CHECK_FAIL_RETURN_STATUS(req.requests_size() > 0, K_INVALID, "BatchGetObjectRemote request is empty");
    const auto &firstReq = req.requests(0);
    std::string host;
    int port = -1;
    if (firstReq.has_urma_info()) {
        host = firstReq.urma_info().request_address().host();
        port = firstReq.urma_info().request_address().port();
    } else if (firstReq.has_ucp_info()) {
        host = firstReq.ucp_info().remote_ip_addr().host();
        port = firstReq.ucp_info().remote_ip_addr().port();
    } else {
        RETURN_STATUS(K_INVALID, "BatchGetObjectRemote request has no remote address");
    }
    requestAddress = HostPort(host, port);
    return Status::OK();
}

void LogBatchGetObjectRemotePrepareFailed(const BatchGetObjectRemoteReqPb &req, const std::string &callerAddress,
                                          const std::string &firstObjectKey, const Status &status)
{
    VLOG(1) << "[REMOTE_GET_CONNECTION_CHECK_FAILED] method=BatchGetObjectRemote"
            << ", count=" << req.requests_size() << ", firstObjectKey=" << firstObjectKey
            << ", src=" << callerAddress << ", dst=" << FLAGS_worker_address
            << ", status=" << status.ToString() << ", willReturnViaBrpcSetFailed=true";
}

void LogBatchGetObjectRemoteFinish(const LatencyTraceConfig &config, const BatchGetObjectRemoteReqPb &req,
                                   const std::vector<RpcMessage> &payload, const std::string &firstObjectKey,
                                   uint64_t realRemainingTime, const std::string &callerAddress, const Timer &timer)
{
    const auto elapsedUs = static_cast<uint64_t>(timer.ElapsedMicroSecond());
    const double elapsedMs = static_cast<double>(elapsedUs) / US_PER_MS;
    // Per-phase breakdown: RemoteLockEntry / RemoteTryRLatch / RemoteWriteFastTransport.
    SLOW_LOG_IF_OR_VLOG(
        INFO, config.processSlowerThanUs > 0 && elapsedUs >= config.processSlowerThanUs, 1,
        AppendRequesterProviderForLog(
            FormatString("[Get/RemotePull] finish, count: %d, firstObjectKey: %s, payload size: %zu, start "
                         "remainingTime: %zu, cost: %.3fms, breakdown: %s",
                         req.requests_size(), firstObjectKey, payload.size(), realRemainingTime, elapsedMs,
                         GetWorkerTimeCost().GetInfo()),
            callerAddress, FLAGS_worker_address));
}
#endif

#ifdef DS_WW_RPC_GET
Status WorkerWorkerOCServiceImpl::DS_WW_RPC_ENTRY(GetObjectRemote,
    std::shared_ptr<::datasystem::ServerUnaryWriterReader<GetObjectRemoteRspPb, GetObjectRemoteReqPb>> serverApi)
{
    ScopedRequestContext ctx;
    Timer timer;
    METRIC_TIMER(metrics::KvMetricId::WORKER_RPC_REMOTE_GET_INBOUND_LATENCY);
    PerfPoint point(PerfKey::WORKER_SERVER_GET_REMOTE);
    GetObjectRemoteReqPb req;
    GetObjectRemoteRspPb rsp;
    std::vector<RpcMessage> payload;
    bool wasRequestRead = false;
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_REMOTE_GET);
    access.ObjectKeyProvider([&req] { return req.object_key(); }).DataSizeProvider([&rsp] {
        return GetRemoteGetDataSize(rsp);
    });
    RemoteGetRpcExecutionContext executionContext{timer, point, wasRequestRead};
    Status rc = DS_WW_RPC_FORWARD(ProcessGetObjectRemoteRpc, serverApi, req, rsp, payload, executionContext);
    access.TransportType(wasRequestRead ? GetRemoteGetTransportName(req) : "UNKNOWN").Result(rc).Record();
    return rc;
}

Status WorkerWorkerOCServiceImpl::DS_WW_RPC_ENTRY(ProcessGetObjectRemoteRpc,
    const std::shared_ptr<::datasystem::ServerUnaryWriterReader<GetObjectRemoteRspPb, GetObjectRemoteReqPb>> &serverApi,
    GetObjectRemoteReqPb &req, GetObjectRemoteRspPb &rsp, std::vector<RpcMessage> &payload,
    RemoteGetRpcExecutionContext &executionContext)
{
    auto &[timer, point, wasRequestRead] = executionContext;
    auto config = GetServerLatencyTraceConfig();
    const bool traceEnabled = ShouldCollectLatencyTrace(config);
    PerfPoint pointImpl(PerfKey::WORKER_SERVER_GET_REMOTE_READ);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "GetObjectRemote read error");
    wasRequestRead = true;
    if (traceEnabled) {
        Trace::Instance().AddLatencyTick(LatencyTickKey::DATA_REMOTEGET_START);
    }
    pointImpl.RecordAndReset(PerfKey::WORKER_SERVER_GET_REMOTE_IMPL);
    INJECT_POINT("worker.GetObjectRemote.afterRead");
    BatchRh2dContext transportContext;
    auto getRc = DS_WW_RPC_FORWARD(ProcessSingleGetObjectRemote, req, rsp, payload, &transportContext);
    const bool providerUbFailureEncoded = TryEncodeProviderUbFailureResponse(getRc, rsp);
    if (!providerUbFailureEncoded) {
        RETURN_IF_NOT_OK_EXCEPT(getRc, StatusCode::K_OC_REMOTE_GET_NOT_ENOUGH);
    }
    TryEncodeRemoteGetLatencySummary(config, traceEnabled, rsp);
    AttachPublishedSelfUbHealth(ocClientWorkerSvc_, rsp);
    pointImpl.RecordAndReset(PerfKey::WORKER_SERVER_GET_REMOTE_WRITE);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Write(rsp), "GetObjectRemote write error");
    pointImpl.RecordAndReset(PerfKey::WORKER_SERVER_GET_REMOTE_SENDPAYLOAD);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(SendGetObjectRemotePayload(serverApi, rsp, payload),
                                     "GetObjectRemote send payload error");
    pointImpl.Record();
    const auto elapsedUs = static_cast<uint64_t>(timer.ElapsedMicroSecond());
    const double elapsedMs = static_cast<double>(elapsedUs) / US_PER_MS;
    const char *requestTransport = req.has_urma_info() ? "UB" : (req.has_ucp_info() ? "RDMA" : "RPC_PAYLOAD");
    SLOW_LOG_IF_OR_VLOG(INFO, config.processSlowerThanUs > 0 && elapsedUs >= config.processSlowerThanUs, 1,
                        AppendRequesterProviderForLog(
                            FormatString("[GetObjRemote] finish, objKey: %s, reqTransport: %s, dataSrc: %d, "
                                         "payloadCnt: %zu, cost: %.3fms",
                                         req.object_key(), requestTransport, static_cast<int>(rsp.data_source()),
                                         payload.size(), elapsedMs),
                            GetRemoteAddressForLog(req), FLAGS_worker_address));
    point.Record();
    return Status::OK();
}

#ifdef DS_WW_RPC_NATIVE_DIRECT
Status WorkerWorkerOCServiceImpl::DS_WW_RPC_ENTRY(GetObjectRemote,GetObjectRemoteReqPb &req, GetObjectRemoteRspPb &rsp,
                                                  std::vector<RpcMessage> &payload)
{
    Raii attachHealth([this, &rsp] { AttachPublishedSelfUbHealth(ocClientWorkerSvc_, rsp); });
    return DS_WW_RPC_FORWARD(ProcessSingleGetObjectRemote, req, rsp, payload, nullptr);
}

#endif

Status WorkerWorkerOCServiceImpl::DS_WW_RPC_ENTRY(ProcessSingleGetObjectRemote,GetObjectRemoteReqPb &req,
                                                               GetObjectRemoteRspPb &rsp,
                                                               std::vector<RpcMessage> &payload,
                                                               BatchRh2dContext *transportContext)
{
    // Inherit the SDK traceID from the worker thread's thread_local Trace (set by
    // WorkerEntryImpl's SetTraceContextFromMeta) into the per-request context so
    // that any access recorder or log emitted inside the pull handler scope carries
    // the same traceID as the original SDK request. Without this, the pull handler
    // runs without an active RequestContext and nested recorders fall back to a
    // detached UUID that does not correlate with the SDK request.
    ScopedRequestContext ctx;
    METRIC_TIMER(metrics::KvMetricId::WORKER_RPC_REMOTE_GET_INBOUND_LATENCY);
    Timer slowLogTimer;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_WW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    const std::string callerAddress = GetRemoteAddressForLog(req);
    if (transportContext != nullptr) {
        RETURN_IF_NOT_OK(PrepareSingleGetObjectRemoteReq(req, *transportContext));
    }
    std::vector<uint64_t> eventKeys;
    RETURN_IF_NOT_OK(GetObjectRemoteHandler(req, rsp, payload, true, eventKeys, nullptr, nullptr, nullptr,
                                            transportContext));
    auto config = GetServerLatencyTraceConfig();
    uint64_t elapsedUs = static_cast<uint64_t>(slowLogTimer.ElapsedMicroSecond());
    SLOW_LOG_IF_OR_VLOG(
        INFO,
        (config.processSlowerThanUs > 0 && elapsedUs >= config.processSlowerThanUs) || FLAGS_enable_perf_trace_log, 1,
        AppendRequesterProviderForLog(
            FormatString("Processing pull obj[%s] offset[%ld] size[%ld], expectDataSize[%ld], ver[%ld], "
                         "hasUrmaInfo[%d], cost: %.3fms",
                         req.object_key(), req.read_offset(), req.read_size(), req.data_size(), req.version(),
                         req.has_urma_info(), static_cast<double>(elapsedUs) / US_PER_MS),
            callerAddress, FLAGS_worker_address));
    return Status::OK();
}

#endif

#ifdef DS_WW_RPC_POLICY
Status WorkerWorkerOCServiceImpl::DS_WW_RPC_ENTRY(CheckCoordinatorState,const CheckCoordinatorStateReqPb &req,
                                                        CheckCoordinatorStateRspPb &rsp)
{
    ScopedRequestContext ctx;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_WW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    CHECK_FAIL_RETURN_STATUS(static_cast<bool>(coordinationAvailable_), K_NOT_READY,
                             "Coordination availability provider is not initialized.");
    bool isCoordinationAvailable = coordinationAvailable_();
    rsp.set_available(isCoordinationAvailable);
    LOG_IF(INFO, isCoordinationAvailable) << "Coordination backend is available";
    return Status::OK();
}

Status WorkerWorkerOCServiceImpl::DS_WW_RPC_ENTRY(GetClusterState,const GetClusterStateReqPb &req, GetClusterStateRspPb &rsp)
{
    ScopedRequestContext ctx;
    INJECT_POINT("WorkerWorkerOCServiceImpl.GetClusterState.returnError");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_WW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    CHECK_FAIL_RETURN_STATUS(static_cast<bool>(backendObservationProvider_), K_NOT_READY,
                             "Control-backend observation provider is not initialized.");
    RETURN_IF_NOT_OK(FillGetClusterStateRspPbFromControlBackendObservation(backendObservationProvider_(), rsp));
    return Status::OK();
}

Status WorkerWorkerOCServiceImpl::DS_WW_RPC_ENTRY(GetPeerHashRing,const GetHashRingReqPb &req, GetHashRingRspPb &rsp)
{
    RETURN_RUNTIME_ERROR_IF_NULL(ocClientWorkerSvc_);
    return DS_WW_RPC_FORWARD(ocClientWorkerSvc_->GetHashRing, req, rsp);
}

Status WorkerWorkerOCServiceImpl::DS_WW_RPC_ENTRY(QueryUbPortHealth,const QueryUbPortHealthReqPb &req,
                                                    QueryUbPortHealthRspPb &rsp)
{
    RETURN_RUNTIME_ERROR_IF_NULL(akSkManager_);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_WW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    RETURN_RUNTIME_ERROR_IF_NULL(ocClientWorkerSvc_);
    return ocClientWorkerSvc_->QuerySelfUbPortHealth(req, rsp);
}

Status WorkerWorkerOCServiceImpl::DS_WW_RPC_ENTRY(MigrateData,const MigrateDataReqPb &req, MigrateDataRspPb &rsp,
                                              std::vector<::datasystem::RpcMessage> payloads)
{
    ScopedRequestContext ctx;
    return DS_WW_RPC_FORWARD(ocClientWorkerSvc_->MigrateData, req, rsp, std::move(payloads));
}

Status WorkerWorkerOCServiceImpl::DS_WW_RPC_ENTRY(MigrateDataDirect,const MigrateDataDirectReqPb &req, MigrateDataDirectRspPb &rsp)
{
    ScopedRequestContext ctx;
    return DS_WW_RPC_FORWARD(ocClientWorkerSvc_->MigrateDataDirect, req, rsp);
}

#endif

#ifdef DS_WW_RPC_BATCH
Status WorkerWorkerOCServiceImpl::DS_WW_RPC_ENTRY(BatchGetObjectRemote,
    std::shared_ptr<::datasystem::ServerUnaryWriterReader<BatchGetObjectRemoteRspPb, BatchGetObjectRemoteReqPb>>
        serverApi)
{
    ScopedRequestContext ctx;
    Timer timer;
    METRIC_TIMER(metrics::KvMetricId::WORKER_RPC_REMOTE_GET_INBOUND_LATENCY);
    PerfPoint point(PerfKey::WORKER_SERVER_GET_REMOTE);
    BatchGetObjectRemoteReqPb req;
    BatchGetObjectRemoteRspPb rsp;
    std::vector<RpcMessage> payload;
    bool wasRequestRead = false;
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_REMOTE_MGET);
    access.ObjectKeyProvider([&req] { return GetBatchRemoteGetObjectKeys(req); }).DataSizeProvider([&rsp] {
        return GetBatchRemoteGetDataSize(rsp);
    });
    RemoteGetRpcExecutionContext executionContext{timer, point, wasRequestRead};
    Status rc = DS_WW_RPC_FORWARD(ProcessBatchGetObjectRemoteRpc, serverApi, req, rsp, payload, executionContext);
    access.Count(req.requests_size())
        .TransportType(wasRequestRead ? GetRemoteGetTransportName(req) : "UNKNOWN")
        .Result(rc)
        .Record();
    return rc;
}

Status WorkerWorkerOCServiceImpl::DS_WW_RPC_ENTRY(ProcessBatchGetObjectRemoteRpc,
    const std::shared_ptr<
        ::datasystem::ServerUnaryWriterReader<BatchGetObjectRemoteRspPb, BatchGetObjectRemoteReqPb>> &serverApi,
    BatchGetObjectRemoteReqPb &req, BatchGetObjectRemoteRspPb &rsp, std::vector<RpcMessage> &payload,
    RemoteGetRpcExecutionContext &executionContext)
{
    auto &[timer, point, wasRequestRead] = executionContext;
    PerfPoint pointImpl(PerfKey::WORKER_SERVER_GET_REMOTE_READ);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "GetObjectRemote read error");
    wasRequestRead = true;
    INJECT_POINT("worker.BatchGetObjectRemote.afterRead");
    pointImpl.RecordAndReset(PerfKey::WORKER_SERVER_GET_REMOTE_IMPL);
    HostPort requestAddress;
    const std::string callerAddress =
        GetRemoteAddressFromBatchGetReq(req, requestAddress).IsOk() ? requestAddress.ToString() : "";
    const auto &firstObjectKey = req.requests_size() > 0 ? req.requests(0).object_key() : "";
    auto realRemainingTime = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTime();
    VLOG(1) << AppendSrcDstForLog(
        FormatString("[Get/RemotePull] Receive, count: %d, remainingTime: %zu", req.requests_size(), realRemainingTime),
        callerAddress, FLAGS_worker_address);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_WW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    BatchRh2dContext batchTransportContext;
    auto prepareRc = PrepareBatchGetObjectRemoteReq(req, batchTransportContext);
    if (prepareRc.IsError()) {
        LogBatchGetObjectRemotePrepareFailed(req, callerAddress, firstObjectKey, prepareRc);
        return prepareRc;
    }
    auto config = GetServerLatencyTraceConfig();
    const bool traceEnabled = ShouldCollectLatencyTrace(config);
    if (traceEnabled) {
        Trace::Instance().AddLatencyTick(LatencyTickKey::DATA_REMOTEGET_START);
    }
    RETURN_IF_NOT_OK(BatchGetObjectRemoteImpl(req, rsp, payload, batchTransportContext));
    TryEncodeRemoteGetLatencySummary(config, traceEnabled, rsp);
    AttachPublishedSelfUbHealth(ocClientWorkerSvc_, rsp);
    pointImpl.RecordAndReset(PerfKey::WORKER_SERVER_GET_REMOTE_WRITE);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Write(rsp), "GetObjectRemote write error");
    pointImpl.RecordAndReset(PerfKey::WORKER_SERVER_GET_REMOTE_SENDPAYLOAD);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->SendAndTagPayload(payload, FLAGS_oc_worker_worker_direct_port > 0),
                                     "GetObjectRemote send payload error");
    pointImpl.Record();
    LogBatchGetObjectRemoteFinish(config, req, payload, firstObjectKey, realRemainingTime, callerAddress, timer);
    point.Record();
    return Status::OK();
}

#endif

