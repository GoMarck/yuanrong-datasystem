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

#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common.h"
#include "datasystem/common/device/ascend/acl_device_manager.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/log/access_recorder.h"
#include "datasystem/common/util/net_util.h"
#include "datasystem/common/util/uuid_generator.h"
#include "datasystem/hetero_client.h"

namespace datasystem {
namespace st {
class HeteroD2RHTest : public ::testing::Test {
public:
    static void SetUpTestSuite()
    {
        if (std::getenv("DS_D2RH_TEST_WORKER") != nullptr) {
            DS_ASSERT_OK(acl::AclDeviceManager::Instance()->aclInit(nullptr));
        }
    }

protected:
    void SetUp() override
    {
        const char *worker = std::getenv("DS_D2RH_TEST_WORKER");
        if (worker == nullptr) {
            GTEST_SKIP() << "Set DS_D2RH_TEST_WORKER to an external remote Worker";
        }
        HostPort address;
        DS_ASSERT_OK(address.ParseString(worker));
        const char *device = std::getenv("DS_TEST_DEVICE_ID");
        deviceId_ = device == nullptr ? 0 : std::stoi(device);
        manager_ = acl::AclDeviceManager::Instance();
        DS_ASSERT_OK(manager_->SetDeviceIdx(deviceId_));
        options_.host = address.Host();
        options_.port = address.Port();
        options_.requestTimeoutMs = 30000;
        options_.enableLocalCache = false;
        options_.dataPlacementPolicy = DataPlacementPolicy::PREFERRED_META_OWNER;
        const char *accessKey = std::getenv("DS_D2RH_TEST_ACCESS_KEY");
        const char *secretKey = std::getenv("DS_D2RH_TEST_SECRET_KEY");
        if (accessKey != nullptr && secretKey != nullptr) {
            options_.SetAkSkAuth(accessKey, secretKey, "");
        }
        client_ = std::make_unique<HeteroClient>(options_);
        DS_ASSERT_OK(client_->Init());
        DS_ASSERT_OK(manager_->MallocDeviceMemory(arenaSize_, source_));
        DS_ASSERT_OK(manager_->MallocDeviceMemory(arenaSize_, destination_));
        keys_ = { "d2rh_" + GetStringUuid(), "d2rh_" + GetStringUuid() };
        sourceBlobs_ = MakeBlobs(source_);
        destinationBlobs_ = MakeBlobs(destination_);
        FillSource('a');
    }

    void TearDown() override
    {
        inject::Clear("HixlTransport.WriteBatch");
        if (client_ != nullptr) {
            DS_ASSERT_OK(client_->ShutDown());
            client_.reset();
        }
        // A real transfer error may retain device access; keep the fixed arena until this test process exits.
        if (manager_ != nullptr && !transferFailed_) {
            if (source_ != nullptr) { DS_ASSERT_OK(manager_->FreeDeviceMemory(source_)); }
            if (destination_ != nullptr) { DS_ASSERT_OK(manager_->FreeDeviceMemory(destination_)); }
        }
    }

    std::vector<DeviceBlobList> MakeBlobs(void *arena)
    {
        std::vector<DeviceBlobList> objects(2);
        for (size_t i = 0; i < objects.size(); ++i) {
            objects[i].deviceIdx = deviceId_;
            objects[i].blobs = { { static_cast<char *>(arena) + i * objectSize_, blobSize_ },
                                  { static_cast<char *>(arena) + i * objectSize_ + blobSize_, blobSize_ } };
        }
        return objects;
    }

    void RegisterSource()
    {
        DS_ASSERT_OK(client_->PreRegisterDeviceMemory({ source_ }, { arenaSize_ }));
    }

    void FillSource(char value)
    {
        std::string content(arenaSize_, value);
        DS_ASSERT_OK(manager_->MemCopyH2D(source_, arenaSize_, content.data(), content.size()));
    }

    Status Write(const std::vector<DeviceBlobList> &blobs, const SetParam &param = {},
                 std::vector<std::string> *confirmed = nullptr)
    {
        auto rc = client_->MSetD2H(keys_, blobs, param, confirmed);
        transferFailed_ = transferFailed_ || rc.IsError();
        return rc;
    }

    void Verify(char value)
    {
        std::vector<std::string> failed;
        DS_ASSERT_OK(client_->MGetH2D(keys_, destinationBlobs_, failed, 5000));
        ASSERT_TRUE(failed.empty());
        std::string actual(arenaSize_, '\0');
        DS_ASSERT_OK(manager_->MemCopyD2H(&actual[0], actual.size(), destination_, arenaSize_));
        ASSERT_EQ(actual, std::string(arenaSize_, value));
    }

