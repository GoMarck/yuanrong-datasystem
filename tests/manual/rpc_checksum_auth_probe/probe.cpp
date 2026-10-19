#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>

#include <bthread/bthread.h>
#include <brpc/channel.h>
#include <brpc/checksum.h>
#include <brpc/closure_guard.h>
#include <brpc/data_factory.h>
#include <brpc/nonreflectable_message.h>
#include <brpc/server.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "probe.pb.h"

namespace {
constexpr auto kAuthChecksum = static_cast<brpc::ChecksumType>(1001);
constexpr auto kUnknownChecksum = static_cast<brpc::ChecksumType>(1002);
using Digest = std::array<unsigned char, 32>;
std::atomic<int> computes{0}, verifies{0}, borrows{0}, resets{0}, business{0};
bthread_key_t credentialKey;

void Require(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL " << message << std::endl;
        std::exit(1);
    }
}

int64_t Now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

Digest Hash(const butil::IOBuf &bytes)
{
    Digest digest{};
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    Require(ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1, "digest init");
    for (size_t i = 0; i < bytes.backing_block_num(); ++i) {
        const auto block = bytes.backing_block(i);
        Require(EVP_DigestUpdate(ctx, block.data(), block.size()) == 1, "digest update");
    }
    unsigned int size = 0;
    Require(EVP_DigestFinal_ex(ctx, digest.data(), &size) == 1 && size == digest.size(), "digest final");
    EVP_MD_CTX_free(ctx);
    return digest;
}

std::string Hex(const Digest &digest)
{
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    for (unsigned char c : digest) {
        result += alphabet[c >> 4];
        result += alphabet[c & 15];
    }
    return result;
}

void Append(std::string &output, const std::string &value)
{
    uint64_t size = value.size();
    for (int shift = 56; shift >= 0; shift -= 8) {
        output += static_cast<char>((size >> shift) & 255);
    }
    output += value;
}

std::string Mac(const brpc::Controller &cntl, const std::string &method, const std::string &ak,
                const std::string &timestamp, const Digest &digest)
{
    const std::string key = ak == "test-ak-a" ? "test-only-secret-a" : "test-only-secret-b";
    std::string input;
    Append(input, method);
    Append(input, ak);
    Append(input, timestamp);
    Append(input, std::to_string(cntl.request_compress_type()));
    Append(input, std::to_string(cntl.request_content_type()));
    Append(input, std::string(reinterpret_cast<const char *>(digest.data()), digest.size()));
    Digest mac{};
    unsigned int size = 0;
    Require(HMAC(EVP_sha256(), key.data(), key.size(), reinterpret_cast<const unsigned char *>(input.data()),
                 input.size(), mac.data(), &size) != nullptr && size == mac.size(), "hmac");
    return Hex(mac);
}

struct Capture {
    brpc::Controller *owner = nullptr;
    bool valid = false;
    Digest digest{};
    size_t bytes = 0;
    size_t blocks = 0;
};

class CaptureFactory final : public brpc::DataFactory {
public:
    void *CreateData() const override { return new Capture; }
    void DestroyData(void *data) const override { delete static_cast<Capture *>(data); }
    bool ResetData(void *data) const override
    {
        *static_cast<Capture *>(data) = Capture{};
        ++resets;
        return true;
    }
};

Capture *Local(brpc::Controller *cntl)
{
    ++borrows;
    return static_cast<Capture *>(cntl->session_local_data());
}

enum class Fault { None, BadBodyDigest, BadMac, BadAk, BadTime, BadMethod, Expired, SkipCapture };
struct Credentials {
    brpc::Controller *owner;
    std::string ak;
    Fault fault;
    bool computed = false;
    Digest digest{};
    size_t bytes = 0;
};

class CredentialScope {
public:
    explicit CredentialScope(Credentials *credentials) : previous_(bthread_getspecific(credentialKey))
    {
        Require(bthread_setspecific(credentialKey, credentials) == 0, "credential scope set");
    }
    ~CredentialScope() { Require(bthread_setspecific(credentialKey, previous_) == 0, "credential scope restore"); }
private:
    void *previous_;
};

