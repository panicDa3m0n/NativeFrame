#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::Pm4::DecodeCopyFromGds;
using AgcDriver::Pm4::DecodeCopyToGds;
using AgcDriver::Pm4::DecodeMemoryCopy;
using AgcDriver::Pm4::GdsAddress;
using AgcDriver::Pm4::GdsBytes;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint32_t> dma(std::uint32_t control, std::uint64_t from, std::uint64_t to, std::uint32_t command) {
    return {0xc0055000u, control, static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(from >> 32u), static_cast<std::uint32_t>(to), static_cast<std::uint32_t>(to >> 32u), command};
}

void fromGds() {
    constexpr std::uint64_t destination = 0x403d7b400ull;
    const auto copy = DecodeCopyFromGds(dma(0x20000000u, 0x100, destination, 64));
    check(copy.has_value() && copy->source == GdsAddress() + 0x100 && copy->destination == destination && copy->bytes == 64, "a DMA_DATA from the GDS to memory did not decode as a copy out of the GDS memory");
    check(DecodeCopyFromGds(dma(0x20300000u, 0x100, destination, 64)).has_value(), "a DMA_DATA from the GDS to memory selector 3 did not decode");
    check(!DecodeCopyFromGds(dma(0x20100000u, 0x100, 0x200, 64)).has_value(), "a DMA_DATA from the GDS into the GDS decoded as a copy to memory");
    check(!DecodeCopyFromGds(dma(0x60000000u, 0x100, destination, 64)).has_value(), "a memory-to-memory DMA_DATA decoded as a copy out of the GDS");
    check(!DecodeCopyFromGds(dma(0x40000000u, 0x100, destination, 64)).has_value(), "an immediate fill decoded as a copy out of the GDS");
    check(!DecodeCopyFromGds(dma(0x20000000u, 0x100, destination, 64 | (1u << 26u))).has_value(), "a register-space GDS source decoded as a copy");
    check(!DecodeCopyFromGds(dma(0x20000000u, 0x100, destination, 64 | (1u << 27u))).has_value(), "a register destination decoded as a copy out of the GDS");
    check(!DecodeCopyFromGds(dma(0x20000000u, 0x100, destination, 64 | (1u << 28u))).has_value(), "a non-incrementing GDS source decoded as a copy");
    check(!DecodeCopyFromGds(dma(0x20000000u, 0x100, destination, 64 | (1u << 29u))).has_value(), "a non-incrementing destination decoded as a copy out of the GDS");
    check(!DecodeCopyFromGds(dma(0x20000000u, 0x102, destination, 64)).has_value(), "a misaligned GDS offset decoded as a copy");
    check(!DecodeCopyFromGds(dma(0x20000000u, GdsBytes - 32, destination, 64)).has_value(), "a copy past the end of the GDS decoded");
    check(DecodeCopyFromGds(dma(0x20000000u, GdsBytes - 64, destination, 64)).has_value(), "a copy ending at the end of the GDS did not decode");
    check(!DecodeCopyFromGds(dma(0x20000000u, 0x1000000100ull, destination, 64)).has_value(), "a GDS offset with high bits decoded as a copy");
    check(!DecodeCopyFromGds(dma(0x20000000u, 0x100, destination, 0)).has_value(), "an empty DMA_DATA decoded as a copy out of the GDS");
    check(!DecodeMemoryCopy(dma(0x20000000u, 0x100, destination, 64)).has_value(), "a DMA_DATA from the GDS decoded as a memory copy");
}

void toGds() {
    constexpr std::uint64_t source = 0x1120000000ull;
    const auto copy = DecodeCopyToGds(dma(0x60100000u, source, 0x200, 128));
    check(copy.has_value() && copy->source == source && copy->destination == GdsAddress() + 0x200 && copy->bytes == 128, "a DMA_DATA from memory into the GDS did not decode as a copy into the GDS memory");
    check(!DecodeCopyToGds(dma(0x40100000u, 0x44332211u, 0x200, 128)).has_value(), "an immediate into the GDS decoded as a copy");
    check(!DecodeCopyToGds(dma(0x20100000u, 0x100, 0x200, 128)).has_value(), "a GDS-to-GDS DMA_DATA decoded as a copy from memory");
    check(!DecodeCopyToGds(dma(0x60000000u, source, 0x200, 128)).has_value(), "a memory-to-memory DMA_DATA decoded as a copy into the GDS");
    check(!DecodeCopyToGds(dma(0x60100000u, source, 0x200, 128 | (1u << 26u))).has_value(), "a register source decoded as a copy into the GDS");
    check(!DecodeCopyToGds(dma(0x60100000u, source, 0x200, 128 | (1u << 29u))).has_value(), "a non-incrementing GDS destination decoded as a copy");
    check(!DecodeCopyToGds(dma(0x60100000u, source, 0x202, 128)).has_value(), "a misaligned GDS destination decoded as a copy");
    check(!DecodeCopyToGds(dma(0x60100000u, source, GdsBytes - 64, 128)).has_value(), "a copy past the end of the GDS decoded");
    check(!DecodeMemoryCopy(dma(0x60100000u, source, 0x200, 128)).has_value(), "a DMA_DATA into the GDS decoded as a memory copy");
}

}

int main() {
    try {
        fromGds();
        toGds();
        std::cout << "DMA_DATA copies out of and into the GDS decode as copies of the GDS memory\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
