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
#ifdef DS_MASTER_SC_HELPERS
namespace {
Status WaitForMigrationSnapshot(const cluster::MembershipEndpointView &membership, const std::string &localAddress,
                                const std::string &sourceAddress, uint64_t batchEpoch, uint64_t requiredVersion,
                                std::chrono::steady_clock::time_point deadline,
                                std::shared_ptr<const cluster::TopologySnapshot> &snapshot)
{
    const uint64_t observedVersion = snapshot == nullptr ? 0 : snapshot->Version();
    const auto waitStart = std::chrono::steady_clock::now();
    VLOG(1) << "CLUSTER_TASK action=wait_migration_fence_snapshot local_address=" << localAddress
            << " source_address=" << sourceAddress << " batch_epoch=" << batchEpoch
            << " observed_version=" << observedVersion << " required_version=" << requiredVersion;
    auto waitStatus = membership.WaitForSnapshotVersion(requiredVersion, deadline, snapshot);
    const auto waitMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
    if (waitStatus.IsError()) {
        LOG(WARNING) << "CLUSTER_TASK action=wait_migration_fence_snapshot local_address=" << localAddress
                     << " source_address=" << sourceAddress << " batch_epoch=" << batchEpoch
                     << " observed_version=" << observedVersion << " required_version=" << requiredVersion
                     << " wait_ms=" << waitMs << " status=" << waitStatus.ToString();
        return waitStatus;
    }
    VLOG(1) << "CLUSTER_TASK action=migration_fence_snapshot_ready local_address=" << localAddress
            << " source_address=" << sourceAddress << " batch_epoch=" << batchEpoch
            << " observed_version=" << snapshot->Version() << " required_version=" << requiredVersion
            << " wait_ms=" << waitMs;
    return Status::OK();
}

Status ValidateMigrationRequest(const cluster::MembershipEndpointView *membership, const std::string &localAddress,
                                uint64_t topologyVersion, uint64_t batchEpoch, const std::string &sourceMemberId,
                                const std::string &targetMemberId, const std::string &sourceAddress)
{
    const bool hasFence = topologyVersion != 0 || batchEpoch != 0 || !sourceMemberId.empty() || !targetMemberId.empty();
    if (!hasFence) {
        return Status::OK();
    }
    CHECK_FAIL_RETURN_STATUS(topologyVersion > 0 && batchEpoch > 0 && !sourceMemberId.empty()
                                 && !targetMemberId.empty() && !sourceAddress.empty(),
                             K_INVALID, "Incomplete topology migration RPC fence");
    CHECK_FAIL_RETURN_STATUS(membership != nullptr && !localAddress.empty(), K_NOT_READY,
                             "Topology membership is unavailable for migration fence validation");
    const auto waitDeadlineBase = std::chrono::steady_clock::now();
    const int64_t remainingMs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTime();
    CHECK_FAIL_RETURN_STATUS(remainingMs > 0, K_RPC_DEADLINE_EXCEEDED,
                             "Migration RPC deadline expired before topology fence validation");
    const auto waitDeadline = waitDeadlineBase + std::chrono::milliseconds(remainingMs);
    std::shared_ptr<const cluster::TopologySnapshot> snapshot;
    auto snapshotStatus = membership->GetSnapshot(snapshot);
    if (snapshotStatus.IsError() && snapshotStatus.GetCode() != K_NOT_READY) {
        return snapshotStatus;
    }
    if (snapshotStatus.IsError() || snapshot->Version() < topologyVersion) {
        RETURN_IF_NOT_OK(WaitForMigrationSnapshot(*membership, localAddress, sourceAddress, batchEpoch, topologyVersion,
                                                   waitDeadline, snapshot));
    }
    cluster::TopologyMigrationFence fence{ topologyVersion, batchEpoch,
                                           { sourceMemberId, sourceAddress },
                                           { targetMemberId, localAddress } };
    RETURN_IF_NOT_OK(snapshot->ValidateMigrationFence(fence));
    CHECK_FAIL_RETURN_STATUS(GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTime() > 0,
                             K_RPC_DEADLINE_EXCEEDED,
                             "Migration RPC deadline expired after topology fence validation");
    return Status::OK();
}
}  // namespace
#endif

