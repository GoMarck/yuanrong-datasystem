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

#include "datasystem/common/rpc/sc_migration_auth_bytes.h"

#include <limits>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format_lite.h>
#include <butil/iobuf.h>

#include "datasystem/common/util/status_helper.h"

namespace datasystem {
namespace {
using google::protobuf::internal::WireFormatLite;
using google::protobuf::io::CodedInputStream;

class WireInputStream final : public butil::IOBufAsZeroCopyInputStream {
public:
    explicit WireInputStream(const butil::IOBuf &body) : IOBufAsZeroCopyInputStream(body) {}

    bool Skip(int count) override
    {
        if (count < 0) {
            return false;
        }
        const auto before = ByteCount();
        // BRPC 1.15 returns false after exactly consuming EOF; protobuf requires success.
        return IOBufAsZeroCopyInputStream::Skip(count) || ByteCount() - before == count;
    }
};

Status InvalidWire()
{
    return Status(K_INVALID, "Malformed SC migration request bytes.");
}

Status ProjectStreamMeta(CodedInputStream &input, int end, butil::IOBuf &unconsumed,
                         butil::IOBuf &result, bool &changed)
{
    while (input.CurrentPosition() < end) {
        const int begin = input.CurrentPosition();
        const auto tag = input.ReadTag();
        if (WireFormatLite::GetTagFieldNumber(tag) == 0 || !WireFormatLite::SkipField(&input, tag)) {
            return InvalidWire();
        }
        if (tag == WireFormatLite::MakeTag(6, WireFormatLite::WIRETYPE_LENGTH_DELIMITED)) {
            changed = true;
            unconsumed.pop_front(input.CurrentPosition() - begin);
        } else {
            unconsumed.cutn(&result, input.CurrentPosition() - begin);
        }
    }
    return input.CurrentPosition() == end ? Status::OK() : InvalidWire();
}
}  // namespace

Status BuildScMigrationAuthBytes(const butil::IOBuf &body, butil::IOBuf &projected)
{
    if (body.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return InvalidWire();
    }
    WireInputStream stream(body);
    CodedInputStream input(&stream);
    butil::IOBuf unconsumed(body);
    butil::IOBuf result;
    const int size = static_cast<int>(body.size());
    while (input.CurrentPosition() < size) {
        const int begin = input.CurrentPosition();
        const auto tag = input.ReadTag();
        if (WireFormatLite::GetTagFieldNumber(tag) == 0) {
            return InvalidWire();
        }
        if (tag != WireFormatLite::MakeTag(2, WireFormatLite::WIRETYPE_LENGTH_DELIMITED)) {
            if (!WireFormatLite::SkipField(&input, tag)) {
                return InvalidWire();
            }
            unconsumed.cutn(&result, input.CurrentPosition() - begin);
            continue;
        }
        const int tagEnd = input.CurrentPosition();
        uint64_t length = 0;
        if (!input.ReadVarint64(&length) || length > static_cast<uint64_t>(size - input.CurrentPosition())) {
            return InvalidWire();
        }
        const int end = input.CurrentPosition() + static_cast<int>(length);
        butil::IOBuf header;
        unconsumed.cutn(&header, input.CurrentPosition() - begin);
        const auto limit = input.PushLimit(static_cast<int>(length));
        butil::IOBuf nested;
        bool changed = false;
        RETURN_IF_NOT_OK(ProjectStreamMeta(input, end, unconsumed, nested, changed));
        input.PopLimit(limit);
        if (!changed) {
            result.append(header);
            result.append(nested);
            continue;
        }
        header.pop_back(header.size() - static_cast<size_t>(tagEnd - begin));
        result.append(header);
        uint8_t encoded[10];
        auto *lengthEnd = google::protobuf::io::CodedOutputStream::WriteVarint64ToArray(nested.size(), encoded);
        CHECK_FAIL_RETURN_STATUS(result.append(encoded, lengthEnd - encoded) == 0, K_OUT_OF_MEMORY,
                                 "Failed to build SC migration authentication bytes.");
        result.append(nested);
    }
    projected.swap(result);
    return Status::OK();
}
}  // namespace datasystem
