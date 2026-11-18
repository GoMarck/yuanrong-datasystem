#include <datasystem/kv_client.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    constexpr int kExpectedArgCount = 3;
    constexpr int kUsageError = 2;
    if (argc != kExpectedArgCount) {
        std::cerr << "usage: ds_kv_example <worker-host> <worker-port>\n";
        return kUsageError;
    }
    constexpr int kConnectTimeoutMs = 9000;
    constexpr int kRequestTimeoutMs = 5000;
    datasystem::ConnectOptions options;
    options.host = argv[1];
    options.port = std::stoi(argv[2]);
    options.connectTimeoutMs = kConnectTimeoutMs;
    options.requestTimeoutMs = kRequestTimeoutMs;
    datasystem::KVClient client(options);
    auto status = client.Init();
    if (status.IsError()) {
        std::cerr << status.ToString() << '\n';
        return 1;
    }
    bool success = true;
    {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const std::vector<std::string> keys{"ds-source-example-a-" + suffix, "ds-source-example-b-" + suffix};
        const std::vector<std::string> values{"value-a", "value-b"};
        const std::vector<datasystem::StringView> views{values[0], values[1]};
        std::vector<std::string> failed;
        status = client.MSet(keys, views, failed);
        success = status.IsOk() && failed.empty();
        if (!success) {
            std::cerr << "MSet: " << status.ToString() << ", failed=" << failed.size() << '\n';
        } else {
            std::vector<datasystem::Optional<datasystem::ReadOnlyBuffer>> results;
            status = client.Get(keys, results);
            success = status.IsOk() && results.size() == keys.size();
            for (size_t i = 0; success && i < keys.size(); ++i) {
                success = static_cast<bool>(results[i]);
                if (success) {
                    auto &buffer = results[i].value();
                    const auto *data = static_cast<const char *>(buffer.ImmutableData());
                    const std::string actual = buffer.GetSize() == 0 ? std::string() : std::string(data, buffer.GetSize());
                    success = actual == values[i];
                }
            }
            if (!success) {
                std::cerr << "Get: " << status.ToString() << '\n';
            }
        }
        std::vector<std::string> notDeleted;
        const auto cleanup = client.Del(keys, notDeleted);
        success = cleanup.IsOk() && notDeleted.empty() && success;
    }
    const auto shutdown = client.ShutDown();
    if (shutdown.IsError()) {
        std::cerr << "ShutDown: " << shutdown.ToString() << '\n';
    }
    return success && shutdown.IsOk() ? 0 : 1;
}
