#include "kv_smoke.h"
#include <iostream>
#include <string>

int main(int argc, char **argv)
{
    constexpr int kExpectedArgs = 3;
    constexpr int kUsageError = 2;
    if (argc != kExpectedArgs) {
        std::cerr << "usage: ds_private_sdk_smoke <worker-host> <worker-port>\n";
        return kUsageError;
    }
    return RunPrivateSdkKvSmoke(argv[1], std::stoi(argv[2]));
}
