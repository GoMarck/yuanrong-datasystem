/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
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

#include "datasystem/common/rdma/npu/remote_h2d_manager.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <sys/mman.h>
#include <tuple>
#include <vector>
#include <thread>
#include <unistd.h>

#include "re2/re2.h"

#include "datasystem/common/log/trace.h"
#include "datasystem/common/perf/perf_manager.h"
#include "datasystem/common/rpc/rpc_constants.h"
#include "datasystem/common/util/format.h"
#include "datasystem/common/util/hash_algorithm.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/common/util/uuid_generator.h"
#include "datasystem/object/buffer.h"
#include "datasystem/utils/status.h"

#include "datasystem/common/rdma/npu/rh2d_transport_strategy.h"
#include "datasystem/common/rdma/npu/roce_transport.h"
#ifdef ASCEND_HIXL_AVAILABLE
#include "datasystem/common/rdma/npu/hixl_transport.h"
#endif
#include "datasystem/common/ak_sk/hasher.h"

DS_DEFINE_string(remote_h2d_device_ids, "",
                 "The NPU device ids to be used for Remote H2D purposes given as a list of comma seperated values");

namespace datasystem {
RemoteH2DManager::ClientHeteroTransportConfig RemoteH2DManager::clientConfig_;
std::string RemoteH2DManager::hccsLocalEndpointIp_;
bool RemoteH2DManager::isClient_ = false;
std::mutex RemoteH2DManager::configMutex_;

namespace {
constexpr uint32_t K_RH2D_DEVICE_INFO_WARNING_LOG_EVERY_N = 100;

const char *InitStateToString(RemoteH2DContext::InitState state)
{
    switch (state) {
        case RemoteH2DContext::InitState::UNINITIALIZED:
            return "UNINITIALIZED";
        case RemoteH2DContext::InitState::INITIALIZING:
            return "INITIALIZING";
        case RemoteH2DContext::InitState::INITIALIZED:
            return "INITIALIZED";
        default:
            return "UNKNOWN";
    }
}

const char *P2pKindToString(P2pKind kind)
{
    switch (kind) {
        case P2P_SENDER:
            return "SENDER";
        case P2P_RECEIVER:
            return "RECEIVER";
        default:
            return "UNKNOWN";
    }
}

std::string CommKeyForLog(const std::string &key)
{
    return FormatString("len=%zu hash=%u", key.size(), MurmurHash3_32(key));
}

std::string DeviceIdsForLog(const std::vector<int32_t> &devIds)
{
    std::string result = "[";
    for (size_t i = 0; i < devIds.size(); ++i) {
        if (i > 0) {
            result += ",";
        }
        result += std::to_string(devIds[i]);
    }
    result += "]";
    return result;
}

std::string ClientDeviceInfoForLog(const RemoteH2DClientDeviceInfoPb *clientDeviceInfo)
{
    if (clientDeviceInfo == nullptr) {
        return "<unset>";
    }
    return FormatString("logic=%d physical=%d", clientDeviceInfo->logic_device_id(),
                        clientDeviceInfo->physical_device_id());
}

PerfKey Rh2dTransferBatchPerfKey(HixlTransferOp op)
{
    return op == HixlTransferOp::WRITE ? PerfKey::RH2D_MANAGER_TRANSFER_BATCH_WRITE
                                       : PerfKey::RH2D_MANAGER_TRANSFER_BATCH_READ;
}

PerfKey Rh2dCommWaitReadyPerfKey(HixlTransferOp op)
{
    return op == HixlTransferOp::WRITE ? PerfKey::RH2D_MANAGER_COMM_WAIT_READY_WRITE
                                       : PerfKey::RH2D_MANAGER_COMM_WAIT_READY_READ;
}

P2pLink LinkTypeFromFlag()
{
    return FLAGS_remote_h2d_link_type == "HCCS" ? P2P_LINK_HCCS : P2P_LINK_ROCE;
}

bool IsHccsFabricMemEnabled()
{
    return FLAGS_remote_h2d_link_type == "HCCS" && FLAGS_remote_h2d_hccs_enable_fabric_mem;
}

const char *P2pLinkName(P2pLink linkType)
{
    switch (linkType) {
        case P2P_LINK_HCCS:
            return "HCCS";
        case P2P_LINK_ROCE:
            return "ROCE";
        default:
            return "UNKNOWN";
    }
}

std::string ClientHeteroTransportConfigForLog(bool enableHeteroTransport, int32_t devId, const std::string &localIp)
{
    return FormatString("(enable=%d, devId=%d, localIp=%s)", enableHeteroTransport, devId, localIp.c_str());
}

Status SetCommandLineOptionFromEnv(const char *envName, const char *optionName)
{
    const char *envValue = std::getenv(envName);
    if (envValue == nullptr || strlen(envValue) == 0) {
        return Status::OK();
    }
    std::string errMsg;
    if (!SetCommandLineOption(optionName, envValue, errMsg)) {
        RETURN_STATUS_LOG_ERROR(K_INVALID, FormatString("Invalid %s: %s", envName, errMsg.c_str()));
    }
    LOG(INFO) << "[RH2D] " << optionName << " overridden by " << envName << "=" << envValue;
    return Status::OK();
}

Status ApplyClientHeteroTransportEnv(const std::string &localIp)
{
    RETURN_IF_NOT_OK(SetCommandLineOptionFromEnv("DS_RH2D_LINK_TYPE", "remote_h2d_link_type"));
    RETURN_OK_IF_TRUE(FLAGS_remote_h2d_link_type != "HCCS");
    RETURN_IF_NOT_OK(SetCommandLineOptionFromEnv("DS_HIXL_CS_ENABLE", "hixl_cs_enable"));
    RETURN_IF_NOT_OK(
        SetCommandLineOptionFromEnv("DS_RH2D_HCCS_ENABLE_FABRIC_MEM", "remote_h2d_hccs_enable_fabric_mem"));
    if (!FLAGS_remote_h2d_hccs_enable_fabric_mem) {
        RETURN_IF_NOT_OK(SetCommandLineOptionFromEnv("DS_RH2D_HCCS_BUFFER_POOL", "remote_h2d_hccs_buffer_pool"));
    }
    return RemoteH2DManager::SetRH2DLocalEndpointIp(localIp);
}
}  // namespace

HostSegment::HostSegment(std::byte *data, std::uint64_t dataSize, P2pSegmentInfo segmentInfo,
                         P2pSegmentPermissions permissions)
    : data{ data }, dataSize{ dataSize }, segmentInfo{ segmentInfo }, permissions{ permissions }
{
}

Status HostSegment::Create(std::shared_ptr<HostSegment> &result, std::byte *data, std::size_t dataSize,
                           P2pSegmentInfo segmentInfo, P2pSegmentPermissions permissions)
{
    result = std::make_shared<HostSegment>(data, dataSize, segmentInfo, permissions);
    return Status::OK();
}

RemoteH2DManager &RemoteH2DManager::Instance()
{
    static RemoteH2DManager manager;
    return manager;
}

bool RemoteH2DManager::IsSameClientHeteroTransportConfig(bool enableHeteroTransport, uint32_t devId,
                                                         const std::string &localIp)
{
    const bool sameTransportConfig = clientConfig_.enableHeteroTransport == enableHeteroTransport;
    const bool sameDeviceId = clientConfig_.devId == static_cast<int32_t>(devId);
    const bool sameLocalIp = FLAGS_remote_h2d_link_type != "HCCS" || clientConfig_.localIp == localIp;
    return sameTransportConfig && sameDeviceId && sameLocalIp;
}

Status RemoteH2DManager::CheckClientHeteroTransportConfig(bool enableHeteroTransport, uint32_t devId,
                                                          const std::string &localIp)
{
    if (clientConfig_.devId < 0 || IsSameClientHeteroTransportConfig(enableHeteroTransport, devId, localIp)) {
        return Status::OK();
    }
    auto currentConfig = ClientHeteroTransportConfigForLog(
        clientConfig_.enableHeteroTransport, clientConfig_.devId, clientConfig_.localIp);
    auto newConfig = ClientHeteroTransportConfigForLog(enableHeteroTransport, static_cast<int32_t>(devId), localIp);
    RETURN_STATUS_LOG_ERROR(K_RUNTIME_ERROR,
                            "Client hetero transport config cannot change after initialization, current="
                                + currentConfig + ", new=" + newConfig);
}

void RemoteH2DManager::CommitClientHeteroTransportConfig(bool enableHeteroTransport, uint32_t devId,
                                                         const std::string &localIp)
{
    LOG(INFO) << "CommitClientHeteroTransportConfig: enableHeteroTransport=" << enableHeteroTransport
              << ", devId=" << devId << ", localIp=" << localIp;
    clientConfig_.enableHeteroTransport = enableHeteroTransport;
    clientConfig_.devId = static_cast<int32_t>(devId);
    clientConfig_.localIp = FLAGS_remote_h2d_link_type == "HCCS" ? localIp : "";
}

Status RemoteH2DManager::UpdateClientHeteroTransportConfig(bool enableHeteroTransport, uint32_t devId,
                                                           const std::string &localIp)
{
    std::lock_guard<std::mutex> configLock(configMutex_);
    if (clientConfig_.devId >= 0) {
        RETURN_IF_NOT_OK(CheckClientHeteroTransportConfig(enableHeteroTransport, devId, localIp));
        if (IsRemoteH2DEnabled()) {
            return RemoteH2DManager::Instance().GetInitStatus();
        }
        return Status::OK();
    }

    // Note: The parameter needs to be consistent in the same client process.
    LOG(INFO) << "[RH2D] Client hetero transport config: enable=" << enableHeteroTransport << " devId=" << devId
              << " localIp=" << localIp;
    isClient_ = true;
    RETURN_IF_NOT_OK(ApplyClientHeteroTransportEnv(localIp));
    CommitClientHeteroTransportConfig(enableHeteroTransport, devId, localIp);
    if (IsRemoteH2DEnabled()) {
        auto &instance = RemoteH2DManager::Instance();
        instance.initStatus_ = instance.Init();
        RETURN_IF_NOT_OK(instance.GetInitStatus());
    }
    return Status::OK();
}

bool RemoteH2DManager::IsRemoteH2DEnabled()
{
    return clientConfig_.enableHeteroTransport || !FLAGS_remote_h2d_device_ids.empty();
}

Status RemoteH2DManager::SetDeviceIdx(std::optional<int32_t> specifiedDevId)
{
    VLOG(1) << "RemoteH2DManager::SetDeviceIdx enter, specified="
            << (specifiedDevId ? std::to_string(*specifiedDevId) : "<none>");
    RETURN_OK_IF_TRUE(!IsRemoteH2DEnabled());

    PerfPoint point(PerfKey::P2P_SET_DEV_IDX);
    int32_t devId;

    // Assume one process works with only one device index.
    if (!specifiedDevId || *specifiedDevId < 0) {
        RETURN_IF_NOT_OK(GetClientDeviceId(devId));
    } else {
        devId = *specifiedDevId;
    }

    uint32_t npuCount;
    RETURN_IF_NOT_OK(acl::AclDeviceManager::Instance()->aclrtGetDeviceCount(&npuCount));
    if (devId < 0 || devId >= static_cast<int32_t>(npuCount)) {
        RETURN_STATUS(StatusCode::K_INVALID,
                      "Invalid device id: " + std::to_string(devId) + ". Total NPU Count: " + std::to_string(npuCount));
    }

    RETURN_IF_NOT_OK(acl::AclDeviceManager::Instance()->SetDeviceIdx(devId));
    LOG(INFO) << "RemoteH2DManager::SetDeviceIdx set to " << devId;
    return Status::OK();
}

Status RemoteH2DManager::GetClientCommUuid(std::string &commId)
{
    RETURN_OK_IF_TRUE(!IsRemoteH2DEnabled());
    RETURN_IF_NOT_OK(initStatus_);
    // Assume client works with one device id, so only one uuid to identify the client process.
    commId = commId_;
    return Status::OK();
}

Status RemoteH2DManager::GetClientDeviceId(int32_t &devId)
{
    if (clientConfig_.devId >= 0) {
        devId = clientConfig_.devId;
        return Status::OK();
    }
    try {
        devId = stoi(FLAGS_remote_h2d_device_ids);
    } catch (const std::invalid_argument &e) {
        RETURN_STATUS(StatusCode::K_INVALID, "Invalid RemoteH2D dev ids set: " + FLAGS_remote_h2d_device_ids);
    } catch (const std::out_of_range &e) {
        RETURN_STATUS(StatusCode::K_INVALID, "RemoteH2D dev id out of range: " + FLAGS_remote_h2d_device_ids);
    }
    return Status::OK();
}

Status RemoteH2DManager::GetClientRemoteH2DDeviceInfo(RemoteH2DClientDeviceInfoPb &deviceInfo)
{
    int32_t logicDeviceId = -1;
    RETURN_IF_NOT_OK(GetClientDeviceId(logicDeviceId));
    int32_t physicalDeviceId = logicDeviceId;
    RETURN_IF_NOT_OK(ResolvePhysicalDeviceId(logicDeviceId, physicalDeviceId));
    deviceInfo.set_logic_device_id(logicDeviceId);
    deviceInfo.set_physical_device_id(physicalDeviceId);
    return Status::OK();
}

Status RemoteH2DManager::GetWorkerDeviceIds(std::vector<int32_t> *devIds)
{
    // Parse gflag for worker device ids if empty
    if (workerDeviceIds_.empty()) {
        std::string devIdStr = FLAGS_remote_h2d_device_ids;
        RE2 pattern("^\\d+(,\\s*\\d+)*$");
        if (!RE2::FullMatch(devIdStr, pattern)) {
            RETURN_STATUS(K_INVALID,
                          "remote_h2d_device_ids flag has invalid form: " + devIdStr + ". Must use form \"1, 2, 3\"");
        }

        size_t start = 0;
        uint32_t npuCount;
        RETURN_IF_NOT_OK(acl::AclDeviceManager::Instance()->aclrtGetDeviceCount(&npuCount));

        while (start < devIdStr.size()) {
            size_t end = devIdStr.find(',', start);
            int32_t devId = -1;
            try {
                devId = std::stoi(devIdStr.substr(start, end - start));
            } catch (const std::invalid_argument &e) {
                RETURN_STATUS(K_INVALID, "remote_h2d_device_ids flag has invalid form: " + devIdStr
                                             + ". Must use form \"1, 2, 3\"");
            } catch (const std::out_of_range &e) {
                RETURN_STATUS(K_INVALID, "remote_h2d_device_ids flag contains out of range number: " + devIdStr);
            }
            if (devId < 0 || devId >= int32_t(npuCount)) {
                RETURN_STATUS(K_INVALID, "remote_h2d_device_ids flag contains out of range id: "
                                             + std::to_string(devId) + ". Total NPU count: "
                                             + std::to_string(npuCount));
            }
            workerDeviceIds_.push_back(devId);
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        if (IsHccsFabricMemEnabled() && workerDeviceIds_.size() > 1) {
            LOG(INFO) << "[RH2D] HCCS FabricMem uses a single HIXL engine; effective engine device id is "
                      << workerDeviceIds_.front() << ". Additional configured device ids are used only for "
                      << "FabricMem arena placement: "
                      << DeviceIdsForLog(std::vector<int32_t>(workerDeviceIds_.begin() + 1,
                                                               workerDeviceIds_.end()));
            workerDeviceIds_.resize(1);
        }
    }
    if (devIds != nullptr) {
        *devIds = workerDeviceIds_;
    }
    return Status::OK();
}

Status RemoteH2DManager::ResolvePhysicalDeviceId(int32_t logicDeviceId, int32_t &physicalDeviceId)
{
    physicalDeviceId = logicDeviceId;
    Status rc = acl::AclDeviceManager::Instance()->GetPhysicalDeviceId(logicDeviceId, physicalDeviceId);
    if (rc.IsError()) {
        physicalDeviceId = logicDeviceId;
        LOG_EVERY_N(WARNING, K_RH2D_DEVICE_INFO_WARNING_LOG_EVERY_N)
            << "[RH2D] Failed to resolve physical device id for logic devId=" << logicDeviceId
            << ", use logic devId as comparable device id. Detail: " << rc.ToString();
    }
    return Status::OK();
}

Status RemoteH2DManager::GetWorkerPhysicalDeviceId(int32_t logicDeviceId, int32_t &physicalDeviceId)
{
    auto iter = workerPhysicalDeviceIds_.find(logicDeviceId);
    if (iter != workerPhysicalDeviceIds_.end()) {
        physicalDeviceId = iter->second;
        return Status::OK();
    }
    LOG_EVERY_N(WARNING, K_RH2D_DEVICE_INFO_WARNING_LOG_EVERY_N)
        << "[RH2D] Worker physical device id cache miss for logic devId=" << logicDeviceId
        << ", resolve it without updating runtime cache.";
    RETURN_IF_NOT_OK(ResolvePhysicalDeviceId(logicDeviceId, physicalDeviceId));
    return Status::OK();
}

Status RemoteH2DManager::SelectWorkerDeviceId(const std::vector<int32_t> &devIds,
                                              const RemoteH2DClientDeviceInfoPb *clientDeviceInfo,
                                              bool avoidClientDeviceForSameNode, int32_t &devId)
{
    CHECK_FAIL_RETURN_STATUS(!devIds.empty(), K_INVALID, "RemoteH2D worker device ids are empty.");
    const bool allowSameDeviceConnection = transport_ != nullptr && transport_->AllowSameDeviceConnection();
    const bool avoidClientDevice = avoidClientDeviceForSameNode && !allowSameDeviceConnection
                                   && clientDeviceInfo != nullptr && clientDeviceInfo->physical_device_id() >= 0;
    std::vector<int32_t> candidates;
    candidates.reserve(devIds.size());
    if (avoidClientDevice) {
        for (auto workerDevId : devIds) {
            int32_t workerPhysicalDeviceId = workerDevId;
            RETURN_IF_NOT_OK(GetWorkerPhysicalDeviceId(workerDevId, workerPhysicalDeviceId));
            if (workerPhysicalDeviceId == clientDeviceInfo->physical_device_id()) {
                continue;
            }
            candidates.emplace_back(workerDevId);
        }
    } else {
        candidates = devIds;
    }
    if (candidates.empty()) {
        RETURN_STATUS(K_RUNTIME_ERROR,
                      FormatString("[RH2D] No worker HIXL device can avoid the client device. clientDevice=%s, "
                                   "workerDeviceIds=%s",
                                   ClientDeviceInfoForLog(clientDeviceInfo), DeviceIdsForLog(devIds)));
    }
    devId = candidates[nextDevIdIndex_.fetch_add(1, std::memory_order_relaxed) % candidates.size()];
    int32_t selectedPhysicalDeviceId = devId;
    RETURN_IF_NOT_OK(GetWorkerPhysicalDeviceId(devId, selectedPhysicalDeviceId));
    LOG(INFO) << "[RH2D] Select worker HIXL device devId=" << devId
              << " physicalDevId=" << selectedPhysicalDeviceId
              << " clientDevice=" << ClientDeviceInfoForLog(clientDeviceInfo)
              << " avoidClientDeviceForSameNode=" << avoidClientDeviceForSameNode
              << " avoidClientDevice=" << avoidClientDevice
              << " allowSameDeviceConnection=" << allowSameDeviceConnection
              << " candidates=" << DeviceIdsForLog(candidates);
    return Status::OK();
}

void RemoteH2DManager::FillRootInfoDeviceFields(int32_t devId, RemoteH2DRootInfoPb *p2pRootInfo)
{
    if (p2pRootInfo == nullptr || devId < 0) {
        return;
    }
    int32_t physicalDeviceId = devId;
    LOG_IF_ERROR(GetWorkerPhysicalDeviceId(devId, physicalDeviceId),
                 "Failed to fill RemoteH2D root info worker physical device id");
    p2pRootInfo->set_worker_logic_device_id(devId);
    p2pRootInfo->set_worker_physical_device_id(physicalDeviceId);
}

Status RemoteH2DManager::P2PGetRootInfo(const std::string &key, RemoteH2DRootInfoPb *p2pRootInfo,
                                        const RemoteH2DClientDeviceInfoPb *clientDeviceInfo,
                                        bool avoidClientDeviceForSameNode)
{
    RETURN_OK_IF_TRUE(!IsRemoteH2DEnabled());
    RETURN_IF_NOT_OK(initStatus_);

    PerfPoint point(PerfKey::P2P_GET_ROOT_INFO);
    std::shared_lock<std::shared_timed_mutex> l(communicatorMutex_);

    CommunicatorMap::ConstAccessor constAcc;
    if (communicatorMap_->Find(constAcc, key)) {
        p2pRootInfo->set_internal(constAcc.entry->data->localIdentity);
        FillRootInfoDeviceFields(constAcc.entry->data->devId, p2pRootInfo);
        return Status::OK();
    }

    CommunicatorMap::Accessor accessor;
    if (!communicatorMap_->Insert(accessor, key)) {
        // Insert() already returns an accessor locked on the existing entry when another thread won the race.
        // Re-finding the same key here would try to lock the same entry mutex again in the same thread.
        p2pRootInfo->set_internal(accessor.entry->data->localIdentity);
        FillRootInfoDeviceFields(accessor.entry->data->devId, p2pRootInfo);
        return Status::OK();
    }

    LOG(INFO) << "RemoteH2DManager::P2PGetRootInfo create new context, key(" << CommKeyForLog(key) << ")";
    bool needRollback = true;
    Raii eraseRaii([this, &needRollback, &accessor]() {
        if (needRollback) {
            communicatorMap_->BlockingErase(accessor);
        }
    });

    auto ctx = std::make_shared<RemoteH2DContext>();
    std::string identity;
    int32_t devId = -1;
    RETURN_IF_NOT_OK(GetDevIdForComm(key, devId, clientDeviceInfo, avoidClientDeviceForSameNode));
    {
        PerfPoint identityPoint(PerfKey::RH2D_MANAGER_GET_CONNECTION_IDENTITY);
        RETURN_IF_NOT_OK(transport_->GetConnectionIdentity(&identity, devId));
    }
    ctx->localIdentity = identity;
    ctx->devId = devId;
    accessor.entry->data = ctx;
    needRollback = false;

    p2pRootInfo->set_internal(ctx->localIdentity);
    FillRootInfoDeviceFields(devId, p2pRootInfo);
    return Status::OK();
}

Status RemoteH2DManager::CompleteConnectionInit(const std::string &key, P2pKind kind,
                                                std::shared_ptr<RemoteH2DContext> &p2pComm, int32_t devId)
{
    std::shared_lock<std::shared_timed_mutex> l(communicatorMutex_);
    CommunicatorMap::Accessor accessor;
    CHECK_FAIL_RETURN_STATUS(communicatorMap_->Find(accessor, key), K_NOT_FOUND,
                             "Communicator context not found for key");
    p2pComm = accessor.entry->data;
    auto state = p2pComm->initialized.load(std::memory_order_acquire);
    if (state == RemoteH2DContext::InitState::INITIALIZED) {
        LOG_EVERY_N(INFO, 200) << "RemoteH2D communicator already initialized before async run, key("
                               << CommKeyForLog(key) << "), kind=" << P2pKindToString(kind) << ", devId=" << devId;
        return Status::OK();
    }
    CHECK_FAIL_RETURN_STATUS(
        state == RemoteH2DContext::InitState::INITIALIZING, K_RUNTIME_ERROR,
        FormatString("Communicator context has unexpected init state: %s", InitStateToString(state)));

    bool needRollback = true;
    bool needDisconnect = false;
    std::string remoteIdentity;
    Status rollbackStatus = Status(K_RUNTIME_ERROR, "RemoteH2D communicator init failed");
    Raii eraseRaii([this, &needRollback, &needDisconnect, &remoteIdentity, &key, &p2pComm, &rollbackStatus, kind,
                    devId]() {
        if (needRollback) {
            auto failedComm = p2pComm;
            LOG(WARNING) << "RemoteH2D comm init rollback, key(" << CommKeyForLog(key)
                         << "), kind=" << P2pKindToString(kind) << ", devId=" << devId;
            if (needDisconnect && transport_ != nullptr) {
                LOG_IF_ERROR(transport_->Disconnect(remoteIdentity), "RemoteH2D transport disconnect rollback failed");
            }
            {
                std::lock_guard<std::mutex> hbLock(heartbeatMutex_);
                clientPingFunctions_.erase(key);
                clientDisconnectChecks_.erase(key);
            }
            CommunicatorMap::Accessor accessor;
            if (communicatorMap_->Find(accessor, key)) {
                communicatorMap_->BlockingErase(accessor);
            }
            if (failedComm != nullptr) {
                failedComm->initialized.store(RemoteH2DContext::InitState::UNINITIALIZED, std::memory_order_release);
                failedComm->waitPost.SetWithStatus(rollbackStatus);
            }
            p2pComm = nullptr;
        }
    });
    LOG(INFO) << "RemoteH2D start communicator init, key(" << CommKeyForLog(key) << "), kind=" << P2pKindToString(kind)
              << ", devId=" << devId;
    remoteIdentity = p2pComm->remoteEndpoint;
    accessor.Release();
    auto p2pCallback = std::make_shared<std::function<int()>>();
    Status rc = transport_->Connect(remoteIdentity, kind, p2pCallback.get());
    if (rc.IsError()) {
        rollbackStatus = Status(K_RUNTIME_ERROR, FormatString("Failed to initialize communicator: %s", rc.ToString()));
        return rollbackStatus;
    }
    needDisconnect = true;
    p2pComm->linkType = transport_->LinkType();
    if (p2pComm->linkType == P2P_LINK_ROCE) {
        std::lock_guard<std::mutex> hbLock(heartbeatMutex_);
        if (kind == P2P_RECEIVER) {
            clientPingFunctions_.insert({ key, p2pCallback });
        } else if (kind == P2P_SENDER) {
            clientDisconnectChecks_.insert({ key, p2pCallback });
        }
    }

    if (transport_->LinkType() == P2P_LINK_ROCE) {
        p2pComm->stream = std::make_shared<aclrtStream>();
        rc = acl::AclDeviceManager::Instance()->RtCreateStream(p2pComm->stream.get());
        if (rc.IsError()) {
            rollbackStatus = rc;
            return rc;
        }
    }

    if (devId == -1) {
        rc = GetClientDeviceId(p2pComm->devId);
        if (rc.IsError()) {
            rollbackStatus = rc;
            return rc;
        }
    } else {
        p2pComm->devId = devId;
    }

    p2pComm->initialized.store(RemoteH2DContext::InitState::INITIALIZED, std::memory_order_release);
    p2pComm->waitPost.Set();
    needRollback = false;
    LOG(INFO) << "RemoteH2D communicator init done, key(" << CommKeyForLog(key) << "), kind=" << P2pKindToString(kind)
              << ", devId=" << p2pComm->devId;
    return Status::OK();
}

void RemoteH2DManager::SubmitConnectionInitTask(const std::string &key, P2pKind kind, int32_t devId,
                                                const std::shared_ptr<ThreadPool> &threadPool)
{
    auto traceId = Trace::Instance().GetTraceID();
    LOG(INFO) << "RemoteH2D enqueue async comm init, key(" << CommKeyForLog(key) << "), kind=" << P2pKindToString(kind)
              << ", devId=" << devId;
    threadPool->Execute([this, traceId, key, kind, devId]() {
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID(traceId);
        LOG_EVERY_N(INFO, 200) << "RemoteH2D async comm init worker start, key(" << CommKeyForLog(key)
                               << "), kind=" << P2pKindToString(kind) << ", devId=" << devId;
        LOG_IF_ERROR(SetDeviceIdx(devId), "SetDeviceId failed.");
        std::shared_ptr<RemoteH2DContext> p2pComm;
        auto rc = CompleteConnectionInit(key, kind, p2pComm, devId);
        if (rc.IsError() && p2pComm != nullptr) {
            p2pComm->waitPost.SetWithStatus(rc);
        }
        LOG_IF_ERROR(rc, "Create p2p communicator connection failed in thread");
    });
}

Status RemoteH2DManager::P2PCommInitRootInfo(const std::string &key, const RemoteH2DRootInfoPb &p2pRootInfo,
                                             P2pKind kind, std::shared_ptr<RemoteH2DContext> &p2pComm, int32_t devId,
                                             std::shared_ptr<ThreadPool> threadPool)
{
    RETURN_OK_IF_TRUE(!IsRemoteH2DEnabled());
    RETURN_IF_NOT_OK(initStatus_);

    PerfPoint point(PerfKey::P2P_COMM_INIT_ROOT);

    std::shared_lock<std::shared_timed_mutex> l(communicatorMutex_);
    CommunicatorMap::Accessor accessor;
    bool inserted = communicatorMap_->Insert(accessor, key);
    if (inserted) {
        accessor.entry->data = std::make_shared<RemoteH2DContext>();
        LOG(INFO) << "RemoteH2D comm reserve new context before init, key(" << CommKeyForLog(key)
                  << "), kind=" << P2pKindToString(kind) << ", devId=" << devId;
    }
    p2pComm = accessor.entry->data;
    auto state = p2pComm->initialized.load();
    if (state != RemoteH2DContext::InitState::UNINITIALIZED) {
        LOG_EVERY_N(INFO, 500) << "RemoteH2D comm init cache hit, key(" << CommKeyForLog(key) << "), state="
                               << InitStateToString(state) << ", kind=" << P2pKindToString(kind)
                               << ", devId=" << devId;
        return Status::OK();
    }

    p2pComm->remoteEndpoint = p2pRootInfo.internal();
    CHECK_FAIL_RETURN_STATUS(!p2pComm->remoteEndpoint.empty(), K_INVALID, "RemoteH2D remote endpoint is empty");
    p2pComm->initialized.store(RemoteH2DContext::InitState::INITIALIZING, std::memory_order_release);
    p2pComm->waitPost.Clear();
    accessor.Release();

    INJECT_POINT("RemoteH2DManager.P2PCommInitRootInfo.delay");
    if (threadPool) {
        l.unlock();
        SubmitConnectionInitTask(key, kind, devId, threadPool);
    } else {
        LOG(INFO) << "RemoteH2D do sync comm init, key(" << CommKeyForLog(key) << "), kind=" << P2pKindToString(kind)
                  << ", devId=" << devId;
        RETURN_IF_NOT_OK(CompleteConnectionInit(key, kind, p2pComm, devId));
    }

    // Note: 2 threads cannot launch ops on the same comm at the same time, so client side is currently not thread safe.
    // multiple client processes should be fine, as they use different communicator connections.
    // Mutex is added before launching operation.
    return Status::OK();
}

Status RemoteH2DManager::RegisterHostMemoryOnDevice(int32_t devId, void *data, uint64_t dataSize)
{
    RETURN_IF_NOT_OK(SetDeviceIdx(devId));

    uint64_t castedData = reinterpret_cast<uint64_t>(data);
    std::shared_lock<std::shared_timed_mutex> l(segmentMutex_);
    HostSegmentMap::Accessor accessor;
    bool insertedDevEntry = false;
    if (!hostSegmentMap_->Find(accessor, devId)) {
        (void)hostSegmentMap_->Insert(accessor, devId);
        insertedDevEntry = true;
    }

    RETURN_OK_IF_TRUE(accessor.entry->data.find(castedData) != accessor.entry->data.end());
    bool needRollback = insertedDevEntry;
    Raii eraseRaii([this, &needRollback, &accessor]() {
        if (needRollback) {
            hostSegmentMap_->BlockingErase(accessor);
        }
    });
    P2pSegmentInfo segmentInfo;
    P2pSegmentPermissions permissions = P2pSegmentPermissions::P2P_SEGMENT_READ_WRITE;
    RETURN_IF_NOT_OK(transport_->RegisterMemory(data, dataSize, &segmentInfo));

    std::shared_ptr<HostSegment> hostSegment;
    HostSegment::Create(hostSegment, reinterpret_cast<std::byte *>(data), dataSize, segmentInfo, permissions);
    accessor.entry->data[castedData] = hostSegment;
    needRollback = false;
    return Status::OK();
}

Status RemoteH2DManager::RegisterHostMemory(void *data, uint64_t dataSize)
{
    RETURN_OK_IF_TRUE(!IsRemoteH2DEnabled());
    RETURN_IF_NOT_OK(initStatus_);
    LOG(INFO) << "RemoteH2DManager::RegisterHostMemory addr=" << data << " size=" << dataSize
              << " link_type=" << FLAGS_remote_h2d_link_type;

    // mlock is needed only for RoCE: DSP2PRegisterHostMem DMAs from these pages so they must be resident.
    // HCCS buffer-pool mode uses a relay. HCCS FabricMem uses a physical-backed arena directly when available.
    if (FLAGS_remote_h2d_link_type != "HCCS") {
        CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(mlock(data, dataSize) == 0, K_RUNTIME_ERROR,
                                             "Failed to lock physical pages.");
        LOG(INFO) << "RemoteH2DManager::RegisterHostMemory mlock done";
    } else {
        LOG(INFO) << "RemoteH2DManager::RegisterHostMemory skipping mlock for HCCS link type";
    }

    RETURN_IF_NOT_OK(GetWorkerDeviceIds());

    std::vector<std::future<Status>> futures;
    auto pool = std::make_unique<ThreadPool>(workerDeviceIds_.size(), workerDeviceIds_.size(), "", false, 10 * 1000);

    // Register Host Memory on the effective worker device set.
    for (const int32_t &devId : workerDeviceIds_) {
        futures.emplace_back(pool->Submit([this, devId, data, dataSize] {
            return RegisterHostMemoryOnDevice(devId, data, dataSize);
        }));
    }
    Status firstError = Status::OK();
    for (auto &future : futures) {
        Status status = future.get();
        if (status.IsError()) {
            LOG(ERROR) << "RegisterHostMemory failed: " << status.ToString();
            if (firstError.IsOk()) {
                firstError = status;
            }
        }
    }
    RETURN_IF_NOT_OK(firstError);
    return Status::OK();
}

Status RemoteH2DManager::PreRegisterDeviceMemory(const std::vector<void *> &data, const std::vector<uint64_t> &dataSize)
{
    PerfPoint point(PerfKey::RH2D_MANAGER_PRE_REGISTER_DEVICE_MEMORY);
    CHECK_FAIL_RETURN_STATUS(IsRemoteH2DEnabled(), K_RUNTIME_ERROR,
                             "RemoteH2D is not enabled for device memory pre-registration.");
    CHECK_FAIL_RETURN_STATUS(FLAGS_remote_h2d_link_type == "HCCS", K_RUNTIME_ERROR,
                             "RemoteH2D device memory pre-registration is only supported for HCCS.");
    RETURN_IF_NOT_OK(initStatus_);
    CHECK_FAIL_RETURN_STATUS(!data.empty(), K_INVALID, "Device memory address list cannot be empty.");
    CHECK_FAIL_RETURN_STATUS(data.size() == dataSize.size(), K_INVALID,
                             FormatString("Device memory address count %zu does not match size count %zu.",
                                          data.size(), dataSize.size()));
    for (size_t i = 0; i < data.size(); ++i) {
        CHECK_FAIL_RETURN_STATUS(data[i] != nullptr, K_INVALID,
                                 FormatString("Device memory address cannot be null, index: %zu.", i));
        CHECK_FAIL_RETURN_STATUS(dataSize[i] > 0, K_INVALID,
                                 FormatString("Device memory size must be greater than 0, index: %zu.", i));
    }

    RETURN_IF_NOT_OK(transport_->PreRegisterDeviceMemory(data, dataSize));
    return Status::OK();
}

Status RemoteH2DManager::UnregisterDeviceMemory(const std::vector<void *> &data)
{
    CHECK_FAIL_RETURN_STATUS(IsRemoteH2DEnabled(), K_RUNTIME_ERROR,
                             "RemoteH2D is not enabled for device memory unregistration.");
    CHECK_FAIL_RETURN_STATUS(FLAGS_remote_h2d_link_type == "HCCS", K_RUNTIME_ERROR,
                             "RemoteH2D device memory unregistration is only supported for HCCS.");
    RETURN_IF_NOT_OK(initStatus_);
    for (size_t i = 0; i < data.size(); ++i) {
        CHECK_FAIL_RETURN_STATUS(data[i] != nullptr, K_INVALID,
                                 FormatString("Device memory address cannot be null, index: %zu.", i));
    }

    RETURN_IF_NOT_OK(transport_->UnregisterDeviceMemory(data));
    return Status::OK();
}

Status RemoteH2DManager::UnregisterHostMemory(void *data)
{
    RETURN_OK_IF_TRUE(!IsRemoteH2DEnabled() || data == nullptr || hostSegmentMap_ == nullptr);
    RETURN_IF_NOT_OK(GetWorkerDeviceIds());

    const bool unregisterFabricOnce = IsHccsFabricMemEnabled();
    if (unregisterFabricOnce) {
        RETURN_IF_NOT_OK(transport_->UnregisterMemory(data));
    }

    Status firstError = Status::OK();
    for (const int32_t devId : workerDeviceIds_) {
        if (!unregisterFabricOnce) {
            Status rc = SetDeviceIdx(devId);
            if (rc.IsError()) {
                firstError = firstError.IsOk() ? rc : firstError;
                continue;
            }
        }

        std::shared_lock<std::shared_timed_mutex> l(segmentMutex_);
        HostSegmentMap::Accessor accessor;
        if (!hostSegmentMap_->Find(accessor, devId)) {
            continue;
        }
        auto segmentIt = accessor.entry->data.find(reinterpret_cast<uint64_t>(data));
        if (segmentIt == accessor.entry->data.end()) {
            continue;
        }
        if (!unregisterFabricOnce) {
            Status rc = transport_->UnregisterMemory(data);
            if (rc.IsError()) {
                firstError = firstError.IsOk() ? rc : firstError;
                continue;
            }
        }
        accessor.entry->data.erase(segmentIt);
        if (accessor.entry->data.empty()) {
            hostSegmentMap_->BlockingErase(accessor);
        }
    }
    return firstError;
}

Status RemoteH2DManager::FillSegmentInfo(uint64_t segLen, uint64_t segDataOffset, uint64_t dataOffset,
                                         uint64_t dataSize, uint64_t key, RemoteHostSegmentPb &segmentPb, int32_t devId)
{
    PerfPoint point(PerfKey::P2P_FILL_SEG_INFO);

    HostSegmentMap::Accessor accessor;
    if (hostSegmentMap_->Find(accessor, devId)) {
        auto segmentIt = accessor.entry->data.find(key);
        CHECK_FAIL_RETURN_STATUS(segmentIt != accessor.entry->data.end() && segmentIt->second != nullptr,
                                 StatusCode::K_NOT_FOUND,
                                 FormatString("Cannot find segment address %zu for devId %d", key, devId));
        auto &segInfo = segmentIt->second->segmentInfo;
        CHECK_FAIL_RETURN_STATUS(segDataOffset <= segLen && dataOffset <= segLen - segDataOffset
                                     && dataSize <= segLen - segDataOffset - dataOffset,
                                 K_INVALID, "Remote H2D data range exceeds registered host segment");
        uint64_t remoteAddr = 0;
        RETURN_IF_NOT_OK(transport_->PrepareRemoteMemory(reinterpret_cast<void *>(key), segDataOffset + dataOffset,
                                                         dataSize, &remoteAddr));
        segmentPb.set_name(std::string(std::begin(segInfo.internal), std::end(segInfo.internal)));
        segmentPb.set_seg_va(remoteAddr);
        segmentPb.set_seg_len(segLen);
        segmentPb.set_seg_data_offset(segDataOffset);
        return Status::OK();
    }
    return Status(K_NOT_FOUND, FormatString("Cannot find segment for devId %d at address %zu", devId, key));
}

Status RemoteH2DManager::FillDataInfo(uint64_t *dataPtr, RemoteH2DDataInfoPb &dataInfoPb)
{
    PerfPoint point(PerfKey::P2P_FILL_DATA_INFO);

    uint64_t sz = *dataPtr;

    // Get the first offset
    dataPtr++;
    uint64_t firstOffset = *dataPtr;
    dataInfoPb.set_offset(firstOffset);

    // Calculate sizes of data
    for (uint64_t i = 0; i < sz; i++) {
        uint64_t offsetX = *dataPtr;
        dataPtr++;
        uint64_t offsetY = *dataPtr;
        dataInfoPb.add_sizes(offsetY - offsetX);
    }
    return Status::OK();
}

void RemoteH2DManager::EnsureStateMaps()
{
    if (communicatorMap_ == nullptr) {
        communicatorMap_ = std::make_unique<CommunicatorMap>();
    }
    if (hostSegmentMap_ == nullptr) {
        hostSegmentMap_ = std::make_unique<HostSegmentMap>();
    }
    if (remoteSegmentMap_ == nullptr) {
        remoteSegmentMap_ = std::make_unique<RemoteSegmentMap>();
    }
}

Status RemoteH2DManager::FillDataInfo(const std::vector<uint64_t> &blobSizes, RemoteH2DDataInfoPb &dataInfoPb)
{
    PerfPoint point(PerfKey::P2P_FILL_DATA_INFO);
    CHECK_FAIL_RETURN_STATUS(!blobSizes.empty(), K_INVALID, "HIXL payload-only blob metadata is empty");

    dataInfoPb.set_offset(0);
    dataInfoPb.mutable_sizes()->Add(blobSizes.begin(), blobSizes.end());
    return Status::OK();
}

Status RemoteH2DManager::InitAclRuntime()
{
    Status rc = acl::AclDeviceManager::Instance()->aclInit(nullptr);
    if (rc.IsError()) {
        LOG(INFO) << "RemoteH2DManager acl init failed. Repeated init with error code 100002 is acceptable. "
                  << rc.ToString();
    }
    aclInitialized_ = true;
    return Status::OK();
}

Status RemoteH2DManager::PrepareTransportDeviceIds(std::vector<int32_t> &workerDevIdsForInit)
{
    if (!isClient_) {
        // Parse worker device IDs before initializing the transport, so that HCCS can create one HIXL engine per
        // device for multi-device load balancing.
        RETURN_IF_NOT_OK(GetWorkerDeviceIds(&workerDevIdsForInit));
        workerPhysicalDeviceIds_.clear();
        for (auto devId : workerDevIdsForInit) {
            int32_t physicalDeviceId = devId;
            RETURN_IF_NOT_OK(ResolvePhysicalDeviceId(devId, physicalDeviceId));
            workerPhysicalDeviceIds_[devId] = physicalDeviceId;
            LOG(INFO) << "[RH2D] Worker HIXL device mapping logicDevId=" << devId
                      << " physicalDevId=" << physicalDeviceId;
        }
        return Status::OK();
    }
    Status rc = SetDeviceIdx();
    LOG_IF_ERROR(rc, FormatString("RemoteH2DManager set device index failed. %s", rc.ToString()));
    return rc;
}

Status RemoteH2DManager::StartHeartbeatThread()
{
    try {
        heartbeatThread_ = std::thread(&RemoteH2DManager::ManageHeartbeats, this);
    } catch (const std::system_error &error) {
        RETURN_STATUS(K_RUNTIME_ERROR, "Failed to start RemoteH2D heartbeat thread: " + std::string(error.what()));
    }
    return Status::OK();
}

Status RemoteH2DManager::Init()
{
    std::lock_guard<std::mutex> lock(initMutex_);
    RETURN_OK_IF_TRUE(!IsRemoteH2DEnabled());
    RETURN_OK_IF_TRUE(initialized_);
    LOG(INFO) << "RemoteH2DManager::Init enter, enabled=" << IsRemoteH2DEnabled()
              << " device_ids=" << FLAGS_remote_h2d_device_ids
              << " link_type=" << FLAGS_remote_h2d_link_type << " role=" << (isClient_ ? "client" : "worker")
              << " hccsLocalIp=" << hccsLocalEndpointIp_
              << " hccsBufferPool=" << FLAGS_remote_h2d_hccs_buffer_pool;

    RETURN_IF_NOT_OK(EnsureTransportForCurrentConfig());
    RETURN_RUNTIME_ERROR_IF_NULL(transport_);
    EnsureStateMaps();
    interrupted_.store(false);

    bool initCommitted = false;
    Raii rollback([this, &initCommitted]() {
        if (initCommitted) {
            return;
        }
        if (transport_ != nullptr) {
            LOG_IF_ERROR(transport_->DisconnectAll(), "Rollback RemoteH2D transport initialization failed");
        }
        if (aclInitialized_) {
            LOG_IF_ERROR(acl::AclDeviceManager::Instance()->aclFinalize(),
                         "Rollback RemoteH2D ACL initialization failed");
            aclInitialized_ = false;
        }
    });

    RETURN_IF_NOT_OK(InitAclRuntime());
    std::vector<int32_t> workerDevIdsForInit;
    RETURN_IF_NOT_OK(PrepareTransportDeviceIds(workerDevIdsForInit));

    LOG(INFO) << "RemoteH2DManager::Init calling transport_->Init() with " << workerDevIdsForInit.size() << " devId(s)";
    RETURN_IF_NOT_OK(transport_->Init(workerDevIdsForInit));
    RETURN_IF_NOT_OK(StartHeartbeatThread());
    initCommitted = true;
    initialized_ = true;
    LOG(INFO) << "RemoteH2DManager::Init done, transport=" << P2pLinkName(transport_->LinkType())
              << " role=" << (isClient_ ? "client" : "worker");
    return Status::OK();
}

Status RemoteH2DManager::GetInitStatus() const
{
    return initStatus_;
}

Status RemoteH2DManager::GetConnectionIdentityForComm(const std::string &commId, std::string &identity)
{
#ifdef ASCEND_HIXL_AVAILABLE
    if (!isClient_ && transport_->LinkType() == P2P_LINK_HCCS) {
        int32_t devId = -1;
        RETURN_IF_NOT_OK(GetDevIdForComm(commId, devId));
        return static_cast<HixlTransport *>(transport_.get())->GetDeviceEndpoint(devId, identity);
    }
#else
    (void)commId;
#endif
    return transport_->GetConnectionIdentity(&identity);
}

Status RemoteH2DManager::GetHixlWriteEndpoint(const std::string &commId, std::string &endpoint)
{
    RETURN_IF_NOT_OK(initStatus_);
#ifdef ASCEND_HIXL_AVAILABLE
    CHECK_FAIL_RETURN_STATUS(!isClient_ && IsRemoteH2DEnabled() && transport_ != nullptr, K_NOT_READY,
                             "Worker RH2D transport is not initialized");
    CHECK_FAIL_RETURN_STATUS(transport_->LinkType() == P2P_LINK_HCCS, K_NOT_SUPPORTED,
                             "D2RH requires the HIXL transport");
    auto *hixl = static_cast<HixlTransport *>(transport_.get());
    CHECK_FAIL_RETURN_STATUS(hixl->UsesBufferPool(), K_NOT_SUPPORTED,
                             "D2RH currently requires the HIXL buffer pool");
    return GetConnectionIdentityForComm(commId, endpoint);
#else
    (void)commId;
    (void)endpoint;
    RETURN_STATUS(K_NOT_SUPPORTED, "D2RH requires a build with HIXL support");
#endif
}

Status RemoteH2DManager::GetClientHixlTransport(int32_t deviceId, std::shared_ptr<HixlTransport> &transport)
{
    RETURN_IF_NOT_OK(initStatus_);
#ifdef ASCEND_HIXL_AVAILABLE
    CHECK_FAIL_RETURN_STATUS(isClient_ && clientConfig_.enableHeteroTransport && !interrupted_.load()
                                 && transport_ != nullptr,
                             K_NOT_READY,
                             "Client RH2D transport is not initialized");
    CHECK_FAIL_RETURN_STATUS(clientConfig_.devId == deviceId && transport_->LinkType() == P2P_LINK_HCCS,
                             K_NOT_SUPPORTED, "Shared D2RH requires the same RH2D HIXL device");
    auto hixl = std::static_pointer_cast<HixlTransport>(transport_);
    RETURN_IF_NOT_OK(hixl->CheckReady());
    CHECK_FAIL_RETURN_STATUS(hixl->UsesBufferPool(), K_NOT_SUPPORTED, "D2RH requires the HIXL buffer pool");
    transport = std::move(hixl);
    return Status::OK();
#else
    (void)deviceId;
    (void)transport;
    RETURN_STATUS(K_NOT_SUPPORTED, "D2RH requires a build with HIXL support");
#endif
}

Status RemoteH2DManager::Uninit()
{
    std::lock_guard<std::mutex> lock(initMutex_);
    interrupted_.store(true);
    interruptCv_.notify_all();
    if (heartbeatThread_.joinable()) {
        heartbeatThread_.join();
    }

    if (aclInitialized_) {
        LOG_IF_ERROR(SetDeviceIdx(), "Set device index failed at RemoteH2DManager uninitialization");
    }

    CHECK_FAIL_RETURN_STATUS(transport_.use_count() <= 1, K_NOT_READY,
                             "Shared RH2D transport is still in use by a D2RH client");

    {
        std::lock_guard<std::mutex> lock(heartbeatMutex_);
        clientDisconnectChecks_.clear();
        clientPingFunctions_.clear();
    }

    {
        std::lock_guard<std::shared_timed_mutex> l(segmentMutex_);
        hostSegmentMap_.reset();
        remoteSegmentMap_.reset();
    }

    {
        std::lock_guard<std::shared_timed_mutex> l(communicatorMutex_);
        communicatorMap_.reset();
    }

    commDevIdMap_.clear();
    workerPhysicalDeviceIds_.clear();

    if (transport_) {
        RETURN_IF_NOT_OK(transport_->DisconnectAll());
    }

    if (aclInitialized_) {
        RETURN_IF_NOT_OK(acl::AclDeviceManager::Instance()->aclFinalize());
        aclInitialized_ = false;
    }
    initialized_ = false;
    return Status::OK();
}

Status RemoteH2DManager::ShutdownTransport()
{
    RETURN_OK_IF_TRUE(!IsRemoteH2DEnabled() || transport_ == nullptr);
    return transport_->DisconnectAll();
}

Status RemoteH2DManager::ImportHostSegment(const std::string &remoteEndpoint, const RemoteHostSegmentPb &seg)
{
    RETURN_OK_IF_TRUE(!IsRemoteH2DEnabled());
    RETURN_IF_NOT_OK(initStatus_);

    PerfPoint point(PerfKey::P2P_IMPORT_HOST_SEG);

    std::shared_lock<std::shared_timed_mutex> l(segmentMutex_);
    const std::string &segInternal = seg.name();
    RemoteSegmentMap::Accessor accessor;
    if (!remoteSegmentMap_->Find(accessor, segInternal)) {
        if (remoteSegmentMap_->Insert(accessor, segInternal)) {
            bool needRollback = true;
            Raii eraseRaii([this, &needRollback, &accessor]() {
                if (needRollback) {
                    remoteSegmentMap_->BlockingErase(accessor);
                }
            });
            RETURN_IF_NOT_OK(transport_->ImportRemoteAddressInfo(remoteEndpoint, seg));
            // Only for record purposes so that remote segment is not imported repeatedly,
            // the actual segment content does not need to be kept.
            accessor.entry->data = nullptr;
            needRollback = false;
        }
    }
    return Status::OK();
}

Status RemoteH2DManager::ScatterBatch(P2pScatterEntry *entries, uint32_t size,
                                      std::shared_ptr<RemoteH2DContext> p2pComm)
{
    RETURN_OK_IF_TRUE(!IsRemoteH2DEnabled());
    RETURN_IF_NOT_OK(initStatus_);

    PerfPoint point(PerfKey::P2P_SCATTER_BATCH);
    PerfPoint readPoint(PerfKey::RH2D_MANAGER_SCATTER_BATCH_READ);

    // Thread safety needs to be ensured for the p2p communicator.
    PerfPoint lockPoint(PerfKey::RH2D_MANAGER_COMM_LOCK_WAIT_READ);
    std::lock_guard<std::mutex> lock(p2pComm->mutex);
    lockPoint.Record();
    PerfPoint waitPoint(PerfKey::P2P_COMM_INIT_WAIT_READY);
    PerfPoint readWaitPoint(PerfKey::RH2D_MANAGER_COMM_WAIT_READY_READ);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(p2pComm->waitPost.WaitAndGetStatus(),
                                     "[RH2D][ScatterBatch][Manager] waitPost failed");
    readWaitPoint.Record();
    waitPoint.Record();

    PerfPoint transportPoint(PerfKey::P2P_TRANSPORT_SCATTER_BATCH);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(transport_->ScatterBatch(entries, size, p2pComm->remoteEndpoint, p2pComm->stream),
                                     "[RH2D][ScatterBatch][Manager] transport ScatterBatch failed");
    return Status::OK();
}

Status RemoteH2DManager::TransferBatch(HixlTransferOp op, const std::vector<HixlTransferDesc> &descs,
                                       std::shared_ptr<RemoteH2DContext> p2pComm)
{
    RETURN_OK_IF_TRUE(!IsRemoteH2DEnabled());
    RETURN_IF_NOT_OK(initStatus_);
    RETURN_RUNTIME_ERROR_IF_NULL(p2pComm);
    RETURN_OK_IF_TRUE(descs.empty());

    PerfPoint point(PerfKey::P2P_SCATTER_BATCH);
    PerfPoint directionPoint(Rh2dTransferBatchPerfKey(op));

    PerfPoint lockPoint(op == HixlTransferOp::WRITE ? PerfKey::RH2D_MANAGER_COMM_LOCK_WAIT_WRITE
                                                   : PerfKey::RH2D_MANAGER_COMM_LOCK_WAIT_READ);
    std::lock_guard<std::mutex> lock(p2pComm->mutex);
    lockPoint.Record();
    PerfPoint waitPoint(PerfKey::P2P_COMM_INIT_WAIT_READY);
    PerfPoint directionWaitPoint(Rh2dCommWaitReadyPerfKey(op));
    RETURN_IF_NOT_OK(p2pComm->waitPost.WaitAndGetStatus());
    directionWaitPoint.Record();
    waitPoint.Record();

    {
        PerfPoint transferPoint(PerfKey::RH2D_MANAGER_TRANSPORT_SCATTER_BATCH);
        RETURN_IF_NOT_OK(transport_->TransferBatch(op, descs, p2pComm->remoteEndpoint, p2pComm->stream));
    }
    return Status::OK();
}

Status RemoteH2DManager::GetDevIdForComm(const std::string &commId, int32_t &devId,
                                         const RemoteH2DClientDeviceInfoPb *clientDeviceInfo,
                                         bool avoidClientDeviceForSameNode)
{
    RETURN_IF_NOT_OK(initStatus_);
    if (transport_ != nullptr && transport_->LinkType() == P2P_LINK_HCCS) {
        CHECK_FAIL_RETURN_STATUS(!commId.empty(), K_INVALID, "HIXL client communicator ID is empty");
        RETURN_IF_NOT_OK(GetWorkerDeviceIds());
        CHECK_FAIL_RETURN_STATUS(!workerDeviceIds_.empty(), K_NOT_READY, "Worker device list is empty");
        // One SDK engine must retain the same Worker engine for both READ and WRITE IPC imports.
        devId = workerDeviceIds_[MurmurHash3_32(commId) % workerDeviceIds_.size()];
        return Status::OK();
    }
    devId = -1;
    {
        // fast path: use const accessor
        tbb::concurrent_hash_map<std::string, int32_t>::const_accessor cacc;
        if (commDevIdMap_.find(cacc, commId)) {
            devId = cacc->second;
        }
    }
    if (devId == -1) {
        std::vector<int32_t> devIds;
        RETURN_IF_NOT_OK(GetWorkerDeviceIds(&devIds));
        int32_t selectedDevId = -1;
        RETURN_IF_NOT_OK(SelectWorkerDeviceId(devIds, clientDeviceInfo, avoidClientDeviceForSameNode, selectedDevId));

        tbb::concurrent_hash_map<std::string, int32_t>::accessor acc;
        if (commDevIdMap_.insert(acc, commId)) {
            acc->second = selectedDevId;
        }
        devId = acc->second;
    }
    return Status::OK();
}

void RemoteH2DChildAfterFork()
{
    RemoteH2DManager::Instance().AfterFork();
}

void RemoteH2DManager::AfterFork()
{
    LOG(ERROR) << "Note: fork happens with RemoteH2DManager initialized";
    initStatus_ = Uninit();
    if (initStatus_.IsError()) {
        LOG(ERROR) << "Uninitialize acl for Remote H2D at fork failed: " << initStatus_;
        return;
    }
    initStatus_ = Init();
    LOG_IF_ERROR(initStatus_, "Initialize acl for Remote H2D at fork failed.");
}

Status RemoteH2DManager::SetRH2DLocalEndpointIp(const std::string &localIp)
{
    if (FLAGS_remote_h2d_link_type == "HCCS") {
        if (localIp.empty()) {
            RETURN_STATUS_LOG_ERROR(K_RUNTIME_ERROR, "SetRH2DLocalEndpointIp: local IP is empty.");
        }
        hccsLocalEndpointIp_ = localIp;
        LOG(INFO) << "[HCCS] Local endpoint IP configured: " << hccsLocalEndpointIp_;
    }
    return Status::OK();
}

std::unique_ptr<RH2DTransportStrategy> RemoteH2DManager::CreateTransport()
{
    const std::string &type = FLAGS_remote_h2d_link_type;
    LOG(INFO) << "RemoteH2DManager::CreateTransport link_type=" << type
              << " role=" << (isClient_ ? "client" : "worker") << " hccsLocalIp=" << hccsLocalEndpointIp_;
    if (type == "ROCE") {
        return std::make_unique<RoCETransport>();
    }
    if (type == "HCCS") {
#ifdef ASCEND_HIXL_AVAILABLE
        auto transport = std::make_unique<HixlTransport>();
        if (!hccsLocalEndpointIp_.empty()) {
            transport->SetLocalEndpoint(hccsLocalEndpointIp_, isClient_);
        }
        return transport;
#else
        LOG(FATAL) << "remote_h2d_link_type=HCCS requested but datasystem was built without HIXL (cann_hixl) support";
        return nullptr;
#endif
    }
    LOG(FATAL) << "Unknown remote_h2d_link_type: " << type;
    return nullptr;
}

Status RemoteH2DManager::EnsureTransportForCurrentConfig()
{
    const P2pLink expectedLinkType = LinkTypeFromFlag();
    if (transport_ == nullptr) {
        transport_ = CreateTransport();
        RETURN_RUNTIME_ERROR_IF_NULL(transport_);
        return Status::OK();
    }
    if (transport_->LinkType() == expectedLinkType) {
#ifdef ASCEND_HIXL_AVAILABLE
        if (!initialized_ && expectedLinkType == P2P_LINK_HCCS) {
            auto *hixlTransport = dynamic_cast<HixlTransport *>(transport_.get());
            RETURN_RUNTIME_ERROR_IF_NULL(hixlTransport);
            hixlTransport->SetLocalEndpoint(hccsLocalEndpointIp_, isClient_);
        }
#endif
        return Status::OK();
    }
    CHECK_FAIL_RETURN_STATUS(!initialized_, K_RUNTIME_ERROR,
                             FormatString("RemoteH2D transport is already initialized as %s, cannot switch to %s",
                                          P2pLinkName(transport_->LinkType()), P2pLinkName(expectedLinkType)));
    LOG(WARNING) << "RemoteH2DManager recreate transport before initialization, old="
                 << P2pLinkName(transport_->LinkType()) << " new=" << P2pLinkName(expectedLinkType);
    transport_ = CreateTransport();
    RETURN_RUNTIME_ERROR_IF_NULL(transport_);
    return Status::OK();
}

void RemoteH2DManager::ManageHeartbeats()
{
    INJECT_POINT("RH2D.ManageHeartbeats.heartbeat_interval_s",
                 [this](int32_t interval) { this->heartbeatIntervalS_ = interval; });
    INJECT_POINT("RH2D.ManageHeartbeats.heartbeat_timeout_s",
                 [this](int32_t timeout) { this->heartbeatTimeoutS_ = timeout; });
    while (!interrupted_.load()) {
        {
            std::unique_lock<std::mutex> lock(interruptMutex_);
            interruptCv_.wait_for(lock, std::chrono::seconds(heartbeatIntervalS_),
                                  [this] { return interrupted_.load(); });
        }
        if (interrupted_.load()) {
            break;
        }
        TraceGuard traceGuard = Trace::Instance().SetTraceNewID("H2DHeartbeat;" + GetStringUuid());
        DispatchPings();
        std::vector<std::string> disconnected = CheckDisconnectedClients();
        CleanupDisconnectedClients(disconnected);
    }
}

void RemoteH2DManager::DispatchPings()
{
    std::vector<std::shared_ptr<std::function<int()>>> pingFuncs;
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex_);
        for (const auto &kv : clientPingFunctions_) {
            if (kv.second) {
                pingFuncs.push_back(kv.second);
            }
        }
    }
    for (auto &func : pingFuncs) {
        if (func) {
            (*func)();
        }
    }
}

