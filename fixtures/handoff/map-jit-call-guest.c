#include <stdint.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <stdio.h>
#include <sys/mman.h>

typedef uint64_t (*JitFunction)(void);

static int publish_code(volatile uint32_t* page, uint32_t immediate) {
    pthread_jit_write_protect_np(0);
    page[0] = 0xd2800000U | (immediate << 5U);  // mov x0, #immediate
    page[1] = 0xd65f03c0U;                      // ret
    sys_icache_invalidate((void*)page, 2U * sizeof(uint32_t));
    pthread_jit_write_protect_np(1);
    return 0;
}

int main(void) {
    volatile uint32_t* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (page == MAP_FAILED) return 41;
    if (publish_code(page, 41) != 0) return 42;
    const JitFunction function = (JitFunction)(uintptr_t)page;
    const uint64_t first = function();
    printf("JIT_RETURN_%llu\n", (unsigned long long)first);
    if (first != 41) return 43;
    if (publish_code(page, 42) != 0) return 44;
    const uint64_t second = function();
    printf("JIT_RETURN_%llu\n", (unsigned long long)second);
    if (second != 42) return 45;
    return 0;
}
