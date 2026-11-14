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
#include <gtest/gtest.h>

#include "datasystem/common/rdma/remote_h2d_host_info.h"
#include "datasystem/protos/worker_object.pb.h"

namespace datasystem {
namespace {
constexpr int32_t SECOND_DEVICE = 4;
constexpr uint64_t BLOB_SIZE = 128;
constexpr uint64_t SEGMENT_SIZE = 4096;
constexpr uint32_t OBJECT_COUNT = 1024;
constexpr uint32_t ENGINE_COUNT = 2;

SharedRemoteH2DRootInfoTable MakeRoots(const std::string &endpoint, int32_t device = 0)
{
    auto roots = std::make_shared<RemoteH2DRootInfoTable>();
    auto *root = roots->Add();
    root->set_internal(endpoint);
    root->set_worker_logic_device_id(device);
    root->set_worker_physical_device_id(device);
    return roots;
}

TEST(RemoteH2DHostInfoTest, RejectsMissingAndInvalidIndexesButAcceptsZero)
{
    auto roots = MakeRoots("192.0.2.1:21001");
    RemoteH2DHostInfo host;
    RemoteH2DHostInfoPb wire;
    EXPECT_FALSE(host.HasRootInfo());
    EXPECT_TRUE(host.root_info().internal().empty());
    RemoteH2DRootInfoBuilder builder;
    EXPECT_EQ(builder.Encode(host, wire).GetCode(), K_INVALID);
    EXPECT_EQ(host.Decode(wire, roots).GetCode(), K_INVALID);
    wire.set_root_info_index(1);
    EXPECT_EQ(host.Decode(wire, roots).GetCode(), K_INVALID);
    wire.set_root_info_index(0);
    EXPECT_EQ(host.Decode(wire, nullptr).GetCode(), K_INVALID);
    ASSERT_TRUE(host.Decode(wire, roots).IsOk());
    EXPECT_EQ(&host.root_info(), &roots->Get(0));

    const RemoteH2DRootInfoPb *resolved = nullptr;
    ASSERT_TRUE(ResolveRemoteH2DRootInfo(wire, *roots, resolved).IsOk());
    EXPECT_EQ(resolved, &roots->Get(0));
    wire.clear_root_info_index();
    EXPECT_EQ(ResolveRemoteH2DRootInfo(wire, *roots, resolved).GetCode(), K_INVALID);
    EXPECT_EQ(resolved, nullptr);
}

TEST(RemoteH2DHostInfoTest, RetainsOneSharedTableAfterResponseDestruction)
{
    RemoteH2DHostInfo first;
    RemoteH2DHostInfo second;
    std::weak_ptr<const RemoteH2DRootInfoTable> weakRoots;
    {
        GetRspPb response;
        *response.add_root_infos() = MakeRoots("192.0.2.1:21001")->Get(0);
        auto *wire = response.add_objects()->mutable_host_info();
        wire->set_root_info_index(0);
        wire->mutable_data_info()->add_sizes(BLOB_SIZE);
        auto roots = std::make_shared<RemoteH2DRootInfoTable>();
        roots->Swap(response.mutable_root_infos());
        weakRoots = roots;
        ASSERT_TRUE(first.Decode(*wire, roots).IsOk());
        ASSERT_TRUE(second.Decode(*wire, roots).IsOk());
    }
    ASSERT_FALSE(weakRoots.expired());
    EXPECT_EQ(&first.root_info(), &second.root_info());
    EXPECT_EQ(first.data_info().sizes(0), BLOB_SIZE);
    first = RemoteH2DHostInfo{};
    EXPECT_FALSE(weakRoots.expired());
    second = RemoteH2DHostInfo{};
    EXPECT_TRUE(weakRoots.expired());
}

TEST(RemoteH2DHostInfoTest, DeduplicatesRootsWithoutExpandingObjectMetadata)
{
    auto firstRoots = MakeRoots("192.0.2.1:21001");
    auto secondRoots = MakeRoots("192.0.2.1:21002", SECOND_DEVICE);
    RemoteH2DHostInfo first;
    RemoteH2DHostInfo second;
    ASSERT_TRUE(first.SetRootInfo(firstRoots, 0).IsOk());
    ASSERT_TRUE(second.SetRootInfo(secondRoots, 0).IsOk());
    first.mutable_remote_host_segment()->set_seg_len(SEGMENT_SIZE);
    second.mutable_remote_host_segment()->set_seg_len(SEGMENT_SIZE);
    first.mutable_data_info()->add_sizes(BLOB_SIZE);
    second.mutable_data_info()->add_sizes(BLOB_SIZE);
    RemoteH2DRootInfoBuilder builder;
    MultiCreateRspPb response;
    response.mutable_results()->Reserve(OBJECT_COUNT);
    for (uint32_t i = 0; i < OBJECT_COUNT; ++i) {
        auto *wire = response.add_results()->mutable_host_info();
        ASSERT_TRUE(builder.Encode(i % ENGINE_COUNT == 0 ? first : second, *wire).IsOk());
        EXPECT_EQ(wire->root_info_index(), i % ENGINE_COUNT);
    }
    EXPECT_EQ(builder.Roots().size(), ENGINE_COUNT);
    builder.MoveTo(*response.mutable_root_infos());
    MultiCreateRspPb decoded;
    ASSERT_TRUE(decoded.ParseFromString(response.SerializeAsString()));
    EXPECT_EQ(decoded.root_infos_size(), ENGINE_COUNT);
    EXPECT_EQ(decoded.results_size(), OBJECT_COUNT);
    EXPECT_EQ(decoded.results(1).host_info().data_info().sizes(0), BLOB_SIZE);
    EXPECT_TRUE(decoded.results(1).root_infos().empty());
}

TEST(RemoteH2DHostInfoTest, RemapsCollidingIndexesFromIndependentPartitions)
{
    auto first = MakeRoots("192.0.2.1:21001");
    auto second = MakeRoots("192.0.2.2:21001");
    RemoteH2DRootInfoBuilder builder;
    std::vector<uint32_t> firstIndexes;
    std::vector<uint32_t> secondIndexes;
    std::vector<uint32_t> repeatedIndexes;
    ASSERT_TRUE(builder.Merge(*first, firstIndexes).IsOk());
    ASSERT_TRUE(builder.Merge(*second, secondIndexes).IsOk());
    ASSERT_TRUE(builder.Merge(*first, repeatedIndexes).IsOk());
    ASSERT_EQ(firstIndexes.size(), 1);
    ASSERT_EQ(secondIndexes.size(), 1);
    EXPECT_NE(firstIndexes[0], secondIndexes[0]);
    EXPECT_EQ(firstIndexes, repeatedIndexes);
    BatchGetObjectRemoteRspPb response;
    response.add_responses()->mutable_host_info()->set_root_info_index(firstIndexes[0]);
    response.add_responses()->mutable_host_info()->set_root_info_index(secondIndexes[0]);
    builder.MoveTo(*response.mutable_root_infos());
    EXPECT_EQ(response.root_infos_size(), ENGINE_COUNT);
    const RemoteH2DRootInfoPb *root = nullptr;
    ASSERT_TRUE(ResolveRemoteH2DRootInfo(response.responses(1).host_info(), response.root_infos(), root).IsOk());
    EXPECT_EQ(root->internal(), second->Get(0).internal());
}

TEST(RemoteH2DHostInfoTest, RejectsConflictingDeviceMetadataAndEmptyEndpoints)
{
    auto first = MakeRoots("192.0.2.1:21001");
    auto conflicting = MakeRoots("192.0.2.1:21001", SECOND_DEVICE);
    RemoteH2DRootInfoBuilder builder;
    uint32_t index = 0;
    ASSERT_TRUE(builder.Add(first->Get(0), index).IsOk());
    EXPECT_EQ(builder.Add(conflicting->Get(0), index).GetCode(), K_INVALID);
    EXPECT_EQ(builder.Add(RemoteH2DRootInfoPb{}, index).GetCode(), K_INVALID);
    EXPECT_EQ(builder.Roots().size(), 1);
}
}  // namespace
}  // namespace datasystem
