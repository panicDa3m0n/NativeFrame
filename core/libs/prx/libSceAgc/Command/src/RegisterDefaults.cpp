#include "prx/libSceAgc/Command/include/RegisterDefaults.hpp"
#include "prx/libSceAgc/Command/include/Packet.hpp"
#include "prx/libc/include/ApplicationHeap.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <mutex>
#include <vector>

namespace Agc::Command {

namespace {

struct TypeIndex {
    std::uint32_t key;
    std::uint32_t bankIndex;
};

static_assert(sizeof(TypeIndex) == 8);

struct RegisterDefaults {
    ShaderRegister** tables[4];
    std::uint32_t registerCounts[4];
    TypeIndex* types;
    std::uint32_t count;
};

static_assert(offsetof(RegisterDefaults, types) == 0x30);
static_assert(offsetof(RegisterDefaults, count) == 0x38);

struct CompactBank {
    ShaderRegister* registers;
    std::uint32_t registerCount;
    const std::uint16_t* offsets;
    std::uint32_t pointerCount;
};

struct CompactRegisterDefaults {
    CompactBank banks[4];
    std::uint32_t* types;
    std::uint32_t count;
};

#include "prx/libSceAgc/Command/include/RegisterDefaultsData.hpp"

struct Storage {
    RegisterDefaults defaults{};
    std::array<std::vector<ShaderRegister*>, 4> pointers;
    std::vector<TypeIndex> types;
    bool initialized = false;
};

std::mutex defaultsMutex;
std::array<std::array<Storage, 14>, 2> storage;

// The SDK the title was built with, from its process parameters (0 without them, as in the tests).
std::uint32_t titleSdkVersion() {
    static const std::uint32_t version = [] {
        try {
            std::uint32_t value = 0;
            std::memcpy(&value, static_cast<const std::byte*>(ApplicationProcessParameters_nid_no_patch()) + 0x10, sizeof(value));
            return value;
        } catch (const std::exception&) {
            return std::uint32_t{0};
        }
    }();
    return version;
}

// Titles built before SDK 10.00 walk the compact 12-byte type entries, whose second word also packs
// each block's register count and stride: Astro Bot (PPSA21567, SDK 9.04) writes past its
// command buffer when it gets the pairs. Later titles, PPSA99011 among them, read the pairs.
bool compactTypes() {
    const auto sdk = titleSdkVersion();
    return sdk != 0 && sdk < 0x10000000u;
}

struct AgcContextState {
    std::uint32_t data[512];
};

static_assert(sizeof(AgcContextState) == 2048);

std::mutex defaultStateMutex;
AgcContextState defaultState{};
bool defaultStateInitialized = false;

void InitDefaultCxState() {
    std::memset(&defaultState, 0, sizeof(defaultState));
    const auto* source = agcPublicRegDefaultsByVersion[8];
    for (std::size_t bank = 0; bank < 4; ++bank) {
        const auto& b = source->banks[bank];
        if (b.registers == nullptr) continue;
        for (std::uint32_t r = 0; r < b.registerCount; ++r) {
            const auto offset = b.registers[r].offset;
            const auto value = b.registers[r].value;
            if (offset < 512) {
                defaultState.data[offset] = value;
            }
        }
    }
    defaultStateInitialized = true;
}

}

void* GetRegisterDefaults(std::uint32_t version, bool internal, const char* function) {
    Require(version < 14, function, "unsupported register defaults version");
    const auto* source = internal ? agcInternalRegDefaultsByVersion[version] : agcPublicRegDefaultsByVersion[version];
    std::lock_guard lock(defaultsMutex);
    auto& destination = storage[internal ? 1 : 0][version];
    if (!destination.initialized) {
        std::array<std::vector<ShaderRegister*>, 4> pointers;
        RegisterDefaults defaults{};
        for (std::size_t bank = 0; bank < pointers.size(); ++bank) {
            const auto& input = source->banks[bank];
            Require(input.pointerCount == 0 || (input.registers != nullptr && input.offsets != nullptr), function, "invalid register defaults bank");
            auto& table = pointers[bank];
            table.reserve(input.pointerCount);
            for (std::uint32_t index = 0; index < input.pointerCount; ++index) {
                Require(input.offsets[index] < input.registerCount, function, "register defaults pointer exceeds bank");
                table.push_back(input.registers + input.offsets[index]);
            }
            defaults.tables[bank] = table.empty() ? nullptr : table.data();
            defaults.registerCounts[bank] = input.registerCount;
        }
        Require(source->count == 0 || source->types != nullptr, function, "missing register defaults types");
        std::vector<TypeIndex> types;
        types.reserve(source->count);
        for (std::uint32_t entry = 0; entry < source->count; ++entry) {
            const std::uint32_t bankIndex = source->types[entry * 3 + 1] & 0x3ffu;
            Require((bankIndex >> 2) < source->banks[bankIndex & 3u].pointerCount, function, "register defaults type exceeds its bank");
            types.push_back({source->types[entry * 3], bankIndex});
        }
        defaults.types = compactTypes() ? reinterpret_cast<TypeIndex*>(source->types) : types.empty() ? nullptr : types.data();
        defaults.count = source->count;
        destination.pointers = std::move(pointers);
        destination.types = std::move(types);
        destination.defaults = defaults;
        destination.initialized = true;
    }
    return &destination.defaults;
}

int GetDefaultCxStateFlat(void* destination, std::uint32_t size) {
    if (destination == nullptr) return static_cast<int>(0x80890001);
    std::lock_guard lock(defaultStateMutex);
    if (!defaultStateInitialized) {
        InitDefaultCxState();
    }
    const auto copyBytes = std::min(size, static_cast<std::uint32_t>(sizeof(AgcContextState)));
    std::memcpy(destination, defaultState.data, copyBytes);
    return 0;
}

}
