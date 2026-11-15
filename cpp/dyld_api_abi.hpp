#pragma once

// Derives the libdyld -> dyld4::APIs virtual-call ABI (vtable slot offsets and
// pointer-authentication discriminators) from the cached libdyld code itself,
// so a shared-cache update does not need a hand-maintained per-UUID table.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "shared_cache.hpp"

namespace arm64metal {

struct DyldApiCall {
    std::uint64_t offset = 0;         // byte offset into the APIs vtable
    std::uint64_t discriminator = 0;  // 16-bit ptrauth discriminator for the slot
};

struct DyldApiAbi {
    std::uint64_t vptrDiscriminator = 0;
    DyldApiCall initialize;
    DyldApiCall writableTpro;
    DyldApiCall basePlatform;
    DyldApiCall activePlatform;
    DyldApiCall simulatorPlatform;
    DyldApiCall bulkImageLoads;
    DyldApiCall sharedCacheRealPath;
    DyldApiCall sharedCacheContainsPath;
    DyldApiCall dlopenFrom;
    DyldApiCall programSdkVersion;
    DyldApiCall programMinOsVersion;
    std::uint64_t vtableBytes = 0;    // highest derived slot end
};

namespace dyld_api_abi_detail {

struct ScannedCall {
    std::uint64_t vptrDiscriminator;
    DyldApiCall call;
};

// Linear scan of `code` for the clang arm64e virtual-call sequence:
//   movk xK, #vptrDisc, lsl #48 ; autda xV, xK ;
//   { ldr xT, [xV, #off]! | mov x17, #off ; add xV, xV, x17 ; ldr xT, [xV] } ;
//   movk xD, #slotDisc, lsl #48
inline std::vector<ScannedCall> scan_virtual_calls(const std::vector<std::uint8_t>& code) {
    std::vector<ScannedCall> calls;
    int pendingKeyReg = -1;
    std::uint64_t pendingKeyDisc = 0;
    int vtableReg = -1;
    std::uint64_t vptrDisc = 0;
    std::optional<std::uint64_t> offset;
    std::optional<std::uint64_t> movOffset;
    for (std::size_t i = 0; i + 4 <= code.size(); i += 4) {
        const std::uint32_t insn = static_cast<std::uint32_t>(code[i]) |
                                   (static_cast<std::uint32_t>(code[i + 1]) << 8) |
                                   (static_cast<std::uint32_t>(code[i + 2]) << 16) |
                                   (static_cast<std::uint32_t>(code[i + 3]) << 24);
        // Stop at the function's return so a function without an APIs call
        // cannot pick up a neighbouring function's slot.
        if (insn == 0xD65F03C0U || insn == 0xD65F0BFFU || insn == 0xD65F0FFFU) break;
        const unsigned rd = insn & 31U;
        const unsigned rn = (insn >> 5) & 31U;
        if ((insn & 0xFF800000U) == 0xF2800000U) {  // movk (64-bit)
            const unsigned hw = (insn >> 21) & 3U;
            const std::uint64_t imm = (insn >> 5) & 0xFFFFU;
            if (hw != 3) continue;
            if (vtableReg >= 0 && offset) {
                calls.push_back({vptrDisc, {*offset, imm}});
                vtableReg = -1;
                offset.reset();
                movOffset.reset();
                pendingKeyReg = -1;
                continue;
            }
            pendingKeyReg = static_cast<int>(rd);
            pendingKeyDisc = imm;
            continue;
        }
        if ((insn & 0xFFFFFC00U) == 0xDAC11800U) {  // autda xd, xn
            if (pendingKeyReg >= 0 && static_cast<int>(rn) == pendingKeyReg) {
                vtableReg = static_cast<int>(rd);
                vptrDisc = pendingKeyDisc;
                offset.reset();
                movOffset.reset();
            }
            pendingKeyReg = -1;
            continue;
        }
        if (vtableReg < 0) continue;
        if ((insn & 0xFFE00C00U) == 0xF8400C00U && static_cast<int>(rn) == vtableReg) {
            // ldr xt, [xn, #imm9]!
            std::int64_t imm9 = static_cast<std::int64_t>((insn >> 12) & 0x1FFU);
            if (imm9 & 0x100) imm9 -= 0x200;
            offset = static_cast<std::uint64_t>(imm9);
            continue;
        }
        if ((insn & 0xFFE00000U) == 0xD2800000U) {  // movz xd, #imm16 (hw=0)
            if (rd == 17U) movOffset = (insn >> 5) & 0xFFFFU;
            continue;
        }
        if ((insn & 0xFFE0FC00U) == 0x8B000000U && static_cast<int>(rd) == vtableReg &&
            static_cast<int>(rn) == vtableReg && ((insn >> 16) & 31U) == 17U && movOffset) {
            offset = *movOffset;  // add xV, xV, x17
            continue;
        }
        if ((insn & 0xFFC00000U) == 0xF9400000U && static_cast<int>(rn) == vtableReg) {
            // ldr xt, [xV, #imm12*8]
            const std::uint64_t imm = ((insn >> 10) & 0xFFFU) * 8U;
            offset = offset.value_or(0) + imm;
            continue;
        }
    }
    return calls;
}

}  // namespace dyld_api_abi_detail

inline DyldApiAbi derive_dyld_api_abi(const SharedCache& cache) {
    static constexpr const char* libdyld = "/usr/lib/system/libdyld.dylib";
    constexpr std::uint64_t scanBytes = 0x600;
    std::vector<std::string> missing;
    std::optional<std::uint64_t> vptr;
    const auto callsOf = [&](const char* symbol) {
        std::vector<dyld_api_abi_detail::ScannedCall> calls;
        const auto resolved = cache.resolveExport(libdyld, symbol);
        if (!resolved || resolved->guest_address < 0) {
            missing.push_back(std::string(symbol) + " (export not found)");
            return calls;
        }
        calls = dyld_api_abi_detail::scan_virtual_calls(
            cache.bytes(cache.originalAddress(resolved->guest_address), scanBytes));
        if (calls.empty()) missing.push_back(std::string(symbol) + " (no APIs virtual call)");
        return calls;
    };
    const auto take = [&](const char* symbol, DyldApiCall& out,
                          const DyldApiCall* exclude = nullptr) {
        for (const auto& scanned : callsOf(symbol)) {
            if (exclude && scanned.call.offset == exclude->offset &&
                scanned.call.discriminator == exclude->discriminator) {
                continue;
            }
            if (vptr && *vptr != scanned.vptrDiscriminator) {
                missing.push_back(std::string(symbol) + " (inconsistent vptr discriminator)");
                return;
            }
            vptr = scanned.vptrDiscriminator;
            out = scanned.call;
            return;
        }
    };
    DyldApiAbi abi;
    take("__dyld_call_with_writable_tpro_memory", abi.writableTpro);
    take("__dyld_initializer", abi.initialize, &abi.writableTpro);
    take("_dyld_get_base_platform", abi.basePlatform);
    take("_dyld_get_active_platform", abi.activePlatform);
    take("_dyld_is_simulator_platform", abi.simulatorPlatform);
    take("__dyld_register_for_bulk_image_loads", abi.bulkImageLoads);
    take("__dyld_shared_cache_real_path", abi.sharedCacheRealPath);
    take("__dyld_shared_cache_contains_path", abi.sharedCacheContainsPath);
    take("_dlopen_from", abi.dlopenFrom);
    take("_dyld_get_program_sdk_version", abi.programSdkVersion);
    take("_dyld_get_program_min_os_version", abi.programMinOsVersion);
    if (!missing.empty() || !vptr) {
        std::ostringstream message;
        message << "Unsupported dyld API ABI for shared-cache UUID ";
        for (const auto byte : cache.uuid()) {
            char hex[3];
            std::snprintf(hex, sizeof hex, "%02x", byte);
            message << hex;
        }
        message << "; underivable libdyld facts:";
        if (!vptr) message << " APIs vptr discriminator;";
        for (const auto& item : missing) message << ' ' << item << ';';
        throw std::runtime_error(message.str());
    }
    abi.vptrDiscriminator = *vptr;
    if (const char* trace = std::getenv("ARM64_METAL_TRACE_DYLD_API_ABI");
        trace != nullptr && trace[0] == '1') {
        const std::pair<const char*, const DyldApiCall*> rows[] = {
            {"initialize", &abi.initialize}, {"writableTpro", &abi.writableTpro},
            {"basePlatform", &abi.basePlatform}, {"activePlatform", &abi.activePlatform},
            {"simulatorPlatform", &abi.simulatorPlatform},
            {"bulkImageLoads", &abi.bulkImageLoads},
            {"sharedCacheRealPath", &abi.sharedCacheRealPath},
            {"sharedCacheContainsPath", &abi.sharedCacheContainsPath},
            {"dlopenFrom", &abi.dlopenFrom}, {"programSdkVersion", &abi.programSdkVersion},
            {"programMinOsVersion", &abi.programMinOsVersion}};
        std::fprintf(stderr, "arm64-metal dyld-api vptr-disc=0x%llx\n",
                     static_cast<unsigned long long>(abi.vptrDiscriminator));
        for (const auto& [name, call] : rows) {
            std::fprintf(stderr, "arm64-metal dyld-api %s slot=0x%llx disc=0x%llx\n", name,
                         static_cast<unsigned long long>(call->offset),
                         static_cast<unsigned long long>(call->discriminator));
        }
    }
    for (const auto* call : {&abi.initialize, &abi.writableTpro, &abi.basePlatform,
                             &abi.activePlatform, &abi.simulatorPlatform, &abi.bulkImageLoads,
                             &abi.sharedCacheRealPath, &abi.sharedCacheContainsPath,
                             &abi.dlopenFrom, &abi.programSdkVersion,
                             &abi.programMinOsVersion}) {
        if (call->offset % 8 != 0 || call->offset > 0x10000) {
            throw std::runtime_error("Derived dyld APIs vtable slot is not a pointer slot");
        }
        abi.vtableBytes = std::max(abi.vtableBytes, call->offset + 8);
    }
    return abi;
}

}  // namespace arm64metal
