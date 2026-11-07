// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef DATASYSTEM_COMMON_SIGNAL_PERF_SIGNAL_H
#define DATASYSTEM_COMMON_SIGNAL_PERF_SIGNAL_H

#include <signal.h>

namespace datasystem {
// Owns process-wide SIGUSR handlers; handlers only enqueue coalesced requests for the main loop.
class ScopedPerfSignals final {
public:
    static constexpr unsigned int DUMP = 1;
    static constexpr unsigned int RESET = 1U << 1;

    ScopedPerfSignals() = default;
    ~ScopedPerfSignals();
    ScopedPerfSignals(const ScopedPerfSignals &) = delete;
    ScopedPerfSignals &operator=(const ScopedPerfSignals &) = delete;
    ScopedPerfSignals(ScopedPerfSignals &&) = delete;
    ScopedPerfSignals &operator=(ScopedPerfSignals &&) = delete;

    bool Install();
    unsigned int Consume();

private:
    bool installed_ = false;
    struct sigaction previousDump_ {};
    struct sigaction previousReset_ {};
};
}  // namespace datasystem

#endif
