#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libSceAgcDriver/Graphics/include/SlimMutex.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>

namespace AgcDriver::Graphics {

namespace {

constexpr std::uint64_t UnitBytes = 65536;
constexpr std::size_t ReasonCount = static_cast<std::size_t>(PublishReason::Count);
constexpr const char* ReasonNames[ReasonCount] = {"hook", "region", "indirect", "copy-src", "copy-dst", "fill", "fill-clear", "label", "keys", "scanout", "validate", "upload", "tail", "evict", "retire", "teardown", "rehome", "border", "writer", "address"};

std::uint64_t BudgetBytes() {
    static const std::uint64_t budget = [] {
        const char* text = std::getenv("APS5_UNIT_SHADOW_MIB");
        return (text != nullptr ? std::strtoull(text, nullptr, 10) : 1024ull) << 20u;
    }();
    return budget;
}

std::uint32_t SlabUnits() {
    static const std::uint32_t units = [] {
        const char* text = std::getenv("APS5_UNIT_SHADOW_SLAB_MIB");
        const auto mib = text != nullptr ? std::strtoull(text, nullptr, 10) : 8ull;
        return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(mib, 1, 1024) * 16);
    }();
    return units;
}

// APS5_TRACE_SHADOW=<n>: the first n publishes, the first 64 seeds, every evict and retire.
std::atomic<std::int64_t> tracePublishesLeft{[] {
    const char* text = std::getenv("APS5_TRACE_SHADOW");
    return text != nullptr ? static_cast<std::int64_t>(std::strtoll(text, nullptr, 10)) : std::int64_t{0};
}()};
std::atomic<std::int64_t> traceSeedsLeft{64};
thread_local PublishSite publishSite = PublishSite::Other;
constexpr std::size_t SiteCount = static_cast<std::size_t>(PublishSite::Count);
constexpr const char* SiteNames[SiteCount] = {"other", "upload-copied", "upload-in-place", "upload-gpu-copy", "sampled-texture", "proof-pending", "proof-in-place", "proof-direct", "draw-bindings"};
std::atomic<std::uint64_t> bufferShadowSerial{1};

void bumpBufferShadowSerial() {
    bufferShadowSerial.fetch_add(1, std::memory_order_acq_rel);
}

bool TraceEnabled() {
    static const bool enabled = std::getenv("APS5_TRACE_SHADOW") != nullptr;
    return enabled;
}

struct Statistics {
    std::atomic<std::uint64_t> retiled{0}, retiledBytes{0}, refused{0}, refusedBytes{0}, seeds{0}, seedBytes{0};
    std::atomic<std::uint64_t> detiledShadow{0}, detiledShadowBytes{0}, detiledImport{0}, detiledImportBytes{0}, tailPublishes{0};
    std::array<std::atomic<std::uint64_t>, ReasonCount> published{}, publishedBytes{};
    std::atomic<std::uint64_t> staleDroppedCpu{0}, staleDroppedDriver{0}, skippedInCompletion{0}, evictions{0}, lostOnRetire{0}, droppedOnRetire{0}, slabLost{0}, verified{0}, mismatches{0};
    std::atomic<std::uint64_t> rangeMade{0}, rangeGrown{0}, rangeRefused{0}, rangeRehomed{0};
    std::atomic<std::uint64_t> strandedMoved{0}, strandedPublished{0};
    std::array<std::atomic<std::uint64_t>, SiteCount> sitePublished{}, sitePublishedBytes{};
};

Statistics& Stats() {
    static Statistics statistics;
    return statistics;
}

using GuestRanges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

// Adds [begin, end) to sorted, disjoint ranges, merging the ranges it touches.
void addRange(GuestRanges& ranges, std::uint64_t begin, std::uint64_t end) {
    if (end <= begin) return;
    auto it = std::lower_bound(ranges.begin(), ranges.end(), begin, [](const std::pair<std::uint64_t, std::uint64_t>& range, std::uint64_t at) { return range.second < at; });
    while (it != ranges.end() && it->first <= end) {
        begin = std::min(begin, it->first);
        end = std::max(end, it->second);
        it = ranges.erase(it);
    }
    ranges.insert(it, {begin, end});
}

// Takes [begin, end) out of sorted, disjoint ranges.
void removeRange(GuestRanges& ranges, std::uint64_t begin, std::uint64_t end) {
    if (end <= begin) return;
    GuestRanges kept;
    kept.reserve(ranges.size() + 1);
    for (const auto& [from, to] : ranges) {
        if (to <= begin || from >= end) {
            kept.emplace_back(from, to);
            continue;
        }
        if (from < begin) kept.emplace_back(from, begin);
        if (to > end) kept.emplace_back(end, to);
    }
    ranges = std::move(kept);
}

// The parts of sorted, disjoint ranges inside [begin, end).
GuestRanges clippedRanges(const GuestRanges& ranges, std::uint64_t begin, std::uint64_t end) {
    GuestRanges result;
    auto it = std::lower_bound(ranges.begin(), ranges.end(), begin, [](const std::pair<std::uint64_t, std::uint64_t>& range, std::uint64_t at) { return range.second <= at; });
    for (; it != ranges.end() && it->first < end; ++it) result.emplace_back(std::max(it->first, begin), std::min(it->second, end));
    return result;
}

// A range slab and the guest ranges written into it (sorted, disjoint, inside its units).
struct RangeSlab {
    std::shared_ptr<ShadowSlab> slab;
    GuestRanges written;
};

struct UnitShadow {
    Context context;
    // The import's range, buffer and lead (taken at creation: a publish under the import table's
    // mutex must not look them up), and the unit base (the import's base rounded down to a unit).
    std::uint64_t importBase = 0;
    std::uint64_t importBytes = 0;
    VkBuffer importBuffer = VK_NULL_HANDLE;
    std::uint64_t importLead = 0;
    std::uint64_t base = 0;
    // Per unit: 0 = never shadowed, else the tracker generation the slab bytes are current at;
    // whether a slab -> import copy was recorded at that generation.
    std::vector<std::uint64_t> generation;
    std::vector<std::uint8_t> published;
    // Per unit: the address writer serial (AddressWriterSerial) when its slab bytes became current.
    std::vector<std::uint64_t> authority;
    // Slab k covers units [k * SlabUnits(), (k + 1) * SlabUnits()), made on demand; a failed
    // allocation is not retried before the noted present.
    std::vector<std::shared_ptr<ShadowSlab>> slabs;
    std::vector<std::uint64_t> slabFailedUntil;
    std::uint32_t liveUnits = 0;
    // Per unit: 0 = owned by its grid slab (slabs[unit / SlabUnits()]), else 1 + the index in
    // `ranges` of the range slab that owns it. A retired range slab leaves a null entry.
    std::vector<std::uint32_t> owner;
    std::vector<RangeSlab> ranges;
    std::uint64_t rangeOwned = 0;

