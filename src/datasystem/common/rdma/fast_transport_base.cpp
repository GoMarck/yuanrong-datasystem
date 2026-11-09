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

#include "datasystem/utils/status.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "datasystem/common/flags/common_flags.h"
#include "datasystem/common/log/log.h"
#include "datasystem/common/util/format.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/common/rdma/fast_transport_base.h"

#ifdef USE_URMA
#include "datasystem/common/rdma/urma_manager.h"
#endif
#ifdef USE_RDMA
#include "datasystem/common/rdma/ucp_manager.h"
#endif
#ifdef USE_NPU
#include "datasystem/common/rdma/npu/remote_h2d_manager.h"
#endif
#ifdef ASCEND_HIXL_AVAILABLE
#include "datasystem/common/device/ascend/acl_device_manager.h"
#include "datasystem/common/rdma/npu/hixl_plugin_loader.h"
#endif

namespace datasystem {
constexpr int FAILURE_LOG_RATE = 100;
namespace {
#ifdef ASCEND_HIXL_AVAILABLE
Status HixlApiResultToStatus(DsHixlResult result, uint32_t vendorStatus, const std::string &operation)
{
    if (result == DS_HIXL_OK) {
        return Status::OK();
    }
    const std::string message =
        FormatString("HIXL plugin %s failed, result=%d, vendor_status=%u", operation, result, vendorStatus);
    if (result == DS_HIXL_INVALID_ARGUMENT) {
        return Status(StatusCode::K_INVALID, message);
    }
    if (result == DS_HIXL_NOT_SUPPORTED) {
        return Status(StatusCode::K_NOT_SUPPORTED, message);
    }
    return Status(StatusCode::K_RUNTIME_ERROR, message);
}

bool HasFabricMemoryApi(const DsHixlApi *api)
{
    constexpr size_t requiredSize =
        offsetof(DsHixlApi, free_memory) + sizeof(((DsHixlApi *)nullptr)->free_memory);
    return api != nullptr && api->structSize >= requiredSize && api->malloc_memory != nullptr
           && api->free_memory != nullptr;
}
#endif
}  // namespace

Status RegisterFastTransportMemory(void *segAddress, const uint64_t &segSize)
{
    (void)segAddress;
    (void)segSize;
#ifdef USE_URMA
    if (IsUrmaEnabled() && IsRegisterWholeArenaEnabled() && segAddress != nullptr) {
        LOG(INFO) << "Doing URMA memory registration of size " << segSize;
        RETURN_IF_NOT_OK(UrmaManager::Instance().RegisterSegment(reinterpret_cast<uint64_t>(segAddress), segSize));
    }
#endif

#ifdef USE_RDMA
    if (IsUcpEnabled() && IsRegisterWholeArenaEnabled() && segAddress != nullptr) {
        LOG(INFO) << "Doing UCP memory registration of size " << segSize;
        RETURN_IF_NOT_OK(UcpManager::Instance().RegisterSegment(reinterpret_cast<uint64_t>(segAddress), segSize));
    }
#endif
    return Status::OK();
}

Status InitializeHostMemoryContext()
{
#ifdef USE_NPU
    if (IsRemoteH2DEnabled() && FLAGS_remote_h2d_link_type == "HCCS") {
        (void)RemoteH2DManager::Instance();
    }
#endif
    return Status::OK();
}

Status GetFabricMemoryArenaDeviceIds(std::vector<int32_t> &deviceIds)
{
    deviceIds.clear();
#ifdef USE_NPU
    std::string devIdStr = FLAGS_remote_h2d_device_ids;
    std::stringstream ss(devIdStr);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token.erase(std::remove_if(token.begin(), token.end(),
                                   [](unsigned char ch) { return std::isspace(ch) != 0; }),
                    token.end());
        if (token.empty()) {
            RETURN_STATUS(K_INVALID, "remote_h2d_device_ids flag has invalid empty item: " + devIdStr);
        }
        try {
            deviceIds.emplace_back(std::stoi(token));
        } catch (const std::invalid_argument &) {
            RETURN_STATUS(K_INVALID, "remote_h2d_device_ids flag has invalid form: " + devIdStr);
        } catch (const std::out_of_range &) {
            RETURN_STATUS(K_INVALID, "remote_h2d_device_ids flag contains out of range number: " + devIdStr);
        }
    }
    CHECK_FAIL_RETURN_STATUS(!deviceIds.empty(), K_INVALID,
                             "remote_h2d_device_ids must be set when HCCS FabricMem is enabled");
#else
    (void)deviceIds;
#endif
    return Status::OK();
}

