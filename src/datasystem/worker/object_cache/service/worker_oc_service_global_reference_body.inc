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

Status WorkerOcServiceGlobalReferenceImpl::DS_OC_PROC_ENTRY(GIncreaseRef, const GIncreaseReqPb &req, GIncreaseRspPb &resp)
{
    if (!req.remote_client_id().empty()) {
        return DS_OC_PROC_FORWARD(GIncreaseRefWithRemoteClientId, req, resp);
    }
    ScopedRequestContext ctx;
    Timer timer;
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_GINCREASEREF);
    access.ObjectKeysRef(req.object_keys());
    std::string tenantId;
    Status authRc = DS_OC_SESSION_TENANT(akSkManager_, req, tenantId);
    if (authRc.IsError()) {
        LOG(ERROR) << "Authenticate failed. Detail: " << authRc.ToString();
        access.Result(authRc).Record();
        return authRc;
    }
    if (!Validator::IsBatchSizeUnderLimit(req.object_keys_size())) {
        LOG(ERROR) << "invalid object size";
        Status rc(StatusCode::K_INVALID, __LINE__, __FILE__, "invalid object size");
        access.Result(rc).Record();
        return rc;
    }
    auto objectKeys = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_keys());
    access.ObjectKeysRef(objectKeys);
    LOG(INFO) << "[Ref] GIncreaseRef client id: " << req.address() << ", object list: " << VectorToString(objectKeys);
    INJECT_POINT("worker.GIncrease_ref_failure");
    std::vector<std::string> failIncIds;
    Status lastErr = RetryGIncreaseRef(req, objectKeys, failIncIds);
    FillFailedObjectKeys(resp, failIncIds);
    resp.mutable_last_rc()->set_error_code(lastErr.GetCode());
    resp.mutable_last_rc()->set_error_msg(lastErr.GetMsg());
    access.Result(lastErr).Record();
    GetWorkerTimeCost().Append("Total GIncreaseRef", timer.ElapsedMilliSecond());

    bool partlySuccess = !failIncIds.empty() && (objectKeys.size() > failIncIds.size());
    LOG(INFO) << FormatString("[Ref] GIncreaseRef finish with status: %s. The operations of worker GIncreaseRef %s",
                              lastErr.ToString(), GetWorkerTimeCost().GetInfo());
    return partlySuccess ? Status::OK() : lastErr;
}

Status WorkerOcServiceGlobalReferenceImpl::DS_OC_PROC_ENTRY(GDecreaseRef, const GDecreaseReqPb &req, GDecreaseRspPb &resp)
{
    ScopedRequestContext ctx;
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(Validator::IsBatchSizeUnderLimit(req.object_keys_size()),
                                         StatusCode::K_INVALID, "invalid object size");
    auto objectKeys = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_keys());
    LOG(INFO) << "[Ref] GDecreaseRef client id: " << req.address() << ", object list: " << VectorToString(objectKeys);
    INJECT_POINT("worker.GDecreaseRef.before");
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_GDECREASEREF);
    access.ObjectKeysRef(objectKeys).RemoteClientId(req.remote_client_id());
    std::vector<std::string> failedObjectKeys;
    Status rc = RetryWhenDeadlock([this, &objectKeys, &req, &failedObjectKeys] {
        if (!failedObjectKeys.empty()) {
            objectKeys = std::move(failedObjectKeys);
        }
        if (!req.remote_client_id().empty()) {
            return GDecreaseRefWithLockWithRemoteClientId(objectKeys, req.remote_client_id(), failedObjectKeys);
        }
        return GDecreaseRefWithLock(objectKeys, ClientKey::Intern(req.client_id()), failedObjectKeys);
    });

    if (!failedObjectKeys.empty()) {
        LOG(WARNING) << "[Ref] GDecreaseRef failed list: " << VectorToString(failedObjectKeys);
        for (auto &objectKey : failedObjectKeys) {
            std::string failedId;
            TenantAuthManager::Instance()->NamespaceUriToObjectKey(objectKey, failedId);
            resp.add_failed_object_keys(failedId);
        }
    }
    resp.mutable_last_rc()->set_error_code(rc.GetCode());
    resp.mutable_last_rc()->set_error_msg(rc.GetMsg());
    access.Result(rc).Record();
    LOG(INFO) << FormatString("[Ref] GDecreaseRef finish with status: %s. The operations %s", rc.ToString(),
                              GetWorkerTimeCost().GetInfo());
    return Status::OK();
}

Status WorkerOcServiceGlobalReferenceImpl::DS_OC_PROC_ENTRY(ReleaseGRefs, const ReleaseGRefsReqPb &req, ReleaseGRefsRspPb &resp)
{
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    LOG(INFO) << "[Ref] ReleaseGRefs object list: " << req.remote_client_id();
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_RELEASEGREFS);
    access.RemoteClientId(req.remote_client_id());
    HostPort masterAddr;
    if (metadataRouteResolver_ == nullptr || endpointPolicy_ == nullptr) {
        RETURN_STATUS(StatusCode::K_NOT_FOUND, "ETCD cluster manager is not provided");
    }
    RETURN_IF_NOT_OK(metadataRouteResolver_->ResolveOwner(req.remote_client_id(), masterAddr));
    if (masterAddr != localAddress_) {
        RETURN_IF_NOT_OK(endpointPolicy_->CheckEndpoint(masterAddr, allowDirectoryLag_));
    }
    auto api = workerMasterApiManager_->GetWorkerMasterApi(masterAddr);
    CHECK_FAIL_RETURN_STATUS(api != nullptr, StatusCode::K_INVALID,
                             "Getting master api failed. masterAddrs: " + masterAddr.ToString());
    master::ReleaseGRefsReqPb releaseReq;
    releaseReq.set_address(localAddress_.ToString());
    releaseReq.set_remote_client_id(req.remote_client_id());
    master::ReleaseGRefsRspPb releaseRsp;
    Status rc = api->ReleaseGRefs(releaseReq, releaseRsp);
    resp.mutable_last_rc()->set_error_code(rc.GetCode());
    resp.mutable_last_rc()->set_error_msg(rc.GetMsg());
    access.Result(rc).Record();
    LOG(INFO) << FormatString("[Ref] ReleaseGRefs finish with status: %s", rc.ToString());
    return Status::OK();
}

