#include "datasystem/common/metrics/benchmark_profile.h"
#include "vendor/nlohmann_json.hpp"

#include <cstdlib>
#define REQUIRE(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: requirement failed: %s\n", __FILE__, __LINE__, #condition); \
    std::abort(); \
} } while (false)
#include <fstream>
#include <glob.h>
#include <vector>

int main(int argc, char **argv)
{
    const bool cpu = argc == 2 && std::string(argv[1]) == "enabled-cpu";
    const bool enabled = cpu || (argc == 2 && std::string(argv[1]) == "enabled");
    const std::string prefix = "/tmp/kvtest-stage-profile-test-" + std::to_string(getpid());
    setenv("KVTEST_STAGE_PROFILE", enabled ? "1" : "0", 1);
    setenv("KVTEST_STAGE_PROFILE_OUTPUT", prefix.c_str(), 1);
    setenv("KVTEST_DETAIL_PROFILE", "1", 1);
    setenv("KVTEST_DETAIL_SAMPLE_EVERY", "1", 1);
    setenv("KVTEST_DETAIL_SAMPLES", "1", 1);
    setenv("KVTEST_DETAIL_CPU", cpu ? "1" : "0", 1);
    auto findLogs = [&prefix]() {
        glob_t found{};
        std::vector<std::string> paths;
        if (glob((prefix + ".*.log").c_str(), 0, nullptr, &found) == 0) {
            for (size_t i = 0; i < found.gl_pathc; ++i) paths.emplace_back(found.gl_pathv[i]);
        }
        globfree(&found);
        return paths;
    };
    REQUIRE(kvtest_profile::Enabled() == enabled);
    REQUIRE(kvtest_profile::RpcMethodId("datasystem.worker.WorkerOCService.QueryAndGet") == 3);
    REQUIRE(kvtest_profile::RpcMethodId("GetReplicaList") == 8);
    REQUIRE(kvtest_profile::RpcMethodId("UnrelatedMethod") == 0);
    REQUIRE(kvtest_profile::RpcMethodId("WorkerOCService.DecreaseReference") == 4);
    {
        kvtest_profile::Span ignored(kvtest_profile::Stage::COUNT);
        ignored.Finish(true);
    }
    REQUIRE(findLogs().empty());
    kvtest_profile::RecordDuration(kvtest_profile::Stage::DS_RPC_NATIVE_QUERY, 5000, false);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < 32; ++i) {
        threads.emplace_back([] {
            for (size_t j = 0; j < 1000; ++j) {
                kvtest_profile::RecordDuration(kvtest_profile::Stage::DS_RPC_CREATE, 1500, j % 10 != 0);
            }
        });
    }
    for (auto &thread : threads) thread.join();
    {
        kvtest_profile::Span span(kvtest_profile::Stage::DS_COPY, 7);
        span.Finish(true);
        span.Finish(false);
    }
    {
        kvtest_profile::Span span(kvtest_profile::Stage::DS_SET);
    }
    {
        kvtest_profile::Span span(kvtest_profile::Stage::COUNT);
        span.Next(kvtest_profile::Stage::DS_GET_ROUTE);
        span.Next(kvtest_profile::Stage::DS_GET_MATERIALIZE);
        span.Finish(true);
        span.Next(kvtest_profile::Stage::COUNT);
    }
    {
        kvtest_profile::DetailFrame frame(kvtest_profile::Stage::D_DS_DETAIL_SET_SDK_TOTAL,
                                          kvtest_profile::Stage::D_DS_DETAIL_SET_SDK_CONTEXT);
        frame.Observe(kvtest_profile::DetailAttribute::TABLE_SIZE, 17);
        frame.Observe(kvtest_profile::DetailAttribute::RPC_METHOD, 3);
        frame.Observe(kvtest_profile::DetailAttribute::REQUEST_BYTES, 128);
        frame.Observe(kvtest_profile::DetailAttribute::RESPONSE_BYTES, 256);
        frame.Observe(kvtest_profile::DetailAttribute::ATTACHMENT_BYTES, 64);
        frame.Next(kvtest_profile::Stage::D_DS_DETAIL_SET_SDK_COPY);
        frame.Finish(1);
        frame.Finish(0);
    }
    if (cpu) {
        auto frame = std::make_unique<kvtest_profile::DetailFrame>(
            kvtest_profile::Stage::D_DS_DETAIL_SET_SDK_TOTAL,
            kvtest_profile::Stage::D_DS_DETAIL_SET_SDK_CONTEXT);
        frame->Observe(kvtest_profile::DetailAttribute::TABLE_SIZE, 99);
        std::thread migrated([frame = std::move(frame)]() mutable { frame->Finish(1); });
        migrated.join();
    }
    if (!enabled) {
        REQUIRE(findLogs().empty());
        return 0;
    }
    kvtest_profile::detail::GetRecorder()->Stop();
    const auto paths = findLogs();
    REQUIRE(paths.size() == 1);
    const auto &path = paths.front();
    std::ifstream input(path);
    REQUIRE(input.good());
    std::string line;
    size_t stages = 0;
    size_t samples = 0;
    while (std::getline(input, line)) {
        auto row = nlohmann::json::parse(line.substr(line.find('{')));
        REQUIRE(row["final"] == true);
        if (line.find("[KVTEST_DETAIL_SAMPLE]") == 0) {
            uint64_t sum = 0;
            for (const auto &step : row["steps"]) sum += step["duration_ns"].get<uint64_t>();
            REQUIRE(sum == row["duration_ns"]);
            const bool migrated = row["attributes"]["table_size"] == 99;
            REQUIRE(row["attributes"]["table_size"] == (migrated ? 99 : 17));
            if (!migrated) {
                REQUIRE(row["attributes"]["rpc_method"] == 3);
                REQUIRE(row["attributes"]["request_bytes"] == 128);
                REQUIRE(row["attributes"]["response_bytes"] == 256);
                REQUIRE(row["attributes"]["attachment_bytes"] == 64);
            }
            REQUIRE(row["steps"].size() == (migrated ? 1 : 2));
            for (const auto &step : row["steps"]) {
                if (cpu) {
                    REQUIRE(step["cpu_valid"] == !migrated);
                    REQUIRE((step["begin_tid"] == step["end_tid"]) == !migrated);
                } else {
                    REQUIRE(!step.contains("cpu_ns"));
                }
            }
            REQUIRE(row["overflow"] == false);
            ++samples;
            continue;
        }
        if (line.find("[KVTEST_DETAIL_STATUS]") == 0) continue;
        if (line.find("[KVTEST_PROFILE_RESOURCE]") == 0) continue;
        if (row["phase"] == "ds.rpc.create") {
            REQUIRE(row["count"] == 32000);
            REQUIRE(row["sum_ns"] == 48000000);
            REQUIRE(row["failed"] == 3200);
            REQUIRE(row["unknown"] == 0);
            REQUIRE(row["max_ns"] == 1500);
            REQUIRE(row["buckets"][4] == 32000);
        } else if (row["phase"] == "ds.rpc_native.query") {
            REQUIRE(row["count"] == 1);
            REQUIRE(row["sum_ns"] == 5000);
            REQUIRE(row["failed"] == 1);
        } else if (row["phase"] == "ds.copy") {
            REQUIRE(row["count"] == 1);
            REQUIRE(row["requested_bytes"] == 7);
            REQUIRE(row["failed"] == 0);
            REQUIRE(row["unknown"] == 0);
        } else if (row["phase"] == "ds.sdk.get.route" || row["phase"] == "ds.sdk.get.materialize") {
            REQUIRE(row["count"] == 1);
            REQUIRE(row["failed"] == 0);
            REQUIRE(row["requested_bytes"] == 0);
            REQUIRE(row["unknown"] == (row["phase"] == "ds.sdk.get.route" ? 1 : 0));
        } else if (row["phase"].get<std::string>().find("ds.detail.") == 0) {
            const bool doubled = cpu && (row["phase"] == "ds.detail.set.sdk.total"
                || row["phase"] == "ds.detail.set.sdk.total.calls"
                || row["phase"] == "ds.detail.set.sdk.context");
            REQUIRE(row["count"] == (doubled ? 2 : 1));
            REQUIRE(row["failed"] == 0);
        } else {
            REQUIRE(row["phase"] == "ds.api.set");
            REQUIRE(row["count"] == 1);
            REQUIRE(row["unknown"] == 1);
        }
        ++stages;
    }
    REQUIRE(stages == 10);
    REQUIRE(samples == (cpu ? 2 : 1));
    kvtest_profile::RecordDuration(kvtest_profile::Stage::DS_RPC_QUERY, 1000, true);
    std::remove(path.c_str());
    return 0;
}
