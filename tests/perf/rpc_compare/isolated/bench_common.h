// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace rpc_isolation {
using Clock = std::chrono::steady_clock;
inline uint64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

struct Config {
    std::string mode = "client", host = "127.0.0.1";
    int port = 18620, threads = 4, ioThreads = 4, serverThreads = 4;
    int requestBytes = 128, responseBytes = 256, warmupSeconds = 10, measureSeconds = 60;
    int timeoutMs = 3000;
    double ratePerThread = 51;
};

inline Config Parse(int argc, char **argv)
{
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string key(argv[i]);
        if (++i == argc) throw std::invalid_argument("missing option value");
        std::string value(argv[i]);
        if (key == "--mode") c.mode = value;
        else if (key == "--host") c.host = value;
        else if (key == "--port") c.port = std::stoi(value);
        else if (key == "--threads") c.threads = std::stoi(value);
        else if (key == "--io-threads") c.ioThreads = std::stoi(value);
        else if (key == "--server-threads") c.serverThreads = std::stoi(value);
        else if (key == "--request-bytes") c.requestBytes = std::stoi(value);
        else if (key == "--response-bytes") c.responseBytes = std::stoi(value);
        else if (key == "--warmup-seconds") c.warmupSeconds = std::stoi(value);
        else if (key == "--measure-seconds") c.measureSeconds = std::stoi(value);
        else if (key == "--rate-per-thread") c.ratePerThread = std::stod(value);
        else if (key == "--timeout-ms") c.timeoutMs = std::stoi(value);
        else throw std::invalid_argument("unknown option: " + key);
    }
    constexpr int MAX_THREADS = 64, MAX_BYTES = 1048576, MAX_SECONDS = 3600;
    if ((c.mode != "client" && c.mode != "server") || c.port <= 0 || c.port > 65535 || c.threads <= 0
        || c.threads > MAX_THREADS || c.ioThreads <= 0 || c.ioThreads > MAX_THREADS || c.serverThreads <= 0
        || c.serverThreads > MAX_THREADS || c.requestBytes < 0 || c.requestBytes > MAX_BYTES
        || c.responseBytes < 0 || c.responseBytes > MAX_BYTES || c.warmupSeconds < 0
        || c.measureSeconds <= 0 || c.measureSeconds > MAX_SECONDS || !std::isfinite(c.ratePerThread)
        || c.ratePerThread < 0 || c.ratePerThread > 1000000 || c.timeoutMs <= 0)
        throw std::invalid_argument("invalid configuration");
    return c;
}

struct CallResult {
    bool success = false;
    uint64_t nativeNs = 0, handlerNs = 0;
};
struct Sample {
    uint64_t nativeNs, envelopeNs, handlerNs;
};
struct Stats {
    uint64_t attempts = 0, failed = 0, lastCompletionNs = 0;
    bool setupFailed = false, overflow = false;
    std::vector<Sample> samples;
};

inline double Quantile(const std::vector<uint64_t> &sorted, double p)
{
    if (sorted.empty()) return 0;
    const size_t index = static_cast<size_t>(std::ceil(p * sorted.size())) - 1;
    return sorted[std::min(index, sorted.size() - 1)] / 1000.0;
}

