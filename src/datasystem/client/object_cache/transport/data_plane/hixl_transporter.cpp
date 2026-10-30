/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */
#include "datasystem/client/object_cache/transport/data_plane/hixl_transporter.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "datasystem/common/flags/flags.h"
#include "datasystem/common/log/access_recorder.h"
#include "datasystem/common/object_cache/buffer_composer.h"
#include "datasystem/common/rpc/api_deadline.h"
#include "datasystem/common/rpc/timeout_duration.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/common/util/uuid_generator.h"
#ifdef ASCEND_HIXL_AVAILABLE
#include "datasystem/common/rdma/npu/hixl_transport.h"
#include "datasystem/common/device/ascend/acl_device_manager.h"
#endif

namespace datasystem {
namespace client {
HixlTransporter::~HixlTransporter()
{
    Shutdown();
}

void HixlTransporter::Shutdown()
{
    std::lock_guard<std::timed_mutex> lock(mutex_);
    failed_ = true;
#ifdef ASCEND_HIXL_AVAILABLE
    if (transport_ != nullptr && transferFailed_) {
        // A failed transfer has no verified DMA fence. Retain registrations until process exit.
        auto *retained = new std::shared_ptr<HixlTransport>(std::move(transport_));
        (void)retained;
        LOG(ERROR) << "D2RH transfer failed; retain the registered arena and restart this client process";
    }
    if (transport_ != nullptr) {
        LOG_IF_ERROR(transport_->DisconnectAll(), "Failed to close D2RH transport");
        transport_.reset();
    }
#endif
    ranges_.clear();
}

Status HixlTransporter::RegisterDeviceMemory(int32_t deviceId, const std::string &localIp,
                                             const std::vector<void *> &addresses,
                                             const std::vector<uint64_t> &sizes)
{
    std::lock_guard<std::timed_mutex> lock(mutex_);
    CHECK_FAIL_RETURN_STATUS(!failed_, K_NOT_READY, "D2RH transport has stopped");
#ifdef ASCEND_HIXL_AVAILABLE
    CHECK_FAIL_RETURN_STATUS(addresses.size() == sizes.size() && !addresses.empty(), K_INVALID,
                             "Invalid D2RH registration ranges");
    CHECK_FAIL_RETURN_STATUS(transport_ == nullptr, K_NOT_SUPPORTED,
                             "Register the fixed D2RH arena once before writing");
    HixlTransport::Options options;
    const char *bufferPool = std::getenv("DS_RH2D_HCCS_BUFFER_POOL");
    if (bufferPool != nullptr && bufferPool[0] != '\0') {
        options.bufferPool = bufferPool;
    }
    options.csEnable = GetBoolFromEnv("DS_HIXL_CS_ENABLE", false);
    auto transport = std::make_shared<HixlTransport>();
    const char *endpointIp = std::getenv("DS_HIXL_D2RH_LOCAL_IP");
    transport->SetLocalEndpoint(endpointIp != nullptr && endpointIp[0] != '\0' ? endpointIp : localIp, true);
    RETURN_IF_NOT_OK(transport->Init({ deviceId }, options));
    CHECK_FAIL_RETURN_STATUS(transport->UsesBufferPool(), K_NOT_SUPPORTED,
                             "D2RH currently requires the HIXL buffer pool");
    RETURN_IF_NOT_OK(transport->PreRegisterDeviceMemory(addresses, sizes));
    for (size_t i = 0; i < addresses.size(); ++i) {
        ranges_.emplace_back(reinterpret_cast<uintptr_t>(addresses[i]), sizes[i]);
    }
    deviceId_ = deviceId;
    transport_ = std::move(transport);
    return Status::OK();
#else
    (void)deviceId; (void)localIp; (void)addresses; (void)sizes;
    RETURN_STATUS(K_NOT_SUPPORTED, "D2RH requires a build with HIXL support");
#endif
}

Status HixlTransporter::ValidateSources(const std::vector<const DeviceBlobList *> &blobs) const
{
    CHECK_FAIL_RETURN_STATUS(!failed_ && transport_ != nullptr, K_NOT_READY,
                             "Pre-register the D2RH device arena before writing");
    for (const auto *item : blobs) {
        CHECK_FAIL_RETURN_STATUS(item != nullptr && item->deviceIdx == deviceId_, K_INVALID,
                                 "D2RH source device differs from the registered device");
        for (const auto &blob : item->blobs) {
            const auto address = reinterpret_cast<uintptr_t>(blob.pointer);
            const bool covered = std::any_of(ranges_.begin(), ranges_.end(), [&](const auto &range) {
                return address >= range.first && address - range.first <= range.second
                    && blob.size > 0 && blob.size <= range.second - (address - range.first);
            });
            CHECK_FAIL_RETURN_STATUS(covered, K_INVALID, "D2RH source lies outside the registered arena");
        }
    }
    return Status::OK();
}

#ifdef ASCEND_HIXL_AVAILABLE
namespace {
Status BuildHixlCreate(const std::vector<std::string> &keys,
                       const std::vector<const DeviceBlobList *> &blobs, TransportCreateParam &param,
                       MultiCreateReqPb &request, std::vector<ShmKey> &allocations)
{
    constexpr uint32_t defaultAlignment = 64;
    std::vector<uint64_t> sizes;
    sizes.reserve(keys.size());
    param.allocationIds.clear();
    for (const auto *item : blobs) {
        std::vector<uint64_t> blobSizes;
        for (const auto &blob : item->blobs) { blobSizes.push_back(blob.size); }
        std::vector<uint64_t> header;
        uint64_t size = 1;
        if (!blobSizes.empty()) {
            RETURN_IF_NOT_OK(object_cache::BuildComposedBufferHeader(blobSizes, defaultAlignment, header, size));
        }
        sizes.push_back(size);
        param.allocationIds.push_back(GetStringUuid());
        allocations.emplace_back(ShmKey::Intern(param.allocationIds.back()));
    }
    RETURN_IF_NOT_OK(BuildMultiCreateRequest(keys, sizes, param, request));
    request.set_skip_check_existence(false);
    for (const auto *item : blobs) {
        auto *layout = request.add_hixl_layouts();
        for (const auto &blob : item->blobs) { layout->add_blob_sizes(blob.size); }
    }
    return Status::OK();
}

Status BuildHixlWrites(const std::vector<std::string> &keys,
                       const std::vector<const DeviceBlobList *> &blobs, const MultiCreateRspPb &response,
                       std::vector<DsHixlTransferDesc> &writes, MultiPublishReqPb &publish)
{
    CHECK_FAIL_RETURN_STATUS(response.results_size() == static_cast<int>(keys.size())
                                 && response.exists_size() == static_cast<int>(keys.size()), K_RUNTIME_ERROR,
                             "D2RH allocation response count mismatch");
    CHECK_FAIL_RETURN_STATUS(!response.hixl_endpoint().empty(), K_NOT_SUPPORTED,
                             "Target worker does not support HIXL D2RH");
    for (size_t i = 0; i < keys.size(); ++i) {
        if (response.exists(static_cast<int>(i))) { continue; }
        const auto &item = response.results(static_cast<int>(i));
        const auto &target = item.hixl_target();
        CHECK_FAIL_RETURN_STATUS(item.has_hixl_target() && !item.shm_id().empty()
                                     && target.address() != 0 && target.header_size() > 0
                                     && target.header_size() <= target.size()
                                     && target.address() <= UINT64_MAX - target.size(), K_RUNTIME_ERROR,
                                 "Invalid D2RH target range");
        auto *info = publish.add_object_info();
        info->set_object_key(keys[i]);
        info->set_shm_id(item.shm_id());
        info->set_data_size(target.size());
        uint64_t offset = target.header_size();
        for (const auto &blob : blobs[i]->blobs) {
            CHECK_FAIL_RETURN_STATUS(blob.size > 0 && blob.size <= target.size() - offset, K_INVALID,
                                     "D2RH blob exceeds target allocation");
            writes.push_back({ reinterpret_cast<uintptr_t>(blob.pointer), target.address() + offset, blob.size });
            info->add_blob_sizes(blob.size);
            offset += blob.size;
        }
        CHECK_FAIL_RETURN_STATUS(offset == target.size(), K_INVALID, "D2RH target size mismatch");
    }
    return Status::OK();
}
}  // namespace
#endif

Status HixlTransporter::MSetDevice(const std::shared_ptr<WorkerRpcClient> &rpc,
                                   const std::vector<std::string> &keys,
                                   const std::vector<const DeviceBlobList *> &blobs, TransportCreateParam createParam,
                                   const TransportSetParam &setParam, TransportMSetResult &result,
                                   std::vector<std::string> *confirmedKeys)
{
    result.Clear();
    auto remaining = TimeoutDuration::CeilUsToMs(ApiDeadline::Instance().ApiRemainingUs());
    CHECK_FAIL_RETURN_STATUS(remaining > 0, K_RPC_DEADLINE_EXCEEDED, "D2RH deadline expired");
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    CHECK_FAIL_RETURN_STATUS(lock.try_lock_for(std::chrono::milliseconds(remaining)), K_RPC_DEADLINE_EXCEEDED,
                             "D2RH deadline expired waiting for the transport");
    CHECK_FAIL_RETURN_STATUS(ApiDeadline::Instance().ApiRemainingUs() > 0, K_RPC_DEADLINE_EXCEEDED,
                             "D2RH deadline expired waiting for the transport");
    result.actualKind = AccessTransportKind::HIXL;
    CHECK_FAIL_RETURN_STATUS(keys.size() == blobs.size() && !keys.empty(), K_INVALID, "Invalid D2RH batch");
    RETURN_IF_NOT_OK(ValidateSources(blobs));
#ifdef ASCEND_HIXL_AVAILABLE
    RETURN_IF_NOT_OK(acl::AclDeviceManager::Instance()->SetDeviceIdx(deviceId_));
    MultiCreateReqPb request;
    std::vector<ShmKey> allocations;
    RETURN_IF_NOT_OK(BuildHixlCreate(keys, blobs, createParam, request, allocations));
    Raii release([&]() {
        constexpr int64_t releaseTimeoutMs = 1000;
        ApiDeadlineGuard releaseDeadline(releaseTimeoutMs);
        LOG_IF_ERROR(rpc->InvokeDecreaseReferences(createParam.requestContext, allocations),
                     "Failed to release D2RH worker references");
    });
    MultiCreateRspPb response;
    uint32_t version = 0;
    RETURN_IF_NOT_OK(rpc->InvokeMultiCreate(createParam.subTimeoutMs, request, response, version));
    for (const auto &item : response.results()) {
        if (!item.shm_id().empty()) {
            auto id = ShmKey::Intern(item.shm_id());
            if (std::find(allocations.begin(), allocations.end(), id) == allocations.end()) { allocations.push_back(id); }
        }
    }
    MultiPublishReqPb publish;
    std::vector<DsHixlTransferDesc> writes;
    RETURN_IF_NOT_OK(BuildHixlWrites(keys, blobs, response, writes, publish));
    if (writes.empty()) { return Status::OK(); }
    remaining = TimeoutDuration::CeilUsToMs(ApiDeadline::Instance().ApiRemainingUs());
    CHECK_FAIL_RETURN_STATUS(remaining > 0, K_RPC_DEADLINE_EXCEEDED, "D2RH deadline expired before connect");
    RETURN_IF_NOT_OK(transport_->Connect(response.hixl_endpoint(),
                                         static_cast<int32_t>(std::min<int64_t>(remaining, INT32_MAX))));
    remaining = TimeoutDuration::CeilUsToMs(ApiDeadline::Instance().ApiRemainingUs());
    CHECK_FAIL_RETURN_STATUS(remaining > 0, K_RPC_DEADLINE_EXCEEDED, "D2RH deadline expired before write");
    auto rc = transport_->WriteBatch(response.hixl_endpoint(), writes,
                                    static_cast<int32_t>(std::min<int64_t>(remaining, INT32_MAX)));
    if (rc.IsError()) {
        failed_ = true;
        transferFailed_ = true;
        return rc;
    }
    publish.set_client_id(setParam.requestContext.clientId);
    publish.set_token(setParam.requestContext.token);
    publish.set_tenant_id(setParam.requestContext.tenantId);
    publish.set_write_mode(static_cast<uint32_t>(createParam.writeMode));
    publish.set_consistency_type(static_cast<uint32_t>(createParam.consistencyType));
    publish.set_cache_type(static_cast<uint32_t>(createParam.cacheType));
    publish.set_existence(static_cast<uint32_t>(setParam.existence));
    publish.set_ttl_second(setParam.ttlSecond);
    publish.set_is_routed(true);
    publish.set_hixl_write_complete(true);
    publish.set_return_local_published_indexes(confirmedKeys != nullptr);
    MultiPublishRspPb published;
    result.publishAttempted = true;
    rc = rpc->InvokeMultiSet(setParam.subTimeoutMs, publish, {}, published, version);
    AccessTransportTracker::Record(AccessTransportKind::HIXL);
    if (rc.IsError()) { return rc; }
    result.failedKeys.assign(published.failed_object_keys().begin(), published.failed_object_keys().end());
    result.lastRc = Status(static_cast<StatusCode>(published.last_rc().error_code()), published.last_rc().error_msg());
    if (confirmedKeys != nullptr) {
        for (auto index : published.local_published_indexes()) {
            CHECK_FAIL_RETURN_STATUS(index < static_cast<uint32_t>(publish.object_info_size()), K_RUNTIME_ERROR,
                                     "Invalid D2RH publish result index");
            confirmedKeys->push_back(publish.object_info(static_cast<int>(index)).object_key());
        }
    }
    RETURN_IF_NOT_OK(result.lastRc);
    CHECK_FAIL_RETURN_STATUS(result.failedKeys.empty(), K_RUNTIME_ERROR, "Some D2RH objects failed to publish");
    return Status::OK();
#else
    (void)rpc; (void)createParam; (void)setParam; (void)confirmedKeys;
    RETURN_STATUS(K_NOT_SUPPORTED, "D2RH requires a build with HIXL support");
#endif
}
}  // namespace client
}  // namespace datasystem
