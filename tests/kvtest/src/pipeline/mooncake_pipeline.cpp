#include "mooncake_pipeline.h"
#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

class MooncakeSamples {
public:
    explicit MooncakeSamples(const std::string &directory)
        : path_(directory + "/api_latency_samples"), output_(path_ + ".csv", std::ios::trunc) {
        if (!output_) throw std::runtime_error("Cannot open Mooncake latency sample file");
        output_ << "op,latency_us,status_code\n";
        pending_.reserve(kCapacity);
        writer_ = std::thread([this] { Flush(); });
    }

    ~MooncakeSamples() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        ready_.notify_one();
        writer_.join();
        std::ofstream status(path_ + ".status.txt");
        status << "{\"dropped_samples\":" << dropped_ << ",\"written_samples\":" << written_
               << ",\"io_error\":" << (ioError_ ? "true" : "false") << "}\n";
    }

    void Record(const char *op, double milliseconds, uint32_t code) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.size() == kCapacity) {
            ++dropped_;
            return;
        }
        pending_.push_back({op, static_cast<uint64_t>(std::max(0.0, milliseconds * 1000)), code});
        if (pending_.size() == 4096) ready_.notify_one();
    }

private:
    struct Sample { const char *op; uint64_t latencyUs; uint32_t code; };
    static constexpr size_t kCapacity = 65536;

    void Flush() {
        std::vector<Sample> batch;
        batch.reserve(kCapacity);
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait_for(lock, std::chrono::seconds(1),
                                [this] { return stopped_ || pending_.size() >= 4096; });
                if (stopped_ && pending_.empty()) break;
                batch.swap(pending_);
            }
            for (const auto &sample : batch) {
                output_ << sample.op << ',' << sample.latencyUs << ',' << sample.code << '\n';
            }
            output_.flush();
            if (output_) written_ += batch.size();
            else ioError_ = true;
            batch.clear();
        }
        output_.close();
        if (output_.fail()) ioError_ = true;
    }

    std::string path_;
    std::ofstream output_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<Sample> pending_;
    bool stopped_ = false;
    uint64_t dropped_ = 0;
    uint64_t written_ = 0;
    bool ioError_ = false;
    std::thread writer_;
};

class NativeMooncakePipeline final : public MooncakePipeline {
public:
    NativeMooncakePipeline(const Config &cfg, std::shared_ptr<MooncakeKvClient> client)
        : client_(std::move(client)), samples_(cfg.outputDir) {
        if (!client_) throw std::invalid_argument("Mooncake client is required");
    }

    OpFunc GetOp(const std::string &name) override {
        if (name == "put") return [this](PipelineContext &ctx, double &latencyMs) { return Put(ctx, latencyMs); };
        if (name == "get_buffer") return [this](PipelineContext &ctx, double &latencyMs) { return Get(ctx, latencyMs); };
        return nullptr;
    }

private:
    datasystem::Status Put(PipelineContext &ctx, double &latencyMs) {
        const auto start = std::chrono::steady_clock::now();
        const bool ok = client_->Put(ctx.key, ctx.data);
        latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const auto rc = ok ? datasystem::Status::OK()
                           : datasystem::Status(datasystem::K_RUNTIME_ERROR, "Mooncake put failed");
        samples_.Record("put", latencyMs, rc.GetCode());
        return rc;
    }

    datasystem::Status Get(PipelineContext &ctx, double &latencyMs) {
        MooncakeReadResult result;
        const auto start = std::chrono::steady_clock::now();
        const bool ok = client_->Get(ctx.key, result);
        latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        auto rc = datasystem::Status::OK();
        if (!ok || !result.owner || (!result.data && result.size != 0)) {
            rc = datasystem::Status(datasystem::K_RUNTIME_ERROR, "Mooncake get_buffer failed");
        } else if (!VerifyBuffer(result.data, result.size, ctx.size, ctx.senderId, ctx.verifyCfg)) {
            if (ctx.verifyFailCount) ++(*ctx.verifyFailCount);
            if (ctx.verifyCfg.failOp) rc = datasystem::Status(datasystem::K_INVALID, "Mooncake get_buffer verification failed");
        }
        samples_.Record("get_buffer", latencyMs, rc.GetCode());
        return rc;
    }

    std::shared_ptr<MooncakeKvClient> client_;
    MooncakeSamples samples_;
};

}

std::unique_ptr<MooncakePipeline> CreateMooncakePipeline(
    const Config &cfg, std::shared_ptr<MooncakeKvClient> client) {
    return std::make_unique<NativeMooncakePipeline>(cfg, std::move(client));
}