    static constexpr uint64_t blobSize_ = 4096;
    static constexpr uint64_t objectSize_ = blobSize_ * 2;
    static constexpr uint64_t arenaSize_ = objectSize_ * 2;
    acl::AclDeviceManager *manager_ = nullptr;
    int32_t deviceId_ = 0;
    void *source_ = nullptr;
    void *destination_ = nullptr;
    bool transferFailed_ = false;
    ConnectOptions options_;
    std::unique_ptr<HeteroClient> client_;
    std::vector<std::string> keys_;
    std::vector<DeviceBlobList> sourceBlobs_;
    std::vector<DeviceBlobList> destinationBlobs_;
};

TEST_F(HeteroD2RHTest, DISABLED_ConcurrentClientRegistration)
{
    HeteroClient other(options_);
    DS_ASSERT_OK(other.Init());
    auto registerArena = [&](HeteroClient &client, void *arena) {
        auto rc = manager_->SetDeviceIdx(deviceId_);
        return rc.IsError() ? rc : client.PreRegisterDeviceMemory({ arena }, { arenaSize_ });
    };
    auto first = std::async(std::launch::async, [&]() { return registerArena(*client_, source_); });
    auto second = std::async(std::launch::async, [&]() { return registerArena(other, destination_); });
    auto firstRc = first.get();
    auto secondRc = second.get();
    DS_ASSERT_OK(other.ShutDown());
    DS_ASSERT_OK(firstRc);
    DS_ASSERT_OK(secondRc);
}

TEST_F(HeteroD2RHTest, DISABLED_MultiBlobRoundTrip)
{
    RegisterSource();
    std::vector<std::string> confirmed;
    DS_ASSERT_OK(Write(sourceBlobs_, {}, &confirmed));
    ASSERT_NE(AccessTransportTracker::ToString().find("HIXL"), std::string::npos);
    ASSERT_EQ(confirmed.size(), keys_.size());
    Verify('a');
}

TEST_F(HeteroD2RHTest, DISABLED_SameHostRoundTrip)
{
    DS_ASSERT_OK(Write(sourceBlobs_));
    ASSERT_EQ(AccessTransportTracker::ToString(), "SHM");
    Verify('a');
}

TEST_F(HeteroD2RHTest, DISABLED_ReuseRegisteredArena)
{
    RegisterSource();
    for (int i = 0; i < 4; ++i) {
        keys_ = { "d2rh_" + GetStringUuid(), "d2rh_" + GetStringUuid() };
        FillSource(static_cast<char>('a' + i));
        DS_ASSERT_OK(Write(sourceBlobs_));
        ASSERT_NE(AccessTransportTracker::ToString().find("HIXL"), std::string::npos);
        Verify(static_cast<char>('a' + i));
    }
}

TEST_F(HeteroD2RHTest, DISABLED_NxKeepsExistingValue)
{
    RegisterSource();
    DS_ASSERT_OK(Write(sourceBlobs_));
    ASSERT_NE(AccessTransportTracker::ToString().find("HIXL"), std::string::npos);
    FillSource('b');
    SetParam param;
    param.existence = ExistenceOpt::NX;
    std::vector<std::string> confirmed;
    DS_ASSERT_OK(Write(sourceBlobs_, param, &confirmed));
    ASSERT_TRUE(confirmed.empty());
    Verify('a');
}

TEST_F(HeteroD2RHTest, DISABLED_UnregisteredSourceRejected)
{
    RegisterSource();
    auto rc = Write(destinationBlobs_);
    ASSERT_EQ(rc.GetCode(), K_INVALID) << rc.ToString();
    std::vector<std::string> failed;
    ASSERT_TRUE(client_->MGetH2D(keys_, destinationBlobs_, failed, 0).IsError());
    ASSERT_EQ(failed.size(), keys_.size());
}

TEST_F(HeteroD2RHTest, DISABLED_AsyncRejected)
{
    const auto result = client_->AsyncMSetD2H(keys_, sourceBlobs_).get();
    ASSERT_EQ(result.status.GetCode(), K_NOT_SUPPORTED);
    ASSERT_EQ(result.failedList, keys_);
}

TEST_F(HeteroD2RHTest, DISABLED_WriteFailureDoesNotPublish)
{
    RegisterSource();
    DS_ASSERT_OK(inject::Set("HixlTransport.WriteBatch", "1*return(K_RUNTIME_ERROR)"));
    std::vector<std::string> confirmed;
    ASSERT_EQ(Write(sourceBlobs_, {}, &confirmed).GetCode(), K_RUNTIME_ERROR);
    ASSERT_TRUE(confirmed.empty());
    inject::Clear("HixlTransport.WriteBatch");
    ASSERT_EQ(Write(sourceBlobs_).GetCode(), K_NOT_READY);
    std::vector<std::string> failed;
    ASSERT_TRUE(client_->MGetH2D(keys_, destinationBlobs_, failed, 0).IsError());
    ASSERT_EQ(failed.size(), keys_.size());
}
}  // namespace st
}  // namespace datasystem