template <typename Factory>
int Run(const Config &c, const char *backend, Factory factory)
{
    constexpr size_t MAX_SAMPLES_PER_THREAD = 4000000, INITIAL_SAMPLES = 100000;
    constexpr uint64_t NS_PER_SECOND = 1000000000ULL;
    std::atomic<int> ready{0};
    std::atomic<uint64_t> startNs{0};
    std::vector<Stats> stats(c.threads);
    std::vector<std::thread> threads;
    for (int i = 0; i < c.threads; ++i) {
        threads.emplace_back([&, i] {
            auto &s = stats[i];
            s.samples.reserve(INITIAL_SAMPLES);
            decltype(factory(i)) client;
            try { client = factory(i); }
            catch (const std::exception &e) { std::cerr << "connect: " << e.what() << '\n'; s.setupFailed = true; }
            ready.fetch_add(1, std::memory_order_release);
            while (startNs.load(std::memory_order_acquire) == 0) std::this_thread::yield();
            if (s.setupFailed) return;
            const uint64_t begin = startNs.load(std::memory_order_relaxed);
            const uint64_t measureBegin = begin + c.warmupSeconds * NS_PER_SECOND;
            const uint64_t end = measureBegin + c.measureSeconds * NS_PER_SECOND;
            const uint64_t period = c.ratePerThread > 0 ? NS_PER_SECOND / c.ratePerThread : 0;
            uint64_t due = begin;
            for (;;) {
                if (period != 0) std::this_thread::sleep_until(Clock::time_point(std::chrono::nanoseconds(due)));
                const uint64_t t0 = NowNs();
                if (t0 >= end) break;
                const auto result = client->Call();
                const uint64_t t1 = NowNs();
                if (t0 >= measureBegin) {
                    ++s.attempts;
                    if (!result.success) ++s.failed;
                    s.lastCompletionNs = t1;
                    if (s.samples.size() >= MAX_SAMPLES_PER_THREAD) { s.overflow = true; break; }
                    s.samples.push_back({result.nativeNs, t1 - t0, result.handlerNs});
                }
                if (period != 0) due = std::max(due + period, t1);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != c.threads) std::this_thread::yield();
    startNs.store(NowNs(), std::memory_order_release);
    for (auto &thread : threads) thread.join();
    uint64_t attempted = 0, failed = 0, nativeSum = 0, envelopeSum = 0, handlerSum = 0, last = 0;
    bool invalid = false;
    std::vector<uint64_t> native, envelope;
    for (const auto &s : stats) {
        attempted += s.attempts; failed += s.failed; last = std::max(last, s.lastCompletionNs);
        invalid = invalid || s.setupFailed || s.overflow;
        for (const auto &sample : s.samples) {
            nativeSum += sample.nativeNs; envelopeSum += sample.envelopeNs; handlerSum += sample.handlerNs;
            native.push_back(sample.nativeNs); envelope.push_back(sample.envelopeNs);
        }
    }
    std::sort(native.begin(), native.end()); std::sort(envelope.begin(), envelope.end());
    const double n = native.size();
    const uint64_t begin = startNs.load() + c.warmupSeconds * NS_PER_SECOND;
    const double elapsed = std::max<double>(c.measureSeconds, last > begin ? (last - begin) / 1e9 : 0);
    const auto mean = [n](uint64_t value) { return n > 0 ? value / n / 1000.0 : 0; };
    std::cout << "[RPC_ISOLATION_RESULT] {\"backend\":\"" << backend << "\",\"threads\":" << c.threads
              << ",\"io_threads\":" << c.ioThreads << ",\"server_threads\":" << c.serverThreads
              << ",\"request_bytes\":" << c.requestBytes << ",\"response_bytes\":" << c.responseBytes
              << ",\"warmup_seconds\":" << c.warmupSeconds << ",\"measure_seconds\":" << elapsed
              << ",\"rate_per_thread\":" << c.ratePerThread << ",\"attempted\":" << attempted
              << ",\"timeout_ms\":" << c.timeoutMs
              << ",\"failed\":" << failed << ",\"samples\":" << native.size()
              << ",\"invalid\":" << (invalid ? "true" : "false") << ",\"qps\":" << attempted / elapsed
              << ",\"native_mean_us\":" << mean(nativeSum) << ",\"handler_mean_us\":" << mean(handlerSum)
              << ",\"native_minus_handler_us\":" << mean(nativeSum) - mean(handlerSum)
              << ",\"envelope_mean_us\":" << mean(envelopeSum)
              << ",\"native_p50_us\":" << Quantile(native, .50) << ",\"native_p99_us\":" << Quantile(native, .99)
              << ",\"envelope_p99_us\":" << Quantile(envelope, .99) << "}" << std::endl;
    return invalid || failed != 0 || native.empty() || native.size() != attempted ? 1 : 0;
}
}  // namespace rpc_isolation
