#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPrepare.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

namespace AgcDriver::DriverDetail {

namespace {

thread_local bool sequencingThread = false;

bool overlaps(const std::vector<DrawPrepare::Range>& reads, const std::vector<DrawPrepare::Range>& writes) {
    for (const auto& [begin, end] : reads) {
        for (const auto& [first, last] : writes) {
            if (begin < last && first < end) return true;
        }
    }
    return false;
}

}

DrawPrepare& DrawPrepare::Queue0() {
    static DrawPrepare* prepare = new DrawPrepare();
    return *prepare;
}

unsigned DrawPrepare::Threads() {
    static const unsigned threads = [] {
        const char* text = std::getenv("APS5_PARALLEL_DRAW_PREPARE");
        if (text == nullptr) return 0u;
        return static_cast<unsigned>(std::min<unsigned long>(std::strtoul(text, nullptr, 10), 8ul));
    }();
    return threads;
}

bool DrawPrepare::Transparent(std::span<const std::uint32_t> packet, std::uint32_t header, std::uint32_t opcode) {
    if (Pm4::Predicated(header)) return false;
    if (Pm4::DrawOpcode(opcode)) return true;
    switch (opcode) {
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x26: case 0x28: case 0x2a: case 0x2f: case 0x69: case 0x76: case 0x79: case 0x7a: return true;
        case 0x46: return packet.size() >= 2 && !Pm4::SampleDump(packet) && (packet[1] & 0x3fu) != 0x39u;
        default: return false;
    }
}

bool DrawPrepare::Sequencing() {
    return sequencingThread;
}

void DrawPrepare::Submit(Job job, std::vector<Range> earlyWrites) {
    std::lock_guard orderLock(orderMutex);
    if (threads.empty()) {
        for (unsigned index = 0; index < Threads(); ++index) threads.emplace_back([this] { run(); });
    }
    auto slot = std::make_shared<Slot>();
    slot->job = std::move(job);
    slot->earlyWrites = std::move(earlyWrites);
    slot->readSerial = indirectSerial;
    slot->predecessors.assign(order.begin(), order.end());
    order.push_back(slot);
    {
        std::lock_guard lock(mutex);
        pending.push_back(std::move(slot));
    }
    wake.notify_one();
    submitted.fetch_add(1, std::memory_order_relaxed);
    sequence(false);
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (profile) Report();
}

void DrawPrepare::SubmitReady(std::function<void()> finish) {
    std::unique_lock orderLock(orderMutex);
    if (order.empty()) {
        orderLock.unlock();
        finish();
        return;
    }
    auto slot = std::make_shared<Slot>();
    slot->prepared.finish = std::move(finish);
    slot->readSerial = indirectSerial;
    slot->ready = true;
    order.push_back(std::move(slot));
}

void DrawPrepare::Flush(std::uint32_t reason) {
    if (Threads() == 0 || sequencingThread) return;
    std::lock_guard orderLock(orderMutex);
    if (order.empty()) return;
    flushes.fetch_add(1, std::memory_order_relaxed);
    reasons[std::min<std::uint32_t>(reason, 0x201)].fetch_add(1, std::memory_order_relaxed);
    sequence(true);
}

bool DrawPrepare::IndirectRegistersConflict(std::span<const std::uint32_t> packet) {
    if (packet.size() < 5) return true;
    const std::uint64_t address = packet[1] | (static_cast<std::uint64_t>(packet[2]) << 32u);
    const std::vector<Range> read{{address, address + static_cast<std::uint64_t>(packet[4]) * 8u}};
    std::lock_guard orderLock(orderMutex);
    if (order.empty()) return false;
    for (const auto& slot : order) {
        std::lock_guard lock(mutex);
        if (overlaps(read, slot->ready ? slot->prepared.writes : slot->earlyWrites)) return true;
    }
    indirectReads.emplace_back(read.front(), indirectSerial++);
    indirectPassed.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool DrawPrepare::Idle() {
    std::lock_guard orderLock(orderMutex);
    return order.empty();
}

void DrawPrepare::sequence(bool all) {
    const std::size_t window = std::max<std::size_t>(2, static_cast<std::size_t>(Threads()) * 3);
    struct Scope {
        Scope() { sequencingThread = true; }
        ~Scope() { sequencingThread = false; }
    } scope;
    while (!order.empty()) {
        auto front = order.front();
        {
            std::unique_lock lock(mutex);
            if (!front->ready) {
                if (!all && order.size() <= window) return;
                waits.fetch_add(1, std::memory_order_relaxed);
                done.wait(lock, [&] { return front->ready; });
            }
        }
        order.pop_front();
        if (front->failure != nullptr) {
            std::exception_ptr failure = front->failure;
            front->predecessors.clear();
            std::rethrow_exception(failure);
        }
        bool redo = front->prepared.retry;
        if (!redo) {
            for (const auto& predecessor : front->predecessors) {
                if (overlaps(front->prepared.reads, predecessor->prepared.writes)) {
                    redo = true;
                    overlapRetries.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
        front->predecessors.clear();
        if (redo) {
            retries.fetch_add(1, std::memory_order_relaxed);
            front->prepared = front->job(true);
        }
        for (const auto& [range, serial] : indirectReads) {
            if (serial < front->readSerial) continue;
            const std::vector<Range> read{range};
            if (overlaps(read, front->prepared.writes)) throw std::runtime_error("parallel draw preparation: a draw writes a register table that a later SET_*_REG_INDIRECT read before the draw was recorded");
        }
        if (order.empty()) indirectReads.clear();
        else while (!indirectReads.empty() && indirectReads.front().second < order.front()->readSerial) indirectReads.pop_front();
        front->job = nullptr;
        if (front->prepared.finish) front->prepared.finish();
        front->prepared.finish = nullptr;
        front->prepared.reads.clear();
    }
}

void DrawPrepare::run() {
    for (;;) {
        std::shared_ptr<Slot> slot;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return closing || !pending.empty(); });
            if (pending.empty()) return;
            slot = std::move(pending.front());
            pending.pop_front();
        }
        Prepared prepared;
        std::exception_ptr failure;
        try {
            prepared = slot->job(false);
        } catch (...) {
            failure = std::current_exception();
        }
        {
            std::lock_guard lock(mutex);
            slot->prepared = std::move(prepared);
            slot->failure = failure;
            slot->ready = true;
        }
        done.notify_all();
    }
}

void DrawPrepare::Report() {
    static std::atomic<std::int64_t> last{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto previous = last.load(std::memory_order_relaxed);
    if (previous == 0) {
        last.compare_exchange_strong(previous, now);
        return;
    }
    if (now - previous < 10000 || !last.compare_exchange_strong(previous, now)) return;
    std::string top;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> ranked;
    for (std::uint32_t reason = 0; reason < reasons.size(); ++reason) {
        if (const auto count = reasons[reason].exchange(0); count != 0) ranked.emplace_back(count, reason);
    }
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    for (std::size_t index = 0; index < ranked.size() && index < 8; ++index) {
        char item[48];
        std::snprintf(item, sizeof(item), " 0x%x x%llu", ranked[index].second, static_cast<unsigned long long>(ranked[index].first));
        top += item;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[draw-prepare] 10 s: %llu draws prepared on %u threads, %llu flushes (by opcode:%s), %llu waits for the oldest, %llu redone on the worker (%llu over a predecessor's writes)\n", static_cast<unsigned long long>(submitted.exchange(0)), Threads(), static_cast<unsigned long long>(flushes.exchange(0)), top.c_str(), static_cast<unsigned long long>(waits.exchange(0)), static_cast<unsigned long long>(retries.exchange(0)), static_cast<unsigned long long>(overlapRetries.exchange(0)));
}

void DrawPrepare::Stop() {
    {
        std::lock_guard lock(mutex);
        closing = true;
    }
    wake.notify_all();
    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }
    threads.clear();
}

}
