/** Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved. */

#include "common/client_config.h"
#include "pipeline/mooncake_client.h"
#include "vendor/nlohmann_json.hpp"

#include <datasystem/utils/service_discovery.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
using namespace datasystem;

namespace {
uint64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

std::string Key(const std::string &prefix, uint64_t index, size_t bytes)
{
    std::ostringstream stream;
    stream << prefix << '-' << std::setw(16) << std::setfill('0') << std::hex << index;
    auto key = stream.str();
    if (key.size() > bytes) throw std::runtime_error("key_bytes is too small for the unique prefix and index");
    key.resize(bytes, 'k');
    return key;
}

struct Result {
    std::vector<uint64_t> latency;
    uint64_t failed{0};
    uint64_t verifyFailed{0};
    bool overflow{false};
};

struct Client {
    std::shared_ptr<IServiceDiscovery> discovery;
    std::shared_ptr<KVClient> ds;
    std::shared_ptr<MooncakeKvClient> mc;

    explicit Client(const Config &cfg)
    {
        if (cfg.backend == "mooncake") {
#ifdef KVTEST_WITH_MOONCAKE
            mc = CreateMooncakeClient(cfg);
#else
            throw std::runtime_error("Mooncake support was not built");
#endif
            return;
        }
        if (cfg.coordinatorAddress.empty() || !cfg.jfAddress.empty()) {
            throw std::runtime_error("table probe requires direct coordinator discovery for DataSystem");
        }
        CoordinatorServiceDiscoveryOptions options;
        options.serviceAddress = cfg.coordinatorAddress;
        options.clusterName = cfg.clusterName;
        options.hostIdEnvName = cfg.hostIdEnvName;
        discovery = std::make_shared<CoordinatorServiceDiscovery>(options);
        auto status = discovery->Init();
        if (!status.IsOk()) throw std::runtime_error(status.ToString());
        ConnectOptions conn;
        conn.serviceDiscovery = discovery;
        conn.connectTimeoutMs = cfg.connectTimeoutMs;
        conn.requestTimeoutMs = cfg.requestTimeoutMs;
        conn.enableCrossNodeConnection = cfg.enableCrossNodeConnection;
        conn.enableLocalCache = cfg.enableLocalCache;
        conn.dataPlacementPolicy = cfg.dataPlacementPolicy;
        conn.fastTransportMemSize = cfg.fastTransportMemSize;
        ds = std::make_shared<KVClient>(conn);
        status = InitKvtestClient(cfg, *ds);
        if (!status.IsOk()) throw std::runtime_error(status.ToString());
    }

    bool Put(const std::string &key, const std::string &value)
    {
        if (mc) return mc->Put(key, value);
        SetParam param;
        param.writeMode = WriteMode::NONE_L2_CACHE_EVICT;
        param.ttlSecond = 0;
        return ds->Set(key, StringView(value), param).IsOk();
    }

    bool Get(const std::string &key, const std::string &value, uint64_t &latency, bool &verified)
    {
        // Match KVTest: time the API; verify and release the returned owner afterwards.
        if (mc) {
            MooncakeReadResult result;
            const auto start = NowNs();
            const bool ok = mc->Get(key, result);
            latency = NowNs() - start;
            verified = ok && result.data && result.size == value.size()
                       && std::memcmp(result.data, value.data(), value.size()) == 0;
            return ok;
        }
        Optional<Buffer> result;
        const auto start = NowNs();
        const auto status = ds->Get(key, result);
        latency = NowNs() - start;
        verified = status.IsOk() && result && result->GetSize() == static_cast<int64_t>(value.size())
                   && std::memcmp(result->ImmutableData(), value.data(), value.size()) == 0;
        return status.IsOk();
    }
};

void Phase(Client &client, const std::vector<std::string> &keys, const std::string &value,
           unsigned threads, unsigned seconds, bool hot, unsigned warmup, bool failOnApiError, const Json &identity)
{
    std::atomic<unsigned> ready{0};
    std::atomic<uint64_t> startNs{0};
    std::vector<Result> results(threads);
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            auto &result = results[t];
            result.latency.reserve(100000);
            ready.fetch_add(1);
            uint64_t start;
            while ((start = startNs.load(std::memory_order_acquire)) == 0) std::this_thread::yield();
            const auto measured = start + static_cast<uint64_t>(warmup) * 1000000000ULL;
            const auto end = measured + static_cast<uint64_t>(seconds) * 1000000000ULL;
            uint64_t random = 0x9e3779b97f4a7c15ULL ^ (t + 1);
            while (NowNs() < end) {
                random ^= random << 13; random ^= random >> 7; random ^= random << 17;
                const size_t index = hot ? 0 : random % keys.size();
                const bool record = NowNs() >= measured;
                uint64_t latency = 0;
                bool verified = false;
                const bool ok = client.Get(keys[index], value, latency, verified);
                if (record) {
                    if (result.latency.size() == 1000000) { result.overflow = true; break; }
                    result.latency.push_back(latency);
                    result.failed += !ok;
                    result.verifyFailed += ok && !verified;
                }
            }
        });
    }
    while (ready.load() != threads) std::this_thread::yield();
    const auto start = NowNs();
    startNs.store(start, std::memory_order_release);
    for (auto &thread : workers) thread.join();
    std::vector<uint64_t> latency;
    uint64_t failed = 0, verifyFailed = 0, sum = 0;
    for (const auto &result : results) {
        if (result.overflow) throw std::runtime_error("phase exceeded sample capacity; reduce seconds");
        latency.insert(latency.end(), result.latency.begin(), result.latency.end());
        failed += result.failed; verifyFailed += result.verifyFailed;
    }
    for (auto ns : latency) sum += ns;
    std::sort(latency.begin(), latency.end());
    auto percentile = [&](double p) -> double {
        if (latency.empty()) return 0;
        return latency[std::min(latency.size() - 1, static_cast<size_t>(std::ceil(p * latency.size())) - 1)] / 1000.0;
    };
    Json row = identity;
    row.update({{"event", "get_phase"}, {"threads", threads}, {"skew", hot ? "hot_key" : "uniform"},
                {"start_ns", start + static_cast<uint64_t>(warmup) * 1000000000ULL},
                {"end_ns", start + static_cast<uint64_t>(warmup + seconds) * 1000000000ULL},
                {"count", latency.size()}, {"failed", failed}, {"verify_failed", verifyFailed},
                {"mean_us", latency.empty() ? 0 : sum / static_cast<double>(latency.size()) / 1000.0},
                {"p50_us", percentile(.5)}, {"p99_us", percentile(.99)},
                {"qps", latency.size() / static_cast<double>(seconds)}});
    std::cout << "[KVTEST_TABLE_PROBE] " << row.dump() << std::endl;
    if ((failed && failOnApiError) || verifyFailed || latency.empty()) {
        throw std::runtime_error("invalid measured phase");
    }
}
}  // namespace