#ifdef DS_MASTER_SC_GROUP_0
Status MasterSCServiceImpl::DS_MASTER_SC_ENTRY(CreateProducer,
    std::shared_ptr<ServerUnaryWriterReader<CreateProducerRspPb, CreateProducerReqPb>> serverApi)
{
    CreateProducerReqPb req;
    CreateProducerRspPb rsp;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    return DS_MASTER_SC_FORWARD(CreateProducerImpl, serverApi, req, rsp);
}

Status MasterSCServiceImpl::DS_MASTER_SC_ENTRY(CreateProducerImpl,
    const std::shared_ptr<ServerUnaryWriterReader<CreateProducerRspPb, CreateProducerReqPb>> &serverApi,
    const CreateProducerReqPb &req, CreateProducerRspPb &rsp)
{
    Timer timer(req.timeout());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MASTER_SC_VERIFY(akSkManager_, req), "AK/SK failed.");
    LOG(INFO) << FormatString("Master receive create producer request: <%s> with timeout: %d",
                              LogHelper::IgnoreSensitive(req.producer_meta()), req.timeout());
    Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
    std::shared_ptr<SCMetadataManager> scMetadataManager;
    INJECT_POINT("master.CreateProducer");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(metadataManagerHolder_->GetScMetadataManager(scMetadataManager),
                                     "GetScMetadataManager failed");
    if (serverApi) {
        // Launch child thread to run the real logic and then return this thread. This avoids the rpc thread being
        // active during the logic of this request so that it can be re-used by other requests.
        auto traceId = Trace::Instance().GetTraceID();
        threadPool_->Execute([=]() mutable {
            GetRequestContext()->scTimeoutDuration.Init(timer.GetRemainingTimeMs());
            Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
            TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceId);
            Status rc = scMetadataManager->CreateProducer(req, rsp);
            CheckErrorReturn(
                rc, rsp, FormatString("[S:%s] CreateProducerImpl failed with rc ", req.producer_meta().stream_name()),
                serverApi);
        });
    } else {
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(scMetadataManager->CreateProducer(req, rsp), "CreateProducer failed");
    }
    LOG(INFO) << FormatString("Master create producer request: <%s> Successful",
                              LogHelper::IgnoreSensitive(req.producer_meta()));
    return Status::OK();
}

Status MasterSCServiceImpl::DS_MASTER_SC_ENTRY(CloseProducer,
    std::shared_ptr<ServerUnaryWriterReader<CloseProducerRspPb, CloseProducerReqPb>> serverApi)
{
    CloseProducerReqPb req;
    CloseProducerRspPb rsp;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    return DS_MASTER_SC_FORWARD(CloseProducerImpl, serverApi, req, rsp);
}

Status MasterSCServiceImpl::DS_MASTER_SC_ENTRY(CloseProducerImpl,
    const std::shared_ptr<ServerUnaryWriterReader<CloseProducerRspPb, CloseProducerReqPb>> &serverApi,
    const CloseProducerReqPb &req, CloseProducerRspPb &rsp)
{
    Timer timer(req.timeout());
    INJECT_POINT("master.CloseProducerImpl");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MASTER_SC_VERIFY(akSkManager_, req), "AK/SK failed.");
    std::string infoMsg;
    // If there's more than one producer to close, only log the count. If there is only one, show the detail
    if (req.producer_infos_size() == 1) {
        infoMsg = FormatString("S:%s", req.producer_infos(0).stream_name());
    } else {
        infoMsg = FormatString("Number of producers: %d", req.producer_infos_size());
    }
    LOG(INFO) << "Master receive close producer request: " << infoMsg << " with timeout: " << req.timeout();
    std::shared_ptr<SCMetadataManager> scMetadataManager;
    INJECT_POINT("master.CloseProducer");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(metadataManagerHolder_->GetScMetadataManager(scMetadataManager),
                                     "GetScMetadataManager failed");
    if (serverApi) {
        // Launch child thread to run the real logic and then return this thread. This avoids the rpc thread being
        // active during the logic of this request so that it can be re-used by other requests.
        auto traceId = Trace::Instance().GetTraceID();
        threadPool_->Execute([=]() mutable {
            GetRequestContext()->scTimeoutDuration.Init(timer.GetRemainingTimeMs());
            Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
            TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceId);
            Status rc = scMetadataManager->CloseProducer(req, rsp);
            CheckErrorReturn(rc, rsp, "CloseProducerImpl failed with rc", serverApi);
        });
    } else {
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(scMetadataManager->CloseProducer(req, rsp), "CloseProducer failed");
    }

    return Status::OK();
}

