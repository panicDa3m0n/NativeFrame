#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPREPARE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPREPARE_HPP

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

class DrawPrepare {
public:
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    struct Prepared {
        std::function<void()> finish;
        std::vector<Range> reads;
        std::vector<Range> writes;
        bool retry = false;
    };
    using Job = std::function<Prepared(bool synchronous)>;

    static DrawPrepare& Queue0();
    static unsigned Threads();
    static bool Transparent(std::span<const std::uint32_t> packet, std::uint32_t header, std::uint32_t opcode);
    static bool Sequencing();

    void Submit(Job job, std::vector<Range> earlyWrites);
    void SubmitReady(std::function<void()> finish);
    bool IndirectRegistersConflict(std::span<const std::uint32_t> packet);
    void Flush(std::uint32_t reason = 0x200);
    bool Idle();
    void Stop();
    void Report();

private:
    struct Slot {
        Job job;
        Prepared prepared;
        std::exception_ptr failure;
        bool ready = false;
        std::vector<std::shared_ptr<Slot>> predecessors;
        std::vector<Range> earlyWrites;
        std::uint64_t readSerial = 0;
    };
    DrawPrepare() = default;
    void sequence(bool all);
    void run();

    std::mutex orderMutex;
    std::deque<std::shared_ptr<Slot>> order;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable done;
    std::deque<std::shared_ptr<Slot>> pending;
    std::vector<std::thread> threads;
    bool closing = false;
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> flushes{0};
    std::atomic<std::uint64_t> retries{0};
    std::atomic<std::uint64_t> overlapRetries{0};
    std::atomic<std::uint64_t> waits{0};
    std::array<std::atomic<std::uint64_t>, 0x202> reasons{};
    std::deque<std::pair<Range, std::uint64_t>> indirectReads;
    std::uint64_t indirectSerial = 0;
    std::atomic<std::uint64_t> indirectPassed{0};
};

}

#endif