    std::uint64_t Units() const { return generation.size(); }
    std::uint64_t UnitBegin(std::uint64_t unit) const { return base + unit * UnitBytes; }
    std::uint64_t UnitOf(std::uint64_t address) const { return (address - base) / UnitBytes; }
    std::uint64_t ImportEnd() const { return importBase + importBytes; }
    // The unit's bytes inside the import.
    std::pair<std::uint64_t, std::uint64_t> UnitRange(std::uint64_t unit) const { return {std::max(UnitBegin(unit), importBase), std::min(UnitBegin(unit + 1), ImportEnd())}; }
    bool Live(std::uint64_t unit) const { return generation[unit] != 0 && published[unit] == 0; }
    // The slab holding the unit's bytes: its range slab's, else its grid slab's (null when none).
    const std::shared_ptr<ShadowSlab>& SlabOf(std::uint64_t unit) const { return owner[unit] == 0 ? slabs[static_cast<std::size_t>(unit / SlabUnits())] : ranges[owner[unit] - 1].slab; }
    RangeSlab* RangeOf(std::uint64_t unit) { return owner[unit] == 0 ? nullptr : &ranges[owner[unit] - 1]; }
    const RangeSlab* RangeOf(std::uint64_t unit) const { return owner[unit] == 0 ? nullptr : &ranges[owner[unit] - 1]; }
    void Recount() {
        std::uint32_t live = 0;
        for (std::uint64_t unit = 0; unit < Units(); ++unit) live += Live(unit) ? 1u : 0u;
        liveUnits = live;
    }
};

struct Shadows {
    SlimMutex mutex;
    std::map<std::uint64_t, std::shared_ptr<UnitShadow>> byBase;
    std::uint64_t liveBytes = 0;
    std::uint64_t peakBytes = 0;
    std::uint32_t liveSlabs = 0;
    // Counted for the [storage] segment: shadows retired and the slab bytes they returned.
    std::uint64_t retired = 0;
};

Shadows& Registry() {
    static Shadows shadows;
    return shadows;
}

VkDeviceSize SlabBytes(const ShadowSlab& slab) {
    return static_cast<VkDeviceSize>(slab.units) * UnitBytes;
}

template<typename TVisit>
void forEachSlab(const UnitShadow& shadow, TVisit&& visit) {
    for (const auto& slab : shadow.slabs) {
        if (slab != nullptr) visit(*slab);
    }
    for (const auto& range : shadow.ranges) {
        if (range.slab != nullptr) visit(*range.slab);
    }
}

// The shadows overlapping [address, end): imports never overlap, so at most a few.
template<typename TVisit>
bool anyOverlapping(const Shadows& registry, std::uint64_t address, std::uint64_t end, TVisit&& visit) {
    if (end <= address || registry.byBase.empty()) return false;
    auto it = registry.byBase.upper_bound(address);
    if (it != registry.byBase.begin()) --it;
    for (; it != registry.byBase.end() && it->first < end; ++it) {
        const auto& shadow = it->second;
        if (address < shadow->ImportEnd() && shadow->importBase < end && visit(shadow)) return true;
    }
    return false;
}

std::vector<std::shared_ptr<UnitShadow>> overlapping(const Shadows& registry, std::uint64_t address, std::uint64_t end) {
    std::vector<std::shared_ptr<UnitShadow>> result;
    anyOverlapping(registry, address, end, [&](const std::shared_ptr<UnitShadow>& shadow) {
        result.push_back(shadow);
        return false;
    });
    return result;
}

std::shared_ptr<UnitShadow> find(const Shadows& registry, const HostImport& import) {
    const auto found = registry.byBase.find(import.base);
    if (found == registry.byBase.end()) return nullptr;
    const auto& shadow = found->second;
    return shadow->importBuffer == import.buffer && shadow->importBytes == import.bytes ? shadow : nullptr;
}

std::shared_ptr<UnitShadow> findOrCreate(Shadows& registry, const Context& context, const HostImport& import) {
    if (auto shadow = find(registry, import); shadow != nullptr && shadow->context.device == context.device) return shadow;
    // A stale entry (an import replaced without a retire, a replaced device): dropped; its slabs
    // live on wherever a batch kept them.
    if (const auto found = registry.byBase.find(import.base); found != registry.byBase.end()) {
        forEachSlab(*found->second, [&](const ShadowSlab& slab) {
            registry.liveBytes -= SlabBytes(slab);
            --registry.liveSlabs;
        });
        registry.byBase.erase(found);
    }
    auto shadow = std::make_shared<UnitShadow>();
    shadow->context = context;
    shadow->importBase = import.base;
    shadow->importBytes = import.bytes;
    shadow->importBuffer = import.buffer;
    shadow->importLead = import.lead;
    shadow->base = import.base & ~(UnitBytes - 1);
    const auto units = (import.base + import.bytes - shadow->base + UnitBytes - 1) / UnitBytes;
    shadow->generation.assign(static_cast<std::size_t>(units), 0);
    shadow->published.assign(static_cast<std::size_t>(units), 0);
    shadow->authority.assign(static_cast<std::size_t>(units), 0);
    shadow->owner.assign(static_cast<std::size_t>(units), 0);
    const auto slabs = (units + SlabUnits() - 1) / SlabUnits();
    shadow->slabs.assign(static_cast<std::size_t>(slabs), nullptr);
    shadow->slabFailedUntil.assign(static_cast<std::size_t>(slabs), 0);
    registry.byBase.emplace(import.base, shadow);
    return shadow;
}

// One tracker query over units [first, last]: `changed[k]` (k = unit - first) whether a block
// stamp exceeds the unit's generation (a generation of 0 reads as changed), `cpu[k]` whether a
// collect made it (a CPU store).
void changedUnits(const UnitShadow& shadow, std::uint64_t first, std::uint64_t last, std::vector<std::uint8_t>& changed, std::vector<std::uint8_t>& cpu) {
    const auto count = static_cast<std::size_t>(last - first + 1);
    changed.assign(count, 1);
    cpu.assign(count, 1);
    const auto begin = std::max(shadow.UnitBegin(first), shadow.importBase);
    const auto end = std::min(shadow.UnitBegin(last + 1), shadow.ImportEnd());
    if (end <= begin) return;
    GuestMemory::ChangedBlocks(begin, static_cast<std::size_t>(end - begin), std::span(shadow.generation).subspan(static_cast<std::size_t>(first), count), changed, cpu);
}

// The bytes of [from, to) inside `unit` (owned by a range slab) that slab is authoritative for: its
// written ranges on the pages no store reached since the unit's generation; none for a unit never
// marked or dropped. `unchanged`: the caller's tracker query found no stamp over the unit newer
// than its generation, so every written byte holds (no page query). Under the registry mutex (the
// tracker's is taken after it).
GuestRanges authoritativeParts(const UnitShadow& shadow, std::uint64_t unit, std::uint64_t from, std::uint64_t to, bool unchanged = false) {
    GuestRanges result;
    const auto* range = shadow.RangeOf(unit);
    if (range == nullptr || shadow.generation[unit] == 0) return result;
    const auto [unitBegin, unitEnd] = shadow.UnitRange(unit);
    const auto written = clippedRanges(range->written, std::max(from, unitBegin), std::min(to, unitEnd));
    if (written.empty() || unchanged) return written;
    constexpr auto PageBytes = static_cast<std::uint64_t>(GuestMemory::StoredPageBytes);
    std::array<std::uint8_t, UnitBytes / PageBytes + 1> stored{};
    const auto firstPage = unitBegin / PageBytes;
    const auto pages = static_cast<std::size_t>((unitEnd - 1) / PageBytes - firstPage + 1);
    GuestMemory::StoredPagesSince(unitBegin, static_cast<std::size_t>(unitEnd - unitBegin), shadow.generation[unit], std::span(stored).first(pages));
    for (const auto& [begin, end] : written) {
        for (auto at = begin; at < end;) {
            const auto pageEnd = std::min(end, (at / PageBytes + 1) * PageBytes);
            if (stored[static_cast<std::size_t>(at / PageBytes - firstPage)] == 0) {
                if (!result.empty() && result.back().second == at) result.back().second = pageEnd;
                else result.emplace_back(at, pageEnd);
            }
            at = pageEnd;
        }
    }
    return result;
}

// Before units of a range slab are marked again (MarkShadowed moves their generation): the written
// bytes that lost their authority (a store reached their page since the unit's generation, or the
// unit was dropped) leave the written ranges, so the new generation does not make them current
// again. The caller collected the units first.
void settleRangeUnits(UnitShadow& shadow, std::uint64_t first, std::uint64_t last) {
    std::vector<std::uint8_t> changed, cpu;
    changedUnits(shadow, first, last, changed, cpu);
    for (auto unit = first; unit <= last; ++unit) {
        auto* range = shadow.RangeOf(unit);
        if (range == nullptr) continue;
        const auto [unitBegin, unitEnd] = shadow.UnitRange(unit);
        const auto kept = authoritativeParts(shadow, unit, unitBegin, unitEnd, changed[static_cast<std::size_t>(unit - first)] == GuestMemory::BlockUnchanged);
        removeRange(range->written, unitBegin, unitEnd);
        for (const auto& [begin, end] : kept) addRange(range->written, begin, end);
    }
}

std::shared_ptr<ShadowSlab> makeSlab(const Context& context, std::uint64_t firstUnit, std::uint32_t units) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = static_cast<VkDeviceSize>(units) * UnitBytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    // Lazy range slabs are mapped by BDA tables (LiveRangeSlabSpans), so a slab gets an address.
    const bool addressable = context.bufferDeviceAddress;
    if (addressable) info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer) != VK_SUCCESS) return nullptr;
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    if (addressable) allocation.pNext = &flags;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    try {
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory) != VK_SUCCESS) memory = VK_NULL_HANDLE;
        if (memory != VK_NULL_HANDLE && context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0) != VK_SUCCESS) {
            context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    } catch (const std::exception&) {
        memory = VK_NULL_HANDLE;
    }
    if (memory == VK_NULL_HANDLE) {
        context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        return nullptr;
    }
    auto slab = std::make_shared<ShadowSlab>(context, buffer, memory, firstUnit, units);
    if (addressable) {
        const VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, buffer};
        slab->deviceAddress = context.Function<PFN_vkGetBufferDeviceAddressKHR>("vkGetBufferDeviceAddressKHR")(context.device, &address);
    }
    return slab;
}

struct SlabCopies {
    std::shared_ptr<ShadowSlab> slab;
    std::vector<VkBufferCopy> regions;
};

// Appends the copy slab -> import of guest [begin, end), merged with the previous one when both
// continue each other in the same slab.
void appendCopy(const UnitShadow& shadow, std::vector<SlabCopies>& copies, GuestRanges& ranges, const std::shared_ptr<ShadowSlab>& slab, std::uint64_t begin, std::uint64_t end) {
    const VkBufferCopy region{(begin - shadow.base) - slab->firstUnit * UnitBytes, begin - shadow.importBase + shadow.importLead, end - begin};
    if (!copies.empty() && copies.back().slab == slab && copies.back().regions.back().dstOffset + copies.back().regions.back().size == region.dstOffset) {
        copies.back().regions.back().size += region.size;
        ranges.back().second = end;
        return;
    }
    if (copies.empty() || copies.back().slab != slab) copies.push_back({slab, {}});
    copies.back().regions.push_back(region);
    ranges.emplace_back(begin, end);
}

// Records the copies as one command group into the active recorder's open batch (a waited batch
// without one). `ranges` are the guest ranges copied, for the note and the hazard tracker.
void recordCopies(const UnitShadow& shadow, const std::vector<SlabCopies>& copies, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, std::uint64_t bytes) {
    const auto& context = shadow.context;
    using CommandClass = Recorder::CommandClass;
    auto* recorder = Recorder::Active();
    std::unique_ptr<CommandBatch> batch;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkAccessFlags covered = 0;
    auto timing = Recorder::NoTiming;
    if (recorder != nullptr) {
        commands = recorder->Commands(&covered);
        timing = recorder->BeginGpuTiming(CommandClass::ShadowPublish);
        if (Recorder::BarrierValidate()) recorder->NoteAccess(CommandClass::ShadowPublish, Recorder::Access{{}, ranges, {}, VK_PIPELINE_STAGE_TRANSFER_BIT});
    } else {
        batch = std::make_unique<CommandBatch>(context);
        commands = batch->Handle();
    }
    // The retile that filled the slab (its trailing barrier covers transfer reads and writes) and
    // every earlier import writer precede the copies; the CopyBuffer merge rule.
    constexpr VkAccessFlags transferAccess = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    if (recorder != nullptr && (covered & transferAccess) == transferAccess && Recorder::MergeBarriers()) {
        Recorder::CountMerged(CommandClass::ShadowPublish);
    } else {
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, transferAccess);
        Recorder::CountBarriers(CommandClass::ShadowPublish);
    }
    for (const auto& entry : copies) {
        for (const auto& region : entry.regions) CopyHostBuffer(context, commands, entry.slab->buffer, region.srcOffset, shadow.importBuffer, region.dstOffset, region.size);
    }
    // The published bytes are visible to everything recorded after and to the host, as after a fill.
    constexpr VkAccessFlags publishedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, publishedAccess);
    Recorder::CountBarriers(CommandClass::ShadowPublish);
    if (batch) {
        batch->SubmitAndWait();
        return;
    }
    recorder->EndGpuTiming(timing, bytes);
    recorder->MarkCovered(publishedAccess);
    for (const auto& entry : copies) recorder->Keep(entry.slab);
    // CPU readers reach the import's bytes only after the hook's SyncThrough waits by this note.
    recorder->NotePendingWrites(ranges);
}

