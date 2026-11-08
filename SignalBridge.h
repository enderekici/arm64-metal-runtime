#include <stdint.h>

int guest_signal_listen(int number, int ignore, int restart);
uint32_t guest_signal_take(void);

int guest_version_token_at_least(uint64_t token, uint64_t request);
int guest_shm_open(const char *name, int flags, int mode);
int guest_cached_selector(const char *name, uint64_t *offset, uint8_t *uuid);
int guest_cached_classes(const char *name, uint64_t *offsets, int capacity, uint8_t *uuid);
int guest_cached_pointer_lookup(uint64_t table, const uint64_t *keys, uint32_t count,
                                const uint8_t *uuid, uint64_t *result);
int guest_entropy(void *buffer, uint64_t size);
int guest_feature_enabled(const char *domain, const char *feature);
int guest_sandbox_check_path(const char *operation, const char *path, uint64_t *result);
