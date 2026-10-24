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


#ifdef DS_OC_READ_META_AUTH_BODY
Status DS_OC_READ_ENTRY(AuthenticateGetMetaUser, AkSkManager *akSkManager, const GetObjMetaInfoReqPb &req)
{
    if (akSkManager->SystemAuthEnabled()) {
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_READ_VERIFY(akSkManager, req),
                                         "AK/SK failed. Only the ak/sk client can check the meta info of objects.");
    } else {
        CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(
            !FLAGS_authorization_enable, K_NOT_AUTHORIZED,
            "Only the ak/sk client can check the meta info of objects if tenant authorization is enabled.");
    }
    return Status::OK();
}
#endif

#ifdef DS_OC_READ_OBJECT_INFO_BODY
Status WorkerOcServiceGetImpl::DS_OC_READ_ENTRY(GetObjMetaInfo, const GetObjMetaInfoReqPb &req, GetObjMetaInfoRspPb &resp)
{
    ScopedRequestContext ctx;
    INJECT_POINT("worker.GetObjMetaInfo");
    RETURN_IF_NOT_OK(DS_OC_READ_FORWARD(AuthenticateGetMetaUser, akSkManager_.get(), req));
    // construct objectKey by the input tenantId
    auto objectKeys = TenantAuthManager::ConstructNamespaceUriWithTenantId(req.tenantid(), req.object_keys());
    Status lastRc;
    std::unordered_map<std::string, ObjectLocationInfoPb> result;
    auto rc = GetMapOfObjectKeys(objectKeys, result, lastRc);
    if (rc.IsError()) {
        RETURN_IF_NOT_OK_EXCEPT(rc, K_METADATA_OWNER_UNAVAILABLE);
        std::vector<std::string> localMisses;
        std::unordered_map<std::string, ObjectLocationInfoPb> localResult;
        RETURN_IF_NOT_OK(FillLocalObjectLocations(objectKeys, localResult, localMisses));
        if (!localMisses.empty()) {
            return rc;
        }
        result = std::move(localResult);
    }
    FillGetObjMetaInfoRspPb(objectKeys, result, resp);
    return Status::OK();
}

Status WorkerOcServiceGetImpl::DS_OC_READ_ENTRY(QuerySize, const QuerySizeReqPb &req, QuerySizeRspPb &rsp)
{
    ScopedRequestContext ctx;
    Timer timer;
    auto clientId = ClientKey::Intern(req.client_id());
    LOG(INFO) << "QuerySize start from client:" << clientId;
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_READ_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    auto objectKeys = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_keys());
    QueryMetadataFromMasterResult queryMetaResult;
    std::vector<master::QueryMetaInfoPb> &queryMetas = queryMetaResult.queryMetas;
    int64_t subTimeout = 0;
    Status lastRc = QueryMetadataFromMaster(objectKeys, subTimeout, queryMetaResult);
    std::unordered_map<std::string, uint64_t> result;
    result.reserve(queryMetas.size());
    for (auto queryIt = queryMetas.begin(); queryIt != queryMetas.end(); ++queryIt) {
        const auto &queryMeta = *queryIt;
        const auto &meta = queryMeta.meta();
        const auto dataSize = meta.data_size();
        const auto objectKey = meta.object_key();
        result.emplace(objectKey, dataSize);
    }

    for (const auto &objectKey : objectKeys) {
        auto it = result.find(objectKey);
        if (it != result.end()) {
            rsp.add_sizes(it->second);
        } else {
            rsp.add_sizes(0);
        }
    }

    rsp.mutable_last_rc()->set_error_code(lastRc.GetCode());
    rsp.mutable_last_rc()->set_error_msg(lastRc.GetMsg());
    return Status::OK();
}
#endif

#ifdef DS_OC_READ_EXIST_INFO_BODY
static constexpr double EXIST_LOCAL_CHECK_TIMEOUT_US = 50.0;

