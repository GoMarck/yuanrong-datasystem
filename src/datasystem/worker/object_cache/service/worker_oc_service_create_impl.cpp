/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.
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
 * Description: Defines the worker service processing create buffer process.
 */
#include <new>

#include "datasystem/common/util/validator.h"
#include "datasystem/worker/object_cache/service/worker_oc_service_create_impl.h"

#include <securec.h>
#include "datasystem/common/object_cache/buffer_composer.h"
#ifdef ASCEND_HIXL_AVAILABLE
#include "datasystem/common/rdma/npu/hixl_transport.h"
#include "datasystem/common/rdma/npu/remote_h2d_manager.h"
#endif
#include "datasystem/common/flags/flags.h"
#include "datasystem/common/flags/common_flags.h"
#include "datasystem/common/log/log.h"
#include "datasystem/common/iam/tenant_auth_manager.h"
#include "datasystem/common/parallel/parallel_for.h"
#include "datasystem/common/perf/perf_manager.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/rdma/fast_transport_manager_wrapper.h"
#include "datasystem/common/string_intern/string_ref.h"
#include "datasystem/common/util/format.h"
#include "datasystem/common/log/access_recorder.h"
#include "datasystem/common/log/latency_phase.h"
#include "datasystem/common/log/trace.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/status_helper.h"
#include "datasystem/common/util/uuid_generator.h"
#include "datasystem/utils/status.h"
#include "datasystem/worker/authenticate.h"

DS_DECLARE_uint64(oc_shm_transfer_threshold_kb);
DS_DECLARE_uint32(memory_alignment);

