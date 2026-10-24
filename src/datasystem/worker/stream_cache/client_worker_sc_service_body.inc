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


#ifdef DS_CLIENT_SC_RPC_GROUP_0
Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(CreateProducer,
    std::shared_ptr<ServerUnaryWriterReader<CreateProducerRspPb, CreateProducerReqPb>> serverApi)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    CreateProducerReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");

    auto recorder =
        std::make_shared<StreamAccessRecorder>(AccessRecorder::Stream(AccessRecorderKey::DS_POSIX_CREATE_PRODUCER));
    recorder->StreamName(req.stream_name())
        .ProducerId(req.producer_id())
        .PageSize(req.page_size())
        .MaxStreamSize(req.max_stream_size())
        .AutoCleanup(req.auto_cleanup())
        .RetainForNumConsumers(req.retain_num_consumer())
        .EncryptStream(req.encrypt_stream())
        .ReserveSize(req.reserve_size())
        .StreamMode(req.stream_mode());
    auto rc = DS_CLIENT_SC_RPC_FORWARD(CreateProducerInternal, req, recorder, serverApi);
    return rc;
}

Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(CreateProducerInternal,
    const CreateProducerReqPb &req, std::shared_ptr<StreamAccessRecorder> recorder,
    std::shared_ptr<ServerUnaryWriterReader<CreateProducerRspPb, CreateProducerReqPb>> serverApi)
{
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");
    LOG(INFO) << "Worker received CreateProducer request: " << LogHelper::IgnoreSensitive(req);
    TimeoutDuration parentDuration = GetRequestContext()->scTimeoutDuration;
    Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.stream_name());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(CheckConnection(namespaceUri), "worker check connection failed");

    PerfPoint point(PerfKey::WORKER_CREATE_PRODUCER_ALL);

    // The real work of the close will be driven in another thread. Launch it now and then release this current thread
    // so that it does not hold up the rpc threads.
    auto traceId = Trace::Instance().GetTraceID();
    threadPool_->Execute([=]() mutable {
        GetRequestContext()->scTimeoutDuration = parentDuration;
        Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceId);
        CreateProducerRspPb rsp;
        Status rc = CreateProducerImpl(namespaceUri, req, rsp);
        CheckErrorReturn(rc, rsp, FormatString("[S:%s] CreateProducerImpl failed with rc ", namespaceUri), serverApi);
        recorder->Result(rc);
        if (rc.IsOk()) {
            recorder->SenderProducerNo(rsp.sender_producer_no())
                .EnableDataVerification(rsp.enable_data_verification())
                .StreamNo(rsp.stream_no())
                .SharedPageSize(rsp.shared_page_size())
                .EnableSharedPage(rsp.enable_shared_page());
        }
        recorder->Record();
        recorder.reset();
    });

    return Status::OK();
}

#endif

