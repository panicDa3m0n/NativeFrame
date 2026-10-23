#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

class DrawPipeline {
public:
    using Commit = std::function<void()>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    struct Watch {
        std::uint64_t retired = 0;
        std::size_t items = 0;
        std::vector<Range> writes;
    };
    enum class DrainReason : std::uint8_t { Packet, Flush, Labels, Capture, Indirect, Submission, Count };
    enum class Event : std::uint8_t { IndirectCommit, IndirectArgsDrain, SubmitInterrupt, SubmitDeadline, SubmitCap, Count };

    static DrawPipeline& Queue0();
    static std::size_t Depth();
    static std::size_t RefillMark();
    std::uint64_t ProducerWakes();
    static std::size_t QueuedItems();
    static bool PipelineIndirect();
    static bool DrainBeforeRead(std::uint64_t address, std::size_t bytes);
    static bool& Active();
    static std::atomic<std::uint64_t>& EpochToken();
    static void FollowEpoch(std::uint64_t token);
    static unsigned Committers();

    void Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress = 0, std::vector<std::byte> labelBytes = {}, std::vector<Range> reads = {}, bool draw = false);
    void Drain(DrainReason reason, std::uint32_t opcode = 0x100);
    void Stop();
    void ReleaseRetired();
    static std::uint64_t DrainRequestsByOpcode(std::uint32_t opcode);
    bool Busy() const;
    std::size_t Outstanding() const { return outstanding.load(std::memory_order_acquire); }
    bool Overlaps(std::uint64_t address, std::size_t bytes);
    bool DrainIfOverlaps(std::span<const Range> ranges, DrainReason reason, std::uint32_t opcode = 0x100) {
        return DrainIfOverlaps(ranges, [](const Range& range) { return range; }, reason, opcode);
    }
    template<typename TRanges, typename TBounds>
    bool DrainIfOverlaps(const TRanges& ranges, TBounds&& bounds, DrainReason reason, std::uint32_t opcode = 0x100) {
        if (!Busy()) return false;
        if (!pendingWrites(ranges, bounds)) return false;
        Drain(reason, opcode);
        return true;
    }
    void Note(Event event) { events[static_cast<std::size_t>(event)].fetch_add(1, std::memory_order_relaxed); }
    std::uint64_t Events(Event event) const { return events[static_cast<std::size_t>(event)].load(std::memory_order_relaxed); }
    bool OverlapsAny(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
    Watch WatchWrites();
    bool WrittenSince(const Watch& watch, const std::vector<Range>& reads);
    std::uint64_t Retired() const { return retiredItems.load(std::memory_order_acquire); }
    void NoteDecodeDrain() { decodeDrains.fetch_add(1, std::memory_order_relaxed); }
    std::uint64_t DecodeDrains() const { return decodeDrains.load(std::memory_order_relaxed); }
    std::optional<std::uint64_t> PendingLabel(std::uint64_t address, std::size_t bytes);
    static bool HeldReads(std::uint64_t address, std::size_t bytes);
    void ReleaseRunningReads();

    static bool MetadataPassesPipelined();
    static void CountPipelinedMetadataPass();
    static std::uint64_t PipelinedMetadataPasses();
    static std::uint64_t CommittedDraws();
private:
    struct Item {
        Commit commit;
        std::vector<Range> writes;
        std::uint64_t labelAddress = 0;
        std::vector<std::byte> labelBytes;
        std::vector<Range> reads;
        bool draw = false;
    };
    DrawPipeline() = default;
    template<typename TRanges, typename TBounds>
    bool pendingWrites(const TRanges& ranges, TBounds& bounds) {
        std::uint64_t lowest = ~std::uint64_t{0}, highest = 0;
        for (const auto& range : ranges) {
            const auto [first, end] = bounds(range);
            if (first >= end) continue;
            lowest = std::min(lowest, first);
            highest = std::max(highest, end);
        }
        if (lowest >= highest) return false;
        std::lock_guard lock(mutex);
        for (const auto& item : items) {
            for (const auto& [begin, limit] : item.writes) {
                if (begin >= highest || limit <= lowest) continue;
                for (const auto& range : ranges) {
                    const auto [first, end] = bounds(range);
                    if (first < limit && begin < end) return true;
                }
            }
        }
        return false;
    }
    void run();
    void retire(Item&& item);
    void reap();
    void rethrowFailure();
    void report(std::chrono::steady_clock::time_point now);
    void countWrites(const std::vector<Range>& writes, bool added);

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Item> items;
    std::vector<std::pair<Range, std::uint32_t>> queuedWrites;
    std::exception_ptr failure;
    std::atomic<std::size_t> outstanding{0};
    std::atomic<std::uint64_t> retiredItems{0};
    std::atomic<std::uint64_t> decodeDrains{0};
    std::uint64_t reportedDecodeDrains = 0;
    std::uint32_t idleWaiters = 0;
    std::uint64_t producerWakes = 0;
    bool committerWaiting = false;
    bool stopped = false;
    bool closing = false;
    std::uint64_t wakes = 0;
    std::uint64_t lockWaitNs = 0;
    std::thread thread;
    std::mutex retireMutex;
    std::condition_variable reapWake;
    std::deque<Item> retired;
    std::thread reaper;
    bool reaperWaiting = false;
    std::uint64_t commits = 0;
    std::uint64_t commitNs = 0;
    std::uint64_t commitErrors = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t fullWaitNs = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drains{};
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drainWaitNs{};
    std::array<std::uint64_t, 257> drainOpcodes{};
    std::array<std::atomic<std::uint64_t>, 257> drainRequests{};
    std::uint64_t depthSum = 0;
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Event::Count)> events{};
    std::array<std::uint64_t, static_cast<std::size_t>(Event::Count)> reportedEvents{};
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

class DrawFinisher {
public:
    using Job = std::function<void()>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    struct Pending {
        std::uint64_t key = 0;
        std::uint64_t shape = 0;
        bool unknown = true;
        std::vector<Range> writes;
        std::uint64_t labelAddress = 0;
        std::vector<std::byte> labelBytes;
    };
    enum class LabelLookup : std::uint8_t { NotHeld, Value, Unknown };
    static bool Overlap();
    bool PendingOverlaps(std::uint64_t begin, std::uint64_t end);
    LabelLookup PendingLabel(std::uint64_t address, std::size_t bytes, std::uint64_t& value);
    bool PendingOverlapsAny(std::span<const Range> ranges);
    bool PendingKey(std::uint64_t key, std::uint64_t shape);
    enum class DrainSite : std::uint32_t { Lookup, Reads, Packet, Kept, Count };
    static void NoteDrain(DrainSite site, bool drained);
    static std::string DrainText();
    static void NoteDrainedOpcode(std::uint32_t opcode, bool besideCommits);
    static DrawFinisher& Get();
    static bool Enabled();
    static bool OnFinisherThread();
    static bool Busy() { return outstanding.load(std::memory_order_acquire) != 0; }
    void Enqueue(Job job);
    void Enqueue(Job job, Pending held);
    void Drain();
    void Settle();
    void ReleaseRan();

private:
    DrawFinisher() = default;
    void run();
    inline static std::atomic<std::size_t> outstanding{0};
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Job> jobs;
    std::deque<Pending> pending;
    std::vector<Job> ran;
    std::exception_ptr failure;
    bool running = false;
    bool sleeping = false;
    std::thread thread;
};

}

#endif
