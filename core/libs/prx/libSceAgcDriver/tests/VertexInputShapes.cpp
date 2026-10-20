#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include <atomic>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;
using ShaderRecompiler::VertexAttribute;
std::atomic<unsigned> queries{0};

Context context() {
    Context result{};
    result.limits.maxVertexInputBindings = 32;
    result.limits.maxVertexInputAttributes = 32;
    result.limits.maxVertexInputBindingStride = 2048;
    result.formatProperties = [](VkPhysicalDevice, VkFormat, VkFormatProperties* properties) {
        ++queries;
        *properties = {};
        properties->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    };
    return result;
}

VertexAttribute attribute(std::uint32_t stride, std::uint32_t address = 0x1000) {
    VertexAttribute result{};
    result.location = 0;
    result.components = 4;
    result.resource.fields = {address, stride << 16u, 128, 77u << 12u};
    return result;
}

void run() {
    auto ctx = context();
    VertexInputCache cache(ctx);
    const std::vector<VertexAttribute> narrow{attribute(32)};
    const std::vector<VertexAttribute> wide{attribute(48)};
    Require(cache.TryGet(7, narrow) != nullptr, "the first shape of a variant was not cached");
    Require(cache.TryGet(7, wide) == nullptr, "a variant whose shape changed was not bypassed");
    const auto before = queries.load();
    const auto wideLayout = cache.TryGetByShape(wide);
    Require(wideLayout != nullptr && wideLayout->bindings.size() == 1 && wideLayout->bindings[0].stride == 48, "a bypassed variant's shape got no layout of its own");
    const auto built = queries.load();
    Require(built > before, "a new shape was not built");
    Require(cache.TryGetByShape(wide) == wideLayout && queries.load() == built, "the same shape was built again");
    const auto narrowLayout = cache.TryGetByShape(narrow);
    Require(narrowLayout != nullptr && narrowLayout != wideLayout && narrowLayout->bindings[0].stride == 32, "two shapes that differ in stride shared a layout");
    const std::vector<VertexAttribute> moved{attribute(48, 0x2000)};
    Require(cache.TryGetByShape(moved) == wideLayout, "a shape at another address did not reuse its layout");
    const std::vector<VertexAttribute> unaligned{attribute(48, 0x2002)};
    bool rejected = false;
    try {
        static_cast<void>(cache.TryGetByShape(unaligned));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Require(rejected, "a reused shape accepted an unaligned vertex buffer");
    Require(cache.ShapeCount() == 2, "the shape cache holds an unexpected number of shapes");
    VertexInputCache small(ctx, 1);
    Require(small.TryGetByShape(narrow) != nullptr && small.TryGetByShape(wide) == nullptr && small.ShapeCount() == 1, "the shape cache grew past its capacity");
    Require(cache.TryGetByShape({}) == nullptr, "an empty attribute list entered the shape cache");
}

}

int main() {
    try {
        run();
        std::puts("vertex input shape tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
