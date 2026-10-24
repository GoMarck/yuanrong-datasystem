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

#ifdef DS_OC_WRITE_MULTIPUBLISH_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(MultiPublish, const MultiPublishReqPb &req, MultiPublishRspPb &resp,
                                         std::vector<RpcMessage> payloads)
{
    ScopedRequestContext ctx;
    Raii attachHealth([this, &resp] { AttachPublishedSelfUbHealth(*this, resp); });
    METRIC_TIMER(metrics::KvMetricId::WORKER_PROCESS_PUBLISH_LATENCY);
    uint64_t payloadBytes = PayloadBytes(payloads);
    RETURN_IF_NOT_OK(VerifyClientWriteAdmission(req.is_routed()));
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerStateForRequest(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    auto clientId = ClientKey::Intern(req.client_id());
    RETURN_IF_NOT_OK(multiPublishProc_->DS_OC_WRITE_FORWARD(MultiPublish, req, resp, payloads, clientId));
    const bool clientShmEnabled = WorkerOcServiceCrudCommonApi::ClientShmEnabled(clientId);
    RecordMultiPublishTransportMetrics(req, payloadBytes, clientShmEnabled);
    if (req.auto_release_memory_ref()) {
        std::set<std::string> failedSet{ resp.failed_object_keys().begin(), resp.failed_object_keys().end() };
        std::vector<ShmKey> shmIds;
        shmIds.reserve(req.object_info_size());
        for (auto &info : req.object_info()) {
            if (failedSet.find(info.object_key()) != failedSet.end()) {
                continue;
            }
            shmIds.emplace_back(ShmKey::Intern(info.shm_id()));
        }
        VLOG(1) << "auto release ref " << VectorToString(shmIds);
        return DecreaseMemoryRef(clientId, shmIds);
    }
    return Status::OK();
}

#endif

#ifdef DS_OC_WRITE_CREATE_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(Create, const CreateReqPb &req, CreateRspPb &resp)
{
    ScopedRequestContext ctx;
    METRIC_TIMER(metrics::KvMetricId::WORKER_PROCESS_CREATE_LATENCY);
    auto admission = VerifyClientWriteAdmission(req.is_routed());
    if (admission.GetCode() == K_SCALE_DOWN && membership_.SupportsWriteRedirect()) {
        std::string tenantId;
        RETURN_IF_NOT_OK(req.is_routed() ? DS_OC_WRITE_REQUEST_TENANT(akSkManager_, req, tenantId)
                                        : DS_OC_WRITE_SESSION_TENANT(akSkManager_, req, tenantId));
        FillWorkerRedirect(req.object_key(), *resp.mutable_worker_redirect());
        return Status::OK();
    }
    RETURN_IF_NOT_OK(admission);
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerStateForRequest(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    Status rc = createProc_->DS_OC_WRITE_FORWARD(Create, req, resp);
    if (rc.IsOk()) {
        METRIC_ADD(metrics::KvMetricId::WORKER_CREATE_ALLOCATED_BYTES,
                   static_cast<uint64_t>(req.data_size()) + resp.metadata_size());
    }
    return rc;
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(MultiCreate, const MultiCreateReqPb &req, MultiCreateRspPb &resp)
{
    ScopedRequestContext ctx;
    METRIC_TIMER(metrics::KvMetricId::WORKER_PROCESS_CREATE_LATENCY);
    Status returnStatus;
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_MULTI_CREATE);
    access.ObjectKeysRef(req.object_key());
    Raii raii([&returnStatus, &access]() { access.Result(returnStatus).Record(); });
    returnStatus = VerifyClientWriteAdmission(req.is_routed());
    if (returnStatus.IsError()) {
        return returnStatus;
    }
    BthreadReadGuard noRecon;
    returnStatus = ValidateWorkerStateForRequest(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime());
    if (returnStatus.IsError()) {
        LOG(ERROR) << "validate worker state failed:" << returnStatus.ToString();
        return returnStatus;
    }
    returnStatus = createProc_->DS_OC_WRITE_FORWARD(MultiCreate, req, resp);
    if (returnStatus.IsOk()) {
        uint64_t totalBytes = 0;
        const int resultCount = resp.results_size();
        for (int i = 0; i < resultCount; ++i) {
            const auto &result = resp.results(i);
            if (result.shm_id().empty()) {
                // existence/NX skip: no memory allocated for this object
                continue;
            }
            const uint64_t dataSize = (i < req.data_size_size()) ? static_cast<uint64_t>(req.data_size(i)) : 0;
            totalBytes += dataSize + static_cast<uint64_t>(result.metadata_size());
        }
        METRIC_ADD(metrics::KvMetricId::WORKER_CREATE_ALLOCATED_BYTES, totalBytes);
    }
    return returnStatus;
}

#endif

#ifdef DS_OC_WRITE_PUBLISH_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(Publish, const PublishReqPb &req, PublishRspPb &resp, std::vector<RpcMessage> payloads)
{
    ScopedRequestContext ctx;
    Raii attachHealth([this, &resp] { AttachPublishedSelfUbHealth(*this, resp); });
    METRIC_TIMER(metrics::KvMetricId::WORKER_PROCESS_PUBLISH_LATENCY);
    uint64_t payloadBytes = PayloadBytes(payloads);
    int64_t remainingUs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTimeUs();
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(
        remainingUs > 0, K_RPC_DEADLINE_EXCEEDED,
        FormatString("RPC deadline exceeded before Publish dispatch, remaining %ld us.", remainingUs));
    auto admission = VerifyClientWriteAdmission(req.is_routed());
    if (admission.GetCode() == K_SCALE_DOWN && membership_.SupportsWriteRedirect()) {
        std::string tenantId;
        RETURN_IF_NOT_OK(req.is_routed() ? DS_OC_WRITE_REQUEST_TENANT(akSkManager_, req, tenantId)
                                        : DS_OC_WRITE_SESSION_TENANT(akSkManager_, req, tenantId));
        FillWorkerRedirect(req.object_key(), *resp.mutable_worker_redirect());
        return Status::OK();
    }
    RETURN_IF_NOT_OK(admission);
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerStateForRequest(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    Status rc = publishProc_->DS_OC_WRITE_FORWARD(Publish, req, resp, payloads);
    if (rc.IsOk()) {
        auto clientId = ClientKey::Intern(req.client_id());
        const bool clientShmEnabled = WorkerOcServiceCrudCommonApi::ClientShmEnabled(clientId);
        if (!req.shm_id().empty()) {
            if (WorkerOcServiceCrudCommonApi::ShmEnable() && clientShmEnabled) {
                METRIC_ADD(metrics::KvMetricId::WORKER_FROM_CLIENT_PUBLISH_SHM_TOTAL_BYTES,
                           static_cast<uint64_t>(req.data_size()));
            } else {
                METRIC_ADD(metrics::KvMetricId::WORKER_FROM_CLIENT_PUBLISH_URMA_TOTAL_BYTES,
                           static_cast<uint64_t>(req.data_size()));
            }
        } else if (payloadBytes > 0) {
            if (clientShmEnabled) {
                METRIC_ADD(metrics::KvMetricId::WORKER_FROM_CLIENT_PUBLISH_LOCAL_TOTAL_BYTES, payloadBytes);
            } else {
                METRIC_ADD(metrics::KvMetricId::WORKER_FROM_CLIENT_PUBLISH_TCP_TOTAL_BYTES, payloadBytes);
            }
        }
    }
    return rc;
}
#endif

#ifdef DS_OC_WRITE_GREF_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(ReleaseGRefs, const ReleaseGRefsReqPb &req, ReleaseGRefsRspPb &resp)
{
    ScopedRequestContext ctx;
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");

    return gRefProc_->DS_OC_WRITE_FORWARD(ReleaseGRefs, req, resp);
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(GIncreaseRef, const GIncreaseReqPb &req, GIncreaseRspPb &resp)
{
    ScopedRequestContext ctx;
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");

    return gRefProc_->DS_OC_WRITE_FORWARD(GIncreaseRef, req, resp);
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(GDecreaseRef, const GDecreaseReqPb &req, GDecreaseRspPb &resp)
{
    ScopedRequestContext ctx;
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");

    return gRefProc_->DS_OC_WRITE_FORWARD(GDecreaseRef, req, resp);
}
#endif

#ifdef DS_OC_WRITE_DELETE_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(DeleteAllCopy, const DeleteAllCopyReqPb &req, DeleteAllCopyRspPb &resp)
{
    ScopedRequestContext ctx;
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    PerfPoint point(PerfKey::WORKER_DELETE_OBJECT);
    if (req.are_device_objects()) {
        return DeleteDevObjects(req, resp);
    }
    return deleteProc_->DS_OC_WRITE_FORWARD(DeleteAllCopy, req, resp);
}
#endif

#ifdef DS_OC_WRITE_QUERY_REF_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(QueryGlobalRefNum, const QueryGlobalRefNumReqPb &req, QueryGlobalRefNumRspCollectionPb &rsp)
{
    ScopedRequestContext ctx;
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");

    return gRefProc_->DS_OC_WRITE_FORWARD(QueryGlobalRefNum, req, rsp);
}
#endif

#ifdef DS_OC_WRITE_EXPIRE_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(Expire, const ExpireReqPb &req, ExpireRspPb &rsp)
{
    ScopedRequestContext ctx;
    BthreadReadGuard noReconciliation;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noReconciliation, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    return expireProc_->DS_OC_WRITE_FORWARD(Expire, req, rsp);
}
#endif

#ifdef DS_OC_WRITE_READ_INFO_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(GetObjMetaInfo, const GetObjMetaInfoReqPb &req, GetObjMetaInfoRspPb &resp)
{
    BthreadReadGuard noReconciliation;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noReconciliation, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    return getProc_->DS_OC_WRITE_FORWARD(GetObjMetaInfo, req, resp);
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(QuerySize, const QuerySizeReqPb &req, QuerySizeRspPb &rsp)
{
    ScopedRequestContext ctx;
    BthreadReadGuard noReconciliation;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noReconciliation, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    return getProc_->DS_OC_WRITE_FORWARD(QuerySize, req, rsp);
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(Exist, const ExistReqPb &req, ExistRspPb &rsp)
{
    ScopedRequestContext ctx;
    METRIC_TIMER(metrics::KvMetricId::WORKER_PROCESS_EXIST_LATENCY);
    BthreadReadGuard noReconciliation;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noReconciliation, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    PerfPoint perfPoint(PerfKey::WORKER_EXIST);
    return getProc_->DS_OC_WRITE_FORWARD(Exist, req, rsp);
}
#endif

#ifdef DS_OC_WRITE_META_INFO_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(GetMetaInfo, const GetMetaInfoReqPb &req, GetMetaInfoRspPb &rsp)
{
    BthreadReadGuard noReconciliation;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noReconciliation, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    return getProc_->DS_OC_WRITE_FORWARD(GetMetaInfo, req, rsp);
}
#endif

#ifdef DS_OC_WRITE_HEALTH_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(HealthCheck, const HealthCheckRequestPb &req, HealthCheckReplyPb &resp)
{
    INJECT_POINT("worker.HealthCheck.begin");
    BthreadReadGuard noRecon;
    auto rc = ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime());
    if (rc.IsError()) {
        LOG(WARNING) << rc;
        return rc;
    }
    if (not req.client_id().empty()) {
        std::string tenantId;
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    }
    (void)resp;
    const bool exitRequested = exitRequested_ != nullptr && exitRequested_->load(std::memory_order_acquire);
    if (exitRequested || shutdownRequested_.load(std::memory_order_acquire)) {
        constexpr int logInterval = 60;
        LOG_EVERY_T(INFO, logInterval) << "[HealthCheck] Worker is exiting now";
        RETURN_STATUS(StatusCode::K_SCALE_DOWN, "Worker is exiting now");
    }
    return Status::OK();
}
#endif

#ifdef DS_OC_WRITE_MIGRATION_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(MigrateData, const MigrateDataReqPb &req, MigrateDataRspPb &rsp,
                                        std::vector<RpcMessage> payloads)
{
    ScopedRequestContext ctx;
    Raii attachHealth([this, &rsp] { AttachPublishedSelfUbHealth(*this, rsp); });
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_VERIFY(akSkManager_, req), "AK/SK failed.");
    return gMigrateProc_->MigrateData(req, rsp, std::move(payloads));
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(MigrateDataDirect, const MigrateDataDirectReqPb &req, MigrateDataDirectRspPb &rsp)
{
    ScopedRequestContext ctx;
    Raii attachHealth([this, &rsp] { AttachPublishedSelfUbHealth(*this, rsp); });
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_VERIFY(akSkManager_, req), "AK/SK failed.");
    return gMigrateProc_->MigrateDataDirect(req, rsp);
}
#endif

#ifdef DS_OC_WRITE_SHM_REFERENCE_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(DecreaseReference, const DecreaseReferenceRequest &req, DecreaseReferenceResponse &resp)
{
    ScopedRequestContext ctx;
    Timer timer;
    std::string tenantId;
    Status authRc = req.is_routed() ? DS_OC_WRITE_REQUEST_TENANT(akSkManager_, req, tenantId)
                                    : DS_OC_WRITE_SESSION_TENANT(akSkManager_, req, tenantId);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(authRc, "Authenticate failed.");
    if (req.object_keys_size() > 0) {
        VLOG(1) << FormatString("[shmId %s] [client: %s] DoDecrease", req.object_keys(0), req.client_id());
    }
    std::vector<ShmKey> shmIds;
    // Although the field in pb is called object key, its content is actually shmId, which is very misleading.
    shmIds.reserve(req.object_keys().size());
    std::transform(req.object_keys().begin(), req.object_keys().end(), std::back_inserter(shmIds),
                   [](const auto &key) { return ShmKey::Intern(key); });
    auto rc = DecreaseMemoryRef(ClientKey::Intern(req.client_id()), shmIds, req.delay_release());
    if (rc.IsError()) {
        resp.mutable_error()->set_error_code(rc.GetCode());
        resp.mutable_error()->set_error_msg(rc.GetMsg());
    }
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(resp.error().error_code() == 0,
                                         static_cast<StatusCode>(resp.error().error_code()),
                                         "Decrease reference failed");
    GetWorkerTimeCost().Append("DecreaseReference", timer.ElapsedMilliSecond());
    VLOG(1) << FormatString("[Ref] DecreaseReference finish. The operations of worker DecreaseReference %s",
                            GetWorkerTimeCost().GetInfo());
    return Status::OK();
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(ReconcileShmRef, const ReconcileShmRefReqPb &req, ReconcileShmRefRspPb &resp)
{
    ScopedRequestContext ctx;
    Timer timer;
    std::string tenantId;
    bool exist;
    auto clientId = ClientKey::Intern(req.client_id());
    std::string authTenantId = worker::ClientManager::Instance().GetAuthTenantIdByClientId(clientId, exist);
    CHECK_FAIL_RETURN_STATUS(exist, K_INVALID, FormatString("Client %s not found", req.client_id()));
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_KNOWN_TENANT(akSkManager_, req, authTenantId, tenantId),
                                     "Authenticate failed");
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");

    std::vector<ShmKey> confirmedExpiredShmIds;
    confirmedExpiredShmIds.reserve(req.confirmed_expired_shm_ids_size());
    std::transform(req.confirmed_expired_shm_ids().begin(), req.confirmed_expired_shm_ids().end(),
                   std::back_inserter(confirmedExpiredShmIds), [](const auto &shmId) { return ShmKey::Intern(shmId); });
    std::vector<ShmKey> maybeExpiredShmIds;
    memoryRefTable_->ReconcileClientShmRefs(clientId, confirmedExpiredShmIds, maybeExpiredShmIds);
    for (const auto &shmId : maybeExpiredShmIds) {
        resp.add_maybe_expired_shm_ids(shmId);
    }
    GetWorkerTimeCost().Append("ReconcileShmRef", timer.ElapsedMilliSecond());
    return Status::OK();
}
#endif

#ifdef DS_OC_WRITE_INVALIDATE_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(InvalidateBuffer, const InvalidateBufferReqPb &req, InvalidateBufferRspPb &rsp)
{
    ScopedRequestContext ctx;
    Timer timer;
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    LOG(INFO) << FormatString("InvalidateBuffer begin, cliendId: %s, objectKey: %s", req.client_id(), req.object_key());
    (void)rsp;
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_key());

    std::shared_ptr<SafeObjType> entry;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(objectTable_->Get(namespaceUri, entry), "worker objecttable get entry failed");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(entry->WLock(true), "entry wlock failed");
    Raii unlock([&entry]() { entry->WUnlock(); });
    if (entry->Get() == nullptr) {
        LOG(WARNING) << FormatString("The memory of object %s has been removed, cannot invalidate.", namespaceUri);
        return Status::OK();
    }
    entry->Get()->stateInfo.SetCacheInvalid(true);
    if (entry->Get()->stateInfo.IsPrimaryCopy()) {
        std::list<std::string> objectKeysRemove{ namespaceUri };
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
            RemoveMetaFromMaster(objectKeysRemove, master::RemoveMetaReqPb::INVALID_BUFFER), "remove meta failed");
    }
    GetWorkerTimeCost().Append("Total InvalidateBuffer", timer.ElapsedMilliSecond());
    LOG(INFO) << FormatString("InvalidateBuffer end, clientId: %s. The operations of worker InvalidateBuffer %s",
                              req.client_id(), GetWorkerTimeCost().GetInfo());
    return Status::OK();
}
#endif

#ifdef DS_OC_WRITE_HASH_RING_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(GetHashRing, const GetHashRingReqPb &req, GetHashRingRspPb &rsp)
{
    ScopedRequestContext ctx;
    RETURN_RUNTIME_ERROR_IF_NULL(akSkManager_);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_VERIFY(akSkManager_, req), "AK/SK failed.");
    RETURN_RUNTIME_ERROR_IF_NULL(topologyEngine_);
    std::shared_ptr<const cluster::TopologySnapshot> snapshot;
    RETURN_IF_NOT_OK(membership_.GetSnapshot(snapshot));
    return BuildGetHashRingResponse(*snapshot, req.version(), FLAGS_master_address, rsp, req.host_ids_digest());
}
#endif

#ifdef DS_OC_WRITE_DEVICE_P2P_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(PublishDeviceObject, const PublishDeviceObjectReqPb &req, PublishDeviceObjectRspPb &resp,
                                                std::vector<RpcMessage> payloads)
{
    ScopedRequestContext ctx;
    std::string tenantId;
    auto clientId = ClientKey::Intern(req.client_id());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    std::vector<ShmKey> shmUnits = { ShmKey::Intern(req.shm_id()) };
    RETURN_IF_NOT_OK(
        WorkerOcServiceCrudCommonApi::CheckShmUnitByTenantId(tenantId, clientId, shmUnits, memoryRefTable_));
    PerfPoint point(PerfKey::WORKER_SEAL_OBJECT);
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    const std::string &objectKey = req.dev_object_key();
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, objectKey);

    LOG(INFO) << FormatString("DeviceObj[%s, Sz: %zu] is being publishing by client[%s]", objectKey, req.data_size(),
                              clientId);
    (void)resp;
    return workerDevOcManager_->PublishDeviceObject(namespaceUri, req, payloads);
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(GetDeviceObject,
    std::shared_ptr<ServerUnaryWriterReader<GetDeviceObjectRspPb, GetDeviceObjectReqPb>> serverApi)
{
    ScopedRequestContext ctx;
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    GetDeviceObjectReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(Validator::IsBatchSizeUnderLimit(req.device_object_keys_size()),
                                         StatusCode::K_INVALID, "invalid object size");

    auto clientId = ClientKey::Intern(req.client_id());
    auto objectKeys = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.device_object_keys());
    LOG(INFO) << "GetDeviceObject start from client:" << clientId << ", objects: " << VectorToString(objectKeys);
    int64_t subTimeout = req.sub_timeout();
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(Validator::IsInNonNegativeInt32(subTimeout), K_RUNTIME_ERROR,
                                         "SubTimeout is out of range.");

    int64_t remainingUs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTimeUs();
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(
        remainingUs > 0, K_RPC_DEADLINE_EXCEEDED,
        FormatString("RPC deadline exceeded before dispatch, remaining %ld us.", remainingUs));
    std::string traceID = Trace::Instance().GetTraceID();
    auto dispatchTime = std::chrono::steady_clock::now();
    threadPool_->Execute([objectKeys, serverApi, subTimeout, clientId, remainingUs, dispatchTime, this,
                          traceID]() mutable {
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceID);
        LOG(INFO) << "Processing GetDeviceObject, threads Statistics: " << threadPool_->GetStatistics();
        auto initRc = InitTimeoutsFromDispatch(remainingUs, dispatchTime);
        if (initRc.IsError()) {
            LOG(ERROR) << initRc.GetMsg();
            LOG_IF_ERROR(serverApi->SendStatus(initRc), "Send status failed");
            return;
        }
        workerDevOcManager_->ProcessGetDeviceObjectRequest(objectKeys, serverApi, subTimeout, clientId);
        LOG(INFO) << "Process GetDeviceObject done, threads Statistics: " << threadPool_->GetStatistics();
    });
    return Status::OK();
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(PutP2PMeta, const PutP2PMetaReqPb &req, PutP2PMetaRspPb &resp)
{
    ScopedRequestContext ctx;
    PerfPoint point(PerfKey::WORKER_PUT_P2PMETA);
    std::string tenantId;
    CHECK_FAIL_RETURN_STATUS(req.dev_obj_meta_size() > 0 && req.dev_obj_meta(0).locations_size() > 0, K_INVALID,
                             "The device meta and locations cannot be empty.");
    const auto clientId = ClientKey::Intern(req.dev_obj_meta(0).locations().begin()->client_id());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_CLIENT_TENANT(akSkManager_, req, tenantId, clientId),
                                     "Authenticate failed.");
    std::shared_ptr<WorkerMasterOCApi> workerMasterApi;
    RETURN_IF_NOT_OK(workerMasterApiManager_->GetWorkerMasterApi(P2P_DEFAULT_MASTER, workerMasterApi));
    PutP2PMetaReqPb reqCopy = req;
    reqCopy.set_worker_address(localAddress_.ToString());

    // Time complexity O(n), because in putp2pmeta, each obj_meta has only one location
    for (auto &dev_obj_meta : *reqCopy.mutable_dev_obj_meta()) {
        for (auto &location : *dev_obj_meta.mutable_locations()) {
            location.set_worker_ip(localAddress_.Host());
        }
    }
    return workerMasterApi->PutP2PMeta(reqCopy, resp);
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(SubscribeReceiveEvent,
    std::shared_ptr<ServerUnaryWriterReader<SubscribeReceiveEventRspPb, SubscribeReceiveEventReqPb>> serverApi)
{
    ScopedRequestContext ctx;
    PerfPoint point(PerfKey::WORKER_SUBSCRIBE_EVENT);
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    SubscribeReceiveEventReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    std::string tenantId;
    auto clientId = ClientKey::Intern(req.src_client_id());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_CLIENT_TENANT(akSkManager_, req, tenantId, clientId),
                                     "Authenticate failed.");
    int64_t remainingUs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTimeUs();
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(
        remainingUs > 0, K_RPC_DEADLINE_EXCEEDED,
        FormatString("RPC deadline exceeded before dispatch, remaining %ld us.", remainingUs));
    std::string traceID = Trace::Instance().GetTraceID();
    auto dispatchTime = std::chrono::steady_clock::now();
    devThreadPool_->Execute([=]() mutable {
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceID);
        LOG(INFO) << "Worker processes SubscribeReceiveEvent from srcClientId: " << req.src_client_id()
                  << ", src_device_id: " << req.src_device_id()
                  << ", threads Statistics: " << devThreadPool_->GetStatistics();
        auto initRc = InitTimeoutsFromDispatch(remainingUs, dispatchTime);
        if (initRc.IsError()) {
            LOG(ERROR) << initRc.GetMsg();
            LOG_IF_ERROR(serverApi->SendStatus(initRc), "Send status failed");
            return;
        }
        req.set_worker_ip(localAddress_.Host());
        LOG_IF_ERROR(workerDevOcManager_->ProcessSubscribeReceiveEventRequest(req, serverApi),
                     "Process SubscribeReceiveEvent failed");
        LOG(INFO) << "Process SubscribeReceiveEvent done";
    });
    return Status::OK();
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(GetP2PMeta,
    std::shared_ptr<ServerUnaryWriterReader<GetP2PMetaRspPb, GetP2PMetaReqPb>> serverApi)
{
    ScopedRequestContext ctx;
    PerfPoint point(PerfKey::WORKER_GET_P2PMEATA);
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    GetP2PMetaReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    std::string tenantId;
    CHECK_FAIL_RETURN_STATUS(req.dev_obj_meta_size() > 0 && req.dev_obj_meta(0).locations_size() > 0, K_INVALID,
                             "The device meta and locations cannot be empty.");
    const auto clientId = ClientKey::Intern(req.dev_obj_meta(0).locations().begin()->client_id());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_CLIENT_TENANT(akSkManager_, req, tenantId, clientId),
                                     "Authenticate failed.");
    int64_t remainingUs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTimeUs();
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(
        remainingUs > 0, K_RPC_DEADLINE_EXCEEDED,
        FormatString("RPC deadline exceeded before dispatch, remaining %ld us.", remainingUs));
    std::string traceID = Trace::Instance().GetTraceID();
    auto dispatchTime = std::chrono::steady_clock::now();
    devThreadPool_->Execute([=]() mutable {
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceID);
        std::stringstream allKeys;
        bool first = true;
        for (const auto &dev_obj_meta : *req.mutable_dev_obj_meta()) {
            if (!first) {
                allKeys << ", ";
            }
            allKeys << dev_obj_meta.object_key();
            first = false;
        }
        LOG(INFO) << FormatString("Worker processes GetP2PMeta from client: %s, allKeys: [%s], threads Statistics: %s",
                                  clientId, allKeys.str(), devThreadPool_->GetStatistics());
        auto initRc = InitTimeoutsFromDispatch(remainingUs, dispatchTime);
        if (initRc.IsError()) {
            LOG(ERROR) << initRc.GetMsg();
            LOG_IF_ERROR(serverApi->SendStatus(initRc), "Send status failed");
            return;
        }
        for (auto &dev_obj_meta : *req.mutable_dev_obj_meta()) {
            for (auto &location : *dev_obj_meta.mutable_locations()) {
                location.set_worker_ip(localAddress_.Host());
            }
        }
        req.set_worker_address(localAddress_.ToString());
        LOG_IF_ERROR(workerDevOcManager_->ProcessGetP2PMetaRequest(req, serverApi), "Process GetP2PMeta failed");
        LOG(INFO) << "Process GetP2PMeta done";
    });
    return Status::OK();
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(SendRootInfo, const SendRootInfoReqPb &req, SendRootInfoRspPb &resp)
{
    ScopedRequestContext ctx;
    std::string tenantId;
    auto clientId = ClientKey::Intern(req.dst_client_id());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_CLIENT_TENANT(akSkManager_, req, tenantId, clientId),
                                     "Authenticate failed.");
    std::shared_ptr<WorkerMasterOCApi> workerMasterApi;
    RETURN_IF_NOT_OK(workerMasterApiManager_->GetWorkerMasterApi(P2P_DEFAULT_MASTER, workerMasterApi));
    SendRootInfoReqPb reqCopy = req;
    return workerMasterApi->SendRootInfo(reqCopy, resp);
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(RecvRootInfo,
    std::shared_ptr<ServerUnaryWriterReader<RecvRootInfoRspPb, RecvRootInfoReqPb>> serverApi)
{
    ScopedRequestContext ctx;
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    RecvRootInfoReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    std::string tenantId;
    auto clientId = ClientKey::Intern(req.src_client_id());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_CLIENT_TENANT(akSkManager_, req, tenantId, clientId),
                                     "Authenticate failed.");
    int64_t remainingUs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTimeUs();
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(
        remainingUs > 0, K_RPC_DEADLINE_EXCEEDED,
        FormatString("RPC deadline exceeded before dispatch, remaining %ld us.", remainingUs));
    std::string traceID = Trace::Instance().GetTraceID();
    auto dispatchTime = std::chrono::steady_clock::now();
    devThreadPool_->Execute([=]() mutable {
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceID);
        LOG(INFO) << "Worker processes RecvRootInfo from srcClientId: " << req.src_device_id()
                  << ", src_device_id: " << req.src_device_id()
                  << ", threads Statistics: " << devThreadPool_->GetStatistics();
        auto initRc = InitTimeoutsFromDispatch(remainingUs, dispatchTime);
        if (initRc.IsError()) {
            LOG(ERROR) << initRc.GetMsg();
            LOG_IF_ERROR(serverApi->SendStatus(initRc), "Send status failed");
            return;
        }
        LOG_IF_ERROR(workerDevOcManager_->ProcessRecvRootInfoRequest(req, serverApi),
                     "Process RecvRootInfo failed");
        LOG(INFO) << "Process RecvRootInfo done";
    });
    return Status::OK();
}

Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(AckRecvFinish, const AckRecvFinishReqPb &req, AckRecvFinishRspPb &resp)
{
    ScopedRequestContext ctx;
    std::string tenantId;
    auto clientId = ClientKey::Intern(req.dst_client_id());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_CLIENT_TENANT(akSkManager_, req, tenantId, clientId),
                                     "Authenticate failed.");
    std::shared_ptr<WorkerMasterOCApi> workerMasterApi;
    RETURN_IF_NOT_OK(workerMasterApiManager_->GetWorkerMasterApi(P2P_DEFAULT_MASTER, workerMasterApi));
    AckRecvFinishReqPb reqCopy = req;
    return workerMasterApi->AckRecvFinish(reqCopy, resp);
}
#endif

