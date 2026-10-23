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
        GetClientFdReqPb req;
        AkSkManager akSkManager;
        akSkManager.GenerateSignature(req);
        akSkManager.SetClientAkSk(clientAk, clientSk);
        akSkManager.SetServerAkSk(serverType, serverAk, serverSk);
        std::string clientId = "client1";
        req.set_client_id(clientId);
        akSkManager.GenerateSignature(req);
        auto serializedStr = req.SerializeAsString();
        DS_ASSERT_OK(GetRequestContext()->serializedMessage.CopyBuffer(serializedStr.c_str(), serializedStr.size()));
        if (expectOk) {
            EXPECT_EQ(akSkManager.VerifySignatureAndTimestamp(req), Status::OK());
        } else {
            EXPECT_NE(akSkManager.VerifySignatureAndTimestamp(req), Status::OK());
        }
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
    GetClientFdReqPb req;
    AkSkManager akSkManager(1);
    DS_ASSERT_OK(akSkManager.GenerateSignature(req));
    DS_ASSERT_OK(akSkManager.SetClientAkSk(accessKey_, secretKey_));
    DS_ASSERT_OK(akSkManager.SetServerAkSk(AkSkType::SYSTEM, "", secretKey_,  dataKey_));
    DS_ASSERT_OK(akSkManager.SetServerAkSk(AkSkType::SYSTEM, accessKey_, secretKey_,  dataKey_));
    std::string clientId = "client1";
    req.set_client_id(clientId);
    DS_ASSERT_OK(akSkManager.GenerateSignature(req));
    sleep(2);
    EXPECT_NE(akSkManager.VerifySignatureAndTimestamp(req), Status::OK());
}

TEST_F(AkSkTest, AkSkCompatibility)
{
    DS_ASSERT_OK(datasystem::inject::Set("AkSk.SetTimestamp", "call(123456)"));

    GetClientFdReqPb req;
    req.set_client_id("clientId");
    AkSkManager akSkManager;
    DS_ASSERT_OK(akSkManager.GenerateSignature(req));
    DS_ASSERT_OK(akSkManager.SetClientAkSk(accessKey_, secretKey_));
    DS_ASSERT_OK(akSkManager.SetServerAkSk(AkSkType::SYSTEM, "", secretKey_,  dataKey_));
    DS_ASSERT_OK(akSkManager.SetServerAkSk(AkSkType::SYSTEM, accessKey_, secretKey_,  dataKey_));
    std::string clientId = "client1";
    req.set_client_id(clientId);
    DS_ASSERT_OK(akSkManager.GenerateSignature(req));

    char expectSignRaw[] = "0539b3418b6ce67ae741dca0edf21ed40b76cd075965cef55c5959c181b70f2a\0";
    std::string expectSign(expectSignRaw, sizeof(expectSignRaw) - 1);
    ASSERT_EQ(req.signature(), expectSign);
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

TEST_F(AkSkTest, DigestVerificationRejectsInvalidProof)
{
    AkSkData key;
    key.accessKey = "test-only-access";
    key.secretKey = SensitiveValue("test-only-secret");
    AkSkDigest digest{};
    digest[0] = 7;
    std::string signature;
    DS_ASSERT_OK(Signature::GetSignatureFromDigest(key, digest, signature));
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    AkSkManager verifier;
    DS_ASSERT_OK(verifier.VerifySignatureFromDigest("", 0, "", digest));
    DS_ASSERT_OK(verifier.SetServerAkSk(AkSkType::SYSTEM, key.accessKey, key.secretKey));
    DS_ASSERT_OK(verifier.VerifySignatureFromDigest(signature, now, key.accessKey, digest));
    EXPECT_EQ(verifier.VerifySignatureFromDigest(signature, now, "test-only-other", digest).GetCode(),
              K_NOT_AUTHORIZED);
    auto changed = digest;
    changed[0] ^= 1;
    EXPECT_EQ(verifier.VerifySignatureFromDigest(signature, now, key.accessKey, changed).GetCode(), K_NOT_AUTHORIZED);
    EXPECT_EQ(verifier.VerifySignatureFromDigest(signature.substr(0, 64), now, key.accessKey, digest).GetCode(),
              K_NOT_AUTHORIZED);
    EXPECT_EQ(verifier.VerifySignatureFromDigest(signature, 0, key.accessKey, digest).GetCode(), K_NOT_AUTHORIZED);
}

TEST_F(AkSkTest, ClientKeySnapshotStaysPairedDuringRotation)
{
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

TEST_F(AkSkTest, FailedKeyRotationKeepsPreviousPair)
{
    class RejectingSignature final : public Signature {
        Status CopyAkSk(const std::string &accessKey, SensitiveValue secretKey, AkSkType type,
                        AkSkData &out) override
        {
            RETURN_IF_NOT_OK(Signature::CopyAkSk(accessKey, std::move(secretKey), type, out));
            return accessKey == "test-only-rejected" ? Status(K_INVALID, "test-only rejection") : Status::OK();
        }
    } provider;
    DS_ASSERT_OK(provider.SetClientAkSk("test-only-previous", SensitiveValue("test-only-previous-secret")));
    ASSERT_TRUE(provider.SetClientAkSk("test-only-rejected", SensitiveValue("test-only-new-secret")).IsError());
    AkSkData snapshot;
    DS_ASSERT_OK(provider.SnapshotClientKey(snapshot));
    EXPECT_EQ(snapshot.accessKey, "test-only-previous");
    EXPECT_EQ(std::string(snapshot.secretKey.GetData(), snapshot.secretKey.GetSize()), "test-only-previous-secret");
}
}  // namespace ut
}  // namespace datasystem