std::vector<std::string> RemoteH2DManager::CheckDisconnectedClients()
{
    std::vector<std::pair<std::string, std::shared_ptr<std::function<int()>>>> disconnectChecks;
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex_);
        for (const auto &kv : clientDisconnectChecks_) {
            if (kv.second) {
                disconnectChecks.emplace_back(kv.first, kv.second);
            }
        }
    }
    std::vector<std::string> disconnected;
    for (const auto &kv : disconnectChecks) {
        if (kv.second && ((*(kv.second))() > heartbeatTimeoutS_)) {
            disconnected.push_back(kv.first);
        }
    }
    return disconnected;
}

void RemoteH2DManager::CleanupDisconnectedClients(const std::vector<std::string> &disconnected)
{
    if (disconnected.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> hbLock(heartbeatMutex_);
        for (const std::string &commId : disconnected) {
            clientDisconnectChecks_.erase(commId);
        }
    }
    for (const std::string &commId : disconnected) {
        tbb::concurrent_hash_map<std::string, int32_t>::accessor acc;
        if (commDevIdMap_.find(acc, commId)) {
            commDevIdMap_.erase(acc);
        }
        CommunicatorMap::Accessor accessor;
        if (communicatorMap_->Find(accessor, commId)) {
            std::string remoteEndpoint = accessor.entry->data->remoteEndpoint;
            communicatorMap_->BlockingErase(accessor);
            if (transport_) {
                LOG_IF_ERROR(transport_->Disconnect(remoteEndpoint), "Transport disconnect failed for evicted peer");
            }
        }
    }
}

