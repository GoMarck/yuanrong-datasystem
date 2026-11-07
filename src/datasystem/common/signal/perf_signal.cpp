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

#include "datasystem/common/signal/perf_signal.h"

#include <atomic>
#include <cerrno>

namespace datasystem {
namespace {
static_assert(std::atomic<unsigned int>::is_always_lock_free, "Signal requests require lock-free atomics");
std::atomic<unsigned int> pending{ 0 };
std::atomic<bool> owned{ false };

void HandlePerfSignal(int signal)
{
    const int savedErrno = errno;
    const auto request = signal == SIGUSR1 ? ScopedPerfSignals::DUMP : ScopedPerfSignals::RESET;
    pending.fetch_or(request, std::memory_order_relaxed);
    errno = savedErrno;
}
}  // namespace

bool ScopedPerfSignals::Install()
{
    if (installed_) {
        return true;
    }
    bool expected = false;
    if (!owned.compare_exchange_strong(expected, true)) {
        errno = EBUSY;
        return false;
    }
    pending.store(0, std::memory_order_relaxed);
    struct sigaction action {};
    action.sa_handler = HandlePerfSignal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, &previousDump_) != 0) {
        owned.store(false);
        return false;
    }
    if (sigaction(SIGUSR2, &action, &previousReset_) != 0) {
        const int savedErrno = errno;
        (void)sigaction(SIGUSR1, &previousDump_, nullptr);
        owned.store(false);
        errno = savedErrno;
        return false;
    }
    installed_ = true;
    return true;
}

ScopedPerfSignals::~ScopedPerfSignals()
{
    if (installed_) {
        (void)sigaction(SIGUSR2, &previousReset_, nullptr);
        (void)sigaction(SIGUSR1, &previousDump_, nullptr);
        owned.store(false);
    }
}

unsigned int ScopedPerfSignals::Consume()
{
    return installed_ ? pending.exchange(0, std::memory_order_relaxed) : 0;
}
}  // namespace datasystem
