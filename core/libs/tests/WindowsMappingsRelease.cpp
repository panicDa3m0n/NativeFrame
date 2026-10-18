#include "prx/libc/include/WindowsMappings.hpp"
#include <cstdio>
#include <stdexcept>
#include <source_location>

static void Require(bool value, std::source_location location = std::source_location::current()) {
    if (!value) throw std::runtime_error("Windows resident release regression at " + std::to_string(location.line()));
}

static void CheckMetadataMismatch(unsigned mode) {
    constexpr std::size_t page = 65536;
    auto& mappings = GuestArena::WindowsMappings::Get();
    auto* base = static_cast<unsigned char*>(mappings.Reserve(nullptr, 3 * page));
    Require(base != nullptr);
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 3 * page, nullptr);
    Require(section != nullptr);
    auto* address = base + page;
    mappings.Map(address, page, section, 0, PAGE_READWRITE, true, true);
    Require(VirtualFree(address, 0, MEM_RELEASE));
    if (mode == 0) {
        Require(VirtualFree(base, 0, MEM_RELEASE));
        Require(VirtualFree(base + 2 * page, 0, MEM_RELEASE));
        Require(VirtualAlloc(base, 3 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == base);
    } else if (mode == 1) {
        Require(VirtualFree(base + 2 * page, 0, MEM_RELEASE));
        Require(VirtualAlloc(address, 2 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == address);
    } else {
        Require(MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, page, address) == address);
    }
    address[0] = 73;
    bool rejected = false;
    try { mappings.Reset(address, page); }
    catch (const std::runtime_error&) { rejected = true; }
    std::uint32_t protection = 0;
    Require(rejected && address[0] == 73 && mappings.Protection(reinterpret_cast<std::uintptr_t>(address), &protection));
    if (mode == 2) Require(UnmapViewOfFile(address));
    else Require(VirtualFree(mode == 0 ? base : address, 0, MEM_RELEASE));
    CloseHandle(section);
}

static void CheckValidRelease() {
    constexpr std::size_t bytes = 2 * 1024 * 1024;
    auto& mappings = GuestArena::WindowsMappings::Get();
    auto* base = static_cast<unsigned char*>(mappings.Reserve(nullptr, bytes));
    Require(base != nullptr);
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, nullptr);
    Require(section != nullptr);
    mappings.Map(base, bytes, section, 0, PAGE_READWRITE, true, true);
    base[0] = 31;
    base[bytes - 1] = 89;
    DWORD previous;
    Require(VirtualProtect(base + 65536, 65536, PAGE_READONLY, &previous));
    mappings.Reset(base, bytes);
    mappings.Map(base, bytes, section, 0, PAGE_READWRITE, true, true);
    Require(base[0] == 31 && base[bytes - 1] == 89);
    mappings.Reset(base, bytes);
    Require(VirtualFree(base, 0, MEM_RELEASE));
    mappings.ForgetSection(section);
    CloseHandle(section);
}

int main(int argc, char** argv) {
    try {
        if (argc == 2) CheckMetadataMismatch(static_cast<unsigned>(argv[1][0] - '0'));
        else CheckValidRelease();
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "%s\n", failure.what());
        return 1;
    }
    return 0;
}
