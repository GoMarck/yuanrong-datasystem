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

#include "datasystem/common/rdma/npu/hixl_transport.h"

#include <cstdlib>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "datasystem/common/flags/flags.h"
#include "datasystem/common/metrics/kv_metrics.h"
#include "datasystem/common/util/raii.h"

DS_DECLARE_string(remote_h2d_hccs_buffer_pool);
DS_DECLARE_bool(log_monitor);
DS_DECLARE_bool(json_log_monitor);

namespace datasystem {
namespace {

struct FakeHixlState {
    int createCalls = 0;
    int initializeCalls = 0;
    int finalizeCalls = 0;
    int destroyCalls = 0;
    int connectCalls = 0;
    int32_t connectTimeoutMs = 0;
    int disconnectCalls = 0;
    int failInitializeAt = 0;
    bool failTransfer = false;
    std::string bufferPool;
    std::string globalResourceConfig;
};

FakeHixlState g_state;

DsHixlResult CreateEngine(DsHixlEngineHandle *engine)
{
    if (engine == nullptr) {
        return DS_HIXL_INVALID_ARGUMENT;
    }
    ++g_state.createCalls;
    *engine = reinterpret_cast<DsHixlEngineHandle>(new int(g_state.createCalls));
    return DS_HIXL_OK;
}

DsHixlResult FinalizeEngine(DsHixlEngineHandle)
{
    ++g_state.finalizeCalls;
    return DS_HIXL_OK;
}

DsHixlResult DestroyEngine(DsHixlEngineHandle engine)
{
    ++g_state.destroyCalls;
    delete reinterpret_cast<int *>(engine);
    return DS_HIXL_OK;
}

DsHixlResult InitializeEngine(DsHixlEngineHandle, DsHixlStringView, const DsHixlOption *options, uint32_t optionCount,
                              uint32_t *)
{
    ++g_state.initializeCalls;
    for (uint32_t i = 0; i < optionCount; ++i) {
        std::string key(options[i].key.data, options[i].key.size);
        if (key == "BufferPool") {
            g_state.bufferPool.assign(options[i].value.data, options[i].value.size);
        } else if (key == "GlobalResourceConfig") {
            g_state.globalResourceConfig.assign(options[i].value.data, options[i].value.size);
        }
    }
    return g_state.initializeCalls == g_state.failInitializeAt ? DS_HIXL_RUNTIME_ERROR : DS_HIXL_OK;
}

DsHixlResult ConnectEngine(DsHixlEngineHandle, DsHixlStringView, int32_t timeoutMs, uint32_t *)
{
    ++g_state.connectCalls;
    g_state.connectTimeoutMs = timeoutMs;
    return DS_HIXL_OK;
}

DsHixlResult DisconnectEngine(DsHixlEngineHandle, DsHixlStringView, int32_t, uint32_t *)
{
    ++g_state.disconnectCalls;
    return DS_HIXL_OK;
}

DsHixlResult RegisterMemory(DsHixlEngineHandle, const DsHixlRegisterMemoryRequest *, DsHixlMemHandle *handle,
                            uint32_t *)
{
    *handle = reinterpret_cast<DsHixlMemHandle>(static_cast<uintptr_t>(1));
    return DS_HIXL_OK;
}

DsHixlResult DeregisterMemory(DsHixlEngineHandle, DsHixlMemHandle, uint32_t *)
{
    return DS_HIXL_OK;
}

DsHixlResult TransferSync(DsHixlEngineHandle, const DsHixlTransferRequest *, uint32_t *)
{
    return g_state.failTransfer ? DS_HIXL_INVALID_ARGUMENT : DS_HIXL_OK;
}

const DsHixlApi FAKE_API = {
    DS_HIXL_ABI_VERSION_1,
    sizeof(DsHixlApi),
    CreateEngine,
    FinalizeEngine,
    DestroyEngine,
    InitializeEngine,
    ConnectEngine,
    DisconnectEngine,
    RegisterMemory,
    DeregisterMemory,
    TransferSync,
};

class HixlTransportPluginTest : public testing::Test {
protected:
    void SetUp() override
    {
        g_state = {};
        savedBufferPool_ = FLAGS_remote_h2d_hccs_buffer_pool;
        const char *roceEnabled = std::getenv("HCCL_INTRA_ROCE_ENABLE");
        hadRoceEnabled_ = roceEnabled != nullptr;
        if (hadRoceEnabled_) {
            savedRoceEnabled_ = roceEnabled;
        }
    }

    void TearDown() override
    {
        FLAGS_remote_h2d_hccs_buffer_pool = savedBufferPool_;
        if (hadRoceEnabled_) {
            ASSERT_EQ(setenv("HCCL_INTRA_ROCE_ENABLE", savedRoceEnabled_.c_str(), 1), 0);
        } else {
            ASSERT_EQ(unsetenv("HCCL_INTRA_ROCE_ENABLE"), 0);
        }
    }

