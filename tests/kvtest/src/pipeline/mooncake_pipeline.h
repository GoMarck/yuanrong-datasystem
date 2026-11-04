#pragma once

#include "pipeline.h"
#include "mooncake_client.h"

class MooncakePipeline {
public:
    virtual ~MooncakePipeline() = default;
    virtual OpFunc GetOp(const std::string &name) = 0;
};

// The owner must outlive the writer and notification executors using its callbacks.
std::unique_ptr<MooncakePipeline> CreateMooncakePipeline(
    const Config &cfg, std::shared_ptr<MooncakeKvClient> client);
