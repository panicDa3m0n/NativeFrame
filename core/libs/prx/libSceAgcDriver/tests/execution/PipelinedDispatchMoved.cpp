#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;

constexpr std::uint32_t Lanes = 32;
constexpr std::uint32_t BufferBytes = Lanes * 4u;
constexpr std::uint32_t DispatchOpcode = 0x15;

constexpr std::size_t BlockBytes = 65536;

std::uint32_t* NewBlock() {
#ifdef _WIN32
    auto* block = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    auto* block = static_cast<std::uint32_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
    if (block == nullptr) throw std::runtime_error("pipelined moved dispatch: cannot allocate a guest block");
    std::memset(block, 0, BlockBytes);
    GuestAllocations::Mutation().Add(block, BlockBytes, true, true, false);
    return block;
}

std::uint32_t* Input = nullptr;
std::uint32_t* Output = nullptr;
std::uint32_t* TargetA = nullptr;
std::uint32_t* TargetB = nullptr;
std::uint32_t* Table = nullptr;

alignas(256) constexpr std::array<std::uint32_t, 11> TableCode{0xf4080200u, 0xfa000000u, 0xbf8cc07fu, 0x34020082u, 0xe0301000u, 0x80020201u, 0xbf8c3f70u, 0x4a060481u, 0xe0701000u, 0x80010301u, 0xbf810000u};

alignas(256) constexpr std::array<std::uint32_t, 8> Code{0x34020082u, 0xe0301000u, 0x80000201u, 0xbf8c3f70u, 0x4a060481u, 0xe0701000u, 0x80010301u, 0xbf810000u};

void check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("pipelined moved dispatch: " + what);
}

template <typename Ready>
void WaitUntil(Ready ready, const std::string& what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("pipelined moved dispatch: timed out waiting for " + what);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::array<std::uint32_t, 7> FillPacket(std::uint32_t value, const void* destination, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    return {0xc0055000u, 2u << 29u, value, 0u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), bytes};
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::vector<std::uint32_t> MovedRound(const void* source, std::uint32_t value) {
    const auto fill = FillPacket(value, source, BufferBytes);
    std::vector<std::uint32_t> words(fill.begin(), fill.end());
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(Code.data());
    const auto input = BufferDescriptor(source, BufferBytes);
    const auto output = BufferDescriptor(Output, BufferBytes);
    const std::vector<std::uint32_t> commands{
        0xc0027600u, 0x20cu, static_cast<std::uint32_t>(codeAddress >> 8u), static_cast<std::uint32_t>(codeAddress >> 40u),
        0xc0037600u, 0x207u, Lanes, 1u, 1u,
        0xc0017600u, 0x213u, 8u << 1u,
        0xc0087600u, 0x240u, input[0], input[1], input[2], input[3], output[0], output[1], output[2], output[3],
        0xc0031500u, 1u, 1u, 1u, 0x8041u,
    };
    words.insert(words.end(), commands.begin(), commands.end());
    return words;
}

std::vector<std::uint32_t> TableRound(const void* target, std::uint32_t value) {
    const auto fill = FillPacket(value, target, BufferBytes);
    std::vector<std::uint32_t> words(fill.begin(), fill.end());
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(TableCode.data());
    const auto tableAddress = reinterpret_cast<std::uintptr_t>(Table);
    const auto output = BufferDescriptor(Output, BufferBytes);
    const std::vector<std::uint32_t> commands{
        0xc0027600u, 0x20cu, static_cast<std::uint32_t>(codeAddress >> 8u), static_cast<std::uint32_t>(codeAddress >> 40u),
        0xc0037600u, 0x207u, Lanes, 1u, 1u,
        0xc0017600u, 0x213u, 8u << 1u,
        0xc0087600u, 0x240u, static_cast<std::uint32_t>(tableAddress), static_cast<std::uint32_t>(tableAddress >> 32u), 0u, 0u, output[0], output[1], output[2], output[3],
        0xc0031500u, 1u, 1u, 1u, 0x8041u,
    };
    words.insert(words.end(), commands.begin(), commands.end());
    return words;
}

void Submit(std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(sceAgcDriverSubmitDcb(&packet) == 0, "submission failed");
}

template <typename Body>
std::uint64_t GatedRound(Body&& submit) {
    WaitUntil([] { return DrawPipeline::QueuedItems() == 0; }, "the pipeline to empty before a round");
    std::atomic<bool> released{false};
    struct Release {
        std::atomic<bool>& flag;
        ~Release() { flag.store(true, std::memory_order_release); }
    } release{released};
    const auto drainsBefore = DrawPipeline::DrainRequestsByOpcode(DispatchOpcode);
    DrawPipeline::Queue0().Enqueue([&released] {
        while (!released.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, {});
    submit();
    WaitUntil([&drainsBefore] { return DrawPipeline::QueuedItems() >= 3 || DrawPipeline::DrainRequestsByOpcode(DispatchOpcode) > drainsBefore; }, "the fill and the dispatch handled behind the gate");
    released.store(true, std::memory_order_release);
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output), BufferBytes);
    return DrawPipeline::DrainRequestsByOpcode(DispatchOpcode) - drainsBefore;
}


}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
    _putenv_s("APS5_PIPELINE_DMA", "1");
    _putenv_s("APS5_PIPELINE_DISPATCH", "1");
    _putenv_s("APS5_NO_DISPATCH_RECIPE", "1");
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
    setenv("APS5_PIPELINE_DMA", "1", 1);
    setenv("APS5_PIPELINE_DISPATCH", "1", 1);
    setenv("APS5_NO_DISPATCH_RECIPE", "1", 1);
