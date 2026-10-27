/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
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

#ifndef DATASYSTEM_COMMON_RPC_BRPC_SINGLE_STREAM_H
#define DATASYSTEM_COMMON_RPC_BRPC_SINGLE_STREAM_H

#include <cerrno>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

#include <brpc/stream.h>
#include <bthread/condition_variable.h>
#include <bthread/mutex.h>
#include <butil/time.h>
#include <google/protobuf/service.h>

#include "datasystem/common/rpc/brpc_status_util.h"
#include "datasystem/common/util/status_helper.h"

namespace datasystem {
inline constexpr char SINGLE_STREAM_DATA = 0;
inline constexpr char SINGLE_STREAM_END_WRITES[] = "\x01" "DS_END";

inline Status WriteSingleStreamFrame(brpc::StreamId id, const butil::IOBuf &frame, const timespec &deadline)
{
    CHECK_FAIL_RETURN_STATUS(butil::gettimeofday_us() < deadline.tv_sec * 1000000LL + deadline.tv_nsec / 1000,
                             K_RPC_DEADLINE_EXCEEDED, "Client-streaming write deadline exceeded.");
    int error = 0;
    while ((error = brpc::StreamWrite(id, frame)) == EAGAIN) {
        error = brpc::StreamWait(id, &deadline);
        if (error != 0) {
            break;
        }
    }
    CHECK_FAIL_RETURN_STATUS(error == 0,
                             error == ETIMEDOUT ? K_RPC_DEADLINE_EXCEEDED : K_RPC_CANCELLED,
                             "Failed to write client-streaming frame.");
    return Status::OK();
}

class BrpcSingleStreamCall {
public:
    virtual ~BrpcSingleStreamCall() = default;
    virtual void Close() = 0;
};

// Early handshake completion ends BRPC's RPC accounting before the business call returns.
class BrpcSingleStreamService : public google::protobuf::Service {
public:
    class CallScope {
    public:
        CallScope(BrpcSingleStreamService &owner, std::shared_ptr<BrpcSingleStreamCall> call) : owner_(owner)
        {
            std::lock_guard<bthread::Mutex> lock(owner_.mutex_);
            active_ = !owner_.stopping_;
            if (active_) {
                entry_ = owner_.calls_.insert(owner_.calls_.end(), std::move(call));
            }
        }
        ~CallScope()
        {
            if (active_) {
                std::lock_guard<bthread::Mutex> lock(owner_.mutex_);
                owner_.calls_.erase(entry_);
                owner_.condition_.notify_all();
            }
        }
        CallScope(const CallScope &) = delete;
        CallScope &operator=(const CallScope &) = delete;
        bool IsActive() const { return active_; }
    private:
        BrpcSingleStreamService &owner_;
        std::list<std::shared_ptr<BrpcSingleStreamCall>>::iterator entry_;
        bool active_ = false;
    };

    void Stop()
    {
        std::vector<std::shared_ptr<BrpcSingleStreamCall>> calls;
        {
            std::lock_guard<bthread::Mutex> lock(mutex_);
            stopping_ = true;
            calls.assign(calls_.begin(), calls_.end());
        }
        for (const auto &call : calls) {
            call->Close();
        }
    }
    void Wait()
    {
        std::unique_lock<bthread::Mutex> lock(mutex_);
        while (!calls_.empty()) {
            condition_.wait(lock);
        }
    }
    void StopAndWait()
    {
        Stop();
        Wait();
    }

private:
    bthread::Mutex mutex_;
    bthread::ConditionVariable condition_;
    std::list<std::shared_ptr<BrpcSingleStreamCall>> calls_;
    bool stopping_ = false;
};
}  // namespace datasystem
#endif  // DATASYSTEM_COMMON_RPC_BRPC_SINGLE_STREAM_H
