/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
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

Status WorkerServiceImpl::DS_WORKER_RPC_ENTRY(GetClientFd, const GetClientFdReqPb &req, GetClientFdRspPb &rsp)
{
    INJECT_POINT("worker.before_GetClientFd");
    (void)rsp;
    int socketFd;
    LOG(INFO) << "Start to query client fd, localAddress: " << localAddress_.ToString();
    std::string tenantId;
    std::string authTenantId = req.tenant_id();
    auto clientId = ClientKey::Intern(req.client_id());
    if (authTenantId.empty()) {
        bool exist;
        authTenantId = worker::ClientManager::Instance().GetAuthTenantIdByClientId(clientId, exist);
    }
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_RESOLVE_WORKER_TENANT(akSkManager_, req, authTenantId, tenantId),
                                     "Authenticate failed");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ClientManager::Instance().GetClientSocketFd(clientId, socketFd),
                                     "worker get client socketfd failed");

    std::vector<int> workerFds = { req.worker_fds().cbegin(), req.worker_fds().cend() };
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(memory::Allocator::Instance()->CheckWorkerFdTenant(tenantId, workerFds),
                                     "Authenticate workerFd failed.");

    LOG(INFO) << "Start to send client fd. worker fds: " << VectorToString(workerFds) << ", socket fd: " << socketFd
              << ", request id: " << req.request_id() << ", client id: " << clientId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(SockSendFd(socketFd, shmWorkerPort_ > 0, workerFds, req.request_id()),
                                     "worker socketfd send failed");
    ClientManager::Instance().SetClientId2WorkerFdMap(clientId, std::move(workerFds));
    LOG(INFO) << "Finish to send client fd. worker fds: " << VectorToString(workerFds) << ", socket fd: " << socketFd;
    return Status::OK();
}

Status WorkerServiceImpl::DS_WORKER_RPC_ENTRY(DisconnectClient, const DisconnectClientReqPb &req, DisconnectClientRspPb &rsp)
{
    std::string tenantId;
    bool exist;
    auto clientId = ClientKey::Intern(req.client_id());
    std::string authTenantId = worker::ClientManager::Instance().GetAuthTenantIdByClientId(clientId, exist);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_RESOLVE_WORKER_TENANT(akSkManager_, req, authTenantId, tenantId),
                                     "Authenticate failed");
    LOG(INFO) << FormatString("disconnect client: %s", clientId);
    (void)rsp;
    worker_->AfterClientLostHandler(clientId);
    return Status::OK();
}

Status WorkerServiceImpl::DS_WORKER_RPC_ENTRY(RegisterClient, const RegisterClientReqPb &req, RegisterClientRspPb &rsp)
{
    INJECT_POINT("WorkerServiceImpl.RegisterClient.begin");
    // An auxiliary session is a fresh endpoint-scoped shm session of an already-counted primary
    // client: not a reconnect, only excluded from active_client_count.
    bool auxiliarySession = req.auxiliary_session();
    bool remainClient = !req.client_id().empty() && !auxiliarySession;
    auto clientId = ClientKey::Intern(remainClient ? req.client_id() : GetStringUuid());
    std::string tenantId;
    RETURN_IF_NOT_OK(DS_WORKER_RPC_FORWARD(ValidateRegisterClientRequest, req, tenantId));
    RETURN_IF_NOT_OK(ConsumeRegisterClientFd(req));
    RaiiPlus raiiP([&req]() {
        if (req.shm_enabled() && req.server_fd() > 0) {
            RETRY_ON_EINTR(close(req.server_fd()));
        }
    });
    CheckClientGitHash(req.git_hash());
    const auto &version = req.version();
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(CheckClientVersion(version), "worker check client version failed");
    CompatibilityVersion compatibilityVersion;
    if (!req.compatibility_version().empty()) {
        LOG_IF_ERROR(CompatibilityVersion::FromString(req.compatibility_version(), compatibilityVersion),
                     "worker parse compatibility version failed");
    }
    uint32_t lockId = 0;
    uint32_t pipelineQueueId = OsXprtPipln::INVALID_PIPLN_QUEUE_ID;
    bool supportMultiShmRefCount = req.support_multi_shm_ref_count();
    RETURN_IF_NOT_OK(AddRegisteringClient(req, clientId, tenantId, compatibilityVersion, lockId, pipelineQueueId,
                                          supportMultiShmRefCount));
    // After executing "AddClient", the server fd will be bound to the client and released when the client loses
    // connection, so there is no need to roll back.
    raiiP.ClearAllTask();

    if (remainClient) {
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(worker_->ProcessServerReboot(clientId, tenantId, req.token(), req.extend()),
                                         "worker process server reboot failed");
    }
    int fd = -1;
    uint64_t mmapSize = 0;
    ptrdiff_t offset = 0;
    ShmKey id;
    if (req.shm_enabled()) {
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(worker_->GetShmQueueUnit(lockId, fd, mmapSize, offset, id),
                                         "worker process get ShmQ unit failed");
    }
    PopulateRegisterClientResponse(rsp, clientId, tenantId, lockId, pipelineQueueId, supportMultiShmRefCount,
                                   req.shm_enabled(), fd, mmapSize, offset, id);
    INJECT_POINT("worker.RegisterClient.end", [&rsp](int injectedFd) {
        rsp.set_store_fd(injectedFd);
        return Status::OK();
    });
    LOG(INFO) << "Register client " << clientId << " done, healthy: " << !rsp.unhealthy()
              << ", worker support multi shm ref count:" << rsp.support_multi_shm_ref_count();
    return Status::OK();
}

Status WorkerServiceImpl::DS_WORKER_RPC_ENTRY(ValidateRegisterClientRequest, const RegisterClientReqPb &req, std::string &tenantId) const
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_RESOLVE_WORKER_TENANT(akSkManager_, req, req.tenant_id(), tenantId),
                                     "Authenticate failed");
    if (localExiting_.load(std::memory_order_acquire)) {
        LOG(WARNING) << "Register client failed because the Worker is draining for ScaleIn";
        RETURN_STATUS(StatusCode::K_NOT_READY, "Worker is draining for ScaleIn");
    }
    return Status::OK();
}

Status WorkerServiceImpl::DS_WORKER_RPC_ENTRY(Heartbeat, const HeartbeatReqPb &req, HeartbeatRspPb &rsp)
{
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
}

Status WorkerServiceImpl::DS_WORKER_RPC_ENTRY(GetSocketPath, const GetSocketPathReqPb &req, GetSocketPathRspPb &rsp)
{
    INJECT_POINT("WorkerServiceImpl.GetSocketPath.begin");
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_RESOLVE_WORKER_TENANT(akSkManager_, req, req.tenant_id(), tenantId),
                                     "Authenticate failed");
    if (FLAGS_ipc_through_shared_memory) {
        rsp.set_path(sockPath_);
        if (shmWorkerPort_ > 0) {
            rsp.set_shm_worker_port(shmWorkerPort_);
        }
    }
    return Status::OK();
}
