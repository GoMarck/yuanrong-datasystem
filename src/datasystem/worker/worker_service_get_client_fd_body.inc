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
