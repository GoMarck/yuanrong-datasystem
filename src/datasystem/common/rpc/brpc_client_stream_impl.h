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

/**
 * Description: brpc client-side streaming adapters for single-streaming modes.
 *
 * BrpcClientWriterImpl<W> — client-streaming (stream→unary):
 *   The first request establishes the stream through the RPC body; later writes
 *   use the stream. Read ends writes and receives the final response on the stream.
 *
 * BrpcClientReaderImpl<R> — server-streaming (unary→stream):
 *   Client sends a single request via CallMethod, receives N response
 *   messages via the stream's on_received_messages callback.
 */
#ifndef DATASYSTEM_COMMON_RPC_BRPC_CLIENT_STREAM_IMPL_H
#define DATASYSTEM_COMMON_RPC_BRPC_CLIENT_STREAM_IMPL_H

#include <atomic>
#include <condition_variable>
#include <bthread/mutex.h>
#include <bthread/condition_variable.h>
#include <deque>
#include <mutex>

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <brpc/stream.h>
#include <butil/iobuf.h>
#include <google/protobuf/message.h>
#include "datasystem/common/log/log.h"
#include "datasystem/common/rpc/trace_attachment.h"
#include "datasystem/common/rpc/brpc_perf_trace.h"
#include "datasystem/common/rpc/brpc_status_util.h"
#include "datasystem/common/rpc/brpc_stream_close_helper.h"
#include "datasystem/common/rpc/brpc_single_stream.h"
#include "datasystem/common/rpc/rpc_message.h"
#include "datasystem/common/rpc/mem_view.h"
#include "datasystem/utils/status.h"

namespace datasystem {

class BrpcClientWriterState final : public brpc::StreamInputHandler {
public:
    int on_received_messages(brpc::StreamId, butil::IOBuf *const messages[], size_t size) override
    {
        std::lock_guard<bthread::Mutex> lock(mutex);
        for (size_t i = 0; i < size; ++i) {
            if (messages[i] != nullptr) {
                replies.emplace_back(std::move(*messages[i]));
            }
        }
        condition.notify_all();
        return 0;
    }
    void on_idle_timeout(brpc::StreamId) override
    {
        std::lock_guard<bthread::Mutex> lock(mutex);
        failed = true;
        condition.notify_all();
    }
    void on_failed(brpc::StreamId id, int, const std::string &) override { on_idle_timeout(id); }
    void on_closed(brpc::StreamId) override
    {
        {
            std::lock_guard<bthread::Mutex> lock(mutex);
            closed = true;
            condition.notify_all();
        }
        keepalive.reset();
    }

    bthread::Mutex mutex;
    bthread::ConditionVariable condition;
    std::deque<butil::IOBuf> replies;
    bool closed = false;
    bool failed = false;
    std::unique_ptr<brpc::Controller> controller;
    std::shared_ptr<BrpcClientWriterState> keepalive;
};

template <typename W>
class BrpcClientWriterImpl {
public:
    BrpcClientWriterImpl(brpc::Channel *channel, const google::protobuf::MethodDescriptor *method, int32_t timeoutMs)
        : channel_(channel), method_(method), timeoutMs_(timeoutMs),
          trace_(Trace::Instance().GetTraceID(), method == nullptr ? "unknown" : method->full_name())
    {
    }
    ~BrpcClientWriterImpl()
    {
        Close();
        if (ShouldRecordBrpcTraceOnDestroy(trace_)) {
            RecordTraceOnce();
        }
    }
    BrpcClientWriterImpl(const BrpcClientWriterImpl &) = delete;
    BrpcClientWriterImpl &operator=(const BrpcClientWriterImpl &) = delete;

