#include "test_harness.h"
#include "pipeline/mooncake_client.h"
#include "pipeline/mooncake_pipeline.h"
#include "common/config.h"
#include "metrics/metrics.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>

class FakeMooncakeClient final : public MooncakeKvClient {
public:
    bool fail = false;
    int puts = 0;
    int gets = 0;
    std::string value = "test";
    bool Put(const std::string &, const std::string &data) override {
        ++puts;
        if (fail) return false;
        value = data;
        return true;
    }
    bool Get(const std::string &, MooncakeReadResult &out) override {
        ++gets;
        if (fail) return false;
        auto owner = std::make_shared<std::string>(value);
        out.data = owner->data();
        out.size = owner->size();
        out.owner = owner;
        return true;
    }
};

static Config MooncakeTestConfig() {
    Config cfg;
    cfg.outputDir = "/tmp/kvtest-mooncake-pipeline-" + std::to_string(getpid());
    std::filesystem::create_directories(cfg.outputDir);
    return cfg;
}

TEST(MooncakeNativeOperationsUseOriginalExecutor) {
    auto cfg = MooncakeTestConfig();
    auto client = std::make_shared<FakeMooncakeClient>();
    auto backend = CreateMooncakePipeline(cfg, client);
    ASSERT_FALSE(backend->GetOp("setStringView"));
    ASSERT_FALSE(backend->GetOp("getBuffer"));
    MetricsCollector metrics(0, 1000, cfg.outputDir);
    metrics.EnableMooncakeMetrics();
    PipelineContext ctx;
    ctx.key = "key";
    ctx.data = "test";
    ctx.size = 4;
    std::atomic<uint64_t> failures{0};
    SetKvtestClientInitialized(false);
    ASSERT_TRUE(ExecutePipeline({{"put", backend->GetOp("put")},
                                {"get_buffer", backend->GetOp("get_buffer")}},
                               ctx, metrics, failures, 0));
    ASSERT_EQ(client->puts, 1);
    ASSERT_EQ(client->gets, 1);
    ASSERT_EQ(failures.load(), 0u);
    backend.reset();
    std::ifstream samples(cfg.outputDir + "/api_latency_samples.csv");
    std::string line;
    int rows = 0;
    while (std::getline(samples, line)) ++rows;
    ASSERT_EQ(rows, 3);
    std::ifstream status(cfg.outputDir + "/api_latency_samples.status.txt");
    const std::string exported((std::istreambuf_iterator<char>(status)), std::istreambuf_iterator<char>());
    ASSERT_TRUE(exported.find("\"written_samples\":2") != std::string::npos);
    ASSERT_TRUE(exported.find("\"dropped_samples\":0") != std::string::npos);
    std::filesystem::remove_all(cfg.outputDir);
}

TEST(MooncakeFailureRetainsPipelineShortCircuit) {
    auto cfg = MooncakeTestConfig();
    auto client = std::make_shared<FakeMooncakeClient>();
    client->fail = true;
    auto backend = CreateMooncakePipeline(cfg, client);
    MetricsCollector metrics(0, 1000, cfg.outputDir);
    metrics.EnableMooncakeMetrics();
    PipelineContext ctx;
    ctx.key = "key";
    ctx.data = "test";
    ctx.size = 4;
    std::atomic<uint64_t> failures{0};
    ASSERT_FALSE(ExecutePipeline({{"put", backend->GetOp("put")},
                                 {"get_buffer", backend->GetOp("get_buffer")}},
                                ctx, metrics, failures, 0));
    ASSERT_EQ(client->puts, 1);
    ASSERT_EQ(client->gets, 0);
    backend.reset();
    std::ifstream samples(cfg.outputDir + "/api_latency_samples.csv");
    std::string header, row;
    std::getline(samples, header);
    std::getline(samples, row);
    ASSERT_TRUE(row.find("put,") == 0);
    ASSERT_TRUE(row.substr(row.rfind(",") + 1) != "0");
    std::filesystem::remove_all(cfg.outputDir);
}

TEST(MooncakeReadVerificationUsesOriginalPatternRules) {
    auto cfg = MooncakeTestConfig();
    auto client = std::make_shared<FakeMooncakeClient>();
    client->value = "short";
    auto backend = CreateMooncakePipeline(cfg, client);
    MetricsCollector metrics(0, 1000, cfg.outputDir);
    metrics.EnableMooncakeMetrics();
    PipelineContext ctx;
    ctx.key = "key";
    ctx.size = 8;
    ctx.verifyCfg.failOp = true;
    std::atomic<uint64_t> failures{0};
    ASSERT_FALSE(ExecutePipeline({{"get_buffer", backend->GetOp("get_buffer")}},
                                ctx, metrics, failures, 0));
    ASSERT_EQ(failures.load(), 1u);
    backend.reset();
    std::filesystem::remove_all(cfg.outputDir);
}

TEST(MooncakeConfigDoesNotChangeDataSystemDefaults) {
    auto cfg = MooncakeTestConfig();
    const auto configFile = cfg.outputDir + "/config.json";
    {
        std::ofstream config(configFile);
        config << R"({"backend":"mooncake","mode":"pipeline","pipeline":["put"],
            "notify_pipeline":["get_buffer"],"mooncake":{"master_address":"master:50051",
            "pod_ip":"127.0.0.1","transport":"tcp","local_buffer_size":"2GB"}})";
    }
    ASSERT_TRUE(LoadConfig(configFile, cfg, cfg.outputDir));
    ASSERT_EQ(cfg.backend, "mooncake");
    ASSERT_EQ(cfg.mooncakeLocalBufferSize, 2ULL * 1024 * 1024 * 1024);
    ASSERT_EQ(Config().backend, "datasystem");
    {
        std::ofstream config(configFile);
        config << R"({"backend":"mooncake","mode":"pipeline","pipeline":["setBuffer"],
            "notify_pipeline":["get_buffer"],"mooncake":{"master_address":"master:50051",
            "pod_ip":"127.0.0.1","transport":"tcp"}})";
    }
    ASSERT_FALSE(LoadConfig(configFile, cfg, cfg.outputDir));
    std::filesystem::remove_all(cfg.outputDir);
}

int main() { RUN_TESTS(); }