void Compute(const brpc::ChecksumIn &in)
{
    ++computes;
    auto *credentials = static_cast<Credentials *>(bthread_getspecific(credentialKey));
    if (!credentials || credentials->owner != in.cntl || !in.cntl->method()) {
        in.cntl->SetFailed("missing local credential scope");
        return;
    }
    credentials->computed = true;
    credentials->digest = Hash(*in.buf);
    credentials->bytes = in.buf->size();
    Digest digest = credentials->digest;
    if (credentials->fault == Fault::BadBodyDigest) {
        digest[0] ^= 1;
    }
    auto &fields = *in.cntl->request_user_fields();
    fields["ak"] = credentials->ak;
    fields["timestamp"] = std::to_string(Now() - (credentials->fault == Fault::Expired ? 3600 : 0));
    const std::string method = credentials->fault == Fault::BadMethod
                                   ? checksum_probe::AuthProbe::descriptor()->FindMethodByName("Other")->full_name()
                                   : in.cntl->method()->full_name();
    fields["signature"] = Mac(*in.cntl, method, fields["ak"], fields["timestamp"], digest);
    fields["verified"] = "true";
    switch (credentials->fault) {
        case Fault::BadMac: fields["signature"][0] ^= 1; break;
        case Fault::BadAk: fields["ak"] = "test-ak-b"; break;
        case Fault::BadTime: fields["timestamp"] = std::to_string(Now() + 1); break;
        case Fault::SkipCapture: fields["test_skip_capture"] = "true"; break;
        default: break;
    }
}

bool Verify(const brpc::ChecksumIn &in)
{
    ++verifies;
    if (in.cntl->request_user_fields()->seek("test_skip_capture")) {
        return true;  // Test-only fault injection: transport hook succeeds without recording a digest.
    }
    Capture *capture = Local(in.cntl);
    if (!capture || !in.cntl->method()) {
        return false;
    }
    capture->owner = in.cntl;
    capture->digest = Hash(*in.buf);
    capture->bytes = in.buf->size();
    capture->blocks = in.buf->backing_block_num();
    capture->valid = true;
    return true;
}

class CountingRequest final : public brpc::NonreflectableMessage<CountingRequest> {
public:
    CountingRequest() = default;
    explicit CountingRequest(const google::protobuf::Message *message) : message_(message) {}
    void MergeFrom(const CountingRequest &other) override { message_ = other.message_; }
    size_t ByteSizeLong() const override { return message_->ByteSizeLong(); }
    uint8_t *_InternalSerialize(uint8_t *target, google::protobuf::io::EpsCopyOutputStream *stream) const override
    {
        ++serializations;
        return message_->_InternalSerialize(target, stream);
    }
    mutable int serializations = 0;
private:
    const google::protobuf::Message *message_ = nullptr;
};

class AuthService final : public checksum_probe::AuthProbe {
public:
    void Echo(google::protobuf::RpcController *controller, const checksum_probe::RequestV1 *request,
              checksum_probe::Reply *reply, google::protobuf::Closure *done) override
    {
        brpc::ClosureGuard guard(done);
        auto *cntl = static_cast<brpc::Controller *>(controller);
        if (cntl->request_checksum_type() != kAuthChecksum) {
            cntl->SetFailed("auth checksum required");
            return;
        }
        Capture *capture = Local(cntl);
        auto &fields = *cntl->request_user_fields();
        const std::string timestamp = fields["timestamp"];
        const bool validTime = !timestamp.empty() && timestamp.find_first_not_of("0123456789") == std::string::npos
                               && timestamp.size() <= 10 && std::llabs(Now() - std::atoll(timestamp.c_str())) <= 60;
        if (!capture || !capture->valid || capture->owner != cntl || !validTime
            || (fields["ak"] != "test-ak-a" && fields["ak"] != "test-ak-b")) {
            cntl->SetFailed("missing trusted capture or credential");
            return;
        }
        const std::string expected = Mac(*cntl, cntl->method()->full_name(), fields["ak"], timestamp, capture->digest);
        if (fields["signature"].size() != expected.size()
            || CRYPTO_memcmp(fields["signature"].data(), expected.data(), expected.size()) != 0) {
            cntl->SetFailed("signature mismatch");
            return;
        }
        ++business;
        reply->set_digest(Hex(capture->digest));
        reply->set_wire_size(capture->bytes);
        reply->set_blocks(capture->blocks);
        reply->set_unknown_seen(request->GetReflection()->GetUnknownFields(*request).field_count() != 0);
        butil::IOBuf reencoded;
        reencoded.append(request->SerializeAsString());
        reply->set_reencoded_differs(Hash(reencoded) != capture->digest);
    }
    void Other(google::protobuf::RpcController *controller, const checksum_probe::RequestV1 *request,
               checksum_probe::Reply *reply, google::protobuf::Closure *done) override
    {
        Echo(controller, request, reply, done);
    }
};