#ifdef DS_OC_WRITE_DATA_INFO_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(GetDataInfo,
    std::shared_ptr<ServerUnaryWriterReader<GetDataInfoRspPb, GetDataInfoReqPb>> serverApi)
{
    ScopedRequestContext ctx;
    BthreadReadGuard noReconciliation;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerState(noReconciliation, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    GetDataInfoReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_WRITE_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    auto objectKey = req.object_key();
    int64_t subTimeout = req.sub_timeout();
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(Validator::IsInNonNegativeInt32(subTimeout), K_RUNTIME_ERROR,
                                         "SubTimeout is out of range.");
    int64_t remainingUs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTimeUs();
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(
        remainingUs > 0, K_RPC_DEADLINE_EXCEEDED,
        FormatString("RPC deadline exceeded before dispatch, remaining %ld us.", remainingUs));
    std::string traceID = Trace::Instance().GetTraceID();
    auto dispatchTime = std::chrono::steady_clock::now();
    devThreadPool_->Execute([=]() mutable {
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceID);
        LOG(INFO) << "Worker processes GetDataInfo from object: " << objectKey
                  << ", threads Statistics: " << devThreadPool_->GetStatistics();
        auto initRc = InitTimeoutsFromDispatch(remainingUs, dispatchTime);
        if (initRc.IsError()) {
            LOG(ERROR) << initRc.GetMsg();
            LOG_IF_ERROR(serverApi->SendStatus(initRc), "Send status failed");
            return;
        }
        LOG_IF_ERROR(workerDevOcManager_->ProcessGetDataInfoRequest(req, serverApi, subTimeout),
                     "Process GetDataInfo failed");
        LOG(INFO) << "Process GetDataInfo done";
    });
    return Status::OK();
}
#endif

#ifdef DS_OC_WRITE_QUERY_GET_BODY
Status WorkerOCServiceImpl::DS_OC_WRITE_ENTRY(QueryAndGet,
    std::shared_ptr<ServerUnaryWriterReader<QueryAndGetRspPb, QueryAndGetReqPb>> serverApi)
{
    ScopedRequestContext ctx;
    BthreadReadGuard noRecon;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        ValidateWorkerStateForRequest(noRecon, GetRequestContext()->reqTimeoutDuration.CalcRemainingTime()),
        "validate worker state failed");
    RETURN_RUNTIME_ERROR_IF_NULL(queryAndGetProc_);
    return queryAndGetProc_->DS_OC_WRITE_FORWARD(QueryAndGet, serverApi);
}
#endif
