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

#ifndef DATASYSTEM_COMMON_RDMA_NPU_HIXL_TRANSPORT_H
#define DATASYSTEM_COMMON_RDMA_NPU_HIXL_TRANSPORT_H

#include "rh2d_transport_strategy.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "datasystem/common/rdma/npu/hixl_plugin_api.h"

namespace datasystem {

enum class HixlMemoryMode { BUFFER_POOL, ROCE_DIRECT, FABRIC_MEM };

class HixlTransport : public RH2DTransportStrategy {
public:
    struct Options {
        std::string bufferPool = "0:0";
        bool csEnable = false;
    };

    HixlTransport() = default;
#ifdef WITH_TESTS
    HixlTransport(const DsHixlApi *api, bool skipDeviceBinding) : api_(api), skipDeviceBinding_(skipDeviceBinding)
    {
    }
#endif
    ~HixlTransport() override;

    Status Init(const std::vector<int32_t> &deviceIds) override;
    Status Init(const std::vector<int32_t> &deviceIds, const Options &options);
    Status GetConnectionIdentity(std::string *identity, int32_t devId = -1) override;
    Status GetDeviceEndpoint(int32_t devId, std::string &endpoint) const;
    Status Connect(const std::string &remoteIdentity, P2pKind kind, std::function<int()> *heartbeatCallback) override;
    Status Connect(const std::string &remoteIdentity, int32_t timeoutMs);
    Status RetainWriteConnection(const std::string &remoteIdentity, int32_t timeoutMs);
    Status ReleaseWriteConnection(const std::string &remoteIdentity);
    Status CheckReady() const;
    Status CheckReady(int32_t timeoutMs) const;
    Status Disconnect(const std::string &remoteIdentity) override;
    Status DisconnectAll() override;
    Status RegisterMemory(void *addr, uint64_t size, P2pSegmentInfo *segInfo) override;
    Status UnregisterMemory(void *addr) override;
    Status PrepareRemoteMemory(void *addr, uint64_t offset, uint64_t size, uint64_t *remoteAddr) override;
    Status PreRegisterDeviceMemory(const std::vector<void *> &addrs, const std::vector<uint64_t> &sizes) override;
    Status UnregisterDeviceMemory(const std::vector<void *> &addrs) override;
    Status ImportRemoteAddressInfo(const std::string &remoteEndpoint, const RemoteHostSegmentPb &seg) override;
    Status ScatterBatch(P2pScatterEntry *entries, uint32_t count, const std::string &remoteEndpoint,
                        std::shared_ptr<aclrtStream> stream) override;
    Status TransferBatch(HixlTransferOp op, const std::vector<HixlTransferDesc> &descs,
                         const std::string &remoteEndpoint, std::shared_ptr<aclrtStream> stream) override;
    P2pLink LinkType() const override;
    Status WriteBatch(const std::string &remoteEndpoint, const std::vector<DsHixlTransferDesc> &descriptors,
                       int32_t timeoutMs);
    bool UsesBufferPool() const { return bufferPoolEnabled_; }
    bool AllowSameDeviceConnection() const override;

    // Setter for HCCS local IP. Must be called before Init().
    void SetLocalEndpoint(const std::string &ep, bool isClient = false);

private:
    struct RegisteredDeviceMemory {
        uintptr_t addr;
        uint64_t size;
        DsHixlMemHandle handle;
        uint64_t references = 0;
    };

    struct WriteConnection {
        uint64_t references = 0;
        bool disconnectPending = false;
    };

    struct DeviceMemoryReference {
        std::pair<uintptr_t, uint64_t> range;
        uint64_t references = 0;
    };

    struct RegisteredHostMemory {
        int32_t devId;
        uintptr_t addr;
        uint64_t size;
        DsHixlMemHandle handle;
    };

    struct FabricMemoryRegion {
        void *sourceAddr = nullptr;
        uint64_t size = 0;
        std::map<int32_t, DsHixlMemHandle> registrationHandles;
    };

