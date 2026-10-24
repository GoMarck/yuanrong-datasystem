/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
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
Status WaitExpireTasks(std::vector<std::future<Status>> &futures, ObjectAccessRecorder &access)
{
    for (auto &future : futures) {
        future.wait();
        Status rc = future.get();
        if (rc.IsError()) {
            access.Result(rc).Record();
            return rc;
        }
    }
    return Status::OK();
}

void FinishExpireRequest(const worker::MetaOwnerRouteGroups &grouped, std::vector<std::string> &absentObjectKeys,
                         std::unordered_set<std::string> &failedObjectKeys, ExpireRspPb &rsp,
                         ObjectAccessRecorder &access, const Status &status, Timer &timer)
{
    for (const auto &failure : grouped.failures) {
        absentObjectKeys.emplace_back(ExtractObjectId(failure.first));
    }
    failedObjectKeys.insert(absentObjectKeys.begin(), absentObjectKeys.end());
    *rsp.mutable_failed_object_keys() = { failedObjectKeys.begin(), failedObjectKeys.end() };
    access.Result(status).Record();
    GetWorkerTimeCost().Append("Total Expire", static_cast<int64_t>(timer.ElapsedMilliSecond()));
    LOG(INFO) << FormatString("The operations of Expire %s", GetWorkerTimeCost().GetInfo());
}
}  // namespace

Status WorkerOcServiceExpireImpl::DS_OC_PROC_ENTRY(Expire, const ExpireReqPb &req, ExpireRspPb &rsp)
{
    ScopedRequestContext ctx;
    Timer timer;
    int64_t realTimeoutMs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTime();
    LOG(INFO) << "Expire start from client:" << req.client_id();
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    auto objectKeys = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_keys());
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_EXPIRE);
    access.ObjectKeysRef(objectKeys).TtlSecond(req.ttl_second());

    CHECK_FAIL_RETURN_STATUS(metadataRouteResolver_ != nullptr, K_NOT_READY, "Metadata route resolver is unavailable");
    auto grouped = metadataRouteResolver_->GroupOwners(objectKeys);
    auto &objKeysGrpByMaster = grouped.groups;

    std::unordered_set<std::string> objKeysExpireFailed;
    std::vector<std::string> absentObjectKeys;
    uint32_t ttlSeconds = req.ttl_second();
    std::vector<std::future<Status>> futures;
    std::string traceID = Trace::Instance().GetTraceID();
    Status rc;
    const bool useThreadPoolFanout = ShouldUseServiceThreadPoolFanout();
    std::unique_ptr<ThreadPool> batchExpireThreadPool;
    if (useThreadPoolFanout) {
        size_t threadNum = std::min<size_t>(objKeysGrpByMaster.size(), FLAGS_rpc_thread_num);
        batchExpireThreadPool = std::make_unique<ThreadPool>(1, threadNum, "BatchExpireMeta");
    }
    futures.reserve(objKeysGrpByMaster.size());
    auto expire = [&, timer](const auto &item) {
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceID);
        int64_t elapsed = static_cast<int64_t>(timer.ElapsedMilliSecond());
        GetRequestContext()->reqTimeoutDuration.Init(realTimeoutMs - elapsed);

        HostPort workerAddr = item.first;
        const std::vector<std::string> &currentIds = item.second;
        rc = ExpireFromMaster(currentIds, workerAddr, ttlSeconds, absentObjectKeys, objKeysExpireFailed, rsp);
        if (rc.IsError()) {
            return rc;
        }
        return Status::OK();
    };
    if (useThreadPoolFanout) {
        for (auto &item : objKeysGrpByMaster) {
            futures.emplace_back(batchExpireThreadPool->Submit([&, item]() { return expire(item); }));
        }
    } else {
        Status serialRc = RunServiceTasksSerially(objKeysGrpByMaster.begin(), objKeysGrpByMaster.end(), expire);
        if (serialRc.IsError()) {
            access.Result(serialRc).Record();
            return serialRc;
        }
    }

    Status waitRc = WaitExpireTasks(futures, access);
    if (waitRc.IsError()) {
        return waitRc;
    }
    FinishExpireRequest(grouped, absentObjectKeys, objKeysExpireFailed, rsp, access, rc, timer);
    return rc;
}
