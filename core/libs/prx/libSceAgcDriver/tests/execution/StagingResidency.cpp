#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>

namespace {

using AgcDriver::Graphics::GuestBufferMemory;
using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 1024u;
constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t GuestBytes = 32 * BlockBytes;
constexpr std::size_t Slice = Threads * 16u;
constexpr std::size_t FirstInput = 0x0000;
constexpr std::size_t SecondInput = 0x2000;
constexpr std::size_t Shared = BlockBytes;
constexpr std::uint32_t Tail = 0x7a7a7a7au;
constexpr std::uint32_t CpuTail = 0x24681357u;

alignas(256) constexpr std::array<std::uint32_t, 31> Code{
    0x34020083u, 0x34040084u, 0xe0341000u, 0x80000401u, 0xbf8c3f70u, 0xd7630008u, 0x00020b04u,
    0xbe8c0380u, 0xbe8d0380u, 0xf4240200u, 0x18000000u, 0xbf8cc07fu, 0xbf060d0du, 0x920a0908u,
    0x850b8081u, 0xd7610009u, 0x00001a0au, 0xd761000au, 0x00001a0bu, 0xbf070d0du, 0x920a0908u,
    0x850b8081u, 0xd761000bu, 0x00001a0bu, 0x800c880cu, 0x800d810du, 0xbf0aa00du, 0xbf85ffedu,
    0xe0781000u, 0x80010802u, 0xbf810000u
};

class WatchedGuest {
public:
    WatchedGuest() {
        if (!AgcDriver::GuestMemory::WriteWatched()) return;
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(GuestBytes, BlockBytes));
        if (block == nullptr) return;
        GuestArena::GuestArenaCommit_nid_postfix(block, GuestBytes, PAGE_READWRITE, GuestBytes);
#else
        void* raw = mmap(nullptr, GuestBytes + BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(raw != MAP_FAILED, "staging residency: cannot map the guest block");
        const auto begin = reinterpret_cast<std::uintptr_t>(raw);
        const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
        if (aligned != begin) munmap(raw, aligned - begin);
        if (aligned + GuestBytes != begin + GuestBytes + BlockBytes) munmap(reinterpret_cast<void*>(aligned + GuestBytes), begin + BlockBytes - aligned);
        block = reinterpret_cast<std::uint8_t*>(aligned);
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, GuestBytes);
#endif
        GuestAllocations::Mutation().Add(block, GuestBytes, true, true, true);
    }

    ~WatchedGuest() {
        if (block == nullptr) return;
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, GuestBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, GuestBytes);
#else
        munmap(block, GuestBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, GuestBytes);
#endif
    }

    WatchedGuest(const WatchedGuest&) = delete;
    WatchedGuest& operator=(const WatchedGuest&) = delete;

    bool Available() const { return block != nullptr; }
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

