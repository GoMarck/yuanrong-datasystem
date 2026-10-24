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

#include "datasystem/common/rpc/brpc_auth.h"

#include <charconv>
#include <cerrno>
#include <new>
#include <string_view>

#include <brpc/server.h>

#include "datasystem/common/ak_sk/ak_sk_manager.h"
#include "datasystem/common/util/timer.h"
#include "datasystem/common/rpc/sc_migration_auth_bytes.h"
#include "datasystem/common/util/status_helper.h"

namespace datasystem {
namespace {
constexpr char ACCESS_KEY_FIELD[] = "ds.auth.ak";
constexpr char TIMESTAMP_FIELD[] = "ds.auth.ts";
constexpr char SIGNATURE_FIELD[] = "ds.auth.sig";
using Serializer = void (*)(butil::IOBuf *, brpc::Controller *, const google::protobuf::Message *);

struct RpcAuthRequestCarrier final : brpc::SerializedRequest {
    RpcAuthRequestCarrier(const google::protobuf::Message *request, const RpcSigningBinding &binding,
                          Serializer serializer, Status status)
        : request(request), binding(binding), serializer(serializer), status(std::move(status)) {}
    RpcAuthRequestCarrier(const RpcAuthRequestCarrier &) = delete;
    RpcAuthRequestCarrier &operator=(const RpcAuthRequestCarrier &) = delete;
    const google::protobuf::Message *request;
    const RpcSigningBinding &binding;
    Serializer serializer;
    Status status;
};

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

Status RpcMethodMap::Create(std::initializer_list<ServicePair> services, std::shared_ptr<const RpcMethodMap> &out)
{
    auto methods = std::make_shared<RpcMethodMap>();
    for (const auto &pair : services) {
        const auto *plain = pair.first;
        const auto *auth = pair.second;
        CHECK_FAIL_RETURN_STATUS(plain != nullptr && auth != nullptr &&
                                     auth->full_name() == plain->full_name() + "Auth" &&
                                     plain->method_count() == auth->method_count(),
                                 K_INVALID, "Invalid RPC authentication service mapping.");
        for (int i = 0; i < plain->method_count(); ++i) {
            const auto *from = plain->method(i);
            const auto *to = auth->method(i);
            CHECK_FAIL_RETURN_STATUS(from->name() == to->name() && from->input_type() == to->input_type() &&
                                         from->output_type() == to->output_type() &&
                                         from->client_streaming() == to->client_streaming() &&
                                         from->server_streaming() == to->server_streaming(),
                                     K_INVALID, "Invalid RPC authentication method mapping.");
            CHECK_FAIL_RETURN_STATUS(methods->methods_.emplace(from, to).second, K_INVALID,
                                     "Duplicate RPC authentication method mapping.");
        }
    }
    out = std::move(methods);
    return Status::OK();
}

const google::protobuf::MethodDescriptor *RpcMethodMap::Find(const google::protobuf::MethodDescriptor *method) const
{
    const auto found = methods_.find(method);
    return found == methods_.end() ? nullptr : found->second;
}

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

Status RegisterRpcAuthCapture()
{
    static const Status result = [] {
        BrpcChannelFactory::EnsureGlobalInitialized();
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
    if (method.name() == "MigrateSCMetadata" &&
        method.input_type()->full_name() == "datasystem.master.MigrateSCMetadataReqPb") {
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

RpcRequestProof CopyRpcRequestProof(brpc::Controller &controller) noexcept
{
    try {
        const auto *capture = GetRpcRequestCapture(controller);
        return capture == nullptr ? RpcRequestProof{} : static_cast<const RpcRequestProof &>(*capture);
    } catch (const std::bad_alloc &) {
        return RpcRequestProof{};
    }
}

Status VerifyRpcRequest(AkSkManager &manager, const RpcRequestProof *capture)
{
    if (capture == nullptr || !capture->captured) {
        return manager.VerifySignatureFromDigest("", 0, "", AkSkDigest{});
    }
    return manager.VerifySignatureFromDigest(capture->signature, capture->timestampUs,
                                             capture->accessKey, capture->digest);
}

Status CreateBrpcAuthChannel(const BrpcChannelConfig &config, RpcSigningBinding binding,
                            std::unique_ptr<brpc::Channel> &out)
{
    CHECK_FAIL_RETURN_STATUS(binding.credentials != nullptr && binding.methods != nullptr, K_INVALID,
                             "Missing RPC signing credentials or method mapping.");
    RETURN_IF_NOT_OK(RegisterRpcAuthCapture());
    std::unique_ptr<BrpcAuthChannel> channel(new BrpcAuthChannel(std::move(binding)));
    RETURN_IF_NOT_OK(InitializeBrpcChannel(config, *channel));
    channel->serializer_ = channel->_serialize_request;
    channel->_serialize_request = BrpcAuthChannel::SerializeAuthenticated;
    out = std::move(channel);
    return Status::OK();
}

void BrpcAuthChannel::CallMethod(const google::protobuf::MethodDescriptor *method,
                                google::protobuf::RpcController *controller,
                                const google::protobuf::Message *request, google::protobuf::Message *response,
                                google::protobuf::Closure *done)
{
    const auto *authenticated = binding_.methods->Find(method);
    Status status;
    if (authenticated == nullptr) {
        status = Status(K_INVALID, "Missing RPC authentication method mapping.");
    }
    RpcAuthRequestCarrier carrier(request, binding_, serializer_, std::move(status));
    brpc::Channel::CallMethod(authenticated != nullptr ? authenticated : method, controller, &carrier, response, done);
}

void BrpcAuthChannel::SerializeAuthenticated(butil::IOBuf *out, brpc::Controller *controller,
                                            const google::protobuf::Message *message)
{
    if (controller->Failed()) {
        return;
    }
    const auto *carrier = dynamic_cast<const RpcAuthRequestCarrier *>(message);
    auto prepare = [&]() -> Status {
        CHECK_FAIL_RETURN_STATUS(carrier != nullptr && carrier->serializer != nullptr && carrier->request != nullptr,
                                 K_INVALID, "Invalid RPC authentication request carrier.");
        RETURN_IF_NOT_OK(carrier->status);
        AkSkData key;
        RETURN_IF_NOT_OK(carrier->binding.credentials->SnapshotClientKey(key));
        CHECK_FAIL_RETURN_STATUS(!key.accessKey.empty() && !key.secretKey.Empty(), K_INVALID,
                                 "Missing RPC signing credentials.");
        const auto timestamp = static_cast<uint64_t>(GetSystemClockTimeStampUs());
        controller->set_request_checksum_type(brpc::CHECKSUM_TYPE_NONE);
        carrier->serializer(out, controller, carrier->request);
        if (controller->Failed()) {
            return Status::OK();
        }
        AkSkDigest digest{};
        RETURN_IF_NOT_OK(ComputeRpcRequestDigest(*controller->method(), timestamp, *out, digest));
        std::string signature;
        RETURN_IF_NOT_OK(Signature::GetSignatureFromDigest(key, digest, signature));
        auto &fields = *controller->request_user_fields();
        fields[ACCESS_KEY_FIELD] = key.accessKey;
        fields[TIMESTAMP_FIELD] = std::to_string(timestamp);
        fields[SIGNATURE_FIELD] = std::move(signature);
        controller->set_request_checksum_type(RPC_AUTH_CHECKSUM_TYPE);
        return Status::OK();
    };
    try {
        const auto status = prepare();
        if (status.IsError()) {
            controller->SetFailed(status.GetMsg() + "\x01" "DS_ERR:" +
                                  std::to_string(static_cast<int>(status.GetCode())) + "\x02");
        }
    } catch (const std::bad_alloc &) {
        controller->SetFailed(ENOMEM, "Failed to allocate RPC authentication request data.");
    }
}
}  // namespace datasystem