#ifdef DS_CLIENT_SC_RPC_GROUP_1
Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(CloseProducer,
    std::shared_ptr<ServerUnaryWriterReader<CloseProducerRspPb, CloseProducerReqPb>> serverApi)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    CloseProducerReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    auto recorder =
        std::make_shared<StreamAccessRecorder>(AccessRecorder::Stream(AccessRecorderKey::DS_POSIX_CLOSE_PRODUCER));
    recorder->StreamName(req.stream_name()).ProducerId(req.producer_id()).ClientId(req.client_id());
    auto rc = DS_CLIENT_SC_RPC_FORWARD(CloseProducerInternal, req, recorder, serverApi);
    return rc;
}

Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(CloseProducerInternal,
    const CloseProducerReqPb &req, std::shared_ptr<StreamAccessRecorder> recorder,
    std::shared_ptr<ServerUnaryWriterReader<CloseProducerRspPb, CloseProducerReqPb>> serverApi)
{
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");
    TimeoutDuration parentDuration = GetRequestContext()->scTimeoutDuration;
    Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
    LOG(INFO) << "Worker received CloseProducer request:" << LogHelper::IgnoreSensitive(req);
    const std::string &producerId = req.producer_id();
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.stream_name());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(CheckConnection(namespaceUri), "worker check connection failed");
    const std::string &clientId = req.client_id();
    PerfPoint point(PerfKey::WORKER_CLOSE_PRODUCER_ALL);

    // The real work of the close will be driven in another thread. Launch it now and then release this current thread
    // so that it does not hold up the rpc threads.
    auto traceId = Trace::Instance().GetTraceID();
    threadPool_->Execute([=]() mutable {
        GetRequestContext()->scTimeoutDuration = parentDuration;
        Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceId);
        CloseProducerRspPb rsp;
        Status rc = CloseProducerImpl(producerId, namespaceUri, true);
        if (rc.IsOk() || rc.GetCode() == StatusCode::K_SC_PRODUCER_NOT_FOUND) {
            std::unique_lock<SharedMutex> lock(clearMutex_);
            clientProducers_[clientId].remove_if([&namespaceUri, &producerId](const StreamProducer &data) {
                return (data.streamName_ == namespaceUri && data.producerId_ == producerId);
            });
        }
        LOG(INFO) << FormatString("[%s, S:%s, P:%s] CloseProducer finish with %s", LogPrefix(), namespaceUri,
                                  producerId, rc.ToString());
        CheckErrorReturn(rc, rsp, FormatString("[S:%s] CloseProducerImpl failed with rc ", namespaceUri), serverApi);
        recorder->Result(rc);
        recorder->Record();
        recorder.reset();
    });

    return Status::OK();
}

#endif

#ifdef DS_CLIENT_SC_RPC_GROUP_2
Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(Subscribe,
    std::shared_ptr<ServerUnaryWriterReader<SubscribeRspPb, SubscribeReqPb>> serverApi)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    SubscribeReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    auto recorder =
        std::make_shared<StreamAccessRecorder>(AccessRecorder::Stream(AccessRecorderKey::DS_POSIX_SUBSCRIBE));
    recorder->StreamName(req.stream_name()).ConsumerId(req.consumer_id()).ClientId(req.client_id());
    auto rc = DS_CLIENT_SC_RPC_FORWARD(SubscribeInternal, req, recorder, serverApi);
    return rc;
}

Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(SubscribeInternal,
    const SubscribeReqPb &req, std::shared_ptr<StreamAccessRecorder> recorder,
    std::shared_ptr<ServerUnaryWriterReader<SubscribeRspPb, SubscribeReqPb>> serverApi)
{
    LOG(INFO) << "Worker received Subscribe request:" << LogHelper::IgnoreSensitive(req);
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");
    TimeoutDuration parentDuration = GetRequestContext()->scTimeoutDuration;
    Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
    PerfPoint point(PerfKey::WORKER_CREATE_SUB_ALL);

    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.stream_name());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(CheckConnection(namespaceUri), "worker check connection failed");

    // The real work of the close will be driven in another thread. Launch it now and then release this current thread
    // so that it does not hold up the rpc threads.
    auto traceId = Trace::Instance().GetTraceID();
    threadPool_->Execute([=]() mutable {
        GetRequestContext()->scTimeoutDuration = parentDuration;
        Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceId);
        SubscribeRspPb rsp;
        Status rc = SubscribeImpl(namespaceUri, req, rsp);
        CheckErrorReturn(rc, rsp, "SubscribeImpl failed with rc ", serverApi);
        recorder->Result(rc);
        recorder->Record();
        recorder.reset();
    });

    return Status::OK();
}

#endif

