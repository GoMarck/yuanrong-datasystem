/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#ifdef DS_MW_RPC_GROUP_0
Status MasterWorkerOCServiceImpl::DS_MW_RPC_ENTRY(UpdateNotification,
    const UpdateObjectReqPb &reqs, UpdateObjectRspPb &rsp)
{
    ScopedRequestContext ctx;
    INJECT_POINT("worker.UpdateNotification.begin");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MW_RPC_VERIFY(akSkManager_, reqs), "AK/SK failed.");
    LOG(INFO) << "Received UpdateNotification request";
    std::vector<std::string> updateFailIds;
    for (const UpdateObjectInfoPb &req : reqs.object_infos()) {
        Status rc = UpdateSingleNotification(req, reqs.sync());
        if (rc.GetCode() == StatusCode::K_WORKER_TIMEOUT) {
            LOG(ERROR) << FormatString("[ObjectKey %s] Cache invalidation meet a deadlock, rollback.",
                                       req.object_key());
            return rc;
        } else if (rc.IsError()) {
            updateFailIds.emplace_back(req.object_key());
        }
    }
    *rsp.mutable_failed_ids() = { updateFailIds.begin(), updateFailIds.end() };
    INJECT_POINT("MasterWorkerOCServiceImpl.UpdateNotification.retry");
    LOG(INFO) << "UpdateNotification done";
    return Status::OK();
}

#endif

#ifdef DS_MW_RPC_GROUP_1
Status MasterWorkerOCServiceImpl::DS_MW_RPC_ENTRY(PublishMeta,
    const PublishMetaReqPb &req, PublishMetaRspPb &resp)
{
    ScopedRequestContext ctx;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    (void)resp;
    LOG(INFO) << FormatString("[ObjectKey %s] Publish meta for object", req.meta().object_key());
    auto traceID = Trace::Instance().GetTraceID();
    // The asynchronous pull belongs to the subscriber and must not inherit the metadata notification RPC deadline.
    const int64_t remainingUs =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::milliseconds(req.timeout())).count();
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(remainingUs > 0, K_RPC_DEADLINE_EXCEEDED,
        FormatString("Subscription deadline exceeded before PublishMeta dispatch, remaining %ld us.", remainingUs));
    auto dispatchTime = std::chrono::steady_clock::now();
    ocClientWorkerSvc_->threadPool_->Execute([this, req, traceID, remainingUs, dispatchTime] {
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceID);
        auto initRc = InitTimeoutsFromDispatch(remainingUs, dispatchTime);
        if (initRc.IsError()) {
            LOG(ERROR) << FormatString("[ObjectKey %s] PublishMeta dropped: %s",
                                       req.meta().object_key(), initRc.GetMsg());
            return;
        }
        Raii outerResetDuration([]() { GetRequestContext()->timeoutDuration.Reset(); });
        Status rc = Status::OK();
        master::QueryMetaInfoPb queryMeta;
        std::vector<RpcMessage> payloads;
        queryMeta.mutable_meta()->CopyFrom(req.meta());
        queryMeta.set_address(req.address());
        ReadKey readKey(queryMeta.meta().object_key());
        RetryGetObjectFromRemote(readKey, queryMeta, payloads);
    });
    return Status::OK();
}

#endif

#ifdef DS_MW_RPC_GROUP_2
Status MasterWorkerOCServiceImpl::DS_MW_RPC_ENTRY(DeleteNotification,
    const DeleteObjectReqPb &req, DeleteObjectRspPb &rsp)
{
    ScopedRequestContext ctx;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    VLOG(1) << FormatString("DeleteNotification, async: %d, object count: %d",
              req.is_async(), req.object_keys_size());
    if (req.is_async() && !req.require_sync_version_fence()) {
        auto traceID = Trace::Instance().GetTraceID();
        ocClientWorkerSvc_->threadPool_->Execute([this, req, traceID] {
            TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceID);
            DeleteObjectRspPb rsp;
            auto rc = ocClientWorkerSvc_->DeleteCopyNotification(req, rsp);
            if (rc.IsError()) {
                LOG(ERROR) << FormatString("DeleteCopyNotification failed: %s", rc.ToString());
            }
        });
    } else {
        DeleteObjectReqPb syncReq = req;
        syncReq.set_is_async(false);
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ocClientWorkerSvc_->DeleteCopyNotification(syncReq, rsp),
                                         "woker DeleteCopyNotification failed");
        rsp.set_sync_version_fence_applied(req.require_sync_version_fence());
    }
    INJECT_POINT("MasterWorkerOCServiceImpl.DeleteNotification.retry");
    VLOG(1) << "DeleteNotification done";
    return Status::OK();
}