class PlainService final : public checksum_probe::PlainProbe {
public:
    void Echo(google::protobuf::RpcController *, const checksum_probe::RequestV1 *,
              checksum_probe::Reply *, google::protobuf::Closure *done) override
    {
        brpc::ClosureGuard guard(done);
    }
};

void Pass(const std::string &name) { std::cout << "PASS " << name << std::endl; }

void Call(brpc::Channel &channel, const google::protobuf::Message &message, bool auth, Fault fault = Fault::None,
          bool asynchronous = false, bool compressed = false, const std::string &ak = "test-ak-a",
          brpc::ChecksumType checksum = kAuthChecksum)
{
    brpc::Controller cntl;
    cntl.set_timeout_ms(3000);
    cntl.set_max_retry(0);
    if (auth) {
        cntl.set_request_checksum_type(checksum);
    }
    if (compressed) {
        cntl.set_request_compress_type(brpc::COMPRESS_TYPE_GZIP);
    }
    CountingRequest counted(&message);
    checksum_probe::Reply reply;
    Credentials credentials{&cntl, ak, fault};
    const auto *method = auth ? checksum_probe::AuthProbe::descriptor()->method(0)
                              : checksum_probe::PlainProbe::descriptor()->method(0);
    brpc::CallId callId = cntl.call_id();
    {
        CredentialScope scope(&credentials);
        channel.CallMethod(method, &cntl, &counted, &reply, asynchronous ? brpc::DoNothing() : nullptr);
        Require(counted.serializations == 1, "exactly one business request serialization");
        if (auth && checksum == kAuthChecksum) {
            Require(credentials.computed, "compute completed inside CallMethod scope");
        }
    }
    if (asynchronous) {
        brpc::Join(callId);
    }
    const bool success = !cntl.Failed();
    const bool expected = !auth || (fault == Fault::None && checksum == kAuthChecksum);
    Require(success == expected, std::string("RPC outcome: ") + cntl.ErrorText());
    if (success && auth) {
        Require(reply.digest() == Hex(credentials.digest), "sender and receiver wire digest equal");
        Require(reply.wire_size() == credentials.bytes, "wire body size preserved");
        if (message.GetDescriptor() == checksum_probe::RequestV2::descriptor()) {
            Require(reply.unknown_seen(), "old schema retained new field");
            Require(reply.reencoded_differs(), "old schema reencoding actually changes digest");
        }
        if (message.ByteSizeLong() > 65536 && !compressed) {
            Require(reply.blocks() > 1, "segmented IOBuf traversed without flattening");
        }
        if (compressed) {
            Require(reply.wire_size() < message.ByteSizeLong(), "hash covers compressed bytes");
        }
    }
}

class OffPathStub {
public:
    explicit OffPathStub(brpc::Channel &channel)
        : channel_(channel), plain_(checksum_probe::PlainProbe::descriptor()->method(0)),
          auth_(checksum_probe::AuthProbe::descriptor()->method(0)),
          enabled_(std::getenv("CHECKSUM_PROBE_AUTH_ENABLED") != nullptr) {}

    __attribute__((noinline, noclone)) void Original(brpc::Controller *cntl, const checksum_probe::RequestV1 *request,
                                                  checksum_probe::Reply *reply, google::protobuf::Closure *done)
    {
        channel_.CallMethod(plain_, cntl, request, reply, done);
    }