ShaderRecompiler::RecompileResult compile(AgcDriver::VulkanDevice& device, const std::uint32_t* input, const std::uint32_t* output, std::uint32_t outputBytes) {
    std::array<std::uint32_t, 8> userData{};
    const auto source = bufferDescriptor(input, Threads * 2u * 4u);
    const auto destination = bufferDescriptor(output, outputBytes);
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

void fillInput(std::uint32_t* input, std::uint32_t seed) {
    for (std::uint32_t thread = 0u; thread < Threads; ++thread) {
        input[thread * 2u] = (thread * 7u + seed * 5u + 3u) % 32u;
        input[thread * 2u + 1u] = (thread + seed * 3u) % 32u;
    }
}

void checkProduced(const std::uint32_t* input, const std::uint32_t* output, const std::string& what) {
    for (std::uint32_t thread = 0u; thread < Threads; ++thread) {
        const auto mask = expectedMask(input[thread * 2u], input[thread * 2u + 1u]);
        const std::array<std::uint32_t, 4> expected{mask, mask, 1u, 0u};
        for (std::uint32_t word = 0u; word < 4u; ++word) {
            const auto actual = output[thread * 4u + word];
            Require(actual == expected[word], what + ": word " + std::to_string(thread * 4u + word) + " holds " + std::to_string(actual) + ", expected " + std::to_string(expected[word]));
        }
    }
}

void checkWords(const std::uint32_t* words, std::size_t count, std::uint32_t value, const std::string& what) {
    for (std::size_t word = 0; word < count; ++word) {
        Require(words[word] == value, what + ": word " + std::to_string(word) + " holds " + std::to_string(words[word]) + ", expected " + std::to_string(value));
    }
}

int run(AgcDriver::VulkanDevice& device) {
    WatchedGuest guest;
    if (!guest.Available()) {
        std::puts("skipped, write watching unavailable");
        return VulkanTestSkipped;
    }
    auto* firstInput = guest.Words(FirstInput);
    auto* secondInput = guest.Words(SecondInput);
    auto* head = guest.Words(Shared);
    auto* middle = guest.Words(Shared + Slice);
    auto* tail = guest.Words(Shared + 2 * Slice);
    std::fill(head, head + 3 * Slice / 4, Tail);
    const auto whole = compile(device, firstInput, head, static_cast<std::uint32_t>(3 * Slice));
    const auto upper = compile(device, secondInput, middle, static_cast<std::uint32_t>(2 * Slice));
    const auto program = reinterpret_cast<std::uintptr_t>(Code.data());
    const auto before = GuestBufferMemory::StagingSources();
    auto tailValue = Tail;
    for (std::uint32_t round = 0u; round < 6u; ++round) {
        fillInput(firstInput, round);
        fillInput(secondInput, round + 11u);
        const bool cpuMiddle = round == 3u;
        constexpr std::uint32_t CpuValue = 0x13572468u;
        if (round == 4u) {
            std::fill(tail, tail + Slice / 4, CpuTail);
            tailValue = CpuTail;
        }
        if (cpuMiddle) std::fill(middle, middle + Slice / 4, CpuValue);
        else device.Dispatch(upper, 1u, 1u, 1u, {}, program);
        device.Dispatch(whole, 1u, 1u, 1u, {}, program);
        device.WaitIdle();
        const auto label = "round " + std::to_string(round);
        checkProduced(firstInput, head, label + ", the slice the whole-range dispatch wrote");
        if (cpuMiddle) checkWords(middle, Slice / 4, CpuValue, label + ", the slice the CPU wrote under the whole-range dispatch");
        else checkProduced(secondInput, middle, label + ", the slice the upper dispatch wrote under the whole-range dispatch");
        checkWords(tail, Slice / 4, tailValue, label + ", the slice no dispatch wrote");
        if (round == 0u && GuestBufferMemory::StagingSources().importBytes == before.importBytes) {
            std::puts("skipped, the written ranges were not staged in device memory");
            return VulkanTestSkipped;
        }
    }
    const auto middleBeforeRepeat = GuestBufferMemory::StagingSources();
    for (int repeat = 0; repeat < 2; ++repeat) {
        device.Dispatch(whole, 1u, 1u, 1u, {}, program);
        device.WaitIdle();
        const auto label = "repeat " + std::to_string(repeat);
        checkProduced(firstInput, head, label + ", the slice the whole-range dispatch wrote");
        checkProduced(secondInput, middle, label + ", the slice the upper dispatch wrote last");
        checkWords(tail, Slice / 4, tailValue, label + ", the slice no dispatch wrote");
    }
    const auto after = GuestBufferMemory::StagingSources();
    const bool resident = std::getenv("APS5_NO_RESIDENT_STAGING") == nullptr;
    const auto fromDevice = after.deviceBytes - before.deviceBytes;
    const auto kept = after.keptBytes - middleBeforeRepeat.keptBytes;
    std::printf("staged copy-ins: %llu bytes kept, %llu from device copies, %llu from the import\n", static_cast<unsigned long long>(after.keptBytes - before.keptBytes), static_cast<unsigned long long>(fromDevice), static_cast<unsigned long long>(after.importBytes - before.importBytes));
    if (resident) {
        Require(fromDevice != 0, "no staged range was copied in from another staging buffer that copied it back");
        Require(kept != 0, "a staged range nothing wrote since its copy-back was copied in again");
    } else {
        Require(fromDevice == 0 && after.keptBytes == before.keptBytes, "with APS5_NO_RESIDENT_STAGING a staged range was not copied in from its import");
    }
    return 0;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (device->Target().subgroupSize < 32u) {
            std::printf("skipped, subgroup size %u cannot hold a wave32\n", device->Target().subgroupSize);
            return VulkanTestSkipped;
        }
        return run(*device);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
