#include <string>
#include <optional>
#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAW_HPP

#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/Recipe.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include <chrono>
#include <memory>
#include <set>
#include <vector>

namespace AgcDriver::Graphics {

struct HostImport;

// `recipe`, when given, receives the DrawRecipe a recorded, cacheable, reusable, non-indirect draw
// built for its draw-cache entry (design_cpu_final M8; null otherwise, and always under
// APS5_NO_DRAW_RECIPE=1).
// O16.T8 probe (APS5_SPEC_PROBE=1, studies/committer-fuori-dal-lock.md P0): what a prepare thread
// would take off the GPU mutex for a draw, taken beside the prepared key with lock-free reads only
// (the template under the key, the draw's read-only guest buffer ranges clean in the published
// pending-write snapshot, the stamps that would prove them). The commit compares and counts (the
// [spec-probe] line); nothing else reads it, so no decision changes.
struct DrawSpeculationProbe {
    std::weak_ptr<ShaderResources> candidate;
    bool found = false;
    std::uint64_t tracker = 0;
    std::uint64_t writeGeneration = 0;
    std::uint64_t publishGeneration = 0;
    std::uint64_t pendingSerial = 0;
    std::uint64_t storageCacheSerial = 0;
    std::uint64_t registry = 0;
    std::uint64_t commits = 0;
    std::chrono::steady_clock::time_point at;
    std::vector<std::pair<std::uint64_t, std::size_t>> ranges;
    std::uint32_t pendingAtPrepare = 0;
    std::uint64_t probeNanoseconds = 0;
    // The queue-0 epoch token at the start of the prepare job and whether the job was sequenced
    // under the same token (set by the execution layer before the commit).
    std::uint64_t epoch = 0;
    bool epochSame = true;
};
// O16.T9 (APS5_SPEC_BINDINGS=1): a draw's bindings made by its prepare thread from the template its
// key found (ShaderResources::SpeculateDrawBindings), with what the commit checks before it takes
// them instead of making its own (recordDraw): the same template, moved buffers and in-place
// reads, the queue-0 epoch and the allocation registry unchanged, no pending GPU write over a
// copied range and no tracker stamp over one since `tracker` (read before the copies; every range
// was watched and unchanged after them, and no range was unwatched since `unwatch`). Without
// `ready` there is nothing to take. APS5_VERIFY_SPEC_BINDINGS=1 makes the commit's own bindings
// beside a taken speculation, records those, and aborts when the choices differ.
struct DrawSpeculation {
    std::shared_ptr<ShaderResources> resources;
    std::vector<ShaderResources::MovedBuffer> moved;
    std::shared_ptr<const ShaderResources::InPlaceRanges> inPlace;
    std::shared_ptr<ShaderResources::DrawBindings> bindings;
    std::vector<std::pair<std::uint64_t, std::size_t>> ranges;
    std::uint64_t tracker = 0;
    std::uint64_t unwatch = 0;
    std::uint64_t registry = 0;
    // The queue-0 epoch token at the start of the prepare job and whether the job was sequenced
    // under the same token (set by the execution layer before the commit).
    std::uint64_t epoch = 0;
    bool epochSame = true;
    bool ready = false;
};
// O22.T3 (APS5_SPEC_INPUTS=1): what a draw's prepare thread derives from its stages and state alone
// (the fragment outputs, the vertex input layout, the described set layouts and the pipeline for a
// recorded draw without proxied targets or stencil replay). The commit takes the outputs for the
// same state and stage objects on the same device, and the pipeline when its own vertex layout,
// set layout keys and attachment layout match too. APS5_VERIFY_SPEC_INPUTS=1 derives them again at
// the commit and reports any difference.
struct DrawInputSpeculation {
    VkDevice device = VK_NULL_HANDLE;
    const State* state = nullptr;
    const CompiledShader* shaders = nullptr;
    std::size_t shaderCount = 0;
    std::uint32_t fragmentOutputs = 0;
    VertexInputLayout vertexInput;
    std::vector<std::uint32_t> mainKey;
    std::vector<std::uint32_t> fragmentKey;
    std::shared_ptr<Pipeline> pipeline;
};
struct PreparedDrawKey {
    ResourceCache::Key key;
    std::size_t hash = 0;
    std::shared_ptr<DrawSpeculationProbe> probe;
    std::shared_ptr<DrawSpeculation> speculation;
    std::shared_ptr<const DrawInputSpeculation> inputs;
};
bool PreparedDrawKeys();
bool SpecProbe();
bool SpecBindings();
bool SpecInputs();
std::shared_ptr<const DrawInputSpeculation> SpeculateDrawInputs(const Context& context, const State& state, std::span<const CompiledShader> shaders);
const std::uint32_t* TakeSpeculatedOutputs(const DrawInputSpeculation* speculation, const Context& context, const State& state, std::span<const CompiledShader> shaders);
std::shared_ptr<Pipeline> TakeSpeculatedPipeline(const DrawInputSpeculation* speculation, const Context& context, const State& state, std::span<const CompiledShader> shaders, const VertexInputLayout& vertexInput, const PipelineSetLayouts& layouts, bool lean, bool proxiedTargets);
// The two halves of a DrawSpeculation outside a parallel-prepared draw (tests): what a prepare
// thread makes for a key whose template SharedResourceCache holds, and the commit's check, which
// takes the bindings (null for a draw without snapshots) or returns false for PrepareDrawBindings.
std::shared_ptr<DrawSpeculation> SpeculateDrawBindings(const PreparedDrawKey& prepared, std::span<const CompiledShader> shaders);
bool TakeSpeculativeBindings(const DrawSpeculation* speculation, const ShaderResources& resources, std::span<const ShaderResources::MovedBuffer> moved, Recorder& recorder, std::shared_ptr<ShaderResources::DrawBindings>& bindings);
PreparedDrawKey PrepareDrawKey(VkDevice device, std::span<const CompiledShader> shaders, std::uint32_t colorAttachments);
void Draw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots = {}, std::shared_ptr<const DrawRecipe>* recipe = nullptr, const PreparedDrawKey* prepared = nullptr);
std::optional<std::string> KnownValidationFailure(const Context& context, std::span<const CompiledShader> shaders, const State& state);
std::uint64_t DrawRenderPassKey(const Context& context, const State& state, std::span<const VkImageView> targetViews);

