#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

// The buffer read probe (APS5_BUFFER_READ_PROBE=1, O16.T15) protects the interior pages of a buffer
// range the GPU wrote: the first native read faults once, is counted for the title's image (this
// test's executable) and returns the bytes that were there; a second read does not fault; after the
// range is protected again a native write is counted as a write and lands.

namespace {

using namespace AgcDriver::Graphics;

void Expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

}

int main() {
    try {
#ifndef _WIN32
        std::cout << "the buffer read probe is Windows only: not tested\n";
        return 77;
#else
        if (!BufferReadProbe()) {
            std::cout << "APS5_BUFFER_READ_PROBE is not set: buffer read probe not tested\n";
            return 77;
        }
        constexpr std::size_t Bytes = std::size_t{4} << 20u;
        auto* memory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, Bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Expect(memory != nullptr, "cannot allocate the test range");
        for (std::size_t i = 0; i < Bytes; ++i) memory[i] = static_cast<std::uint8_t>(i * 13u + 7u);
        const auto address = reinterpret_cast<std::uint64_t>(memory);
        ResetBufferReadProbe();
        ProbeProtectWrittenRange(address + 100, Bytes - 200);
        auto totals = BufferReadProbeTotals();
        Expect(totals.protects == 1, "the written range was not protected");
        constexpr auto title = static_cast<std::size_t>(ProbeModule::Title);

        const volatile std::uint8_t* inside = memory + 8192 + 5;
        const std::uint8_t first = *inside;
        Expect(first == static_cast<std::uint8_t>((8192 + 5) * 13u + 7u), "the first read did not return the bytes that were there");
        totals = BufferReadProbeTotals();
        Expect(totals.reads[title] == 1 && totals.writes[title] == 0, "the first read was not counted once for the title's image (" + std::to_string(totals.reads[title]) + " reads)");
        const std::uint8_t second = *(inside + 4096);
        Expect(second == static_cast<std::uint8_t>((8192 + 5 + 4096) * 13u + 7u), "the second read returned other bytes");
        Expect(BufferReadProbeTotals().reads[title] == 1, "a read after the range got its protection back faulted again");

        // The border pages are left alone: a byte before the first interior page never faults.
        const volatile std::uint8_t* border = memory + 50;
        static_cast<void>(*border);
        Expect(BufferReadProbeTotals().reads[title] == 1, "a border page was protected");

        ProbeProtectWrittenRange(address + 100, Bytes - 200);
        volatile std::uint8_t* target = memory + 3 * 65536;
        *target = 0xa5;
        totals = BufferReadProbeTotals();
        Expect(totals.protects == 2 && totals.writes[title] == 1, "the write after the second protect was not counted");
        Expect(memory[3 * 65536] == 0xa5, "the write did not land");

        // A driver read inside a ProbeHostRead scope does not fault and is not counted; the pages
        // are armed again when the scope ends, so the title's next read still counts.
        ProbeProtectWrittenRange(address + 100, Bytes - 200);
        {
            const ProbeHostRead scope(address + 65536, 65536);
            const volatile std::uint8_t* scoped = memory + 65536 + 10;
            Expect(*scoped == static_cast<std::uint8_t>((65536 + 10) * 13u + 7u), "a read inside a scope returned other bytes");
        }
        totals = BufferReadProbeTotals();
        Expect(totals.reads[title] == 1 && totals.scoped == 16, "a read inside a scope was counted or the scope gave back other pages (" + std::to_string(totals.scoped) + " pages)");
        const volatile std::uint8_t* after = memory + 65536 + 20;
        static_cast<void>(*after);
        Expect(BufferReadProbeTotals().reads[title] == 2, "the scope did not arm its pages again");

        // A page someone else made accessible again counts as lost and is armed at the next protect.
        ProbeProtectWrittenRange(address + 100, Bytes - 200);
        DWORD previous = 0;
        Expect(VirtualProtect(memory + 5 * 65536, 4096, PAGE_READWRITE, &previous) != 0 && previous == PAGE_NOACCESS, "the page was not protected before the foreign change");
        ProbeProtectWrittenRange(address + 100, Bytes - 200);
        totals = BufferReadProbeTotals();
        Expect(totals.lost == 1, "the page made accessible again was not counted as lost (" + std::to_string(totals.lost) + ")");
        const volatile std::uint8_t* rearmed = memory + 5 * 65536 + 1;
        static_cast<void>(*rearmed);
        Expect(BufferReadProbeTotals().reads[title] == 3, "the lost page was not armed again");
        ResetBufferReadProbe();
        VirtualFree(memory, 0, MEM_RELEASE);
        std::cout << "buffer read probe counts native accesses once per protect\n";
        return 0;
#endif
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
