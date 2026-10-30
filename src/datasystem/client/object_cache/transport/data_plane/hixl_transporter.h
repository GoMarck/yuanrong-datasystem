/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */
#ifndef DATASYSTEM_CLIENT_TRANSPORT_HIXL_TRANSPORTER_H
#define DATASYSTEM_CLIENT_TRANSPORT_HIXL_TRANSPORTER_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "datasystem/client/object_cache/transport/rpc/worker_rpc_client.h"
#include "datasystem/client/object_cache/transport/rpc/mset_request_builder.h"
#include "datasystem/hetero/device_common.h"

namespace datasystem {
class HixlTransport;
namespace client {
class HixlTransporter {
public:
    HixlTransporter() = default;
    ~HixlTransporter();
    Status RegisterDeviceMemory(int32_t deviceId, const std::string &localIp,
                                const std::vector<void *> &addresses, const std::vector<uint64_t> &sizes);
    Status MSetDevice(const std::shared_ptr<WorkerRpcClient> &rpc, const std::vector<std::string> &keys,
                      const std::vector<const DeviceBlobList *> &blobs, TransportCreateParam createParam,
                      const TransportSetParam &setParam, TransportMSetResult &result,
                      std::vector<std::string> *confirmedKeys);
    void Shutdown();
private:
    Status ValidateSources(const std::vector<const DeviceBlobList *> &blobs) const;
    std::timed_mutex mutex_;
    std::shared_ptr<HixlTransport> transport_;
    std::vector<std::pair<uintptr_t, uint64_t>> ranges_;
    int32_t deviceId_ = -1;
    bool failed_ = false;
    bool transferFailed_ = false;
};
}  // namespace client
}  // namespace datasystem
#endif
