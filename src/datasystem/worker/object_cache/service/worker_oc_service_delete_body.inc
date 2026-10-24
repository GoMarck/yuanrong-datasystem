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

Status WorkerOcServiceDeleteImpl::DS_OC_PROC_ENTRY(DeleteAllCopy, const DeleteAllCopyReqPb &req, DeleteAllCopyRspPb &resp)
{
    ScopedRequestContext ctx;
    Timer timer;
    uint64_t deletedSize = 0;
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_DELETE_ALL_COPY);
    access.ObjectKeysRef(req.object_keys()).DataSizeProvider([&deletedSize]() -> uint64_t { return deletedSize; });
    Status rc = DS_OC_PROC_FORWARD(DeleteAllCopyImpl, req, resp, deletedSize);
    access.Result(rc).Record();
    GetWorkerTimeCost().Append("Total DeleteAllCopy", timer.ElapsedMilliSecond());
    LOG(INFO) << "DeleteAllCopy finish with request size: " << req.object_keys_size()
              << ", failed size: " << resp.fail_object_keys_size() << ", the operation cost "
              << GetWorkerTimeCost().GetInfo();
    return rc;
}

Status WorkerOcServiceDeleteImpl::DS_OC_PROC_ENTRY(DeleteAllCopyImpl, const DeleteAllCopyReqPb &req, DeleteAllCopyRspPb &resp,
                                                    uint64_t &deletedSize)
{
    Timer timerCost;
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(Validator::IsBatchSizeUnderLimit(req.object_keys_size()),
                                         StatusCode::K_INVALID, "invalid object size");
    auto needDeleteObjs = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_keys());
    LOG(INFO) << "DeleteAllCopy request: client_id: " << req.client_id()
              << ", objectKeys: " << VectorToString(needDeleteObjs)
              << " before delete elapsed ms: " << timerCost.ElapsedMilliSecond();

    std::vector<std::string> failedObjectKeys;
    Status rc = RetryWhenDeadlock([this, &needDeleteObjs, &failedObjectKeys, &deletedSize] {
        if (GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTime() <= 0) {
            return Status(K_RPC_DEADLINE_EXCEEDED, "Rpc timeout");
        }
        if (!failedObjectKeys.empty()) {
            needDeleteObjs = std::move(failedObjectKeys);
        }
        return DeleteAllCopyWithLock(needDeleteObjs, failedObjectKeys, deletedSize);
    });
    VLOG(1) << FormatString("The total size of the current deletion is %llu", deletedSize);
    if (!failedObjectKeys.empty()) {
        LOG(WARNING) << "Delete failed list: " << VectorToString(failedObjectKeys);
        for (auto &objectKey : failedObjectKeys) {
            std::string failedId;
            TenantAuthManager::Instance()->NamespaceUriToObjectKey(objectKey, failedId);
            resp.add_fail_object_keys(failedId);
        }
    }
    resp.mutable_last_rc()->set_error_code(rc.GetCode());
    resp.mutable_last_rc()->set_error_msg(rc.GetMsg());
    if (rc.GetCode() == K_RPC_DEADLINE_EXCEEDED) {
        return rc;
    }
    return Status::OK();
}
