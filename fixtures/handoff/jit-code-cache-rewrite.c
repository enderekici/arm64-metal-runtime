#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef int (*jit_function)(void);

static void write_return(volatile uint32_t *code, uint16_t value) {
    code[0] = 0x52800000U | ((uint32_t)value << 5U);
    code[1] = 0xd65f03c0U;
    sys_icache_invalidate((void *)code, 2U * sizeof(*code));
}

int main(int argc, char **argv) {
    const long queried_page_size = sysconf(_SC_PAGESIZE);
    if (queried_page_size <= 0) return 10;
    const size_t page_size = (size_t)queried_page_size;
    const int flags = MAP_PRIVATE | MAP_ANON | MAP_JIT | MAP_NORESERVE;

    if (argc == 2 && strcmp(argv[1], "expect-denied") == 0) {
        errno = 0;
        void *denied = mmap(NULL, page_size, PROT_NONE, flags, -1, 0);
        if (denied != MAP_FAILED || errno == 0) return 11;
        puts("jit-map-denied-without-entitlement");
        return 0;
    }
    if (argc != 1) return 12;

    volatile uint32_t *code = mmap(NULL, page_size, PROT_NONE, flags, -1, 0);
    if (code == MAP_FAILED) { perror("MAP_JIT reserve"); return 13; }
    if (mprotect((void *)code, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("MAP_JIT commit"); return 14;
    }

    pthread_jit_write_protect_np(0);
    write_return(code, 41);
    pthread_jit_write_protect_np(1);
    const int first = ((jit_function)(uintptr_t)code)();

    pthread_jit_write_protect_np(0);
    write_return(code, 73);
    pthread_jit_write_protect_np(1);
    const int second = ((jit_function)(uintptr_t)code)();

    if (munmap((void *)code, page_size) != 0) return 15;
    printf("jit-first=%d jit-second=%d rewrite=observed\n", first, second);
    return first == 41 && second == 73 ? 0 : 16;
}
