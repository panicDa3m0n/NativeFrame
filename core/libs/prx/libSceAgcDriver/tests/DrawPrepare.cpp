#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPrepare.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using AgcDriver::DriverDetail::DrawPrepare;
using Range = DrawPrepare::Range;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

DrawPrepare& Prepare() {
    return DrawPrepare::Queue0();
}

void WaitFor(const std::atomic<bool>& gate) {
    while (!gate.load(std::memory_order_acquire)) std::this_thread::sleep_for(50us);
}

DrawPrepare::Prepared Finishing(std::vector<int>& finished, int index, std::vector<Range> reads = {}, std::vector<Range> writes = {}) {
    DrawPrepare::Prepared prepared;
    prepared.finish = [&finished, index] { finished.push_back(index); };
    prepared.reads = std::move(reads);
    prepared.writes = std::move(writes);
    return prepared;
}

std::vector<std::uint32_t> IndirectRegisters(std::uint64_t address, std::uint32_t count) {
    return {0xc0039f00u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 0, count};
}

void CheckTransparentPackets() {
    const std::vector<std::uint32_t> contextRegister{0xc0016900u, 0x10, 0};
    Require(DrawPrepare::Transparent(contextRegister, contextRegister[0], 0x69), "SET_CONTEXT_REG is not transparent");
    Require(!DrawPrepare::Transparent(contextRegister, contextRegister[0] | 1u, 0x69), "a predicated SET_CONTEXT_REG is transparent");
    const std::vector<std::uint32_t> draw{0xc0012d00u, 3, 0};
    Require(DrawPrepare::Transparent(draw, draw[0], 0x2d), "DRAW_INDEX_AUTO is not transparent");
    const std::vector<std::uint32_t> cacheFlush{0xc0004600u, 0x16};
    Require(DrawPrepare::Transparent(cacheFlush, cacheFlush[0], 0x46), "a cache flush EVENT_WRITE is not transparent");
    const std::vector<std::uint32_t> sampleDump{0xc0024600u, 0x39, 0x1000, 0};
    Require(!DrawPrepare::Transparent(sampleDump, sampleDump[0], 0x46), "a sample dump EVENT_WRITE is transparent");
    const std::vector<std::uint32_t> dispatch{0xc0031500u, 1, 1, 1, 0};
    Require(!DrawPrepare::Transparent(dispatch, dispatch[0], 0x15), "DISPATCH_DIRECT is transparent");
    const std::vector<std::uint32_t> dma(7, 0);
    Require(!DrawPrepare::Transparent(dma, 0xc0055000u, 0x50), "DMA_DATA is transparent");
    const std::vector<std::uint32_t> releaseMem(8, 0);
    Require(!DrawPrepare::Transparent(releaseMem, 0xc0064900u, 0x49), "RELEASE_MEM is transparent");
}

void CheckSwitchedOff() {
    Require(DrawPrepare::Threads() == 0, "draw preparation runs on threads without APS5_PARALLEL_DRAW_PREPARE");
    Prepare().Flush();
    Require(Prepare().Idle(), "a flush with the switch off leaves work behind");
    bool ran = false;
    Prepare().SubmitReady([&ran] { ran = true; });
    Require(ran, "SubmitReady with the switch off does not run at once");
    Require(!Prepare().IndirectRegistersConflict(IndirectRegisters(0x3000, 4)), "an indirect register table conflicts with no open draw");
}

void CheckOrder() {
    const auto caller = std::this_thread::get_id();
    std::vector<int> finished;
    std::vector<std::thread::id> finishers;
    std::atomic<int> helped{0};
    constexpr int draws = 96;
    for (int index = 0; index < draws; ++index) {
        Prepare().Submit([&, index, caller](bool synchronous) {
            if (!synchronous && std::this_thread::get_id() != caller) helped.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds((index * 37) % 400));
            DrawPrepare::Prepared prepared;
            prepared.finish = [&finished, &finishers, index] {
                finished.push_back(index);
                finishers.push_back(std::this_thread::get_id());
            };
            return prepared;
        }, {});
    }
    Prepare().Flush();
    Require(Prepare().Idle(), "draws are left open after a flush");
    Require(finished.size() == draws, "not every draw was committed");
    for (int index = 0; index < draws; ++index) {
        Require(finished[index] == index, "draw " + std::to_string(finished[index]) + " was committed in place of draw " + std::to_string(index));
        Require(finishers[index] == caller, "a draw was committed off the submitting thread");
    }
    Require(helped.load() == draws, "a draw was not prepared on a helper thread");
}

void CheckReadOverPredecessorWrites() {
    std::vector<int> finished;
    std::atomic<bool> gate{false};
    std::atomic<int> calls{0};
    bool redoneAfterWriter = false;
    Prepare().Submit([&](bool) {
        WaitFor(gate);
        return Finishing(finished, 0, {}, {{0x1000, 0x2000}});
    }, {});
    Prepare().Submit([&](bool synchronous) {
        calls.fetch_add(1, std::memory_order_relaxed);
        if (synchronous) redoneAfterWriter = finished.size() == 1 && finished[0] == 0;
        return Finishing(finished, 1, {{0x1800, 0x1900}});
    }, {});
    Prepare().Submit([&](bool) {
        return Finishing(finished, 2, {{0x2000, 0x2100}});
    }, {});
    gate.store(true, std::memory_order_release);
    Prepare().Flush();
    Require(finished == std::vector<int>{0, 1, 2}, "draws reading earlier writes were committed out of order");
    Require(calls.load() == 2, "a draw reading a predecessor's writes was not prepared again");
    Require(redoneAfterWriter, "a draw reading a predecessor's writes was prepared again before the writer was committed");
}

