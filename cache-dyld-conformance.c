#include <stdint.h>
#include <stdio.h>

typedef struct {
    uintptr_t kind;
    uintptr_t value;
} conformance_result;

extern conformance_result _dyld_find_protocol_conformance_on_disk(
    const void *, const void *, const void *, uint32_t);

int main(void) {
    const conformance_result result = _dyld_find_protocol_conformance_on_disk(
        (const void *)main, (const void *)main, (const void *)main, 0);
    printf("%lu %lu\n", (unsigned long)result.kind, (unsigned long)result.value);
    return 0;
}
