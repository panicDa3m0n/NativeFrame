#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::ColorTargetLayout;
using AgcDriver::Graphics::ColorTileMode;
using AgcDriver::Graphics::GuestBufferMemory;
using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::size_t BufferBytes = 65536;
constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 128;
constexpr std::size_t PixelBytes = std::size_t{Width} * Height * 4u;
constexpr std::size_t StoreBytes = 2 * PixelBytes;
constexpr std::uint32_t Stored = 0xa5a5a5a5u;

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 13> PixelCode{
    0x7e040f00, 0x7e060f01, 0x34060688, 0x4a040702, 0x34040482, 0x7e0802ff, Stored,
    0xe0705000, 0x80000402, 0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000,
};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = AddressOf(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, stride == 0u ? 0x31016facu : 0x01016facu};
}

class GuestBuffer {
public:
    explicit GuestBuffer(bool registered, std::size_t bytes = BufferBytes) : registered(registered), bytes(bytes), watched(registered && AgcDriver::GuestMemory::WriteWatched()) {
#ifdef _WIN32
        if (watched) {
            block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
            if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
        } else {
            block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
#else
        if (watched) {
            constexpr std::uintptr_t alignment = 65536;
            void* mapped = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            Require(mapped != MAP_FAILED, "draw staged writes: cannot map a guest buffer");
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
        } else {
            block = static_cast<std::uint8_t*>(std::aligned_alloc(65536, bytes));
        }
#endif
        Require(block != nullptr, "draw staged writes: cannot allocate a guest buffer");
        std::memset(block, 0, bytes);
        if (registered) GuestAllocations::Mutation().Add(block, bytes, true, true, true);
    }

    ~GuestBuffer() {
        if (registered) {
            AgcDriver::Graphics::StorageTexture::FlushPending(AddressOf(block), bytes, nullptr, "test release");
            GuestAllocations::Mutation().Remove(block);
        }
#ifdef _WIN32
        if (watched) {
            GuestArena::GuestArenaReset_nid_postfix(block, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
        } else {
            VirtualFree(block, 0, MEM_RELEASE);
        }
#else
        if (watched) {
            munmap(block, bytes);
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
        } else {
            std::free(block);
        }
#endif
    }

    GuestBuffer(const GuestBuffer&) = delete;
    GuestBuffer& operator=(const GuestBuffer&) = delete;

    std::uint8_t* Data() { return block; }

private:
    bool registered;
    std::size_t bytes;
    bool watched;
    std::uint8_t* block = nullptr;
};

const ColorTargetLayout TargetLayout(Width, Height, ColorTileMode::RenderTarget, 4);

constexpr std::array<std::array<float, 4>, 3> Triangle{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}
}};

std::uint64_t StagedBytes() {
    const auto sources = GuestBufferMemory::StagingSources();
    return sources.keptBytes + sources.deviceBytes + sources.importBytes;
}

void Draw(AgcDriver::VulkanDevice& device, GuestBuffer& pixels, GuestBuffer& vertices, GuestBuffer& stores) {
    const auto target = device.Target();
    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto vertexBuffer = BufferDescriptor(vertices.Data(), 16u, static_cast<std::uint32_t>(BufferBytes / 16u));
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {64, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    std::vector<std::uint32_t> pixelUserData(4, 0u);
    const auto storeBuffer = BufferDescriptor(stores.Data(), 0u, static_cast<std::uint32_t>(StoreBytes));
    std::copy(storeBuffer.begin(), storeBuffer.end(), pixelUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {64, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}
    }};

    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    state.color = {AddressOf(pixels.Data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, TargetLayout.Bytes(), 0xe4u, ColorTileMode::RenderTarget};
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
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Triangle.size()), 0, 1, 0, false};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    device.Draw(state, draw, shaders);
}

void Settle(AgcDriver::VulkanDevice& device, GuestBuffer& pixels, GuestBuffer& stores) {
    device.WaitIdle();
    AgcDriver::Graphics::StorageTexture::FlushPending(AddressOf(pixels.Data()), TargetLayout.Bytes(), nullptr, "draw staged writes test");
    AgcDriver::Graphics::StorageTexture::FlushPending(AddressOf(stores.Data()), StoreBytes, nullptr, "draw staged writes test");
    device.WaitIdle();
}

bool Filled(const std::uint8_t* data, std::size_t bytes, std::uint8_t value) {
    return std::all_of(data, data + bytes, [value](std::uint8_t byte) { return byte == value; });
}

bool Stores(const std::uint8_t* data, std::size_t bytes) {
    for (std::size_t offset = 0; offset < bytes; offset += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, data + offset, sizeof(word));
        if (word != Stored) return false;
    }
    return true;
}

void StagedCase(AgcDriver::VulkanDevice& device, bool stagingOn) {
    GuestBuffer pixels(true, TargetLayout.Bytes());
    GuestBuffer vertices(false);
    GuestBuffer stores(true, StoreBytes);
    std::memcpy(vertices.Data(), Triangle.data(), sizeof(Triangle));
    std::memset(stores.Data(), 0x11, StoreBytes);
    const auto before = StagedBytes();
    Draw(device, pixels, vertices, stores);
    Settle(device, pixels, stores);
    const auto staged = StagedBytes() - before;
    Require(Filled(pixels.Data(), 4096, 0xff), "the draw did not reach its resident color target");
    Require(Stores(stores.Data(), PixelBytes), "a pixel's buffer store did not reach guest memory");
    Require(Filled(stores.Data() + PixelBytes, StoreBytes - PixelBytes, 0x11), "the buffer bytes no pixel stores to lost the CPU's values");
    if (stagingOn) Require(staged >= StoreBytes, "a recorded draw's written buffer was not staged in device memory (" + std::to_string(staged) + " bytes staged)");
    else Require(staged == 0, "APS5_NO_DRAW_STAGING=1 still staged a draw's written buffer (" + std::to_string(staged) + " bytes)");

    std::memset(stores.Data(), 0x33, PixelBytes);
    std::memset(stores.Data() + PixelBytes, 0x22, StoreBytes - PixelBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Draw(device, pixels, vertices, stores);
    Settle(device, pixels, stores);
    Require(Stores(stores.Data(), PixelBytes), "a reused draw build's buffer stores did not reach guest memory");
    Require(Filled(stores.Data() + PixelBytes, StoreBytes - PixelBytes, 0x22), "a reused draw build overwrote the CPU's newer bytes in the range no pixel stores to");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const bool stagingOn = std::getenv("APS5_NO_DRAW_STAGING") == nullptr;
        StagedCase(*device, stagingOn);
        std::printf("draw staged writes tests passed (staging %s)\n", stagingOn ? "on" : "off");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
