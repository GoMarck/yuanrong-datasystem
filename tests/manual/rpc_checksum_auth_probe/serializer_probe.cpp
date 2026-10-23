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

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>

#include <brpc/channel.h>
#include <brpc/checksum.h>
#include <brpc/closure_guard.h>
#include <brpc/data_factory.h>
#include <brpc/nonreflectable_message.h>
#include <brpc/serialized_request.h>
#include <brpc/server.h>
#include <openssl/evp.h>

#include "probe.pb.h"

namespace {
constexpr auto kCaptureType = static_cast<brpc::ChecksumType>(1003);
using Digest = std::array<unsigned char, 32>;
using Serializer = void (*)(butil::IOBuf *, brpc::Controller *, const google::protobuf::Message *);

void Require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL " << message << std::endl;
        std::exit(1);
    }
}

Digest Hash(const butil::IOBuf &bytes)
{
    Digest digest{};
    auto *ctx = EVP_MD_CTX_new();
    Require(ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1, "digest init");
    for (size_t i = 0; i < bytes.backing_block_num(); ++i) {
        const auto block = bytes.backing_block(i);
        Require(EVP_DigestUpdate(ctx, block.data(), block.size()) == 1, "digest update");
    }
    unsigned size = 0;
    Require(EVP_DigestFinal_ex(ctx, digest.data(), &size) == 1 && size == digest.size(), "digest final");
    EVP_MD_CTX_free(ctx);
    return digest;
}

std::string Bytes(const Digest &digest)
{
    return {reinterpret_cast<const char *>(digest.data()), digest.size()};
}

struct Capture {
    brpc::Controller *owner = nullptr;
    Digest digest{};
};

class CaptureFactory final : public brpc::DataFactory {
public:
    void *CreateData() const override { return new Capture; }
    void DestroyData(void *data) const override { delete static_cast<Capture *>(data); }
    bool ResetData(void *data) const override
    {
        *static_cast<Capture *>(data) = Capture{};
        return true;
    }
};

void UnusedCompute(const brpc::ChecksumIn &) { Require(false, "native encoder must see checksum NONE"); }

bool CaptureBytes(const brpc::ChecksumIn &in)
{
    auto *capture = static_cast<Capture *>(in.cntl->session_local_data());
    Require(capture != nullptr, "request capture factory");
    capture->owner = in.cntl;
    capture->digest = Hash(*in.buf);
    return true;
}

class CountingRequest final : public brpc::NonreflectableMessage<CountingRequest> {
public:
    CountingRequest() = default;
    explicit CountingRequest(const google::protobuf::Message &message) : message_(&message) {}
    void MergeFrom(const CountingRequest &other) override { message_ = other.message_; }
    size_t ByteSizeLong() const override { return message_->ByteSizeLong(); }
    uint8_t *_InternalSerialize(uint8_t *target, google::protobuf::io::EpsCopyOutputStream *stream) const override
    {
        ++encodes;
        return message_->_InternalSerialize(target, stream);
    }
    mutable unsigned encodes = 0;
private:
    const google::protobuf::Message *message_ = nullptr;
};

struct Carrier final : brpc::SerializedRequest {
    const google::protobuf::Message *original = nullptr;
    Serializer serializer = nullptr;
    bool failPreparation = false;
};

class ProbeChannel final : public brpc::Channel {
public:
    int InitProbe(const butil::EndPoint &endpoint)
    {
        brpc::ChannelOptions options;
        options.timeout_ms = 1000;
        options.max_retry = 0;
        const int result = Init(endpoint, &options);
        if (result == 0) {
            original_ = _serialize_request;
            _serialize_request = Serialize;
        }
        return result;
    }

    void CallMethod(const google::protobuf::MethodDescriptor *method, google::protobuf::RpcController *controller,
                    const google::protobuf::Message *request, google::protobuf::Message *response,
                    google::protobuf::Closure *done) override
    {
        Carrier carrier;
        carrier.original = request;
        carrier.serializer = original_;
        carrier.failPreparation = failPreparation;
        brpc::Channel::CallMethod(method, controller, &carrier, response, done);
    }