namespace datasystem {
namespace object_cache {


static constexpr double US_PER_MS = 1000.0;

static bool IsHexDigit(char value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

static bool IsAllocationIdValid(const std::string &allocationId)
{
    if (allocationId.size() != UUID_STRING_SIZE) {
        return false;
    }
    for (size_t index = 0; index < allocationId.size(); ++index) {
        const char value = allocationId[index];
        const bool hyphenPosition = index == 8 || index == 13 || index == 18 || index == 23;
        if (hyphenPosition ? value != '-' : !IsHexDigit(value)) {
            return false;
        }
    }
    return true;
}

WorkerOcServiceCreateImpl::WorkerOcServiceCreateImpl(WorkerOcServiceCrudParam &initParam,
                                                     std::shared_ptr<AkSkManager> akSkManager, HostPort &localAddress)
    : WorkerOcServiceCrudCommonApi(initParam),
      akSkManager_(std::move(akSkManager)),
      localAddress_(localAddress)
{
}

Status WorkerOcServiceCreateImpl::ReserveAllocationId(const std::string &allocationId, ShmKey &shmId)
{
    CHECK_FAIL_RETURN_STATUS(IsAllocationIdValid(allocationId), K_INVALID, "Invalid allocation ID");
    bool reservationInserted = false;
    try {
        shmId = ShmKey::Intern(allocationId);
        TbbCreatingAllocationTable::accessor accessor;
        if (!creatingAllocations_.insert(accessor, shmId)) {
            RETURN_STATUS(K_TRY_AGAIN, "The allocation ID is being created");
        }
        reservationInserted = true;
        accessor.release();

        const bool allocationExists = memoryRefTable_->ContainsShmUnit(shmId);
        INJECT_POINT_NO_RETURN("WorkerOcServiceCreateImpl.ReserveAllocationId.afterShmLookup");
        if (allocationExists) {
            (void)creatingAllocations_.erase(shmId);
            reservationInserted = false;
            RETURN_STATUS(K_DUPLICATED, "The allocation ID already has shared memory");
        }
    } catch (const std::bad_alloc &error) {
        if (reservationInserted) {
            (void)creatingAllocations_.erase(shmId);
        }
        return Status(K_OUT_OF_MEMORY, error.what());
    }
    return Status::OK();
}

Status WorkerOcServiceCreateImpl::ResolveCreateShmId(const std::string &allocationId, ShmKey &shmId, bool &reserved)
{
    reserved = false;
    if (allocationId.empty()) {
        std::string legacyShmId;
        RETURN_IF_NOT_OK(IndexUuidGenerator(shmIdCounter.fetch_add(1), legacyShmId));
        shmId = ShmKey::Intern(legacyShmId);
        return Status::OK();
    }
    RETURN_IF_NOT_OK(ReserveAllocationId(allocationId, shmId));
    reserved = true;
    return Status::OK();
}

Status WorkerOcServiceCreateImpl::ReserveMultiAllocationIds(const MultiCreateReqPb &req,
                                                            std::vector<ShmKey> &shmIds,
                                                            std::vector<ShmKey> &reservations)
{
    if (req.allocation_ids_size() == 0) {
        return Status::OK();
    }
    CHECK_FAIL_RETURN_STATUS(req.allocation_ids_size() == req.object_key_size(), K_INVALID,
                             "Allocation ID count does not match object key count");
    shmIds.reserve(req.allocation_ids_size());
    reservations.reserve(req.allocation_ids_size());
    for (const auto &allocationId : req.allocation_ids()) {
        ShmKey shmId;
        RETURN_IF_NOT_OK(ReserveAllocationId(allocationId, shmId));
        reservations.emplace_back(shmId);
        shmIds.emplace_back(std::move(shmId));
    }
    return Status::OK();
}

void WorkerOcServiceCreateImpl::ReleaseReservations(const std::vector<ShmKey> &reservations)
{
    for (const auto &shmId : reservations) {
        (void)creatingAllocations_.erase(shmId);
    }
}

Status WorkerOcServiceCreateImpl::FillCreateResponse(const ClientKey &clientId,
                                                     const std::shared_ptr<ShmUnit> &shmUnit,
                                                     size_t metadataSize, CreateRspPb &resp, bool hixlWrite)
{
    (void)clientId;
    (void)hixlWrite;
    resp.set_store_fd(shmUnit->GetFd());
    resp.set_mmap_size(shmUnit->GetMmapSize());
    resp.set_offset(shmUnit->GetOffset());
    resp.set_shm_id(shmUnit->GetId());
    resp.set_metadata_size(metadataSize);
#ifdef USE_URMA
    if (!hixlWrite && !ClientShmEnabled(clientId) && IsUrmaEnabled()) {
        RETURN_IF_NOT_OK(FillRequestUrmaInfo(localAddress_, shmUnit->GetPointer(), shmUnit->GetOffset(), metadataSize,
                                             resp, shmUnit->GetNumaId()));
    }
#endif
    return Status::OK();
}

Status WorkerOcServiceCreateImpl::AuthenticateCreateRequest(const CreateReqPb &req, std::string &tenantId)
{
    CHECK_FAIL_RETURN_STATUS(metadataRouteResolver_ != nullptr, StatusCode::K_NOT_READY,
                             "ETCD cluster manager is not provided.");
    Status rc = req.is_routed() ? worker::AuthenticateRequest(akSkManager_, req, req.tenant_id(), tenantId)
                                : worker::Authenticate(akSkManager_, req, tenantId);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(rc, "Authenticate failed.");
    return Status::OK();
}

Status WorkerOcServiceCreateImpl::Create(const CreateReqPb &req, CreateRspPb &resp)
{
    INJECT_POINT("worker.Create.begin");
    ScopedRequestContext ctx;
    Timer timer;
    PerfPoint point(PerfKey::WORKER_CREATE_OBJECT);
    auto config = GetServerLatencyTraceConfig();
    const bool traceEnabled = ShouldCollectLatencyTrace(config);
    if (traceEnabled) {
        Trace::Instance().AddLatencyTick(LatencyTickKey::WORKER_CREATE_START);
    }
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_CREATE);
    access.ObjectKeyProvider([&req]() -> std::string { return req.object_key(); }).DataSize(req.data_size());
    VLOG(1) << FormatString("Receive create meta request, clientId: %s, objectKey: %s, size: %zu", req.client_id(),
                            req.object_key(), req.data_size());
    int64_t remainingTimeMs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTime();
    INJECT_POINT_NO_RETURN("WorkerOcServiceCreateImpl.Create.timeoutMs",
                           [&remainingTimeMs]() { remainingTimeMs = -1; });
    if (remainingTimeMs <= 0) {
        const auto totalUs = static_cast<uint64_t>(timer.ElapsedMicroSecond());
        FinalizeWorkerLatencyTrace(LatencyTickKey::WORKER_CREATE_END, config, traceEnabled, totalUs, false, resp);
        Status rc = Status(StatusCode::K_RPC_DEADLINE_EXCEEDED,
            FormatString("The create request process time has exceeded the request timeout time (remaining: %lld ms)",
                         remainingTimeMs));
        access.Result(rc).Record();
        return rc;
    }
    std::string tenantId;
    Status authRc = AuthenticateCreateRequest(req, tenantId);
    if (authRc.IsError()) {
        access.Result(authRc).Record();
        return authRc;
    }
    Status rc = CreateImpl(tenantId, ClientKey::Intern(req.client_id()), req.object_key(), req.data_size(),
                           req.request_timeout(), req.allocation_id(), resp,
                           static_cast<CacheType>(req.cache_type()));
    const auto totalUs = static_cast<uint64_t>(timer.ElapsedMicroSecond());
    FinalizeWorkerLatencyTrace(LatencyTickKey::WORKER_CREATE_END, config, traceEnabled, totalUs, true, resp);
    access.Result(rc).Record();
    point.Record();
    const double totalMs = static_cast<double>(totalUs) / US_PER_MS;
    GetWorkerTimeCost().Append("Total Create", totalMs);
    INJECT_POINT("worker.Create.end");
    SLOW_LOG_IF_OR_VLOG(INFO, config.processSlowerThanUs > 0 && totalUs >= config.processSlowerThanUs, 1,
                        FormatString("Create done, cost: %.3fms, %s", totalMs, GetWorkerTimeCost().GetInfo()));
    return rc;
}

Status WorkerOcServiceCreateImpl::CreateImpl(const std::string &tenantId, const ClientKey &clientId,
                                             const std::string &rawObjectKey, size_t dataSize,
                                             int64_t requestTimeoutMs, const std::string &allocationId,
                                             CreateRspPb &resp, CacheType cacheType)
{
    auto objectKey = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, rawObjectKey);
    std::shared_ptr<SafeObjType> entry;
    bool isExist = objectTable_->Get(objectKey, entry).IsOk();
    if (isExist && entry != nullptr) {
        int64_t remUs = GetRequestContext()->reqTimeoutDuration.CalcRealRemainingTimeUs();
        if (entry->RLock(false, remUs).IsOk()) {
            Raii unlock([&entry]() { entry->RUnlock(); });
            CHECK_FAIL_RETURN_STATUS(!(*entry)->IsSealed(), K_OC_ALREADY_SEALED, "Cannot create sealed object.");
        }
    }
    ShmKey shmId;
    bool reserved = false;
    RETURN_IF_NOT_OK(ResolveCreateShmId(allocationId, shmId, reserved));
    Raii releaseReservation([this, &shmId, &reserved]() {
        if (reserved) {
            (void)creatingAllocations_.erase(shmId);
        }
    });

