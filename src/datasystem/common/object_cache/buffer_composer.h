/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
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

/**
 * Description: Implementation of compose and decompose buffer data.
 */
#ifndef DATASYSTEM_COMMON_OBJECT_CACHE_BUFFER_COMPOSER_H
#define DATASYSTEM_COMMON_OBJECT_CACHE_BUFFER_COMPOSER_H

#include <iomanip>
#include <limits>
#include <memory>
#include <vector>

#include "datasystem/common/util/status_helper.h"

#include "datasystem/hetero/device_common.h"
#include "datasystem/object/buffer.h"

namespace datasystem {
namespace object_cache {

struct BlobListInfo {
    int64_t keyNums = 0;
    int64_t nonExistNums = 0;
    // the key copy size
    int64_t totalSize = 0;
    int64_t minBlobNums = 0;
    int64_t maxBlobNums = 0;
    // avg blob nums per key
    int64_t avgBlobNums = 0;
    int64_t minBlockSize = 0;
    int64_t maxBlockSize = 0;
    // avg blob size per blob/key
    int64_t avgBlockSize = 0;

    std::string ToString(bool isSet) const
    {
        std::ostringstream oss;
        oss << "{\"KeyNum\":" << keyNums;
        if (isSet) {
            oss << ",\"SetKeyNum\":" << nonExistNums;
        }
        oss << ",\"KeySize\":" << totalSize;
        oss << ",\"BlobNum\":[" << minBlobNums << "," << maxBlobNums << "," << avgBlobNums << "]";
        std::string sizeDescStr = isSet ? ",\"BlobSize\":[" : ",\"KeySize\":[";
        oss << sizeDescStr << minBlockSize << "," << maxBlockSize << "," << avgBlockSize << "]}";
        return oss.str();
    }
};

/**
 * @brief Get the composed blob header size aligned to memoryAlignment.
 * @param[in] blobCount The number of blobs in one composed object.
 * @param[in] memoryAlignment The payload alignment.
 * @pre memoryAlignment must be a non-zero power of two.
 * @pre `(blobCount + 2) * sizeof(uint64_t)` must fit in size_t, and adding the alignment padding must fit in
 *      uint64_t.
 * @return The aligned header size.
 */
uint64_t GetComposedBufferHeaderSize(size_t blobCount, uint32_t memoryAlignment);

inline Status GetComposedBufferHeaderSizeChecked(size_t blobCount, uint32_t memoryAlignment, uint64_t &headerSize)
{
    CHECK_FAIL_RETURN_STATUS(memoryAlignment > 0 && (memoryAlignment & (memoryAlignment - 1)) == 0, K_INVALID,
                             FormatString("memoryAlignment %u must be a non-zero power of two", memoryAlignment));
    constexpr uint64_t kHeaderFieldCount = 2;
    constexpr uint64_t kFieldSize = sizeof(uint64_t);
    constexpr uint64_t kUint64Max = std::numeric_limits<uint64_t>::max();
    CHECK_FAIL_RETURN_STATUS(blobCount <= kUint64Max / kFieldSize - kHeaderFieldCount, K_INVALID,
                             "Composed buffer header size overflows uint64_t");
    const auto rawHeaderSize = (static_cast<uint64_t>(blobCount) + kHeaderFieldCount) * kFieldSize;
    const auto alignmentPadding = static_cast<uint64_t>(memoryAlignment) - 1;
    CHECK_FAIL_RETURN_STATUS(rawHeaderSize <= kUint64Max - alignmentPadding, K_INVALID,
                             "Aligned composed buffer header size overflows uint64_t");
    headerSize = (rawHeaderSize + alignmentPadding) & ~alignmentPadding;
    return Status::OK();
}

inline Status BuildComposedBufferHeader(const std::vector<uint64_t> &blobSizes, uint32_t memoryAlignment,
                                 std::vector<uint64_t> &header, uint64_t &composedSize)
{
    CHECK_FAIL_RETURN_STATUS(!blobSizes.empty(), K_INVALID, "Blob sizes cannot be empty");
    uint64_t headerSize = 0;
    RETURN_IF_NOT_OK(GetComposedBufferHeaderSizeChecked(blobSizes.size(), memoryAlignment, headerSize));
    CHECK_FAIL_RETURN_STATUS(headerSize <= SIZE_MAX, K_INVALID, "Composed header exceeds size_t");
    header.assign(headerSize / sizeof(uint64_t), 0);
    header[0] = blobSizes.size();
    header[1] = headerSize;
    composedSize = headerSize;
    for (size_t i = 0; i < blobSizes.size(); ++i) {
        CHECK_FAIL_RETURN_STATUS(blobSizes[i] > 0 && composedSize <= UINT64_MAX - blobSizes[i], K_INVALID,
                                 "Invalid blob size or composed size overflow");
        composedSize += blobSizes[i];
        constexpr size_t headerFieldCount = 2;
        header[i + headerFieldCount] = composedSize;
    }
    return Status::OK();
}

/**
 * @brief Prepare the data sizes by user data list
 * @param[out] sizeList The list of all data sizes
 * @param[in] devBlobList The user data list
 * @param[in] blobInfo The information of blob
 * @param[in] memoryAlignment The payload alignment.
 * @return K_OK on any object success; the error code otherwise.
 */
Status PrepareDataSizeList(std::vector<size_t> &sizeList, const std::vector<DeviceBlobList> &devBlobList,
                           BlobListInfo &blobInfo, uint32_t memoryAlignment);

/**
 * @brief Compose buffer list from non-owning DeviceBlobList references.
 *
 * Reads DeviceBlobList by const pointer so callers that already hold a request-owned vector (synchronous
 * MSetD2H view path, async MSetD2H ownership copy) never copy a DeviceBlobList or Blob. Validation covers
 * object count match, non-null views, blob count > 0, and whether each composed object's total size fits in
 * the allocated buffer.
 *
 * @param[in] deviceBlobRefs Non-owning pointers into a request-owned DeviceBlobList vector.
 * @param[in] bufferList The host buffers to compose into (same length as deviceBlobRefs).
 * @param[in] memoryAlignment The payload alignment.
 * @return K_OK on success; the error code otherwise.
 */
Status ComposeBufferDataRefs(const std::vector<const DeviceBlobList *> &deviceBlobRefs,
                             const std::vector<std::shared_ptr<Buffer>> &bufferList, uint32_t memoryAlignment);

}  // namespace object_cache
}  // namespace datasystem

#endif
