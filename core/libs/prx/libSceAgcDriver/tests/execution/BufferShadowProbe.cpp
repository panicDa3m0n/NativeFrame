#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

// The buffer shadow probe (APS5_BUFFER_SHADOW_PROBE=1, O16.T10 P0) counts what publishing the bytes
// the GPU wrote into buffers at the boundaries a title can observe would copy: a dispatch writing
// 4 MiB, a hook read before the next label and two labels give 1024 pages published at the first
// boundary, none at the second, and one reader meeting live bytes. Overlapping writes count their
// pages once; a read after the boundary meets nothing.

namespace {

using namespace AgcDriver::Graphics;

void Expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint64_t Base = 0x100000000ull;
constexpr std::size_t FourMiB = std::size_t{4} << 20u;

}

int main() {
    try {
        if (!BufferShadowProbe()) {
            std::cout << "APS5_BUFFER_SHADOW_PROBE is not set: buffer shadow probe not tested\n";
            return 77;
        }
        ResetBufferShadowProbe();
        ProbeGpuBufferWrite(Base, FourMiB, true);
        ProbeBufferRead(ProbeReader::Hook, Base + 4096, 64);
        ProbeBufferBoundary(ProbeBoundary::Label);
        ProbeBufferRead(ProbeReader::Hook, Base + 8192, 64);
        ProbeBufferBoundary(ProbeBoundary::Label);
        auto totals = BufferShadowProbeTotals();
        Expect(totals.publishedPages == 1024, "a 4 MiB write published " + std::to_string(totals.publishedPages) + " pages, not 1024");
        Expect(totals.publishedBytes == FourMiB, "a 4 MiB write published " + std::to_string(totals.publishedBytes) + " bytes");
        Expect(totals.boundaries[static_cast<std::size_t>(ProbeBoundary::Label)] == 2 && totals.busyBoundaries == 1, "two labels did not count as two boundaries, one with pages");
        Expect(totals.reads[static_cast<std::size_t>(ProbeReader::Hook)] == 1 && totals.readBytes[static_cast<std::size_t>(ProbeReader::Hook)] == 64, "the hook read before the label was not the one reader meeting live bytes");
        Expect(totals.stagedBytes == FourMiB && totals.inPlaceBytes == 0 && totals.writes == 1, "the staged write was not counted as staged");

        // Overlapping and unaligned writes: their union [Base + 100, Base + 12288) touches pages 0 to 2.
        ResetBufferShadowProbe();
        ProbeGpuBufferWrite(Base + 100, 8192, false);
        ProbeGpuBufferWrite(Base + 4096, 8192, false);
        ProbeBufferBoundary(ProbeBoundary::Submission);
        totals = BufferShadowProbeTotals();
        Expect(totals.publishedPages == 3, "overlapping writes published " + std::to_string(totals.publishedPages) + " pages, not 3");
        Expect(totals.publishedBytes == 4096 + 8192 - 100, "overlapping writes published " + std::to_string(totals.publishedBytes) + " bytes");
        Expect(totals.inPlaceBytes == 16384 && totals.boundaries[static_cast<std::size_t>(ProbeBoundary::Submission)] == 1, "the in-place writes or the submission boundary were not counted");
        std::cout << "buffer shadow probe counts as expected\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
