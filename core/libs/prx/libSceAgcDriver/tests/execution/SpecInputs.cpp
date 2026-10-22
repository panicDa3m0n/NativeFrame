#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

// Speculated draw inputs (APS5_SPEC_INPUTS=1, O22.T3): a prepare thread derives a draw's fragment
// outputs, vertex input layout, set layout keys and pipeline from its stages and state, and the
// commit takes them only for the same state and stage objects:
//   the same draw: outputs and pipeline taken, the pipeline the commit itself would look up;
//   a target format changed (another state): refused, the commit's own pipeline differs;
//   another stage list, a transitioned draw, a proxied target, other set layout keys, another
//   vertex layout: refused.

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {

using namespace AgcDriver::Graphics;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::Width;
using ShaderRecompiler::ShaderStage;

State ColorState(VkFormat format) {
    State state{};
    state.stages = {ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(DepthFastClearHarness::Pixels.data()), {Width, Height}, format, DepthFastClearHarness::Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    return state;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
        DescriptorCache cache(context);
        context.descriptorCache = &cache;
        const auto compiled = DepthFastClearHarness::Compile(*device);
        const std::array<CompiledShader, 2> shaders{{
            {ShaderStage::Vertex, &compiled.vertex, 0},
            {ShaderStage::Fragment, &compiled.pixel, PixelPushOffset(compiled.vertexPush, device->Target())}
        }};
        const auto state = ColorState(VK_FORMAT_R8G8B8A8_UNORM);

        std::shared_ptr<const DrawInputSpeculation> speculation;
        std::thread prepareThread([&] { speculation = SpeculateDrawInputs(context, state, shaders); });
        prepareThread.join();
        Require(speculation != nullptr && speculation->pipeline != nullptr, "the prepare thread did not speculate the draw's inputs");

        const auto described = DescribeSetLayouts(context, shaders);
        const auto layouts = described.View();
        const auto vertexInput = BuildVertexInputLayout(context, compiled.vertex.vertexAttributes);
        const auto* outputs = TakeSpeculatedOutputs(speculation.get(), context, state, shaders);
        Require(outputs != nullptr && (*outputs & 1u) != 0, "the commit did not take the speculated fragment outputs of the same draw");
        const auto taken = TakeSpeculatedPipeline(speculation.get(), context, state, shaders, vertexInput, layouts, true, false);
        Require(taken != nullptr && taken == speculation->pipeline, "the commit did not take the speculated pipeline of the same draw");
        Require(CachedPipeline(context, state, vertexInput, layouts, shaders, VK_IMAGE_LAYOUT_GENERAL) == taken, "the speculated pipeline is not the one the commit looks up");

        const auto changed = ColorState(VK_FORMAT_B8G8R8A8_UNORM);
        Require(TakeSpeculatedOutputs(speculation.get(), context, changed, shaders) == nullptr, "fragment outputs speculated for one state were taken for another");
        Require(TakeSpeculatedPipeline(speculation.get(), context, changed, shaders, vertexInput, layouts, true, false) == nullptr, "a pipeline speculated for an R8G8B8A8 target was taken for a B8G8R8A8 one");
        Require(CachedPipeline(context, changed, vertexInput, layouts, shaders, VK_IMAGE_LAYOUT_GENERAL) != taken, "the changed target format looks up the same pipeline: the refusal is not tested");

        const auto copied = shaders;
        Require(TakeSpeculatedPipeline(speculation.get(), context, state, copied, vertexInput, layouts, true, false) == nullptr, "a pipeline speculated for one stage list was taken for another");
        Require(TakeSpeculatedPipeline(speculation.get(), context, state, shaders, vertexInput, layouts, false, false) == nullptr, "a pipeline speculated for the general layout was taken for a transitioned draw");
        Require(TakeSpeculatedPipeline(speculation.get(), context, state, shaders, vertexInput, layouts, true, true) == nullptr, "a pipeline speculated without proxies was taken for a proxied target");
        auto otherKey = described.mainKey;
        otherKey.push_back(VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);
        const PipelineSetLayouts otherLayouts{layouts.main, layouts.fragment, layouts.split, otherKey, layouts.fragmentKey};
        Require(TakeSpeculatedPipeline(speculation.get(), context, state, shaders, vertexInput, otherLayouts, true, false) == nullptr, "a pipeline speculated for one set layout key was taken for another");
        auto otherInput = vertexInput;
        otherInput.bindings.push_back({static_cast<std::uint32_t>(otherInput.bindings.size()), 16u, VK_VERTEX_INPUT_RATE_VERTEX});
        Require(TakeSpeculatedPipeline(speculation.get(), context, state, shaders, otherInput, layouts, true, false) == nullptr, "a pipeline speculated for one vertex input layout was taken for another");
        device->WaitIdle();
        std::puts("spec inputs tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