void CheckRetryRequest() {
    std::vector<int> finished;
    std::atomic<bool> gate{false};
    std::vector<bool> modes;
    Prepare().Submit([&](bool synchronous) {
        WaitFor(gate);
        DrawPrepare::Prepared prepared = Finishing(finished, synchronous ? 1 : -1);
        prepared.retry = !synchronous;
        modes.push_back(synchronous);
        return prepared;
    }, {});
    gate.store(true, std::memory_order_release);
    Prepare().Flush();
    Require(modes == std::vector<bool>{false, true}, "a draw asking to be redone was not prepared again on the worker");
    Require(finished == std::vector<int>{1}, "the commit of a redone draw is not the one from the worker");
}

void CheckReadyItemsKeepOrder() {
    std::vector<int> finished;
    bool immediate = false;
    Prepare().SubmitReady([&immediate] { immediate = true; });
    Require(immediate, "a ready item with no open draw did not run at once");
    std::atomic<bool> gate{false};
    Prepare().Submit([&](bool) {
        WaitFor(gate);
        return Finishing(finished, 0);
    }, {});
    Prepare().SubmitReady([&finished] { finished.push_back(1); });
    Prepare().Submit([&](bool) { return Finishing(finished, 2); }, {});
    Require(finished.empty(), "a ready item ran before the open draw in front of it");
    gate.store(true, std::memory_order_release);
    Prepare().Flush();
    Require(finished == std::vector<int>{0, 1, 2}, "a ready item left its place among the draws");
}

void CheckFlushWhileSequencing() {
    std::vector<int> finished;
    bool sequencing = false;
    Prepare().Submit([&](bool) {
        DrawPrepare::Prepared prepared;
        prepared.finish = [&] {
            sequencing = DrawPrepare::Sequencing();
            Prepare().Flush();
            finished.push_back(0);
        };
        return prepared;
    }, {});
    Prepare().Submit([&](bool) { return Finishing(finished, 1); }, {});
    Prepare().Flush();
    Require(sequencing, "a commit does not run as part of the sequencer");
    Require(!DrawPrepare::Sequencing(), "the sequencer flag outlives the flush");
    Require(finished == std::vector<int>{0, 1}, "a flush from inside a commit changed the order");
}

void CheckIndirectRegisterTables() {
    const auto table = IndirectRegisters(0x3000, 4);
    Require(!Prepare().IndirectRegistersConflict(table), "an indirect register table conflicts with no open draw");
    std::vector<int> finished;
    std::atomic<bool> gate{false};
    Prepare().Submit([&](bool) {
        WaitFor(gate);
        return Finishing(finished, 0);
    }, {{0x3010, 0x3018}});
    Require(Prepare().IndirectRegistersConflict(table), "an indirect register table over an open draw's targets does not conflict");
    Require(!Prepare().IndirectRegistersConflict(IndirectRegisters(0x3020, 4)), "an indirect register table after an open draw's targets conflicts");
    gate.store(true, std::memory_order_release);
    Prepare().Flush();
    Require(finished == std::vector<int>{0}, "the open draw was not committed");

    gate.store(false, std::memory_order_release);
    Prepare().Submit([&](bool) {
        WaitFor(gate);
        return Finishing(finished, 1, {}, {{0x3000, 0x3008}});
    }, {});
    Require(!Prepare().IndirectRegistersConflict(table), "a table read before the open draw's writes are known conflicts");
    gate.store(true, std::memory_order_release);
    bool rejected = false;
    try {
        Prepare().Flush();
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find("register table") != std::string_view::npos;
    }
    Require(rejected, "a draw writing a register table read while it was open was committed");
    Require(finished == std::vector<int>{0}, "a draw writing a register table read while it was open was committed");
    Require(Prepare().Idle(), "the rejected draw is still open");

    Prepare().Submit([&](bool) { return Finishing(finished, 2, {}, {{0x3000, 0x3008}}); }, {});
    Prepare().Flush();
    Require(finished == std::vector<int>{0, 2}, "a later draw is held back by a table read before it was submitted");
}

void CheckFailureInOrder() {
    std::vector<int> finished;
    std::atomic<bool> gate{false};
    Prepare().Submit([&](bool) {
        WaitFor(gate);
        return Finishing(finished, 0);
    }, {});
    Prepare().Submit([](bool) -> DrawPrepare::Prepared { throw std::runtime_error("prepare failed"); }, {});
    Prepare().Submit([&](bool) { return Finishing(finished, 2); }, {});
    gate.store(true, std::memory_order_release);
    bool reported = false;
    try {
        Prepare().Flush();
    } catch (const std::runtime_error& error) {
        reported = std::string_view(error.what()) == "prepare failed";
    }
    Require(reported, "a failed preparation was not reported");
    Require(finished == std::vector<int>{0}, "a failed preparation was not reported in its place");
    Prepare().Flush();
    Require(finished == std::vector<int>{0, 2}, "the draw after a failed preparation was lost");
    Require(Prepare().Idle(), "draws are left open after a failure");
}

}

int main(int argc, char** argv) {
    try {
        CheckTransparentPackets();
        if (argc > 1 && std::string_view(argv[1]) == "off") {
            CheckSwitchedOff();
            std::cout << "draw prepare tests passed with the switch off\n";
            return 0;
        }
        Require(DrawPrepare::Threads() == 3, "APS5_PARALLEL_DRAW_PREPARE=3 does not give 3 threads");
        CheckOrder();
        CheckReadOverPredecessorWrites();
        CheckRetryRequest();
        CheckReadyItemsKeepOrder();
        CheckFlushWhileSequencing();
        CheckIndirectRegisterTables();
        CheckFailureInOrder();
        Prepare().Stop();
        std::cout << "draw prepare tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
