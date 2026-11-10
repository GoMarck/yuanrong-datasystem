/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and limitations under the License.
 */
#ifndef DATASYSTEM_COMMON_RDMA_NPU_HIXL_ASYNC_EXECUTOR_H
#define DATASYSTEM_COMMON_RDMA_NPU_HIXL_ASYNC_EXECUTOR_H

#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "datasystem/common/rdma/npu/hixl_plugin_api.h"

namespace datasystem {
struct HixlAsyncResult {
    DsHixlResult result = DS_HIXL_OK;
    uint32_t vendorStatus = 0;
    uint64_t elapsedUs = 0;
};

// Callers retain the engine, registrations and backing memory until the future completes.
// Failed native handles are intentionally not released: completion is not a DMA fence on error.
class HixlAsyncExecutor {
public:
    static constexpr size_t MAX_IN_FLIGHT = 128;

    HixlAsyncExecutor() : poller_([this] { Poll(); }) {}
    ~HixlAsyncExecutor()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        poller_.join();
    }
    HixlAsyncExecutor(const HixlAsyncExecutor &) = delete;
    HixlAsyncExecutor &operator=(const HixlAsyncExecutor &) = delete;

    static bool Supported(const DsHixlApi *api)
    {
        constexpr size_t required = offsetof(DsHixlApi, release_transfer) + sizeof(api->release_transfer);
        return api != nullptr && api->structSize >= required && api->transfer_async != nullptr
            && api->query_transfer != nullptr && api->release_transfer != nullptr;
    }

    std::future<HixlAsyncResult> Submit(const DsHixlApi *api, DsHixlEngineHandle engine,
                                       const DsHixlTransferRequest &request)
    {
        auto task = std::make_shared<Task>();
        auto future = task->promise.get_future();
        task->api = api;
        task->engine = engine;
        task->deadline = Clock::now() + std::chrono::milliseconds(request.timeoutMs);
        if (!Supported(api) || request.timeoutMs <= 0) {
            task->promise.set_value({ Supported(api) ? DS_HIXL_TIMEOUT : DS_HIXL_NOT_SUPPORTED, 0 });
            return future;
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!changed_.wait_until(lock, task->deadline, [this] { return stopping_ || count_ < MAX_IN_FLIGHT; })) {
                task->promise.set_value({ DS_HIXL_TIMEOUT, 0 });
                return future;
            }
            if (stopping_) {
                task->promise.set_value({ DS_HIXL_RUNTIME_ERROR, 0 });
                return future;
            }
            for (auto &slot : tasks_) {
                if (slot == nullptr) {
                    slot = task;
                    ++count_;
                    break;
                }
            }
        }
        HixlAsyncResult submitted;
        submitted.result = api->transfer_async(engine, &request, &task->handle, &submitted.vendorStatus);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task->submitted = submitted;
            task->ready = true;
        }
        changed_.notify_all();
        return future;
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr auto POLL_INTERVAL = std::chrono::microseconds(50);
    struct Task {
        const DsHixlApi *api = nullptr;
        DsHixlEngineHandle engine = nullptr;
        DsHixlTransferHandle handle = nullptr;
        Clock::time_point started = Clock::now();
        Clock::time_point deadline;
        std::promise<HixlAsyncResult> promise;
        HixlAsyncResult submitted;
        bool ready = false;
    };

    void Poll()
    {
        for (;;) {
            std::array<std::shared_ptr<Task>, MAX_IN_FLIGHT> snapshot{};
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this] { return stopping_ || count_ != 0; });
                if (stopping_ && count_ == 0) {
                    return;
                }
                for (size_t i = 0; i < tasks_.size(); ++i) {
                    if (tasks_[i] != nullptr && tasks_[i]->ready) {
                        snapshot[i] = tasks_[i];
                    }
                }
            }
            for (size_t i = 0; i < snapshot.size(); ++i) {
                const auto &task = snapshot[i];
                if (task == nullptr) {
                    continue;
                }
                auto result = task->submitted;
                uint32_t state = DS_HIXL_TRANSFER_PENDING;
                if (result.result == DS_HIXL_OK) {
                    result.result = task->api->query_transfer(task->engine, task->handle, &state,
                                                              &result.vendorStatus);
                    if (result.result == DS_HIXL_OK && state == DS_HIXL_TRANSFER_PENDING) {
                        if (Clock::now() < task->deadline) {
                            continue;
                        }
                        result.result = DS_HIXL_TIMEOUT;
                    } else if (result.result == DS_HIXL_OK && state != DS_HIXL_TRANSFER_COMPLETED) {
                        result.result = DS_HIXL_RUNTIME_ERROR;
                    }
                    if (result.result == DS_HIXL_OK) {
                        result.result = task->api->release_transfer(task->handle);
                    }
                }
                result.elapsedUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::now() - task->started).count());
                task->promise.set_value(result);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    tasks_[i].reset();
                    --count_;
                }
                changed_.notify_all();
            }
            std::this_thread::sleep_for(POLL_INTERVAL);
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::array<std::shared_ptr<Task>, MAX_IN_FLIGHT> tasks_{};
    size_t count_ = 0;
    bool stopping_ = false;
    std::thread poller_;
};
}  // namespace datasystem
#endif