    auto shmUnit = std::make_shared<ShmUnit>();
    auto metadataSize = GetMetadataSize();
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(
        AllocateMemoryForObject(objectKey, dataSize, metadataSize, true, evictionManager_, *shmUnit, cacheType),
        "worker allocate memory failed");

    shmUnit->id = shmId;
    RETURN_IF_NOT_OK(FillCreateResponse(clientId, shmUnit, metadataSize, resp));
    bool reclaimable = !ClientShmEnabled(clientId);
    INJECT_POINT_NO_RETURN("worker.Create.reclaimable", [&reclaimable](bool value) { reclaimable = value; });
#ifdef WITH_TESTS
    INJECT_POINT("worker.Create.BeforeAddShmUnit");
#endif
    memoryRefTable_->AddShmUnit(clientId, shmUnit, requestTimeoutMs, reclaimable);

    INJECT_POINT("worker.Create.AllocateMemory");
    return Status::OK();
}

Status WorkerOcServiceCreateImpl::AggregateAllocateHelper(const MultiCreateReqPb &req, const std::string &tenantId,
                                                          std::vector<std::shared_ptr<ShmOwner>> &shmOwners,
                                                          std::vector<uint32_t> &shmIndexMapping)
{
    const size_t metaSz = GetMetadataSize();
    std::function<void(std::function<void(uint64_t, uint64_t, uint32_t)>, bool &)> traversalHelper =
        [&req, &metaSz](const std::function<void(uint64_t, uint64_t, uint32_t)> &collector, bool &needAggregate) {
            needAggregate = req.object_key_size() > 1;
            for (int i = 0; i < req.object_key_size(); i++) {
                collector(req.data_size(i), req.data_size(i) + metaSz, i);
            }
        };
    const auto firstObjectKey =
        TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, *req.object_key().begin());
    return AggregateAllocate(firstObjectKey, traversalHelper, evictionManager_, shmOwners, shmIndexMapping);
}

