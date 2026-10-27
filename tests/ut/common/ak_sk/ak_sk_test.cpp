/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2023. All rights reserved.
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

/**
 * Description: AK/SK test.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/ak_sk/ak_sk_manager.h"
#include "datasystem/common/util/net_util.h"
#include "datasystem/common/util/timer.h"
#include "datasystem/common/util/request_context.h"
#include "datasystem/common/util/thread_local.h"
#include "datasystem/protos/share_memory.pb.h"
#include "datasystem/common/inject/inject_point.h"
#include "datasystem/common/encrypt/secret_manager.h"
#include "datasystem/common/util/raii.h"
#include "datasystem/utils/sensitive_value.h"

#include "ut/common.h"

namespace datasystem {
namespace ut {
class AkSkManagerHelper : public AkSkManager {
public:
    using Signature::GetSignature;
};

class AkSkTest : public CommonTest {
public:
    void SetUp() override
    {
        akSkManager_ = std::make_shared<AkSkManagerHelper>();
    }

    void VerifyAkSk(const std::string &clientAk, const std::string &clientSk, const std::string &serverAk,
                    const std::string &serverSk, AkSkType serverType, bool expectOk)
    {
        AkSkManagerHelper manager;
        DS_ASSERT_OK(manager.SetServerAkSk(serverType, serverAk, serverSk));
        AkSkData key;
        key.accessKey = clientAk;
        key.secretKey = SensitiveValue(clientSk);
        const std::string body("test-only-request\0bytes", 23);
        std::string signature;
        DS_ASSERT_OK(manager.GetSignature(key, body.data(), body.size(), signature));
        const auto timestamp = GetSystemClockTimeStampUs();
        const auto status = manager.VerifySignatureAndTimestamp(signature, timestamp, clientAk,
                                                                body.data(), body.size());
        EXPECT_EQ(status.IsOk(), expectOk);

    }

protected:
    std::string accessKey_ = "QTWAOYTTINDUT2QVKYUC";
    std::string secretKey_ = "MFyfvK41ba2giqM7**********KGpownRZlmVmHc";
    std::string errorKey_ = "axfasdfasdfacxvzfasdfasdfasdfasdfasdfasdf";
    std::string dataKey_ = "MFyfvK41ba2giqMUiUKGpownRZlmVmHc";
    std::shared_ptr<AkSkManagerHelper> akSkManager_;
};

TEST_F(AkSkTest, SimpleAkSk)
{
    VerifyAkSk(accessKey_, secretKey_, accessKey_, secretKey_, AkSkType::SYSTEM, true);
    VerifyAkSk(accessKey_, accessKey_, accessKey_, secretKey_, AkSkType::SYSTEM, false);
    VerifyAkSk(secretKey_, secretKey_, accessKey_, secretKey_, AkSkType::SYSTEM, false);
    VerifyAkSk(errorKey_, accessKey_, accessKey_, secretKey_, AkSkType::SYSTEM, false);
    VerifyAkSk(errorKey_, secretKey_, accessKey_, secretKey_, AkSkType::SYSTEM, false);
    VerifyAkSk(accessKey_, errorKey_, accessKey_, secretKey_, AkSkType::SYSTEM, false);
}

TEST_F(AkSkTest, AkSkTimeout)
{
    AkSkManagerHelper manager;
    DS_ASSERT_OK(manager.SetServerAkSk(AkSkType::SYSTEM, accessKey_, secretKey_));
    AkSkData key;
    key.accessKey = accessKey_;
    key.secretKey = SensitiveValue(secretKey_);
    const std::string body = "test-only-expired-request";
    std::string signature;
    DS_ASSERT_OK(manager.GetSignature(key, body.data(), body.size(), signature));
    EXPECT_EQ(manager.VerifySignatureAndTimestamp(signature, 1, accessKey_, body.data(), body.size()).GetCode(),
              K_NOT_AUTHORIZED);
}

TEST_F(AkSkTest, DigestSignaturePreservesOriginalAlgorithm)
{
    AkSkData key;
    key.accessKey = "test-only-access";
    key.secretKey = SensitiveValue("test-only-secret");
    for (const auto &body : {std::string(), std::string("binary\0request", 14)}) {
        Hasher hasher;
        std::unique_ptr<unsigned char[]> bytes;
        unsigned size = 0;
        DS_ASSERT_OK(hasher.HashSHA256(body.data(), body.size(), bytes, size));
        AkSkDigest digest{};
        ASSERT_EQ(size, digest.size());
        std::copy_n(bytes.get(), digest.size(), digest.begin());
        std::string original;
        std::string fromDigest;
        DS_ASSERT_OK(akSkManager_->GetSignature(key, body.data(), body.size(), original));
        DS_ASSERT_OK(Signature::GetSignatureFromDigest(key, digest, fromDigest));
        EXPECT_EQ(fromDigest, original);
        ASSERT_EQ(fromDigest.size(), 65);
        EXPECT_EQ(fromDigest.back(), '\0');
    }
}

TEST_F(AkSkTest, ClientKeySnapshotStaysPairedDuringRotation)
{
    Signature empty;
    AkSkData initial;
    initial.secretKey = "test-only-previous";
    DS_ASSERT_OK(empty.SnapshotClientKey(initial));
    EXPECT_TRUE(initial.accessKey.empty());
    EXPECT_TRUE(initial.secretKey.Empty());
    Signature provider("test-only-access-a", SensitiveValue("test-only-secret-a"));
    std::atomic<bool> invalid{false};
    std::thread rotate([&] {
        for (unsigned i = 0; i < 1000; ++i) {
            const bool alternate = (i % 2) != 0;
            if (provider.SetClientAkSk(alternate ? "test-only-access-a" : "test-only-access-b",
                                      SensitiveValue(alternate ? "test-only-secret-a" : "test-only-secret-b")).IsError()) {
                invalid = true;
            }
        }
    });
    for (unsigned i = 0; i < 1000; ++i) {
        AkSkData snapshot;
        if (provider.SnapshotClientKey(snapshot).IsError()) {
            invalid = true;
            continue;
        }
        const std::string secret(snapshot.secretKey.GetData(), snapshot.secretKey.GetSize());
        const bool paired = (snapshot.accessKey == "test-only-access-a" && secret == "test-only-secret-a")
                            || (snapshot.accessKey == "test-only-access-b" && secret == "test-only-secret-b");
        if (!paired) {
            invalid = true;
        }
    }
    rotate.join();
    EXPECT_FALSE(invalid.load());
}

}  // namespace ut
}  // namespace datasystem
