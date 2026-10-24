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

/**
 * Description: Defines the worker service Expire process, for set expiration time.
 */
#include "datasystem/worker/object_cache/service/worker_oc_service_expire_impl.h"

#include <cstdint>
#include <utility>


#include "datasystem/common/flags/common_flags.h"
#include "datasystem/common/iam/tenant_auth_manager.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/log/access_recorder.h"
#include "datasystem/common/log/log.h"
#include "datasystem/common/perf/perf_manager.h"
#include "datasystem/common/util/format.h"
#include "datasystem/common/util/meta_route_tool.h"
#include "datasystem/common/util/rpc_util.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/protos/master_object.pb.h"
#include "datasystem/utils/status.h"
#include "datasystem/worker/authenticate.h"
#include "datasystem/worker/object_cache/service/service_execution_policy.h"

using namespace datasystem::master;
namespace datasystem {
namespace object_cache {

WorkerOcServiceExpireImpl::WorkerOcServiceExpireImpl(WorkerOcServiceCrudParam &initParam,
                                                     std::shared_ptr<AkSkManager> akSkManager)
    : WorkerOcServiceCrudCommonApi(initParam), akSkManager_(std::move(akSkManager))
{
}

#define DS_OC_PROC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_OC_PROC_FORWARD(name, ...) name(__VA_ARGS__)
#define DS_OC_REQUEST_TENANT(manager, request, tenant) \
    worker::AuthenticateRequest(manager, request, request.tenant_id(), tenant)
#define DS_OC_SESSION_TENANT(manager, request, tenant) worker::Authenticate(manager, request, tenant)
#include "datasystem/worker/object_cache/service/worker_oc_service_expire_body.inc"
#undef DS_OC_SESSION_TENANT
#undef DS_OC_REQUEST_TENANT
#undef DS_OC_PROC_FORWARD
#undef DS_OC_PROC_ENTRY

Status WorkerOcServiceExpireImpl::ExpireFromMaster(std::vector<std::string> objectKeys, const HostPort &masterAddr,
                                                   uint32_t ttlSeconds, std::vector<std::string> &absentObj,
                                                   std::unordered_set<std::string> &objExpireFailed, ExpireRspPb &rsp)
{
    master::ExpireReqPb req;
    master::ExpireRspPb resp;
    *req.mutable_object_keys() = { objectKeys.begin(), objectKeys.end() };
    req.set_redirect(true);
    req.set_ttl_second(ttlSeconds);
    INJECT_POINT("worker.expire_failed");
    auto workerMasterApi = workerMasterApiManager_->GetWorkerMasterApi(masterAddr);
    CHECK_FAIL_RETURN_STATUS(workerMasterApi != nullptr, K_RUNTIME_ERROR, "hash master get failed, Exipire failed");
    std::function<Status(master::ExpireReqPb &, master::ExpireRspPb &)> func =
        [&workerMasterApi](master::ExpireReqPb &req, master::ExpireRspPb &resp) {
            return workerMasterApi->Expire(req, resp);
        };

    auto rc = RedirectRetryWhenMetasMoving(req, resp, func);
    if (rc.IsError()) {
        LOG(WARNING) << "Expire meta from master[" << masterAddr.ToString() << "] failed, msg: " << rc.ToString();
        rsp.mutable_last_rc()->set_error_code(rc.GetCode());
        rsp.mutable_last_rc()->set_error_msg(rc.GetMsg());
        objExpireFailed.insert(objectKeys.begin(), objectKeys.end());
        return Status::OK();
    }
    absentObj.insert(absentObj.end(), resp.absent_object_keys().begin(), resp.absent_object_keys().end());
    objExpireFailed.insert(resp.failed_object_keys().begin(), resp.failed_object_keys().end());
    *rsp.mutable_last_rc() = resp.last_rc();
    RETURN_OK_IF_TRUE(resp.info().empty());
    for (const auto &redirectInfo : resp.info()) {
        master::ExpireReqPb redirectReq;
        master::ExpireRspPb redirectRsp;
        *redirectReq.mutable_object_keys() = { redirectInfo.change_meta_ids().begin(),
                                               redirectInfo.change_meta_ids().end() };
        redirectReq.set_ttl_second(ttlSeconds);
        HostPort redirectMasterAddr;
        RETURN_IF_NOT_OK(redirectMasterAddr.ParseString(redirectInfo.redirect_meta_address()));
        auto redirectWorkerMasterApi = workerMasterApiManager_->GetWorkerMasterApi(redirectMasterAddr);
        CHECK_FAIL_RETURN_STATUS(redirectWorkerMasterApi != nullptr, K_RUNTIME_ERROR,
                                 "hash master get failed, ExpireFromMaster failed");
        rc = redirectWorkerMasterApi->Expire(redirectReq, redirectRsp);
        if (rc.IsError()) {
            LOG(WARNING) << "Expire meta from master[" << masterAddr.ToString() << "] failed, msg: " << rc.ToString();
            rsp.mutable_last_rc()->set_error_code(rc.GetCode());
            rsp.mutable_last_rc()->set_error_msg(rc.GetMsg());
            objExpireFailed.insert(redirectReq.object_keys().begin(), redirectReq.object_keys().end());
            continue;
        }
        absentObj.insert(absentObj.end(), redirectRsp.absent_object_keys().begin(),
                         redirectRsp.absent_object_keys().end());
        objExpireFailed.insert(redirectRsp.failed_object_keys().begin(), redirectRsp.failed_object_keys().end());
        *rsp.mutable_last_rc() = redirectRsp.last_rc();
    }
    return Status::OK();
}

}  // namespace object_cache
}  // namespace datasystem