Status WorkerOcServiceGetImpl::DS_OC_READ_ENTRY(Exist, const ExistReqPb &req, ExistRspPb &rsp)
{
    ScopedRequestContext ctx;
    Timer timer;
    PerfPoint point(PerfKey::WORKER_EXIST_PRE_PROCESS);
    auto config = GetServerLatencyTraceConfig();
    const bool traceEnabled = ShouldCollectLatencyTrace(config);
    if (traceEnabled) {
        Trace::Instance().AddLatencyTick(LatencyTickKey::WORKER_EXIST_START);
    }
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_EXIST);
    access.ObjectKeysRef(req.object_keys());
    INJECT_POINT("Exist.Sleep");
    auto clientId = ClientKey::Intern(req.client_id());
    VLOG(1) << "Exist start from client:" << clientId;
    std::string tenantId;
    Status authRc = req.is_routed() ? DS_OC_READ_REQUEST_TENANT(akSkManager_, req, tenantId)
                                    : DS_OC_READ_SESSION_TENANT(akSkManager_, req, tenantId);
    if (authRc.IsError()) {
        LOG(ERROR) << "Authenticate failed. Detail: " << authRc.ToString();
        access.Result(authRc).Record();
        return authRc;
    }
    if (!Validator::IsExistBatchSizeUnderLimit(req.object_keys_size())) {
        LOG(ERROR) << "invalid object size";
        Status rc(StatusCode::K_INVALID, __LINE__, __FILE__, "invalid object size");
        access.Result(rc).Record();
        return rc;
    }
    auto keys = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_keys());

    point.RecordAndReset(PerfKey::WORKER_EXIST_LOCAL_CHECK);
    std::unordered_set<std::string> existKeys;
    std::vector<std::string> nonLocalKeys;
    existKeys.reserve(keys.size());
    nonLocalKeys.reserve(keys.size());
    Timer localCheckTimer;
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        if (localCheckTimer.ElapsedMicroSecond() >= EXIST_LOCAL_CHECK_TIMEOUT_US) {
            nonLocalKeys.insert(nonLocalKeys.end(), it, keys.end());
            break;
        }
        const auto &key = *it;
        if (!IsLocalObject(key)) {
            nonLocalKeys.emplace_back(key);
            continue;
        }
        evictionManager_->Add(key);
        existKeys.emplace(key);
        INJECT_POINT("worker.Exist.after_local_check");
    }

    Status rc;
    if (!req.is_local() && !nonLocalKeys.empty()) {
        point.RecordAndReset(PerfKey::WORKER_EXIST_QUERY_MASTER);
        if (traceEnabled) {
            Trace::Instance().AddLatencyTick(LatencyTickKey::WORKER_EXIST_QUERYMETA_START);
        }
        std::vector<master::QueryMetaInfoPb> queryMetas;
        INJECT_POINT("worker.before_query_meta");
        rc = QueryExistMetadataViaPureQueryMeta(nonLocalKeys, queryMetas);
        if (traceEnabled) {
            Trace::Instance().AddLatencyTick(LatencyTickKey::WORKER_EXIST_QUERYMETA_END);
        }
        if (rc.IsError()) {
            if (rc.GetCode() == K_NOT_OWNER) {
                rsp.set_redirect_extra(rc.GetExtra());
                access.Discard();
            } else {
                access.Result(rc).Record();
            }
            return rc;
        }
        const auto localAddress = localAddress_.ToString();
        for (const auto &meta : queryMetas) {
            const bool isLocalPrimaryCopy = meta.single_copy() && meta.meta().primary_address() == localAddress;
            if (!meta.address().empty() || isLocalPrimaryCopy) {
                existKeys.emplace(meta.meta().object_key());
            }
        }
    }

    point.RecordAndReset(PerfKey::WORKER_EXIST_BUILD_RSP);
    for (const auto &key : keys) {
        rsp.add_exists(existKeys.count(key));
    }

    if (traceEnabled) {
        Trace::Instance().AddLatencyTick(LatencyTickKey::WORKER_EXIST_END);
    }
    const auto totalExistUs = static_cast<uint64_t>(timer.ElapsedMicroSecond());
    if (ShouldPrintLatencySummary(totalExistUs, config)) {
        auto result =
            ComputePhaseDurations(Trace::Instance().GetLatencyTicks(), Trace::Instance().GetLatencyTickCount(),
                                  Trace::Instance().GetLatencyTickDroppedCount());
        auto &downstream = Trace::Instance().GetDownstreamPhases();
        bool hasDownstream = downstream.count > 0;
        if (hasDownstream) {
            for (uint16_t di = 0; di < downstream.count; ++di) {
                result.Add(downstream.entries[di].phase, downstream.entries[di].durationUs);
            }
            result.tickDroppedCount += downstream.tickDroppedCount;
            result.phaseDroppedCount += downstream.phaseDroppedCount;
        }
        bool gateHit = CheckPhaseGate(result, config);
        if (gateHit) {
            Trace::Instance().SetLatencySummary(FormatLatencySummary(result));
        }
        bool shouldEncodeProto = gateHit || hasDownstream;
        if (shouldEncodeProto) {
            auto *phases = rsp.mutable_latency_phase_us();
            for (uint16_t i = 0; i < result.count; ++i) {
                phases->Add(static_cast<unsigned int>(result.entries[i].phase));
                phases->Add(static_cast<unsigned int>(result.entries[i].durationUs));
            }
            if (result.droppedCount() > 0) {
                rsp.set_latency_tick_dropped_count(static_cast<uint32_t>(result.droppedCount()));
            }
        }
    }
    access.Result(rc).Record();
    const double totalExistMs = static_cast<double>(totalExistUs) / US_PER_MS;
    GetWorkerTimeCost().Append("Total Exist", totalExistMs);
    SLOW_LOG_IF_OR_VLOG(INFO, config.processSlowerThanUs > 0 && totalExistUs >= config.processSlowerThanUs, 1,
                        FormatString("Exist done, cost: %.3fms, %s", totalExistMs, GetWorkerTimeCost().GetInfo()));
    return rc;
}

