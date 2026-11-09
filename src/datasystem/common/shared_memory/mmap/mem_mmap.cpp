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

/**
 * Description: Memory mmap instance.
 */

#include "datasystem/common/shared_memory/mmap/mem_mmap.h"

#include <fcntl.h>
#include <atomic>
#include <optional>
#include <sstream>
#include <vector>
#ifdef __linux__
#include <linux/memfd.h>
#endif
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>

#include "datasystem/common/flags/flags.h"
#include "datasystem/common/shared_memory/mmap/allocation.h"
#ifdef ASCEND_SUPPORT_FABRIC_MEM
#include "datasystem/common/device/ascend/acl_device_manager.h"
#endif
#include "datasystem/common/flags/common_flags.h"
#include "datasystem/common/util/numa_util.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/common/util/strings_util.h"
#include "datasystem/common/util/timer.h"
#include "datasystem/utils/optional.h"
#include "datasystem/utils/status.h"

#include "datasystem/common/rdma/fast_transport_base.h"

DS_DEFINE_bool(enable_fallocate, true,
               "Due to Kubernetes' (k8s) resource calculation policies, "
               "shared memory is sometimes counted twice,"
               "which can lead to client crashes.To address this issue, "
               "fallocate is employed to link the client and worker nodes for shared memory,"
               "Enabling fallocate will lower the efficiency of memory allocation"
               "By default, fallocate is enabled.");
DS_DEFINE_bool(
    enable_thp, false,
    "Control this process by enabling transparent huge pages, default is disabled. Enable Transparent Huge Pages (THP) "
    "can enhance performance and reduce page table overhead, but it may also lead to increased memory usage");

