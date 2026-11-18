#include "darwin_bridge.hpp"

#include <atomic>
#include <cerrno>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <unistd.h>

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "Signal queue needs lock-free atomics");

namespace {
std::atomic<std::uint32_t> pending_signals{0};

void enqueue_signal(int number) noexcept {
    pending_signals.fetch_or(std::uint32_t{1} << static_cast<std::uint32_t>(number - 1),
                             std::memory_order_relaxed);
}

struct DyldBuildVersion {
    std::uint32_t platform;
    std::uint32_t version;
};

extern "C" bool dyld_version_token_at_least(std::uint64_t token, DyldBuildVersion request)
    __attribute__((weak_import));
extern "C" const char* _dyld_get_objc_selector(const char* name) __attribute__((weak_import));
extern "C" const void* _dyld_get_shared_cache_range(std::size_t* length)
    __attribute__((weak_import));
extern "C" bool _dyld_get_shared_cache_uuid(std::uint8_t* uuid) __attribute__((weak_import));
extern "C" void _dyld_for_each_objc_class(const char*, void (^)(void*, bool, bool*))
    __attribute__((weak_import));
extern "C" const void* _dyld_find_pointer_hash_table_entry(const void*, const void*,
                                                            std::size_t, const void**)
    __attribute__((weak_import));
extern "C" bool _os_feature_enabled_impl(const char* domain, const char* feature);
extern "C" int __mac_syscall(const char* policy, int call, void* request);

}  // namespace

namespace arm64metal {

GuestSigningPolicy guest_signing_policy(const std::string& guestPath,
                                        bool legacyRestriction) {
    const bool startupRestricted = legacyRestriction || ::issetugid() != 0;
    const auto fallback = GuestSigningPolicy{startupRestricted, 0};
    if (guestPath.empty()) return fallback;

    const auto* pathBytes = reinterpret_cast<const UInt8*>(guestPath.data());
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, pathBytes, static_cast<CFIndex>(guestPath.size()), false);
    if (url == nullptr) return fallback;

    SecStaticCodeRef code = nullptr;
    const auto createStatus = SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code);
    CFRelease(url);
    if (createStatus != errSecSuccess || code == nullptr) return fallback;

    CFDictionaryRef information = nullptr;
    const auto informationStatus = SecCodeCopySigningInformation(
        code, kSecCSSigningInformation, &information);
    if (informationStatus != errSecSuccess || information == nullptr) {
        CFRelease(code);
        return fallback;
    }

    const auto getUInt32 = [](CFDictionaryRef dictionary, CFStringRef key,
                              std::uint32_t& result) {
        const auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, key));
        if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) return false;
        SInt32 number = 0;
        if (!CFNumberGetValue(value, kCFNumberSInt32Type, &number)) return false;
        result = static_cast<std::uint32_t>(number);
        return true;
    };

    const auto entitlements = static_cast<CFDictionaryRef>(CFDictionaryGetValue(
        information, kSecCodeInfoEntitlementsDict));
    const auto allowValue = entitlements == nullptr ? nullptr :
        CFDictionaryGetValue(entitlements,
            CFSTR("com.apple.security.cs.allow-dyld-environment-variables"));
    const bool allowDyldEnvironment = allowValue != nullptr &&
        CFGetTypeID(allowValue) == CFBooleanGetTypeID() &&
        CFBooleanGetValue(static_cast<CFBooleanRef>(allowValue));

    std::uint32_t flags = 0;
    std::uint32_t platform = 0;
    (void)getUInt32(information, kSecCodeInfoFlags, flags);
    (void)getUInt32(information, CFSTR("platform-identifier"), platform);
    const bool restricted = startupRestricted ||
        (!allowDyldEnvironment && ((flags & 0x10800U) != 0 || platform != 0));

    std::uint32_t status = 0x02000000U;  // CS_NO_UNTRUSTED_HELPERS
    if (CFDictionaryContainsKey(information, kSecCodeInfoUnique)) {
        status |= 0x20000000U;  // CS_SIGNED
        if (SecStaticCodeCheckValidity(code, kSecCSDefaultFlags, nullptr) == errSecSuccess) {
            status |= 1U;  // CS_VALID
        }
        status |= flags & 0x00033f00U;
        status |= 0x200U;  // CS_KILL
        if ((flags & 0x10000U) != 0) {
            status |= 0x1100U;  // CS_HARD | CS_ENFORCEMENT
            const auto disableLibraryValidation = entitlements == nullptr ? nullptr :
                CFDictionaryGetValue(entitlements,
                    CFSTR("com.apple.security.cs.disable-library-validation"));
            const bool allowsLibraryValidationDisable = disableLibraryValidation != nullptr &&
                CFGetTypeID(disableLibraryValidation) == CFBooleanGetTypeID() &&
                CFBooleanGetValue(static_cast<CFBooleanRef>(disableLibraryValidation));
            if (!allowsLibraryValidationDisable) {
                status = (status & ~0x2000U) | 0x10U;  // CS_FORCED_LV
            }
        }
        if (platform != 0) status |= 0x04000000U;  // CS_PLATFORM_BINARY
    }
    CFRelease(information);
    CFRelease(code);
    return {restricted, status};
}

