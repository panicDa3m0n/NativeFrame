#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_UNITSHADOW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_UNITSHADOW_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

struct HostImport;

// Device-local tiled shadows of host imports, indexed by 64 KiB unit (one unit = one write-tracker
// block; unit index of guest address a inside a shadow = (a - base) / 65536 with base = the import's
// base rounded down to 64 KiB). A storage image's GPU-direct write-back retiles its pending units into
// the shadow instead of the import, and a later upload of any image over those units detiles from the
// shadow: the title's aliased images then exchange their results in device memory, and only a host-
// facing consumer of the bytes (the flush hook, a V# bound in place, indirect arguments, a copy, a
// label or key store over the range, scanout, the import's retirement) publishes them into the
// import with one vkCmdCopyBuffer per slab.
//
// Freshness is derived from the write tracker, never stored: a unit is shadowed ("fresh") while its
// slab bytes were written at a tracker generation no block stamp exceeds (MarkShadowed takes the
// generation the write-back settled at, after its own MarkWritten); every import writer already
// stamps its blocks (labels, fills, copies, key stores, in-place V# writes, CPU stores), so a stale
// unit reads from the import as today. A CPU store stamps only when a collect walks its page, so a
// publish and a seed decision collect the units they judge first (as the write-back's keep decision
// does): the CPU's bytes win the unit. A publish records the copy and marks the unit published (the
// slab and the import agree until the next retile or stamp); a stale unit met by a publish is dropped
// ("stale-dropped": its generation is cleared). A publish never stamps.
//
// Slabs are 8 MiB pieces of a shadow (APS5_UNIT_SHADOW_SLAB_MIB) with their own VkDeviceMemory,
// made on the first retile into them under a budget (APS5_UNIT_SHADOW_MIB, default 1024, LRU
// eviction with a publish of the evicted slab's fresh units). APS5_NO_UNIT_SHADOW=1 makes every
// primitive inert. Every call but AnyShadowedOverlaps runs under GuestMemory::GpuMutex (they record
// or mutate freshness); the registry's own mutex is a leaf (lock order GpuMutex -> Imports/Pending ->
// Shadows -> tracker).
bool UnitShadowEnabled();

// None: a device-facing caller (a storage upload reads the slab itself); Whole: a reader of every
// byte of the range; PartialUnits: a writer of the range (only units it covers partly are published,
// so the neighbours' results in the rest of the unit reach the import before the write stamps it).
enum class PublishScope : std::uint8_t { None, Whole, PartialUnits };
enum class PublishReason : std::uint8_t { Hook, Region, Indirect, CopySource, CopyDestination, Fill, FillClear, Label, Keys, Scanout, Validate, Upload, TailMip, Evict, Retire, Teardown, Rehome, Border, Writer, Address, Count };
// The reason for a FlushPending reason string ("memory access" -> Hook, ...).
PublishReason PublishReasonFor(const char* flushReason);
// The caller a publish comes from, counted beside its reason in the shadow report ("by site").
enum class PublishSite : std::uint8_t { Other, UploadCopied, UploadInPlace, UploadGpuCopy, SampledTexture, ProofPending, ProofInPlace, ProofDirect, DrawBindings, Count };
// Names the calling thread's publish site while it lives.
struct PublishSiteScope {
    explicit PublishSiteScope(PublishSite site);
    PublishSiteScope(const PublishSiteScope&) = delete;
    PublishSiteScope& operator=(const PublishSiteScope&) = delete;
    ~PublishSiteScope();
    PublishSite previous;
};

