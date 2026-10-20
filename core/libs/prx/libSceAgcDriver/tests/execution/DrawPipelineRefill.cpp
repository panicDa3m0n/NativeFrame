#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;

constexpr std::uint64_t Commits = 64;

void check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("draw pipeline refill: " + what);
}

void hold(std::chrono::microseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until) {
    }
}

}

int main() {
    try {
        auto& pipeline = DrawPipeline::Queue0();
        const auto depth = DrawPipeline::Depth();
        if (depth < 4) {
            std::puts("draw pipeline refill tests skipped (pipeline depth below 4)");
            return 0;
        }
        const bool hysteresis = std::getenv("APS5_NO_COMMIT_HYSTERESIS") == nullptr;
        std::atomic<std::uint64_t> next{0};
        std::atomic<bool> ordered{true};
        const auto wakesBefore = pipeline.ProducerWakes();
        for (std::uint64_t index = 0; index < Commits; ++index) {
            pipeline.Enqueue([&next, &ordered, index] {
                if (next.fetch_add(1, std::memory_order_acq_rel) != index) ordered.store(false, std::memory_order_release);
                hold(std::chrono::microseconds(300));
            }, {});
        }
        pipeline.Drain(DrawPipeline::DrainReason::Packet);
        const auto wakes = pipeline.ProducerWakes() - wakesBefore;
        check(ordered.load(std::memory_order_acquire) && next.load(std::memory_order_acquire) == Commits, "commits ran out of order or were lost");
        if (hysteresis) check(wakes <= Commits / 3, "a producer waiting for room was woken " + std::to_string(wakes) + " times for " + std::to_string(Commits) + " commits");
        else check(wakes >= (Commits - depth) * 3 / 4, "APS5_NO_COMMIT_HYSTERESIS=1 woke the producer only " + std::to_string(wakes) + " times for " + std::to_string(Commits) + " commits");
        std::printf("draw pipeline refill tests passed (%llu producer wakes for %llu commits)\n", static_cast<unsigned long long>(wakes), static_cast<unsigned long long>(Commits));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