Status AllocateFabricMemory(uint64_t size, int32_t deviceId, void **ptr)
{
    CHECK_FAIL_RETURN_STATUS(ptr != nullptr, StatusCode::K_INVALID, "FabricMem allocation output pointer is null");
    *ptr = nullptr;
#ifdef ASCEND_HIXL_AVAILABLE
    RETURN_IF_NOT_OK(acl::AclDeviceManager::Instance()->SetDeviceIdx(deviceId));
    const DsHixlApi *api = nullptr;
    RETURN_IF_NOT_OK(HixlPluginLoader::Instance().GetApi(api));
    CHECK_FAIL_RETURN_STATUS(HasFabricMemoryApi(api), StatusCode::K_NOT_SUPPORTED,
                             "HIXL plugin does not provide FabricMem allocation APIs");
    uint32_t vendorStatus = 0;
    DsHixlResult result = api->malloc_memory(DS_HIXL_MEMORY_HOST, size, ptr, &vendorStatus);
    RETURN_IF_NOT_OK(HixlApiResultToStatus(result, vendorStatus, "malloc_memory(MEM_HOST)"));
    CHECK_FAIL_RETURN_STATUS(*ptr != nullptr, StatusCode::K_RUNTIME_ERROR,
                             "HIXL plugin returned a null FabricMem allocation");
    LOG(INFO) << "Allocated FabricMem host arena through HIXL MallocMem, devId=" << deviceId << ", addr=" << *ptr
              << ", size=" << size;
    return Status::OK();
#else
    (void)size;
    (void)deviceId;
    RETURN_STATUS(StatusCode::K_NOT_SUPPORTED, "HIXL FabricMem allocation APIs are unavailable");
#endif
}

Status FreeFabricMemory(void *ptr)
{
    if (ptr == nullptr) {
        return Status::OK();
    }
#ifdef ASCEND_HIXL_AVAILABLE
    const DsHixlApi *api = nullptr;
    RETURN_IF_NOT_OK(HixlPluginLoader::Instance().GetApi(api));
    CHECK_FAIL_RETURN_STATUS(HasFabricMemoryApi(api), StatusCode::K_NOT_SUPPORTED,
                             "HIXL plugin does not provide FabricMem free APIs");
    uint32_t vendorStatus = 0;
    DsHixlResult result = api->free_memory(ptr, &vendorStatus);
    RETURN_IF_NOT_OK(HixlApiResultToStatus(result, vendorStatus, "free_memory"));
    return Status::OK();
#else
    RETURN_STATUS(StatusCode::K_NOT_SUPPORTED, "HIXL FabricMem free APIs are unavailable");
#endif
}

Status RegisterHostMemory(void *segAddress, const uint64_t &segSize)
{
    (void)segAddress;
    (void)segSize;
#ifdef USE_NPU
    const bool requireWholeArenaRegistration =
        FLAGS_urma_register_whole_arena || FLAGS_remote_h2d_link_type == "HCCS";
    if (IsRemoteH2DEnabled() && requireWholeArenaRegistration && segAddress != nullptr) {
        RETURN_IF_NOT_OK(RemoteH2DManager::Instance().RegisterHostMemory(segAddress, segSize));
    }
#endif
    return Status::OK();
}

Status UnregisterHostMemory(void *segAddress)
{
    (void)segAddress;
#ifdef USE_NPU
    const bool hasWholeArenaRegistration =
        FLAGS_urma_register_whole_arena || FLAGS_remote_h2d_link_type == "HCCS";
    if (IsRemoteH2DEnabled() && hasWholeArenaRegistration && segAddress != nullptr) {
        RETURN_IF_NOT_OK(RemoteH2DManager::Instance().UnregisterHostMemory(segAddress));
    }
#endif
    return Status::OK();
}

bool IsFastTransportEnabled()
{
    if (IsUrmaEnabled()) {
        return true;
    }

    if (IsUcpEnabled()) {
        return true;
    }

    return false;
}

bool IsRemoteH2DEnabled()
{
#ifdef USE_NPU
    return RemoteH2DManager::IsRemoteH2DEnabled();
#else
    return false;
#endif
}

namespace {
std::atomic<bool> &ClientUrmaRuntimeRequested()
{
    static std::atomic<bool> requested{ false };
    return requested;
}

std::atomic<bool> &ClientUrmaRuntimeReady()
{
    static std::atomic<bool> ready{ false };
    return ready;
}
}  // namespace