Status WorkerOcServiceGetImpl::DS_OC_READ_ENTRY(GetMetaInfo, const GetMetaInfoReqPb &req, GetMetaInfoRspPb &rsp)
{
    Timer timer;
    auto clientId = ClientKey::Intern(req.client_id());
    LOG(INFO) << "GetMetaInfo start from client:" << clientId;
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_READ_SESSION_TENANT(akSkManager_, req, tenantId), "Authenticate failed.");
    auto objectKeys = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_keys());
    if (!req.is_dev_key()) {
        QueryMetadataFromMasterResult queryResult;
        auto rc = QueryMetadataFromMaster(objectKeys, 0, queryResult, false);
        std::vector<master::QueryMetaInfoPb> &queryMetas = queryResult.queryMetas;
        std::map<std::string, const ::google::protobuf::RepeatedField<::uint64_t>> mp;
        for (const auto &queryMeta : queryMetas) {
            auto &meta = queryMeta.meta();
            mp.insert({ meta.object_key(), std::move(meta.device_info().blob_sizes()) });
        }
        for (const auto &key : objectKeys) {
            auto devMetaInfos = rsp.add_dev_meta_infos();
            if (mp.find(key) != mp.end()) {
                devMetaInfos->mutable_blob_sizes()->Add(mp[key].begin(), mp[key].end());
            } else {
                *(devMetaInfos->mutable_blob_sizes()) = {};
            }
        }
        return Status::OK();
    }
    std::shared_ptr<WorkerMasterOCApi> workerMasterApi;
    RETURN_IF_NOT_OK(workerMasterApiManager_->GetWorkerMasterApi(P2P_DEFAULT_MASTER, workerMasterApi));
    auto reqCopy = req;
    return workerMasterApi->GetMetaInfo(reqCopy, rsp);
}
#endif