struct ShadowSlab {
    ShadowSlab(const Context& context, VkBuffer buffer, VkDeviceMemory memory, std::uint64_t firstUnit, std::uint32_t units);
    ShadowSlab(const ShadowSlab&) = delete;
    ShadowSlab& operator=(const ShadowSlab&) = delete;
    // Destroys the handles: safe on the recorder's release thread (Keep's contract).
    ~ShadowSlab();
    Context context;
    VkBuffer buffer;
    VkDeviceMemory memory;
    std::uint64_t firstUnit;
    std::uint32_t units;
    // Its device address when the device has buffer device addresses (BDA tables map it), else 0.
    VkDeviceAddress deviceAddress = 0;
    std::uint64_t lastUse = 0;
    // Destinations handed out for a write-back in progress: such a slab is never evicted.
    std::atomic<std::uint32_t> pins{0};
};

// Holds one pin of a slab (ShadowDestination::pin).
struct ShadowSlabPin {
    explicit ShadowSlabPin(std::shared_ptr<ShadowSlab> slab);
    ShadowSlabPin(const ShadowSlabPin&) = delete;
    ShadowSlabPin& operator=(const ShadowSlabPin&) = delete;
    ~ShadowSlabPin();
    std::shared_ptr<ShadowSlab> slab;
};

// A retile piece (guest range) and the slab its copies were recorded into.
struct ShadowedRange {
    std::uint64_t begin;
    std::uint64_t end;
    std::shared_ptr<ShadowSlab> slab;
};

// Records vkCmdCopyBuffer(slab -> import) for every fresh unpublished unit the scope selects over
// [address, address + bytes) into the active recorder's open batch (a waited CommandBatch without
// one), with the barriers, the pending-write note and the timing class of a fill; marks them
// published, clears stale ones. Returns the units copied. Inside a completion action a store's
// publish records nothing (the store's own stamp makes the unit stale; a recorded publish would land
// over the store later).
std::size_t PublishShadow(std::uint64_t address, std::size_t bytes, PublishScope scope, PublishReason reason, bool waited = false);
// No GpuMutex, no tracker: conservative (a unit with a generation and no publish counts), for
// hook-free readers and a build's stage A.
bool AnyShadowedOverlaps(std::uint64_t address, std::size_t bytes);
bool AnyShadowedOverlaps(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);

// The sources of a storage upload's surface-relative runs: [begin, end) pieces of them, each read
// from one buffer (a slab while the unit is fresh, else the import) at `offset` (byte x of the run
// is at offset + (x - begin)), split at unit freshness and slab boundaries with one tracker query
// over the span. A tail block (surface-relative, every unit of it) must have one source: a mixed
// one is published (TailMip) and read from the import. `countReads` false: not counted as detiled
// bytes (a write-back reading the current bytes of its padding).
struct ShadowRun {
    std::uint64_t begin;
    std::uint64_t end;
    VkBuffer buffer;
    VkDeviceSize offset;
    bool shadow;
    std::shared_ptr<ShadowSlab> slab;
};
std::vector<ShadowRun> ShadowSources(const Context& context, const HostImport& import, std::uint64_t surfaceBase, std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::pair<std::uint64_t, std::uint64_t>> tailBlocks, bool countReads = true);