bool UsePhysicalFabricArena()
{
    return IsRemoteH2DEnabled() && FLAGS_remote_h2d_link_type == "HCCS"
           && FLAGS_remote_h2d_hccs_enable_fabric_mem;
}

bool IsUrmaEnabled()
{
#ifdef USE_URMA
    return FLAGS_enable_urma || ClientUrmaRuntimeReady().load(std::memory_order_acquire);
#else
    return false;
#endif
}

void RequestClientUrmaRuntime()
{
    ClientUrmaRuntimeRequested().store(true, std::memory_order_release);
}

bool ShouldRequestClientUrmaRuntime(bool workerUbEnabled, bool clientMayAccessNonBoundWorker, bool endpointUsesUb)
{
    // Endpoint transport and process capability are deliberately separate: SHM can serve the bound worker while
    // UB is still required for another node. Conversely, client routing options must not activate UB when the
    // worker does not advertise it, because activation performs device discovery and memory registration.
    return workerUbEnabled && (clientMayAccessNonBoundWorker || endpointUsesUb);
}

bool ClientMayAccessNonBoundWorker(bool enableLocalCache, bool enableCrossNodeConnection)
{
    return !enableLocalCache || enableCrossNodeConnection;
}

bool IsUrmaRuntimeConfigured()
{
#ifdef USE_URMA
    return FLAGS_enable_urma || ClientUrmaRuntimeRequested().load(std::memory_order_acquire);
#else
    return false;
#endif
}

void PublishClientUrmaRuntimeReady()
{
    ClientUrmaRuntimeReady().store(true, std::memory_order_release);
}

bool IsUcpEnabled()
{
#ifdef USE_RDMA
    return FLAGS_enable_rdma;
#else
    return false;
#endif
}

bool IsRegisterWholeArenaEnabled()
{
    return FLAGS_urma_register_whole_arena;
}

bool IsUbNumaAffinityEnabled()
{
#ifdef USE_URMA
    return IsUrmaEnabled() && FLAGS_enable_ub_numa_affinity && IsRegisterWholeArenaEnabled();
#else
    return false;
#endif
}

bool ShouldBuildUbNumaRangeTable()
{
#ifdef USE_URMA
    // Initialization must use "configured/requested", not "ready": the range table is built from inside
    // UrmaManager::Init, before PublishClientUrmaRuntimeReady can run.
    return IsUrmaRuntimeConfigured() && IsRegisterWholeArenaEnabled();
#else
    return false;
#endif
}

bool NeedRegisterWholeArena()
{
#ifdef USE_URMA
    if (IsUrmaEnabled() && IsRegisterWholeArenaEnabled()) {
        return true;
    }
#endif

#ifdef USE_RDMA
    if (IsUcpEnabled() && IsRegisterWholeArenaEnabled()) {
        return true;
    }
#endif
    return false;
}

Status WaitFastTransportEvent(std::vector<uint64_t> &keys, std::function<int64_t(void)> remainingTime,
                              std::function<Status(Status &)> errorHandler)
{
    return WaitFastTransportEventWithFailure(keys, std::move(remainingTime), std::move(errorHandler), nullptr);
}

Status WaitFastTransportEventWithFailure(std::vector<uint64_t> &keys, std::function<int64_t(void)> remainingTime,
                                         std::function<Status(Status &)> errorHandler, UrmaWriteFailure *failure)
{
    (void)keys;
    (void)remainingTime;
    (void)errorHandler;
    (void)failure;
#ifdef USE_URMA
    if (IsUrmaEnabled()) {
        Status firstError = Status::OK();
        UrmaSequentialWaitContext waitContext;
        for (auto key : keys) {
            // Wait for the event until timeout
            Status rc = UrmaManager::Instance().WaitToFinish(key, remainingTime(), failure, &waitContext);
            if (rc.IsError() && firstError.IsOk()) {
                firstError = errorHandler(rc);
            }
        }
        if (firstError.IsError()) {
            LOG_EVERY_N(ERROR, FAILURE_LOG_RATE) << "Failed to wait for URMA event. Detail: " << firstError;
            return firstError;
        }
    }
#endif

#ifdef USE_RDMA
    if (IsUcpEnabled()) {
        Status firstError = Status::OK();
        for (auto key : keys) {
            // Wait for the event until timeout
            Status rc = UcpManager::Instance().WaitToFinish(key, remainingTime());
            if (rc.IsError() && firstError.IsOk()) {
                firstError = errorHandler(rc);
            }
        }
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(firstError, "Failed to wait for RDMA event.");
    }
#endif
    return Status::OK();
}
}  // namespace datasystem