bool WorkerOcServiceCreateImpl::IsMultiCreateObjectExisting(const MultiCreateReqPb &req, int index,
                                                            const std::string &objectKey, MultiCreateRspPb &resp)
{
    if (req.skip_check_existence() || resp.exists(index)) {
        return !req.skip_check_existence() && resp.exists(index);
    }
    std::shared_ptr<SafeObjType> atomicEntry;
    if (objectTable_->GetAndLock(objectKey, atomicEntry).IsError()) {
        return false;
    }
    Raii unlock([&atomicEntry]() { atomicEntry->WUnlock(); });
    const bool exists = atomicEntry->Get() != nullptr && (*atomicEntry)->IsBinary() && !(*atomicEntry)->IsInvalid();
    if (exists) {
        resp.set_exists(index, true);
    }
    return exists;
}

Status WorkerOcServiceCreateImpl::AllocateMultiCreateShmUnit(
    const MultiCreateReqPb &req, int index, const std::string &objectKey,
    const std::vector<std::shared_ptr<ShmOwner>> &shmOwners,
    const std::vector<uint32_t> &shmIndexMapping, std::shared_ptr<ShmUnit> &shmUnit)
{
    std::shared_ptr<ShmOwner> shmOwner;
    if (shmIndexMapping.size() > static_cast<size_t>(index) && shmOwners.size() > shmIndexMapping[index]) {
        shmOwner = shmOwners[shmIndexMapping[index]];
    }
    shmUnit = std::make_shared<ShmUnit>();
    const auto metadataSize = GetMetadataSize();
    const auto dataSize = req.data_size(index);
    if (shmOwner != nullptr) {
        return DistributeMemoryForObject(objectKey, dataSize, metadataSize, true, shmOwner, *shmUnit);
    }
    return AllocateMemoryForObject(objectKey, dataSize, metadataSize, true, evictionManager_, *shmUnit);
}

