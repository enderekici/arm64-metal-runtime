#pragma once

#include "guest_code_policy.hpp"
#include "image_loader.hpp"
#include "process_host.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace arm64metal {

struct GuestCsopsOutcome {
    std::uint32_t error = 0;
    bool terminate = false;
};

inline constexpr std::uint32_t csOpsStatusValid = 0x1U;
inline constexpr std::uint32_t csOpsStatusHard = 0x100U;
inline constexpr std::uint32_t csOpsStatusKill = 0x200U;
inline constexpr std::uint32_t csOpsStatusRestrict = 0x800U;
inline constexpr std::uint32_t csOpsStatusInstaller = 0x8U;
inline constexpr std::uint32_t csOpsSettableMask = 0x00703000U;

// Errno the kernel returns for a pid that is neither the caller nor 0. The
// operation check precedes the process lookup, so unknown and unentitled
// operations are EPERM while the rest fail on the lookup.
inline std::uint32_t guest_csops_other_pid_error(std::uint32_t operation) {
    if (operation == 4 || operation >= 19) return EPERM;
    if (operation == 1 || operation == 2 || operation == 3 || operation == 8 ||
        operation == 9 || operation == 12 || operation == 13) return EPERM;
    return ESRCH;
}

// CS_OPS_* against the calling process, modelled on the kernel's csops_internal
// for the guest main executable's signature. `status` is the mutable
// code-signing status word.
inline GuestCsopsOutcome guest_csops_self(GuestMemory& memory, const GuestCodePolicy& policy,
                                          std::uint32_t& status, std::uint32_t operation,
                                          std::uint64_t address, std::uint64_t capacity) {
    constexpr std::size_t headerSize = 8;
    GuestCsopsOutcome outcome;
    const auto fail = [&](std::uint32_t error) {
        outcome.error = error;
        return outcome;
    };
    const auto put = [&](const std::uint8_t* bytes, std::size_t length) {
        if (address < ExecutableImage::minimum_image_address) return false;
        try {
            std::memcpy(memory.buffer(address, length, true), bytes, length);
        } catch (const std::out_of_range&) {
            return false;
        }
        return true;
    };
    // Sized output: success copies `length` bytes; a short buffer receives only
    // an eight byte {0, big-endian length} header when it can hold one.
    const auto putSized = [&](const std::uint8_t* bytes, std::size_t length) {
        if (capacity < length) {
            if (capacity >= headerSize) {
                std::uint8_t header[headerSize]{};
                const auto encoded = htonl(static_cast<std::uint32_t>(length));
                std::memcpy(header + 4, &encoded, sizeof(encoded));
                if (!put(header, headerSize)) return fail(EFAULT);
            }
            return fail(ERANGE);
        }
        if (!put(bytes, length)) return fail(EFAULT);
        return outcome;
    };
    const bool valid = policy.validation == GuestCodeValidation::valid;
    switch (operation) {
        case 0:
            if (address != 0 && !put(reinterpret_cast<const std::uint8_t*>(&status),
                                     sizeof(status))) return fail(EFAULT);
            return outcome;
        case 1:
            if ((status & csOpsStatusKill) != 0) outcome.terminate = true;
            status &= ~csOpsStatusValid;
            return outcome;
        case 2:
            status |= csOpsStatusHard;
            return outcome;
        case 3:
            status |= csOpsStatusKill;
            return outcome;
        case 4:
            return fail(EINVAL);
        case 5:
            if (capacity != policy.cdhash.size() || !valid || !policy.has_cdhash) {
                return fail(EINVAL);
            }
            if (!put(policy.cdhash.data(), policy.cdhash.size())) return fail(EFAULT);
            return outcome;
        case 6: {
            const std::uint8_t offset[8]{};
            if (!put(offset, sizeof(offset))) return fail(EFAULT);
            return outcome;
        }
        case 7:
            if (!valid) return fail(EINVAL);
            if (policy.xml_entitlements.empty()) return outcome;
            return putSized(policy.xml_entitlements.data(), policy.xml_entitlements.size());
        case 8:
            status |= csOpsStatusRestrict;
            return outcome;
        case 9: {
            if (capacity < sizeof(std::uint32_t)) return fail(ERANGE);
            if (address < ExecutableImage::minimum_image_address) return fail(EFAULT);
            std::uint32_t requested = 0;
            try {
                std::memcpy(&requested, memory.buffer(address, sizeof(requested)),
                            sizeof(requested));
            } catch (const std::out_of_range&) {
                return fail(EFAULT);
            }
            status |= requested & csOpsSettableMask;
            return outcome;
        }
        case 10:
            if (!valid || policy.code_signature.empty()) return fail(EINVAL);
            return putSized(policy.code_signature.data(), policy.code_signature.size());
        case 11:
        case 14: {
            const bool identity = operation == 11;
            if (capacity < headerSize) return fail(ERANGE);
            if (!valid) return fail(EINVAL);
            const auto& text = identity ? policy.identity : policy.team_id;
            if (text.empty()) return fail(ENOENT);
            std::vector<std::uint8_t> blob(headerSize + text.size() + 1);
            const auto encoded = htonl(static_cast<std::uint32_t>(blob.size()));
            std::memcpy(blob.data() + 4, &encoded, sizeof(encoded));
            std::memcpy(blob.data() + headerSize, text.c_str(), text.size() + 1);
            return putSized(blob.data(), blob.size());
        }
        case 12:
            status &= ~csOpsStatusInstaller;
            return outcome;
        case 13:
            return fail(ENOTSUP);
        case 15:
            return fail(EPERM);
        case 16:
            if (!valid) return fail(EINVAL);
            if (!policy.entitlements_blob_metadata_valid) {
                throw std::runtime_error(
                    "Unsupported malformed guest entitlement signature metadata");
            }
            if (policy.der_entitlements.empty() && policy.has_xml_entitlements) {
                throw std::runtime_error(
                    "Unsupported guest XML-only entitlements without DER transmutation");
            }
            if (policy.der_entitlements.empty()) return outcome;
            return putSized(policy.der_entitlements.data(), policy.der_entitlements.size());
        case 17: {
            if (!valid) return fail(EINVAL);
            const std::uint8_t category[4] = {10, 0, 0, 0};
            if (!put(category, sizeof(category))) return fail(EFAULT);
            return outcome;
        }
        default:
            return fail(EINVAL);
    }
}

}  // namespace arm64metal