#ifdef DS_CLIENT_SC_RPC_GROUP_3
Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(CloseConsumer,
    std::shared_ptr<ServerUnaryWriterReader<CloseConsumerRspPb, CloseConsumerReqPb>> serverApi)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    CloseConsumerReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    auto recorder =
        std::make_shared<StreamAccessRecorder>(AccessRecorder::Stream(AccessRecorderKey::DS_POSIX_CLOSE_CONSUMER));
    recorder->StreamName(req.stream_name())
        .ConsumerId(req.consumer_id())
        .SubscriptionName(req.subscription_name())
        .ClientId(req.client_id());
    auto rc = DS_CLIENT_SC_RPC_FORWARD(CloseConsumerInternal, req, recorder, serverApi);
    return rc;
}

Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(CloseConsumerInternal,
    const CloseConsumerReqPb &req, std::shared_ptr<StreamAccessRecorder> recorder,
    std::shared_ptr<ServerUnaryWriterReader<CloseConsumerRspPb, CloseConsumerReqPb>> serverApi)
{
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");
    TimeoutDuration parentDuration = GetRequestContext()->scTimeoutDuration;
    Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
    LOG(INFO) << "Worker received CloseConsumer request:" << LogHelper::IgnoreSensitive(req);
    const std::string &consumerId = req.consumer_id();
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.stream_name());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(CheckConnection(namespaceUri), "worker check connection failed");
    const std::string &clientId = req.client_id();

    // The real work of the close will be driven in another thread. Launch it now and then release this current thread
    // so that it does not hold up the rpc threads.
    auto traceId = Trace::Instance().GetTraceID();
    threadPool_->Execute([=]() mutable {
        GetRequestContext()->scTimeoutDuration = parentDuration;
        Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceId);
        Status rc = CloseConsumerImpl(consumerId, namespaceUri, req.subscription_name(), true, WORKER_LOCK_ID);
        if (rc.IsOk() || rc.GetCode() == StatusCode::K_SC_CONSUMER_NOT_FOUND) {
            std::unique_lock<SharedMutex> lock(clearMutex_);
            clientConsumers_[clientId].remove_if([&namespaceUri, &consumerId](const SubInfo &data) {
                return (data.streamName == namespaceUri && data.consumerId == consumerId);
            });
        }
        LOG(INFO) << FormatString("[%s, S:%s, C:%s] CloseConsumer finish with %s", LogPrefix(), namespaceUri,
                                  consumerId, rc.ToString());
        // Flow replies back for unary rpc
        if (rc.IsOk()) {
            // Success case, flow the response back to client (rc of OK is inferred)
            CloseConsumerRspPb rsp;
            LOG_IF_ERROR(serverApi->Write(rsp), "Write reply to client stream failed");
        } else {
            // Error case, flow the rc back to client
            LOG_IF_ERROR(serverApi->SendStatus(rc), "Write reply to client stream failed");
        }
        recorder->Result(rc);
        recorder->Record();
        recorder.reset();
    });

    return Status::OK();
}

#endif

#ifdef DS_CLIENT_SC_RPC_GROUP_4
Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(GetDataPage,
    std::shared_ptr<ServerUnaryWriterReader<GetDataPageRspPb, GetDataPageReqPb>> serverApi)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    GetDataPageReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.stream_name());
    VLOG(SC_NORMAL_LOG_LEVEL) << FormatString("Worker(%s) receive GetDataPage request, namespaceUri: %s",
                                              localWorkerAddress_.ToString(), namespaceUri);
    StreamManagerMap::const_accessor accessor;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(GetStreamManager(namespaceUri, accessor), "worker get stream manager failed");
    std::shared_ptr<StreamManager> streamManager = accessor->second;
    RETURN_IF_NOT_OK(streamManager->CheckIfStreamActive());  // Check for Reset or Delete
    std::shared_ptr<Subscription> subscription;
    RETURN_IF_NOT_OK(streamManager->GetSubscription(req.subscription_name(), subscription));
    return streamManager->GetDataPage(req, subscription, serverApi);
}

Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(GetLastAppendCursor,
    const LastAppendCursorReqPb &req, LastAppendCursorRspPb &rsp)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.stream_name());
    StreamManagerMap::const_accessor accessor;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(GetStreamManager(namespaceUri, accessor), "worker get stream manager failed");
    std::shared_ptr<StreamManager> streamManager = accessor->second;
    rsp.set_last_append_cursor(streamManager->GetLastAppendCursor());

    return Status::OK();
}

#endif