#endif
    try {
        {
            const auto probe = OpenVulkanTestDevice();
            if (!probe) return VulkanTestSkipped;
        }
        const bool templateItems = std::getenv("APS5_NO_PIPELINE_DISPATCH_TEMPLATES") == nullptr;
        Shader shader{};
        shader.file_header = 0x34333231;
        shader.version = 0x18;
        shader.header_size = sizeof(Shader);
        shader.shader_size = sizeof(Code);
        shader.code = Code.data();
        AgcDriverRegisterShader_nid_postfix(&shader);
        Shader tableShader{};
        tableShader.file_header = 0x34333231;
        tableShader.version = 0x18;
        tableShader.header_size = sizeof(Shader);
        tableShader.shader_size = sizeof(TableCode);
        tableShader.code = TableCode.data();
        AgcDriverRegisterShader_nid_postfix(&tableShader);
        Input = NewBlock();
        Table = NewBlock();
        Output = NewBlock();
        TargetA = NewBlock();
        TargetB = NewBlock();
        constexpr std::size_t Rounds = 12;
        constexpr std::size_t Warmup = 4;
        std::uint64_t lateDrains = 0;
        for (std::size_t round = 0; round < Rounds; ++round) {
            auto* source = round % 2 == 0 ? TargetA : TargetB;
            const auto value = 0x5a000000u + static_cast<std::uint32_t>(round) * 0x01010101u;
            auto words = MovedRound(source, value);
            const auto drains = GatedRound([&] { Submit(words); });
            for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
                check(source[lane] == value, "the fill did not land in lane " + std::to_string(lane));
                check(Output[lane] == value + 1u, "round " + std::to_string(round) + ": lane " + std::to_string(lane) + " of the output is " + std::to_string(Output[lane]) + ", expected " + std::to_string(value + 1u) + " (the dispatch did not read the buffer filled just before it)");
            }
            std::printf("pipelined moved dispatch: round %zu from %s, dispatch drains %llu\n", round, round % 2 == 0 ? "A" : "B", static_cast<unsigned long long>(drains));
            if (round >= Warmup) lateDrains += drains;
        }
        if (templateItems) check(lateDrains == 0, "dispatches whose buffers moved drained the pipeline " + std::to_string(lateDrains) + " times after the warm-up");
        std::uint64_t tableDrains = 0;
        for (std::size_t round = 0; round < Rounds; ++round) {
            auto* target = round % 2 == 0 ? TargetA : TargetB;
            const auto value = 0x3c000000u + static_cast<std::uint32_t>(round) * 0x00010001u;
            const auto descriptor = BufferDescriptor(target, BufferBytes);
            std::copy(descriptor.begin(), descriptor.end(), Table);
            auto words = TableRound(target, value);
            const auto drains = GatedRound([&] { Submit(words); });
            for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
                check(Output[lane] == value + 1u, "table round " + std::to_string(round) + ": lane " + std::to_string(lane) + " of the output is " + std::to_string(Output[lane]) + ", expected " + std::to_string(value + 1u) + " (the dispatch did not read the buffer its table names, filled just before it)");
            }
            std::printf("pipelined moved dispatch: table round %zu to %s, dispatch drains %llu\n", round, round % 2 == 0 ? "A" : "B", static_cast<unsigned long long>(drains));
            if (round >= Warmup) tableDrains += drains;
        }
        if (templateItems) check(tableDrains == 0, "dispatches served by a template with moved buffers drained the pipeline " + std::to_string(tableDrains) + " times after the warm-up");
        if (!templateItems) check(lateDrains + tableDrains >= 2 * (Rounds - Warmup), "dispatches without a recipe did not drain the pipeline with template items off (" + std::to_string(lateDrains + tableDrains) + " drains)");
        LibcRunShutdown_nid_postfix();
        std::puts(templateItems ? "pipelined moved dispatch tests passed" : "pipelined moved dispatch tests passed (template items off)");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { LibcRunShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
