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

/**
 * Description: Defines the worker to master service processing main class implementation.
 */
#include "datasystem/worker/object_cache/master_worker_oc_service_impl.h"

#include <limits>
#include <sstream>
#include <vector>

#include "datasystem/common/log/log.h"
#include "datasystem/common/iam/tenant_auth_manager.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/log/trace.h"
#include "datasystem/common/rpc/api_deadline.h"
#include "datasystem/common/util/format.h"
#include "datasystem/common/log/log_helper.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/common/util/strings_util.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/protos/master_object.pb.h"
#include "datasystem/utils/status.h"

#include "datasystem/worker/object_cache/object_kv.h"
#include "datasystem/worker/object_cache/service/worker_oc_service_crud_common_api.h"

namespace datasystem {
namespace object_cache {
MasterWorkerOCServiceImpl::MasterWorkerOCServiceImpl(
    std::shared_ptr<datasystem::object_cache::WorkerOCServiceImpl> clientSvc, std::shared_ptr<AkSkManager> akSkManager)
    : ocClientWorkerSvc_(clientSvc), akSkManager_(akSkManager)
{
}

MasterWorkerOCServiceImpl::~MasterWorkerOCServiceImpl()
{
    LOG(INFO) << "MasterWorkerOCServiceImpl exit";
}

Status MasterWorkerOCServiceImpl::Init()
{
    CHECK_FAIL_RETURN_STATUS(ocClientWorkerSvc_ != nullptr, StatusCode::K_NOT_READY,
                             "ClientWorkerService must be initialized before MasterWorkerService construction");
    return MasterWorkerOCService::Init();
}

Status MasterWorkerOCServiceImpl::WaitWorkerOCServiceImplInit()
{
    return ocClientWorkerSvc_->WaitInit();
}

#define DS_MW_RPC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MW_RPC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MW_RPC_GROUP_0
#include "datasystem/worker/object_cache/master_worker_oc_service_body.inc"
#undef DS_MW_RPC_GROUP_0
#undef DS_MW_RPC_VERIFY
#undef DS_MW_RPC_ENTRY

Status MasterWorkerOCServiceImpl::UpdateSingleNotification(const UpdateObjectInfoPb &req, bool sync)
{
    auto getEntryAndLock = [this](const std::string &objectKey, bool sync, std::shared_ptr<SafeObjType> &entry) {
        RETURN_IF_NOT_OK(ocClientWorkerSvc_->objectTable_->Get(objectKey, entry));
        if (sync) {
            RETURN_IF_NOT_OK_PRINT_ERROR_MSG(TryLockWithRetry(objectKey, entry), "Lock failed");
        } else {
            RETURN_IF_NOT_OK_PRINT_ERROR_MSG(entry->WLock(), "Lock failed");
        }
        return Status::OK();
    };

    const std::string &objectKey = req.object_key();
    const uint64_t version = req.version();
    LOG(INFO) << FormatString("[ObjectKey %s] Receive UpdateNotification from %s", objectKey, req.address());
    std::shared_ptr<SafeObjType> safeEntry;
    Status status = getEntryAndLock(objectKey, sync, safeEntry);
    if (status.IsError()) {
        return status.GetCode() == StatusCode::K_NOT_FOUND ? Status::OK() : status;
    }

    Raii unlockRaii([safeEntry]() { safeEntry->WUnlock(); });
    if ((*safeEntry)->IsBinary()) {
        if (version > (*safeEntry)->GetCreateTime()) {
            // Update version to latest time to keep invalidation version.
            (*safeEntry)->SetCreateTime(version);
            ObjectKV objectKV(objectKey, *safeEntry);
            RETURN_IF_NOT_OK(BinaryObjectCacheInvalidation(req, objectKV));
        } else if (version < (*safeEntry)->GetCreateTime()) {
            LOG(INFO) << "Cache Invalidation should not be performed because the version is out of date.";
            RETURN_STATUS(K_INVALID, "Version expired.");
        }
    } else if ((*safeEntry)->IsHashmap()) {
        for (const auto &field : req.secondary_keys()) {
            Status rc = (*safeEntry)->Erase(field);
            if (rc.IsError() && rc.GetCode() != K_NOT_FOUND) {
                RETURN_IF_NOT_OK_PRINT_ERROR_MSG(rc, "UpdateSingleNotification erase data failed");
            }
        }
        LOG(INFO) << FormatString("[ObjectKey %s] Cache invalidation delete success", objectKey);
    } else {
        RETURN_STATUS_LOG_ERROR(K_UNKNOWN_ERROR, "object data format not supported");
    }
    return Status::OK();
}

Status MasterWorkerOCServiceImpl::MetaChange(const MetaChangeReqPb &req, MetaChangeRspPb &rsp)
{
    ScopedRequestContext ctx;
    // Deprecated
    (void)req;
    (void)rsp;
    return Status::OK();
}

#define DS_MW_RPC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MW_RPC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MW_RPC_GROUP_1
#include "datasystem/worker/object_cache/master_worker_oc_service_body.inc"
#undef DS_MW_RPC_GROUP_1
#undef DS_MW_RPC_VERIFY
#undef DS_MW_RPC_ENTRY

void MasterWorkerOCServiceImpl::RetryGetObjectFromRemote(const ReadKey &readKey,
                                                         const master::QueryMetaInfoPb &queryMeta,
                                                         std::vector<RpcMessage> &payloads)
{
    const auto &objectKey = queryMeta.meta().object_key();
    constexpr int MAX_RETRY = 5;
    Status rc;
    for (int i = 0; i < MAX_RETRY; i++) {
        rc = ocClientWorkerSvc_->GetObjectFromAnywhere(readKey, queryMeta, payloads);
        if (rc.IsOk()) {
            LOG(INFO) << FormatString("[ObjectKey %s] Get object from remote success", objectKey);
            break;
        }
    }
    if (rc.IsError()) {
        LOG(ERROR) << FormatString("GetObjectFromRemote failed: %s", rc.ToString());
    }
}

Status MasterWorkerOCServiceImpl::ClearData(const ClearDataReqPb &req, ClearDataRspPb &rsp)
{
    ScopedRequestContext ctx;
    RETURN_IF_NOT_OK(ocClientWorkerSvc_->ClearObject(req));
    (void)rsp;
    return Status::OK();
}

#define DS_MW_RPC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MW_RPC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MW_RPC_GROUP_2
#include "datasystem/worker/object_cache/master_worker_oc_service_body.inc"
#undef DS_MW_RPC_GROUP_2
#undef DS_MW_RPC_VERIFY
#undef DS_MW_RPC_ENTRY

Status MasterWorkerOCServiceImpl::BinaryObjectCacheInvalidation(const UpdateObjectInfoPb &req, ObjectKV &objectKV)
{
    const auto &objectKey = objectKV.GetObjKey();
    SafeObjType &safeEntry = objectKV.GetObjEntry();
    if (safeEntry->IsSpilled()) {
        LOG(INFO) << FormatString("[ObjectKey %s] Spilled and prepare to clean it.", objectKey);
        RETURN_IF_NOT_OK_EXCEPT(WorkerOcServiceCrudCommonApi::DeleteObjectFromDisk(objectKV), K_NOT_FOUND);
    }
    // Mark as invalid and memorize address.
    safeEntry->stateInfo.SetCacheInvalid(true);
    safeEntry->stateInfo.SetPrimaryCopy(false);
    LOG_IF_ERROR(safeEntry->FreeResources(), "SafeObj free failed");
    safeEntry->SetAddress(req.address());
    safeEntry->SetLifeState(ObjectLifeState::OBJECT_INVALID);  // To make IsInvalid() call true.
    LOG(INFO) << FormatString("[ObjectKey %s] Cache invalidation success", objectKey);
    return Status::OK();
}

#define DS_MW_RPC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MW_RPC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MW_RPC_GROUP_3
#include "datasystem/worker/object_cache/master_worker_oc_service_body.inc"
#undef DS_MW_RPC_GROUP_3
#undef DS_MW_RPC_VERIFY
#undef DS_MW_RPC_ENTRY

Status MasterWorkerOCServiceImpl::ProcessChangePrimaryCopy(const std::string &objectKey, bool isPrimaryCopy)
{
    INJECT_POINT("process.change.primary.copy");
    LOG(INFO) << FormatString("ProcessChangePrimaryCopy for object:%s", objectKey);
    std::shared_ptr<SafeObjType> safeEntry;
    RETURN_IF_NOT_OK(ocClientWorkerSvc_->objectTable_->Get(objectKey, safeEntry));
    RETURN_IF_NOT_OK(safeEntry->WLock());
    Raii unlockRaii([safeEntry]() { safeEntry->WUnlock(); });
    if ((*safeEntry)->IsBinary()) {
        (*safeEntry)->stateInfo.SetPrimaryCopy(isPrimaryCopy);
    }
    return Status::OK();
}

#define DS_MW_RPC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MW_RPC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MW_RPC_GROUP_4
#include "datasystem/worker/object_cache/master_worker_oc_service_body.inc"
#undef DS_MW_RPC_GROUP_4
#undef DS_MW_RPC_VERIFY
#undef DS_MW_RPC_ENTRY

}  // namespace object_cache
}  // namespace datasystem