Status MasterSCServiceImpl::DS_MASTER_SC_ENTRY(Subscribe,
    std::shared_ptr<ServerUnaryWriterReader<SubscribeRspPb, SubscribeReqPb>> serverApi)
{
    SubscribeReqPb req;
    SubscribeRspPb rsp;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    return DS_MASTER_SC_FORWARD(SubscribeImpl, serverApi, req, rsp);
}

Status MasterSCServiceImpl::DS_MASTER_SC_ENTRY(SubscribeImpl,
    const std::shared_ptr<ServerUnaryWriterReader<SubscribeRspPb, SubscribeReqPb>> &serverApi,
    const SubscribeReqPb &req, SubscribeRspPb &rsp)
{
    Timer timer(req.timeout());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MASTER_SC_VERIFY(akSkManager_, req), "AK/SK failed.");
    LOG(INFO) << FormatString("Master receive subscribe request: <%s> with timeout: %d",
                              LogHelper::IgnoreSensitive(req.consumer_meta()), req.timeout());
    std::shared_ptr<SCMetadataManager> scMetadataManager;
    INJECT_POINT("master.Subscribe");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(metadataManagerHolder_->GetScMetadataManager(scMetadataManager),
                                     "GetScMetadataManager failed");
    if (serverApi) {
        // Launch child thread to run the real logic and then return this thread. This avoids the rpc thread being
        // active during the logic of this request so that it can be re-used by other requests.
        auto traceId = Trace::Instance().GetTraceID();
        threadPool_->Execute([=]() mutable {
            GetRequestContext()->scTimeoutDuration.Init(timer.GetRemainingTimeMs());
            Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
            TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceId);
            Status rc = scMetadataManager->Subscribe(req, rsp);
            CheckErrorReturn(rc, rsp,
                             FormatString("[S:%s] SubscribeImpl failed with rc", req.consumer_meta().stream_name()),
                             serverApi);
        });
    } else {
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(scMetadataManager->Subscribe(req, rsp), "Subscribe failed");
    }
    return Status::OK();
}

Status MasterSCServiceImpl::DS_MASTER_SC_ENTRY(CloseConsumer,
    std::shared_ptr<ServerUnaryWriterReader<CloseConsumerRspPb, CloseConsumerReqPb>> serverApi)
{
    CloseConsumerReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    CloseConsumerRspPb rsp;
    return DS_MASTER_SC_FORWARD(CloseConsumerImpl, serverApi, req, rsp);
}

Status MasterSCServiceImpl::DS_MASTER_SC_ENTRY(CloseConsumerImpl,
    const std::shared_ptr<ServerUnaryWriterReader<CloseConsumerRspPb, CloseConsumerReqPb>> &serverApi,
    const CloseConsumerReqPb &req, CloseConsumerRspPb &rsp)
{
    Timer timer(req.timeout());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MASTER_SC_VERIFY(akSkManager_, req), "AK/SK failed.");
    LOG(INFO) << FormatString("Master receive close consumer request: <%s> with timeout: %d",
                              LogHelper::IgnoreSensitive(req), req.timeout());
    std::shared_ptr<SCMetadataManager> scMetadataManager;
    INJECT_POINT("master.CloseConsumer");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(metadataManagerHolder_->GetScMetadataManager(scMetadataManager),
                                     "GetScMetadataManager failed");
    if (serverApi) {
        // Launch child thread to run the real logic and then return this thread. This avoids the rpc thread being
        // active during the logic of this request so that it can be re-used by other requests.
        auto traceId = Trace::Instance().GetTraceID();
        threadPool_->Execute([=]() mutable {
            GetRequestContext()->scTimeoutDuration.Init(timer.GetRemainingTimeMs());
            Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
            TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceId);
            Status rc = scMetadataManager->CloseConsumer(req, rsp);
            CheckErrorReturn(rc, rsp,
                             FormatString("[S:%s] CloseConsumer failed with rc", req.consumer_meta().stream_name()),
                             serverApi);
        });
    } else {
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(scMetadataManager->CloseConsumer(req, rsp), "CloseConsumer failed");
        GetRequestContext()->scTimeoutDuration.Reset();
    }

    return Status::OK();
}