int guest_signal_listen(std::int32_t number, int ignore, int restart) {
    switch (number) {
        case SIGHUP:
        case SIGINT:
        case SIGQUIT:
        case SIGPIPE:
        case SIGALRM:
        case SIGTERM:
        case SIGUSR1:
        case SIGUSR2:
        case SIGINFO:
            break;
        default:
            return 0;
    }
    struct sigaction action {};
    action.sa_handler = ignore != 0 ? SIG_IGN : enqueue_signal;
    action.sa_flags = restart != 0 ? SA_RESTART : 0;
    if (sigaction(number, &action, nullptr) == -1) return -1;
    if (ignore != 0) {
        pending_signals.fetch_and(~(std::uint32_t{1} << static_cast<std::uint32_t>(number - 1)),
                                  std::memory_order_relaxed);
    }
    return 0;
}

std::uint32_t guest_signal_take() noexcept {
    return pending_signals.exchange(0, std::memory_order_relaxed);
}

int guest_version_token_at_least(std::uint64_t token, std::uint64_t request) {
    const DyldBuildVersion version{static_cast<std::uint32_t>(request),
                                  static_cast<std::uint32_t>(request >> 32U)};
    if (dyld_version_token_at_least != nullptr) {
        return dyld_version_token_at_least(token, version) ? 1 : 0;
    }
    if (version.platform == UINT32_MAX) return -1;
    return static_cast<std::uint32_t>(token) != 0 &&
                   static_cast<std::uint32_t>(token) == version.platform &&
                   static_cast<std::uint32_t>(token >> 32U) >= version.version
               ? 1
               : 0;
}

int guest_shm_open(const char* name, int flags, int mode) {
    if ((flags & O_CREAT) != 0) return shm_open(name, flags, mode);
    return shm_open(name, flags);
}

int guest_cached_selector(const char* name, std::uint64_t* offset, std::uint8_t* uuid) {
    if (_dyld_get_objc_selector == nullptr || _dyld_get_shared_cache_range == nullptr ||
        _dyld_get_shared_cache_uuid == nullptr) {
        return -1;
    }
    const char* selector = _dyld_get_objc_selector(name);
    if (selector == nullptr) return 0;
    std::size_t length = 0;
    const auto base = reinterpret_cast<std::uintptr_t>(_dyld_get_shared_cache_range(&length));
    const auto address = reinterpret_cast<std::uintptr_t>(selector);
    // Ignore hits specific to the host executable's prebuilt loader.
    if (base == 0 || address < base || address - base >= length) return 0;
    if (!_dyld_get_shared_cache_uuid(uuid)) return -1;
    *offset = static_cast<std::uint64_t>(address - base);
    return 1;
}

int guest_cached_classes(const char* name, std::uint64_t* offsets, int capacity,
                         std::uint8_t* uuid) {
    if (_dyld_for_each_objc_class == nullptr || _dyld_get_shared_cache_range == nullptr ||
        _dyld_get_shared_cache_uuid == nullptr) {
        return -1;
    }
    std::size_t length = 0;
    const auto base = reinterpret_cast<std::uintptr_t>(_dyld_get_shared_cache_range(&length));
    if (base == 0 || !_dyld_get_shared_cache_uuid(uuid)) return -1;
    __block int count = 0;
    _dyld_for_each_objc_class(name, ^(void* classPointer, bool loaded, bool* stop) {
        (void)loaded;
        const auto address = reinterpret_cast<std::uintptr_t>(classPointer);
        if (address < base || address - base >= length) return;
        if (count < capacity) offsets[count] = static_cast<std::uint64_t>(address - base);
        if (count == INT32_MAX) *stop = true;
        else ++count;
    });
    return count;
}

