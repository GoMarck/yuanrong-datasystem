"""Exercise the real perf signal mailbox without DataSystem/NPU dependencies."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
HARNESS = r"""
#include "datasystem/common/signal/perf_signal.h"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <thread>
#include <type_traits>
#include <unistd.h>

using datasystem::ScopedPerfSignals;
static_assert(!std::is_copy_constructible_v<ScopedPerfSignals>);
static_assert(!std::is_move_constructible_v<ScopedPerfSignals>);

void Check(bool value) { if (!value) std::abort(); }
volatile sig_atomic_t restored = 0;
void Previous(int) { restored = 1; }

int main()
{
    struct sigaction previous {};
    previous.sa_handler = Previous;
    sigemptyset(&previous.sa_mask);
    Check(sigaction(SIGUSR1, &previous, nullptr) == 0);
    Check(sigaction(SIGUSR2, &previous, nullptr) == 0);
    struct sigaction termBefore {};
    Check(sigaction(SIGTERM, nullptr, &termBefore) == 0);
    {
        ScopedPerfSignals signals;
        Check(signals.Consume() == 0);
        Check(signals.Install());
        Check(signals.Install());
        ScopedPerfSignals duplicate;
        Check(!duplicate.Install() && errno == EBUSY);
        Check(duplicate.Consume() == 0);
        Check(kill(getpid(), SIGUSR1) == 0);
        Check(signals.Consume() == ScopedPerfSignals::DUMP);
        Check(signals.Consume() == 0);
        Check(kill(getpid(), SIGUSR2) == 0);
        Check(signals.Consume() == ScopedPerfSignals::RESET);
        Check(raise(SIGUSR1) == 0);
        Check(raise(SIGUSR1) == 0);
        Check(raise(SIGUSR2) == 0);
        Check(signals.Consume() == (ScopedPerfSignals::DUMP | ScopedPerfSignals::RESET));
        Check(restored == 0);

        std::atomic<bool> done{false};
        std::thread sender([&] {
            constexpr int iterations = 1000;
            for (int i = 0; i < iterations; ++i) {
                Check(raise(SIGUSR1) == 0);
                Check(raise(SIGUSR2) == 0);
            }
            done.store(true);
        });
        unsigned observed = 0;
        while (!done.load()) {
            observed |= signals.Consume();
            std::this_thread::yield();
        }
        sender.join();
        observed |= signals.Consume();
        Check(observed == (ScopedPerfSignals::DUMP | ScopedPerfSignals::RESET));
        Check(signals.Consume() == 0);
        struct sigaction termAfter {};
        Check(sigaction(SIGTERM, nullptr, &termAfter) == 0);
        Check(termBefore.sa_handler == termAfter.sa_handler);
    }
    Check(raise(SIGUSR1) == 0);
    Check(restored == 1);
    restored = 0;
    Check(raise(SIGUSR2) == 0);
    Check(restored == 1);
    ScopedPerfSignals restarted;
    Check(restarted.Install());
    Check(restarted.Consume() == 0);
    Check(raise(SIGUSR2) == 0);
    Check(restarted.Consume() == ScopedPerfSignals::RESET);
}
"""


class WorkerPerfSignalsTest(unittest.TestCase):
    def test_signal_delivery_coalescing_concurrency_and_restoration(self):
        compiler = shutil.which("g++")
        if compiler is None:
            self.skipTest("g++ is required for the POSIX signal smoke test")
        with tempfile.TemporaryDirectory(prefix="ds-perf-signal-") as directory:
            source = pathlib.Path(directory) / "main.cpp"
            source.write_text(HARNESS)
            for perf_enabled in (False, True):
                with self.subTest(perf_enabled=perf_enabled):
                    binary = pathlib.Path(directory) / ("perf-on" if perf_enabled else "perf-off")
                    subprocess.run(
                        [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                         *(["-DENABLE_PERF"] if perf_enabled else []),
                         "-I", str(ROOT / "src"), str(source),
                         str(ROOT / "src/datasystem/common/signal/perf_signal.cpp"),
                         "-o", str(binary)],
                        check=True, timeout=30,
                    )
                    subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