Status MasterWorkerOCServiceImpl::DS_MW_RPC_ENTRY(DeletePersistenceObject,
    const DeletePersistenceObjectReqPb &req,
                                                          DeletePersistenceObjectRspPb &rsp)
{
    ScopedRequestContext ctx;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    LOG(INFO) << "Delete persistence object: " << req.object_key();
    auto rc = ocClientWorkerSvc_->DeletePersistenceObject(req, rsp);
    rsp.mutable_last_rc()->set_error_code(rc.GetCode());
    rsp.mutable_last_rc()->set_error_msg(rc.GetMsg());
    VLOG(1) << "Delete persistence object done";
    return Status::OK();
}

Status MasterWorkerOCServiceImpl::DS_MW_RPC_ENTRY(QueryGlobalRefNumOnWorker,
    const QueryGlobalRefNumReqPb &req,
                                                            QueryGlobalRefNumRspPb &rsp)
{
    ScopedRequestContext ctx;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    LOG(INFO) << "[GRef] Query All Objs Global References Rpc Received";
    std::unordered_map<std::string, std::unordered_set<ClientKey>> refTable;
    ocClientWorkerSvc_->globalRefTable_->GetAllRef(refTable);
    if (req.object_keys_size() == 0) {
        // Query All objects
        LOG(INFO) << "[GRef] Query All Objs Global References:";
        for (const auto &entry : refTable) {
            GRefDistributionPb entryPb;
            std::string objectKey;
            TenantAuthManager::Instance()->NamespaceUriToObjectKey(entry.first, objectKey);
            entryPb.set_object_key(objectKey);
            *entryPb.mutable_referred_addr() = { entry.second.begin(), entry.second.end() };

            LOG(INFO) << "[GRef] Obj: " << entry.first << "is referenced by: " << entryPb.referred_addr().data();

            rsp.add_objs_glb_ref()->CopyFrom(entryPb);
        }
    } else {
        // Query specific objects
        for (const auto &id : req.object_keys()) {
            // Find each object ref and serialize to pb
            LOG(INFO) << "[GRef] Query Given Objs Global References:";
            auto iter = refTable.find(id);
            if (iter == refTable.end()) {
                LOG(ERROR) << "[GRef] Obj:" << id << "is not referenced by any runtime.";
                continue;
            }
            GRefDistributionPb entryPb;
            std::string objectKey;
            TenantAuthManager::Instance()->NamespaceUriToObjectKey(iter->first, objectKey);
            entryPb.set_object_key(objectKey);
            *entryPb.mutable_referred_addr() = { iter->second.begin(), iter->second.end() };
            LOG(INFO) << "[GRef] Obj: " << iter->first << "is referenced by: " << entryPb.referred_addr().data();
            rsp.add_objs_glb_ref()->CopyFrom(entryPb);
        }
    }
    return Status::OK();
}

#endif

#ifdef DS_MW_RPC_GROUP_3
Status MasterWorkerOCServiceImpl::DS_MW_RPC_ENTRY(PushMetaToWorker,
    const PushMetaToWorkerReqPb &req, PushMetaToWorkerRspPb &rsp)
{
    ScopedRequestContext ctx;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    (void)rsp;
    LOG(INFO) << "Worker receive PushMetaToWorker request from master: " << req.source_address();
    for (auto &cacheInvalid : req.cache_invalids()) {
        LOG(INFO) << FormatString("Receive PushMetaToWorker cache invalid for object:%s", cacheInvalid.object_key());
        Status rc = UpdateSingleNotification(cacheInvalid);
        // If we can not found the object, we just ignore it.
        if (rc.GetCode() == StatusCode::K_NOT_FOUND) {
            rc = Status::OK();
        }
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(rc, "worker update notification failed");
    }

    for (const auto &id : req.primary_copy_invalid_ids()) {
        LOG(INFO) << FormatString("Receive PushMetaToWorker cache invalid for object:%s", id);
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ProcessChangePrimaryCopy(id, false), "worker change primary copy failed");
    }

    DeleteObjectReqPb delReq;
    DeleteObjectRspPb delRsp;
    delReq.mutable_object_keys()->CopyFrom(req.delete_object_keys());
    for (int i = 0; i < delReq.object_keys_size(); ++i) {
        delReq.add_versions(std::numeric_limits<uint64_t>::max());
    }
    delReq.set_is_async(true);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(akSkManager_->GenerateSignature(delReq), "GenerateSignature failed");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DeleteNotification(delReq, delRsp), "worker delete notification failed");
    // We have received the metadata from master, lets reconciliation its data with
    // reconnected client and notify master to remove the not exist client metadata.
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ocClientWorkerSvc_->Reconciliation(req), "worker reconciliation failed");
    LOG(INFO) << "PushMetaToWorker done";
    return Status::OK();
}

