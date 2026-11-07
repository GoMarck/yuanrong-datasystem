/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
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
#ifndef KVTEST_BENCHMARK_PROFILE_H
#define KVTEST_BENCHMARK_PROFILE_H

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace kvtest_profile {
enum class Stage : size_t {
    DS_SET, DS_GET, DS_RPC_CREATE, DS_RPC_PUBLISH, DS_RPC_QUERY, DS_RPC_GET,
    DS_SERVER_CREATE, DS_SERVER_PUBLISH, DS_SERVER_QUERY, DS_SERVER_GET,
    DS_RPC_OVERHEAD_CREATE, DS_RPC_OVERHEAD_PUBLISH, DS_RPC_OVERHEAD_QUERY, DS_RPC_OVERHEAD_GET,
    DS_RPC_RELEASE, DS_SERVER_RELEASE, DS_RPC_OVERHEAD_RELEASE,
    DS_RPC_CREATE_META, DS_SERVER_CREATE_META, DS_RPC_OVERHEAD_CREATE_META,
    DS_RPC_QUERY_META, DS_SERVER_QUERY_META, DS_RPC_OVERHEAD_QUERY_META,
    DS_SHM_WRITE_MAP, DS_SHM_READ_MAP, DS_SHM_LOOKUP, DS_SHM_FETCH_MAP, DS_COPY, DS_UB_ALLOC,
    DS_UB_WRITE, DS_UB_SUBMIT, DS_UB_WAIT,
    MC_PUT, MC_GET, MC_RPC_PUT_START, MC_RPC_PUT_END, MC_RPC_QUERY,
    MC_SERVER_PUT_START, MC_SERVER_PUT_END, MC_SERVER_QUERY,
    MC_PUT_ALLOC, MC_GET_ALLOC, MC_COPY, MC_TE_READ, MC_TE_WRITE, MC_TE_SUBMIT, MC_TE_WAIT, MC_LOCAL_COPY,
    DS_SET_CONTEXT,
    DS_SET_IMPL,
    DS_SET_ACCESS,
    DS_GET_CONTEXT,
    DS_GET_IMPL,
    DS_GET_ACCESS,
    DS_GET_RETURN,
    DS_SET_ROUTE,
    DS_SET_CREATE,
    DS_SET_PUBLISH,
    DS_SET_CLEANUP,
    DS_GET_ROUTE,
    DS_GET_TRANSPORT,
    DS_GET_MATERIALIZE,
    DS_QUERY_PREPARE,
    DS_QUERY_ATTEMPT_PREPARE,
    DS_QUERY_ATTEMPT,
    DS_QUERY_RETRY,
    DS_QUERY_APPLY,
    DS_GET_UB_PREPARE,
    DS_CREATE_RPC_PREPARE,
    DS_CREATE_RPC_SIGN,
    DS_CREATE_RPC_CALL,
    DS_CREATE_RPC_RESULT,
    DS_PUBLISH_RPC_PREPARE,
    DS_PUBLISH_RPC_SIGN,
    DS_PUBLISH_RPC_CALL,
    DS_PUBLISH_RPC_RESULT,
    DS_QUERY_RPC_PREPARE,
    DS_QUERY_RPC_SIGN,
    DS_QUERY_RPC_CALL,
    DS_QUERY_RPC_RESULT,
    DS_W_CREATE_AUTH,
    DS_W_CREATE_LOOKUP,
    DS_W_CREATE_RESERVE,
    DS_W_CREATE_ALLOC,
    DS_W_CREATE_RESPONSE,
    DS_W_CREATE_REF,
    DS_W_PUBLISH_AUTH,
    DS_W_PUBLISH_VALIDATE,
    DS_W_PUBLISH_LOCK,
    DS_W_PUBLISH_PREPARE,
    DS_W_PUBLISH_META,
    DS_W_PUBLISH_COMMIT,
    DS_W_PUBLISH_CLEANUP,
    DS_W_QUERY_AUTH,
    DS_W_QUERY_LOCAL,
    DS_W_QUERY_META,
    DS_W_QUERY_DELIVERY,
    DS_W_QUERY_ACQUIRE,
    DS_W_QUERY_UB_LATCH,
    DS_W_QUERY_UB_PREPARE,
    DS_W_QUERY_UB_WRITE,
    DS_W_QUERY_UB_RESULT,
    DS_W_QUERY_ENCODE_SHM,
    DS_UB_POLL_SLEEP,
    DS_UB_NOTIFY,
    DS_UB_WAIT_EVENT,
    DS_UB_WAIT_RESULT,
    MC_PUT_PREPARE,
    MC_PUT_CLIENT_PUT,
    MC_GET_PREPARE,
    MC_GET_CLIENT_GET,
    MC_GET_QUERY_CALL,
    MC_GET_RESULT,
    MC_PUT_RESULT,
    DS_SET_ADMISSION,
    DS_SET_EXECUTE,
    DS_SET_POSTPROCESS,
    DS_GET_INPUT,
    DS_GET_EXECUTE,
    DS_GET_RESULT,
    DS_UB_WAKE_SCHED,
    DS_UB_COMPLETION_OBSERVE,
    DS_UB_EVENT_PROCESS,
    DS_W_PUBLISH_POSTPROCESS,
    COUNT
};
inline constexpr const char *STAGE_NAMES[] = {
    "ds.api.set", "ds.api.get", "ds.rpc.create", "ds.rpc.publish", "ds.rpc.query", "ds.rpc.get",
    "ds.server.create", "ds.server.publish", "ds.server.query", "ds.server.get",
    "ds.rpc_overhead.create", "ds.rpc_overhead.publish", "ds.rpc_overhead.query", "ds.rpc_overhead.get",
    "ds.rpc.release", "ds.server.release", "ds.rpc_overhead.release",
    "ds.rpc.create_meta", "ds.server.create_meta", "ds.rpc_overhead.create_meta",
    "ds.rpc.query_meta", "ds.server.query_meta", "ds.rpc_overhead.query_meta",
    "ds.shm.write_map", "ds.shm.read_map", "ds.shm.lookup", "ds.shm.fetch_map", "ds.copy", "ds.ub.alloc",
    "ds.ub.write", "ds.ub.submit", "ds.ub.wait",
    "mc.api.put", "mc.api.get", "mc.rpc.put_start", "mc.rpc.put_end", "mc.rpc.query",
    "mc.server.put_start", "mc.server.put_end", "mc.server.query",
    "mc.alloc.put", "mc.alloc.get", "mc.copy", "mc.te.read", "mc.te.write", "mc.te.submit", "mc.te.wait",
    "mc.local_copy",
    "ds.sdk.set.context",
    "ds.sdk.set.impl",
    "ds.sdk.set.access",
    "ds.sdk.get.context",
    "ds.sdk.get.impl",
    "ds.sdk.get.access",
    "ds.sdk.get.return",
    "ds.sdk.set.route",
    "ds.sdk.set.create",
    "ds.sdk.set.publish",
    "ds.sdk.set.cleanup",
    "ds.sdk.get.route",
    "ds.sdk.get.transport",
    "ds.sdk.get.materialize",
    "ds.sdk.get.query_prepare",
    "ds.sdk.get.query_attempt_prepare",
    "ds.sdk.get.query_attempt",
    "ds.sdk.get.query_retry",
    "ds.sdk.get.query_apply",
    "ds.sdk.get.ub_prepare",
    "ds.sdk.create.prepare",
    "ds.sdk.create.sign",
    "ds.sdk.create.call",
    "ds.sdk.create.result",
    "ds.sdk.publish.prepare",
    "ds.sdk.publish.sign",
    "ds.sdk.publish.call",
    "ds.sdk.publish.result",
    "ds.sdk.query.prepare",
    "ds.sdk.query.sign",
    "ds.sdk.query.call",
    "ds.sdk.query.result",
    "ds.worker.create.auth",
    "ds.worker.create.lookup",
    "ds.worker.create.reserve",
    "ds.worker.create.alloc",
    "ds.worker.create.response",
    "ds.worker.create.reference",
    "ds.worker.publish.auth",
    "ds.worker.publish.validate",
    "ds.worker.publish.lock",
    "ds.worker.publish.prepare",
    "ds.worker.publish.metadata",
    "ds.worker.publish.commit",
    "ds.worker.publish.cleanup",
    "ds.worker.query.auth",
    "ds.worker.query.local",
    "ds.worker.query.metadata",
    "ds.worker.query.delivery",
    "ds.worker.query.acquire",
    "ds.worker.query.ub_latch",
    "ds.worker.query.ub_prepare",
    "ds.worker.query.ub_write",
    "ds.worker.query.ub_result",
    "ds.worker.query.encode_shm",
    "ds.ub.poll_sleep",
    "ds.ub.notify",
    "ds.ub.wait_event",
    "ds.ub.wait_result",
    "mc.sdk.put.prepare",
    "mc.sdk.put.client_put",
    "mc.sdk.get.prepare",
    "mc.sdk.get.client_get",
    "mc.sdk.get.query_call",
    "mc.sdk.get.result",
    "mc.sdk.put.result",
    "ds.sdk.set.admission",
    "ds.sdk.set.execute",
    "ds.sdk.set.postprocess",
    "ds.sdk.get.input",
    "ds.sdk.get.execute",
    "ds.sdk.get.result",
    "ds.ub.wake_sched",
    "ds.ub.completion_observe",
    "ds.ub.event_process",
    "ds.worker.publish.postprocess"
};
inline constexpr std::array<uint64_t, 22> BUCKET_NS = {
    100, 250, 500, 1000, 2000, 4000, 8000, 16000, 32000, 64000, 128000, 256000,
    512000, 1000000, 2000000, 4000000, 8000000, 16000000, 64000000, 256000000,
    1000000000, std::numeric_limits<uint64_t>::max()
};
static_assert(sizeof(STAGE_NAMES) / sizeof(*STAGE_NAMES) == static_cast<size_t>(Stage::COUNT));

inline bool Enabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("KVTEST_STAGE_PROFILE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

namespace detail {
inline uint64_t NowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct alignas(64) Slot {
    std::atomic<uint64_t> sum{0}, max{0}, bytes{0}, failed{0}, unknown{0};
    std::array<std::atomic<uint64_t>, BUCKET_NS.size()> buckets{};
};

class Recorder {
public:
    Recorder()
    {
        char host[256]{};
        if (gethostname(host, sizeof(host) - 1) != 0) std::strcpy(host, "unknown");
        for (char &c : host) {
            if (c == '\0') break;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '.')) c = '_';
        }
        instance_ = std::string(host) + ":" + std::to_string(getpid()) + ":" + std::to_string(NowNs());
        const char *prefix = std::getenv("KVTEST_STAGE_PROFILE_OUTPUT");
        if (prefix != nullptr && *prefix != '\0') {
            const std::string path = std::string(prefix) + "." + std::to_string(getpid()) + ".log";
            output_ = std::fopen(path.c_str(), "w");
            if (output_ == nullptr) std::fprintf(stderr, "KVTEST_STAGE_PROFILE: cannot open output file\n");
        } else {
            output_ = stderr;
        }
        try {
            reporter_ = std::thread([this] {
                std::unique_lock<std::mutex> lock(stopMutex_);
                while (!stopping_) {
                    if (stopCv_.wait_for(lock, std::chrono::seconds(10), [this] { return stopping_; })) break;
                    Flush(false);
                }
            });
        } catch (...) {
            if (output_ != nullptr && output_ != stderr) std::fclose(output_);
            throw;
        }
    }

    ~Recorder() { Stop(); }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(stopMutex_);
            if (stopping_) return;
            stopping_ = true;
        }
        stopCv_.notify_one();
        reporter_.join();
        Flush(true);
    }

    Slot &GetSlot(Stage stage)
    {
        // Shards are independent accumulators; bthread migration does not require thread affinity.
        static std::atomic<size_t> next{0};
        thread_local const size_t shard = next.fetch_add(1, std::memory_order_relaxed) % SHARDS;
        return slots_[static_cast<size_t>(stage)][shard];
    }

    void Flush(bool final) noexcept
    {
        try {
            FlushImpl(final);
        } catch (...) {
            std::fprintf(stderr, "KVTEST_STAGE_PROFILE: snapshot failed\n");
        }
    }

    void FlushImpl(bool final)
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        if (output_ == nullptr) return;
        ++sequence_;
        for (size_t stage = 0; stage < slots_.size(); ++stage) {
            std::array<uint64_t, BUCKET_NS.size()> buckets{};
            uint64_t sum = 0, max = 0, bytes = 0, failed = 0, unknown = 0, count = 0;
            for (const auto &slot : slots_[stage]) {
                sum += slot.sum.load(std::memory_order_relaxed);
                bytes += slot.bytes.load(std::memory_order_relaxed);
                failed += slot.failed.load(std::memory_order_relaxed);
                unknown += slot.unknown.load(std::memory_order_relaxed);
                const auto value = slot.max.load(std::memory_order_relaxed);
                if (value > max) max = value;
                for (size_t i = 0; i < buckets.size(); ++i) buckets[i] += slot.buckets[i].load(std::memory_order_relaxed);
            }
            for (auto value : buckets) count += value;
            if (count == 0) continue;
            std::string histogram;
            for (size_t i = 0; i < buckets.size(); ++i) {
                if (i != 0) histogram += ',';
                histogram += std::to_string(buckets[i]);
            }
            std::fprintf(output_, "[KVTEST_STAGE_PROFILE] {\"schema\":1,\"instance\":\"%s\",\"seq\":%llu,"
                "\"final\":%s,\"phase\":\"%s\",\"count\":%llu,\"sum_ns\":%llu,\"max_ns\":%llu,"
                "\"requested_bytes\":%llu,\"failed\":%llu,\"unknown\":%llu,\"buckets\":[%s]}\n",
                instance_.c_str(), static_cast<unsigned long long>(sequence_), final ? "true" : "false",
                STAGE_NAMES[stage], static_cast<unsigned long long>(count), static_cast<unsigned long long>(sum),
                static_cast<unsigned long long>(max), static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(failed), static_cast<unsigned long long>(unknown), histogram.c_str());
        }
        if (std::fflush(output_) != 0) std::fprintf(stderr, "KVTEST_STAGE_PROFILE: output write failed\n");
    }

