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

/**
 * Description: Implement the object cache remote services on the master.
 */

#include "datasystem/common/perf/perf_manager.h"
#include "datasystem/master/object_cache/master_oc_service_impl.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <type_traits>

#include "datasystem/common/flags/flags.h"
#include "datasystem/common/log/log.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/log/latency_phase.h"
#include "datasystem/common/log/trace.h"

#include "datasystem/common/l2cache/persistence_api.h"
#include "datasystem/common/log/log_helper.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/common/util/strings_util.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/master/object_cache/device/master_dev_oc_manager.h"
#include "datasystem/master/object_cache/oc_metadata_manager.h"
#include "datasystem/master/object_cache/oc_migrate_metadata_manager.h"
#include "datasystem/protos/master_object.service.rpc.pb.h"
#include "datasystem/protos/p2p_subscribe.pb.h"

namespace datasystem {
namespace master {
static constexpr int ASYNC_MIN_THREAD_NUM = 1;
static constexpr int ASYNC_MAX_THREAD_NUM = 4;

#define DS_MASTER_RPC_CONSTANT
#include "datasystem/master/object_cache/master_oc_service_body.inc"
#undef DS_MASTER_RPC_CONSTANT

#define DS_MASTER_RPC_HELPERS
#include "datasystem/master/object_cache/master_oc_service_body.inc"
#undef DS_MASTER_RPC_HELPERS

MasterOCServiceImpl::MasterOCServiceImpl(HostPort serverAddress, std::shared_ptr<PersistenceApi> persistApi,
                                         std::shared_ptr<AkSkManager> akSkManager,
                                         MetadataManagerHolder *metadataManagerHolder, ResourceManager *resourceManager,
                                         const cluster::MembershipEndpointView &topologyMembership,
                                         std::string localAddress,
                                         std::function<void(const HostPort &, const Status &)> metadataRpcObserver)
    : MasterOCService(serverAddress),
      masterAddress_(std::move(serverAddress)),
      persistenceApi_(persistApi),
      topologyMembership_(&topologyMembership),
      localAddress_(std::move(localAddress)),
      akSkManager_(akSkManager),
      metadataManagerHolder_(metadataManagerHolder),
      resourceManager_(resourceManager),
      metadataRpcObserver_(std::move(metadataRpcObserver))
{
}

MasterOCServiceImpl::~MasterOCServiceImpl()
{
    Shutdown();
}

void MasterOCServiceImpl::Shutdown()
{
    LOG(INFO) << "MasterOCServiceImpl exit";
    OCMigrateMetadataManager::Instance().Shutdown();
}

Status MasterOCServiceImpl::Init()
{
    RETURN_RUNTIME_ERROR_IF_NULL(topologyMembership_);
    reconciliationAsyncPool_ =
        std::make_unique<ThreadPool>(ASYNC_MIN_THREAD_NUM, ASYNC_MAX_THREAD_NUM, "Reconciliation");
    RETURN_IF_NOT_OK(OCMigrateMetadataManager::Instance().Init(masterAddress_, akSkManager_, topologyMembership_,
                                                               metadataManagerHolder_, metadataRpcObserver_));
    return Status::OK();
}

#define DS_MASTER_RPC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MASTER_RPC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MASTER_RPC_FORWARD(name, ...) name(__VA_ARGS__)
#define DS_MASTER_RPC_TASK_CAPTURE
#define DS_MASTER_RPC_GROUP_0
#include "datasystem/master/object_cache/master_oc_service_body.inc"
#undef DS_MASTER_RPC_GROUP_0
#undef DS_MASTER_RPC_TASK_CAPTURE
#undef DS_MASTER_RPC_FORWARD
#undef DS_MASTER_RPC_VERIFY
#undef DS_MASTER_RPC_ENTRY

std::unordered_map<std::string, QueryGlobalRefNumReqPb> MasterOCServiceImpl::QueryWorkerGRefReqPbGen(
    const std::unordered_set<std::string> &objectKeys, std::shared_ptr<master::OCMetadataManager> &ocMetadataManager)
{
    // Generate the pb used by each worker, with pb containing the required object_keys.
    std::unordered_map<std::string, QueryGlobalRefNumReqPb> queryTarget;  // ip-port string - pb.
    std::unordered_map<std::string, std::unordered_set<std::string>> refTable;
    ocMetadataManager->globalRefTable_->GetAllRef(refTable);
    for (const auto &objKey : objectKeys) {  // For each object_key in referrer set,
        auto iter = refTable.find(objKey);   // get the set of ref addr of this obj.
        if (iter == refTable.end()) {
            continue;
        }
        for (const auto &addr : iter->second) {      // For each ref addr,
            auto targetPb = queryTarget.find(addr);  // check if this addr is in queryTarget.
            if (targetPb == queryTarget.end()) {     // If not, add new the pb req with the obj.
                QueryGlobalRefNumReqPb req;
                req.add_object_keys(objKey);
                queryTarget[addr] = req;
            } else {  // If yes, add this obj to the req pb.
                targetPb->second.add_object_keys(objKey);
            }
            LOG(INFO) << "[GRef] Generate the targetPb with key:" << addr
                      << ", val: " << LogHelper::IgnoreSensitive(queryTarget.at(addr));
        }
    }
    return queryTarget;
}

Status MasterOCServiceImpl::SendQueryGRefReq(const HostPort &addr, QueryGlobalRefNumReqPb req,
                                             QueryGlobalRefNumRspPb &rsp)
{
    LOG(INFO) << "Master send gRef query to worker: " << addr.ToString();
    auto masterWorkerApi =
        MasterWorkerOCApi::CreateMasterWorkerOCApi(addr, masterAddress_, akSkManager_, masterWorkerOCService_);
    RETURN_IF_NOT_OK(masterWorkerApi->Init());
    RETURN_IF_NOT_OK(masterWorkerApi->QueryGlobalRefNumOnWorker(req, rsp));
    LOG(INFO) << "Master receive gRef query rsp from worker: " << addr.ToString();
    return Status::OK();
}

#define DS_MASTER_RPC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MASTER_RPC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MASTER_RPC_FORWARD(name, ...) name(__VA_ARGS__)
#define DS_MASTER_RPC_TASK_CAPTURE
#define DS_MASTER_RPC_GROUP_1
#include "datasystem/master/object_cache/master_oc_service_body.inc"
#undef DS_MASTER_RPC_GROUP_1
#undef DS_MASTER_RPC_TASK_CAPTURE
#undef DS_MASTER_RPC_FORWARD
#undef DS_MASTER_RPC_VERIFY
#undef DS_MASTER_RPC_ENTRY

void MasterOCServiceImpl::AssignLocalWorker(object_cache::MasterWorkerOCServiceImpl *masterWorkerService)
{
    masterWorkerOCService_ = masterWorkerService;  // save a copy of the ptr for the SendQueryGRefReq
}

bool MasterOCServiceImpl::HaveAsyncMetaRequest()
{
    return metadataManagerHolder_->HaveAsyncMetaRequest();
}

std::string MasterOCServiceImpl::GetETCDAsyncQueueUsage()
{
    std::shared_ptr<master::OCMetadataManager> ocMetadataManager;
    Status rc = metadataManagerHolder_->GetOcMetadataManager(ocMetadataManager);
    if (rc.IsError()) {
        return "";
    }
    return ocMetadataManager->GetETCDAsyncQueueUsage();
}

std::string MasterOCServiceImpl::GetMasterAsyncPoolUsage(int64_t intervalMs)
{
    std::shared_ptr<master::OCMetadataManager> ocMetadataManager;
    Status rc = metadataManagerHolder_->GetOcMetadataManager(ocMetadataManager);
    if (rc.IsError()) {
        return "";
    }
    return ocMetadataManager->GetMasterAsyncPoolUsage(intervalMs);
}

#define DS_MASTER_RPC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MASTER_RPC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MASTER_RPC_FORWARD(name, ...) name(__VA_ARGS__)
#define DS_MASTER_RPC_TASK_CAPTURE
#define DS_MASTER_RPC_GROUP_2
#include "datasystem/master/object_cache/master_oc_service_body.inc"
#undef DS_MASTER_RPC_GROUP_2
#undef DS_MASTER_RPC_TASK_CAPTURE
#undef DS_MASTER_RPC_FORWARD
#undef DS_MASTER_RPC_VERIFY
#undef DS_MASTER_RPC_ENTRY

std::string MasterOCServiceImpl::GetWorkerId()
{
    return metadataManagerHolder_->GetCurrentWorkerUuid();
}

#define DS_MASTER_RPC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MASTER_RPC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MASTER_RPC_FORWARD(name, ...) name(__VA_ARGS__)
#define DS_MASTER_RPC_TASK_CAPTURE
#define DS_MASTER_RPC_GROUP_3
#include "datasystem/master/object_cache/master_oc_service_body.inc"
#undef DS_MASTER_RPC_GROUP_3
#undef DS_MASTER_RPC_TASK_CAPTURE
#undef DS_MASTER_RPC_FORWARD
#undef DS_MASTER_RPC_VERIFY
#undef DS_MASTER_RPC_ENTRY
}  // namespace master
}  // namespace datasystem