Status MasterWorkerOCServiceImpl::DS_MW_RPC_ENTRY(RequestMetaFromWorker,
    const RequestMetaFromWorkerReqPb &req,
                                                        RequestMetaFromWorkerRspPb &rsp)
{
    ScopedRequestContext ctx;
    LOG(INFO) << FormatString("worker(%s) received RequestMetaFromWorker request: %s", localAddress_.ToString(),
                              req.address());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ocClientWorkerSvc_->FillRequestMetaByMaster(req, rsp),
                                     "worker FillRequestMetaByMaster failed");
    LOG(INFO) << "RequestMetaFromWorker done";
    return Status::OK();
}

#endif

#ifdef DS_MW_RPC_GROUP_4
Status MasterWorkerOCServiceImpl::DS_MW_RPC_ENTRY(ChangePrimaryCopy,
    const ChangePrimaryCopyReqPb &req, ChangePrimaryCopyRspPb &rsp)
{
    ScopedRequestContext ctx;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    LOG(INFO) << "worker(" << localAddress_.ToString() << ") receive ChangePrimaryCopy request";
    // If migrate data task (Scale-In) has been started, we should not change primary copy in this node.
    if (ocClientWorkerSvc_->MigrateDataStarted()) {
        std::stringstream ss;
        ss << "worker(" << localAddress_.ToString() << ") is exiting, cannot accept change primary copy request";
        LOG(INFO) << ss.str();
        RETURN_STATUS(StatusCode::K_NOT_READY, ss.str());
    }
    for (const auto &id : req.object_keys()) {
        Status rc = ProcessChangePrimaryCopy(id, true);
        if (rc.IsError()) {
            LOG(INFO) << "worker ChangePrimaryCopy failed: " << rc.GetMsg();
            continue;
        }
        rsp.add_success_ids(id);
    }
    return Status::OK();
}

Status MasterWorkerOCServiceImpl::DS_MW_RPC_ENTRY(NotifyMasterIncNestedRefs,
    const NotifyMasterIncNestedReqPb &req,
                                                            NotifyMasterIncNestedResPb &rsp)
{
    ScopedRequestContext ctx;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    (void)rsp;  // empty response
    std::vector<std::string> nested_objs(req.nested_object_keys().begin(), req.nested_object_keys().end());
    LOG(INFO) << "worker received NotifyMasterIncNestedRefs, nested objectkeys: " << VectorToString(nested_objs);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ocClientWorkerSvc_->IncNestedRef(nested_objs), "worker IncNestedRef failed");
    LOG(INFO) << "worker NotifyMasterIncNestedRefs done";
    return Status::OK();
}

Status MasterWorkerOCServiceImpl::DS_MW_RPC_ENTRY(NotifyMasterDecNestedRefs,
    const NotifyMasterDecNestedReqPb &req,
                                                            NotifyMasterDecNestedResPb &rsp)
{
    ScopedRequestContext ctx;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MW_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    (void)rsp;  // empty response
    std::vector<std::string> nested_objs(req.nested_object_keys().begin(), req.nested_object_keys().end());
    LOG(INFO) << "worker received NotifyMasterDecNestedRefs, nested objectkeys: " << VectorToString(nested_objs);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ocClientWorkerSvc_->DecNestedRef(nested_objs), "worker DecNestedRef failed");
    LOG(INFO) << "worker NotifyMasterDecNestedRefs done";
    return Status::OK();
}

#endif