Status WorkerOcServiceGlobalReferenceImpl::DS_OC_PROC_ENTRY(QueryGlobalRefNum, const QueryGlobalRefNumReqPb &req,
                                                             QueryGlobalRefNumRspCollectionPb &rsp)
{
    ScopedRequestContext ctx;
    Timer timer;
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(Validator::IsBatchSizeUnderLimit(req.object_keys_size()),
                                         StatusCode::K_INVALID, "invalid object size");
    auto namespaceUris = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_keys());
    LOG(INFO) << "worker qurey global ref number begin, namespaceUris: " << VectorToString(namespaceUris);
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_QUERY_GLOBAL_REF_NUM);
    access.ObjectKeysRef(namespaceUris);
    // Group ObjectKeys by master
    auto grouped = metadataRouteResolver_->GroupOwners(namespaceUris);
    AppendRouteFailures(grouped);
    auto &objKeysGrpByMaster = grouped.groups;
    // Send requests for each master
    for (auto &item : objKeysGrpByMaster) {
        const HostPort &masterAddr = item.first;
        std::vector<std::string> &currentObjectKeys = item.second;
        std::shared_ptr<WorkerMasterOCApi> workerMasterApi = workerMasterApiManager_->GetWorkerMasterApi(masterAddr);
        if (workerMasterApi == nullptr) {
            Status rc(K_RUNTIME_ERROR, __LINE__, __FILE__, "hash master get failed, QueryGlobal failed");
            access.Result(rc).Record();
            return rc;
        }
        QueryGlobalRefNumReqPb currentReq;
        currentReq.set_redirect(true);
        *currentReq.mutable_object_keys() = { currentObjectKeys.begin(), currentObjectKeys.end() };
        std::function<Status(QueryGlobalRefNumReqPb &, QueryGlobalRefNumRspCollectionPb &)> func =
            [&workerMasterApi](QueryGlobalRefNumReqPb &req, QueryGlobalRefNumRspCollectionPb &rsp) {
                return workerMasterApi->QueryGlobalRefNum(req, rsp);
            };
        std::function<void(QueryGlobalRefNumRspCollectionPb &, QueryGlobalRefNumRspCollectionPb &)> mergeFunc =
            [](QueryGlobalRefNumRspCollectionPb &srcRsp, QueryGlobalRefNumRspCollectionPb &destRsp) {
                destRsp.mutable_objs_glb_refs()->MergeFrom(srcRsp.objs_glb_refs());
            };
        Status rc = WaitForRedirectWhenRefMoving(currentReq, rsp, workerMasterApi, func, mergeFunc);
        if (rc.IsError()) {
            access.Result(rc).Record();
            LOG(ERROR) << "workerMasterApi qurey global ref number failed. master addr: " << masterAddr.ToString();
            return rc;
        }
    }
    access.Result(StatusCode::K_OK).Record();
    GetWorkerTimeCost().Append("Total QueryGlobalRefNum", timer.ElapsedMilliSecond());
    LOG(INFO) << FormatString(
        "worker query global ref number end, namespaceUris: %s. The operations of worker QueryGlobalRefNum %s",
        VectorToString(namespaceUris), GetWorkerTimeCost().GetInfo());
    return Status::OK();
}

Status WorkerOcServiceGlobalReferenceImpl::DS_OC_PROC_ENTRY(GIncreaseRefWithRemoteClientId, const GIncreaseReqPb &req,
                                                                          GIncreaseRspPb &resp)
{
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    ScopedRequestContext ctx;
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_GINCREASEREF);
    INJECT_POINT("worker.GIncrease_ref_failure");
    auto objectKeys = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_keys());
    access.ObjectKeysRef(objectKeys).RemoteClientId(req.remote_client_id());
    LOG(INFO) << "[Ref] GIncreaseRefWithRemoteClientId object list: " << VectorToString(objectKeys)
              << ", remoteClientId:" << req.remote_client_id();
    std::vector<std::string> failIncIds;
    Status lastErr;
    Status status = GIncreaseMasterRef(req, objectKeys, failIncIds);
    if (status.IsError()) {
        lastErr = status;
    } else if (!failIncIds.empty()) {
        lastErr = Status(StatusCode::K_RUNTIME_ERROR, "failIncIds is not empty.");
    }
    if (!failIncIds.empty()) {
        for (const auto &objectKey : failIncIds) {
            std::string failedId;
            TenantAuthManager::Instance()->NamespaceUriToObjectKey(objectKey, failedId);
            resp.add_failed_object_keys(failedId);
        }
    }
    resp.mutable_last_rc()->set_error_code(lastErr.GetCode());
    resp.mutable_last_rc()->set_error_msg(lastErr.GetMsg());

    bool partlySuccess = !failIncIds.empty() && (objectKeys.size() > failIncIds.size());
    access.Result(lastErr).Record();
    LOG(INFO) << FormatString("[Ref] GIncreaseRefWithRemoteClientId finish with status: %s, The operations %s",
                              lastErr.ToString(), GetWorkerTimeCost().GetInfo());
    return partlySuccess ? Status::OK() : lastErr;
}
