#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <SDL_loadso.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t Blocks = 3;
constexpr std::size_t SnapshotBytes = Blocks * BlockBytes;
constexpr int Skipped = 77;

class Device {
public:
    Device() {
#ifdef _WIN32
        library = SDL_LoadObject("vulkan-1.dll");
#else
        library = SDL_LoadObject("libvulkan.so.1");
#endif
        Require(library != nullptr, "cannot load Vulkan");
        try {
            instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(library, "vkGetInstanceProcAddr"));
            Require(instanceProc != nullptr, "missing Vulkan instance resolver");
            VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            application.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &application;
            Check(function<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &instance), "vkCreateInstance");
            std::uint32_t count = 0;
            const auto enumerate = function<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
            Check(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
            Require(count != 0, "no Vulkan device");
            std::vector<VkPhysicalDevice> devices(count);
            Check(enumerate(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
            context.physical = devices.front();
            const auto queues = function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
            queues(context.physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            queues(context.physical, &count, families.data());
            std::uint32_t family = 0;
            while (family < count && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) ++family;
            Require(family < count, "no Vulkan compute queue");
            const float priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            device.queueCreateInfoCount = 1;
            device.pQueueCreateInfos = &queue;
            Check(function<PFN_vkCreateDevice>("vkCreateDevice")(context.physical, &device, nullptr, &context.device), "vkCreateDevice");
            context.deviceProc = function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
            function<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(context.physical, &context.memory);
            VkPhysicalDeviceProperties properties{};
            function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(context.physical, &properties);
            context.limits = properties.limits;
            context.formatProperties = function<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
            context.imageFormatProperties = function<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties");
            context.Function<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(context.device, family, 0, &context.queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = family;
            Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &pool, nullptr, &context.pool), "vkCreateCommandPool");
        } catch (...) {
            release();
            throw;
        }
    }

    ~Device() { release(); }
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    const Context& GetContext() const { return context; }

private:
    template<typename TFunction>
    TFunction function(const char* name) const {
        const auto result = reinterpret_cast<TFunction>(instanceProc(instance, name));
        Require(result != nullptr, name);
        return result;
    }

    void release() noexcept {
        if (context.device != VK_NULL_HANDLE) ClearCachedTextures(context.device);
        if (context.pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, context.pool, nullptr);
        context.bufferPool.reset();
        if (context.device != VK_NULL_HANDLE) function<PFN_vkDestroyDevice>("vkDestroyDevice")(context.device, nullptr);
        if (instance != VK_NULL_HANDLE) function<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (library != nullptr) SDL_UnloadObject(library);
    }

    void* library = nullptr;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    Context context{};
};

class WatchedBlock {
public:
    WatchedBlock() {
        if (!AgcDriver::GuestMemory::WriteWatched()) return;
#ifdef _WIN32
        block = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(SnapshotBytes, BlockBytes));
        if (block == nullptr) return;
        GuestArena::GuestArenaCommit_nid_postfix(block, SnapshotBytes, PAGE_READWRITE, SnapshotBytes);
#else
        void* raw = mmap(nullptr, SnapshotBytes + BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(raw != MAP_FAILED, "cannot map the watched block");
        const auto begin = reinterpret_cast<std::uintptr_t>(raw);
        const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
        if (aligned != begin) munmap(raw, aligned - begin);
        if (aligned + SnapshotBytes != begin + SnapshotBytes + BlockBytes) munmap(reinterpret_cast<void*>(aligned + SnapshotBytes), begin + BlockBytes - aligned);
        block = reinterpret_cast<std::byte*>(aligned);
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, SnapshotBytes);
#endif
        Require(AgcDriver::GuestMemory::Watched(Address(), SnapshotBytes), "the watched block is not watched");
    }

    ~WatchedBlock() {
        if (block == nullptr) return;
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, SnapshotBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, SnapshotBytes);
#else
        munmap(block, SnapshotBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, SnapshotBytes);
#endif
    }

    WatchedBlock(const WatchedBlock&) = delete;
    WatchedBlock& operator=(const WatchedBlock&) = delete;

    std::byte* Bytes() const { return block; }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(block); }

private:
    std::byte* block = nullptr;
};