#ifdef DS_CLIENT_SC_RPC_GROUP_5
Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(CreateShmPage,
    std::shared_ptr<ServerUnaryWriterReader<CreateShmPageRspPb, CreateShmPageReqPb>> serverApi)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    PerfPoint point(PerfKey::WORKER_CREATE_WRITE_PAGE_ALL);
    CreateShmPageReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");

    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");

    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.stream_name());
    VLOG(SC_NORMAL_LOG_LEVEL) << FormatString("[%s, S:%s] CreateShmPage request: %s", LogPrefix(), namespaceUri,
                                              LogHelper::IgnoreSensitive(req));
    StreamManagerMap::const_accessor accessor;
    RETURN_IF_NOT_OK(GetStreamManager(namespaceUri, accessor));
    std::shared_ptr<StreamManager> streamManager = accessor->second;
    RETURN_IF_NOT_OK(streamManager->CheckIfStreamActive());  // Check for Reset or Delete
    // Create a blocked request for FIFO. There can be some previous requests that got OOM and are still waiting.
    auto fn = std::bind(&StreamManager::AllocDataPage, streamManager, std::placeholders::_1);
    auto blockedReq = std::make_unique<BlockedCreateRequest<CreateShmPageRspPb, CreateShmPageReqPb>>(
        namespaceUri, req, streamManager->GetStreamPageSize(), serverApi, fn);
    // Lock to compete with StreamManager::UnblockProducers
    RETURN_IF_NOT_OK(streamManager->AddBlockedCreateRequest(this, std::move(blockedReq), true));
    AsyncSendMemReq<CreateShmPageRspPb, CreateShmPageReqPb>(namespaceUri);
    return Status::OK();
}

#endif

#ifdef DS_CLIENT_SC_RPC_GROUP_6
Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(AllocBigShmMemory,
    std::shared_ptr<ServerUnaryWriterReader<CreateLobPageRspPb, CreateLobPageReqPb>> serverApi)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    CreateLobPageReqPb req;
    RETURN_IF_NOT_OK(serverApi->Read(req));
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.stream_name());
    VLOG(SC_NORMAL_LOG_LEVEL) << FormatString("Worker(%s) receive AllocBigShmMemory request, namespaceUri: %s",
                                              localWorkerAddress_.ToString(), namespaceUri);
    StreamManagerMap::const_accessor accessor;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(GetStreamManager(namespaceUri, accessor), "worker get stream manager failed");
    std::shared_ptr<StreamManager> streamManager = accessor->second;
    RETURN_IF_NOT_OK(streamManager->CheckIfStreamActive());  // Check for Reset or Delete
    // Create a blocked request for FIFO. There can be some previous requests that got OOM and are still waiting.
    auto fn = std::bind(&StreamManager::AllocBigShmMemory, streamManager, std::placeholders::_1);
    auto blockedReq = std::make_unique<BlockedCreateRequest<CreateLobPageRspPb, CreateLobPageReqPb>>(
        namespaceUri, req, req.page_size(), serverApi, fn);
    // Lock to compete with StreamManager::UnblockProducers
    RETURN_IF_NOT_OK(streamManager->AddBlockedCreateRequest(this, std::move(blockedReq), true));
    AsyncSendMemReq<CreateLobPageRspPb, CreateLobPageReqPb>(namespaceUri);
    return Status::OK();
}

Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(ReleaseBigShmMemory,
    std::shared_ptr<ServerUnaryWriterReader<ReleaseLobPageRspPb, ReleaseLobPageReqPb>> serverApi)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    ReleaseLobPageReqPb req;
    RETURN_IF_NOT_OK(serverApi->Read(req));
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.stream_name());
    VLOG(SC_NORMAL_LOG_LEVEL) << FormatString("Worker(%s) receive ReleaseBigShmMemory request, namespaceUri: %s",
                                              localWorkerAddress_.ToString(), namespaceUri);
    StreamManagerMap::const_accessor accessor;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(GetStreamManager(namespaceUri, accessor), "worker get stream manager failed");
    std::shared_ptr<StreamManager> streamManager = accessor->second;
    RETURN_IF_NOT_OK(streamManager->CheckIfStreamActive());  // Check for Reset or Delete
    RETURN_IF_NOT_OK(streamManager->ReleaseBigShmMemory(serverApi, req));
    return Status::OK();
}

