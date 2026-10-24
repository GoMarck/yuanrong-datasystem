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
 * Description: Implement the stream cache services on the master.
 */
#include "datasystem/common/util/uuid_generator.h"
#include "datasystem/master/stream_cache/master_sc_service_impl.h"

#include <chrono>
#include <utility>

#include "datasystem/common/log/log_helper.h"
#include "datasystem/common/stream_cache/util.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/master/metadata_manager_holder.h"
#include "datasystem/master/stream_cache/sc_metadata_manager.h"
#include "datasystem/master/stream_cache/sc_migrate_metadata_manager.h"

DS_DEFINE_int32(master_sc_thread_num, 128, "Max number of threads for (non rpc) master stream cache service work");

namespace datasystem {
namespace master {
#define DS_MASTER_SC_HELPERS
#include "datasystem/master/stream_cache/master_sc_service_body.inc"
#undef DS_MASTER_SC_HELPERS

MasterSCServiceImpl::MasterSCServiceImpl(const HostPort &masterAddress, std::shared_ptr<AkSkManager> akSkManager,
                                         MetadataManagerHolder *metadataManagerHolder,
                                         const cluster::MembershipEndpointView &topologyMembership,
                                         std::string localAddress, bool isRestart,
                                         bool controlBackendAvailableAtStartup)
    : MasterSCService(masterAddress),
      akSkManager_(std::move(akSkManager)),
      topologyMembership_(&topologyMembership),
      localAddress_(std::move(localAddress)),
      isRestart_(isRestart),
      controlBackendAvailableAtStartup_(controlBackendAvailableAtStartup),
      metadataManagerHolder_(metadataManagerHolder)
{
}

void MasterSCServiceImpl::Shutdown()
{
    LOG(INFO) << "MasterSCServiceImpl shutdown.";
    SCMigrateMetadataManager::Instance().Shutdown();
}

Status MasterSCServiceImpl::Init()
{
    RETURN_IF_NOT_OK(MasterSCService::Init());
    const size_t MIN_THREADS = 1;
    size_t minThreads = std::min<size_t>(MIN_THREADS, FLAGS_master_sc_thread_num);
    RETURN_IF_EXCEPTION_OCCURS(threadPool_ =
                                   std::make_unique<ThreadPool>(minThreads, FLAGS_master_sc_thread_num, "MScThreads"));
    RETURN_IF_NOT_OK(SCMigrateMetadataManager::Instance().Init(GetLocalAddr(), akSkManager_, topologyMembership_,
                                                               metadataManagerHolder_));
    VLOG(SC_NORMAL_LOG_LEVEL) << "MasterSCServiceImpl initialization success";
    return Status::OK();
}

#define DS_MASTER_SC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MASTER_SC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MASTER_SC_FORWARD(name, ...) name(__VA_ARGS__)
#define DS_MASTER_SC_NATIVE_SIGNATURE
#define DS_MASTER_SC_GROUP_0
#include "datasystem/master/stream_cache/master_sc_service_body.inc"
#undef DS_MASTER_SC_GROUP_0
#undef DS_MASTER_SC_NATIVE_SIGNATURE
#undef DS_MASTER_SC_FORWARD
#undef DS_MASTER_SC_VERIFY
#undef DS_MASTER_SC_ENTRY

Status MasterSCServiceImpl::StartCheckMetadata()
{
    if (!isRestart_ || !controlBackendAvailableAtStartup_) {
        return Status::OK();
    }
    std::shared_ptr<const cluster::TopologySnapshot> topologySnapshot;
    CHECK_FAIL_RETURN_STATUS(topologyMembership_ != nullptr, K_NOT_READY,
                             "Topology membership view is not available");
    RETURN_IF_NOT_OK(topologyMembership_->GetSnapshot(topologySnapshot));
    std::vector<HostPort> nodeAddrs;
    nodeAddrs.reserve(topologySnapshot->CommittedMembers().size());
    for (const auto *member : topologySnapshot->CommittedMembers()) {
        HostPort address;
        RETURN_IF_NOT_OK(address.ParseString(member->identity.address));
        nodeAddrs.emplace_back(std::move(address));
    }
    const size_t maxThreadNum = 20;
    // Add a condition to forbid thread pool size creation with minThreadNum 0 to avoid cpp runtime exception.
    if (nodeAddrs.empty()) {
        return Status::OK();
    }
    auto checkPool = std::make_unique<ThreadPool>(std::min(maxThreadNum, nodeAddrs.size()), 0, "MScCheck");
    // broadcast over all active masters
    std::vector<std::future<void>> rcs(nodeAddrs.size());
    for (size_t i = 0; i < nodeAddrs.size(); ++i) {
        rcs[i] = checkPool->Submit([this, i, &nodeAddrs]() {
            auto func = [i, &nodeAddrs](const std::string &workerId, MetadataManager metadataMansger) {
                auto traceGuard = Trace::Instance().SetTraceNewID(GetStringUuid() + "-sc-check");
                LOG(INFO) << "Check metadata for worker id " << workerId;
                if (metadataMansger.sc != nullptr) {
                    metadataMansger.sc->StartCheckMetadata(nodeAddrs[i]);
                }
                return Status::OK();
            };
            (void)metadataManagerHolder_->ApplyForAllMetaManager(func);
        });
    }
    // wait for the end of all reconciliations
    // we do not check the results and ignore the failed status
    for (const auto &rc : rcs) {
        rc.wait();
    }
    return Status::OK();
}

#define DS_MASTER_SC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MASTER_SC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MASTER_SC_FORWARD(name, ...) name(__VA_ARGS__)
#define DS_MASTER_SC_NATIVE_SIGNATURE
#define DS_MASTER_SC_GROUP_1
#include "datasystem/master/stream_cache/master_sc_service_body.inc"
#undef DS_MASTER_SC_GROUP_1
#undef DS_MASTER_SC_NATIVE_SIGNATURE
#undef DS_MASTER_SC_FORWARD
#undef DS_MASTER_SC_VERIFY
#undef DS_MASTER_SC_ENTRY

std::string MasterSCServiceImpl::GetWorkerId()
{
    return metadataManagerHolder_->GetCurrentWorkerUuid();
}
}  // namespace master
}  // namespace datasystem
