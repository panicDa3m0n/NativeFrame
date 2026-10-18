#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <thread>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t ReleaseMemHeader = 0xc0064900u;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "unimported label order: cannot allocate the guest block");
        std::memset(block, 0, BlockBytes);
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

    volatile std::uint32_t* Words() { return reinterpret_cast<volatile std::uint32_t*>(block); }

    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

alignas(64) volatile std::uint32_t commandMemory[16] = {};

std::uint64_t AddressOf(volatile const std::uint32_t* word) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(word));
}

int StoreLabel(AgcDriver::VulkanDevice& device, std::uint64_t address, std::uint32_t value, std::uint64_t stamp) {
    const auto bytes = std::as_bytes(std::span(&value, 1));
    const int reason = device.WriteLabelOnGpu(address, bytes, stamp, 0);
    if (reason >= 1 && reason <= 4) {
        if (reason != 1) device.WaitIdle();
        AgcDriver::GuestMemory::Write(address, bytes, 4);
    }
    return reason;
}

}

int main() {
    try {
        GuestBlock labels;
        GuestBlock temporary;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        const std::array<std::uint32_t, 4> pattern{0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
        if (!device->FillBuffer(labels.Address() + 4096, 4096, pattern)) {
            std::printf("skipped, the device does not import guest memory\n");
            return VulkanTestSkipped;
        }
        {
            const auto imported = AgcDriver::Graphics::HostImportLiveBytes();
            const std::array<std::uint32_t, 2> value{0x76543210u, 0xfedcba98u};
            const auto bytes = std::as_bytes(std::span(value));
            const auto address = temporary.Address() + 4092;
            const int reason = device->WriteLabelOnGpu(address, bytes, 90, 0);
            Require(reason == 1 && AgcDriver::Graphics::HostImportLiveBytes() == imported, "an eight-byte label did not select its unimported CPU path");
            Require(labels.Words()[1024] == pattern[0], "the targeted label returned before preceding GPU work completed");
            device->WaitIdle();
            AgcDriver::GuestMemory::Write(address, bytes, 4);
            Require(labels.Words()[1024] == pattern[0], "the CPU label overtook earlier GPU work");
            Require(temporary.Words()[1023] == value[0] && temporary.Words()[1024] == value[1], "the unimported eight-byte label did not land");
            Require(temporary.Words()[1022] == 0 && temporary.Words()[1025] == 0, "the CPU label changed neighboring words");
            Require(device->FillBuffer(labels.Address() + 4096, 4096, pattern), "cannot record work after the unimported label");
        }
        StoreLabel(*device, AddressOf(&commandMemory[8]), 1, 1);
        StoreLabel(*device, labels.Address() + 4, 2, 2);
        device->SubmitRecorded(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (labels.Words()[1] != 2) {
            Require(std::chrono::steady_clock::now() < deadline, "unimported label order: the later label never landed");
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        const std::uint32_t seen = commandMemory[8];
        commandMemory[8] = ReleaseMemHeader;
        device->WaitIdle();
        const std::uint32_t reused = commandMemory[8];
        std::array<char, 256> message{};
        std::snprintf(message.data(), message.size(), "unimported label order: the label held 0x%08x when a later label had landed, and the guest's reused word holds 0x%08x instead of 0x%08x", seen, reused, ReleaseMemHeader);
        Require(seen == 1 && reused == ReleaseMemHeader, message.data());
        std::puts("unimported label order tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