    Status InitializeSingleDevice(int32_t devId, const Options &options, DsHixlEngineHandle &engine,
                                  std::string &endpoint);
    Status BindDeviceForEngine(int32_t devId) const;
    Status ResolveDeviceEndpoint(int32_t devId, std::string &endpoint) const;
    std::string ResolveLocalCommRes(const std::string &bufferPool) const;
    Status ResolveTargetDeviceIds(const std::vector<int32_t> &deviceIds,
                                  std::vector<int32_t> &targetDevIds) const;
    void DestroyEngine(DsHixlEngineHandle engine) const;
    Status CheckReadyLocked() const;
    Status ConnectImpl(const std::string &remoteIdentity, int32_t timeoutMs, bool retainForWrite);
    Status DisconnectLocked(const std::string &remoteIdentity);
    Status ValidateDeviceMemoryRegistrationInputs(const std::vector<void *> &addrs,
                                                  const std::vector<uint64_t> &sizes) const;
    static std::string FormatDeviceMemoryRanges(const std::vector<RegisteredDeviceMemory> &registrations);
    Status DeregisterFabricMemory(const std::shared_ptr<FabricMemoryRegion> &region);
    Status FillFabricSegmentIdentity(int32_t devId, const std::shared_ptr<FabricMemoryRegion> &region,
                                     P2pSegmentInfo *segInfo) const;
    Status RegisterHostMemoryOnEngine(int32_t devId, void *addr, uint64_t size, DsHixlMemHandle &handle);
    Status EnsureFabricMemoryRegisteredOnDevice(int32_t devId, const std::shared_ptr<FabricMemoryRegion> &region);
    Status RegisterFabricMemory(void *addr, uint64_t size, P2pSegmentInfo *segInfo);
    Status RegisterRoceDirectHostMemory(void *addr, uint64_t size);
    Status TransferFabricMemBatchParallel(HixlTransferOp op, const std::vector<HixlTransferDesc> &descs,
                                          const std::string &remoteEndpoint);
    Status RegisterDeviceMemoryLocked(uintptr_t addr, uint64_t size);
    Status ReleaseDeviceMemoryLocked(uintptr_t addr, uint64_t size);
    Status ReleaseDeviceMemoryReferenceLocked(uintptr_t addr);
    bool HasRegisteredDeviceMemoryLocked(uintptr_t addr, uint64_t size) const;
    bool HasRegisteredHostMemoryLocked(int32_t devId, uintptr_t addr, uint64_t size) const;
    void ClearRegisteredDeviceMemory();
    void ClearRegisteredHostMemory();
    bool IsHixlRoceDirectMode() const;
    bool IsHixlFabricMemMode() const;
    bool IsHixlBufferPoolMode() const;
    bool ShouldAllowSameDeviceConnection() const;
    const char *RoleName() const;

    // Per-device plugin-owned HIXL engines: devId -> opaque engine handle.
    std::map<int32_t, DsHixlEngineHandle> engines_;
    // Base local IP (shared across all devices)
    std::string localIp_;
    // Whether this process is a client
    bool isClient_ = false;
    // Per-device "ip:port" endpoint, derived from localIp_ + dynamic port
    std::map<int32_t, std::string> localEndpointById_;
    // Next engine index for round-robin identity selection
    std::atomic<unsigned int> nextEngineIndex_{ 0 };
    bool initialized_ = false;
    HixlMemoryMode hixlMemoryMode_ = HixlMemoryMode::BUFFER_POOL;
    bool bufferPoolEnabled_ = false;
    // Physical FabricMem arenas keyed by source VA. Each worker HIXL engine that may expose the arena keeps its own
    // registration handle.
    std::map<uintptr_t, std::shared_ptr<FabricMemoryRegion>> fabricMemoryRegions_;
    std::mutex fabricMemoryMutex_;
    bool allowSameDeviceConnection_ = false;
    std::unordered_set<std::string> activeEndpoints_;
    // Transfers share a lifecycle lease; teardown and registration changes require exclusive access.
    mutable std::shared_timed_mutex lifecycleMutex_;
    // Connection bookkeeping also takes this mutex when holding a shared lifecycle lease.
    std::timed_mutex connectionMutex_;
    std::mutex temporaryRegistrationMutex_;
    std::atomic<bool> transferFailed_{ false };
    std::unordered_map<std::string, WriteConnection> writeConnections_;
    std::unordered_map<uintptr_t, DeviceMemoryReference> deviceMemoryReferences_;
    std::vector<RegisteredDeviceMemory> registeredDeviceMemories_;
    std::vector<RegisteredHostMemory> registeredHostMemories_;
    const DsHixlApi *api_ = nullptr;
#ifdef WITH_TESTS
    bool skipDeviceBinding_ = false;
#endif
};

}  // namespace datasystem

#endif  // DATASYSTEM_COMMON_RDMA_NPU_HIXL_TRANSPORT_H