// APS5_SHADOW_VERIFY=1: the published bytes read back from the slabs equal the guest bytes.
void verifyPublished(const UnitShadow& shadow, const std::vector<SlabCopies>& copies) {
    const auto& context = shadow.context;
    if (auto* recorder = Recorder::Active()) {
        Recorder::CountSync(4);
        recorder->Sync();
    }
    for (const auto& entry : copies) {
        for (const auto& region : entry.regions) {
            Buffer host(context, static_cast<std::size_t>(region.size), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            CommandBatch batch(context);
            CopyBuffer(context, batch.Handle(), entry.slab->buffer, region.srcOffset, host.Handle(), 0, region.size);
            RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            batch.SubmitAndWait();
            host.Invalidate();
            const auto address = shadow.importBase + region.dstOffset - shadow.importLead;
            Stats().verified.fetch_add(1, std::memory_order_relaxed);
            if (!GuestMemory::Accessible(reinterpret_cast<const void*>(address), static_cast<std::size_t>(region.size)) || std::memcmp(reinterpret_cast<const void*>(address), host.Bytes().data(), static_cast<std::size_t>(region.size)) != 0) {
                Stats().mismatches.fetch_add(1, std::memory_order_relaxed);
                static std::atomic<int> reports{0};
                if (reports.fetch_add(1) < 8) std::fprintf(stderr, "[shadow] verify: published bytes differ from the slab at 0x%llx+0x%llx\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(region.size));
                static const bool abortOnDifference = [] {
                    const char* text = std::getenv("APS5_VERIFY_BUFFER_SHADOW");
                    return text != nullptr && std::strcmp(text, "abort") == 0;
                }();
                if (abortOnDifference) {
                    std::fflush(stderr);
                    std::abort();
                }
            }
        }
    }
}

// The publish of one shadow: `selected[k]` (k = unit - first) names the units the scope chose.
std::size_t publishUnits(const std::shared_ptr<UnitShadow>& shadow, std::uint64_t first, std::uint64_t last, const std::vector<std::uint8_t>& selected, PublishReason reason) {
    auto& registry = Registry();
    std::vector<SlabCopies> copies;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::uint64_t bytes = 0;
    std::size_t units = 0;
    {
        // The stamps judged below see every CPU store into the live selected units up to now: a
        // store no collect walked yet would read as fresh and the copy would land over it. Every
        // caller holds the GpuMutex, so the live set is stable across the walk.
        std::uint64_t walkFirst = last + 1, walkLast = first;
        {
            std::lock_guard lock(registry.mutex);
            for (auto unit = first; unit <= last; ++unit) {
                if (!selected[static_cast<std::size_t>(unit - first)] || !shadow->Live(unit)) continue;
                walkFirst = std::min(walkFirst, unit);
                walkLast = std::max(walkLast, unit);
            }
        }
        if (walkFirst > walkLast) return 0;
        const auto walkBegin = std::max(shadow->UnitBegin(walkFirst), shadow->importBase);
        const auto walkEnd = std::min(shadow->UnitBegin(walkLast + 1), shadow->ImportEnd());
        if (walkEnd > walkBegin) GuestMemory::CollectWritesUncached(walkBegin, static_cast<std::size_t>(walkEnd - walkBegin));
    }
    {
        std::lock_guard lock(registry.mutex);
        std::vector<std::uint8_t> changed, cpu;
        changedUnits(*shadow, first, last, changed, cpu);
        for (auto unit = first; unit <= last; ++unit) {
            const auto k = static_cast<std::size_t>(unit - first);
            if (!selected[k] || !shadow->Live(unit)) continue;
            if (auto* range = shadow->RangeOf(unit)) {
                // A range slab publishes exactly its authoritative bytes; the written bytes of pages a
                // store reached since the unit's generation hold the import's bytes now and leave
                // the written ranges.
                const auto& slab = range->slab;
                const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
                const auto parts = slab != nullptr ? authoritativeParts(*shadow, unit, unitBegin, unitEnd, changed[k] == GuestMemory::BlockUnchanged) : GuestRanges{};
                removeRange(range->written, unitBegin, unitEnd);
                for (const auto& [begin, end] : parts) addRange(range->written, begin, end);
                if (parts.empty()) {
                    shadow->generation[unit] = 0;
                    if (changed[k] == GuestMemory::BlockWritten) (cpu[k] != 0 ? Stats().staleDroppedCpu : Stats().staleDroppedDriver).fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                for (const auto& [begin, end] : parts) {
                    appendCopy(*shadow, copies, ranges, slab, begin, end);
                    bytes += end - begin;
                }
                shadow->published[unit] = 1;
                slab->lastUse = Recorder::Presents();
                ++units;
                continue;
            }
            if (changed[k] == GuestMemory::BlockWritten) {
                // Newer bytes reached the import since the retile: the slab's are dead.
                shadow->generation[unit] = 0;
                (cpu[k] != 0 ? Stats().staleDroppedCpu : Stats().staleDroppedDriver).fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const auto slabIndex = static_cast<std::size_t>(unit / SlabUnits());
            const auto& slab = shadow->slabs[slabIndex];
            if (slab == nullptr) {
                shadow->generation[unit] = 0;
                continue;
            }
            const auto [begin, end] = shadow->UnitRange(unit);
            if (end <= begin) continue;
            appendCopy(*shadow, copies, ranges, slab, begin, end);
            shadow->published[unit] = 1;
            slab->lastUse = Recorder::Presents();
            bytes += end - begin;
            ++units;
        }
        shadow->Recount();
    }
    if (units == 0) return 0;
    Stats().published[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    Stats().publishedBytes[static_cast<std::size_t>(reason)].fetch_add(bytes, std::memory_order_relaxed);
    Stats().sitePublished[static_cast<std::size_t>(publishSite)].fetch_add(1, std::memory_order_relaxed);
    Stats().sitePublishedBytes[static_cast<std::size_t>(publishSite)].fetch_add(bytes, std::memory_order_relaxed);
    if (tracePublishesLeft.load(std::memory_order_relaxed) > 0 && tracePublishesLeft.fetch_sub(1, std::memory_order_relaxed) > 0) {
        const auto packet = GuestMemory::CurrentPacket();
        std::fprintf(stderr, "[shadow] publish %s (%s): 0x%llx+0x%llx, %zu units, %.2f MiB, %zu slabs (packet 0x%x queue 0x%x)\n", ReasonNames[static_cast<std::size_t>(reason)], SiteNames[static_cast<std::size_t>(publishSite)], static_cast<unsigned long long>(ranges.front().first), static_cast<unsigned long long>(ranges.back().second - ranges.front().first), units, bytes / 1048576.0, copies.size(), packet.opcode, packet.queue);
    }
    recordCopies(*shadow, copies, ranges, bytes);
    if (ShadowVerify()) verifyPublished(*shadow, copies);
    return units;
}

std::size_t publishRange(const std::shared_ptr<UnitShadow>& shadow, std::uint64_t address, std::uint64_t end, PublishScope scope, PublishReason reason) {
    const auto begin = std::max(address, shadow->importBase);
    const auto stop = std::min(end, shadow->ImportEnd());
    if (stop <= begin || shadow->liveUnits == 0) return 0;
    const auto first = shadow->UnitOf(begin);
    const auto last = shadow->UnitOf(stop - 1);
    std::vector<std::uint8_t> selected(static_cast<std::size_t>(last - first + 1), 0);
    bool any = false;
    for (auto unit = first; unit <= last; ++unit) {
        const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
        const bool whole = address <= unitBegin && end >= unitEnd;
        if (scope == PublishScope::PartialUnits && whole) continue;
        selected[static_cast<std::size_t>(unit - first)] = 1;
        any = true;
    }
    return any ? publishUnits(shadow, first, last, selected, reason) : 0;
}

// Frees the least recently used unpinned slab of any shadow after publishing its fresh units; false
// when nothing can be freed (a pinned slab is a destination of the write-back in progress: its
// copies are recorded into it after this decision).
bool evictOne(Shadows& registry) {
    std::shared_ptr<UnitShadow> victim;
    std::size_t victimSlab = 0;
    bool victimRange = false;
    std::uint64_t firstUnit = 0, lastUnit = 0;
    {
        std::lock_guard lock(registry.mutex);
        std::uint64_t oldest = ~0ull;
        const auto consider = [&](const std::shared_ptr<UnitShadow>& shadow, const std::shared_ptr<ShadowSlab>& slab, std::size_t index, bool range) {
            if (slab == nullptr || slab->pins.load(std::memory_order_relaxed) != 0 || slab->lastUse >= oldest) return;
            oldest = slab->lastUse;
            victim = shadow;
            victimSlab = index;
            victimRange = range;
        };
        for (const auto& [base, shadow] : registry.byBase) {
            for (std::size_t k = 0; k < shadow->slabs.size(); ++k) consider(shadow, shadow->slabs[k], k, false);
            for (std::size_t k = 0; k < shadow->ranges.size(); ++k) consider(shadow, shadow->ranges[k].slab, k, true);
        }
        if (victim == nullptr) return false;
        const auto& slab = victimRange ? victim->ranges[victimSlab].slab : victim->slabs[victimSlab];
        firstUnit = victimRange ? slab->firstUnit : static_cast<std::uint64_t>(victimSlab) * SlabUnits();
        lastUnit = std::min<std::uint64_t>(firstUnit + (victimRange ? slab->units : SlabUnits()), victim->Units()) - 1;
    }
    const auto published = publishRange(victim, victim->UnitBegin(firstUnit), victim->UnitBegin(lastUnit + 1), PublishScope::Whole, PublishReason::Evict);
    std::lock_guard lock(registry.mutex);
    auto& slab = victimRange ? victim->ranges[victimSlab].slab : victim->slabs[victimSlab];
    if (slab == nullptr) return true;
    if (TraceEnabled()) std::fprintf(stderr, "[shadow] evict %s slab %zu of import 0x%llx (%.1f MiB, %zu units published)\n", victimRange ? "range" : "grid", victimSlab, static_cast<unsigned long long>(victim->importBase), SlabBytes(*slab) / 1048576.0, published);
    // Its units read from the import from now on (the publish put their bytes there); a grid slab's
    // units a range slab owns are not its own.
    const auto own = victimRange ? static_cast<std::uint32_t>(victimSlab + 1) : 0u;
    for (auto unit = firstUnit; unit <= lastUnit; ++unit) {
        if (victim->owner[unit] != own) continue;
        victim->generation[unit] = 0;
        victim->published[unit] = 0;
        if (victimRange) {
            victim->owner[unit] = 0;
            --victim->rangeOwned;
        }
    }
    if (victimRange) {
        victim->ranges[victimSlab].written.clear();
        bumpBufferShadowSerial();
    }
    victim->Recount();
    registry.liveBytes -= SlabBytes(*slab);
    --registry.liveSlabs;
    slab.reset();
    Stats().evictions.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// A destination in the range slab owning units [first, last] at guest `begin`: the units are
// collected and settled first (written bytes that lost their authority leave the written ranges);
// the bytes of [seedBegin, seedEnd) outside the written ranges are the caller's seeds.
// `uncached`: the collect walks the span whatever the epoch did (a writer, whose copy-back lands over
// the import); a reader's collect follows the epoch, as an in-place read's would.
std::optional<ShadowDestination> rangeDestination(const std::shared_ptr<UnitShadow>& shadow, const HostImport& import, std::uint64_t first, std::uint64_t last, std::uint64_t begin, std::uint64_t seedBegin, std::uint64_t seedEnd, bool uncached = true) {
    const auto spanBegin = std::max(shadow->UnitBegin(first), shadow->importBase);
    const auto spanEnd = std::min(shadow->UnitBegin(last + 1), shadow->ImportEnd());
    if (spanEnd > spanBegin) {
        if (uncached) GuestMemory::CollectWritesUncached(spanBegin, static_cast<std::size_t>(spanEnd - spanBegin));
        else static_cast<void>(GuestMemory::CollectWrites(spanBegin, static_cast<std::size_t>(spanEnd - spanBegin)));
    }
    std::lock_guard lock(Registry().mutex);
    const auto owner = shadow->owner[first];
    if (owner == 0) return std::nullopt;
    for (auto unit = first; unit <= last; ++unit) {
        if (shadow->owner[unit] != owner) return std::nullopt;
    }
    const auto& range = shadow->ranges[owner - 1];
    if (range.slab == nullptr) return std::nullopt;
    settleRangeUnits(*shadow, first, last);
    range.slab->lastUse = Recorder::Presents();
    ShadowDestination destination{range.slab->buffer, SlabOffset(import, *range.slab, begin), range.slab, {}, std::make_shared<ShadowSlabPin>(range.slab)};
    if (seedEnd > seedBegin) {
        destination.seedUnits.emplace_back(seedBegin, seedEnd);
        for (const auto& [from, to] : clippedRanges(range.written, seedBegin, seedEnd)) removeRange(destination.seedUnits, from, to);
    }
    return destination;
}

// Records the device copies of absorbed range slabs into the slab that grows over them (every
// earlier writer of the old slabs first) into the active recorder's open batch (a waited batch
// without one); the recorder keeps the old slabs until the copies ran.
void recordSlabMoves(const Context& context, const std::shared_ptr<ShadowSlab>& into, const std::vector<std::shared_ptr<ShadowSlab>>& from) {
    if (from.empty()) return;
    auto* recorder = Recorder::Active();
    std::unique_ptr<CommandBatch> batch;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (recorder != nullptr) {
        commands = recorder->Commands();
    } else {
        batch = std::make_unique<CommandBatch>(context);
        commands = batch->Handle();
    }
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    for (const auto& slab : from) CopyBuffer(context, commands, slab->buffer, 0, into->buffer, (slab->firstUnit - into->firstUnit) * UnitBytes, SlabBytes(*slab));
    constexpr VkAccessFlags movedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, movedAccess);
    if (batch) {
        batch->SubmitAndWait();
        return;
    }
    recorder->MarkCovered(movedAccess);
    recorder->Keep(into);
    for (const auto& slab : from) recorder->Keep(slab);
}

// Bytes a write was recorded into a slab that no longer holds their units when the write is marked
// (a range slab grew over the slab or took its grid units in between: its moves were recorded
// before the write). `to` holds the units now; without one the bytes go to the import.
struct StrandedPiece {
    std::shared_ptr<ShadowSlab> from;
    std::shared_ptr<ShadowSlab> to;
    VkDeviceSize sourceOffset;
    VkDeviceSize destinationOffset;
    VkDeviceSize bytes;
    std::uint64_t address;
};

bool strandedRescue() {
    static const bool enabled = std::getenv("APS5_NO_STRANDED_RESCUE") == nullptr;
    return enabled;
}

// Records the stranded bytes' copies behind the write that made them, as the moves are recorded.
void recordStrandedCopies(const Context& context, VkBuffer importBuffer, const std::vector<StrandedPiece>& pieces) {
    if (pieces.empty()) return;
    auto* recorder = Recorder::Active();
    std::unique_ptr<CommandBatch> batch;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (recorder != nullptr) {
        commands = recorder->Commands();
    } else {
        batch = std::make_unique<CommandBatch>(context);
        commands = batch->Handle();
    }
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    for (const auto& piece : pieces) {
        if (piece.to != nullptr) CopyBuffer(context, commands, piece.from->buffer, piece.sourceOffset, piece.to->buffer, piece.destinationOffset, piece.bytes);
        else CopyHostBuffer(context, commands, piece.from->buffer, piece.sourceOffset, importBuffer, piece.destinationOffset, piece.bytes);
    }
    constexpr VkAccessFlags copiedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, copiedAccess);
    if (batch) {
        batch->SubmitAndWait();
        return;
    }
    recorder->MarkCovered(copiedAccess);
    for (const auto& piece : pieces) {
        recorder->Keep(piece.from);
        if (piece.to != nullptr) recorder->Keep(piece.to);
        else recorder->NotePendingWrite(piece.address, static_cast<std::size_t>(piece.bytes));
    }
}

}

bool UnitShadowEnabled() {
    static const bool enabled = std::getenv("APS5_NO_UNIT_SHADOW") == nullptr && GuestMemory::WriteWatched();
    return enabled;
}

bool ShadowVerify() {
    static const bool verify = std::getenv("APS5_SHADOW_VERIFY") != nullptr || std::getenv("APS5_VERIFY_BUFFER_SHADOW") != nullptr;
    return verify;
}

void NoteShadowVerification(bool matched, std::uint64_t address, std::uint64_t bytes) {
    Stats().verified.fetch_add(1, std::memory_order_relaxed);
    if (matched) return;
    Stats().mismatches.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<int> reports{0};
    if (reports.fetch_add(1) < 8) std::fprintf(stderr, "[bufshadow] verify: the seeded slab differs from the import at 0x%llx+0x%llx\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
    static const bool abortOnDifference = [] {
        const char* text = std::getenv("APS5_VERIFY_BUFFER_SHADOW");
        return text != nullptr && std::strcmp(text, "abort") == 0;
    }();
    if (abortOnDifference) {
        std::fflush(stderr);
        std::abort();
    }
}

bool BufferShadowEnabled() {
    static const bool enabled = std::getenv("APS5_BUFFER_SHADOW") != nullptr && UnitShadowEnabled();
    return enabled;
}

std::uint64_t BufferShadowSerial() {
    return bufferShadowSerial.load(std::memory_order_acquire);
}

bool RangeSlabsEnabled() {
    static const bool enabled = std::getenv("APS5_RANGE_SLABS") != nullptr || std::getenv("APS5_BUFFER_SHADOW") != nullptr;
    return enabled;
}

std::optional<ShadowDestination> RangeShadowDestination(const Context& context, const HostImport& import, std::uint64_t begin, std::uint64_t end) {
    if (!RangeSlabsEnabled() || !UnitShadowEnabled() || end <= begin || begin < import.base || end > import.base + import.bytes) return std::nullopt;
    if (!GuestMemory::Watched(begin, static_cast<std::size_t>(end - begin))) return std::nullopt;
    auto& registry = Registry();
    std::shared_ptr<UnitShadow> shadow;
    std::uint64_t first = 0, last = 0, spanFirst = 0, spanLast = 0;
    bool contained = false;
    // The range slabs the new one absorbs (owner values), pinned against eviction meanwhile, and
    // the grid units with results it takes over.
    std::vector<std::uint32_t> absorbed;
    std::vector<std::shared_ptr<ShadowSlabPin>> pins;
    GuestRanges rehome;
    {
        std::lock_guard lock(registry.mutex);
        shadow = findOrCreate(registry, context, import);
        first = shadow->UnitOf(begin);
        last = shadow->UnitOf(end - 1);
        const auto owner = shadow->owner[first];
        contained = owner != 0 && shadow->ranges[owner - 1].slab != nullptr;
        for (auto unit = first; unit <= last && contained; ++unit) contained = shadow->owner[unit] == owner;
        if (!contained) {
            spanFirst = first;
            spanLast = last;
            for (bool grew = true; grew;) {
                grew = false;
                for (auto unit = spanFirst; unit <= spanLast; ++unit) {
                    const auto own = shadow->owner[unit];
                    if (own == 0 || std::find(absorbed.begin(), absorbed.end(), own) != absorbed.end()) continue;
                    absorbed.push_back(own);
                    const auto& slab = *shadow->ranges[own - 1].slab;
                    if (slab.firstUnit < spanFirst) {
                        spanFirst = slab.firstUnit;
                        grew = true;
                    }
                    if (slab.firstUnit + slab.units - 1 > spanLast) {
                        spanLast = slab.firstUnit + slab.units - 1;
                        grew = true;
                    }
                }
            }
            for (const auto own : absorbed) pins.push_back(std::make_shared<ShadowSlabPin>(shadow->ranges[own - 1].slab));
            for (auto unit = spanFirst; unit <= spanLast; ++unit) {
                if (shadow->owner[unit] != 0 || shadow->generation[unit] == 0) continue;
                const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
                if (!rehome.empty() && rehome.back().second == unitBegin) rehome.back().second = unitEnd;
                else rehome.emplace_back(unitBegin, unitEnd);
            }
        }
    }
    if (!contained) {
        // A grid unit's results reach the import before the range slab takes the unit (its grid
        // slab bytes are dead from then on; the seeds read the import).
        for (const auto& [from, to] : rehome) {
            publishRange(shadow, from, to, PublishScope::Whole, PublishReason::Rehome);
            Stats().rangeRehomed.fetch_add((to - from + UnitBytes - 1) / UnitBytes, std::memory_order_relaxed);
        }
        const auto units = static_cast<std::uint32_t>(spanLast - spanFirst + 1);
        const auto needed = static_cast<std::uint64_t>(units) * UnitBytes;
        for (;;) {
            {
                std::lock_guard lock(registry.mutex);
                if (registry.liveBytes + needed <= BudgetBytes()) break;
            }
            if (!evictOne(registry)) {
                Stats().rangeRefused.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            }
        }
        auto slab = makeSlab(context, spanFirst, units);
        if (slab == nullptr) {
            Stats().rangeRefused.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        std::vector<std::shared_ptr<ShadowSlab>> moved;
        {
            std::lock_guard lock(registry.mutex);
            RangeSlab made{slab, {}};
            for (const auto own : absorbed) {
                auto& old = shadow->ranges[own - 1];
                for (const auto& [from, to] : old.written) addRange(made.written, from, to);
                moved.push_back(old.slab);
                registry.liveBytes -= SlabBytes(*old.slab);
                --registry.liveSlabs;
                old = RangeSlab{};
            }
            shadow->ranges.push_back(std::move(made));
            const auto index = static_cast<std::uint32_t>(shadow->ranges.size());
            for (auto unit = spanFirst; unit <= spanLast; ++unit) {
                if (shadow->owner[unit] == 0) {
                    shadow->generation[unit] = 0;
                    shadow->published[unit] = 0;
                    ++shadow->rangeOwned;
                }
                shadow->owner[unit] = index;
            }
            shadow->Recount();
            registry.liveBytes += SlabBytes(*slab);
            registry.peakBytes = std::max(registry.peakBytes, registry.liveBytes);
            ++registry.liveSlabs;
        }
        // The absorbed slabs' bytes move first: every unit they owned is the new slab's now.
        recordSlabMoves(context, slab, moved);
        (absorbed.empty() ? Stats().rangeMade : Stats().rangeGrown).fetch_add(1, std::memory_order_relaxed);
        bumpBufferShadowSerial();
        if (TraceEnabled()) std::fprintf(stderr, "[shadow] range slab of import 0x%llx: units %llu..%llu (%.1f MiB), %zu absorbed, %zu grid pieces rehomed\n", static_cast<unsigned long long>(import.base), static_cast<unsigned long long>(spanFirst), static_cast<unsigned long long>(spanLast), needed / 1048576.0, absorbed.size(), rehome.size());
    }
    return rangeDestination(shadow, import, first, last, begin, begin, end);
}

bool BufferShadowLazy() {
    static const bool enabled = BufferShadowEnabled() && std::getenv("APS5_BUFFER_SHADOW_LAZY") != nullptr;
    return enabled;
}

namespace {
std::atomic<std::uint64_t> lazySkippedBytes{0};
}

void NoteLazyCopyBackSkipped(std::uint64_t bytes) {
    lazySkippedBytes.fetch_add(bytes, std::memory_order_relaxed);
}

std::uint64_t LazyCopyBackSkippedBytes() {
    return lazySkippedBytes.load(std::memory_order_relaxed);
}

bool BufferShadowReaders() {
    static const bool enabled = BufferShadowEnabled() && std::getenv("APS5_NO_BUFFER_SHADOW_READERS") == nullptr;
    return enabled;
}

std::optional<ShadowDestination> RangeShadowSource(const Context& context, const HostImport& import, std::uint64_t begin, std::uint64_t end) {
    if (!BufferShadowReaders() || end <= begin || begin < import.base || end > import.base + import.bytes) return std::nullopt;
    std::shared_ptr<UnitShadow> shadow;
    std::uint64_t first = 0, last = 0;
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        shadow = find(registry, import);
        if (shadow == nullptr || shadow->context.device != context.device || shadow->rangeOwned == 0) return std::nullopt;
        first = shadow->UnitOf(begin);
        last = shadow->UnitOf(end - 1);
        const auto owner = shadow->owner[first];
        if (owner == 0 || shadow->ranges[owner - 1].slab == nullptr) return std::nullopt;
        for (auto unit = first + 1; unit <= last; ++unit) {
            if (shadow->owner[unit] != owner) return std::nullopt;
        }
    }
    auto source = rangeDestination(shadow, import, first, last, begin, begin, end, false);
    if (!source.has_value() || !source->seedUnits.empty()) return std::nullopt;
    return source;
}

std::size_t PublishShadowBorders(const HostImport& import, std::span<const ShadowedRange> ranges) {
    if (!UnitShadowEnabled() || ranges.empty()) return 0;
    auto& registry = Registry();
    std::shared_ptr<UnitShadow> shadow;
    std::vector<SlabCopies> copies;
    GuestRanges copied;
    std::uint64_t bytes = 0;
    {
        std::lock_guard lock(registry.mutex);
        shadow = find(registry, import);
        if (shadow == nullptr || shadow->rangeOwned == 0) return 0;
        constexpr auto PageBytes = static_cast<std::uint64_t>(GuestMemory::StoredPageBytes);
        GuestRanges pages;
        for (const auto& range : ranges) {
            if (range.end <= range.begin) continue;
            const auto firstPage = range.begin / PageBytes * PageBytes;
            const auto lastPage = (range.end - 1) / PageBytes * PageBytes;
            if (range.begin != firstPage) addRange(pages, firstPage, firstPage + PageBytes);
            if (range.end != lastPage + PageBytes) addRange(pages, lastPage, lastPage + PageBytes);
        }
        for (const auto& [pagesBegin, pagesEnd] : pages) {
            for (auto page = pagesBegin; page < pagesEnd; page += PageBytes) {
                const auto from = std::max(page, shadow->importBase);
                const auto to = std::min(page + PageBytes, shadow->ImportEnd());
                if (to <= from) continue;
                const auto unit = shadow->UnitOf(from);
                if (shadow->RangeOf(unit) == nullptr || !shadow->Live(unit)) continue;
                const auto& slab = shadow->SlabOf(unit);
                if (slab == nullptr) continue;
                for (const auto& [begin, end] : authoritativeParts(*shadow, unit, from, to)) {
                    appendCopy(*shadow, copies, copied, slab, begin, end);
                    bytes += end - begin;
                }
            }
        }
    }
    if (copies.empty()) return 0;
    Stats().published[static_cast<std::size_t>(PublishReason::Border)].fetch_add(1, std::memory_order_relaxed);
    Stats().publishedBytes[static_cast<std::size_t>(PublishReason::Border)].fetch_add(bytes, std::memory_order_relaxed);
    recordCopies(*shadow, copies, copied, bytes);
    if (ShadowVerify()) verifyPublished(*shadow, copies);
    return copied.size();
}

void NoteShadowSeed(std::uint64_t begin, std::uint64_t end) {
    Stats().seeds.fetch_add(1, std::memory_order_relaxed);
    Stats().seedBytes.fetch_add(end - begin, std::memory_order_relaxed);
    if (traceSeedsLeft.load(std::memory_order_relaxed) > 0 && traceSeedsLeft.fetch_sub(1, std::memory_order_relaxed) > 0) std::fprintf(stderr, "[shadow] seed 0x%llx+0x%llx from the import\n", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin));
}

PublishSiteScope::PublishSiteScope(PublishSite site) : previous(publishSite) {
    publishSite = site;
}

PublishSiteScope::~PublishSiteScope() {
    publishSite = previous;
}

PublishReason PublishReasonFor(const char* flushReason) {
    const auto is = [&](const char* name) { return flushReason != nullptr && std::strcmp(flushReason, name) == 0; };
    if (is("memory access")) return GuestMemory::CurrentReadSite() == GuestMemory::ReadSite::Scanout ? PublishReason::Scanout : PublishReason::Hook;
    if (is("imported buffer region") || is("copied buffer region") || is("sampled texture")) return PublishReason::Region;
    if (is("indirect dispatch arguments") || is("indirect draw arguments")) return PublishReason::Indirect;
    if (is("buffer copy source")) return PublishReason::CopySource;
    if (is("buffer copy destination")) return PublishReason::CopyDestination;
    if (is("buffer fill")) return PublishReason::Fill;
    if (is("packet store")) return PublishReason::Label;
    if (is("buffer copy count") || is("dispatch-cache entry")) return PublishReason::Validate;
    if (is("storage image creation") || is("storage refresh")) return PublishReason::Upload;
    if (is("address-based heaps")) return PublishReason::Address;
    return PublishReason::Hook;
}

ShadowSlab::ShadowSlab(const Context& context, VkBuffer buffer, VkDeviceMemory memory, std::uint64_t firstUnit, std::uint32_t units) : context(context), buffer(buffer), memory(memory), firstUnit(firstUnit), units(units) {}

ShadowSlab::~ShadowSlab() {
    if (buffer != VK_NULL_HANDLE) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

ShadowSlabPin::ShadowSlabPin(std::shared_ptr<ShadowSlab> slab) : slab(std::move(slab)) {
    this->slab->pins.fetch_add(1, std::memory_order_relaxed);
}

ShadowSlabPin::~ShadowSlabPin() {
    slab->pins.fetch_sub(1, std::memory_order_relaxed);
}

std::size_t PublishShadow(std::uint64_t address, std::size_t bytes, PublishScope scope, PublishReason reason, bool waited) {
    if (!UnitShadowEnabled() || scope == PublishScope::None || bytes == 0) return 0;
    if (!waited && Recorder::InCompletion() && GuestMemory::CurrentReadSite() == GuestMemory::ReadSite::Store) {
        // The store's own stamp makes the unit stale; a publish recorded now would land over the
        // store when the batch runs (the completion is not waited for).
        Stats().skippedInCompletion.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const auto end = bytes > std::numeric_limits<std::uint64_t>::max() - address ? std::numeric_limits<std::uint64_t>::max() : address + bytes;
    std::vector<std::shared_ptr<UnitShadow>> shadows;
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        shadows = overlapping(registry, address, end);
    }
    std::size_t units = 0;
    for (const auto& shadow : shadows) units += publishRange(shadow, address, end, scope, reason);
    return units;
}

namespace {

bool anyShadowedLocked(const Shadows& registry, std::uint64_t address, std::size_t bytes) {
    if (bytes == 0) return false;
    const auto end = bytes > std::numeric_limits<std::uint64_t>::max() - address ? std::numeric_limits<std::uint64_t>::max() : address + bytes;
    return anyOverlapping(registry, address, end, [&](const std::shared_ptr<UnitShadow>& shadow) {
        if (shadow->liveUnits == 0) return false;
        const auto begin = std::max(address, shadow->importBase);
        const auto stop = std::min(end, shadow->ImportEnd());
        for (auto unit = shadow->UnitOf(begin); unit <= shadow->UnitOf(stop - 1); ++unit) {
            if (shadow->Live(unit)) return true;
        }
        return false;
    });
}

}

bool AnyShadowedOverlaps(std::uint64_t address, std::size_t bytes) {
    if (!UnitShadowEnabled() || bytes == 0) return false;
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    return anyShadowedLocked(registry, address, bytes);
}

bool AnyShadowedOverlaps(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    if (!UnitShadowEnabled() || ranges.empty()) return false;
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    for (const auto& [begin, end] : ranges) {
        if (end > begin && anyShadowedLocked(registry, begin, static_cast<std::size_t>(end - begin))) return true;
    }
    return false;
}

std::uint64_t SlabBoundary(const HostImport& import, std::uint64_t address) {
    const auto base = import.base & ~(UnitBytes - 1);
    const auto unit = (address - base) / UnitBytes;
    const auto grid = base + (unit / SlabUnits() + 1) * SlabUnits() * UnitBytes;
    if (!UnitShadowEnabled()) return grid;
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    const auto shadow = find(registry, import);
    if (shadow == nullptr || shadow->rangeOwned == 0 || unit >= shadow->Units()) return grid;
    // A unit of a range slab: that slab's end; a grid unit: its grid slab's end or the first unit
    // after it that a range slab owns.
    if (const auto* range = shadow->RangeOf(unit)) return range->slab != nullptr ? shadow->UnitBegin(range->slab->firstUnit + range->slab->units) : shadow->UnitBegin(unit + 1);
    for (auto next = unit + 1; next < shadow->Units() && shadow->UnitBegin(next) < grid; ++next) {
        if (shadow->owner[next] != 0) return shadow->UnitBegin(next);
    }
    return grid;
}

VkDeviceSize SlabOffset(const HostImport& import, const ShadowSlab& slab, std::uint64_t address) {
    const auto base = import.base & ~(UnitBytes - 1);
    return (address - base) - slab.firstUnit * UnitBytes;
}

std::optional<ShadowDestination> ShadowDestinationFor(const Context& context, const HostImport& import, std::uint64_t begin, std::uint64_t end) {
    if (!UnitShadowEnabled() || end <= begin || begin < import.base || end > import.base + import.bytes) return std::nullopt;
    if (!GuestMemory::Watched(begin, static_cast<std::size_t>(end - begin))) return std::nullopt;
    auto& registry = Registry();
    std::shared_ptr<UnitShadow> shadow;
    std::size_t slabIndex = 0;
    std::uint64_t first = 0, last = 0;
    bool missing = false;
    std::uint32_t owner = 0;
    {
        std::lock_guard lock(registry.mutex);
        shadow = findOrCreate(registry, context, import);
        first = shadow->UnitOf(begin);
        last = shadow->UnitOf(end - 1);
        owner = shadow->owner[first];
        for (auto unit = first + 1; unit <= last; ++unit) {
            if (shadow->owner[unit] != owner) return std::nullopt;
        }
        if (owner == 0) {
            slabIndex = static_cast<std::size_t>(first / SlabUnits());
            if (last / SlabUnits() != slabIndex) return std::nullopt;
            missing = shadow->slabs[slabIndex] == nullptr;
            if (missing && shadow->slabFailedUntil[slabIndex] > Recorder::Presents()) return std::nullopt;
        }
    }
    // A write-back over units of a range slab writes into it; it claims exactly its piece, so it
    // seeds nothing.
    if (owner != 0) return rangeDestination(shadow, import, first, last, begin, begin, begin);
    if (missing) {
        const auto firstUnit = static_cast<std::uint64_t>(slabIndex) * SlabUnits();
        const auto units = static_cast<std::uint32_t>(std::min<std::uint64_t>(SlabUnits(), shadow->Units() - firstUnit));
        const auto needed = static_cast<std::uint64_t>(units) * UnitBytes;
        for (;;) {
            {
                std::lock_guard lock(registry.mutex);
                if (registry.liveBytes + needed <= BudgetBytes()) break;
            }
            if (!evictOne(registry)) {
                Stats().refused.fetch_add(1, std::memory_order_relaxed);
                Stats().refusedBytes.fetch_add(end - begin, std::memory_order_relaxed);
                return std::nullopt;
            }
        }
        auto slab = makeSlab(context, firstUnit, units);
        std::lock_guard lock(registry.mutex);
        if (slab == nullptr) {
            shadow->slabFailedUntil[slabIndex] = Recorder::Presents() + 1;
            Stats().refused.fetch_add(1, std::memory_order_relaxed);
            Stats().refusedBytes.fetch_add(end - begin, std::memory_order_relaxed);
            return std::nullopt;
        }
        if (shadow->slabs[slabIndex] == nullptr) {
            shadow->slabs[slabIndex] = slab;
            registry.liveBytes += SlabBytes(*slab);
            registry.peakBytes = std::max(registry.peakBytes, registry.liveBytes);
            ++registry.liveSlabs;
        }
    }
    // A unit the copy covers partly keeps its other bytes from the slab when fresh, else from a
    // seed of the import: the stamps must see the CPU's stores into it first (the write-back's own
    // walk covers the stored span only, not the unit's remainder outside the surface).
    for (auto unit = first; unit <= last; unit = last) {
        const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
        if (!(begin <= unitBegin && end >= unitEnd) && unitEnd > unitBegin) GuestMemory::CollectWritesUncached(unitBegin, static_cast<std::size_t>(unitEnd - unitBegin));
        if (unit == last) break;
    }
    std::lock_guard lock(registry.mutex);
    const auto& slab = shadow->slabs[slabIndex];
    if (slab == nullptr) return std::nullopt;
    slab->lastUse = Recorder::Presents();
    ShadowDestination destination{slab->buffer, SlabOffset(import, *slab, begin), slab, {}, std::make_shared<ShadowSlabPin>(slab)};
    std::vector<std::uint8_t> changed, cpu;
    bool queried = false;
    for (auto unit = first; unit <= last; ++unit) {
        const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
        if (begin <= unitBegin && end >= unitEnd) continue;
        if (!queried) {
            changedUnits(*shadow, first, last, changed, cpu);
            queried = true;
        }
        const auto k = static_cast<std::size_t>(unit - first);
        if (shadow->generation[unit] != 0 && changed[k] == 0) continue;
        destination.seedUnits.emplace_back(unitBegin, unitEnd);
    }
    return destination;
}

std::vector<ShadowRun> ShadowSources(const Context& context, const HostImport& import, std::uint64_t surfaceBase, std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::pair<std::uint64_t, std::uint64_t>> tailBlocks, bool countReads) {
    std::vector<ShadowRun> result;
    if (runs.empty()) return result;
    const auto importPiece = [&](std::uint64_t begin, std::uint64_t end) {
        result.push_back({begin, end, import.buffer, HostImportOffset(import, surfaceBase + begin), false, nullptr});
        if (countReads) Stats().detiledImportBytes.fetch_add(end - begin, std::memory_order_relaxed);
    };
    std::shared_ptr<UnitShadow> shadow;
    if (UnitShadowEnabled()) {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        shadow = find(registry, import);
        if (shadow != nullptr && (shadow->context.device != context.device || (shadow->liveUnits == 0 && std::none_of(shadow->generation.begin(), shadow->generation.end(), [](std::uint64_t generation) { return generation != 0; })))) shadow = nullptr;
    }
    if (shadow == nullptr) {
        for (const auto& [begin, end] : runs) importPiece(begin, end);
        if (countReads) Stats().detiledImport.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    const auto first = shadow->UnitOf(surfaceBase + runs.front().first);
    const auto last = shadow->UnitOf(surfaceBase + runs.back().second - 1);
    const auto count = static_cast<std::size_t>(last - first + 1);
    // Per unit of the span: whether it reads the slab, the slab, and for a unit of a range slab its
    // authoritative bytes (the rest of the unit reads the import).
    std::vector<std::uint8_t> fresh(count, 0);
    std::vector<std::uint8_t> ranged(count, 0);
    std::vector<std::shared_ptr<ShadowSlab>> unitSlabs(count);
    std::vector<GuestRanges> parts(count);
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        std::vector<std::uint8_t> changed, cpu;
        changedUnits(*shadow, first, last, changed, cpu);
        for (auto unit = first; unit <= last; ++unit) {
            const auto k = static_cast<std::size_t>(unit - first);
            const auto& slab = shadow->SlabOf(unit);
            unitSlabs[k] = slab;
            if (shadow->RangeOf(unit) != nullptr) {
                ranged[k] = 1;
                if (slab != nullptr) {
                    const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
                    parts[k] = authoritativeParts(*shadow, unit, unitBegin, unitEnd, changed[k] == GuestMemory::BlockUnchanged);
                }
                fresh[k] = parts[k].empty() ? 0 : 1;
                continue;
            }
            fresh[k] = shadow->generation[unit] != 0 && changed[k] == 0 && slab != nullptr ? 1 : 0;
        }
    }
    // A tail block is moved whole by one window: its units take one source.
    for (const auto& [tailBegin, tailEnd] : tailBlocks) {
        if (tailEnd <= tailBegin) continue;
        const auto tailFirst = shadow->UnitOf(surfaceBase + tailBegin);
        const auto tailLast = shadow->UnitOf(surfaceBase + tailEnd - 1);
        if (tailFirst < first || tailLast > last) continue;
        bool anyFresh = false, anyImport = false;
        for (auto unit = tailFirst; unit <= tailLast; ++unit) {
            const auto k = static_cast<std::size_t>(unit - first);
            if (ranged[k] == 0) {
                (fresh[k] != 0 ? anyFresh : anyImport) = true;
                continue;
            }
            const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
            const auto from = std::max(surfaceBase + tailBegin, unitBegin);
            const auto to = std::min(surfaceBase + tailEnd, unitEnd);
            std::uint64_t covered = 0;
            for (const auto& [begin, end] : clippedRanges(parts[k], from, to)) covered += end - begin;
            if (covered != 0) anyFresh = true;
            if (to > from && covered < to - from) anyImport = true;
        }
        if (!anyFresh || !anyImport) continue;
        PublishShadow(surfaceBase + tailBegin, static_cast<std::size_t>(tailEnd - tailBegin), PublishScope::Whole, PublishReason::TailMip);
        Stats().tailPublishes.fetch_add(1, std::memory_order_relaxed);
        for (auto unit = tailFirst; unit <= tailLast; ++unit) {
            fresh[static_cast<std::size_t>(unit - first)] = 0;
            parts[static_cast<std::size_t>(unit - first)].clear();
        }
    }
    std::uint64_t shadowBytes = 0, importBytes = 0;
    const auto present = Recorder::Presents();
    const auto emit = [&](std::uint64_t pieceBegin, std::uint64_t pieceEnd, const std::shared_ptr<ShadowSlab>& slab) {
        const bool isFresh = slab != nullptr;
        if (!result.empty() && result.back().end == pieceBegin && result.back().shadow == isFresh && result.back().slab == slab) {
            result.back().end = pieceEnd;
        } else if (isFresh) {
            slab->lastUse = present;
            result.push_back({pieceBegin, pieceEnd, slab->buffer, SlabOffset(import, *slab, surfaceBase + pieceBegin), true, slab});
        } else {
            result.push_back({pieceBegin, pieceEnd, import.buffer, HostImportOffset(import, surfaceBase + pieceBegin), false, nullptr});
        }
        (isFresh ? shadowBytes : importBytes) += pieceEnd - pieceBegin;
    };
    for (const auto& [runBegin, runEnd] : runs) {
        if (runEnd <= runBegin) continue;
        const auto guestBegin = surfaceBase + runBegin;
        const auto guestEnd = surfaceBase + runEnd;
        for (auto unit = shadow->UnitOf(guestBegin); unit <= shadow->UnitOf(guestEnd - 1); ++unit) {
            const auto pieceBegin = std::max(guestBegin, shadow->UnitBegin(unit)) - surfaceBase;
            const auto pieceEnd = std::min(guestEnd, shadow->UnitBegin(unit + 1)) - surfaceBase;
            if (pieceEnd <= pieceBegin) continue;
            const auto k = static_cast<std::size_t>(unit - first);
            if (ranged[k] == 0) {
                emit(pieceBegin, pieceEnd, fresh[k] != 0 ? unitSlabs[k] : nullptr);
                continue;
            }
            auto at = pieceBegin;
            for (const auto& [begin, end] : clippedRanges(parts[k], surfaceBase + pieceBegin, surfaceBase + pieceEnd)) {
                if (begin - surfaceBase > at) emit(at, begin - surfaceBase, nullptr);
                emit(begin - surfaceBase, end - surfaceBase, unitSlabs[k]);
                at = end - surfaceBase;
            }
            if (at < pieceEnd) emit(at, pieceEnd, nullptr);
        }
    }
    if (!countReads) shadowBytes = importBytes = 0;
    if (shadowBytes != 0) {
        Stats().detiledShadow.fetch_add(1, std::memory_order_relaxed);
        Stats().detiledShadowBytes.fetch_add(shadowBytes, std::memory_order_relaxed);
    }
    if (importBytes != 0) {
        Stats().detiledImport.fetch_add(1, std::memory_order_relaxed);
        Stats().detiledImportBytes.fetch_add(importBytes, std::memory_order_relaxed);
    }
    if (ShadowVerify()) {
        std::vector<std::uint8_t> changed, cpu;
        std::lock_guard lock(Registry().mutex);
        changedUnits(*shadow, first, last, changed, cpu);
        for (const auto& run : result) {
            if (!run.shadow) continue;
            for (auto unit = shadow->UnitOf(surfaceBase + run.begin); unit <= shadow->UnitOf(surfaceBase + run.end - 1); ++unit) {
                Stats().verified.fetch_add(1, std::memory_order_relaxed);
                const auto k = static_cast<std::size_t>(unit - first);
                if (ranged[k] == 0) {
                    if (changed[k] != 0) Stats().mismatches.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                // A range slab's piece: every byte of it still authoritative.
                const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
                const auto from = std::max(surfaceBase + run.begin, unitBegin);
                const auto to = std::min(surfaceBase + run.end, unitEnd);
                std::uint64_t covered = 0;
                for (const auto& [begin, end] : authoritativeParts(*shadow, unit, from, to)) covered += end - begin;
                if (to > from && covered != to - from) Stats().mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return result;
}

void MarkShadowed(const HostImport& import, std::span<const ShadowedRange> ranges, std::uint64_t generation, bool published) {
    if (!UnitShadowEnabled() || ranges.empty() || generation == 0) return;
    auto& registry = Registry();
    std::uint64_t bytes = 0;
    std::uint64_t lost = 0;
    bool grid = false, ranged = false;
    std::vector<StrandedPiece> stranded;
    std::shared_ptr<UnitShadow> strandedShadow;
    {
        std::lock_guard lock(registry.mutex);
        const auto shadow = find(registry, import);
        if (shadow == nullptr) {
            for (const auto& range : ranges) lost += range.end > range.begin ? (range.end - range.begin + UnitBytes - 1) / UnitBytes : 0;
            Stats().slabLost.fetch_add(lost, std::memory_order_relaxed);
            return;
        }
        const auto present = Recorder::Presents();
        for (const auto& [begin, end, slab] : ranges) {
            if (end <= begin) continue;
            bytes += end - begin;
            for (auto unit = shadow->UnitOf(begin); unit <= shadow->UnitOf(end - 1); ++unit) {
                const auto& current = shadow->SlabOf(unit);
                if (current == nullptr || current != slab) {
                    // The copies went into a slab the shadow no longer holds. A range slab holding
                    // the unit now, or the import when no slab does, gets them behind the write;
                    // else (APS5_NO_STRANDED_RESCUE, or a grid slab replaced) the import keeps
                    // whatever it had, as after a refused piece.
                    const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
                    const auto from = std::max(begin, unitBegin);
                    const auto to = std::min(end, unitEnd);
                    const bool toRange = current != nullptr && shadow->RangeOf(unit) != nullptr;
                    if (!strandedRescue() || from >= to || slab == nullptr || (current != nullptr && !toRange)) {
                        ++lost;
                        continue;
                    }
                    const auto source = (from - shadow->base) - slab->firstUnit * UnitBytes;
                    strandedShadow = shadow;
                    if (!toRange) {
                        // A published write's copy-back already put the bytes there.
                        if (!published) {
                            stranded.push_back({slab, nullptr, source, from - shadow->importBase + shadow->importLead, to - from, from});
                            Stats().strandedPublished.fetch_add(1, std::memory_order_relaxed);
                        }
                        continue;
                    }
                    stranded.push_back({slab, current, source, (from - shadow->base) - current->firstUnit * UnitBytes, to - from, from});
                    Stats().strandedMoved.fetch_add(1, std::memory_order_relaxed);
                }
                if (auto* range = shadow->RangeOf(unit)) {
                    const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
                    if (shadow->generation[unit] == 0) removeRange(range->written, unitBegin, unitEnd);
                    addRange(range->written, std::max(begin, unitBegin), std::min(end, unitEnd));
                    ranged = true;
                } else {
                    grid = true;
                }
                const bool clean = shadow->generation[unit] == 0 || shadow->published[unit] != 0;
                shadow->generation[unit] = generation;
                shadow->authority[unit] = AddressWriterSerial();
                shadow->published[unit] = published && clean && shadow->RangeOf(unit) != nullptr ? 1 : 0;
                current->lastUse = present;
            }
        }
        shadow->Recount();
    }
    if (strandedShadow != nullptr) recordStrandedCopies(strandedShadow->context, strandedShadow->importBuffer, stranded);
    if (lost != 0) {
        Stats().slabLost.fetch_add(lost, std::memory_order_relaxed);
        static std::atomic<int> reports{0};
        if (reports.fetch_add(1) < 4) std::fprintf(stderr, "[shadow] %llu retiled units of import 0x%llx lost their slab before they were marked\n", static_cast<unsigned long long>(lost), static_cast<unsigned long long>(import.base));
    }
    Stats().retiled.fetch_add(1, std::memory_order_relaxed);
    Stats().retiledBytes.fetch_add(bytes, std::memory_order_relaxed);
    // The resource cache's pending memos (fastRevalidate) must see the change of the surface's
    // source, as they see a write-back; a buffer writer's range units move their own serial.
    if (grid) StorageTexture::BumpPendingSerial();
    if (ranged) bumpBufferShadowSerial();
}

void RetireShadow(VkDevice device, const HostImport& import, const std::function<bool(std::uint64_t, std::uint64_t)>& registered) {
    if (!UnitShadowEnabled()) return;
    auto& registry = Registry();
    std::shared_ptr<UnitShadow> shadow;
    std::vector<std::uint8_t> selected;
    bool any = false;
    std::uint64_t dropped = 0;
    {
        std::lock_guard lock(registry.mutex);
        shadow = find(registry, import);
        if (shadow == nullptr) return;
        // The memory the title took back holds its bytes now: only a unit still inside a readable
        // registered range receives its results.
        selected.assign(static_cast<std::size_t>(shadow->Units()), 0);
        for (std::uint64_t unit = 0; unit < shadow->Units(); ++unit) {
            if (!shadow->Live(unit)) continue;
            const auto [begin, end] = shadow->UnitRange(unit);
            if (end > begin && registered(begin, end)) {
                selected[static_cast<std::size_t>(unit)] = 1;
                any = true;
            } else {
                shadow->generation[unit] = 0;
                ++dropped;
            }
        }
        shadow->Recount();
    }
    if (dropped != 0) Stats().droppedOnRetire.fetch_add(dropped, std::memory_order_relaxed);
    std::size_t published = 0;
    if (any) {
        if (GuestMemory::GpuMutex().HeldByThisThread() && shadow->context.device == device) {
            published = publishUnits(shadow, 0, shadow->Units() - 1, selected, PublishReason::Retire);
        } else {
            Stats().lostOnRetire.fetch_add(shadow->liveUnits, std::memory_order_relaxed);
            static std::atomic<int> reports{0};
            if (reports.fetch_add(1) < 4) std::fprintf(stderr, "[shadow] import 0x%llx+0x%llx retired outside the device lock: %u shadowed units lost\n", static_cast<unsigned long long>(import.base), static_cast<unsigned long long>(import.bytes), shadow->liveUnits);
        }
    }
    std::lock_guard lock(registry.mutex);
    const auto found = registry.byBase.find(import.base);
    if (found == registry.byBase.end() || found->second != shadow) return;
    forEachSlab(*shadow, [&](const ShadowSlab& slab) {
        registry.liveBytes -= SlabBytes(slab);
        --registry.liveSlabs;
    });
    if (!shadow->ranges.empty()) bumpBufferShadowSerial();
    ++registry.retired;
    if (TraceEnabled()) std::fprintf(stderr, "[shadow] retire import 0x%llx+0x%llx: %zu units published, %llu dropped (memory no longer registered)\n", static_cast<unsigned long long>(import.base), static_cast<unsigned long long>(import.bytes), published, static_cast<unsigned long long>(dropped));
    registry.byBase.erase(found);
}

void PublishAllShadows(const Context& context, PublishReason reason) {
    if (!UnitShadowEnabled()) return;
    std::vector<std::shared_ptr<UnitShadow>> shadows;
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        for (const auto& [base, shadow] : registry.byBase) {
            if (shadow->context.device == context.device) shadows.push_back(shadow);
        }
    }
    for (const auto& shadow : shadows) publishRange(shadow, shadow->importBase, shadow->ImportEnd(), PublishScope::Whole, reason);
}

std::size_t PublishRangeSlabs(const Context& context, PublishReason reason) {
    if (!UnitShadowEnabled()) return 0;
    std::vector<std::pair<std::shared_ptr<UnitShadow>, std::pair<std::uint64_t, std::uint64_t>>> spans;
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        for (const auto& [base, shadow] : registry.byBase) {
            if (shadow->context.device != context.device || shadow->rangeOwned == 0 || shadow->liveUnits == 0) continue;
            for (const auto& range : shadow->ranges) {
                if (range.slab != nullptr) spans.push_back({shadow, {shadow->UnitBegin(range.slab->firstUnit), shadow->UnitBegin(range.slab->firstUnit + range.slab->units)}});
            }
        }
    }
    std::size_t units = 0;
    for (const auto& [shadow, span] : spans) units += publishRange(shadow, span.first, span.second, PublishScope::Whole, reason);
    return units;
}

std::vector<LiveSlabSpan> LiveRangeSlabSpans(const Context& context, std::vector<std::pair<std::uint64_t, std::uint64_t>>* partial) {
    std::vector<LiveSlabSpan> spans;
    if (!UnitShadowEnabled()) return spans;
    auto& registry = Registry();
    // As a publish does (publishUnits): the CPU's stores over the live range slabs are collected
    // first, so a page it stored over since is not taken as the slab's (StoredPagesSince).
    std::vector<std::pair<std::uint64_t, std::uint64_t>> collect;
    {
        std::lock_guard lock(registry.mutex);
        for (const auto& [base, shadow] : registry.byBase) {
            if (shadow->context.device != context.device || shadow->rangeOwned == 0 || shadow->liveUnits == 0) continue;
            for (const auto& range : shadow->ranges) {
                if (range.slab == nullptr || range.slab->deviceAddress == 0) continue;
                const auto begin = std::max(shadow->UnitBegin(range.slab->firstUnit), shadow->importBase);
                const auto end = std::min(shadow->UnitBegin(range.slab->firstUnit + range.slab->units), shadow->ImportEnd());
                if (end > begin) collect.emplace_back(begin, end);
            }
        }
    }
    for (const auto& [begin, end] : collect) GuestMemory::CollectWritesUncached(begin, static_cast<std::size_t>(end - begin));
    std::lock_guard lock(registry.mutex);
    for (const auto& [base, shadow] : registry.byBase) {
        if (shadow->context.device != context.device || shadow->rangeOwned == 0 || shadow->liveUnits == 0) continue;
        for (const auto& range : shadow->ranges) {
            const auto& slab = range.slab;
            if (slab == nullptr || slab->deviceAddress == 0) continue;
            const auto slabBegin = shadow->UnitBegin(slab->firstUnit);
            for (auto unit = slab->firstUnit; unit < slab->firstUnit + slab->units && unit < shadow->Units(); ++unit) {
                if (!shadow->Live(unit) || shadow->SlabOf(unit) != slab) continue;
                const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
                const auto parts = authoritativeParts(*shadow, unit, unitBegin, unitEnd);
                if (parts.size() != 1 || parts.front().first != unitBegin || parts.front().second != unitEnd) {
                    if (partial != nullptr && !parts.empty()) partial->emplace_back(unitBegin, unitEnd);
                    continue;
                }
                if (VerifySlabAuthority()) NoteAuthorityCheck(shadow->authority[unit], unitBegin);
                const auto address = slab->deviceAddress + (unitBegin - slabBegin);
                if (!spans.empty() && spans.back().slab == slab && spans.back().end == unitBegin) spans.back().end = unitEnd;
                else spans.push_back({unitBegin, unitEnd, address, slab});
            }
        }
    }
    std::sort(spans.begin(), spans.end(), [](const LiveSlabSpan& a, const LiveSlabSpan& b) { return a.begin < b.begin; });
    return spans;
}

namespace {
std::atomic<std::uint64_t> addressOverlays{0}, addressOverlayBytes{0}, addressPublishes{0};
// Moves at every use of a build that stores through addresses: its stores reach the imports with no
// stamp, so a slab byte current before it may be older than its import after it.
std::atomic<std::uint64_t> addressWriterSerial{1};
std::atomic<std::uint64_t> authorityChecked{0}, authorityStale{0}, unitsForgotten{0};
}

std::uint64_t AddressWriterSerial() {
    return addressWriterSerial.load(std::memory_order_acquire);
}

bool VerifySlabAuthority() {
    static const bool enabled = std::getenv("APS5_VERIFY_SLAB_AUTHORITY") != nullptr;
    return enabled;
}

void NoteAuthorityCheck(std::uint64_t authority, std::uint64_t address) {
    authorityChecked.fetch_add(1, std::memory_order_relaxed);
    if (authority >= AddressWriterSerial()) return;
    if (authorityStale.fetch_add(1, std::memory_order_relaxed) < 8) std::fprintf(stderr, "[slab-authority] a live unit at 0x%llx is mapped with bytes from before a store through addresses\n", static_cast<unsigned long long>(address));
}

std::size_t PublishRangeSlabsForAddressWriter(const Context& context) {
    const auto units = PublishRangeSlabs(context, PublishReason::Address);
    addressWriterSerial.fetch_add(1, std::memory_order_acq_rel);
    static const bool keep = std::getenv("APS5_NO_ADDRESS_WRITER_FORGET") != nullptr;
    if (keep || !UnitShadowEnabled()) return units;
    // The published range slab units hand their authority to the imports for good: the stores that
    // follow are not stamped, so a later writer of part of a unit must seed the rest from the import
    // instead of taking the slab's bytes as current.
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    for (const auto& [base, shadow] : registry.byBase) {
        if (shadow->context.device != context.device || shadow->rangeOwned == 0) continue;
        for (std::uint64_t unit = 0; unit < shadow->Units(); ++unit) {
            auto* range = shadow->RangeOf(unit);
            if (range == nullptr || shadow->generation[unit] == 0 || shadow->published[unit] == 0) continue;
            const auto [unitBegin, unitEnd] = shadow->UnitRange(unit);
            removeRange(range->written, unitBegin, unitEnd);
            shadow->generation[unit] = 0;
            shadow->published[unit] = 0;
            unitsForgotten.fetch_add(1, std::memory_order_relaxed);
        }
        shadow->Recount();
    }
    return units;
}

std::string SlabAuthorityReport() {
    char text[160];
    std::snprintf(text, sizeof(text), "address writers %llu, units forgotten %llu, authority checked %llu, stale %llu", static_cast<unsigned long long>(AddressWriterSerial() - 1), static_cast<unsigned long long>(unitsForgotten.load()), static_cast<unsigned long long>(authorityChecked.load()), static_cast<unsigned long long>(authorityStale.load()));
    return text;
}

void NoteAddressOverlay(std::uint64_t bytes) {
    addressOverlays.fetch_add(1, std::memory_order_relaxed);
    addressOverlayBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteAddressPublish() {
    addressPublishes.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t AddressOverlays() {
    return addressOverlays.load(std::memory_order_relaxed);
}

std::uint64_t AddressOverlayBytes() {
    return addressOverlayBytes.load(std::memory_order_relaxed);
}

std::uint64_t AddressPublishes() {
    return addressPublishes.load(std::memory_order_relaxed);
}

void DestroyShadows(VkDevice device) {
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    for (auto it = registry.byBase.begin(); it != registry.byBase.end();) {
        if (it->second->context.device != device) {
            ++it;
            continue;
        }
        forEachSlab(*it->second, [&](const ShadowSlab& slab) {
            registry.liveBytes -= SlabBytes(slab);
            --registry.liveSlabs;
        });
        if (!it->second->ranges.empty()) bumpBufferShadowSerial();
        it = registry.byBase.erase(it);
    }
}

namespace {

struct BufferProbe {
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> live;
    BufferShadowProbeCounts totals;
    BufferShadowProbeCounts window;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

BufferProbe& bufferProbe() {
    static BufferProbe probe;
    return probe;
}

void addCounts(BufferShadowProbeCounts& into, const BufferShadowProbeCounts& delta) {
    into.writes += delta.writes;
    into.stagedBytes += delta.stagedBytes;
    into.inPlaceBytes += delta.inPlaceBytes;
    into.publishedPages += delta.publishedPages;
    into.publishedBytes += delta.publishedBytes;
    into.busyBoundaries += delta.busyBoundaries;
    for (std::size_t i = 0; i < into.boundaries.size(); ++i) into.boundaries[i] += delta.boundaries[i];
    for (std::size_t i = 0; i < into.reads.size(); ++i) {
        into.reads[i] += delta.reads[i];
        into.readBytes[i] += delta.readBytes[i];
    }
}

void reportBufferProbe(BufferProbe& probe) {
    const auto now = std::chrono::steady_clock::now();
    if (now - probe.lastReport < std::chrono::seconds(10)) return;
    probe.lastReport = now;
    const auto& w = probe.window;
    constexpr double MiB = 1048576.0;
    std::fprintf(stderr, "[bufshadow-probe] (10 s): %llu GPU buffer writes (staged %.1f MiB, in place %.1f MiB); boundaries label %llu submission %llu flip %llu (%llu with pages to publish); published at boundaries %llu pages %.1f MiB; readers meeting bytes written since the last boundary: hook %llu (%.1f MiB) draw snapshot %llu (%.1f MiB) in place %llu (%.1f MiB)%c",
        static_cast<unsigned long long>(w.writes), w.stagedBytes / MiB, w.inPlaceBytes / MiB,
        static_cast<unsigned long long>(w.boundaries[0]), static_cast<unsigned long long>(w.boundaries[1]), static_cast<unsigned long long>(w.boundaries[2]), static_cast<unsigned long long>(w.busyBoundaries),
        static_cast<unsigned long long>(w.publishedPages), w.publishedBytes / MiB,
        static_cast<unsigned long long>(w.reads[0]), w.readBytes[0] / MiB, static_cast<unsigned long long>(w.reads[1]), w.readBytes[1] / MiB, static_cast<unsigned long long>(w.reads[2]), w.readBytes[2] / MiB, 10);
    probe.window = {};
}

}

bool BufferShadowProbe() {
    static const bool enabled = std::getenv("APS5_BUFFER_SHADOW_PROBE") != nullptr;
    return enabled;
}

void ProbeGpuBufferWrite(std::uint64_t address, std::size_t bytes, bool staged) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) return;
    auto& probe = bufferProbe();
    std::lock_guard lock(probe.mutex);
    probe.live.emplace_back(address, address + bytes);
    BufferShadowProbeCounts delta;
    delta.writes = 1;
    (staged ? delta.stagedBytes : delta.inPlaceBytes) = bytes;
    addCounts(probe.totals, delta);
    addCounts(probe.window, delta);
}

void ProbeBufferBoundary(ProbeBoundary boundary) {
    auto& probe = bufferProbe();
    std::lock_guard lock(probe.mutex);
    BufferShadowProbeCounts delta;
    delta.boundaries[static_cast<std::size_t>(boundary)] = 1;
    if (!probe.live.empty()) {
        // The distinct pages and bytes of the writes since the previous boundary.
        auto& live = probe.live;
        std::sort(live.begin(), live.end());
        std::uint64_t lastPage = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t covered = 0;
        for (std::size_t i = 0; i < live.size();) {
            auto begin = live[i].first;
            auto end = live[i].second;
            for (++i; i < live.size() && live[i].first <= end; ++i) end = std::max(end, live[i].second);
            delta.publishedBytes += end - std::max(begin, covered);
            covered = end;
            auto first = begin / 4096;
            const auto last = (end - 1) / 4096;
            if (lastPage != std::numeric_limits<std::uint64_t>::max() && first <= lastPage) first = lastPage + 1;
            if (first <= last) delta.publishedPages += last - first + 1;
            lastPage = std::max(lastPage == std::numeric_limits<std::uint64_t>::max() ? 0 : lastPage, last);
        }
        delta.busyBoundaries = 1;
        live.clear();
    }
    addCounts(probe.totals, delta);
    addCounts(probe.window, delta);
    reportBufferProbe(probe);
}

void ProbeBufferRead(ProbeReader reader, std::uint64_t address, std::size_t bytes) {
    if (bytes == 0) return;
    auto& probe = bufferProbe();
    std::lock_guard lock(probe.mutex);
    if (probe.live.empty()) return;
    const auto end = address + bytes;
    std::uint64_t met = 0;
    for (const auto& [begin, stop] : probe.live) {
        if (begin < end && address < stop) met = std::max(met, std::min(end, stop) - std::max(address, begin));
    }
    if (met == 0) return;
    BufferShadowProbeCounts delta;
    delta.reads[static_cast<std::size_t>(reader)] = 1;
    delta.readBytes[static_cast<std::size_t>(reader)] = met;
    addCounts(probe.totals, delta);
    addCounts(probe.window, delta);
}

BufferShadowProbeCounts BufferShadowProbeTotals() {
    auto& probe = bufferProbe();
    std::lock_guard lock(probe.mutex);
    return probe.totals;
}

void ResetBufferShadowProbe() {
    auto& probe = bufferProbe();
    std::lock_guard lock(probe.mutex);
    probe.live.clear();
    probe.totals = {};
    probe.window = {};
}

std::string ShadowReport() {
    auto& statistics = Stats();
    const auto take = [](std::atomic<std::uint64_t>& counter) { return static_cast<unsigned long long>(counter.exchange(0, std::memory_order_relaxed)); };
    const auto mib = [](unsigned long long bytes) { return bytes / 1048576.0; };
    char text[1024];
    std::string line;
    if (!UnitShadowEnabled()) return "; shadow: off";
    std::snprintf(text, sizeof(text), "; shadow: retiled %llu/%.1f (refused %llu/%.1f to import), seeds %llu/%.1f, detiled from shadow %llu/%.1f, from import %llu/%.1f, tail publishes %llu, published", take(statistics.retiled), mib(take(statistics.retiledBytes)), take(statistics.refused), mib(take(statistics.refusedBytes)), take(statistics.seeds), mib(take(statistics.seedBytes)), take(statistics.detiledShadow), mib(take(statistics.detiledShadowBytes)), take(statistics.detiledImport), mib(take(statistics.detiledImportBytes)), take(statistics.tailPublishes));
    line += text;
    unsigned long long publishedTotal = 0, publishedBytesTotal = 0;
    std::string reasons, sites;
    for (std::size_t i = 0; i < ReasonCount; ++i) {
        const auto count = take(statistics.published[i]);
        const auto bytes = take(statistics.publishedBytes[i]);
        publishedTotal += count;
        publishedBytesTotal += bytes;
        if (count == 0) continue;
        std::snprintf(text, sizeof(text), " %s %llu/%.1f", ReasonNames[i], count, mib(bytes));
        reasons += text;
    }
    for (std::size_t i = 0; i < SiteCount; ++i) {
        const auto count = take(statistics.sitePublished[i]);
        const auto bytes = take(statistics.sitePublishedBytes[i]);
        if (count == 0) continue;
        std::snprintf(text, sizeof(text), " %s %llu/%.1f", SiteNames[i], count, mib(bytes));
        sites += text;
    }
    const auto cpuDropped = take(statistics.staleDroppedCpu), driverDropped = take(statistics.staleDroppedDriver);
    unsigned long long liveSlabs = 0, liveBytes = 0, peakBytes = 0, retired = 0, rangeSlabs = 0;
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        liveSlabs = registry.liveSlabs;
        liveBytes = registry.liveBytes;
        peakBytes = registry.peakBytes;
        retired = registry.retired;
        for (const auto& [base, shadow] : registry.byBase) {
            for (const auto& range : shadow->ranges) rangeSlabs += range.slab != nullptr ? 1u : 0u;
        }
    }
    std::snprintf(text, sizeof(text), " %llu/%.1f by reason {%s } by site {%s }, stale-dropped %llu (cpu %llu, driver %llu), skipped-in-completion %llu, slabs %llu live %.1f MiB / peak %.1f MiB, evictions %llu, slab lost %llu, retired %llu, dropped on retire %llu, lost on retire %llu", publishedTotal, mib(publishedBytesTotal), reasons.c_str(), sites.c_str(), cpuDropped + driverDropped, cpuDropped, driverDropped, take(statistics.skippedInCompletion), liveSlabs, mib(liveBytes), mib(peakBytes), take(statistics.evictions), take(statistics.slabLost), retired, take(statistics.droppedOnRetire), take(statistics.lostOnRetire));
    line += text;
    if (RangeSlabsEnabled()) {
        std::snprintf(text, sizeof(text), ", range slabs %llu live (made %llu, grown %llu, refused %llu, grid units rehomed %llu, stranded units moved %llu, published %llu)", rangeSlabs, take(statistics.rangeMade), take(statistics.rangeGrown), take(statistics.rangeRefused), take(statistics.rangeRehomed), take(statistics.strandedMoved), take(statistics.strandedPublished));
        line += text;
    }
    if (ShadowVerify()) {
        std::snprintf(text, sizeof(text), ", verified %llu, mismatches %llu", take(statistics.verified), take(statistics.mismatches));
        line += text;
    }
    return line;
}

}
