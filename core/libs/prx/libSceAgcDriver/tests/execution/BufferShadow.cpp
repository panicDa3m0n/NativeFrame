#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

// Large written buffers in range slabs (APS5_BUFFER_SHADOW=1, O16.T13): a recorded use of a written
// region of at least 1 MiB inside a watched import binds a range slab, gets only the bytes the slab
// is not authoritative for copied in, and copies its written sub-ranges back from the slab into the
// import as from a staging buffer; the slab keeps them fresh for the next use. The shader is a
// vkCmdFillBuffer over the region's descriptor, and each use ends as MarkGpuWrites ends a use's
// buffers (copy-backs, pending-write note, stamps, staged copies):
//   ScrittoreLettore: the written bytes of a 3 MiB region reach the import, from a range slab (off:
//     bound in place, as today for a written region past the 2 MiB staging window);
//   ScritturaParziale: a writer of half the region leaves the other half as the import had it;
//   CpuScriveDopo: a CPU store on one page between two uses survives the second use;
//   SemiSoloMancanti: a use after a use seeds nothing (the slab holds every byte);
//   TimbroScrittore: the use stamps the region, so consumers see its bytes change;
//   RiusoSlabCambiato: a reused upload whose slab grew meanwhile is refused;
//   RitiroImport: an import retired between the upload and the copy-back still gets the bytes.
// --evict (APS5_UNIT_SHADOW_MIB=16, Sfratto): three 12 MiB writers each land in the import and the
// live slab bytes stay within the budget. Without the switch (_off) the regions take today's paths.

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {

using namespace AgcDriver::Graphics;

constexpr std::uint64_t Unit = 65536;
constexpr std::uint64_t Page = 4096;
constexpr std::size_t RegionBytes = std::size_t{3} << 20u;

// Guest memory as a title maps it: a shared section in the arena, which keeps its write watch when
// it is imported.
class WatchedBlock {
public:
    explicit WatchedBlock(std::size_t bytes) : bytes(bytes) {
#ifdef _WIN32
        if (!AgcDriver::GuestMemory::WriteWatched()) return;
        block = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, Unit));
        if (block == nullptr) return;
        const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, static_cast<DWORD>(static_cast<std::uint64_t>(bytes) >> 32u), static_cast<DWORD>(bytes), nullptr);
        Require(section != nullptr, "cannot create the shared test memory");
        GuestArena::GuestArenaMap_nid_postfix(block, bytes, section, 0, PAGE_READWRITE);
        CloseHandle(section);
        Require(AgcDriver::GuestMemory::Watched(Address(), bytes), "the test block is not watched");
#endif
    }

    ~WatchedBlock() {
#ifdef _WIN32
        if (block != nullptr) {
            GuestArena::GuestArenaReset_nid_postfix(block, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
        }
#endif
    }

    WatchedBlock(const WatchedBlock&) = delete;
    WatchedBlock& operator=(const WatchedBlock&) = delete;

    std::byte* Bytes() const { return block; }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(block); }

private:
    std::size_t bytes;
    std::byte* block = nullptr;
};

bool Holds(const std::byte* bytes, std::size_t count, std::uint8_t value) {
    return std::all_of(bytes, bytes + count, [&](std::byte byte) { return byte == std::byte{value}; });
}

// The number after `key` in the [storage] shadow segment.
double ReportValue(const std::string& report, std::string_view key) {
    const auto at = report.find(key);
    Require(at != std::string::npos, "the shadow report has no '" + std::string(key) + "': " + report);
    return std::strtod(report.c_str() + at + key.size(), nullptr);
}

class Uses {
public:
    Uses(const Context& context, Recorder& recorder, const HostImport& import) : context(context), recorder(recorder), import(import) {}

    struct Use {
        std::unique_ptr<GuestBufferMemory> memory;
        VkDescriptorBufferInfo info{};
        std::uint32_t adjustment = 0;
    };

    Use Upload(std::uint64_t element, std::size_t bytes) const {
        Use use{std::make_unique<GuestBufferMemory>(context)};
        use.memory->AllowDeviceStaging();
        use.memory->AddWritable(element, bytes, false, false);
        use.memory->Upload(false);
        use.info = use.memory->Descriptor(element, bytes, use.adjustment);
        return use;
    }

    // The shader: `value` over [offset, offset + bytes) of the element.
    void Shade(const Use& use, std::uint64_t offset, std::uint64_t bytes, std::uint8_t value) const {
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        const std::uint32_t word = 0x01010101u * value;
        context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, use.info.buffer, use.info.offset + use.adjustment + offset, bytes, word);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    }

    // What MarkGpuWrites does with a use's buffers after its work.
    void Finish(const Use& use) const {
        use.memory->RecordCopyBacks(recorder);
        recorder.NotePendingWrites(use.memory->Writes());
        use.memory->MarkDirectWrites();
        use.memory->NoteStagedCopies();
    }

    void Settle() const {
        recorder.Submit();
        recorder.Sync();
    }

    bool InSlab(const Use& use) const { return use.info.buffer != import.buffer; }

