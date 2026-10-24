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

#ifdef DS_OC_QUERY_CLOCK_BODY
uint64_t GetSteadyTimeUs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}
#endif

#ifdef DS_OC_QUERY_METHOD_BODY
Status WorkerQueryAndGetImpl::DS_OC_QUERY_ENTRY(QueryAndGet,
    std::shared_ptr<ServerUnaryWriterReader<QueryAndGetRspPb, QueryAndGetReqPb>> &serverApi)
{
    RequestState state;
    state.startUs = GetSteadyTimeUs();
    state.lastCheckpointUs = state.startUs;
    auto access = AccessRecorder::Object(AccessRecorderKey::DS_POSIX_QUERY_AND_GET);
    Status rc = DS_OC_QUERY_FORWARD(ReadAndAuthenticate, serverApi, state.request);
    state.stats.preprocessUs = RecordPhase(state);
    if (rc.IsOk()) {
        access.ObjectKeysRef(state.request.object_keys()).TransportType(GetTransportName(state.request));
        rc = ProcessAndDeliver(serverApi, state);
    }
    const uint64_t totalUs = state.lastCheckpointUs - state.startUs;
    access.DataSize(state.stats.dataSize).Result(rc).Record();
    LogCompletion(state, rc, totalUs);
    return rc;
}

Status WorkerQueryAndGetImpl::DS_OC_QUERY_ENTRY(ReadAndAuthenticate,
    const std::shared_ptr<ServerUnaryWriterReader<QueryAndGetRspPb, QueryAndGetReqPb>> &serverApi,
    QueryAndGetReqPb &request) const
{
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(serverApi->Read(request), "Read QueryAndGet request failed");
    RETURN_RUNTIME_ERROR_IF_NULL(akSkManager_);
    RETURN_IF_NOT_OK_PRINT_ERROR_MSG(DS_OC_QUERY_VERIFY(akSkManager_, request), "AK/SK failed.");
    return Status::OK();
}
#endif