// The slab destination of a retile copy [begin, end) (guest addresses inside one slab: the caller
// splits at SlabBoundary), making the slab under the budget; nullopt when refused (the caller writes
// the import as before). `seedUnits` names the units the copy covers partly that are not fresh (as
// [begin, end) clipped to the import, collected first): the caller records import -> slab for each
// before its copy. The slab stays pinned against eviction while a copy of the destination lives:
// the caller keeps one until MarkShadowed (an evicted slab would take the retile's copies with it).
struct ShadowDestination {
    VkBuffer buffer;
    VkDeviceSize offset;
    std::shared_ptr<ShadowSlab> slab;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> seedUnits;
    std::shared_ptr<ShadowSlabPin> pin;
};
std::optional<ShadowDestination> ShadowDestinationFor(const Context& context, const HostImport& import, std::uint64_t begin, std::uint64_t end);
// Range slabs (APS5_RANGE_SLABS, O16.T16, study scritture-gpu-vram.md section 3.2-3.3): a slab that
// covers exactly the units of one buffer range the GPU writes, so a range of 12, 16 or 32 MiB binds
// as one buffer. Every unit has one owner, its grid slab or one range slab (ShadowDestinationFor and
// SlabBoundary follow the owner). A range slab is authoritative only for the merged guest ranges
// written into it, and inside them only on the 4 KiB pages no store reached since their unit's
// generation (GuestMemory::StoredPagesSince: the CPU wins the page, not the unit); a publish copies
// exactly those bytes. RangeShadowDestination returns the destination of a writer of [begin, end):
// in the range slab that already owns its units, or in a new one; a range overlapping other range
// slabs makes one slab over all of them (their bytes copied device to device, then retired), and a
// grid unit with results is published before a range slab takes it. `seedUnits` names the bytes of
// [begin, end) outside the written ranges, which the caller seeds from the import before binding.
// nullopt when APS5_RANGE_SLABS is unset or the budget refuses (the caller stages as before).
bool RangeSlabsEnabled();
// O16.T13 P2 (APS5_BUFFER_SHADOW=1, which turns range slabs on too): a large written buffer region
// of a recorded use takes a range slab (GuestBufferMemory), and its written bytes are published
// right after its work (PublishReason::Writer) in place of today's copy-back.
bool BufferShadowEnabled();
// O16.T14 P3: the source of a GPU reader of [begin, end) (a read-only buffer region): the range
// slab that already owns every unit of it, when the slab is authoritative for every byte of the
// range (no seed needed: the reader then reads what the import holds); none is made or grown.
// nullopt otherwise, and with APS5_NO_BUFFER_SHADOW_READERS (the reader binds the import).
bool BufferShadowReaders();
// O16.T17 (APS5_BUFFER_SHADOW_LAZY=1 with APS5_BUFFER_SHADOW, variant C of the study): a range slab
// region's written bytes are not copied back after its work; they stay live in the slab and reach
// the import when a host-side or in-place consumer publishes them (the flush hook, a direct or
// copied region's flush, a reused build's proof, a label, a fill, a copy, indirect arguments,
// scanout, an eviction, a retire). A title's own CPU reads of them are not seen (see the debt).
bool BufferShadowLazy();
// A copy-back the lazy mode left out (counted on the [bufshadow] line).
void NoteLazyCopyBackSkipped(std::uint64_t bytes);
std::uint64_t LazyCopyBackSkippedBytes();
std::optional<ShadowDestination> RangeShadowSource(const Context& context, const HostImport& import, std::uint64_t begin, std::uint64_t end);
// Moves whenever range units are marked or a range slab is made, grown, evicted or retired (a
// buffer writer's MarkShadowed does not move StorageTexture::PendingSerial).
std::uint64_t BufferShadowSerial();
std::optional<ShadowDestination> RangeShadowDestination(const Context& context, const HostImport& import, std::uint64_t begin, std::uint64_t end);
// After a writer's MarkShadowed into range slabs: the first and last page of each range, when they
// hold bytes outside it, publish their written bytes at once, so a store into those neighbours does
// not take the GPU's bytes of the page with it (the units stay fresh). Returns the pieces copied.
std::size_t PublishShadowBorders(const HostImport& import, std::span<const ShadowedRange> ranges);
// A seed copy (import -> slab of a partly covered unit) the caller recorded: counted and traced.
void NoteShadowSeed(std::uint64_t begin, std::uint64_t end);
// The first slab boundary above `address` (a guest address inside the import).
std::uint64_t SlabBoundary(const HostImport& import, std::uint64_t address);
// The slab offset of a guest address for a slab returned by ShadowDestinationFor or ShadowSources.
VkDeviceSize SlabOffset(const HostImport& import, const ShadowSlab& slab, std::uint64_t address);
// After a retile's copies were recorded: the units of `ranges` (guest) hold the newest bytes at
// `generation`, unpublished. A unit whose slab is no longer the one the copies went into is not
// marked (counted `slab lost`). In a range slab the ranges join its written ranges. Bumps
// StorageTexture's pending serial when grid units were marked, BufferShadowSerial when range
// units were.
// `published`: the bytes reached the import too (a range slab region's copy-back); a range unit
// with no unpublished results of its own then stays published.
void MarkShadowed(const HostImport& import, std::span<const ShadowedRange> ranges, std::uint64_t generation, bool published = false);
// A range slab region's seeded slab compared with its import before the work (verification).
void NoteShadowVerification(bool matched, std::uint64_t address, std::uint64_t bytes);
// retireImport: publishes the fresh units whose memory `registered` still names as a readable
// registered range into the (old) import buffer, hands the slabs to the batch and drops the shadow.
// The import retires because its registration vanished or changed size, so the rest of its memory
// belongs to the title again and is dropped (counted `dropped on retire`); `lost on retire` counts
// the units nothing could record.
void RetireShadow(VkDevice device, const HostImport& import, const std::function<bool(std::uint64_t, std::uint64_t)>& registered);
// Teardown: every shadow of the context's device is published; DestroyShadows then drops them.
void PublishAllShadows(const Context& context, PublishReason reason);
// O16.T17 (APS5_BUFFER_SHADOW_LAZY): publishes every live range slab unit of the context's device.
// A build that reads guest memory through BDA tables maps the imports, never the slabs, so its use
// publishes them first (PublishReason::Address).
std::size_t PublishRangeSlabs(const Context& context, PublishReason reason);
// O17.T1 (APS5_BUFFER_SHADOW_LAZY): the whole units a range slab of the context's device holds
// newer than its import in every byte (written, not published, not stored over by the CPU since),
// ascending and disjoint, with the slab's device address for each; only slabs with a device
// address. A read-only build that reads through BDA tables maps them instead of the import
// (BdaResources). The shader looks a range up for every access and an access across two ranges
// fails, so a table never splits a unit: a live unit the slab holds only in part goes to `partial`
// (its range), and the build publishes it first.
struct LiveSlabSpan {
    std::uint64_t begin;
    std::uint64_t end;
    VkDeviceAddress deviceAddress;
    std::shared_ptr<ShadowSlab> slab;
};
std::vector<LiveSlabSpan> LiveRangeSlabSpans(const Context& context, std::vector<std::pair<std::uint64_t, std::uint64_t>>* partial = nullptr);
// A build that stores through addresses: every live range slab is published, then the published units
// hand their authority to the imports (APS5_NO_ADDRESS_WRITER_FORGET keeps it, for comparison).
std::size_t PublishRangeSlabsForAddressWriter(const Context& context);
std::uint64_t AddressWriterSerial();
// APS5_VERIFY_SLAB_AUTHORITY: counts the live units a BDA table maps whose bytes predate a store
// through addresses (SlabAuthorityReport).
bool VerifySlabAuthority();
void NoteAuthorityCheck(std::uint64_t authority, std::uint64_t address);
std::string SlabAuthorityReport();
// The [bufshadow] line: address-based builds whose table maps live slab bytes, and uses of builds
// that store through addresses (they publish every live range slab first).
void NoteAddressOverlay(std::uint64_t bytes);
void NoteAddressPublish();
std::uint64_t AddressOverlays();
std::uint64_t AddressOverlayBytes();
std::uint64_t AddressPublishes();
void DestroyShadows(VkDevice device);
// The [storage] line's shadow segment (10 s deltas, live slab totals).
std::string ShadowReport();
// O16.T10 P0 (APS5_BUFFER_SHADOW_PROBE=1, study scritture-gpu-vram.md): what publishing the buffer
// bytes the GPU writes at the boundaries a title can observe (a label written on the GPU, the end
// of a submission, a flip) would copy, and which readers would meet bytes written since the last
// boundary. Counts only (the [bufshadow-probe] line every 10 s); nothing changes. A boundary
// publishes the distinct 4 KiB pages of the writes since the previous one.
bool BufferShadowProbe();
enum class ProbeBoundary : std::uint8_t { Label, Submission, Flip, Count };
enum class ProbeReader : std::uint8_t { Hook, DrawSnapshot, InPlace, Count };
void ProbeGpuBufferWrite(std::uint64_t address, std::size_t bytes, bool staged);
void ProbeBufferBoundary(ProbeBoundary boundary);
void ProbeBufferRead(ProbeReader reader, std::uint64_t address, std::size_t bytes);
struct BufferShadowProbeCounts {
    std::uint64_t writes = 0;
    std::uint64_t stagedBytes = 0;
    std::uint64_t inPlaceBytes = 0;
    std::uint64_t publishedPages = 0;
    std::uint64_t publishedBytes = 0;
    std::uint64_t busyBoundaries = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(ProbeBoundary::Count)> boundaries{};
    std::array<std::uint64_t, static_cast<std::size_t>(ProbeReader::Count)> reads{};
    std::array<std::uint64_t, static_cast<std::size_t>(ProbeReader::Count)> readBytes{};
};
// Totals since the start or the last ResetBufferShadowProbe (tests).
BufferShadowProbeCounts BufferShadowProbeTotals();
void ResetBufferShadowProbe();