struct DrawInputCopy {
    std::shared_ptr<Buffer> buffer;
    bool reused = false;
    std::uint32_t derived = 0;
    std::uint64_t generation = 0;
    std::uint64_t registryGeneration = 0;
};
DrawInputCopy CopyDrawInput(const Context& context, Recorder* recorder, std::uint64_t address, std::size_t bytes, std::size_t alignment, Recorder::SnapshotUse use);
DrawInputCopy CopyZeroPaddedDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t validBytes, std::size_t stride = 0);
void KeepDrawInput(Recorder* recorder, std::uint64_t address, const DrawInputCopy& copy, Recorder::SnapshotUse use, std::uint32_t derived);
const HostImport* InPlaceDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t alignment, bool* behindGpuWrite = nullptr);
struct InPlaceIndices {
    std::optional<std::uint32_t> highest;
    bool reused = false;
};
InPlaceIndices InPlaceIndexRange(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint32_t indexSize, bool skipRestart);

std::array<std::uint32_t, 4> MeshIndexBufferDescriptor(const Pm4::DrawParameters& draw);

struct MeshArguments {
    std::uint32_t groups;
    std::uint32_t instances;
    std::uint32_t layers;
    std::uint32_t indexCount;
    std::uint32_t firstIndex;
};
static_assert(sizeof(MeshArguments) == ShaderRecompiler::MeshArgumentBytes);
struct MeshArgumentRules {
    std::uint32_t indexCount;
    std::uint32_t inputSize;
    std::uint32_t step;
    std::uint32_t primitivesPerGroup;
    std::uint32_t maxGroups;
    std::uint32_t maxInstances;
    std::uint32_t maxTotal;
};
MeshArgumentRules MeshArgumentRulesFor(const Context& context, const ShaderRecompiler::MeshConfiguration& mesh, std::uint32_t indexCount);
MeshArguments ResolveMeshArguments(const Pm4::DrawArguments& record, const MeshArgumentRules& rules);

// Why DrawWithRecipe did not record from the recipe (the caller then runs Draw): the draw is not
// recordable (no recorder, APS5_SYNC_DRAWS, APS5_DUMP_TARGETS), a resident target is gone from the
// storage cache, the template is gone, the pipeline is gone from the pipeline store, or the
// template's proof failed (ShaderResources::ProveCurrent; the template is removed from the
// resource cache and kept by the batch).
enum class DrawRecipeMiss : std::uint8_t { None, NotRecordable, TargetGone, TemplateGone, ObjectsGone, Proof, Count };
const char* DrawRecipeMissName(DrawRecipeMiss miss);
struct DrawRecipeOutcome {
    bool recorded = false;
    DrawRecipeMiss miss = DrawRecipeMiss::None;
    ShaderResources::ProofReport proof;
    // APS5_PROFILE_DRAW: the proof's and the record's time.
    double proofUs = 0;
    double recordUs = 0;
};
// A draw-cache hit recorded from its recipe, under GuestMemory::GpuMutex after the packet's labels
// (design_cpu_final M8, step 8b): the validation with the recipe's stored outputs, the index and
// vertex work as Draw's, the resident-target proof on the stored images (Refresh while Cached()),
// the template's proof (ProveCurrent) with the alias checks repeated, the pipeline and framebuffer
// from the recipe, then the record as Draw's (pass continued or begun, Kept, MarkGpuWrites). A
// miss records nothing.
DrawRecipeOutcome DrawWithRecipe(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, const DrawRecipe& recipe);
// Whether recorded draws build recipes and draw-cache hits use them (APS5_NO_DRAW_RECIPE unset).
bool DrawRecipes();

