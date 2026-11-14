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
#ifndef DATASYSTEM_COMMON_RDMA_REMOTE_H2D_HOST_INFO_H
#define DATASYSTEM_COMMON_RDMA_REMOTE_H2D_HOST_INFO_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "datasystem/common/util/status_helper.h"
#include "datasystem/protos/object_posix.pb.h"

namespace datasystem {
using RemoteH2DRootInfoTable = google::protobuf::RepeatedPtrField<RemoteH2DRootInfoPb>;
using SharedRemoteH2DRootInfoTable = std::shared_ptr<const RemoteH2DRootInfoTable>;

inline Status ResolveRemoteH2DRootInfo(const RemoteH2DHostInfoPb &host, const RemoteH2DRootInfoTable &roots,
                                        const RemoteH2DRootInfoPb *&root)
{
    root = nullptr;
    CHECK_FAIL_RETURN_STATUS(host.has_root_info_index()
                                 && host.root_info_index() < static_cast<uint32_t>(roots.size()),
                             K_INVALID, "Missing or out-of-range Remote H2D root info index");
    const auto &entry = roots.Get(host.root_info_index());
    CHECK_FAIL_RETURN_STATUS(!entry.internal().empty(), K_INVALID, "Remote H2D endpoint is empty");
    root = &entry;
    return Status::OK();
}

class RemoteH2DHostInfo {
public:
    RemoteH2DHostInfo() = default;
    ~RemoteH2DHostInfo() = default;

    Status SetRootInfo(SharedRemoteH2DRootInfoTable roots, uint32_t index)
    {
        CHECK_FAIL_RETURN_STATUS(roots != nullptr && index < static_cast<uint32_t>(roots->size()), K_INVALID,
                                 "Remote H2D root info index is out of range");
        CHECK_FAIL_RETURN_STATUS(!roots->Get(index).internal().empty(), K_INVALID,
                                 "Remote H2D endpoint is empty");
        roots_ = std::move(roots);
        info_.set_root_info_index(index);
        return Status::OK();
    }

    Status Decode(const RemoteH2DHostInfoPb &info, SharedRemoteH2DRootInfoTable roots)
    {
        CHECK_FAIL_RETURN_STATUS(info.has_root_info_index(), K_INVALID, "Remote H2D root info index is missing");
        RETURN_IF_NOT_OK(SetRootInfo(std::move(roots), info.root_info_index()));
        info_ = info;
        return Status::OK();
    }

    const RemoteH2DRootInfoPb &root_info() const
    {
        return HasRootInfo() ? roots_->Get(info_.root_info_index()) : RemoteH2DRootInfoPb::default_instance();
    }

    const RemoteHostSegmentPb &remote_host_segment() const { return info_.remote_host_segment(); }
    RemoteHostSegmentPb *mutable_remote_host_segment() { return info_.mutable_remote_host_segment(); }
    const RemoteH2DDataInfoPb &data_info() const { return info_.data_info(); }
    RemoteH2DDataInfoPb *mutable_data_info() { return info_.mutable_data_info(); }
    bool has_data_info() const { return info_.has_data_info(); }
    const RemoteH2DHostInfoPb &Proto() const { return info_; }
    bool HasRootInfo() const { return roots_ != nullptr && info_.has_root_info_index(); }

private:
    SharedRemoteH2DRootInfoTable roots_;
    RemoteH2DHostInfoPb info_;
};

// One builder belongs to one response (or one parallel partition), never to concurrent writers.
class RemoteH2DRootInfoBuilder {
public:
    RemoteH2DRootInfoBuilder() = default;
    ~RemoteH2DRootInfoBuilder() = default;
    RemoteH2DRootInfoBuilder(const RemoteH2DRootInfoBuilder &) = delete;
    RemoteH2DRootInfoBuilder &operator=(const RemoteH2DRootInfoBuilder &) = delete;
    RemoteH2DRootInfoBuilder(RemoteH2DRootInfoBuilder &&) = default;
    RemoteH2DRootInfoBuilder &operator=(RemoteH2DRootInfoBuilder &&) = default;

    const RemoteH2DRootInfoTable &Roots() const { return roots_; }
    Status Merge(const RemoteH2DRootInfoTable &source, std::vector<uint32_t> &indexes)
    {
        indexes.resize(source.size());
        for (int i = 0; i < source.size(); ++i) {
            RETURN_IF_NOT_OK(Add(source.Get(i), indexes[i]));
        }
        return Status::OK();
    }

    void MoveTo(RemoteH2DRootInfoTable &output)
    {
        output.Swap(&roots_);
        roots_.Clear();
        indexes_.clear();
    }

    Status Encode(const RemoteH2DHostInfo &host, RemoteH2DHostInfoPb &output)
    {
        CHECK_FAIL_RETURN_STATUS(host.HasRootInfo(), K_INVALID, "Remote H2D root info is unavailable");
        uint32_t index = 0;
        RETURN_IF_NOT_OK(Add(host.root_info(), index));
        output = host.Proto();
        output.set_root_info_index(index);
        return Status::OK();
    }

    Status Add(const RemoteH2DRootInfoPb &root, uint32_t &index)
    {
        CHECK_FAIL_RETURN_STATUS(!root.internal().empty(), K_INVALID, "Remote H2D endpoint is empty");
        auto inserted = indexes_.try_emplace(root.internal(), roots_.size());
        index = inserted.first->second;
        if (inserted.second) {
            *roots_.Add() = root;
        } else {
            const auto &existing = roots_.Get(index);
            CHECK_FAIL_RETURN_STATUS(existing.worker_logic_device_id() == root.worker_logic_device_id()
                                         && existing.worker_physical_device_id() == root.worker_physical_device_id(),
                                     K_INVALID, "Conflicting Remote H2D device information for one endpoint");
        }
        return Status::OK();
    }

private:
    RemoteH2DRootInfoTable roots_;
    std::unordered_map<std::string, uint32_t> indexes_;
};
}  // namespace datasystem
#endif