    bool failPreparation = false;

private:
    static void Serialize(butil::IOBuf *buffer, brpc::Controller *controller,
                          const google::protobuf::Message *message)
    {
        if (controller->Failed()) {
            return;
        }
        auto *carrier = dynamic_cast<const Carrier *>(message);
        Require(carrier != nullptr, "carrier type");
        if (carrier->failPreparation) {
            controller->SetFailed("probe preparation failure");
            return;
        }
        carrier->serializer(buffer, controller, carrier->original);
        if (!controller->Failed()) {
            (*controller->request_user_fields())["probe.digest"] = Bytes(Hash(*buffer));
            controller->set_request_checksum_type(kCaptureType);
        }
    }
    Serializer original_ = nullptr;
};

class Service final : public checksum_probe::PlainProbe {
public:
    void Echo(google::protobuf::RpcController *base, const checksum_probe::RequestV1 *request,
              checksum_probe::Reply *reply, google::protobuf::Closure *done) override
    {
        brpc::ClosureGuard guard(done);
        auto *controller = static_cast<brpc::Controller *>(base);
        auto *capture = static_cast<Capture *>(controller->session_local_data());
        const auto *sent = controller->request_user_fields()->seek("probe.digest");
        Require(capture && capture->owner == controller && sent && *sent == Bytes(capture->digest),
                "sender and receiver hash identical raw bytes");
        reply->set_digest(*sent);
        butil::IOBuf reencoded;
        reencoded.append(request->SerializeAsString());
        reply->set_reencoded_differs(Hash(reencoded) != capture->digest);
    }
};

class DeleteController final : public google::protobuf::Closure {
public:
    explicit DeleteController(brpc::Controller *controller) : controller_(controller) {}
    void Run() override
    {
        failed = controller_->Failed();
        delete controller_;
        ++calls;
    }
    std::atomic<unsigned> calls{0};
    bool failed = false;
private:
    brpc::Controller *controller_;
};
}  // namespace

int main()
{
    Require(brpc::RegisterChecksumHandler(kCaptureType, {UnusedCompute, CaptureBytes, "serializer-boundary-probe"}) == 0,
            "register capture");
    CaptureFactory factory;
    Service service;
    brpc::Server server;
    Require(server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) == 0, "register service");
    brpc::ServerOptions options;
    options.session_local_data_factory = &factory;
    Require(server.Start("127.0.0.1:0", &options) == 0, "start loopback server");
    ProbeChannel channel;
    Require(channel.InitProbe(server.listen_address()) == 0, "install serializer after init");
    const auto *method = checksum_probe::PlainProbe::descriptor()->method(0);
    checksum_probe::RequestV2 request;
    request.set_payload("serializer-boundary");
    request.set_added(7);
    request.set_known(3);

    CountingRequest sync(request);
    checksum_probe::Reply reply;
    brpc::Controller controller;
    channel.CallMethod(method, &controller, &sync, &reply, nullptr);
    Require(!controller.Failed() && sync.encodes == 1 && reply.reencoded_differs(), "sync single encode across schemas");

    for (bool failure : {false, true}) {
        channel.failPreparation = failure;
        CountingRequest counted(request);
        checksum_probe::Reply result;
        auto *async = new brpc::Controller;
        DeleteController done(async);
        const auto call = async->call_id();
        channel.CallMethod(method, async, &counted, &result, &done);
        brpc::Join(call);
        Require(done.calls == 1 && done.failed == failure && counted.encodes == (failure ? 0U : 1U),
                "async completes once and permits controller deletion");
    }

    channel.failPreparation = false;
    CountingRequest cancelled(request);
    brpc::Controller before;
    before.StartCancel();
    channel.CallMethod(method, &before, &cancelled, &reply, nullptr);
    Require(before.Failed() && cancelled.encodes == 0, "pre-cancel preserves native failure without encoding");
    server.Stop(0);
    server.Join();
    std::cout << "PASS serializer boundary: sync cross-schema, async completion, preparation failure, pre-cancel\n";
}
