#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BdaResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "Recompiler.hpp"
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
#include <vector>

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
//   LettoreLegaSlab (O16.T14): a read-only use of a range the slab holds whole reads the slab and
//     sees the writers' bytes; one reaching past the slab binds the import;
//   SemeDelLettore: a CPU page stored after the writer reaches the reader (the slab no longer holds
//     the page, so the reader binds the import);
//   IstantaneaEvitata and SpecBindingsOmbra: a reader of the slab is no in-place read, so neither a
//     draw's bindings nor a speculation copy it from the CPU;
//   RiusoLettore: a reused reader whose slab grew meanwhile is refused;
//   RitiroImport: an import retired after the uses gives its slabs back.
// --evict (APS5_UNIT_SHADOW_MIB=16, Sfratto): three 12 MiB writers each land in the import and the
// live slab bytes stay within the budget. Without the switch (_off) the regions take today's paths.
// With APS5_BUFFER_SHADOW_LAZY (O16.T17, variant C) a writer's bytes stay live in the slab:
//   ScrittorePigro: the import keeps its older bytes until GuestMemory::Read publishes them;
//   LettoreRiusatoPubblica: a reused build reading the range in place publishes it in its proof;
//   SpeculazioneRifiutata: neither a draw's bindings nor a speculation copy a range with live bytes
//     (the bindings publish it and read it in place);
//   DescrittoreSulPosto: an address-based build's own region bound in place gets the live bytes
//     under it into the import before its descriptors read it; the rest stays live for its table;
//   ScrittoreAssorbito: a writer whose slab a later upload grows over before the writer's work is
//     marked keeps its bytes (they reach the slab that holds the units now);
//   ScrittoreRiusatoTiene: a reused build writing a range it binds to a range slab keeps the slab's
//     live bytes there (its proof publishes none of them);
//   RitiroPubblica: an import retired while its range stays registered publishes the live bytes;
//   --evict: an evicted slab publishes its live bytes.

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

    Use Read(std::uint64_t element, std::size_t bytes) const {
        Use use{std::make_unique<GuestBufferMemory>(context)};
        use.memory->AllowDeviceStaging();
        use.memory->AddReadable(element, bytes);
        use.memory->Upload(false);
        use.info = use.memory->Descriptor(element, bytes, use.adjustment);
        return use;
    }

    // What a reader shader sees: the bound bytes, copied out after the use.
    std::vector<std::byte> Readback(const Use& use, std::size_t bytes) const {
        auto host = std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy copy{use.info.offset + use.adjustment, 0, bytes};
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, use.info.buffer, host->Handle(), 1, &copy);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Keep(host);
        Finish(use);
        Settle();
        host->Invalidate();
        const auto seen = host->Bytes();
        return {seen.begin(), seen.end()};
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

    // LettoreLegaSlab and IstantaneaEvitata: region A is the slab's whole (the writers above).
    {
        auto reader = uses.Read(a, RegionBytes);
        Require(uses.InSlab(reader) == shadowing, shadowing ? "a read-only range the slab holds whole was not bound to the slab" : "a read-only range was not bound in place without APS5_BUFFER_SHADOW");
        Require(reader.memory->InPlaceReads().empty() == shadowing, shadowing ? "a reader of the slab still counts as an in-place read" : "a reader in place is missing from the in-place reads");
        const auto seen = uses.Readback(reader, RegionBytes);
        Require(std::memcmp(seen.data(), bytes + Unit, RegionBytes) == 0, "the reader did not read the writers' bytes");
        auto past = uses.Read(a + RegionBytes - Page, 2 * Page);
        Require(!uses.InSlab(past), "a read-only range reaching past the slab was not bound to the import");
        const auto pastSeen = uses.Readback(past, 2 * Page);
        Require(std::memcmp(pastSeen.data(), bytes + Unit + RegionBytes - Page, 2 * Page) == 0, "the reader past the slab did not read the import");
    }

    // SemeDelLettore: a CPU page on region B after its writer.
    {
        std::memset(bytes + (4u << 20u) + 2 * Page, 0x31, Page);
        AgcDriver::GuestMemory::CollectWritesUncached(b, RegionBytes);
        auto reader = uses.Read(b, RegionBytes);
        const auto seen = uses.Readback(reader, RegionBytes);
        Require(Holds(seen.data() + 2 * Page, Page, 0x31), "a CPU page stored after the writer did not reach the reader");
        Require(std::memcmp(seen.data(), bytes + (4u << 20u), RegionBytes) == 0, "the reader of region B does not see its bytes");
    }

    // IstantaneaEvitata and SpecBindingsOmbra through a build: a read-only 4 KiB element inside
    // region A (a speculation refuses more than APS5_DRAW_UPLOAD_MAX, 4096 bytes, as Large).
    {
        ShaderRecompiler::RecompileResult program;
        ShaderRecompiler::DescriptorBinding binding;
        binding.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
        binding.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
        binding.descriptorSet = 0;
        binding.binding = 0;
        binding.count = 1;
        const auto element = a + 16 * Page;
        binding.guestDescriptor = {static_cast<std::uint32_t>(element), static_cast<std::uint32_t>(element >> 32u) & 0xffffu, static_cast<std::uint32_t>(Page), 0x31000000u};
        binding.bufferWritten = {false};
        program.bindings.push_back(binding);
        const CompiledShader compute{ShaderRecompiler::ShaderStage::Compute, &program, 0};
        auto resources = std::make_shared<ShaderResources>(context, compute);
        const auto bindings = resources->PrepareDrawBindings(recorder);
        if (shadowing) Require(bindings == nullptr, "a draw's bindings copied a buffer the slab holds");
        else Require(bindings != nullptr && bindings->snapshots.size() == 1, "a draw's bindings did not copy the in-place buffer without APS5_BUFFER_SHADOW");
        const auto speculated = resources->SpeculateDrawBindings({}, resources->InPlaceReads(), nullptr);
        if (shadowing) Require(speculated.bindings == nullptr && speculated.ranges.empty() && speculated.refusal == ShaderResources::SpecRefusal::None, "a speculation copied a buffer the slab holds, or refused it");
        else Require(speculated.bindings != nullptr && speculated.ranges.size() == 1, "a speculation did not copy the in-place 4 KiB buffer without APS5_BUFFER_SHADOW");
        resources->MarkGpuWrites(recorder);
        uses.Settle();
    }

    // RiusoLettore.
    if (shadowing) {
        auto reader = uses.Read(a, RegionBytes);
        Require(uses.InSlab(reader), "the reader of region A is not bound to the slab");
        uses.Finish(reader);
        uses.Settle();
        reader.memory->RecordStagingCopies(recorder);
        uses.Finish(reader);
        uses.Settle();
        const auto grown = RangeShadowDestination(context, import, a, a + RegionBytes + Unit);
        Require(grown.has_value(), "the range reaching over region A got no destination");
        bool refused = false;
        try {
            reader.memory->RecordStagingCopies(recorder);
        } catch (const std::exception&) {
            refused = true;
        }
        Require(refused, "a reused reader whose range slab grew meanwhile was accepted");
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

// A compute build reading [element, element + bytes) through one element, or writing it.
std::shared_ptr<ShaderResources> BufferBuild(const Context& context, std::uint64_t element, std::size_t bytes, ShaderRecompiler::RecompileResult& program, bool written) {
    ShaderRecompiler::DescriptorBinding binding;
    binding.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    binding.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
    binding.descriptorSet = 0;
    binding.binding = 0;
    binding.count = 1;
    binding.guestDescriptor = {static_cast<std::uint32_t>(element), static_cast<std::uint32_t>(element >> 32u) & 0xffffu, static_cast<std::uint32_t>(bytes), 0x31000000u};
    binding.bufferWritten = {written};
    program.bindings.push_back(binding);
    const CompiledShader compute{ShaderRecompiler::ShaderStage::Compute, &program, 0};
    return std::make_shared<ShaderResources>(context, compute);
}

std::shared_ptr<ShaderResources> ReaderBuild(const Context& context, std::uint64_t element, std::size_t bytes, ShaderRecompiler::RecompileResult& program) {
    return BufferBuild(context, element, bytes, program, false);
}

int RunLazy(const Context& context, Recorder& recorder, WatchedBlock& watched, const HostImport& import) {
    const Uses uses(context, recorder, import);
    const auto address = watched.Address();
    auto* bytes = watched.Bytes();
    const auto a = address + Unit;
    const auto write = [&](std::uint64_t element, std::uint8_t value) {
        auto use = uses.Upload(element, RegionBytes);
        Require(uses.InSlab(use), "a written 3 MiB region was not bound to a range slab");
        uses.Shade(use, 0, RegionBytes, value);
        uses.Finish(use);
        uses.Settle();
    };

    // ScrittorePigro.
    write(a, 0x5a);
    Require(Holds(bytes + Unit, RegionBytes, 0x11), "a lazy writer copied its bytes back into the import");
    Require(AnyShadowedOverlaps(a, RegionBytes), "a lazy writer's bytes are not live in the slab");
    {
        std::vector<std::byte> read(RegionBytes);
        AgcDriver::GuestMemory::Read(a, read);
        Require(Holds(read.data(), RegionBytes, 0x5a), "GuestMemory::Read did not publish the live bytes");
        Require(Holds(bytes + Unit, RegionBytes, 0x5a) && !AnyShadowedOverlaps(a, RegionBytes), "the published bytes did not reach the import");
    }

    // LettoreRiusatoPubblica: a 4 KiB build across the end of A's slab, read in place, then a lazy
    // writer of A, then the build's reuse. The storage images' serial is moved off 0 first (a
    // title's always is; 0 never memoizes, so their scan would publish anyway) and the proof
    // before the writer memoizes it: the proof after the writer rests on the range slab serial.
    StorageTexture::BumpPendingSerial();
    const auto across = a + RegionBytes - Page / 2;
    ShaderRecompiler::RecompileResult program;
    auto reader = ReaderBuild(context, across, Page, program);
    Require(reader->InPlaceReads().size() == 1, "the build across the slab's end does not read in place");
    reader->MarkGpuWrites(recorder);
    uses.Settle();
    const CompiledShader compute{ShaderRecompiler::ShaderStage::Compute, &program, 0};
    Require(reader->Revalidate(std::span(&compute, 1), nullptr), "the reader build was not reusable");
    reader->MarkGpuWrites(recorder);
    uses.Settle();
    write(a, 0x6b);
    {
        Require(reader->Revalidate(std::span(&compute, 1), nullptr), "the reader build was not reusable after the writer");
        reader->MarkGpuWrites(recorder);
        uses.Settle();
        Require(Holds(bytes + Unit + RegionBytes - Page / 2, Page / 2, 0x6b), "a reused build reading in place did not publish the live bytes under it");
    }

    // SpeculazioneRifiutata.
    write(a, 0x7c);
    {
        const auto speculated = reader->SpeculateDrawBindings({}, reader->InPlaceReads(), nullptr);
        Require(speculated.bindings == nullptr && speculated.refusal == ShaderResources::SpecRefusal::PendingWriter, "a speculation copied a range with live slab bytes, or refused it for another reason");
        const auto bindings = reader->PrepareDrawBindings(recorder);
        Require(bindings == nullptr, "a draw's bindings copied a range with live slab bytes");
        reader->MarkGpuWrites(recorder);
        uses.Settle();
        Require(Holds(bytes + Unit + RegionBytes - Page / 2, Page / 2, 0x7c), "the draw's bindings did not publish the live bytes they read in place");
    }
    reader.reset();

    // ScrittoreRiusatoTiene (O20.T6): a compute build writing region E, used once and then proved for
    // its reuse; the units its use left live are the ones its reuse writes again in the same slab.
    {
        const auto e = address + (4u << 20u);
        ShaderRecompiler::RecompileResult writerProgram;
        auto writer = BufferBuild(context, e, RegionBytes, writerProgram, true);
        writer->MarkGpuWrites(recorder);
        uses.Settle();
        Require(AnyShadowedOverlaps(e, RegionBytes), "the writer build's use left no live bytes in a range slab");
        const CompiledShader writerCompute{ShaderRecompiler::ShaderStage::Compute, &writerProgram, 0};
        Require(writer->Revalidate(std::span(&writerCompute, 1), nullptr), "the writer build was not reusable");
        Require(AnyShadowedOverlaps(e, RegionBytes), "the reused writer build's proof published the live bytes of the slab it binds");
        writer->MarkGpuWrites(recorder);
        uses.Settle();
        writer.reset();
        PublishRangeSlabs(context, PublishReason::Address);
        uses.Settle();
    }

    // LettorePerIndirizzo: a build reading through BDA tables maps the imports, never the slabs, so
    // its use publishes every live range slab first (PublishRangeSlabs, the Address reason).
    write(a, 0x4d);
    Require(!Holds(bytes + Unit, RegionBytes, 0x4d) && AnyShadowedOverlaps(a, RegionBytes), "the lazy writer before the address-based read copied its bytes back");
    Require(PublishRangeSlabs(context, PublishReason::Address) > 0, "an address-based read published no live range slab unit");
    uses.Settle();
    Require(Holds(bytes + Unit, RegionBytes, 0x4d) && !AnyShadowedOverlaps(a, RegionBytes), "an address-based read did not publish the live bytes into the import");

    // LettorePerIndirizzoSlab (O17.T1): a read-only build's BDA table maps the live bytes to the slab
    // instead: the range over A points at the slab's address, the rest of the import's range stays.
    if (context.bufferDeviceAddress) {
        write(a, 0x5e);
        const auto live = LiveRangeSlabSpans(context);
        Require(live.size() == 1 && live.front().begin == a && live.front().end == a + RegionBytes && live.front().deviceAddress != 0, "the live slab bytes of the lazy writer are not one span over its region");
        const std::uint64_t importAddress = 0x100000000ull;
        const std::vector<ShaderRecompiler::BdaAbi::Range> table{{address, address + (16u << 20u), importAddress, ShaderRecompiler::BdaAbi::Read | ShaderRecompiler::BdaAbi::Write, 0}};
        std::vector<std::shared_ptr<ShadowSlab>> kept;
        std::uint64_t mapped = 0;
        const auto overlaid = OverlayLiveSpans(table, live, kept, mapped);
        Require(overlaid.size() == 3 && mapped == RegionBytes && kept.size() == 1 && kept.front() == live.front().slab, "the table over the live span is not split into import, slab and import parts");
        Require(overlaid[0].begin == address && overlaid[0].end == a && overlaid[0].deviceAddress == importAddress, "the part before the live span does not point at the import");
        Require(overlaid[1].begin == a && overlaid[1].end == a + RegionBytes && overlaid[1].deviceAddress == live.front().deviceAddress && overlaid[1].permissions == table.front().permissions, "the live span does not point at the slab with the memory's permissions");
        Require(overlaid[2].begin == a + RegionBytes && overlaid[2].deviceAddress == importAddress + (a + RegionBytes - address), "the part after the live span does not point at the import");
        PublishRangeSlabs(context, PublishReason::Address);
        uses.Settle();
        Require(LiveRangeSlabSpans(context).empty() && Holds(bytes + Unit, RegionBytes, 0x5e), "the published live span stayed live or did not reach the import");
        // A region off the units: the unit it ends inside is live only in part, so it is reported
        // for publishing and no mapped span covers it or splits a unit.
        write(a + Page, 0x6e);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> partial;
        const auto units = LiveRangeSlabSpans(context, &partial);
        const auto tail = a + RegionBytes;
        Require(std::find(partial.begin(), partial.end(), std::pair<std::uint64_t, std::uint64_t>{tail, tail + Unit}) != partial.end(), "the unit a region ends inside is not reported live in part");
        Require(!units.empty() && std::all_of(units.begin(), units.end(), [&](const LiveSlabSpan& span) { return span.begin % Unit == 0 && span.end % Unit == 0 && (span.end <= tail || span.begin >= tail + Unit); }), "a mapped span splits a unit or covers one the slab holds in part");
        PublishRangeSlabs(context, PublishReason::Address);
        uses.Settle();
        // DescrittoreSulPosto (O20.T1): an address-based upload maps the registered heaps and reads one
        // page of A through a descriptor, as a build whose table maps A's live bytes does; the page's
        // range merges into the heap, which the upload leaves unflushed.
        write(a, 0x7d);
        {
            GuestBufferMemory memory(context);
            memory.AcquireRegistered();
            memory.AddReadable(a + 4 * Page, Page);
            memory.Upload(true);
            Require(AnyShadowedOverlaps(a + 4 * Page, Page), "the address-based upload published the page itself: the merged range is not reproduced");
            static_cast<void>(memory.PublishLiveUnderOwnRanges());
            uses.Settle();
            Require(Holds(bytes + Unit + 4 * Page, Page, 0x7d), "the live bytes under an address-based build's own range did not reach the import");
            Require(AnyShadowedOverlaps(a + 2 * Unit, Unit), "the publish under the build's own range took the rest of A's live units with it");
        }
        PublishRangeSlabs(context, PublishReason::Address);
        uses.Settle();
    } else {
        std::cout << "LettorePerIndirizzoSlab skipped: the device has no buffer device address\n";
    }

    // ScrittorePerIndirizzo (O20.T1): a build that stores through addresses publishes every live range
    // slab, then its stores reach the import with no stamp (here a fill of A's import bytes). A later
    // lazy writer of one page of A must seed the rest from the import: the next publish keeps the
    // stores there instead of bringing the slab's bytes from before them back.
    write(a, 0x2a);
    Require(PublishRangeSlabsForAddressWriter(context) > 0, "a storing address-based build published no live range slab unit");
    uses.Settle();
    Require(Holds(bytes + Unit, RegionBytes, 0x2a), "a storing address-based build did not get the live bytes into the import");
    {
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, import.buffer, HostImportOffset(import, a), RegionBytes, 0x3b3b3b3bu);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT);
        uses.Settle();
    }
    Require(Holds(bytes + Unit, RegionBytes, 0x3b), "the unstamped GPU stores did not reach the import");
    {
        auto later = uses.Upload(a, RegionBytes);
        Require(uses.InSlab(later), "the later writer of A was not bound to the range slab");
        uses.Shade(later, 0, Page, 0x4c);
        uses.Finish(later);
        uses.Settle();
    }
    PublishRangeSlabs(context, PublishReason::Address);
    uses.Settle();
    Require(Holds(bytes + Unit, Page, 0x4c), "the later writer's page did not reach the import");
    Require(Holds(bytes + Unit + Page, RegionBytes - Page, 0x3b), "the slab's bytes from before the stores through addresses came back over them");

    // ScrittoreAssorbito (O20.T1): X takes a range slab over the first 2 MiB of region D; Y's upload of
    // [D + 1 MiB, D + 3 MiB) grows a slab over X's before X's work is recorded, so the move of X's slab
    // runs first and X's fill lands in the slab the units left. X writes the first MiB, Y the last.
    {
        const auto d = address + (12u << 20u);
        constexpr std::size_t Mib = std::size_t{1} << 20u;
        auto x = uses.Upload(d, 2 * Mib);
        auto y = uses.Upload(d + Mib, 2 * Mib);
        Require(uses.InSlab(x) && uses.InSlab(y) && x.info.buffer != y.info.buffer, "the upload over X's slab did not grow a new range slab");
        uses.Shade(x, 0, Mib, 0x1d);
        uses.Finish(x);
        uses.Shade(y, Mib, Mib, 0x2e);
        uses.Finish(y);
        uses.Settle();
        std::vector<std::byte> read(3 * Mib);
        AgcDriver::GuestMemory::Read(d, read);
        Require(Holds(read.data(), Mib, 0x1d), "the writer whose slab grew before its work was marked lost its bytes");
        Require(Holds(read.data() + Mib, Mib, 0x11) && Holds(read.data() + 2 * Mib, Mib, 0x2e), "the bytes around the absorbed writer's are wrong");
        Require(Holds(bytes + (12u << 20u), Mib, 0x1d) && !AnyShadowedOverlaps(d, 3 * Mib), "the absorbed writer's bytes did not reach the import");
    }

    // RitiroPubblica: region C written lazily, then its registration replaced in one mutation (the
    // range stays registered, the import retires).
    const auto c = address + (8u << 20u);
    write(c, 0x3c);
    Require(Holds(bytes + (8u << 20u), RegionBytes, 0x11), "the lazy writer of region C copied its bytes back");
    static_cast<void>(ShadowReport());
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(watched.Bytes());
        mutation.Add(watched.Bytes(), 16u << 20u, true, true, true);
    }
    Require(HostImportFor(context, address, 16u << 20u) != nullptr, "the block registered again was not imported");
    uses.Settle();
    const auto retired = ShadowReport();
    Require(ReportValue(retired, "lost on retire ") == 0, "the retire lost live units: " + retired);
    Require(Holds(bytes + (8u << 20u), RegionBytes, 0x3c), "an import retired with its range still registered did not publish the live bytes");
    std::cout << "lazy range slab buffers: the import gets the bytes when a consumer asks\n";
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
    // Lazy: the last writer's slab still holds its bytes; the two evicted ones published theirs.
    for (std::uint8_t index = 0; index < 3; ++index) {
        const auto offset = Unit + index * (TwelveMiB + Unit);
        if (BufferShadowLazy() && index == 2) {
            std::vector<std::byte> read(TwelveMiB);
            AgcDriver::GuestMemory::Read(watched.Address() + offset, read);
        }
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
    if (evict) return RunEvict(context, recorder, watched, importKey);
    return BufferShadowLazy() ? RunLazy(context, recorder, watched, importKey) : RunCases(context, recorder, watched, importKey, shadowing);
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