    Status Write(const W &pb)
    {
        CHECK_FAIL_RETURN_STATUS(!writesEnded_, K_RUNTIME_ERROR, "Write called after client-streaming writes ended.");
        if (!state_) {
            CHECK_FAIL_RETURN_STATUS(channel_ != nullptr && method_ != nullptr, K_INVALID,
                                     "Missing client-streaming channel or method.");
            const auto *prototype =
                google::protobuf::MessageFactory::generated_factory()->GetPrototype(method_->output_type());
            CHECK_FAIL_RETURN_STATUS(prototype != nullptr, K_RUNTIME_ERROR,
                                     "Missing client-streaming response prototype.");
            deadline_ = butil::microseconds_from_now(
                static_cast<int64_t>(timeoutMs_ > 0 ? timeoutMs_ : kStreamReadTimeoutSec * 1000) * 1000);
            state_ = std::make_shared<BrpcClientWriterState>();
            state_->controller = std::make_unique<brpc::Controller>();
            state_->controller->set_timeout_ms(timeoutMs_ > 0 ? timeoutMs_ : kStreamReadTimeoutSec * 1000);
            brpc::StreamOptions options;
            options.handler = state_.get();
            state_->keepalive = state_;
            if (brpc::StreamCreate(&streamId_, *state_->controller, &options) != 0) {
                state_->keepalive.reset();
                state_.reset();
                RETURN_STATUS(K_RUNTIME_ERROR, "Failed to create client-streaming stream.");
            }
            std::unique_ptr<google::protobuf::Message> response(prototype->New());
            AttachTraceIDToAttachment(state_->controller->request_attachment());
            trace_.MarkClientStart();
            trace_.MarkClientSend();
            channel_->CallMethod(method_, state_->controller.get(), &pb, response.get(), nullptr);
            if (state_->controller->Failed()) {
                auto status = TryExtractStatusFromControllerError(state_->controller->ErrorText(),
                                                                  state_->controller->ErrorCode());
                return Fail(status);
            }
            return Status::OK();
        }
        std::string serialized;
        if (!pb.SerializeToString(&serialized)) {
            return Fail(Status(K_RUNTIME_ERROR, "Failed to serialize client-streaming request."));
        }
        butil::IOBuf frame;
        if (frame.append(&SINGLE_STREAM_DATA, 1) != 0 || frame.append(serialized) != 0) {
            return Fail(Status(K_OUT_OF_MEMORY, "Failed to buffer client-streaming request."));
        }
        auto status = WriteSingleStreamFrame(streamId_, frame, deadline_);
        return status.IsError() ? Fail(status) : status;
    }

    template <typename R>
    Status Read(R &pb)
    {
        auto status = AwaitResponse();
        if (status.IsError()) {
            Close();
            return status;
        }
        CHECK_FAIL_RETURN_STATUS(pb.ParseFromString(response_), K_RUNTIME_ERROR,
                                 "Failed to parse client-streaming response.");
        return Status::OK();
    }

    Status Finish()
    {
        if (!state_) {
            writesEnded_ = true;
            return Status::OK();
        }
        auto status = AwaitResponse();
        Close();
        return status;
    }

    Status SendPayload(const std::vector<MemView> &)
    {
        RETURN_STATUS(K_NOT_SUPPORTED, "SendPayload not supported in BrpcClientWriterImpl");
    }
    Status ReceivePayload(std::vector<RpcMessage> &)
    {
        RETURN_STATUS(K_NOT_SUPPORTED, "ReceivePayload not supported in BrpcClientWriterImpl");
    }

private:
    Status AwaitResponse()
    {
        if (responseReady_) {
            return responseStatus_;
        }
        CHECK_FAIL_RETURN_STATUS(state_ != nullptr, K_RUNTIME_ERROR, "No client-streaming RPC in flight.");
        Status endStatus;
        if (!writesEnded_) {
            writesEnded_ = true;
            butil::IOBuf end;
            if (end.append(SINGLE_STREAM_END_WRITES, sizeof(SINGLE_STREAM_END_WRITES) - 1) != 0) {
                return Fail(Status(K_OUT_OF_MEMORY, "Failed to buffer client-streaming end marker."));
            }
            endStatus = WriteSingleStreamFrame(streamId_, end, deadline_);
        }
        std::unique_lock<bthread::Mutex> lock(state_->mutex);
        while (state_->replies.empty() && !state_->closed && !state_->failed) {
            if (state_->condition.wait_until(lock, deadline_) == ETIMEDOUT) {
                responseStatus_ = Status(K_RPC_DEADLINE_EXCEEDED, "Client-streaming response deadline exceeded.");
                break;
            }
        }
        if (!state_->replies.empty()) {
            response_ = state_->replies.front().to_string();
            if (!response_.empty() && response_.front() == SINGLE_STREAM_DATA) {
                response_.erase(0, 1);
            } else if (response_.compare(0, 8, "\x01" "DS_ERR:") == 0) {
                responseStatus_ = TryExtractStatusFromControllerError(response_);
            } else {
                responseStatus_ = Status(K_INVALID, "Invalid client-streaming response frame.");
            }
        } else if (responseStatus_.IsOk()) {
            responseStatus_ = endStatus.IsError()
                                  ? endStatus : Status(K_RPC_CANCELLED, "Stream closed without a response.");
        }
        responseReady_ = true;
        lock.unlock();
        trace_.MarkClientRecv();
        RecordTraceOnce();
        return responseStatus_;
    }