Status WorkerOcServiceCreateImpl::FillMultiCreateShmUnits(
    const MultiCreateReqPb &req, const std::string &tenantId, const ClientKey &clientId,
    const std::vector<std::shared_ptr<ShmOwner>> &shmOwners,
    const std::vector<uint32_t> &shmIndexMapping, const std::vector<ShmKey> &requestShmIds,
    std::vector<std::shared_ptr<ShmUnit>> &shmUnits, std::vector<CreateRspPb> &subRsp,
    std::vector<Status> &results, MultiCreateRspPb &resp)
{
    const auto metadataSize = GetMetadataSize();
    for (int index = 0; index < req.object_key_size(); ++index) {
        const auto objectKey =
            TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, req.object_key(index));
        if (IsMultiCreateObjectExisting(req, index, objectKey, resp)) {
            continue;
        }
        PerfPoint point(PerfKey::WORKER_MULTI_CREATE_ALLOC_FOR_OBJECT);
        results[index] = AllocateMultiCreateShmUnit(
            req, index, objectKey, shmOwners, shmIndexMapping, shmUnits[index]);
        RETURN_IF_NOT_OK_PRINT_ERROR_MSG(results[index], "worker allocate memory failed");
        point.RecordAndReset(PerfKey::WORKER_MULTI_CREATE_GENERATE_SHM_UUID);
        if (requestShmIds.empty()) {
            std::string legacyShmId;
            RETURN_IF_NOT_OK(IndexUuidGenerator(shmIdCounter.fetch_add(1), legacyShmId));
            shmUnits[index]->id = ShmKey::Intern(legacyShmId);
        } else {
            shmUnits[index]->id = requestShmIds[index];
        }
        point.RecordAndReset(PerfKey::WORKER_MULTI_CREATE_FILL_SUB_RSP);
        results[index] = FillCreateResponse(clientId, shmUnits[index], metadataSize, subRsp[index],
                                                   req.hixl_layouts_size() > 0);
        if (results[index].IsOk() && req.hixl_layouts_size() > 0) {
            results[index] = FillHixlTarget(req, index, shmUnits[index], subRsp[index]);
        }
        if (results[index].IsError()) {
            shmUnits[index].reset();
        }
    }
    return Status::OK();
}

Status WorkerOcServiceCreateImpl::PrepareHixlWrite(MultiCreateReqPb &req, MultiCreateRspPb &resp)
{
    CHECK_FAIL_RETURN_STATUS(FLAGS_enable_hixl_d2rh && req.is_routed(), K_NOT_SUPPORTED,
                             "HIXL D2RH writes are disabled");
    CHECK_FAIL_RETURN_STATUS(FLAGS_remote_h2d_link_type == "HCCS", K_NOT_SUPPORTED,
                             "D2RH requires remote_h2d_link_type=HCCS");
    CHECK_FAIL_RETURN_STATUS(req.hixl_layouts_size() == req.object_key_size(), K_INVALID,
                             "HIXL layout count must match object count");
#ifdef ASCEND_HIXL_AVAILABLE
    {
        std::lock_guard<std::mutex> lock(hixlMutex_);
        if (hixlTransport_ == nullptr) {
            RETURN_IF_NOT_OK(RemoteH2DManager::Instance().GetInitStatus());
            std::vector<int32_t> devices;
            RETURN_IF_NOT_OK(RemoteH2DManager::Instance().GetWorkerDeviceIds(&devices));
            CHECK_FAIL_RETURN_STATUS(devices.size() == 1, K_NOT_SUPPORTED,
                                     "D2RH requires exactly one remote_h2d_device_ids entry");
            auto transport = std::make_shared<HixlTransport>();
            transport->SetLocalEndpoint(localAddress_.Host());
            RETURN_IF_NOT_OK(transport->Init(devices));
            CHECK_FAIL_RETURN_STATUS(transport->UsesBufferPool(), K_NOT_SUPPORTED,
                                     "D2RH currently requires the HIXL buffer pool");
            hixlTransport_ = std::move(transport);
        }
        std::string endpoint;
        RETURN_IF_NOT_OK(hixlTransport_->GetConnectionIdentity(&endpoint));
        resp.set_hixl_endpoint(endpoint);
    }
    uint64_t batchBytes = 0;
    for (int i = 0; i < req.object_key_size(); ++i) {
        const auto &sizes = req.hixl_layouts(i).blob_sizes();
        std::vector<uint64_t> header;
        uint64_t size = 0;
        RETURN_IF_NOT_OK(BuildComposedBufferHeader({ sizes.begin(), sizes.end() }, FLAGS_memory_alignment,
                                                   header, size));
        CHECK_FAIL_RETURN_STATUS(size <= SharedMemoryRefTable::MAX_HIXL_WRITE_BYTES - batchBytes, K_INVALID,
                                 "HIXL batch exceeds the protected memory budget");
        batchBytes += size;
        req.set_data_size(i, size);
    }
    return Status::OK();
#else
    (void)resp;
    RETURN_STATUS(K_NOT_SUPPORTED, "D2RH requires a build with HIXL support");
#endif
}

