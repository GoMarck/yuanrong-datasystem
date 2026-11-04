#pragma once

#include <cstdint>
#include <memory>
#include <string>

// The returned owner must keep data valid until verification finishes. This
// small interface lets KVTest's pipeline and fake tests run without including
// Mooncake headers; the production implementation uses RealClient.
struct MooncakeReadResult {
    std::shared_ptr<void> owner;
    const void *data = nullptr;
    uint64_t size = 0;
};

class MooncakeKvClient {
public:
    virtual ~MooncakeKvClient() = default;
    virtual bool Put(const std::string &key, const std::string &data) = 0;
    virtual bool Get(const std::string &key, MooncakeReadResult &out) = 0;
};

struct Config;
// Implemented only in the Mooncake CMake target, which links the prebuilt SDK.
std::shared_ptr<MooncakeKvClient> CreateMooncakeClient(const Config &cfg);