    __attribute__((noinline, noclone)) void WithModeChoice(brpc::Controller *cntl,
                                                        const checksum_probe::RequestV1 *request,
                                                        checksum_probe::Reply *reply, google::protobuf::Closure *done)
    {
        if (enabled_) {
            cntl->set_request_checksum_type(kAuthChecksum);
            Credentials credentials{cntl, "test-ak-a", Fault::None};
            CredentialScope scope(&credentials);
            channel_.CallMethod(auth_, cntl, request, reply, done);
            return;
        }
        channel_.CallMethod(plain_, cntl, request, reply, done);
    }
private:
    brpc::Channel &channel_;
    const google::protobuf::MethodDescriptor *plain_;
    const google::protobuf::MethodDescriptor *auth_;
    bool enabled_;
};

class BenchFactory final : public brpc::DataFactory {
public:
    void *CreateData() const override { ++created; return new Capture; }
    void DestroyData(void *data) const override { ++destroyed; delete static_cast<Capture *>(data); }
    bool ResetData(void *data) const override { ++reset; *static_cast<Capture *>(data) = Capture{}; return true; }
    mutable std::atomic<int> created{0}, reset{0}, destroyed{0};
};

int64_t MonotonicNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

double CpuSeconds(const rusage &usage)
{
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec
           + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000.0;
}

struct BenchSamples {
    std::vector<int64_t> latency;
    uint64_t errors = 0;
    int64_t finish = 0;
};