private:
    static constexpr size_t SHARDS = 16;
    std::array<std::array<Slot, SHARDS>, static_cast<size_t>(Stage::COUNT)> slots_{};
    FILE *output_{nullptr};
    std::string instance_;
    uint64_t sequence_{0};
    std::mutex stopMutex_, outputMutex_;
    std::condition_variable stopCv_;
    bool stopping_{false};
    std::thread reporter_;
};

inline Recorder *GetRecorder() noexcept
{
    // Keep counters alive for SDK cleanup in later static destructors; stop the exporter at exit.
    static Recorder *const recorder = []() noexcept -> Recorder * {
        try {
            auto value = std::make_unique<Recorder>();
            if (std::atexit([] { if (auto *recorder = GetRecorder()) recorder->Stop(); }) != 0) return nullptr;
            return value.release();
        } catch (...) {
            std::fprintf(stderr, "KVTEST_STAGE_PROFILE: recorder initialization failed\n");
            return nullptr;
        }
    }();
    return recorder;
}

inline Slot *GetSlot(Stage stage) noexcept
{
    if (stage == Stage::COUNT || !Enabled()) return nullptr;
    auto *recorder = GetRecorder();
    return recorder == nullptr ? nullptr : &recorder->GetSlot(stage);
}

inline void Record(Slot &slot, uint64_t ns, uint64_t bytes, int outcome)
{
    slot.sum.fetch_add(ns, std::memory_order_relaxed);
    if (bytes != 0) slot.bytes.fetch_add(bytes, std::memory_order_relaxed);
    if (outcome == 0) slot.failed.fetch_add(1, std::memory_order_relaxed);
    if (outcome < 0) slot.unknown.fetch_add(1, std::memory_order_relaxed);
    auto max = slot.max.load(std::memory_order_relaxed);
    while (ns > max && !slot.max.compare_exchange_weak(max, ns, std::memory_order_relaxed)) {}
    size_t bucket = 0;
    while (ns > BUCKET_NS[bucket]) ++bucket;
    slot.buckets[bucket].fetch_add(1, std::memory_order_relaxed);
}
}  // namespace detail