Status WorkerOcServiceCreateImpl::FillHixlTarget(const MultiCreateReqPb &req, int index,
                                                 const std::shared_ptr<ShmUnit> &unit, CreateRspPb &resp)
{
    const auto &sizes = req.hixl_layouts(index).blob_sizes();
    std::vector<uint64_t> header;
    uint64_t size = 0;
    RETURN_IF_NOT_OK(BuildComposedBufferHeader({ sizes.begin(), sizes.end() }, FLAGS_memory_alignment,
                                               header, size));
    CHECK_FAIL_RETURN_STATUS(size == req.data_size(index), K_INVALID, "HIXL object layout changed");
    auto *data = static_cast<uint8_t *>(unit->GetPointer()) + GetMetadataSize();
    const auto headerSize = header.size() * sizeof(uint64_t);
    CHECK_FAIL_RETURN_STATUS(memcpy_s(data, size, header.data(), headerSize) == EOK, K_RUNTIME_ERROR,
                             "Failed to compose HIXL object header");
    auto *target = resp.mutable_hixl_target();
    target->set_address(reinterpret_cast<uintptr_t>(data));
    target->set_size(size);
    target->set_header_size(headerSize);
    return Status::OK();
}

Status WorkerOcServiceCreateImpl::MultiCreateImpl(const MultiCreateReqPb &req, const std::string &tenantId,
                                                  MultiCreateRspPb &resp)
{
    PerfPoint point(PerfKey::WORKER_MULTI_CREATE_AGGREGATE_ALLOC);
    int objectSize = req.object_key_size();
    const auto clientId = ClientKey::Intern(req.client_id());
    std::vector<ShmKey> requestShmIds;
    std::vector<ShmKey> reservations;
    Raii releaseReservations([this, &reservations]() { ReleaseReservations(reservations); });
    RETURN_IF_NOT_OK(ReserveMultiAllocationIds(req, requestShmIds, reservations));
    std::vector<uint32_t> shmIndexMapping(req.object_key_size(), std::numeric_limits<uint32_t>::max());
    std::vector<std::shared_ptr<ShmOwner>> shmOwners;
    uint64_t hixlBudgetBytes = 0;
    size_t hixlBudgetCount = 0;
    bool releaseHixlBudget = false;
    Raii cancelHixlBudget([&]() {
        if (releaseHixlBudget) {
            memoryRefTable_->ReleaseHixlWriteBudget(hixlBudgetBytes, hixlBudgetCount);
        }
    });
    if (req.hixl_layouts_size() > 0) {
        for (int i = 0; i < objectSize; ++i) {
            if (!req.skip_check_existence() && resp.exists(i)) {
                continue;
            }
            const auto size = req.data_size(i) + GetMetadataSize();
            CHECK_FAIL_RETURN_STATUS(size <= SharedMemoryRefTable::MAX_HIXL_WRITE_BYTES - hixlBudgetBytes,
                                     K_OUT_OF_MEMORY, "HIXL batch exceeds the protected memory budget");
            hixlBudgetBytes += size;
            ++hixlBudgetCount;
        }
        RETURN_IF_NOT_OK(memoryRefTable_->ReserveHixlWriteBudget(hixlBudgetBytes, hixlBudgetCount));
        releaseHixlBudget = true;
    } else {
        RETURN_IF_NOT_OK(AggregateAllocateHelper(req, tenantId, shmOwners, shmIndexMapping));
    }
    std::vector<CreateRspPb> subRsp(objectSize);
    std::vector<Status> results(objectSize);

    point.RecordAndReset(PerfKey::WORKER_MULTI_CREATE_GET_SHM_UNITS);
    TbbMemoryClientRefTable::const_accessor clientAccessor;
    memoryRefTable_->ClientTableGetOrInsert(clientId, clientAccessor);
    std::vector<std::shared_ptr<ShmUnit>> shmUnits(objectSize);
    RETURN_IF_NOT_OK(FillMultiCreateShmUnits(req, tenantId, clientId, shmOwners, shmIndexMapping,
                                             requestShmIds, shmUnits, subRsp, results, resp));
    for (const auto &result : results) {
        RETURN_IF_NOT_OK(result);
    }
    point.RecordAndReset(PerfKey::WORKER_MULTI_CREATE_ADD_SHM_UNITS);
    // Install HIXL protection before exposing addresses; earlier failures retain only local owners.
    // No status-returning operation follows the transfer to the reference tables.
    if (req.hixl_layouts_size() > 0) {
        RETURN_IF_NOT_OK(memoryRefTable_->ProtectHixlWrites(clientId, shmUnits, hixlBudgetBytes, hixlBudgetCount));
        releaseHixlBudget = false;
    }
    memoryRefTable_->AddShmUnits(clientAccessor, shmUnits, req.request_timeout(),
                                 !ClientShmEnabled(clientAccessor->first));

    point.RecordAndReset(PerfKey::WORKER_MULTI_CREATE_FILL_ALL_RSP);
    resp.mutable_results()->Reserve(objectSize);
    for (int i = 0; i < objectSize; i++) {
        resp.mutable_results()->Add(std::move(subRsp[i]));
    }
    return Status::OK();
}

