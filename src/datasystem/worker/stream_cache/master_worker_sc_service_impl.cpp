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

#include "datasystem/worker/stream_cache/master_worker_sc_service_impl.h"

#include "datasystem/common/util/format.h"
#include "datasystem/common/log/log_helper.h"
#include "datasystem/common/util/strings_util.h"

namespace datasystem {
namespace worker {
namespace stream_cache {
MasterWorkerSCServiceImpl::MasterWorkerSCServiceImpl(HostPort serverAddr, HostPort masterAddr,
                                                     ClientWorkerSCServiceImpl *clientSvc,
                                                     std::shared_ptr<AkSkManager> akSkManager)
    : localWorkerAddress_(std::move(serverAddr)),
      masterAddress_(std::move(masterAddr)),
      clientWorkerSCSvc_(clientSvc),
      akSkManager_(std::move(akSkManager))
{
}

Status MasterWorkerSCServiceImpl::Init()
{
    CHECK_FAIL_RETURN_STATUS(clientWorkerSCSvc_ != nullptr, StatusCode::K_NOT_READY,
                             "ClientWorkerService must be initialized before MasterWorkerService construction");
    LOG(INFO) << FormatString("[%s, Master address:%s] Initialization success.", LogPrefix(true),
                              masterAddress_.ToString());
    return MasterWorkerSCService::Init();
}

Status MasterWorkerSCServiceImpl::AddRemoteConsumer(const std::shared_ptr<StreamManager> &streamManager,
                                                    const ConsumerMetaPb &meta)
{
    const auto &workerAddr = meta.worker_address();
    const auto &consumerId = meta.consumer_id();
    const auto &streamName = meta.stream_name();
    HostPort workerAddress(workerAddr.host(), workerAddr.port());

    SubscriptionConfig config(meta.sub_config().subscription_name(),
                              ToSubscriptionType(meta.sub_config().subscription_type()));
    uint64_t lastAckCursor;
    RETURN_IF_NOT_OK_EXCEPT(streamManager->AddRemoteSubNode(workerAddress, config, consumerId, lastAckCursor),
                            K_DUPLICATED);
    auto &remoteWorkerManager = clientWorkerSCSvc_->remoteWorkerManager_;
    RETURN_IF_NOT_OK(remoteWorkerManager->AddRemoteConsumer(streamManager, localWorkerAddress_, workerAddress,
                                                            streamName, config, consumerId, lastAckCursor));
    VLOG(SC_NORMAL_LOG_LEVEL) << FormatString("[%s, S:%s] New Consumer <Sub:%s, C:%s, RW:%s> success", LogPrefix(),
                                              streamName, config.subscriptionName, consumerId,
                                              workerAddress.ToString());
    return Status::OK();
}

Status MasterWorkerSCServiceImpl::DelRemoteConsumer(const std::shared_ptr<StreamManager> &streamManager,
                                                    const ConsumerMetaPb &consumerMeta)
{
    const auto &workerAddr = consumerMeta.worker_address();
    const auto &consumerId = consumerMeta.consumer_id();
    const auto &streamName = consumerMeta.stream_name();
    HostPort workerAddress(workerAddr.host(), workerAddr.port());

    VLOG(SC_NORMAL_LOG_LEVEL) << FormatString("[%s, S:%s] DelSubNode <Sub:%s, C:%s, Remote:%s> begin", LogPrefix(),
                                              streamName, consumerMeta.sub_config().subscription_name(), consumerId,
                                              workerAddress.ToString());

    RETURN_IF_NOT_OK_EXCEPT(streamManager->DelRemoteSubNode(workerAddress, consumerId), K_NOT_FOUND);
    auto &remoteWorkerManager = clientWorkerSCSvc_->remoteWorkerManager_;
    RETURN_IF_NOT_OK_EXCEPT(remoteWorkerManager->DelRemoteConsumer(workerAddress.ToString(), streamName, consumerId),
                            K_NOT_FOUND);
    VLOG(SC_NORMAL_LOG_LEVEL) << FormatString("[%s, S:%s] DelSubNode <Sub:%s, C:%s, Remote:%s> success", LogPrefix(),
                                              streamName, consumerMeta.sub_config().subscription_name(), consumerId,
                                              workerAddress.ToString());
    return Status::OK();
}

#define DS_MASTER_WORKER_SC_RPC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MASTER_WORKER_SC_RPC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MASTER_WORKER_SC_RPC_GROUP_0
#include "datasystem/worker/stream_cache/master_worker_sc_service_body.inc"
#undef DS_MASTER_WORKER_SC_RPC_GROUP_0
#undef DS_MASTER_WORKER_SC_RPC_VERIFY
#undef DS_MASTER_WORKER_SC_RPC_ENTRY

Status MasterWorkerSCServiceImpl::ClearAllRemoteConsumer(const ClearRemoteInfoReqPb &req, ClearRemoteInfoRspPb &rsp)
{
    // ClearAllRemoteConsumer RPC deprecated.
    INJECT_POINT("MasterWorkerSCServiceImpl.ClearAllRemoteConsumer.sleep");
    (void)rsp;
    (void)req;
    return Status::OK();
}

#define DS_MASTER_WORKER_SC_RPC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MASTER_WORKER_SC_RPC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MASTER_WORKER_SC_RPC_GROUP_1
#include "datasystem/worker/stream_cache/master_worker_sc_service_body.inc"
#undef DS_MASTER_WORKER_SC_RPC_GROUP_1
#undef DS_MASTER_WORKER_SC_RPC_VERIFY
#undef DS_MASTER_WORKER_SC_RPC_ENTRY

std::string MasterWorkerSCServiceImpl::LogPrefix(bool withAddress) const
{
    if (withAddress) {
        return FormatString("MasterWorkerSvc, Node:%s", localWorkerAddress_.ToString());
    } else {
        return "MasterWorkerSvc";
    }
}

Status MasterWorkerSCServiceImpl::SyncRemoteConsumer(const std::shared_ptr<StreamManager> &streamManager,
                                                     const std::vector<ConsumerMeta> &remoteConsumerSet,
                                                     uint64_t lastAckCursor)
{
    const std::string streamName = streamManager->GetStreamName();
    auto &remoteWorkerManager = clientWorkerSCSvc_->remoteWorkerManager_;
    for (const auto &remoteConsumer : remoteConsumerSet) {
        // Do sync part
        RETURN_IF_NOT_OK(remoteWorkerManager->AddRemoteConsumer(
            streamManager, localWorkerAddress_, remoteConsumer.WorkerAddress(), streamName, remoteConsumer.SubConfig(),
            remoteConsumer.ConsumerId(), lastAckCursor));
    }
    return Status::OK();
}

#define DS_MASTER_WORKER_SC_RPC_ENTRY(name, ...) name(__VA_ARGS__)
#define DS_MASTER_WORKER_SC_RPC_VERIFY(manager, request) manager->VerifySignatureAndTimestamp(request)
#define DS_MASTER_WORKER_SC_RPC_GROUP_2
#include "datasystem/worker/stream_cache/master_worker_sc_service_body.inc"
#undef DS_MASTER_WORKER_SC_RPC_GROUP_2
#undef DS_MASTER_WORKER_SC_RPC_VERIFY
#undef DS_MASTER_WORKER_SC_RPC_ENTRY
}  // namespace stream_cache
}  // namespace worker
}  // namespace datasystem
