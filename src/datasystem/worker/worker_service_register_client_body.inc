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

    INJECT_POINT("WorkerServiceImpl.RegisterClient.begin");
    // An auxiliary session is a fresh endpoint-scoped shm session of an already-counted primary
    // client: not a reconnect, only excluded from active_client_count.
    bool auxiliarySession = req.auxiliary_session();
    bool remainClient = !req.client_id().empty() && !auxiliarySession;
    auto clientId = ClientKey::Intern(remainClient ? req.client_id() : GetStringUuid());
    std::string tenantId;
    RETURN_IF_NOT_OK(DS_VALIDATE_REGISTER_CLIENT(req, tenantId));
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