// How an indirect draw's records were read, for the [draws] line (APS5_PROFILE_DRAW): Gpu is
// vkCmdDrawIndirect from the host import; every other value names why the CPU read them instead,
// either decided by the driver before the draw (its patched-SGPR fold rules, the shader path, the
// draw index, the vertex ranges, the device features, APS5_NO_GPU_INDIRECT_DRAW) or under the device
// lock in Draw (the memory state of the records). `readMs` is the CPU read's time (its sync);
// `rewritten` counts a GPU-side draw whose records were copied and patched with a constant.
enum class IndirectDrawPath : std::uint8_t { Gpu, NotFolded, FetchUnknown, NonVertexPath, IndxOffset, DrawIndex, VertexRange, FeatureGap, PendingImage, PendingLabelOrCopy, NotImported, Disabled, IndexRange, Count };
void CountIndirectDraw(IndirectDrawPath path, double readMs, bool rewritten = false);
std::array<std::uint64_t, static_cast<std::size_t>(IndirectDrawPath::Count)> IndirectDrawCounts();
void CountEmptyIndirectDraw(double readMs);
std::uint64_t EmptyIndirectDrawCount();

struct DrawPassCounts {
    std::uint64_t draws = 0;
    std::uint64_t passesBegun = 0;
    std::uint64_t passesContinued = 0;
    std::uint64_t keyStoreFlushes = 0;
    std::uint64_t storeFlushes = 0;
};
std::array<DrawPassCounts, 2> DrawPassTotals();
const char* IndirectDrawPathName(IndirectDrawPath path);

// Draw packets that drew nothing, for the [draws] line: Nothing (an empty count, or no color writes
// and no pixel shader), Prechecked (State.hpp's DrawRejection refused it before the decode) and
// Thrown (the decode or the build threw); `us` is the packet's time in the driver.
enum class DrawSkip : std::uint8_t { Nothing, Prechecked, Thrown, Count };
void CountDrawSkip(DrawSkip kind, double us);

// Recorded draws whose written guest buffers were copied (their results reach guest memory by a CPU
// write-back when their batch completes), listed until that write-back ran. It is the draw
// counterpart of VulkanDevice's State::copiedWriters for dispatches, which VulkanDevice.cpp consults
// in two places: DispatchIndirect (an indirect dispatch whose arguments such a writer produced reads
// them on the CPU, behind the write-back, instead of from the host import on the GPU) and the fill
// HLE (a vkCmdFillBuffer over a range a listed write-back will land on must wait for it, or the
// write-back overwrites the fill). Draws are listed here only under APS5_RECORD_COPIED_DRAWS=1,
// which is safe only once BOTH of those checks also test this list, i.e. each extends its
// `std::any_of(state->copiedWriters ...)` with
// `|| std::any_of(Graphics::DrawCopiedWriters()->begin(), Graphics::DrawCopiedWriters()->end(), [&](const auto& writer) { return writer->WritesOverlap(<its range>); })`;
// cleaner still, State::copiedWriters can become this list (one registry for both producers), after
// which the switch can turn into APS5_NO_RECORD_COPIED_DRAWS. Until then such draws are recorded and
// waited for at once. Read and written under GuestMemory::GpuMutex, like the device's list.
std::shared_ptr<std::vector<std::shared_ptr<ShaderResources>>> DrawCopiedWriters();
std::uint64_t ResidentTargetMemoHits();

void RunColorMetadataPass(const Context& context, const ColorMetadataPass& pass);
void RunDepthCopyPass(const Context& context, const DepthCopyPass& pass);
std::vector<std::pair<std::uint64_t, std::uint64_t>> PipelinedColorMetadataPassWrites(const ColorMetadataPass& pass);
bool DrawPipelineEnabled();
bool CommitTrims();

}

#endif
