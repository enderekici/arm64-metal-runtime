#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Signal queue needs lock-free atomics");

static _Atomic uint32_t pending_signals;

static void enqueue_signal(int number) {
    atomic_fetch_or_explicit(&pending_signals, UINT32_C(1) << (number - 1), memory_order_relaxed);
}

int guest_signal_listen(int number, int ignore, int restart) {
    switch (number) {
    case SIGHUP: case SIGINT: case SIGQUIT: case SIGPIPE: case SIGALRM: case SIGTERM:
    case SIGUSR1: case SIGUSR2:
        break;
    default:
        return 0;
    }
    struct sigaction action = {0};
    action.sa_handler = ignore ? SIG_IGN : enqueue_signal;
    action.sa_flags = restart ? SA_RESTART : 0;
    if (sigaction(number, &action, 0) == -1) return -1;
    if (ignore) atomic_fetch_and_explicit(&pending_signals, ~(UINT32_C(1) << (number - 1)), memory_order_relaxed);
    return 0;
}

uint32_t guest_signal_take(void) {
    return atomic_exchange_explicit(&pending_signals, 0, memory_order_relaxed);
}

struct dyld_build_version {
    uint32_t platform;
    uint32_t version;
};

extern _Bool dyld_version_token_at_least(uint64_t token, struct dyld_build_version request)
    __attribute__((weak_import));

int guest_version_token_at_least(uint64_t token, uint64_t request) {
    struct dyld_build_version version = {(uint32_t)request, (uint32_t)(request >> 32)};
    if (dyld_version_token_at_least)
        return dyld_version_token_at_least(token, version);
    if (version.platform == UINT32_MAX)
        return -1;
    return (uint32_t)token != 0 && (uint32_t)token == version.platform &&
        (uint32_t)(token >> 32) >= version.version;
}

int guest_shm_open(const char *name, int flags, int mode) {
    if (flags & O_CREAT)
        return shm_open(name, flags, mode);
    return shm_open(name, flags);
}

extern const char *_dyld_get_objc_selector(const char *name) __attribute__((weak_import));
extern const void *_dyld_get_shared_cache_range(size_t *length) __attribute__((weak_import));
extern _Bool _dyld_get_shared_cache_uuid(uint8_t *uuid) __attribute__((weak_import));

int guest_cached_selector(const char *name, uint64_t *offset, uint8_t *uuid) {
    if (!_dyld_get_objc_selector || !_dyld_get_shared_cache_range || !_dyld_get_shared_cache_uuid)
        return -1;
    const char *selector = _dyld_get_objc_selector(name);
    if (!selector)
        return 0;
    size_t length = 0;
    uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&length);
    uintptr_t address = (uintptr_t)selector;
    // Ignore hits specific to the host executable's prebuilt loader.
    if (!base || address < base || address - base >= length)
        return 0;
    if (!_dyld_get_shared_cache_uuid(uuid))
        return -1;
    *offset = address - base;
    return 1;
}

extern void _dyld_for_each_objc_class(const char *, void (^)(void *, _Bool, _Bool *))
    __attribute__((weak_import));

int guest_cached_classes(const char *name, uint64_t *offsets, int capacity, uint8_t *uuid) {
    if (!_dyld_for_each_objc_class || !_dyld_get_shared_cache_range || !_dyld_get_shared_cache_uuid)
        return -1;
    size_t length = 0;
    uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&length);
    if (!base || !_dyld_get_shared_cache_uuid(uuid)) return -1;
    __block int count = 0;
    _dyld_for_each_objc_class(name, ^(void *classPointer, _Bool loaded, _Bool *stop) {
        (void)loaded;
        uintptr_t address = (uintptr_t)classPointer;
        if (address < base || address - base >= length) return;
        if (count < capacity) offsets[count] = address - base;
        if (count == INT32_MAX) *stop = 1;
        else ++count;
    });
    return count;
}

extern const void *_dyld_find_pointer_hash_table_entry(const void *, const void *, size_t,
                                                       const void **) __attribute__((weak_import));

int guest_cached_pointer_lookup(uint64_t table, const uint64_t *keys, uint32_t count,
                                const uint8_t *uuid, uint64_t *result) {
    if (!_dyld_find_pointer_hash_table_entry || !_dyld_get_shared_cache_range || !_dyld_get_shared_cache_uuid)
        return -1;
    uint8_t host_uuid[16];
    if (!_dyld_get_shared_cache_uuid(host_uuid) || memcmp(uuid, host_uuid, sizeof(host_uuid)))
        return -1;
    size_t length = 0;
    uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&length);
    const void *pointers[64];
    if (!base || table >= length || count == 0 || count > 64) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (keys[i] >= length) return 0;
        pointers[i] = (const void *)(base + keys[i]);
    }
    uintptr_t hit = (uintptr_t)_dyld_find_pointer_hash_table_entry(
        (const void *)(base + table), pointers[0], count - 1, pointers + 1);
    if (hit < base || hit - base >= length) return 0;
    *result = hit - base;
    return 1;
}

int guest_entropy(void *buffer, uint64_t size) {
    return getentropy(buffer, size);
}

extern _Bool _os_feature_enabled_impl(const char *domain, const char *feature);

int guest_feature_enabled(const char *domain, const char *feature) {
    return _os_feature_enabled_impl(domain, feature);
}

extern int __mac_syscall(const char *policy, int call, void *request);

int guest_sandbox_check_path(const char *operation, const char *path, uint64_t *result) {
    uint64_t reply[5] = {0};
    struct {
        uint64_t *reply;
        int64_t pid;
        const char *operation;
        uint64_t filter;
        const char *path;
        uint64_t flags;
    } request = {reply, getpid(), operation, 1, path, 0};
    if (__mac_syscall("Sandbox", 2, &request) == -1) return errno;
    *result = reply[0];
    return 0;
}
