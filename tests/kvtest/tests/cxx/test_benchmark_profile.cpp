#include "datasystem/common/metrics/benchmark_profile.h"
#include "vendor/nlohmann_json.hpp"

#include <cstdlib>
#define REQUIRE(condition) do { if (!(condition)) std::abort(); } while (false)
#include <fstream>
#include <vector>

int main(int argc, char **argv)
{
    const bool enabled = argc == 2 && std::string(argv[1]) == "enabled";
    const std::string prefix = "/tmp/kvtest-stage-profile-test-" + std::to_string(getpid());
    setenv("KVTEST_STAGE_PROFILE", enabled ? "1" : "0", 1);
    setenv("KVTEST_STAGE_PROFILE_OUTPUT", prefix.c_str(), 1);
    const std::string path = prefix + "." + std::to_string(getpid()) + ".log";
    REQUIRE(kvtest_profile::Enabled() == enabled);
    {
        kvtest_profile::Span ignored(kvtest_profile::Stage::COUNT);
        ignored.Finish(true);
    }
    REQUIRE(access(path.c_str(), F_OK) != 0);
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
    if (!enabled) {
        REQUIRE(access(path.c_str(), F_OK) != 0);
        return 0;
    }
    kvtest_profile::detail::GetRecorder()->Stop();
    std::ifstream input(path);
    REQUIRE(input.good());
    std::string line;
    size_t stages = 0;
    while (std::getline(input, line)) {
        auto row = nlohmann::json::parse(line.substr(line.find('{')));
        REQUIRE(row["final"] == true);
        if (row["phase"] == "ds.rpc.create") {
            REQUIRE(row["count"] == 32000);
            REQUIRE(row["sum_ns"] == 48000000);
            REQUIRE(row["failed"] == 3200);
            REQUIRE(row["unknown"] == 0);
            REQUIRE(row["max_ns"] == 1500);
            REQUIRE(row["buckets"][4] == 32000);
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
        } else {
            REQUIRE(row["phase"] == "ds.api.set");
            REQUIRE(row["count"] == 1);
            REQUIRE(row["unknown"] == 1);
        }
        ++stages;
    }
    REQUIRE(stages == 5);
    kvtest_profile::RecordDuration(kvtest_profile::Stage::DS_RPC_QUERY, 1000, true);
    std::remove(path.c_str());
    return 0;
}
