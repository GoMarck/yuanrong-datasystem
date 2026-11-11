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

#ifndef DATASYSTEM_COMMON_URMA_MOCK_TCP_ENDPOINT_SERVICE_H
#define DATASYSTEM_COMMON_URMA_MOCK_TCP_ENDPOINT_SERVICE_H

#include <cstddef>
#include <cstdint>

#include "datasystem/common/urma_mock/abi/urma_abi_compat.h"
#include "datasystem/common/urma_mock/registry/import_endpoint_registry.h"

namespace datasystem {
namespace urma_mock {
class MockSeg;

class TcpEndpointService {
public:
    static bool ConfigureSegment(urma_seg_t &seg);
    static bool DecodeEndpoint(const urma_seg_t &seg, ImportEndpoint &endpoint);
    static bool Transfer(const MockSeg &seg, uint64_t offset, void *data, size_t size, bool read);
    static void Shutdown();
};
}  // namespace urma_mock
}  // namespace datasystem

#endif
