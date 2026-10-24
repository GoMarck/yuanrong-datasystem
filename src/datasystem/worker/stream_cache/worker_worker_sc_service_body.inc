/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2023. All rights reserved.
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


#ifdef DS_PEER_SC_RPC_GROUP_0
Status WorkerWorkerSCServiceImpl::DS_PEER_SC_RPC_ENTRY(PushElementsCursors,
    std::shared_ptr<ServerUnaryWriterReader<PushRspPb, PushReqPb>> serverApi)
{
    INJECT_POINT("PushElementsCursors.begin");
    PerfPoint point(PerfKey::PUSH_CURSOR_ALL);
    PushReqPb pushReqPb;
    PushRspPb pushRspPb;
    std::vector<RpcMessage> payloads;
    RETURN_IF_NOT_OK(serverApi->Read(pushReqPb));
    RETURN_IF_NOT_OK(serverApi->ReceivePayload(payloads));
    VLOG(1) << FormatString("Preparing to receive pushed data. streamName: %s", pushReqPb.stream_name());
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_PEER_SC_RPC_VERIFY(akSkManager_, pushReqPb), "AK/SK failed");
    TraceGuard traceGuard = Trace::Instance().SetTraceNewID(pushReqPb.trace_id());
    RETURN_IF_NOT_OK(PushElementsCursorsHelper(pushReqPb, payloads, pushRspPb));
    point.RecordAndReset(PerfKey::PUSH_CURSOR_RESPONSE);
    VLOG(SC_DEBUG_LOG_LEVEL) << "worker PushElementsCursors done";
    // We reply to the client at this point.
    RETURN_IF_NOT_OK(serverApi->Write(pushRspPb));
    return Status::OK();
}

#endif

#ifdef DS_PEER_SC_RPC_GROUP_1
Status WorkerWorkerSCServiceImpl::DS_PEER_SC_RPC_ENTRY(PushSharedPageCursors,
    std::shared_ptr<ServerUnaryWriterReader<PushRspPb, SharedPagePushReqPb>> serverApi)
{
    INJECT_POINT("PushElementsCursors.begin");
    PerfPoint point(PerfKey::PUSH_CURSOR_ALL);
    VLOG(SC_DEBUG_LOG_LEVEL) << FormatString("Preparing to receive pushed shared page data.");
    SharedPagePushReqPb pushReqPb;
    PushRspPb pushRspPb;
    std::vector<RpcMessage> payloads;
    RETURN_IF_NOT_OK(serverApi->Read(pushReqPb));
    RETURN_IF_NOT_OK(serverApi->ReceivePayload(payloads));
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_PEER_SC_RPC_VERIFY(akSkManager_, pushReqPb), "AK/SK failed");
    TraceGuard traceGuard = Trace::Instance().SetTraceNewID(pushReqPb.trace_id());
    // Group requests by stream name, and also record the indexes so the response is still in order.
    std::unordered_map<std::string, PushSharedPageTuple> streamDataMap;
    std::vector<ErrorInfoPb> errorList(pushReqPb.metas_size());
    for (int i = 0; i < pushReqPb.metas_size(); ++i) {
        const std::string &streamName = pushReqPb.stream_names(pushReqPb.metas(i).stream_index());
        VLOG(SC_INTERNAL_LOG_LEVEL) << FormatString("PushSharedPageCursors req involves stream %s at iteration %d",
                                                    streamName, i);
        auto iter = streamDataMap.find(streamName);
        if (iter == streamDataMap.end()) {
            PushReqPb req;
            req.set_stream_name(streamName);
            req.set_worker_addr(pushReqPb.worker_addr());
            req.set_producer_id(pushReqPb.producer_id());
            // Force the first sequence number to be 1,
            // so that UnsortedInsert only waits for the expected sequence number.
            req.set_first_seq(1);
            req.set_worker_instance_id(pushReqPb.worker_instance_id());
            iter = streamDataMap.emplace(streamName, PushSharedPageTuple()).first;
            iter->second.req_ = std::move(req);
        }
        iter->second.index_.emplace_back(i);
        auto &req = iter->second.req_;
        *req.mutable_element_meta()->Add() = pushReqPb.metas(i).element_meta();
        *req.mutable_seq()->Add() = pushReqPb.metas(i).seq();
        iter->second.payload_.emplace_back(std::move(payloads[i]));
    }
    for (auto &tuple : streamDataMap) {
        PushReqPb &req = tuple.second.req_;
        std::vector<RpcMessage> &payload = tuple.second.payload_;
        PushRspPb rsp;
        auto rc = PushElementsCursorsHelper(req, payload, rsp);
        ErrorInfoPb err;
        if (rc.IsError()) {
            err.set_error_code(rc.GetCode());
            err.set_error_msg(rc.GetMsg());
        } else {
            uint64_t expectedSize = tuple.second.index_.size();
            uint64_t actualSize = static_cast<uint64_t>(rsp.error_size());
            CHECK_FAIL_RETURN_STATUS_PRINT_ERROR(
                actualSize == expectedSize, K_RUNTIME_ERROR,
                FormatString("Unexpected number of error info, expected %zu, actual %zu", expectedSize, actualSize));
        }
        uint64_t rspIndex = 0;
        for (const auto &index : tuple.second.index_) {
            errorList[index] = rc.IsError() ? err : rsp.error(rspIndex++);
        }
    }
    *pushRspPb.mutable_error() = { errorList.begin(), errorList.end() };
    point.RecordAndReset(PerfKey::PUSH_CURSOR_RESPONSE);
    VLOG(SC_DEBUG_LOG_LEVEL) << "worker PushSharedPageCursors done";
    // We reply to the client at this point.
    RETURN_IF_NOT_OK(serverApi->Write(pushRspPb));
    return Status::OK();
}

#endif