RemoteH2DManager::RemoteH2DManager()
{
    LOG(INFO) << "RemoteH2DManager::Constructor start, link_type=" << FLAGS_remote_h2d_link_type;
    int rc = pthread_atfork(nullptr, nullptr, &RemoteH2DChildAfterFork);
    if (rc != 0) {
        LOG(WARNING) << "RemoteH2DManager: Constructor failure pthread_atfork.";
    }
    communicatorMap_ = std::make_unique<CommunicatorMap>();
    hostSegmentMap_ = std::make_unique<HostSegmentMap>();
    remoteSegmentMap_ = std::make_unique<RemoteSegmentMap>();
    commId_ = GetStringUuid();
    LOG(INFO) << "RemoteH2DManager::Constructor maps allocated, commId=" << commId_ << ", calling Init()";
    initStatus_ = Init();
    LOG_IF_ERROR(initStatus_, "Initialize acl for Remote H2D failed.");
    LOG(INFO) << "RemoteH2DManager::Constructor done";
}

RemoteH2DManager::~RemoteH2DManager()
{
    LOG_IF_ERROR(Uninit(), "Uninitialize acl for Remote H2D failed.");
}

RemoteH2DContext::~RemoteH2DContext()
{
    if (stream) {
        LOG_IF_ERROR(acl::AclDeviceManager::Instance()->SetDeviceIdx(devId),
                     "Failed to set device id to " + std::to_string(devId));
        LOG_IF_ERROR(acl::AclDeviceManager::Instance()->RtDestroyStream(*stream), "Destroy stream failed.");
    }
}

}  // namespace datasystem
