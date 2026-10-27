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

#include "datasystem/common/rpc/brpc_auth_capture.h"

#include <charconv>
#include <new>
#include <string_view>

#include <brpc/server.h>

#include "datasystem/common/rpc/sc_migration_auth_bytes.h"
#include "datasystem/common/util/status_helper.h"

namespace datasystem {
bool UsesScMigrationProjection(const google::protobuf::MethodDescriptor &method)
{
    return method.name() == "MigrateSCMetadata" &&
           method.input_type()->full_name() == "datasystem.master.MigrateSCMetadataReqPb";
}

namespace {
constexpr char ACCESS_KEY_FIELD[] = "ds.auth.ak";
constexpr char TIMESTAMP_FIELD[] = "ds.auth.ts";
constexpr char SIGNATURE_FIELD[] = "ds.auth.sig";

void ComputeCapture(const brpc::ChecksumIn &)
{
}

RpcRequestCapture *RequestCaptureFor(brpc::Controller &controller)
{
    const auto *server = controller.server();
    if (server == nullptr ||
        dynamic_cast<const RpcAuthDataFactory *>(server->options().session_local_data_factory) == nullptr) {
        return nullptr;
    }
    return static_cast<RpcRequestCapture *>(controller.session_local_data());
}

bool VerifyCapture(const brpc::ChecksumIn &input)
{
    auto *capture = RequestCaptureFor(*input.cntl);
    if (capture != nullptr) {
        try {
            (void)CaptureRpcRequest(input, *capture);
        } catch (const std::bad_alloc &) {
            *capture = RpcRequestCapture{};
        }
    }
    return true;
}
}  // namespace

void *RpcAuthDataFactory::CreateData() const { return new (std::nothrow) RpcRequestCapture; }
void RpcAuthDataFactory::DestroyData(void *data) const { delete static_cast<RpcRequestCapture *>(data); }
bool RpcAuthDataFactory::ResetData(void *data) const
{
    if (data == nullptr) {
        return false;
    }
    *static_cast<RpcRequestCapture *>(data) = RpcRequestCapture{};
    return true;
}

Status RegisterRpcAuthChecksumColdOnce()
{
    static const Status result = [] {
        const auto *name = brpc::ChecksumTypeToCStr(RPC_AUTH_CHECKSUM_TYPE);
        CHECK_FAIL_RETURN_STATUS(name != nullptr && std::string_view(name) == "unknown", K_RUNTIME_ERROR,
                                 "RPC authentication checksum registration conflict.");
        const brpc::ChecksumHandler handler{ComputeCapture, VerifyCapture, "datasystem-rpc-ak-sk"};
        CHECK_FAIL_RETURN_STATUS(brpc::RegisterChecksumHandler(RPC_AUTH_CHECKSUM_TYPE, handler) == 0,
                                 K_RUNTIME_ERROR, "RPC authentication checksum registration conflict.");
        return Status::OK();
    }();
    return result;
}

Status ComputeRpcRequestDigest(const google::protobuf::MethodDescriptor &method, uint64_t timestampUs,
                               const butil::IOBuf &body, AkSkDigest &out)
{
    const butil::IOBuf *input = &body;
    butil::IOBuf projected;
    if (UsesScMigrationProjection(method)) {
        RETURN_IF_NOT_OK(BuildScMigrationAuthBytes(body, projected));
        input = &projected;
    }
    unsigned char timestamp[8];
    for (size_t i = 0; i < sizeof(timestamp); ++i) {
        timestamp[i] = static_cast<unsigned char>(timestampUs >> ((sizeof(timestamp) - i - 1) * 8));
    }
    SHA256_CTX context;
    CHECK_FAIL_RETURN_STATUS(SHA256_Init(&context) == 1 && SHA256_Update(&context, timestamp, sizeof(timestamp)) == 1,
                             K_RUNTIME_ERROR, "Failed to initialize RPC authentication digest.");
    for (size_t i = 0; i < input->backing_block_num(); ++i) {
        const auto block = input->backing_block(i);
        CHECK_FAIL_RETURN_STATUS(SHA256_Update(&context, block.data(), block.size()) == 1, K_RUNTIME_ERROR,
                                 "Failed to calculate RPC authentication digest.");
    }
    AkSkDigest digest{};
    CHECK_FAIL_RETURN_STATUS(SHA256_Final(digest.data(), &context) == 1, K_RUNTIME_ERROR,
                             "Failed to finish RPC authentication digest.");
    out = digest;
    return Status::OK();
}

Status CaptureRpcRequest(const brpc::ChecksumIn &input, RpcRequestCapture &capture)
{
    capture = RpcRequestCapture{};
    capture.owner = input.cntl;
    const auto *fields = input.cntl->request_user_fields();
    const auto *accessKey = fields->seek(ACCESS_KEY_FIELD);
    const auto *timestamp = fields->seek(TIMESTAMP_FIELD);
    const auto *signature = fields->seek(SIGNATURE_FIELD);
    if (accessKey != nullptr) {
        capture.accessKey = *accessKey;
    }
    if (signature != nullptr) {
        capture.signature = *signature;
    }
    CHECK_FAIL_RETURN_STATUS(timestamp != nullptr && !timestamp->empty() && timestamp->size() <= 20 &&
                                 input.cntl->method() != nullptr && input.buf != nullptr,
                             K_NOT_AUTHORIZED, "The AK/SK authorized failed.");
    const auto parsed = std::from_chars(timestamp->data(), timestamp->data() + timestamp->size(), capture.timestampUs);
    CHECK_FAIL_RETURN_STATUS(parsed.ec == std::errc{} && parsed.ptr == timestamp->data() + timestamp->size() &&
                                 !capture.accessKey.empty() && capture.signature.size() == 65,
                             K_NOT_AUTHORIZED, "The AK/SK authorized failed.");
    RETURN_IF_NOT_OK(ComputeRpcRequestDigest(*input.cntl->method(), capture.timestampUs, *input.buf, capture.digest));
    capture.captured = true;
    return Status::OK();
}

const RpcRequestCapture *GetRpcRequestCapture(brpc::Controller &controller)
{
    if (controller.request_checksum_type() != RPC_AUTH_CHECKSUM_TYPE) {
        return nullptr;
    }
    const auto *capture = RequestCaptureFor(controller);
    return capture != nullptr && capture->owner == &controller ? capture : nullptr;
}

}  // namespace datasystem