#endif

#ifdef DS_CLIENT_SC_RPC_GROUP_7
Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(DeleteStream,
    const DeleteStreamReqPb &req, DeleteStreamRspPb &rsp)
{
    auto access = AccessRecorder::Stream(AccessRecorderKey::DS_POSIX_DELETE_STREAM);
    auto rc = DS_CLIENT_SC_RPC_FORWARD(DeleteStreamImpl, req, rsp);
    access.StreamName(req.stream_name()).ClientId(req.client_id()).Result(rc).Record();
    return rc;
}

Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(DeleteStreamImpl,
    const DeleteStreamReqPb &req, DeleteStreamRspPb &rsp)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    (void)rsp;
    Raii outerResetDuration([]() { GetRequestContext()->scTimeoutDuration.Reset(); });
    PerfPoint point(PerfKey::WORKER_DELETE_STREAM_ALL);
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.stream_name());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(CheckConnection(namespaceUri), "worker check connection failed");
    LOG(INFO) << FormatString("Worker(%s) received DeleteStream request, namespaceUri: %s",
                              localWorkerAddress_.ToString(), namespaceUri);
    bool needsRollback = true;
    {
        StreamManagerMap::const_accessor accessor;
        if (GetStreamManager(namespaceUri, accessor).IsOk()) {  // If exists on local worker node
            std::shared_ptr<StreamManager> streamManager = accessor->second;
            RETURN_IF_NOT_OK_PRINT_ERROR_MSG(streamManager->CheckDeleteStreamCondition(),
                                             "streamManager check delete stream condition failed");
            // Set the state to DELETE_IN_PROGRESS to prevent a timing hole after
            // the master is updated but before the stream manager is removed from tbb.
            Status rc = streamManager->SetDeleteState();
            if (rc.IsError()) {
                // If delete is already in progress
                if (rc.GetCode() == StatusCode::K_SC_STREAM_DELETE_IN_PROGRESS) {
                    // Then this function is a no op
                    // No need to decrement because without ignore, RefCount does not increase
                    return Status::OK();
                }
                // No need to decrement because on error, RefCount does not increase
                return rc;  // Else return any other error
            }
        }
        accessor.release();
    }
    Raii unsetDelete([this, &needsRollback, &namespaceUri]() {
        StreamManagerMap::const_accessor accessor;
        if (needsRollback && GetStreamManager(namespaceUri, accessor).IsOk()) {
            std::shared_ptr<StreamManager> streamManager = accessor->second;
            LOG(INFO) << FormatString("[S:%s] Setting Active State", namespaceUri);
            streamManager->SetActiveState();
        }
    });
    INJECT_POINT("ClientWorkerSCServiceImpl.DELETE_IN_PROGRESS.sleep");
    // We call master to process broadcast to all other workers, except this node, that's related to this stream.
    Status rc = DeleteStreamHandleSend(namespaceUri);
    // Still proceed to handle DeleteStreamLocally even if K_NOT_FOUND is returned.
    INJECT_POINT("ClientWorkerSCServiceImpl.DeleteStreamHandleSend.sleep");
    RETURN_IF_NOT_OK_EXCEPT(rc, K_NOT_FOUND);
    // Now we need an exclusive accessor lock when we remove the stream from the tbb map
    LOG(INFO) << FormatString("[S:%s] Waiting for stream to be free of use", namespaceUri);
    INJECT_POINT("ClientWorkerSCServiceImpl.DeleteStreamLocally.sleep");
    bool success;
    {
        ReadLockHelper rlock(STREAM_COMMON_LOCK_ARGS(mutex_));
        StreamManagerMap::accessor accessor;
        success = streamMgrDict_.find(accessor, namespaceUri);
        if (success) {
            RETURN_IF_NOT_OK(DeleteStreamLocally(accessor));
        }
    }
    LOG_IF(INFO, !success) << FormatString("[S:%s] Stream manager is already gone", namespaceUri);
    LOG(INFO) << FormatString("[%s, S:%s] DeleteStream success.", LogPrefix(true), namespaceUri);
    needsRollback = false;
    return success ? Status::OK() : rc;
}