private:
    const Context& context;
    Recorder& recorder;
    const HostImport& import;
};

int RunCases(const Context& context, Recorder& recorder, WatchedBlock& watched, const HostImport& import, bool shadowing) {
    const Uses uses(context, recorder, import);
    const auto address = watched.Address();
    auto* bytes = watched.Bytes();

    // ScrittoreLettore.
    const auto a = address + Unit;
    auto first = uses.Upload(a, RegionBytes);
    Require(uses.InSlab(first) == shadowing, shadowing ? "a written 3 MiB region was not bound to a range slab" : "a written 3 MiB region was not bound in place without APS5_BUFFER_SHADOW");
    if (shadowing) Require(ReportValue(ShadowReport(), "range slabs ") >= 1, "no range slab lives after the upload");
    uses.Shade(first, 0, RegionBytes, 0x5a);
    uses.Finish(first);
    uses.Settle();
    Require(Holds(bytes + Unit, RegionBytes, 0x5a), "the written bytes did not reach the import");
    first.memory.reset();

    // ScritturaParziale: the shader writes only the first half of a region it was given whole.
    const auto b = address + (4u << 20u);
    {
        auto half = uses.Upload(b, RegionBytes);
        uses.Shade(half, 0, RegionBytes / 2, 0x22);
        uses.Finish(half);
        uses.Settle();
        Require(Holds(bytes + (4u << 20u), RegionBytes / 2, 0x22), "the written half did not reach the import");
        Require(Holds(bytes + (4u << 20u) + RegionBytes / 2, RegionBytes / 2, 0x11), "the half the shader left alone lost the import's bytes");
    }

    // CpuScriveDopo: a CPU store on page 3 of region A between two uses; the second writes page 0.
    std::memset(bytes + Unit + 3 * Page, 0x44, Page);
    AgcDriver::GuestMemory::CollectWritesUncached(a, RegionBytes);
    {
        auto second = uses.Upload(a, RegionBytes);
        uses.Shade(second, 0, Page, 0x55);
        uses.Finish(second);
        uses.Settle();
        Require(Holds(bytes + Unit, Page, 0x55), "the second use's page did not reach the import");
        Require(Holds(bytes + Unit + 3 * Page, Page, 0x44), "the CPU's page was overwritten by the slab's older bytes");
        Require(Holds(bytes + Unit + Page, 2 * Page, 0x5a) && Holds(bytes + Unit + 4 * Page, RegionBytes - 4 * Page, 0x5a), "the first use's bytes did not stay in the other pages");
    }

    // SemiSoloMancanti and TimbroScrittore: nothing changed region A since its last use, so the
    // next use seeds nothing; its work stamps the region.
    {
        static_cast<void>(ShadowReport());
        const auto before = AgcDriver::GuestMemory::TrackerGeneration();
        auto third = uses.Upload(a, RegionBytes);
        const auto seeds = ReportValue(ShadowReport(), "seeds ");
        if (shadowing) Require(seeds == 0, "a use after an unchanged use seeded " + std::to_string(seeds) + " pieces");
        uses.Shade(third, 8 * Page, Page, 0x66);
        uses.Finish(third);
        Require(!AgcDriver::GuestMemory::UnchangedSince(a, RegionBytes, before), "the use did not stamp its written region");
        uses.Settle();
        Require(Holds(bytes + Unit + 8 * Page, Page, 0x66) && Holds(bytes + Unit + 3 * Page, Page, 0x44), "the third use's bytes are wrong");
    }

    // RiusoSlabCambiato: a reused upload binds the slab it took while nothing moved it; after a
    // range reaching over it grew the slab, the reuse is refused.
    if (shadowing) {
        auto reused = uses.Upload(a, RegionBytes);
        uses.Shade(reused, 0, Page, 0x77);
        uses.Finish(reused);
        uses.Settle();
        reused.memory->RecordStagingCopies(recorder);
        uses.Shade(reused, Page, Page, 0x78);
        uses.Finish(reused);
        uses.Settle();
        Require(Holds(bytes + Unit, Page, 0x77) && Holds(bytes + Unit + Page, Page, 0x78), "the reused upload's bytes are wrong");
        const auto grown = RangeShadowDestination(context, import, a - Unit, a + RegionBytes);
        Require(grown.has_value(), "the overlapping range got no destination");
        bool refused = false;
        try {
            reused.memory->RecordStagingCopies(recorder);
        } catch (const std::exception&) {
            refused = true;
        }
        Require(refused, "a reused upload whose range slab grew meanwhile was accepted");
        uses.Settle();
    }

    // RitiroImport: the registration goes after the uses (no use holds its lease), so the import
    // and its shadow retire: the range slabs give their bytes back to the budget and nothing is
    // lost. The CPU writes the memory while it is unregistered; a use after the range is
    // registered again seeds from the new import.
    {
        const auto c = address + (8u << 20u);
        auto* cBytes = bytes + (8u << 20u);
        static_cast<void>(ShadowReport());
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(watched.Bytes());
        }
        HostImportFor(context, address, Unit);
        const auto retired = ShadowReport();
        std::memset(cBytes, 0x99, RegionBytes);
        {
            GuestAllocations::Mutation mutation;
            mutation.Add(watched.Bytes(), 16u << 20u, true, true, true);
        }
        Require(ReportValue(retired, "lost on retire ") == 0, "the retire lost shadowed units: " + retired);
        Require(ReportValue(retired, "live ") == 0, "range slabs stayed live after their import retired: " + retired);
        Require(HostImportFor(context, address, 16u << 20u) != nullptr, "the block registered again was not imported");
        AgcDriver::GuestMemory::CollectWritesUncached(c, RegionBytes);
        auto last = uses.Upload(c, RegionBytes);
        uses.Shade(last, 0, Page, 0x3c);
        uses.Finish(last);
        uses.Settle();
        Require(Holds(cBytes, Page, 0x3c) && Holds(cBytes + Page, RegionBytes - Page, 0x99), "a use after the import came back did not seed from it");
    }
    if (shadowing && ShadowVerify()) {
        const auto verified = ShadowReport();
        Require(ReportValue(verified, "verified ") >= 1 && ReportValue(verified, "mismatches ") == 0, "the slab copy-backs were not verified clean: " + verified);
    }
    std::cout << "range slab buffers " << (shadowing ? "take every case" : "off: today's paths") << '\n';
    return 0;
}