int guest_cached_pointer_lookup(std::uint64_t table, const std::uint64_t* keys,
                                std::uint32_t count, const std::uint8_t* uuid,
                                std::uint64_t* result) {
    if (_dyld_find_pointer_hash_table_entry == nullptr ||
        _dyld_get_shared_cache_range == nullptr || _dyld_get_shared_cache_uuid == nullptr) {
        return -1;
    }
    std::uint8_t hostUuid[16];
    if (!_dyld_get_shared_cache_uuid(hostUuid) || std::memcmp(uuid, hostUuid, sizeof(hostUuid)) != 0) {
        return -1;
    }
    std::size_t length = 0;
    const auto base = reinterpret_cast<std::uintptr_t>(_dyld_get_shared_cache_range(&length));
    const void* pointers[64];
    if (base == 0 || table >= length || count == 0 || count > 64) return 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (keys[index] >= length) return 0;
        pointers[index] = reinterpret_cast<const void*>(base + keys[index]);
    }
    const auto hit = reinterpret_cast<std::uintptr_t>(_dyld_find_pointer_hash_table_entry(
        reinterpret_cast<const void*>(base + table), pointers[0], count - 1, pointers + 1));
    if (hit < base || hit - base >= length) return 0;
    *result = static_cast<std::uint64_t>(hit - base);
    return 1;
}

int guest_entropy(void* buffer, std::uint64_t size) {
    return getentropy(buffer, static_cast<std::size_t>(size));
}

int guest_feature_enabled(const char* domain, const char* feature) {
    return _os_feature_enabled_impl(domain, feature) ? 1 : 0;
}

namespace {
int check_sandbox(std::int64_t pid, const char* operation, std::uint64_t filter,
                  std::uintptr_t argument, std::uint64_t flags, std::uint64_t* result) {
    std::uint64_t reply[5] = {};
    struct Request {
        std::uint64_t* reply;
        std::int64_t pid;
        const char* operation;
        std::uint64_t filter;
        std::uintptr_t argument;
        std::uint64_t flags;
    } request{reply, pid, operation, filter, argument, flags};
    if (__mac_syscall("Sandbox", 2, &request) == -1) return errno;
    *result = reply[0];
    return 0;
}
}  // namespace

int guest_sandbox_check_persona(std::uint64_t* result) {
    return check_sandbox(getpid(), "syscall-unix", sandboxSyscallNumberFilter, SYS_persona, 0, result);
}

int guest_sandbox_check_path(const char* operation, const char* path, std::uint64_t* result) {
    return check_sandbox(getpid(), operation, 1, reinterpret_cast<std::uintptr_t>(path), 0, result);
}

int guest_sandbox_check_write_data_status(bool emptyArgument, std::uint64_t* result) {
    return check_sandbox(1, "file-write-data", 240,
                         reinterpret_cast<std::uintptr_t>(emptyArgument ? "" : nullptr),
                         0x20000005, result);
}

}  // namespace arm64metal

extern "C" {
int guest_signal_listen(int number, int ignore, int restart) {
    return arm64metal::guest_signal_listen(number, ignore, restart);
}
std::uint32_t guest_signal_take(void) { return arm64metal::guest_signal_take(); }
int guest_version_token_at_least(std::uint64_t token, std::uint64_t request) {
    return arm64metal::guest_version_token_at_least(token, request);
}
int guest_shm_open(const char* name, int flags, int mode) {
    return arm64metal::guest_shm_open(name, flags, mode);
}
int guest_cached_selector(const char* name, std::uint64_t* offset, std::uint8_t* uuid) {
    return arm64metal::guest_cached_selector(name, offset, uuid);
}
int guest_cached_classes(const char* name, std::uint64_t* offsets, int capacity,
                         std::uint8_t* uuid) {
    return arm64metal::guest_cached_classes(name, offsets, capacity, uuid);
}
int guest_cached_pointer_lookup(std::uint64_t table, const std::uint64_t* keys,
                                std::uint32_t count, const std::uint8_t* uuid,
                                std::uint64_t* result) {
    return arm64metal::guest_cached_pointer_lookup(table, keys, count, uuid, result);
}
int guest_entropy(void* buffer, std::uint64_t size) {
    return arm64metal::guest_entropy(buffer, size);
}
int guest_feature_enabled(const char* domain, const char* feature) {
    return arm64metal::guest_feature_enabled(domain, feature);
}
int guest_sandbox_check_path(const char* operation, const char* path, std::uint64_t* result) {
    return arm64metal::guest_sandbox_check_path(operation, path, result);
}
int guest_sandbox_check_write_data_status(bool emptyArgument, std::uint64_t* result) {
    return arm64metal::guest_sandbox_check_write_data_status(emptyArgument, result);
}
}  // extern "C"
