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