int RunEvict(const Context& context, Recorder& recorder, WatchedBlock& watched, const HostImport& import) {
    const Uses uses(context, recorder, import);
    constexpr std::size_t TwelveMiB = std::size_t{12} << 20u;
    for (std::uint8_t index = 0; index < 3; ++index) {
        const auto offset = Unit + index * (TwelveMiB + Unit);
        auto use = uses.Upload(watched.Address() + offset, TwelveMiB);
        Require(uses.InSlab(use), "a 12 MiB writer under a 16 MiB budget was not bound to a range slab");
        uses.Shade(use, 0, TwelveMiB, static_cast<std::uint8_t>(0x81 + index));
        uses.Finish(use);
        uses.Settle();
        const auto live = ReportValue(ShadowReport(), "live ");
        Require(live <= 16.0, "the live slab bytes exceed the 16 MiB budget: " + std::to_string(live) + " MiB");
    }
    for (std::uint8_t index = 0; index < 3; ++index) {
        const auto offset = Unit + index * (TwelveMiB + Unit);
        Require(Holds(watched.Bytes() + offset, TwelveMiB, static_cast<std::uint8_t>(0x81 + index)), "an evicted writer's bytes did not stay in the import");
    }
    std::cout << "range slab buffers within the budget: evictions keep every writer's bytes\n";
    return 0;
}

int Run(const Context& deviceContext, bool evict) {
    const bool shadowing = BufferShadowEnabled();
    if (evict && !shadowing) {
        std::cout << "APS5_BUFFER_SHADOW is not set: range slab eviction not tested\n";
        return VulkanTestSkipped;
    }
    const std::size_t blockBytes = evict ? std::size_t{40} << 20u : std::size_t{16} << 20u;
    WatchedBlock watched(blockBytes);
    if (watched.Bytes() == nullptr) {
        std::cout << "write-watched shared guest memory unavailable: range slab buffers not tested\n";
        return VulkanTestSkipped;
    }
    std::memset(watched.Bytes(), 0x11, blockBytes);
    const auto address = watched.Address();
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(watched.Bytes(), blockBytes, true, true, true);
    }
    struct Unregister {
        void* block;
        ~Unregister() {
            GuestAllocations::Mutation mutation;
            mutation.Remove(block);
        }
    } unregister{watched.Bytes()};
    const auto* import = deviceContext.hostImportAlignment != 0 ? HostImportFor(deviceContext, address, blockBytes) : nullptr;
    if (import == nullptr) {
        std::cout << "host imports unavailable: range slab buffers not tested\n";
        return VulkanTestSkipped;
    }
    if (!AgcDriver::GuestMemory::Watched(address, blockBytes)) {
        std::cout << "the imported guest memory is no longer write-watched: range slab buffers not tested\n";
        return VulkanTestSkipped;
    }
    AgcDriver::GuestMemory::CollectWritesUncached(address, blockBytes);
    // The import's identity only: a copy of its registry ranges would hold the registration like a
    // lease, and RitiroImport's removal would wait for it.
    HostImport importKey{};
    importKey.base = import->base;
    importKey.bytes = import->bytes;
    importKey.buffer = import->buffer;
    importKey.lead = import->lead;
    auto context = deviceContext;
    DescriptorCache cache(context);
    context.descriptorCache = &cache;
    Recorder recorder(context);
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    recorder.Activate();
    return evict ? RunEvict(context, recorder, watched, importKey) : RunCases(context, recorder, watched, importKey, shadowing);
}

}

int main(int argc, char** argv) {
    try {
        const bool evict = argc == 2 && std::string_view(argv[1]) == "--evict";
        auto device = OpenVulkanTestDevice();
        if (device == nullptr) return VulkanTestSkipped;
        return Run(AgcDriver::VulkanDeviceTestAccess::ContextOf(*device), evict);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