    Status Fail(const Status &status)
    {
        responseStatus_ = status;
        responseReady_ = true;
        writesEnded_ = true;
        trace_.MarkClientRecv();
        RecordTraceOnce();
        Close();
        return status;
    }

    void Close()
    {
        if (streamId_ != brpc::INVALID_STREAM_ID) {
            brpc::StreamClose(streamId_);
            streamId_ = brpc::INVALID_STREAM_ID;
        }
    }
    void RecordTraceOnce()
    {
        if (!traceRecorded_) {
            traceRecorded_ = true;
            trace_.MarkClientEnd();
            RecordBrpcRpcTrace(trace_);
        }
    }

    brpc::Channel *channel_;
    const google::protobuf::MethodDescriptor *method_;
    int32_t timeoutMs_;
    brpc::StreamId streamId_ = brpc::INVALID_STREAM_ID;
    std::shared_ptr<BrpcClientWriterState> state_;
    timespec deadline_{};
    bool writesEnded_ = false;
    bool responseReady_ = false;
    bool traceRecorded_ = false;
    std::string response_;
    Status responseStatus_;
    BrpcPerfTrace trace_;
};

/**
 * @brief brpc adapter for server-streaming (unary→stream) on the client side.
 *
 * The client sends a single request via CallMethod, then receives N
 * response messages through the stream's on_received_messages callback.
 *
 * @tparam R Read (response) protobuf type that the client reads from the stream.
 */
template <typename R>
class BrpcClientReaderImpl : public brpc::StreamInputHandler,
                            public std::enable_shared_from_this<BrpcClientReaderImpl<R>> {
public:
    BrpcClientReaderImpl(brpc::Channel *channel,
                         const google::protobuf::MethodDescriptor *method,
                         int32_t timeoutMs)
        : channel_(channel),
          method_(method),
          timeoutMs_(timeoutMs),
          streamId_(brpc::INVALID_STREAM_ID),
          writeOnce_(false),
          finished_(false),
          readReady_(false),
          streamEnd_(false),
          readError_(false),
          traceRecorded_(false),
          trace_(Trace::Instance().GetTraceID(), method == nullptr ? "unknown" : method->full_name())
    {
    }

    ~BrpcClientReaderImpl() override
    {
        // Under the keepalive model this destructor runs only after on_closed has
        // fired (keepalive_ cleared) — or when Write() was never called (stream not
        // created, keepalive_ unarmed). In both cases there is nothing to close;
        // brpc is already done with the raw handler pointer. Just record the trace.
        if (ShouldRecordBrpcTraceOnDestroy(trace_)) {
            RecordTraceOnce();
        }
    }

    // --- brpc::StreamInputHandler callbacks ---

    int on_received_messages(brpc::StreamId id, butil::IOBuf *const messages[], size_t size) override
    {
        (void)id;
        if (destroyed_->load(std::memory_order_acquire)) {
            return 0;  // stream logically closed; ignore racing late data
        }
        if (firstResponseTs_.exchange(BrpcTraceNowNs(), std::memory_order_acq_rel) == 0) {
            trace_.MarkClientRecv(firstResponseTs_.load(std::memory_order_relaxed));
            VLOG(1) << FormatString("ClientRecv ts %llu tid %d cpu %d bid %llu\n", trace_.ClientRecvTs(),
                                    BrpcTraceGetTid(), sched_getcpu(),
                                    static_cast<unsigned long long>(bthread_self()));
        }
        std::lock_guard<bthread::Mutex> lock(readMtx_);
        for (size_t i = 0; i < size; ++i) {
            if (messages[i] != nullptr) {
                pendingMessages_.push_back(std::move(*messages[i]));
            }
        }
        readReady_ = true;
        readCond_.notify_one();
        return 0;
    }

    void on_idle_timeout(brpc::StreamId id) override
    {
        (void)id;
        if (destroyed_->load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<bthread::Mutex> lock(readMtx_);
        readError_ = true;
        readCond_.notify_one();
    }

    void on_closed(brpc::StreamId id) override
    {
        (void)id;
        if (closeNotifier_) closeNotifier_->store(true, std::memory_order_relaxed);
        destroyed_->store(true, std::memory_order_release);
        {
            std::lock_guard<bthread::Mutex> lock(readMtx_);
            streamEnd_ = true;
            readReady_ = true;
            readCond_.notify_one();
        }
        // Drop self-keepalive LAST. on_closed is brpc's final callback, so once
        // this runs no further callback can touch this object. If the external
        // owner has already released its ref, this destroys this object here —
        // the lock is released and all member access is done, so it is safe.
        keepalive_.reset();
    }

    void on_failed(brpc::StreamId id, int error_code, const std::string &error_text) override
    {
        (void)id;
        (void)error_code;
        LOG(WARNING) << "BrpcClientReaderImpl stream failed: " << error_text;
        if (destroyed_->load(std::memory_order_acquire)) {
            return;
        }
        // Do NOT set closeNotifier_ here: brpc always fires on_closed after on_failed
        // (stream.cpp:594 -> :596); the Controller/handler are only safe to release once
        // on_closed (the final callback) has run.
        std::lock_guard<bthread::Mutex> lock(readMtx_);
        readError_ = true;
        readCond_.notify_one();
    }

    /**
     * @brief Send the request and set up the stream for reading responses.
     * @tparam W Request protobuf type.
     * @param[in] pb Request protobuf to send.
     * @return Status of the call.
     */
    template <typename W>
    Status Write(const W &pb)
    {
        bool expected = false;
        if (!writeOnce_.compare_exchange_strong(expected, true)) {
            RETURN_STATUS(StatusCode::K_RUNTIME_ERROR,
                "BrpcClientReaderImpl::Write called more than once");
        }

        cntl_ = std::make_unique<brpc::Controller>();
        if (timeoutMs_ > 0) {
            cntl_->set_timeout_ms(timeoutMs_);
        }

        brpc::StreamOptions options;
        options.handler = this;
        int rc = brpc::StreamCreate(&streamId_, *cntl_, &options);
        if (rc != 0) {
            cntl_.reset();
            RETURN_STATUS(StatusCode::K_RUNTIME_ERROR,
                "brpc::StreamCreate failed for server-streaming reader");
        }
        // Arm self-keepalive so this object outlives brpc's final on_closed
        // callback (fired asynchronously after StreamClose). Must run after the
        // object is managed by shared_ptr (post-construction).
        keepalive_ = this->shared_from_this();

        // Send the RPC with the request in the body.
        // The response is a placeholder — real responses arrive via the stream.
        R dummyResponse;
        AttachTraceIDToAttachment(cntl_->request_attachment());
        trace_.MarkClientStart();
        trace_.MarkClientSend();
        VLOG(1) << FormatString("ClientSend ts %llu tid %d cpu %d bid %llu\n", trace_.ClientSendTs(),
                                BrpcTraceGetTid(),
                                sched_getcpu(), static_cast<unsigned long long>(bthread_self()));
        channel_->CallMethod(method_, cntl_.get(), &pb, &dummyResponse, nullptr);
        if (cntl_->Failed()) {
            trace_.MarkClientRecv();
            VLOG(1) << FormatString("ClientRecv ts %llu tid %d cpu %d bid %llu\n", trace_.ClientRecvTs(),
                                    BrpcTraceGetTid(), sched_getcpu(),
                                    static_cast<unsigned long long>(bthread_self()));
            Status embedded = TryExtractStatusFromResponse(dummyResponse);
            if (embedded.IsError()) {
                RecordTraceOnce();
                Close();  // trigger async on_closed so the armed keepalive_ can release
                return embedded;
            }
            auto errText = cntl_->ErrorText();
            RecordTraceOnce();
            Close();  // trigger async on_closed so the armed keepalive_ can release
            // Wrap with RETURN_STATUS to retain this adapter's call-site
            // (file/line) for on-call; the helper's Status carries the brpc
            // errno/name + ErrorText diagnostics in its message.
            auto st = TryExtractStatusFromControllerError(errText, cntl_->ErrorCode());
            RETURN_STATUS(st.GetCode(), st.GetMsg());
        }

        return Status::OK();
    }

    /**
     * @brief Read one response from the stream.
     * @param[out] pb Response protobuf to fill.
     * @return Status of the call; K_RPC_STREAM_END when the server has finished.
     */
    Status Read(R &pb)
    {
        if (finished_.load()) {
            RETURN_STATUS(StatusCode::K_RPC_STREAM_END, "Stream already finished");
        }

        std::unique_lock<bthread::Mutex> lock(readMtx_);
        while (!readReady_ && !readError_ && !streamEnd_) {
            readCond_.wait(lock);
        }

        if (readError_) {
            RETURN_STATUS(StatusCode::K_RPC_CANCELLED, "Stream error during read");
        }

        // Check for pending messages
        if (!pendingMessages_.empty()) {
            butil::IOBuf msg = std::move(pendingMessages_.front());
            pendingMessages_.pop_front();
            if (pendingMessages_.empty() && !streamEnd_) {
                readReady_ = false;
            }
            lock.unlock();

            std::string serialized = msg.to_string();
            if (!pb.ParseFromString(serialized)) {
                RETURN_STATUS(StatusCode::K_RUNTIME_ERROR,
                    "Failed to parse protobuf from stream message");
            }
            return Status::OK();
        }

        // No pending messages and stream ended
        if (streamEnd_) {
            RecordTraceOnce();
            RETURN_STATUS(StatusCode::K_RPC_STREAM_END, "Stream ended");
        }

        RETURN_STATUS(StatusCode::K_RPC_CANCELLED, "Unexpected read state");
    }

    /**
     * @brief Signal reading is done. Closes the stream.
     * @return Status of the call.
     */
    // Non-blocking close: triggers brpc to fire on_closed asynchronously, which
    // drops the self-keepalive and lets this object be destroyed safely once brpc
    // is finished with the raw handler pointer. Called by the wrapper destructor
    // when the caller drops the stream without Finish().
    void Close()
    {
        brpc::StreamId id = brpc::INVALID_STREAM_ID;
        {
            std::lock_guard<bthread::Mutex> lock(readMtx_);
            id = streamId_;
            streamId_ = brpc::INVALID_STREAM_ID;
        }
        if (id != brpc::INVALID_STREAM_ID) {
            brpc::StreamClose(id);
        }
    }

    /**
     * @brief Signal reading is done. Closes the stream.
     * @return Status of the call.
     */
    Status Finish()
    {
        if (finished_.exchange(true)) {
            return Status::OK();
        }
        // Bounded wait for on_closed so callers retain blocking close semantics.
        // On timeout the object stays alive via keepalive_ until the (possibly
        // late) on_closed fires, so a timeout here is safe (no UAF).
        (void)StreamCloseAndWait(
            {streamId_, readMtx_, readCond_, streamEnd_, readError_, closeNotifier_, kDefaultDeferredWaitSec});
        RecordTraceOnce();
        return Status::OK();
    }

    Status SendPayload(const std::vector<MemView> &payload)
    {
        (void)payload;
        RETURN_STATUS(StatusCode::K_NOT_SUPPORTED,
            "SendPayload not supported in BrpcClientReaderImpl");
    }

    Status ReceivePayload(std::vector<RpcMessage> &recvBuffer)
    {
        (void)recvBuffer;
        RETURN_STATUS(StatusCode::K_NOT_SUPPORTED,
            "ReceivePayload not supported in BrpcClientReaderImpl");
    }

private:
    brpc::Channel *channel_;
    const google::protobuf::MethodDescriptor *method_;
    int32_t timeoutMs_;
    std::unique_ptr<brpc::Controller> cntl_;
    brpc::StreamId streamId_;
    std::atomic<bool> writeOnce_;
    std::atomic<bool> finished_;

    // Read synchronization
    bthread::Mutex readMtx_;
    bthread::ConditionVariable readCond_;
    std::deque<butil::IOBuf> pendingMessages_;
    bool readReady_;
    bool streamEnd_;
    bool readError_;
    std::shared_ptr<std::atomic<bool>> closeNotifier_ =
        std::make_shared<std::atomic<bool>>(false);
    // Defense-in-depth: set when the stream is logically closed so that any
    // racing callback (on_received_messages/on_idle_timeout) can bail out early
    // instead of touching state after the consumer is gone. The object itself
    // stays alive until on_closed via keepalive_, so this guards logic, not UAF.
    std::shared_ptr<std::atomic<bool>> destroyed_ =
        std::make_shared<std::atomic<bool>>(false);
    // Self-keepalive: armed after StreamCreate, cleared at the tail of on_closed
    // (brpc's guaranteed-final callback). Keeps this object (and its cntl_) alive
    // until brpc is finished with the raw handler pointer, closing the UAF window.
    std::shared_ptr<BrpcClientReaderImpl<R>> keepalive_;
    std::atomic<uint64_t> firstResponseTs_ { 0 };
    std::atomic<bool> traceRecorded_;
    BrpcPerfTrace trace_;

    void RecordTraceOnce()
    {
        if (!traceRecorded_.exchange(true, std::memory_order_acq_rel)) {
            // Stream RPC tracing is one summary sample per stream, not one sample per message.
            trace_.MarkClientEnd();
            RecordBrpcRpcTrace(trace_);
        }
    }
};

}  // namespace datasystem
#endif  // DATASYSTEM_COMMON_RPC_BRPC_CLIENT_STREAM_IMPL_H
