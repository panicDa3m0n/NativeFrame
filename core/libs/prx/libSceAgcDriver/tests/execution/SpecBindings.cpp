#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
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
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

// Speculative draw bindings (APS5_SPEC_BINDINGS=1, O16.T9): a prepare thread copies a draw's
// read-only guest buffer off the GPU mutex, and the commit takes the copy only when nothing that
// lands before the draw touched the buffer meanwhile. Each case makes the speculation on another
// thread, then the event, then the commit's check under the mutex:
//   a GPU writer recorded meanwhile (still pending): refused, the draw reads the buffer in place;
//   a GPU writer whose batch finished meanwhile (its MarkWritten): refused, a fresh copy;
//   a label stored over the buffer meanwhile: refused, a fresh copy holds the label;
//   a CPU store collected meanwhile: refused, a fresh copy;
//   nothing meanwhile: taken, the commit binds the prepare thread's copy.
// Without the switch (the _off variant) nothing is speculated and every draw makes its own copy.

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {

using namespace AgcDriver::Graphics;

constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t ElementOffset = 4096;
constexpr std::size_t ElementBytes = 1024;

// Guest memory as a title maps it: a shared section in the arena (with a second alias), which keeps
// its write watch when it is imported for in-place reads.
class WatchedBlock {
public:
    WatchedBlock() {
#ifdef _WIN32
        if (!AgcDriver::GuestMemory::WriteWatched()) return;
        block = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes));
        if (block == nullptr) return;
        const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, BlockBytes, nullptr);
        Require(section != nullptr, "cannot create the shared test memory");
        GuestArena::GuestArenaMap_nid_postfix(block, BlockBytes, section, 0, PAGE_READWRITE);
        twin = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes));
        Require(twin != nullptr, "cannot reserve the second mapping of the shared test memory");
        GuestArena::GuestArenaMap_nid_postfix(twin, BlockBytes, section, 0, PAGE_READWRITE);
        CloseHandle(section);
        Require(AgcDriver::GuestMemory::Watched(Address(), BlockBytes), "the test block is not watched");
#endif
    }

    ~WatchedBlock() {
#ifdef _WIN32
        if (twin != nullptr) {
            GuestArena::GuestArenaReset_nid_postfix(twin, BlockBytes);
            GuestArena::GuestArenaRelease_nid_postfix(twin, BlockBytes);
        }
        if (block != nullptr) {
            GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
            GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
        }
#endif
    }

    WatchedBlock(const WatchedBlock&) = delete;
    WatchedBlock& operator=(const WatchedBlock&) = delete;

    std::byte* Bytes() const { return block; }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(block); }

private:
    std::byte* block = nullptr;
    std::byte* twin = nullptr;
};

bool Holds(std::span<const std::byte> bytes, std::byte value) {
    return std::all_of(bytes.begin(), bytes.end(), [&](std::byte byte) { return byte == value; });
}