#endif

#ifdef DS_CLIENT_SC_RPC_GROUP_8
Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(QueryGlobalProducersNum,
    const QueryGlobalNumReqPb &req, QueryGlobalNumRsqPb &rsp)
{
    auto access = AccessRecorder::Stream(AccessRecorderKey::DS_POSIX_QUERY_PRODUCERS_NUM);
    auto rc = DS_CLIENT_SC_RPC_FORWARD(QueryGlobalProducersNumImpl, req, rsp);
    access.StreamName(req.stream_name()).ClientId(req.client_id()).Result(rc).Count(rsp.global_count()).Record();
    return rc;
}

Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(QueryGlobalProducersNumImpl,
    const QueryGlobalNumReqPb &req, QueryGlobalNumRsqPb &rsp)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.stream_name());
    LOG(INFO) << FormatString("Worker(%s) received QueryGlobalProducersNum request, namespaceUri: %s",
                              localWorkerAddress_.ToString(), namespaceUri);
    auto queryFn = [&] {
        std::shared_ptr<WorkerMasterSCApi> api;
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
            workerMasterApiManager_->GetWorkerMasterApi(namespaceUri, api),
            "Getting master api failed. stream name = " + namespaceUri);
        master::QueryGlobalNumReqPb masterReq;
        masterReq.set_stream_name(namespaceUri);
        masterReq.set_redirect(true);
        master::QueryGlobalNumRsqPb masterRsp;
        std::function<Status(master::QueryGlobalNumReqPb & req, master::QueryGlobalNumRsqPb & rsp)> func =
            [&api](master::QueryGlobalNumReqPb &req, master::QueryGlobalNumRsqPb &rsp) {
                return api->QueryGlobalProducersNum(req, rsp);
            };
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
            RedirectRetryWhenMetaMoving(masterReq, masterRsp, api, func),
            "workerMasterApi query global producers number failed on master" + api->Address());
        rsp.set_global_count(masterRsp.global_count());
        LOG(INFO) << "worker QueryGlobalProducersNum done, namespaceUri: " << namespaceUri;
        return Status::OK();
    };
    return WorkerMasterOcApiManager::RetryForReplicaNotReady(
        GetRequestContext()->scTimeoutDuration.CalcRealRemainingTime(), std::move(queryFn));
}

Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(QueryGlobalConsumersNum,
    const QueryGlobalNumReqPb &req, QueryGlobalNumRsqPb &rsp)
{
    auto access = AccessRecorder::Stream(AccessRecorderKey::DS_POSIX_QUERY_CONSUMERS_NUM);
    auto rc = DS_CLIENT_SC_RPC_FORWARD(QueryGlobalConsumersNumImpl, req, rsp);
    access.StreamName(req.stream_name()).ClientId(req.client_id()).Result(rc).Count(rsp.global_count()).Record();
    return rc;
}

Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(QueryGlobalConsumersNumImpl,
    const QueryGlobalNumReqPb &req, QueryGlobalNumRsqPb &rsp)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");
    std::string namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.stream_name());
    LOG(INFO) << FormatString("Worker(%s) received QueryGlobalConsumersNum request, namespaceUri: %s",
                              localWorkerAddress_.ToString(), namespaceUri);
    auto queryFn = [&] {
        std::shared_ptr<WorkerMasterSCApi> api;
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
            workerMasterApiManager_->GetWorkerMasterApi(namespaceUri, api),
            "Getting master api failed. stream name = " + namespaceUri);
        master::QueryGlobalNumReqPb masterReq;
        masterReq.set_stream_name(namespaceUri);
        masterReq.set_redirect(true);
        master::QueryGlobalNumRsqPb masterRsp;
        std::function<Status(master::QueryGlobalNumReqPb & req, master::QueryGlobalNumRsqPb & rsp)> func =
            [&api](master::QueryGlobalNumReqPb &req, master::QueryGlobalNumRsqPb &rsp) {
                return api->QueryGlobalConsumersNum(req, rsp);
            };
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
            RedirectRetryWhenMetaMoving(masterReq, masterRsp, api, func),
            "workerMasterApi query global consumers number failed on master " + api->Address());
        rsp.set_global_count(masterRsp.global_count());
        LOG(INFO) << "worker QueryGlobalConsumersNum done, namespaceUri: " << namespaceUri;
        return Status::OK();
    };

    return WorkerMasterOcApiManager::RetryForReplicaNotReady(
        GetRequestContext()->scTimeoutDuration.CalcRealRemainingTime(), std::move(queryFn));
}

