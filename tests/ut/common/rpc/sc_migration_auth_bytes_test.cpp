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
#include <algorithm>
#include <memory>
#include <utility>

#include "datasystem/common/rpc/sc_migration_auth_bytes.h"

namespace datasystem {
namespace test {
namespace {
std::string Wire(std::initializer_list<unsigned char> bytes)
{
    return {reinterpret_cast<const char *>(bytes.begin()), bytes.size()};
}

butil::IOBuf Buffer(const std::string &bytes)
{
    butil::IOBuf buffer;
    buffer.append(bytes);
    return buffer;
}

butil::IOBuf SegmentedBuffer(const std::string &bytes, size_t split)
{
    butil::IOBuf buffer;
    for (const auto range : {std::make_pair(size_t{0}, split), std::make_pair(split, bytes.size())}) {
        const auto size = range.second - range.first;
        auto data = std::make_unique<char[]>(size);
        std::copy_n(bytes.data() + range.first, size, data.get());
        const auto result = buffer.append_user_data(data.get(), size, [](void *p) { delete[] static_cast<char *>(p); });
        EXPECT_EQ(result, 0);
        if (result == 0) {
            data.release();
        }
    }
    EXPECT_EQ(buffer.backing_block_num(), 2);
    return buffer;
}
}  // namespace

TEST(ScMigrationAuthBytesTest, ExcludesOnlyNotificationsAcrossRepeatedEntries)
{
    const auto original = Wire({0x48, 0x07, 0x12, 0x06, 0x0a, 0x00, 0x32, 0x02, 0x08, 0x01,
                                0x12, 0x04, 0x32, 0x00, 0x38, 0x09, 0x08, 0x03});
    const auto expected = Wire({0x48, 0x07, 0x12, 0x02, 0x0a, 0x00, 0x12, 0x02, 0x38, 0x09, 0x08, 0x03});
    butil::IOBuf projected;
    ASSERT_TRUE(BuildScMigrationAuthBytes(Buffer(original), projected).IsOk());
    EXPECT_EQ(projected.to_string(), expected);
    auto changed = original;
    changed[9] = 2;
    ASSERT_TRUE(BuildScMigrationAuthBytes(Buffer(changed), projected).IsOk());
    EXPECT_EQ(projected.to_string(), expected);
    changed[15] = 10;
    ASSERT_TRUE(BuildScMigrationAuthBytes(Buffer(changed), projected).IsOk());
    EXPECT_NE(projected.to_string(), expected);
}

TEST(ScMigrationAuthBytesTest, PreservesUnknownWireTypesAndUnchangedLengthEncoding)
{
    const auto original = Wire({0x10, 0x07, 0x12, 0x84, 0x00, 0x30, 0x09, 0x0a, 0x00});
    butil::IOBuf projected;
    ASSERT_TRUE(BuildScMigrationAuthBytes(Buffer(original), projected).IsOk());
    EXPECT_EQ(projected.to_string(), original);
}

TEST(ScMigrationAuthBytesTest, RejectsMalformedNestedAndTopLevelWire)
{
    for (const auto &bytes : {Wire({0x12, 0x03, 0x0a, 0x00}), Wire({0x12, 0x02, 0x32, 0x01}),
                              Wire({0x00}), Wire({0x12, 0x01, 0x0b}),
                              Wire({0x12, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01})}) {
        butil::IOBuf projected;
        EXPECT_TRUE(BuildScMigrationAuthBytes(Buffer(bytes), projected).IsError());
    }
}

TEST(ScMigrationAuthBytesTest, IndependentBlocksAndSourceReleasePreserveProjectionAtEveryCut)
{
    for (const auto &entry : {
             std::make_pair(Wire({0x12, 0x04, 0x32, 0x02, 0x08, 0x01}), Wire({0x12, 0x00})),
             std::make_pair(Wire({0x4a, 0x02, 0x08, 0x01}), Wire({0x4a, 0x02, 0x08, 0x01})),
             std::make_pair(Wire({0x12, 0x04, 0x1b, 0x32, 0x00, 0x1c}),
                            Wire({0x12, 0x04, 0x1b, 0x32, 0x00, 0x1c}))}) {
        for (size_t split = 1; split < entry.first.size(); ++split) {
            butil::IOBuf projected;
            {
                const auto source = SegmentedBuffer(entry.first, split);
                ASSERT_TRUE(BuildScMigrationAuthBytes(source, projected).IsOk()) << "split=" << split;
            }
            EXPECT_EQ(projected.to_string(), entry.second) << "split=" << split;
        }
    }
}
}  // namespace test
}  // namespace datasystem