int Run(const Context& deviceContext, bool speculating) {
    WatchedBlock watched;
    if (watched.Bytes() == nullptr) {
        std::cout << "write-watched shared guest memory unavailable: speculative bindings not tested\n";
        return VulkanTestSkipped;
    }
    const auto address = watched.Address();
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(watched.Bytes(), BlockBytes, true, true, true);
    }
    struct Unregister {
        void* block;
        ~Unregister() {
            GuestAllocations::Mutation mutation;
            mutation.Remove(block);
        }
    } unregister{watched.Bytes()};
    if (deviceContext.hostImportAlignment == 0 || HostImportFor(deviceContext, address, BlockBytes) == nullptr) {
        std::cout << "host imports unavailable: speculative bindings not tested\n";
        return VulkanTestSkipped;
    }
    const auto element = address + ElementOffset;
    if (!AgcDriver::GuestMemory::Watched(element, ElementBytes)) {
        std::cout << "the imported guest memory is no longer write-watched: speculative bindings not tested\n";
        return VulkanTestSkipped;
    }
    auto* bytes = watched.Bytes() + ElementOffset;
    const auto fill = [&](std::uint8_t value) {
        std::memset(bytes, value, ElementBytes);
        AgcDriver::GuestMemory::CollectWritesUncached(element, ElementBytes);
    };
    fill(0x11);

    ShaderRecompiler::RecompileResult program;
    ShaderRecompiler::DescriptorBinding binding;
    binding.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    binding.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
    binding.descriptorSet = 0;
    binding.binding = 0;
    binding.count = 1;
    binding.guestDescriptor = {static_cast<std::uint32_t>(element), static_cast<std::uint32_t>(element >> 32u) & 0xffffu, static_cast<std::uint32_t>(ElementBytes), 0x31000000u};
    binding.bufferWritten = {false};
    program.bindings.push_back(binding);
    const CompiledShader compute{ShaderRecompiler::ShaderStage::Compute, &program, 0};
    const std::span<const CompiledShader> shaders(&compute, 1);

    auto context = deviceContext;
    DescriptorCache cache(context);
    context.descriptorCache = &cache;
    Recorder recorder(context);
    const ResourceCache::Key key{0x5bec0b1du, 9u};
    const PreparedDrawKey prepared{key, ResourceCache::Hash(key)};
    std::shared_ptr<ShaderResources> resources;
    struct Forget {
        const ResourceCache::Key& key;
        ~Forget() { SharedResourceCache().Remove(key); }
    } forget{key};
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        recorder.Activate();
        resources = std::make_shared<ShaderResources>(context, compute);
        SharedResourceCache().Insert(key, resources);
        // The first use publishes the in-place reads the speculation decides with.
        const auto first = resources->PrepareDrawBindings(recorder);
        Require(first != nullptr && first->snapshots.size() == 1 && Holds(first->snapshots[0].Bytes(), std::byte{0x11}), "the template's read-only buffer was not snapshotted");
        recorder.Submit();
        recorder.Sync();
    }

    const auto speculate = [&] {
        std::shared_ptr<DrawSpeculation> made;
        std::thread prepareThread([&] { made = SpeculateDrawBindings(prepared, shaders); });
        prepareThread.join();
        Require(made != nullptr, "no speculation object");
        if (speculating) {
            Require(made->ready && made->bindings != nullptr && made->bindings->snapshots.size() == 1, "the prepare thread did not speculate the draw's bindings");
            Require(Holds(made->bindings->snapshots[0].Bytes(), std::byte{0x11}), "the speculative copy does not hold the buffer");
        } else {
            Require(!made->ready && made->bindings == nullptr, "a speculation was made without APS5_SPEC_BINDINGS");
        }
        return made;
    };
    const auto commit = [&](const DrawSpeculation& made, bool& taken) {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        std::shared_ptr<ShaderResources::DrawBindings> bindings;
        taken = TakeSpeculativeBindings(&made, *resources, {}, recorder, bindings);
        if (!taken) bindings = resources->PrepareDrawBindings(recorder);
        return bindings;
    };
    const auto settle = [&] {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        recorder.Submit();
        recorder.Sync();
    };
    const auto refused = [&](bool taken, const std::string& what) {
        Require(!taken, what + ": the commit took a stale speculative copy");
    };

    // A GPU writer recorded meanwhile, still pending at the commit.
    fill(0x11);
    {
        const auto made = speculate();
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            std::memset(bytes, 0x22, ElementBytes);
            recorder.NotePendingWrite(element, ElementBytes);
        }
        bool taken = false;
        const auto bindings = commit(*made, taken);
        refused(taken, "a pending GPU writer");
        Require(bindings == nullptr, "a draw behind a pending GPU writer did not read the buffer in place");
        settle();
    }

    // A GPU writer whose batch finished before the commit (stamped by its MarkWritten).
    fill(0x11);
    {
        const auto made = speculate();
        std::memset(bytes, 0x33, ElementBytes);
        AgcDriver::GuestMemory::MarkWritten(element, ElementBytes);
        bool taken = false;
        const auto bindings = commit(*made, taken);
        refused(taken, "a finished GPU writer");
        Require(bindings != nullptr && bindings->snapshots.size() == 1 && Holds(bindings->snapshots[0].Bytes(), std::byte{0x33}), "the draw after a finished GPU writer does not read its result");
        settle();
    }

    // A label stored over the buffer before the commit.
    fill(0x11);
    {
        const auto made = speculate();
        std::array<std::byte, 8> label{};
        label.fill(std::byte{0x44});
        AgcDriver::GuestMemory::StoreOwnBytes(element + 16, label);
        bool taken = false;
        const auto bindings = commit(*made, taken);
        refused(taken, "a label store");
        Require(bindings != nullptr && bindings->snapshots.size() == 1 && Holds(bindings->snapshots[0].Bytes().subspan(16, label.size()), std::byte{0x44}), "the draw after a label store does not read the label");
        settle();
    }

    // A CPU store collected before the commit.
    fill(0x11);
    {
        const auto made = speculate();
        std::memset(bytes, 0x55, ElementBytes);
        AgcDriver::GuestMemory::CollectWritesUncached(element, ElementBytes);
        bool taken = false;
        const auto bindings = commit(*made, taken);
        refused(taken, "a collected CPU store");
        Require(bindings != nullptr && bindings->snapshots.size() == 1 && Holds(bindings->snapshots[0].Bytes(), std::byte{0x55}), "the draw after a collected CPU store does not read it");
        settle();
    }

    // Nothing meanwhile: the commit binds the prepare thread's copy.
    fill(0x11);
    {
        const auto made = speculate();
        bool taken = false;
        const auto bindings = commit(*made, taken);
        Require(taken == speculating, speculating ? "the commit refused a speculation nothing invalidated" : "the commit took a speculation without the switch");
        Require(bindings != nullptr && bindings->snapshots.size() == 1 && Holds(bindings->snapshots[0].Bytes(), std::byte{0x11}), "the draw does not read the buffer");
        if (speculating) Require(bindings == made->bindings, "the taken bindings are not the prepare thread's");
        settle();
    }

    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        SharedResourceCache().Remove(key);
        resources.reset();
    }
    std::cout << "speculative bindings " << (speculating ? "taken only when nothing touched the buffer" : "off: every draw made its own") << '\n';
    return 0;
}

}

int main() {
    try {
        const bool speculating = std::getenv("APS5_SPEC_BINDINGS") != nullptr;
        auto device = OpenVulkanTestDevice();
        if (device == nullptr) return VulkanTestSkipped;
        return Run(AgcDriver::VulkanDeviceTestAccess::ContextOf(*device), speculating);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
