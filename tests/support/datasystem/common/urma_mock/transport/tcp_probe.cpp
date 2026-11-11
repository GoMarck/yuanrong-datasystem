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

#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include "datasystem/common/urma_mock/abi/mock_abi.h"

int main(int argc, char **argv)
{
    if (argc != 2 || ::setenv("URMA_MOCK_TCP_ENDPOINT", argv[1], 1) != 0
        || ds_urma_mock_init(nullptr) != URMA_SUCCESS) { return 1; }
    int count = 0;
    auto **devices = ds_urma_mock_get_device_list(&count);
    if (devices == nullptr || count == 0) { return 2; }
    auto *context = ds_urma_mock_create_context(devices[0], 0);
    if (context == nullptr) { return 3; }
    int fd = ::memfd_create("datasystem", MFD_CLOEXEC);
    constexpr size_t size = 4096;
    if (fd < 0 || ::ftruncate(fd, size) != 0) { return 4; }
    void *memory = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED) { return 5; }
    std::memset(memory, 0x5a, size);
    urma_seg_cfg_t config{};
    config.va = reinterpret_cast<uint64_t>(memory);
    config.len = size;
    config.token_value.value = 12345;
    auto *segment = ds_urma_mock_register_seg(context, &config);
    if (segment == nullptr) { return 6; }
    std::cout << "{\"address\":" << segment->seg.ubva.va << ",\"length\":" << segment->seg.len
              << ",\"token\":" << segment->seg.token_id << "}" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(45));
    ds_urma_mock_unregister_seg(segment);
    ds_urma_mock_delete_context(context);
    ds_urma_mock_uninit();
    ::munmap(memory, size);
    ::close(fd);
    return 0;
}