#endif

#ifdef DS_CLIENT_SC_RPC_GROUP_9
Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(UnblockProducer,
    std::shared_ptr<ServerUnaryWriterReader<UnblockProducerRspPb, UnblockProducerReqPb>> serverApi)
{
    UnblockProducerReqPb req;
    RETURN_IF_NOT_OK(serverApi->Read(req));
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    const auto &streamName = req.stream_name();
    const auto &workerAddr = req.worker_addr();
    LOG(INFO) << "UnBlocking Producer for stream: " << streamName << " From remote worker: " << workerAddr;
    HostPort workerHostPort;
    RETURN_IF_NOT_OK(workerHostPort.ParseString(workerAddr));
    RETURN_IF_NOT_OK(remoteWorkerManager_->ToggleStreamBlocking(workerAddr, streamName, false));
    RETURN_IF_NOT_OK(serverApi->Write(UnblockProducerRspPb()));
    VLOG(SC_NORMAL_LOG_LEVEL) << "UnBlocking Producer Request for stream: " << streamName
                              << " From remote worker: " << workerAddr << " Successful";
    return Status::OK();
}

Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(BlockProducer,
    std::shared_ptr<ServerUnaryWriterReader<BlockProducerRspPb, BlockProducerReqPb>> serverApi)
{
    BlockProducerReqPb req;
    RETURN_IF_NOT_OK(serverApi->Read(req));
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_VERIFY(akSkManager_, req), "AK/SK failed.");
    const auto &streamName = req.stream_name();
    const auto &workerAddr = req.worker_addr();
    LOG(INFO) << "Blocking Producer for stream: " << streamName << " From remote worker: " << workerAddr;
    HostPort workerHostPort;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(workerHostPort.ParseString(workerAddr), "ParseString error");
    RETURN_IF_NOT_OK(remoteWorkerManager_->ToggleStreamBlocking(workerAddr, streamName, true));
    RETURN_IF_NOT_OK(serverApi->Write(BlockProducerRspPb()));
    VLOG(SC_NORMAL_LOG_LEVEL) << "Blocking Producer for stream: " << streamName << " From remote worker: " << workerAddr
                              << " Successful";
    return Status::OK();
}

#endif

