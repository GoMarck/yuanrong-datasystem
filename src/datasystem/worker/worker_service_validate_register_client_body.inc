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

    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_RESOLVE_WORKER_TENANT(akSkManager_, req, req.tenant_id(), tenantId),
                                     "Authenticate failed");
    if (localExiting_.load(std::memory_order_acquire)) {
        LOG(WARNING) << "Register client failed because the Worker is draining for ScaleIn";
        RETURN_STATUS(StatusCode::K_NOT_READY, "Worker is draining for ScaleIn");
    }
    return Status::OK();