int RunBenchmark(char **argv)
{
    const std::string mode = argv[2];
    const bool disabled = mode == "disabled";
    Require(disabled || mode == "baseline", "benchmark mode");
    Require(std::getenv("CHECKSUM_PROBE_AUTH_ENABLED") == nullptr, "benchmark must keep AK/SK disabled");
    const size_t bytes = std::stoull(argv[3]);
    const int clients = std::stoi(argv[4]);
    const std::string style = argv[5];
    const bool asynchronous = style == "async";
    const int seconds = std::stoi(argv[6]), warmup = std::stoi(argv[7]);
    Require(bytes <= 1024 * 1024 && clients > 0 && clients <= 32 && seconds > 0 && warmup >= 0,
            "benchmark bounds");
    Require(asynchronous || style == "sync", "benchmark call style");
    if (disabled) {
        Require(bthread_key_create(&credentialKey, nullptr) == 0, "benchmark credential key init");
        Require(brpc::RegisterChecksumHandler(kAuthChecksum, {Compute, Verify, "test-only-auth-sha256"}) == 0,
                "benchmark checksum registration");
    }
    BenchFactory factory;
    PlainService plain;
    AuthService auth;
    brpc::Server server;
    Require(server.AddService(&plain, brpc::SERVER_DOESNT_OWN_SERVICE) == 0, "benchmark plain service");
    if (disabled) {
        Require(server.AddService(&auth, brpc::SERVER_DOESNT_OWN_SERVICE) == 0, "benchmark auth service");
    }
    brpc::ServerOptions options;
    options.num_threads = 8;
    options.has_builtin_services = false;
    options.session_local_data_factory = disabled ? &factory : nullptr;
    Require(server.Start("127.0.0.1:0", &options) == 0, "benchmark server start");
    brpc::Channel channel;
    brpc::ChannelOptions channelOptions;
    channelOptions.protocol = brpc::PROTOCOL_BAIDU_STD;
    channelOptions.connection_type = brpc::CONNECTION_TYPE_SINGLE;
    Require(channel.Init(server.listen_address(), &channelOptions) == 0, "benchmark channel init");
    OffPathStub stub(channel);
    using Send = void (OffPathStub::*)(brpc::Controller *, const checksum_probe::RequestV1 *,
                                      checksum_probe::Reply *, google::protobuf::Closure *);
    const Send send = disabled ? &OffPathStub::WithModeChoice : &OffPathStub::Original;
    std::vector<BenchSamples> samples(clients);
    std::mutex mutex;
    std::condition_variable readyCondition, startCondition;
    int ready = 0;
    bool start = false;
    int64_t deadline = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < clients; ++i) {
        threads.emplace_back([&, i] {
            checksum_probe::RequestV1 request;
            request.set_payload(std::string(bytes, 'x'));
            request.set_known(3);
            auto invoke = [&] {
                brpc::Controller cntl;
                cntl.set_timeout_ms(3000);
                cntl.set_max_retry(0);
                checksum_probe::Reply reply;
                const brpc::CallId callId = asynchronous ? cntl.call_id() : brpc::CallId{};
                (stub.*send)(&cntl, &request, &reply, asynchronous ? brpc::DoNothing() : nullptr);
                if (asynchronous) {
                    brpc::Join(callId);
                }
                return !cntl.Failed();
            };
            samples[i].latency.reserve(4 * 1024 * 1024);
            const int64_t warmDeadline = MonotonicNs() + int64_t(warmup) * 1000000000;
            do {
                Require(invoke(), "benchmark warmup RPC");
            } while (MonotonicNs() < warmDeadline);
            {
                std::unique_lock<std::mutex> lock(mutex);
                ++ready;
                readyCondition.notify_one();
                startCondition.wait(lock, [&] { return start; });
            }
            do {
                const int64_t begin = MonotonicNs();
                if (!invoke()) {
                    ++samples[i].errors;
                }
                samples[i].finish = MonotonicNs();
                samples[i].latency.push_back(samples[i].finish - begin);
            } while (samples[i].finish < deadline);
        });
    }
    rusage cpuBegin{}, cpuEnd{};
    int64_t begin = 0;
    {
        std::unique_lock<std::mutex> lock(mutex);
        readyCondition.wait(lock, [&] { return ready == clients; });
        Require(getrusage(RUSAGE_SELF, &cpuBegin) == 0, "benchmark cpu start");
        begin = MonotonicNs();
        deadline = begin + int64_t(seconds) * 1000000000;
        start = true;
        startCondition.notify_all();
    }
    for (auto &thread : threads) {
        thread.join();
    }
    Require(getrusage(RUSAGE_SELF, &cpuEnd) == 0, "benchmark cpu end");
    int64_t finish = begin;
    uint64_t errors = 0;
    std::vector<int64_t> latency;
    for (const auto &sample : samples) {
        finish = std::max(finish, sample.finish);
        errors += sample.errors;
        latency.insert(latency.end(), sample.latency.begin(), sample.latency.end());
    }
    std::sort(latency.begin(), latency.end());
    const double wall = (finish - begin) / 1000000000.0;
    const double cpu = CpuSeconds(cpuEnd) - CpuSeconds(cpuBegin);
    const auto quantile = [&](double q) { return latency[static_cast<size_t>(q * (latency.size() - 1))] / 1000.0; };
    double sum = 0;
    for (auto ns : latency) {
        sum += ns;
    }
    Require(errors == 0, "benchmark RPC errors");
    Require(computes == 0 && verifies == 0 && borrows == 0 && business == 0 && factory.created == 0
                && factory.reset == 0 && factory.destroyed == 0, "disabled path unexpectedly did auth work");
    std::cout << std::setprecision(12)
              << "BENCH {\"mode\":\"" << mode << "\",\"bytes\":" << bytes << ",\"clients\":" << clients
              << ",\"style\":\"" << style << "\",\"operations\":" << latency.size()
              << ",\"errors\":" << errors << ",\"wall_s\":" << wall << ",\"qps\":" << latency.size() / wall
              << ",\"mean_us\":" << sum / latency.size() / 1000 << ",\"p50_us\":" << quantile(.50)
              << ",\"p99_us\":" << quantile(.99) << ",\"cpu_s\":" << cpu
              << ",\"cpu_us_per_op\":" << cpu * 1000000 / latency.size()
              << ",\"peak_rss_kib\":" << cpuEnd.ru_maxrss
              << ",\"compute\":" << computes << ",\"verify\":" << verifies << ",\"local_access\":" << borrows
              << ",\"factory_create\":" << factory.created << ",\"factory_reset\":" << factory.reset
              << "}" << std::endl;
    server.Stop(0);
    server.Join();
    if (disabled) {
        bthread_key_delete(credentialKey);
    }
    return 0;
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc == 8 && std::string(argv[1]) == "--bench") {
        return RunBenchmark(argv);
    }
    Require(argc == 1, "usage: probe [--bench baseline|disabled bytes clients sync|async seconds warmup]");
    Require(bthread_key_create(&credentialKey, nullptr) == 0, "credential key create");
    Require(brpc::RegisterChecksumHandler(kAuthChecksum, {Compute, Verify, "test-only-auth-sha256"}) == 0,
            "checksum registration");
    CaptureFactory factory;
    PlainService plain;
    AuthService auth;
    brpc::Server server;
    Require(server.AddService(&plain, brpc::SERVER_DOESNT_OWN_SERVICE) == 0, "plain service registration");
    Require(server.AddService(&auth, brpc::SERVER_DOESNT_OWN_SERVICE) == 0, "auth service registration");
    brpc::ServerOptions options;
    options.session_local_data_factory = &factory;
    Require(server.Start("127.0.0.1:0", &options) == 0, "server start");
    brpc::Channel channel;
    brpc::ChannelOptions channelOptions;
    channelOptions.protocol = brpc::PROTOCOL_BAIDU_STD;
    Require(channel.Init(server.listen_address(), &channelOptions) == 0, "channel init");
    checksum_probe::RequestV1 request;
    request.set_payload("sample");
    request.set_known(3);
    Call(channel, request, false);
    Require(computes == 0 && verifies == 0 && borrows == 0, "off path has no auth callbacks or local state access");
    Pass("off_path_single_serialization_no_auth_work");
    Call(channel, request, true);
    Pass("signed_unary_single_serialization_native_meta");
    checksum_probe::RequestV2 v2;
    v2.set_payload("sample");
    v2.set_added(7);
    v2.set_known(3);
    Call(channel, v2, true);
    Pass("new_field_old_schema_raw_digest_matches_reencoded_digest_differs");
    Call(channel, request, true, Fault::None, true);
    Pass("async_compute_finishes_before_credential_scope_exit");
    const int accepted = business;
    for (auto fault : {Fault::BadBodyDigest, Fault::BadMac, Fault::BadAk, Fault::BadTime, Fault::BadMethod,
                       Fault::Expired, Fault::SkipCapture}) {
        Call(channel, request, true, fault);
    }
    Require(business == accepted, "all invalid requests rejected before business side effect");
    Pass("wrong_body_digest_mac_ak_timestamp_method_expiry_missing_local_capture_rejected");
    Require(resets > 0, "session-local data reset observed");
    Pass("pooled_capture_reset_and_wire_verified_marker_not_trusted");
    const int oldVerifies = verifies;
    brpc::Controller unknown;
    unknown.set_request_checksum_type(kUnknownChecksum);
    checksum_probe::Reply reply;
    channel.CallMethod(checksum_probe::PlainProbe::descriptor()->method(0), &unknown, &request, &reply, nullptr);
    Require(!unknown.Failed() && verifies == oldVerifies, "unknown checksum is ignored by native transport");
    Call(channel, request, true, Fault::None, false, false, "test-ak-a", kUnknownChecksum);
    Call(channel, request, true, Fault::None, false, false, "test-ak-a", brpc::CHECKSUM_TYPE_NONE);
    Pass("unknown_checksum_native_fail_open_auth_entry_fails_closed");
    checksum_probe::RequestV1 large;
    large.set_payload(std::string(256 * 1024, 'x'));
    Call(channel, large, true);
    Pass("segmented_256KiB_body_single_serialization");
    Call(channel, large, true, Fault::None, false, true);
    Pass("gzip_signs_compressed_wire_bytes_single_serialization");
    std::vector<std::thread> threads;
    const int before = business;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i] {
            for (int j = 0; j < 20; ++j) {
                Call(channel, request, true, Fault::None, true, false, i % 2 ? "test-ak-a" : "test-ak-b");
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    Require(business == before + 80, "concurrent credential isolation");
    Pass("80_concurrent_async_calls_two_credentials_one_shared_channel");
    const int lastComputes = computes, lastVerifies = verifies, lastBorrows = borrows;
    Call(channel, request, false);
    Require(computes == lastComputes && verifies == lastVerifies && borrows == lastBorrows,
            "off path remains untouched after signed traffic");
    server.Stop(0);
    server.Join();
    bthread_key_delete(credentialKey);
    std::cout << "RESULT PASS compute=" << computes << " verify=" << verifies << " local_access=" << borrows
              << " reset=" << resets << " business=" << business << std::endl;
}
