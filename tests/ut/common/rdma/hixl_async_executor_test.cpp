/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and limitations under the License.
 */
#include "datasystem/common/rdma/npu/hixl_async_executor.h"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifndef DS_HIXL_ASYNC_STANDALONE_TEST
#include <gtest/gtest.h>
#endif

namespace {
struct FakeEngine {
    std::atomic<bool> complete{ false };
    std::atomic<bool> queryError{ false };
    std::atomic<bool> nativeFailed{ false };
    std::atomic<bool> submitError{ false };
    std::atomic<size_t> submitted{ 0 };
    std::atomic<size_t> released{ 0 };
};

DsHixlResult Submit(DsHixlEngineHandle engine, const DsHixlTransferRequest *, DsHixlTransferHandle *handle,
                    uint32_t *vendor)
{
    auto *fake = reinterpret_cast<FakeEngine *>(engine);
    ++fake->submitted;
    *vendor = 0;
    *handle = reinterpret_cast<DsHixlTransferHandle>(engine);
    return fake->submitError ? DS_HIXL_RUNTIME_ERROR : DS_HIXL_OK;
}

DsHixlResult Query(DsHixlEngineHandle engine, DsHixlTransferHandle, uint32_t *state, uint32_t *vendor)
{
    auto *fake = reinterpret_cast<FakeEngine *>(engine);
    *vendor = 0;
    *state = fake->complete ? DS_HIXL_TRANSFER_COMPLETED : DS_HIXL_TRANSFER_PENDING;
    if (fake->nativeFailed) {
        *state = DS_HIXL_TRANSFER_FAILED;
    }
    return fake->queryError ? DS_HIXL_RUNTIME_ERROR : DS_HIXL_OK;
}

DsHixlResult Release(DsHixlTransferHandle handle)
{
    ++reinterpret_cast<FakeEngine *>(handle)->released;
    return DS_HIXL_OK;
}

void Require(bool value, const char *message)
{
    if (!value) {
        throw std::runtime_error(message);
    }
}

void RunAsyncExecutorChecks()
{
    DsHixlApi api{};
    api.structSize = sizeof(api);
    api.transfer_async = Submit;
    api.query_transfer = Query;
    api.release_transfer = Release;
    DsHixlTransferDesc desc{ 1, 1, 1 };
    constexpr int timeoutMs = 1000;
    DsHixlTransferRequest request{ { "peer", 4 }, DS_HIXL_TRANSFER_READ, &desc, 1, timeoutMs };
    FakeEngine engine;
    auto handle = reinterpret_cast<DsHixlEngineHandle>(&engine);
    datasystem::HixlAsyncExecutor executor;

    auto legacy = api;
    legacy.structSize = offsetof(DsHixlApi, transfer_async);
    Require(executor.Submit(&legacy, handle, request).get().result == DS_HIXL_NOT_SUPPORTED,
            "Legacy plugin must not silently fall back");
    Require(engine.submitted == 0, "Unsupported plugin must not submit");
    auto missingQuery = api;
    missingQuery.query_transfer = nullptr;
    Require(executor.Submit(&missingQuery, handle, request).get().result == DS_HIXL_NOT_SUPPORTED,
            "Missing optional callback must be rejected");

    constexpr size_t chunkCount = 8;
    std::vector<std::future<datasystem::HixlAsyncResult>> futures;
    for (size_t i = 0; i < chunkCount; ++i) {
        futures.push_back(executor.Submit(&api, handle, request));
    }
    Require(engine.submitted == chunkCount && engine.released == 0, "All chunks must submit before completion");
    for (auto &future : futures) {
        Require(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                "Submission is not transfer completion");
    }
    engine.complete = true;
    for (auto &future : futures) {
        Require(future.get().result == DS_HIXL_OK, "Completion must succeed");
    }
    Require(engine.released == chunkCount, "Completed requests must release exactly once");

    FakeEngine other;
    other.complete = true;
    engine.complete = false;
    auto slow = executor.Submit(&api, handle, request);
    auto writeRequest = request;
    writeRequest.operation = DS_HIXL_TRANSFER_WRITE;
    auto fast = executor.Submit(&api, reinterpret_cast<DsHixlEngineHandle>(&other), writeRequest);
    Require(fast.get().result == DS_HIXL_OK, "Independent engine must complete without waiting for a slow route");
    Require(slow.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
            "Slow request must remain pending");
    engine.complete = true;
    Require(slow.get().result == DS_HIXL_OK, "Slow route must eventually complete");

    engine.complete = false;
    futures.clear();
    for (size_t i = 0; i < datasystem::HixlAsyncExecutor::MAX_IN_FLIGHT; ++i) {
        futures.push_back(executor.Submit(&api, handle, request));
    }
    const auto before = engine.submitted.load();
    auto shortRequest = request;
    shortRequest.timeoutMs = 1;
    Require(executor.Submit(&api, handle, shortRequest).get().result == DS_HIXL_TIMEOUT,
            "Admission must be bounded by deadline");
    Require(engine.submitted == before, "Full executor must not submit another native request");
    engine.complete = true;
    for (auto &future : futures) {
        Require(future.get().result == DS_HIXL_OK, "Accepted tasks must complete after backpressure");
    }

    engine.complete = false;
    const auto released = engine.released.load();
    Require(executor.Submit(&api, handle, shortRequest).get().result == DS_HIXL_TIMEOUT,
            "Pending request must time out");
    Require(engine.released == released, "Timeout must retain an uncertain native request");
    engine.queryError = true;
    Require(executor.Submit(&api, handle, request).get().result == DS_HIXL_RUNTIME_ERROR,
            "Query error must reach caller");
    Require(engine.released == released, "Query failure must not release an uncertain request");
    engine.queryError = false;
    engine.nativeFailed = true;
    Require(executor.Submit(&api, handle, request).get().result == DS_HIXL_RUNTIME_ERROR,
            "Native FAILED status must reach caller");
    Require(engine.released == released, "Native failure must not release uncertain resources");
    engine.nativeFailed = false;
    engine.submitError = true;
    Require(executor.Submit(&api, handle, request).get().result == DS_HIXL_RUNTIME_ERROR,
            "Submission error must reach caller");
    engine.submitError = false;
    engine.complete = true;
    std::future<datasystem::HixlAsyncResult> draining;
    {
        datasystem::HixlAsyncExecutor temporary;
        draining = temporary.Submit(&api, handle, request);
    }
    Require(draining.get().result == DS_HIXL_OK, "Shutdown must drain accepted tasks");
}
}  // namespace

#ifdef DS_HIXL_ASYNC_STANDALONE_TEST
int main()
{
    RunAsyncExecutorChecks();
    std::cout << "HiXL async executor checks passed\n";
}
#else
TEST(HixlAsyncExecutorTest, SubmissionCompletionBackpressureFailureAndShutdown)
{
    EXPECT_NO_THROW(RunAsyncExecutorChecks());
}
#endif