inline void RecordDuration(Stage stage, uint64_t ns, bool success)
{
    if (auto *slot = detail::GetSlot(stage)) detail::Record(*slot, ns, 0, success ? 1 : 0);
}

class Span {
public:
    explicit Span(Stage stage, uint64_t requestedBytes = 0)
        : slot_(detail::GetSlot(stage)), bytes_(requestedBytes),
          start_(slot_ == nullptr ? 0 : detail::NowNs())
    {
    }
    Span(const Span &) = delete;
    Span &operator=(const Span &) = delete;
    ~Span() { FinishUnknown(); }

    void Next(Stage stage)
    {
        FinishUnknown();
        slot_ = detail::GetSlot(stage);
        bytes_ = 0;
        start_ = slot_ == nullptr ? 0 : detail::NowNs();
    }

    void Finish(bool success)
    {
        Record(success ? 1 : 0);
    }

private:
    void FinishUnknown() { Record(-1); }
    void Record(int outcome)
    {
        if (slot_ == nullptr) return;
        const uint64_t ns = detail::NowNs() - start_;
        detail::Record(*slot_, ns, bytes_, outcome);
        slot_ = nullptr;
    }
    detail::Slot *slot_;
    uint64_t bytes_, start_;
};
}  // namespace kvtest_profile
#endif
