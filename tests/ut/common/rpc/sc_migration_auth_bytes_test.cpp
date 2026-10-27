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

TEST(ScMigrationAuthBytesTest, RejectsMalformedNestedAndTopLevelWire)
{
    for (const auto &bytes : {Wire({0x12, 0x03, 0x0a, 0x00}), Wire({0x12, 0x02, 0x32, 0x01}),
                              Wire({0x00}), Wire({0x12, 0x01, 0x0b}),
                              Wire({0x12, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01})}) {
        butil::IOBuf projected;
        EXPECT_TRUE(BuildScMigrationAuthBytes(Buffer(bytes), projected).IsError());
    }
}

}  // namespace test
}  // namespace datasystem