#ifdef DS_CLIENT_SC_RPC_GROUP_10
Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(ResetStreams,
    std::shared_ptr<ServerUnaryWriterReader<ResetOrResumeStreamsRspPb, ResetOrResumeStreamsReqPb>> serverApi)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    ResetOrResumeStreamsReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");

    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");

    const auto &streamNamesRepeated = req.stream_names();
    std::unordered_set<std::string> streams{ streamNamesRepeated.begin(), streamNamesRepeated.end() };

    LOG(INFO) << "ResetStreams request start, clientId: " << req.client_id()
              << ", streams: " << VectorToString(streams);
    // Divide Streams into 2 Lists based on current Reset state
    std::unordered_set<std::string> doneList, errorList;

    // Start Reset Streams for all streams
    for (auto &stream : streams) {
        // Get Stream Manager for the stream
        auto namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, stream);
        LOG(INFO) << "Begin to clear data for stream Id: " << namespaceUri;
        StreamManagerMap::const_accessor accessor;
        Status rc = GetStreamManager(namespaceUri, accessor);
        if (rc.IsError()) {
            LOG(ERROR) << "Could not get stream manager for stream: " << namespaceUri;
            // If stream manager is not found, the stream is considered to be reset.
            doneList.insert(stream);
            continue;
        }
        std::shared_ptr<StreamManager> streamMgr = accessor->second;
        // If stream is getting deleted dont allow Reset on it
        rc = streamMgr->CheckIfStreamActive();
        if (rc.GetCode() == StatusCode::K_SC_STREAM_DELETE_IN_PROGRESS) {
            LOG(ERROR) << "Delete in progress for stream: " << namespaceUri;
            // If stream is getting deleted, the stream is considered to be reset.
            doneList.insert(stream);
            continue;
        }

        std::vector<std::string> prodConList;
        (void)GetPubSubForClientStream(req.client_id(), namespaceUri, prodConList);
        // Send Reset Start Request
        rc = streamMgr->ResetStreamStart(prodConList);
        if (rc.IsError()) {
            // Check if reset completed or is in progress by another client.
            if (streamMgr->CheckIfStreamInState(StreamState::RESET_COMPLETE)
                || streamMgr->CheckIfStreamInState(StreamState::DELETE_IN_PROGRESS)) {
                // Reset is done for the stream
                doneList.insert(stream);
            } else {
                LOG(ERROR) << rc.GetMsg();
                errorList.insert(stream);
            }
            continue;
        }
        // Reset Stream Start Successful
        doneList.insert(stream);
    }

    // We are done starting up Reset and Now we wait for Reset to be done
    RETURN_IF_NOT_OK(ResetStreamsReply(serverApi, streams.size(), doneList.size(), errorList.size()));
    LOG(INFO) << "ResetStreams request end";
    return Status::OK();
}

#endif

#ifdef DS_CLIENT_SC_RPC_GROUP_11
Status ClientWorkerSCServiceImpl::DS_CLIENT_SC_RPC_ENTRY(ResumeStreams,
    std::shared_ptr<ServerUnaryWriterReader<ResetOrResumeStreamsRspPb, ResetOrResumeStreamsReqPb>> serverApi)
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(ValidateWorkerState(), "validate worker state failed");
    ResetOrResumeStreamsReqPb req;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(req), "serverApi read request failed");

    std::string tenantId;
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_CLIENT_SC_RPC_TENANT(akSkManager_, req, tenantId, ClientKey::Intern(req.client_id())),
                                     "Authenticate failed.");

    const auto &streamNamesRepeated = req.stream_names();
    std::unordered_set<std::string> streams{ streamNamesRepeated.begin(), streamNamesRepeated.end() };
    LOG(INFO) << "ResumeStreams request start, clientId: " << req.client_id()
              << ", streams: " << VectorToString(streams);
    Status rc = Status::OK();
    for (const auto &stream : streams) {
        auto namespaceUri = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, stream);
        LOG(INFO) << "Begin to resume stream " << namespaceUri;
        StreamManagerMap::const_accessor accessor;
        // If StreamManager is not found, may be stream is deleted while reset was in progress. Log error, return OK.
        if (GetStreamManager(namespaceUri, accessor).IsOk()) {
            std::shared_ptr<StreamManager> streamMgr = accessor->second;
            Status status = streamMgr->ResumeStream();
            if (status.IsError()) {
                LOG(ERROR) << status.GetMsg();
                rc = status;
            }
        } else {
            LOG(ERROR) << "Could not get stream manager for stream: " << namespaceUri;
        }
    }
    ResetOrResumeStreamsRspPb rsp;
    CheckErrorReturn(rc, rsp, "Reset failed with rc ", serverApi);
    LOG(INFO) << "ResumeStreams request end";
    return Status::OK();
}

#endif