Status MasterSCServiceImpl::DS_MASTER_SC_ENTRY(DeleteStream,
    const DeleteStreamReqPb &req, DeleteStreamRspPb &rsp)
{
    GetRequestContext()->scTimeoutDuration.Init(req.timeout());
    Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MASTER_SC_VERIFY(akSkManager_, req), "AK/SK failed.");
    std::shared_ptr<SCMetadataManager> scMetadataManager;
    INJECT_POINT("master.DeleteStream");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(metadataManagerHolder_->GetScMetadataManager(scMetadataManager),
                                     "GetScMetadataManager failed");
    LOG(INFO) << FormatString("Master receive delete stream request: <%s> with timeout: %d",
                              LogHelper::IgnoreSensitive(req), req.timeout());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(scMetadataManager->DeleteStream(req, rsp), "DeleteStream failed");
    return Status::OK();
}

Status MasterSCServiceImpl::DS_MASTER_SC_ENTRY(QueryGlobalProducersNum,
    const QueryGlobalNumReqPb &req, QueryGlobalNumRsqPb &rsp)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MASTER_SC_VERIFY(akSkManager_, req), "AK/SK failed.");
    VLOG(SC_NORMAL_LOG_LEVEL) << FormatString("Master receive query producer number request: <%s>",
                                              LogHelper::IgnoreSensitive(req));
    std::shared_ptr<SCMetadataManager> scMetadataManager;
    INJECT_POINT("master.QueryGlobalProducersNum");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(metadataManagerHolder_->GetScMetadataManager(scMetadataManager),
                                     "GetScMetadataManager failed");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(scMetadataManager->QueryGlobalProducersNum(req, rsp),
                                     "QueryGlobalProducersNum failed");
    return Status::OK();
}

Status MasterSCServiceImpl::DS_MASTER_SC_ENTRY(QueryGlobalConsumersNum,
    const QueryGlobalNumReqPb &req, QueryGlobalNumRsqPb &rsp)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MASTER_SC_VERIFY(akSkManager_, req), "AK/SK failed.");
    VLOG(SC_NORMAL_LOG_LEVEL) << FormatString("Master receive query consumer number request: <%s>",
                                              LogHelper::IgnoreSensitive(req));
    std::shared_ptr<SCMetadataManager> scMetadataManager;
    INJECT_POINT("master.QueryGlobalConsumersNum");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(metadataManagerHolder_->GetScMetadataManager(scMetadataManager),
                                     "GetScMetadataManager failed");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(scMetadataManager->QueryGlobalConsumersNum(req, rsp),
                                     "QueryGlobalConsumersNum failed");
    return Status::OK();
}

#endif

#ifdef DS_MASTER_SC_GROUP_1
Status MasterSCServiceImpl::DS_MASTER_SC_ENTRY(MigrateSCMetadata,
    const MigrateSCMetadataReqPb &req, MigrateSCMetadataRspPb &rsp)
{
    ScopedRequestContext ctx;
    Timer timer;
#ifdef DS_MASTER_SC_NATIVE_SIGNATURE
    auto copyReq = req;
    for (int i = 0; i < copyReq.stream_metas_size(); ++i) {
        auto *meta = copyReq.mutable_stream_metas(i);
        if (meta != nullptr) {
            meta->clear_notifications();
        }
    }
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MASTER_SC_VERIFY(akSkManager_, copyReq), "AK/SK failed.");
#else
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_MASTER_SC_VERIFY(akSkManager_, req), "AK/SK failed.");
#endif
    RETURN_IF_NOT_OK(ValidateMigrationRequest(topologyMembership_, localAddress_, req.topology_version(),
                                              req.batch_epoch(), req.source_member_id(), req.target_member_id(),
                                              req.source_addr()));
    std::shared_ptr<SCMetadataManager> scMetadataManager;
    INJECT_POINT("master.MigrateSCMetadata");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(metadataManagerHolder_->GetScMetadataManager(scMetadataManager),
                                     "GetScMetadataManager failed");
    RETURN_IF_NOT_OK(scMetadataManager->SaveMigrationMetadata(req, rsp));
    GetMasterTimeCost().Append("Total MigrateMetadata", timer.ElapsedMilliSecond());
    LOG(INFO) << FormatString("The operations of SC master MigrateMetadata %s", GetMasterTimeCost().GetInfo());
    return Status::OK();
}

#endif