    std::string savedBufferPool_;
    bool hadRoceEnabled_ = false;
    std::string savedRoceEnabled_;
};

TEST_F(HixlTransportPluginTest, WriteMetricsCountCallsAndOnlySuccessfulBytes)
{
    const bool monitor = FLAGS_log_monitor;
    const bool jsonMonitor = FLAGS_json_log_monitor;
    Raii restore([monitor, jsonMonitor]() {
        FLAGS_log_monitor = monitor;
        FLAGS_json_log_monitor = jsonMonitor;
        metrics::ResetKvMetricsForTest();
    });
    FLAGS_log_monitor = true;
    FLAGS_json_log_monitor = false;
    metrics::ResetKvMetricsForTest();
    ASSERT_TRUE(metrics::InitKvMetrics().IsOk());
    constexpr uintptr_t arena = 0x200000;
    constexpr uint64_t arenaBytes = 2 * 1024 * 1024;
    constexpr uint64_t payloadBytes = 16;
    HixlTransport transport(&FAKE_API, true);
    transport.SetLocalEndpoint("127.0.0.1", true);
    ASSERT_TRUE(transport.Init({ 0 }, { "4:8", false }).IsOk());
    ASSERT_TRUE(transport.PreRegisterDeviceMemory({ reinterpret_cast<void *>(arena) }, { arenaBytes }).IsOk());
    const std::vector<DsHixlTransferDesc> descriptors{ { arena, arena + arenaBytes, payloadBytes } };
    ASSERT_TRUE(transport.WriteBatch("127.0.0.2:30000", descriptors, 1000).IsOk());
    g_state.failTransfer = true;
    EXPECT_TRUE(transport.WriteBatch("127.0.0.2:30000", descriptors, 1000).IsError());
    std::string summary;
    for (const auto &line : metrics::DumpSummariesForTest()) {
        summary += line;
    }
    EXPECT_NE(summary.find("\"name\":\"hixl_d2rh_write_request_total\",\"total\":2"), std::string::npos);
    EXPECT_NE(summary.find("\"name\":\"hixl_d2rh_write_error_total\",\"total\":1"), std::string::npos);
    EXPECT_NE(summary.find("\"name\":\"hixl_d2rh_write_success_bytes\",\"total\":16"), std::string::npos);
    EXPECT_NE(summary.find("\"name\":\"hixl_d2rh_write_latency\",\"total\":{\"count\":2"), std::string::npos);
}

TEST_F(HixlTransportPluginTest, RoceEnvironmentDoesNotOverrideBufferPool)
{
    FLAGS_remote_h2d_hccs_buffer_pool = "4:8";
    ASSERT_EQ(setenv("HCCL_INTRA_ROCE_ENABLE", "1", 1), 0);

    HixlTransport transport(&FAKE_API, true);
    transport.SetLocalEndpoint("127.0.0.1");
    ASSERT_TRUE(transport.Init({ 0 }).IsOk());
    EXPECT_EQ(g_state.bufferPool, "4:8");
}

TEST_F(HixlTransportPluginTest, RollsBackAllDevicesWhenInitializationFails)
{
    g_state.failInitializeAt = 2;
    HixlTransport transport(&FAKE_API, true);
    transport.SetLocalEndpoint("127.0.0.1");
    Status status = transport.Init({ 0, 1 });
    EXPECT_EQ(status.GetCode(), K_RUNTIME_ERROR);
    EXPECT_EQ(g_state.createCalls, 2);
    EXPECT_EQ(g_state.initializeCalls, 2);
    EXPECT_EQ(g_state.finalizeCalls, 2);
    EXPECT_EQ(g_state.destroyCalls, 2);

    g_state.failInitializeAt = 0;
    EXPECT_TRUE(transport.Init({ 0, 1 }).IsOk());
    EXPECT_TRUE(transport.DisconnectAll().IsOk());
    EXPECT_EQ(g_state.createCalls, 4);
    EXPECT_EQ(g_state.destroyCalls, 4);
}

TEST_F(HixlTransportPluginTest, RejectsDuplicateDeviceAndRollsBackFirstEngine)
{
    HixlTransport transport(&FAKE_API, true);
    transport.SetLocalEndpoint("127.0.0.1");
    Status status = transport.Init({ 0, 0 });
    EXPECT_EQ(status.GetCode(), K_INVALID);
    EXPECT_EQ(g_state.createCalls, 1);
    EXPECT_EQ(g_state.finalizeCalls, 1);
    EXPECT_EQ(g_state.destroyCalls, 1);
}

TEST_F(HixlTransportPluginTest, ConnectionAndCleanupAreIdempotent)
{
    HixlTransport transport(&FAKE_API, true);
    transport.SetLocalEndpoint("127.0.0.1");
    ASSERT_TRUE(transport.Init({ 0 }).IsOk());
    std::function<int()> heartbeat;
    ASSERT_TRUE(transport.Connect("127.0.0.1:12345", P2P_RECEIVER, &heartbeat).IsOk());
    ASSERT_TRUE(transport.Connect("127.0.0.1:12345", P2P_RECEIVER, &heartbeat).IsOk());
    EXPECT_EQ(g_state.connectCalls, 1);

    EXPECT_TRUE(transport.DisconnectAll().IsOk());
    EXPECT_TRUE(transport.DisconnectAll().IsOk());
    EXPECT_EQ(g_state.disconnectCalls, 1);
    EXPECT_EQ(g_state.finalizeCalls, 1);
    EXPECT_EQ(g_state.destroyCalls, 1);
}

TEST_F(HixlTransportPluginTest, ExplicitOptionsDoNotMutateProcessFlags)
{
    FLAGS_remote_h2d_hccs_buffer_pool = "4:8";
    HixlTransport first(&FAKE_API, true);
    first.SetLocalEndpoint("127.0.0.1", true);
    ASSERT_TRUE(first.Init({ 0 }, { "2:16", false }).IsOk());
    EXPECT_EQ(g_state.bufferPool, "2:16");
    EXPECT_EQ(FLAGS_remote_h2d_hccs_buffer_pool, "4:8");
    HixlTransport second(&FAKE_API, true);
    second.SetLocalEndpoint("127.0.0.1", true);
    ASSERT_TRUE(second.Init({ 0 }, { "3:32", false }).IsOk());
    EXPECT_EQ(g_state.bufferPool, "3:32");
    EXPECT_EQ(FLAGS_remote_h2d_hccs_buffer_pool, "4:8");
}

TEST_F(HixlTransportPluginTest, ConnectHonorsCallerBudget)
{
    HixlTransport transport(&FAKE_API, true);
    transport.SetLocalEndpoint("127.0.0.1");
    ASSERT_TRUE(transport.Init({ 0 }).IsOk());
    ASSERT_TRUE(transport.Connect("127.0.0.1:12345", 100).IsOk());
    EXPECT_EQ(g_state.connectTimeoutMs, 100);
    EXPECT_EQ(transport.Connect("127.0.0.1:12346", 0).GetCode(), K_RPC_DEADLINE_EXCEEDED);
    EXPECT_EQ(g_state.connectCalls, 1);
}

TEST_F(HixlTransportPluginTest, ClientUsesAllocatedDeviceCommPort)
{
    HixlTransport transport(&FAKE_API, true);
    transport.SetLocalEndpoint("127.0.0.1", true);
    ASSERT_TRUE(transport.Init({ 0 }, { "4:8", false }).IsOk());
    std::string endpoint;
    ASSERT_TRUE(transport.GetConnectionIdentity(&endpoint).IsOk());
    const auto port = endpoint.substr(endpoint.rfind(':') + 1);
    EXPECT_NE(port, "16666");
    EXPECT_EQ(g_state.globalResourceConfig, "{\"comm_resource_config.listen_port\":\"" + port + "\"}");
}

TEST_F(HixlTransportPluginTest, WorkerKeepsFixedDeviceCommPort)
{
    HixlTransport transport(&FAKE_API, true);
    transport.SetLocalEndpoint("127.0.0.1");
    ASSERT_TRUE(transport.Init({ 0 }, { "4:8", false }).IsOk());
    EXPECT_EQ(g_state.globalResourceConfig, "{\"comm_resource_config.listen_port\":\"26666\"}");
}

TEST_F(HixlTransportPluginTest, InvalidExplicitPoolDoesNotCreateEngine)
{
    HixlTransport transport(&FAKE_API, true);
    transport.SetLocalEndpoint("127.0.0.1");
    EXPECT_EQ(transport.Init({ 0 }, { "4:0", false }).GetCode(), K_INVALID);
    EXPECT_EQ(g_state.createCalls, 0);
}

TEST_F(HixlTransportPluginTest, SixteenWorkerDevicesShareRoundRobinEndpoints)
{
    constexpr int32_t deviceCount = 16;
    std::vector<int32_t> devices;
    for (int32_t device = 0; device < deviceCount; ++device) {
        devices.push_back(device);
    }
    HixlTransport transport(&FAKE_API, true);
    transport.SetLocalEndpoint("127.0.0.1");
    ASSERT_TRUE(transport.Init(devices, { "4:8", false }).IsOk());
    std::vector<std::string> endpoints;
    for (int32_t device = 0; device < deviceCount; ++device) {
        std::string endpoint;
        ASSERT_TRUE(transport.GetConnectionIdentity(&endpoint).IsOk());
        endpoints.push_back(endpoint);
    }
    EXPECT_EQ(std::set<std::string>(endpoints.begin(), endpoints.end()).size(), devices.size());
    for (const auto &expected : endpoints) {
        std::string endpoint;
        ASSERT_TRUE(transport.GetConnectionIdentity(&endpoint).IsOk());
        EXPECT_EQ(endpoint, expected);
    }
    EXPECT_EQ(g_state.createCalls, deviceCount);
    ASSERT_TRUE(transport.DisconnectAll().IsOk());
    EXPECT_EQ(g_state.finalizeCalls, deviceCount);
    EXPECT_EQ(g_state.destroyCalls, deviceCount);
}

}  // namespace
}  // namespace datasystem
