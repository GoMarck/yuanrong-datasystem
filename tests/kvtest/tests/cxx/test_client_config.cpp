#include "test_harness.h"
#include "common/client_config.h"
#include "vendor/nlohmann_json.hpp"

TEST(ClientConfig_NativeDefaultsArePreserved) {
    Config cfg;
    datasystem::KVClient client;
    ASSERT_TRUE(InitKvtestClient(cfg, client).IsOk());
    ASSERT_EQ(client.initCalls, 1);
    ASSERT_TRUE(client.initConfig.empty());
    ASSERT_EQ(client.updateCalls, 0);
}

TEST(ClientConfig_FullLoggingIsAppliedBeforeRequests) {
    Config cfg;
    cfg.fullAccessLog = true;
    cfg.urmaSendLaneCountPerPeer = 8;
    datasystem::KVClient client;
    ASSERT_TRUE(InitKvtestClient(cfg, client).IsOk());
    ASSERT_EQ(client.initConfig.at("log_monitor"), "true");
    ASSERT_EQ(client.initConfig.at("monitor_config_file"), "");
    ASSERT_EQ(client.initConfig.at("urma_send_lane_count_per_peer"), "8");
    ASSERT_EQ(client.initCalls, 1);
    ASSERT_EQ(client.updateCalls, 1);
    const auto flags = nlohmann::json::parse(client.updatedConfig);
    ASSERT_EQ(flags.size(), 3u);
    for (const auto &name : {"request_sample_rate", "access_sample_rate", "diagnostic_sample_rate"}) {
        ASSERT_EQ(flags.at(name).get<std::string>(), "1.0");
    }
}

TEST(ClientConfig_InitFailureSkipsLoggingUpdate) {
    Config cfg;
    cfg.fullAccessLog = true;
    datasystem::KVClient client;
    client.initStatus = datasystem::Status::Error("init failed");
    ASSERT_EQ(InitKvtestClient(cfg, client).GetMsg(), "init failed");
    ASSERT_EQ(client.updateCalls, 0);
}

TEST(ClientConfig_LoggingFailureIsNotSilentlyIgnored) {
    Config cfg;
    cfg.fullAccessLog = true;
    datasystem::KVClient client;
    client.updateStatus = datasystem::Status::Error("sampling flag unsupported");
    ASSERT_EQ(InitKvtestClient(cfg, client).GetMsg(), "sampling flag unsupported");
    ASSERT_EQ(client.updateCalls, 1);
}
