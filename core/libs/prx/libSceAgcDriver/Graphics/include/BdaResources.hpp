#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BDARESOURCES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BDARESOURCES_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "Recompiler.hpp"
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

bool LoopGuardTripped();

class BdaResources {
public:
    explicit BdaResources(const Context& context);
    // `liveSlabs` (a read-only build in the lazy mode of the range slabs): the table maps the bytes
    // range slabs hold newer than their imports to the slabs (OverlayLiveSpans), which it keeps.
    BdaResources(const Context& context, const GuestBufferMemory& memory, bool liveSlabs = false);
    // Whether the table maps live slab bytes (such a build is not reused: Revalidate).
    bool Overlaid() const { return !keptSlabs.empty(); }
    VkDescriptorBufferInfo Table() const;
    VkDescriptorBufferInfo Fault() const;
    void CheckFault() const;
    struct ImageTableFaults {
        std::uint32_t stage = 0;
        std::uint64_t shader = 0;
        std::vector<ShaderRecompiler::ImageTableEntryPoison> poison;
        std::vector<std::string> messages;
    };
    void SetImageTables(std::vector<ImageTableFaults> tables, std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges);
    struct SrtGuardFaults {
        std::uint32_t stage = 0;
        std::vector<std::uint32_t> flattenedSrt;
    };
    void SetSrtGuards(std::vector<SrtGuardFaults> guards);
    // APS5_PROFILE_DRAW: page tables served from the per-device cache and built anew (cumulative),
    // and how many recent tables the cache holds right now.
    struct TableCacheStats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::size_t held = 0;
        // Hits served by the cached address space's serial alone (no hash, no compare; see
        // GuestBufferMemory::CachedAddressTable), counted in `hits` too.
        std::uint64_t spaceTables = 0;
        // Why the most recently used table did not serve a build ([bda-table] first-differing entry
        // class): its owners were gone (expired weak reference), or its word hash differed, split by
        // whether the first differing range begins below 0x10000000 (a low mapping: the exe image,
        // the system heaps) or in the guest heap above; `sameHash` is a hash match whose ranges
        // still differed, `empty` a lookup with no entry held.
        std::uint64_t firstExpired = 0;
        std::uint64_t firstDiffersLow = 0;
        std::uint64_t firstDiffersHeap = 0;
        std::uint64_t firstSameHash = 0;
        std::uint64_t firstEmpty = 0;
    };
    static TableCacheStats TableCacheCounters();

private:
    // The page table is read-only to the shader, so consecutive builds mapping the same ranges to the
    // same device addresses share one buffer while one of them is alive (see the table cache in
    // BdaResources.cpp, which refers to it weakly).
    void markWrittenPages() const;
    void checkSrtGuard() const;
    std::shared_ptr<Buffer> table;
    std::unique_ptr<Buffer> fault;
    std::size_t tableBytes = 0;
    std::vector<ImageTableFaults> imageTables;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> imageTableRanges;
    std::vector<std::shared_ptr<ShadowSlab>> keptSlabs;
    std::vector<SrtGuardFaults> srtGuards;
};

// O17.T1: `ranges` (ascending, disjoint) with the parts that `live` covers pointed at the slabs
// (permissions kept), the rest unchanged; the slabs mapped go to `kept`, the bytes mapped to `mapped`.
std::vector<ShaderRecompiler::BdaAbi::Range> OverlayLiveSpans(const std::vector<ShaderRecompiler::BdaAbi::Range>& ranges, const std::vector<LiveSlabSpan>& live, std::vector<std::shared_ptr<ShadowSlab>>& kept, std::uint64_t& mapped);

}

#endif
