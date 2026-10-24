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