namespace datasystem {
namespace memory {
#ifdef ASCEND_SUPPORT_FABRIC_MEM
namespace {
std::atomic<uint32_t> g_nextFabricArenaDevice{ 0 };

Status SelectFabricMemoryDeviceId(int32_t &deviceId)
{
    std::vector<int32_t> deviceIds;
    RETURN_IF_NOT_OK(GetFabricMemoryArenaDeviceIds(deviceIds));
    auto index = g_nextFabricArenaDevice.fetch_add(1, std::memory_order_relaxed) % deviceIds.size();
    deviceId = deviceIds[index];
    return Status::OK();
}
}  // namespace
#endif

MemMmap::~MemMmap()
{
#ifdef ASCEND_SUPPORT_FABRIC_MEM
    if (fabricMemoryInitialized_) {
        Destroy();
        if (!fabricUnregistered_) {
            LOG(ERROR) << "Keeping HIXL-allocated FabricMem arena alive because HIXL deregistration failed";
            return;
        }
        if (pointer_ != nullptr) {
            Status rc = FreeFabricMemory(pointer_);
            if (rc.IsError()) {
                LOG(ERROR) << "Failed to free FabricMem arena: " << rc.ToString();
                return;
            }
            pointer_ = nullptr;
        }
        fabricMemoryInitialized_ = false;
    }
#endif
}

void MemMmap::Destroy()
{
    if (fabricMemoryInitialized_) {
        if (!fabricUnregistered_) {
            Status rc = UnregisterHostMemory(pointer_);
            LOG_IF_ERROR(rc, "Failed to unregister HIXL-allocated FabricMem arena");
            fabricUnregistered_ = rc.IsOk();
        }
        return;
    }
    BaseMmap::Destroy();
}

#ifdef ASCEND_SUPPORT_FABRIC_MEM
Status MemMmap::GetFabricMemoryDeviceIds(int32_t &logicDevId, int32_t &physicalDevId)
{
    auto *aclManager = acl::AclDeviceManager::Instance();
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(aclManager->GetDeviceIdx(logicDevId), "aclrtGetDevice failed");
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(aclManager->GetPhysicalDeviceId(logicDevId, physicalDevId),
                                     "aclrtGetPhyDevIdByLogicDevId failed");
    return Status::OK();
}

#endif

Status MemMmap::InitializeFabricMemory(uint64_t size)
{
#ifdef ASCEND_SUPPORT_FABRIC_MEM
    RETURN_IF_NOT_OK(SelectFabricMemoryDeviceId(fabricMemoryDeviceId_));
    RETURN_IF_NOT_OK(AllocateFabricMemory(size, fabricMemoryDeviceId_, &pointer_));
    bool keepFabricMemory = false;
    Raii cleanupFabricMemory([this, &keepFabricMemory]() {
        if (keepFabricMemory || pointer_ == nullptr) {
            return;
        }
        LOG_IF_ERROR(FreeFabricMemory(pointer_), "Failed to roll back HIXL-allocated FabricMem arena");
        pointer_ = nullptr;
    });

    int32_t logicDevId = -1;
    int32_t physicalDevId = -1;
    RETURN_IF_NOT_OK(GetFabricMemoryDeviceIds(logicDevId, physicalDevId));

    fabricMemoryInitialized_ = true;
    posixMapped_ = false;
    type_ = "fabric_memory";
    mmapSize_ = size;
    curr_ = reinterpret_cast<uintptr_t>(pointer_);
    tail_ = curr_ + static_cast<uintptr_t>(mmapSize_);
    LOG(INFO) << "Initialized HIXL-allocated FabricMem arena, fd token=" << fd_ << ", addr=" << pointer_
              << ", size=" << size << ", logicDevId=" << logicDevId << ", physicalDevId=" << physicalDevId
              << ", requestedDeviceId=" << fabricMemoryDeviceId_;
    keepFabricMemory = true;
    return Status::OK();
#else
    (void)size;
    RETURN_STATUS(StatusCode::K_RUNTIME_ERROR, "FabricMem allocation APIs are unavailable");
#endif
}

Status MemMmap::Initialize(uint64_t size, bool populate, bool hugepage)
{
    // 1. Create tmpfs file.
    std::string tmpfs = "datasystem";
    std::string hugeTlbOpenHint = "";
    const bool usePhysicalFabricArena = UsePhysicalFabricArena();

    unsigned int mfdFlag = MFD_ALLOW_SEALING;
    if (hugepage && !usePhysicalFabricArena) {
#ifdef MFD_HUGETLB
        mfdFlag = MFD_ALLOW_SEALING | MFD_HUGETLB;
        hugeTlbOpenHint = " huge tlb is open, this probably means you have to increase /proc/sys/vm/nr_hugepages";
#else
        RETURN_STATUS(StatusCode::K_RUNTIME_ERROR, "Huge tlb not support!");
#endif
    }

    // memfd_create is not defined in EulerOS, use syscall instead for compatibility purposes.
    fd_ = syscall(SYS_memfd_create, tmpfs.c_str(), mfdFlag);
    CHECK_FAIL_RETURN_STATUS(fd_ >= 0, StatusCode::K_RUNTIME_ERROR,
                             "memfd_create failed: " + StrErr(errno) + hugeTlbOpenHint);

    if (usePhysicalFabricArena) {
        RETURN_IF_NOT_OK(InitializeHostMemoryContext());
        RETURN_IF_NOT_OK(InitializeFabricMemory(size));
        RETURN_IF_NOT_OK(RegisterHostMemory(pointer_, mmapSize_));
        return Status::OK();
    }

    unsigned int flags = MAP_SHARED;
    if (hugepage) {
        flags = MAP_SHARED | MAP_HUGETLB;
    }
    if (populate) {
        flags |= MAP_POPULATE;
    }
    type_ = "memory";
    RETURN_IF_NOT_OK(SetupFileMapping(size, flags, true));
    if (FLAGS_enable_thp && !hugepage) {
        int ret = madvise(pointer_, mmapSize_, MADV_HUGEPAGE);
        if (ret != 0) {
            LOG(WARNING) << "madvise HUGEPAGE " << type_ << " failed: " << StrErr(errno);
        }
    }
    const std::string &policy = FLAGS_shared_memory_distribution_policy;
    if (IsUrmaEnabled() && IsRegisterWholeArenaEnabled()
        && (policy == "interleave_all_numa" || policy == "interleave_affinity_numa")) {
        Timer timer;
        Status rc = (policy == "interleave_all_numa") ? DistributeMemoryAcrossAllNumaNodes(pointer_, mmapSize_)
                                                      : DistributeMemoryAcrossAffinityNumaNodes(pointer_, mmapSize_);
        LOG_IF_ERROR(rc, "DistributeMemoryAcross failed");
        LOG(INFO) << "DistributeMemory with policy " << policy << " took " << timer.ElapsedMilliSecond() << "ms";
    }
    // If urma or rdma is enabled, register the memory.
    RETURN_IF_NOT_OK(RegisterFastTransportMemory(pointer_, mmapSize_));
    // If Remote H2D is enabled, pin and register the npu memory
    RETURN_IF_NOT_OK(RegisterHostMemory(pointer_, mmapSize_));
    return Status::OK();
}

bool MemMmap::InRange(void *pointer, ptrdiff_t &offset)
{
    auto alloc = GetAllocation(pointer);
    if (alloc) {
        offset = alloc->offset;
        return true;
    }
    offset = 0;
    return false;
}

bool MemMmap::Commit(void *addr, size_t offset, size_t length)
{
    if (fabricMemoryInitialized_) {
        return false;
    }
    ptrdiff_t offsetInMmap = 0;
    if (!InRange(addr, offsetInMmap)) {
        return true;
    }
    VLOG(1) << "fallocate fd:" << fd_ << ", offsetInMmap:" << offsetInMmap << ", offset:" << offset
            << ", length:" << length;
    if (offsetInMmap < 0 || length > SSIZE_MAX || offset > SSIZE_MAX
        || offsetInMmap > SSIZE_MAX - static_cast<ssize_t>(offset)) {
        LOG(ERROR) << "Invalid Commit info: offsetInMmap:" << offsetInMmap << ", offset:" << offset
                   << ", length:" << length;
        return true;
    }
    if (!FLAGS_enable_fallocate) {
        return false;
    }
    // retry for EINTR
    int remainRetryTimes = 3;
    while (fallocate(fd_, 0, static_cast<ssize_t>(offsetInMmap + offset), static_cast<ssize_t>(length)) != 0) {
        LOG(ERROR) << "Failed to fallocate file:" << StrErr(errno);
        remainRetryTimes--;
        if (errno != EINTR || remainRetryTimes == 0) {
            return true;
        }
    }
    return false;
}

bool MemMmap::Decommit(void *addr, size_t offset, size_t length)
{
    if (fabricMemoryInitialized_) {
        return false;
    }
    if (IsFastTransportEnabled() || IsRemoteH2DEnabled()) {
        // if urma/rdma/RH2D is enabled memory is pinned
        // Decommit is a noop
        return false;
    }
    if (!FLAGS_enable_fallocate) {
        return false;
    }
    return BaseMmap::Decommit(addr, offset, length);
}

uint64_t MemMmap::GetPhysicalBackingSize() const
{
#ifdef ASCEND_SUPPORT_FABRIC_MEM
    return fabricMemoryInitialized_ ? mmapSize_ : 0;
#else
    return 0;
#endif
}

}  // namespace memory
}  // namespace datasystem