void WorkerOcServiceCreateImpl::CheckExistence(const MultiCreateReqPb &req, const std::string &tenantId,
                                               MultiCreateRspPb &resp)
{
    for (int i = 0; i < req.object_key().size(); i++) {
        const auto &objectKey = req.object_key(i);
        // Check whether the object is in local.
        {
            auto key = TenantAuthManager::ConstructNamespaceUriWithTenantId(tenantId, objectKey);
            std::shared_ptr<SafeObjType> entry;
            if (objectTable_->Get(key, entry).IsOk() && entry->RLock(false).IsOk()) {
                Raii unlock([&entry]() { entry->RUnlock(); });
                if ((*entry)->IsBinary() && !(*entry)->IsInvalid()) {
                    resp.add_exists(true);
                    continue;
                };
            }
        }
        resp.add_exists(false);
    }
}

Status WorkerOcServiceCreateImpl::MultiCreate(const MultiCreateReqPb &req, MultiCreateRspPb &resp)
{
    PerfPoint pointAll(PerfKey::WORKER_MULTI_CREATE_TOTAL);
    PerfPoint point(PerfKey::WORKER_MULTI_CREATE_INPUT_CHECK);
    CHECK_FAIL_RETURN_STATUS(metadataRouteResolver_ != nullptr, StatusCode::K_NOT_READY,
                             "ETCD cluster manager is not provided.");
    std::string tenantId;
    Status authRc = req.is_routed() ? worker::AuthenticateRequest(akSkManager_, req, req.tenant_id(), tenantId)
                                    : worker::Authenticate(akSkManager_, req, tenantId);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(authRc, "Authenticate failed.");
    CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(Validator::IsBatchSizeUnderLimit(req.object_key_size()),
                                         StatusCode::K_INVALID, "invalid object size");
    CHECK_FAIL_RETURN_STATUS(req.object_key_size() == req.data_size_size(), K_INVALID,
                             FormatString("object key count %zu not match with data size count %zu",
                                          req.object_key_size(), req.data_size_size()));
    if (!req.skip_check_existence()) {
        CheckExistence(req, tenantId, resp);
    }
    point.RecordAndReset(PerfKey::WORKER_MULTI_CREATE_IMPL);
    Status rc;
    if (req.hixl_layouts_size() > 0) {
        auto hixlReq = req;
        rc = PrepareHixlWrite(hixlReq, resp);
        if (rc.IsOk()) {
            rc = MultiCreateImpl(hixlReq, tenantId, resp);
        }
    } else {
        rc = MultiCreateImpl(req, tenantId, resp);
    }
    if (rc.IsError()) {
        resp.Clear();
    }
    return rc;
}
}  // namespace object_cache
}  // namespace datasystem
