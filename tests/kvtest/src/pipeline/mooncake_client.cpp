#include "mooncake_client.h"

#include "common/config.h"

#include <real_client.h>

#include <span>
#include <stdexcept>
#include <utility>

namespace {

class RealMooncakeClient final : public MooncakeKvClient {
public:
    explicit RealMooncakeClient(const Config &cfg)
        : client_(mooncake::RealClient::create()) {
        if (!client_) throw std::runtime_error("RealClient::create returned null");
        const int status = client_->setup_real(
            cfg.mooncakePodIp, "P2PHANDSHAKE", 0,
            cfg.mooncakeLocalBufferSize, cfg.mooncakeTransport,
            cfg.mooncakeDeviceName, cfg.mooncakeMasterAddress);
        if (status != 0) {
            throw std::runtime_error("RealClient::setup_real returned " + std::to_string(status));
        }
        replica_.replica_num = 1;
    }

    ~RealMooncakeClient() override { (void)client_->tearDownAll(); }

    bool Put(const std::string &key, const std::string &data) override {
        return client_->put(key, std::span<const char>(data.data(), data.size()), replica_) == 0;
    }

    bool Get(const std::string &key, MooncakeReadResult &out) override {
        auto buffer = client_->get_buffer(key);
        if (!buffer) return false;
        out.data = buffer->ptr();
        out.size = buffer->size();
        out.owner = std::move(buffer);
        return true;
    }

private:
    std::shared_ptr<mooncake::RealClient> client_;
    mooncake::ReplicateConfig replica_;
};

}  // namespace

std::shared_ptr<MooncakeKvClient> CreateMooncakeClient(const Config &cfg) {
    return std::make_shared<RealMooncakeClient>(cfg);
}
