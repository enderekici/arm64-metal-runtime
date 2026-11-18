#pragma once

// Darwin-backed host services used by the guest process runtime. The namespaced
// API is for C++ callers; the C declarations retain the ABI used by Swift and
// existing POSIX-facing code. Buffer pointers are host buffers unless noted.
#include <cstdint>
#include <string>

namespace arm64metal {

struct GuestSigningPolicy {
    bool restricted = false;
    std::uint32_t status = 0;
};

// Query signing metadata for the guest executable path, not the launcher.
[[nodiscard]] GuestSigningPolicy guest_signing_policy(const std::string& guestPath,
                                                       bool legacyRestriction);

// Installs a supported host signal forwarder. Unsupported signals are ignored
// and return 0; OS registration errors return -1. Handler work is limited to a
// lock-free atomic OR so it remains async-signal-safe.
int guest_signal_listen(std::int32_t number, int ignore, int restart);
[[nodiscard]] std::uint32_t guest_signal_take() noexcept;

// Return 1 for a matching dyld version token, 0 for mismatch, and -1 if the
// dyld query is unavailable and no local token fallback can answer.
int guest_version_token_at_least(std::uint64_t token, std::uint64_t request);
int guest_shm_open(const char* name, int flags, int mode);

// Shared-cache lookups return 1 for a hit, 0 for a miss, and -1 when the host
// dyld API or UUID validation is unavailable. Offsets are relative to the
// shared-cache base; caller-provided arrays have `capacity` elements.
int guest_cached_selector(const char* name, std::uint64_t* offset, std::uint8_t* uuid);
int guest_cached_classes(const char* name, std::uint64_t* offsets, int capacity,
                         std::uint8_t* uuid);
int guest_cached_pointer_lookup(std::uint64_t table, const std::uint64_t* keys,
                                std::uint32_t count, const std::uint8_t* uuid,
                                std::uint64_t* result);

int guest_entropy(void* buffer, std::uint64_t size);
int guest_feature_enabled(const char* domain, const char* feature);
inline constexpr std::uint64_t sandboxSyscallNumberFilter = 65;
// Returns 0 on success or the errno value from the host sandbox check.
int guest_sandbox_check_persona(std::uint64_t* result);
int guest_sandbox_check_path(const char* operation, const char* path,
                             std::uint64_t* result);
int guest_sandbox_check_write_data_status(bool emptyArgument, std::uint64_t* result);

}  // namespace arm64metal

// Stable C ABI for the existing Swift/Objective-C bridge boundary.
extern "C" {
int guest_signal_listen(int number, int ignore, int restart);
std::uint32_t guest_signal_take(void);
int guest_version_token_at_least(std::uint64_t token, std::uint64_t request);
int guest_shm_open(const char* name, int flags, int mode);
int guest_cached_selector(const char* name, std::uint64_t* offset, std::uint8_t* uuid);
int guest_cached_classes(const char* name, std::uint64_t* offsets, int capacity,
                         std::uint8_t* uuid);
int guest_cached_pointer_lookup(std::uint64_t table, const std::uint64_t* keys,
                                std::uint32_t count, const std::uint8_t* uuid,
                                std::uint64_t* result);
int guest_entropy(void* buffer, std::uint64_t size);
int guest_feature_enabled(const char* domain, const char* feature);
int guest_sandbox_check_path(const char* operation, const char* path,
                             std::uint64_t* result);
int guest_sandbox_check_write_data_status(bool emptyArgument, std::uint64_t* result);
}