int main(int argc, char **argv)
{
    try {
        if (argc != 3) throw std::runtime_error("usage: kvtest_table_probe CLIENT_CONFIG PLAN_JSON");
        Config cfg;
        if (!LoadConfig(argv[1], cfg)) return 2;
        std::ifstream input(argv[2]);
        const Json plan = Json::parse(input);
        const uint64_t count = plan.at("resident_keys");
        const uint64_t seedBegin = plan.value("seed_begin", uint64_t{0});
        const unsigned seconds = plan.value("seconds", 20U);
        const unsigned warmup = plan.value("warmup_seconds", 3U);
        const bool failOnApiError = plan.value("fail_on_api_error", true);
        const auto threads = plan.at("threads").get<std::vector<unsigned>>();
        const unsigned seedThreads = plan.value("seed_threads", 4U);
        const std::string prefix = plan.at("prefix");
        const size_t keyBytes = plan.value("key_bytes", size_t{32});
        const size_t valueBytes = plan.value("value_bytes", size_t{1});
        if (!count || count > 1000000 || seedBegin > count || !seconds || seconds > 120
            || warmup > 30 || !seedThreads || seedThreads > 32 || !valueBytes || valueBytes > 8388608 || keyBytes > 1024) {
            throw std::runtime_error("probe plan outside bounded diagnostic limits");
        }
        for (auto t : threads) if (!t || t > 32) throw std::runtime_error("threads must be 1..32");
        std::vector<std::string> keys;
        keys.reserve(count);
        for (uint64_t i = 0; i < count; ++i) keys.emplace_back(Key(prefix, i, keyBytes));
        const std::string value(valueBytes, 'v');
        Client client(cfg);
        const auto seedStart = NowNs();
        std::atomic<uint64_t> next{seedBegin}, seedFailures{0};
        std::vector<std::thread> seeders;
        std::vector<std::vector<uint64_t>> seedLatency(seedThreads);
        for (unsigned t = 0; t < seedThreads; ++t) seeders.emplace_back([&, t] {
            uint64_t i;
            while ((i = next.fetch_add(1)) < count) {
                const auto start = NowNs();
                const bool ok = client.Put(keys[i], value);
                seedLatency[t].push_back(NowNs() - start);
                if (!ok) seedFailures.fetch_add(1);
            }
        });
        for (auto &thread : seeders) thread.join();
        Json identity{{"backend", cfg.backend}, {"resident_keys", count}, {"prefix", prefix},
                      {"key_bytes", keyBytes}, {"value_bytes", valueBytes}, {"ttl_second", 0},
                      {"fail_on_api_error", failOnApiError}};
        std::vector<uint64_t> seedTimes;
        uint64_t seedSum = 0;
        for (const auto &times : seedLatency) for (auto ns : times) { seedSum += ns; seedTimes.push_back(ns); }
        std::sort(seedTimes.begin(), seedTimes.end());
        Json seed = identity;
        seed.update({{"event", "seed"}, {"seed_begin", seedBegin}, {"seed_end", count},
                     {"threads", seedThreads}, {"count", seedTimes.size()},
                     {"mean_us", seedTimes.empty() ? 0 : seedSum / static_cast<double>(seedTimes.size()) / 1000.0},
                     {"p99_us", seedTimes.empty() ? 0 : seedTimes[static_cast<size_t>(std::ceil(.99 * seedTimes.size())) - 1] / 1000.0},
                     {"failed", seedFailures.load()}, {"start_ns", seedStart}, {"end_ns", NowNs()}});
        std::cout << "[KVTEST_TABLE_PROBE] " << seed.dump() << std::endl;
        if (seedFailures.load()) throw std::runtime_error("seeding failed; resident population invalid");
        for (auto t : threads) {
            Phase(client, keys, value, t, seconds, false, warmup, failOnApiError, identity);
            Phase(client, keys, value, t, seconds, true, warmup, failOnApiError, identity);
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "table_probe: " << error.what() << std::endl;
        return 1;
    }
}
