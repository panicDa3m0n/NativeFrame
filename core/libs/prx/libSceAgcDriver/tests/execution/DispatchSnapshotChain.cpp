#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32u;
constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t InputOffset = 0x0000;
constexpr std::size_t ProducedOffset = 0x1000;
constexpr std::size_t ConsumedOffset = 0x2000;
constexpr std::uint32_t Fill = 0xdeadbeefu;

alignas(256) constexpr std::array<std::uint32_t, 31> Code{
    0x34020083u, 0x34040084u, 0xe0341000u, 0x80000401u, 0xbf8c3f70u, 0xd7630008u, 0x00020b04u,
    0xbe8c0380u, 0xbe8d0380u, 0xf4240200u, 0x18000000u, 0xbf8cc07fu, 0xbf060d0du, 0x920a0908u,
    0x850b8081u, 0xd7610009u, 0x00001a0au, 0xd761000au, 0x00001a0bu, 0xbf070d0du, 0x920a0908u,
    0x850b8081u, 0xd761000bu, 0x00001a0bu, 0x800c880cu, 0x800d810du, 0xbf0aa00du, 0xbf85ffedu,
    0xe0781000u, 0x80010802u, 0xbf810000u
};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "dispatch snapshot chain: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint32_t* Words(std::size_t offset) { return reinterpret_cast<std::uint32_t*>(block + offset); }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 4> bufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::uint32_t expectedMask(std::uint32_t width, std::uint32_t offset) {
    width &= 31u;
    offset &= 31u;
    std::uint32_t mask = 0u;
    for (std::uint32_t bit = offset; bit < 32u && bit - offset < width; ++bit) mask |= 1u << bit;
    return mask;
}

ShaderRecompiler::RecompileResult compile(AgcDriver::VulkanDevice& device, const std::uint32_t* input, const std::uint32_t* output) {
    std::array<std::uint32_t, 8> userData{};
    const auto source = bufferDescriptor(input, Threads * 2u * 4u);
    const auto destination = bufferDescriptor(output, Threads * 4u * 4u);
    std::copy(source.begin(), source.end(), userData.begin());
    std::copy(destination.begin(), destination.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}},
        {32u, 0u, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0u, 0u, 0u, 128u}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

void run(AgcDriver::VulkanDevice& device) {
    GuestBlock block;
    auto* input = block.Words(InputOffset);
    auto* produced = block.Words(ProducedOffset);
    auto* consumed = block.Words(ConsumedOffset);
    const auto producer = compile(device, input, produced);
    const auto consumer = compile(device, produced, consumed);
    const auto program = reinterpret_cast<std::uintptr_t>(Code.data());
    for (std::uint32_t round = 0u; round < 4u; ++round) {
        for (std::uint32_t thread = 0u; thread < Threads; ++thread) {
            input[thread * 2u] = (thread * 7u + round * 5u + 3u) % 32u;
            input[thread * 2u + 1u] = (thread + round * 3u) % 32u;
        }
        AgcDriver::GuestMemory::MarkWritten(reinterpret_cast<std::uint64_t>(input), Threads * 2u * 4u);
        std::fill(produced, produced + Threads * 4u, Fill);
        std::fill(consumed, consumed + Threads * 4u, Fill);
        AgcDriver::GuestMemory::MarkWritten(reinterpret_cast<std::uint64_t>(produced), Threads * 4u * 4u);
        AgcDriver::GuestMemory::MarkWritten(reinterpret_cast<std::uint64_t>(consumed), Threads * 4u * 4u);
        device.Dispatch(producer, 1u, 1u, 1u, {}, program);
        device.Dispatch(consumer, 1u, 1u, 1u, {}, program);
        device.WaitIdle();
        std::array<std::uint32_t, Threads * 4u> expectedProduced{};
        for (std::uint32_t thread = 0u; thread < Threads; ++thread) {
            const auto mask = expectedMask(input[thread * 2u], input[thread * 2u + 1u]);
            expectedProduced[thread * 4u] = mask;
            expectedProduced[thread * 4u + 1u] = mask;
            expectedProduced[thread * 4u + 2u] = 1u;
            expectedProduced[thread * 4u + 3u] = 0u;
        }
        for (std::uint32_t word = 0u; word < Threads * 4u; ++word) {
            Require(produced[word] == expectedProduced[word], "round " + std::to_string(round) + ": the producer read stale CPU input at word " + std::to_string(word) + " (" + std::to_string(produced[word]) + ", expected " + std::to_string(expectedProduced[word]) + ")");
        }
        for (std::uint32_t thread = 0u; thread < Threads; ++thread) {
            const auto mask = expectedMask(expectedProduced[thread * 2u], expectedProduced[thread * 2u + 1u]);
            Require(consumed[thread * 4u] == mask, "round " + std::to_string(round) + ": the consumer of thread " + std::to_string(thread) + " did not read the producer's GPU results (" + std::to_string(consumed[thread * 4u]) + ", expected " + std::to_string(mask) + ")");
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (device->Target().subgroupSize < Threads) {
            std::printf("skipped, subgroup size %u cannot hold a wave32\n", device->Target().subgroupSize);
            return VulkanTestSkipped;
        }
        run(*device);
        std::puts("4 dispatch chains read CPU input and the producer's GPU results");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
