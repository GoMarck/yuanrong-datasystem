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

    INJECT_POINT("worker.Heartbeat.begin");
    std::string tenantId;
    bool exist;
    auto clientId = ClientKey::Intern(req.client_id());
    std::string authTenantId = worker::ClientManager::Instance().GetAuthTenantIdByClientId(clientId, exist);
    // When exist is false, it indicates a restart scenario, skip iam and authenticate.
    if (exist) {
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_RESOLVE_WORKER_TENANT(akSkManager_, req, authTenantId, tenantId),
                                         "Authenticate failed");
    }
    VLOG(HEARTBEAT_LEVEL) << "Receive a heartbeat from client " << clientId;
    INJECT_POINT("Worker.Heartbeat.startToHeartbeat");
    rsp.set_worker_start_id(workerStartId_);
    Status status = ClientManager::Instance().UpdateLastHeartbeat(clientId, req.removable());
    std::vector<int> releasedFds;
    for (auto i : req.released_worker_fds()) {
        releasedFds.emplace_back(i);
    }
    LOG_IF(INFO, !releasedFds.empty()) << FormatString("[TENANT RELEASER]Receive cleint[%s] has released fds[%s]",
                                                       clientId, VectorToString(releasedFds));
    if (exist) {
        // We only allow authenticated requests to modify data.
        ClientManager::Instance().DelClientId2WorkerFdMap(clientId, releasedFds);
    }
    // If the client has been removed, UpdateLastHeartbeat will return RuntimeError. Otherwise, OK is returned.
    rsp.set_client_removed(status.IsError());
    rsp.set_standby_worker(GetLocalStandbyWorker());
    rsp.set_unhealthy(!IsHealthy());
    SetAvailableWorkers(rsp);
    PopulateUbHealthSummary(rsp);
    LogSampler::Instance().PopulateConfigProto(rsp.mutable_log_sample_config());
    rsp.set_is_voluntary_scale_down(localExiting_.load(std::memory_order_acquire));

    auto intersection = GetExpiredFdsForClient(clientId);
    for (auto fd : intersection) {
        rsp.add_expired_worker_fds(fd);
    }
    VLOG(HEARTBEAT_LEVEL) << "Response heartbeat message to client " << clientId << ", response: " << rsp.DebugString();
    return Status::OK();