bool holds(Buffer& buffer, const std::byte* guest) {
    const auto held = buffer.Bytes();
    return held.size() == SnapshotBytes && std::memcmp(held.data(), guest, SnapshotBytes) == 0;
}

int run(const Context& context) {
    WatchedBlock watched;
    if (watched.Bytes() == nullptr) {
        std::cout << "write watching unavailable: snapshot refresh not tested\n";
        return Skipped;
    }
    auto* guest = watched.Bytes();
    for (std::size_t offset = 0; offset < SnapshotBytes; ++offset) guest[offset] = static_cast<std::byte>((offset * 7u + 3u) & 0xffu);
    const auto address = watched.Address();
    const auto registry = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const auto properties = context.SnapshotProperties();
    const bool deviceLocal = (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    const bool shadowed = std::getenv("APS5_NO_SNAPSHOT_SHADOW") == nullptr;
    Recorder recorder(context);
    auto buffer = std::make_shared<Buffer>(context, SnapshotBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, properties);
    const Buffer* const kept = buffer.get();
    const auto generation = AgcDriver::GuestMemory::CollectWrites(address, SnapshotBytes);
    Require(generation != 0, "the watched block has no generation");
    auto shadow = Recorder::FillDrawSnapshot(*buffer, address, SnapshotBytes);
    Require((shadow != nullptr) == (deviceLocal && shadowed), deviceLocal ? "a device-local snapshot does not hold the host copy its switch asks for" : "a host snapshot holds a second host copy");
    Require(holds(*buffer, guest), "a filled snapshot does not hold the guest bytes");
    recorder.KeepDrawSnapshot(address, SnapshotBytes, generation, registry, std::move(buffer), Recorder::SnapshotUse::Storage, 0, std::move(shadow));
    const bool compared = !deviceLocal || shadowed;

    const auto refresh = [&](const std::string& what) {
        const auto before = Recorder::SnapshotRefreshes();
        const auto collected = AgcDriver::GuestMemory::CollectWrites(address, SnapshotBytes);
        const auto reused = recorder.ReusableDrawSnapshot(address, SnapshotBytes, Recorder::SnapshotUse::Storage, nullptr, collected);
        const auto after = Recorder::SnapshotRefreshes();
        Require(reused.get() == kept, what + ": the snapshot was not refreshed in place");
        Require(after.refreshes - before.refreshes == 1, what + ": the snapshot was not refreshed once");
        Require(holds(*reused, guest), what + ": the refreshed snapshot does not hold the guest bytes");
        return after.copiedBytes - before.copiedBytes;
    };

    std::vector<std::byte> same(guest, guest + BlockBytes);
    std::memcpy(guest, same.data(), BlockBytes);
    const auto sameCopied = refresh("a store of the same bytes");
    Require(compared ? sameCopied == 0 : sameCopied == BlockBytes, "a store of the same bytes copied " + std::to_string(sameCopied) + " bytes into the snapshot");

    guest[BlockBytes + 100] ^= std::byte{0xff};
    const auto changedCopied = refresh("a store that changed one byte");
    Require(changedCopied == BlockBytes, "a store that changed one byte of one block copied " + std::to_string(changedCopied) + " bytes");

    std::memcpy(guest, same.data(), BlockBytes);
    guest[SnapshotBytes - 1] ^= std::byte{0x5a};
    const auto mixedCopied = refresh("a store of the same bytes beside a changed block");
    Require(compared ? mixedCopied == BlockBytes : mixedCopied == 2 * BlockBytes, "a store of the same bytes beside a changed block copied " + std::to_string(mixedCopied) + " bytes");

    for (int round = 0; round < 4; ++round) {
        for (std::size_t offset = round; offset < SnapshotBytes; offset += 4099) guest[offset] = static_cast<std::byte>(round * 31 + 1);
        refresh("round " + std::to_string(round) + " of scattered stores");
    }

    std::cout << (deviceLocal ? "device-local" : "host") << " snapshot " << (compared ? "compared" : "copied whole blocks") << ": same bytes copied " << sameCopied << ", one changed block " << changedCopied << ", mixed " << mixedCopied << '\n';
    return 0;
}

}

int main() {
    try {
        Device device;
        return run(device.GetContext());
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
