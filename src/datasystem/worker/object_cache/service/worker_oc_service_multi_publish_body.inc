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

Status WorkerOcServiceMultiPublishImpl::DS_OC_PROC_ENTRY(MultiPublish, const MultiPublishReqPb &req, MultiPublishRspPb &resp,
                                                     std::vector<RpcMessage> &payloads, const ClientKey &clientId)
{
    ScopedRequestContext ctx;
    Timer timer;
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_MULTIPUBLISH);
    access
        .ObjectKeyProvider([&req]() -> std::string {
            return req.object_info().empty() ? std::string() : req.object_info(0).object_key();
        })
        .DataSizeProvider(
            [&req]() -> uint64_t { return req.object_info().empty() ? 0 : req.object_info(0).data_size(); });
    if (req.object_info().empty()) {
        Status rc(K_INVALID, __LINE__, __FILE__, "The list of object info is empty.");
        access.Result(rc).Record();
        return rc;
    }
    Status status = DS_OC_PROC_FORWARD(MultiPublishImpl, req, resp, payloads, clientId);
    access.Result(status).Record();
    auto totalMs = timer.ElapsedMilliSecond();
    GetWorkerTimeCost().Append("Total MultiPublish", totalMs);
    auto vlogLevel = (totalMs > 1 || status.IsError()) ? 0 : 1;
    VLOG(vlogLevel) << FormatString("MultiPublish done, cost: %.1fms, %s", totalMs, GetWorkerTimeCost().GetInfo());
    return status;
}

Status WorkerOcServiceMultiPublishImpl::DS_OC_PROC_ENTRY(MultiPublishImpl, const MultiPublishReqPb &req, MultiPublishRspPb &resp,
                                                         std::vector<RpcMessage> &payloads, const ClientKey &clientId)
{
    INJECT_POINT("worker.before_CreateMultiMetaToMaster");
    PerfPoint pointAll(PerfKey::WORKER_MULTI_PUBLISH_TOTAL);
    PerfPoint point(PerfKey::WORKER_MULTI_PUBLISH_INPUT_CHECK);
    // Add namespace for objectKey.
    std::string tenantId;
    Status authRc = req.is_routed() ? DS_OC_REQUEST_TENANT(akSkManager_, req, tenantId)
                                    : DS_OC_SESSION_TENANT(akSkManager_, req, tenantId);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(authRc, "Authenticate failed.");
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(Validator::IsBatchSizeUnderLimit(req.object_info_size()),
                                         StatusCode::K_INVALID,
                                         "The objectKeys size exceed " + std::to_string(OBJECT_KEYS_MAX_SIZE_LIMIT));
    std::vector<ShmKey> shmUnits;
    shmUnits.reserve(req.object_info_size());
    for (const auto &info : req.object_info()) {
        if (!info.shm_id().empty()) {
            shmUnits.emplace_back(ShmKey::Intern(info.shm_id()));
        }
    }
    RETURN_IF_NOT_OK(
        WorkerOcServiceCrudCommonApi::CheckShmUnitByTenantId(tenantId, clientId, shmUnits, memoryRefTable_));

    RETURN_IF_NOT_OK(CheckIfL2CacheNeededAndWritable(supportL2Storage_, WriteMode(req.write_mode())));

    std::vector<std::string> namespaceUri;
    size_t objectSize = static_cast<size_t>(req.object_info_size());
    namespaceUri.reserve(objectSize);
    Status checkStatus = Status::OK();
    for (size_t i = 0; i < objectSize; ++i) {
        namespaceUri.emplace_back(
            TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_info(i).object_key()));
    }

    VLOG(1) << FormatString("Process multi pub, cacheType: %d", req.cache_type());
    RETURN_IF_NOT_OK(VerifyDuplicateKeys(namespaceUri));
    VLOG(1) << "Multi publish with key : " << VectorToString(namespaceUri);

    // Insert entry to object table and get lock.
    std::vector<bool> ifInserts(namespaceUri.size(), false);
    std::vector<std::shared_ptr<SafeObjType>> entries(namespaceUri.size(), nullptr);
    point.RecordAndReset(PerfKey::WORKER_MULTI_PUBLISH_NTX);
    return MultiPublishNtx(namespaceUri, entries, ifInserts, req, resp, payloads);
}