// O16.T15 (APS5_BUFFER_READ_PROBE=1, diagnostics, Windows only): the interior pages of every buffer
// range of at least APS5_BUFFER_READ_PROBE_MIN_KIB (1024) the GPU writes become PAGE_NOACCESS once
// noted (ProbeProtectWrittenRange, from MarkGpuWrites), page by page with the protection each had.
// A vectored handler (first in line) counts each access that faults by kind (read or write) and by
// module: the faulting instruction's, or its first caller within four frames in the title's image
// or this driver. An access of the title's image gives the whole range its protection back, so it
// counts once per protect; one of the driver or of another module gives back only its page, armed
// again at the next protect, so the driver's own reads do not hide the title's. A driver reader
// that knows it reads such ranges opens a ProbeHostRead scope: the range's pages are given back for
// its lifetime and armed again after it. Pages someone else made accessible again (the C library's
// write tracking, a title's mprotect) count as lost and are armed again at the next protect. The
// bytes stay where they are today, so only the faults change; a system call that reads or writes a
// protected range fails as on any protected memory. The [bufread-probe] line every 10 s gives the
// ranges with their notes, protects and accesses, and the callers of the driver's and other faults.
bool BufferReadProbe();
void ProbeProtectWrittenRange(std::uint64_t address, std::size_t bytes);
class ProbeHostRead {
public:
    ProbeHostRead(std::uint64_t address, std::size_t bytes);
    ~ProbeHostRead();
    ProbeHostRead(const ProbeHostRead&) = delete;
    ProbeHostRead& operator=(const ProbeHostRead&) = delete;

private:
    std::uint64_t scopeBegin = 0;
    std::size_t scopeBytes = 0;
};
enum class ProbeModule : std::uint8_t { Title, Driver, Other, Count };
struct BufferReadProbeCounts {
    std::uint64_t protects = 0;
    std::uint64_t refused = 0;
    std::uint64_t lost = 0;
    std::uint64_t scoped = 0;
    std::uint64_t pageFaults = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(ProbeModule::Count)> reads{};
    std::array<std::uint64_t, static_cast<std::size_t>(ProbeModule::Count)> writes{};
};
BufferReadProbeCounts BufferReadProbeTotals();
// Gives every protected range its protection back and clears the counts (tests).
void ResetBufferReadProbe();

// APS5_SHADOW_VERIFY=1: after a shadow-sourced upload, the units read must carry no stamp newer
// than their generation (counted as mismatches otherwise).
bool ShadowVerify();

}

#endif
