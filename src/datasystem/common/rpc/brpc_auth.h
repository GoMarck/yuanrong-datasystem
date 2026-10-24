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

#ifndef DATASYSTEM_COMMON_RPC_BRPC_AUTH_H
#define DATASYSTEM_COMMON_RPC_BRPC_AUTH_H

#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <brpc/channel.h>
#include <brpc/checksum.h>
#include <brpc/data_factory.h>
#include <brpc/serialized_request.h>

#include "datasystem/common/ak_sk/signature.h"
#include "datasystem/common/rpc/brpc_factory.h"

namespace datasystem {
class AkSkManager;
inline constexpr auto RPC_AUTH_CHECKSUM_TYPE = static_cast<brpc::ChecksumType>(1004);

class RpcMethodMap {
public:
    using ServicePair = std::pair<const google::protobuf::ServiceDescriptor *,
                                  const google::protobuf::ServiceDescriptor *>;
    static Status Create(std::initializer_list<ServicePair> services, std::shared_ptr<const RpcMethodMap> &out);
    const google::protobuf::MethodDescriptor *Find(const google::protobuf::MethodDescriptor *method) const;
private:
    std::unordered_map<const google::protobuf::MethodDescriptor *, const google::protobuf::MethodDescriptor *> methods_;
};

struct RpcSigningBinding {
    std::shared_ptr<Signature> credentials;
    std::shared_ptr<const RpcMethodMap> methods;
};

struct RpcRequestProof {
    bool captured = false;
    AkSkDigest digest{};
    uint64_t timestampUs = 0;
    std::string accessKey;
    std::string signature;
};

struct RpcRequestCapture : RpcRequestProof {
    const brpc::Controller *owner = nullptr;
};

class RpcAuthDataFactory final : public brpc::DataFactory {
public:
    void *CreateData() const override;
    void DestroyData(void *data) const override;
    bool ResetData(void *data) const override;
};

// BRPC's registry is not thread-safe. Register before starting any RPC server
// or exposing authenticated channels to concurrent requests.
Status RegisterRpcAuthCapture();
Status ComputeRpcRequestDigest(const google::protobuf::MethodDescriptor &method, uint64_t timestampUs,
                               const butil::IOBuf &body, AkSkDigest &out);
Status CaptureRpcRequest(const brpc::ChecksumIn &input, RpcRequestCapture &capture);
const RpcRequestCapture *GetRpcRequestCapture(brpc::Controller &controller);
RpcRequestProof CopyRpcRequestProof(brpc::Controller &controller) noexcept;
Status VerifyRpcRequest(AkSkManager &manager, const RpcRequestProof *proof);

class BrpcAuthChannel final : public brpc::Channel {
public:
    void CallMethod(const google::protobuf::MethodDescriptor *method, google::protobuf::RpcController *controller,
                    const google::protobuf::Message *request, google::protobuf::Message *response,
                    google::protobuf::Closure *done) override;
private:
    friend Status CreateBrpcAuthChannel(const BrpcChannelConfig &, RpcSigningBinding,
                                       std::unique_ptr<brpc::Channel> &);
    using Serializer = void (*)(butil::IOBuf *, brpc::Controller *, const google::protobuf::Message *);
    explicit BrpcAuthChannel(RpcSigningBinding binding) : binding_(std::move(binding)) {}
    static void SerializeAuthenticated(butil::IOBuf *out, brpc::Controller *controller,
                                       const google::protobuf::Message *message);
    Serializer serializer_ = nullptr;
    RpcSigningBinding binding_;
};

Status CreateBrpcAuthChannel(const BrpcChannelConfig &config, RpcSigningBinding binding,
                            std::unique_ptr<brpc::Channel> &out);
}  // namespace datasystem
#endif  // DATASYSTEM_COMMON_RPC_BRPC_AUTH_H
