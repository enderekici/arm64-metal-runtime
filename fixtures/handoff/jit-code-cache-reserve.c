#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    const long queried_page_size = sysconf(_SC_PAGESIZE);
    if (queried_page_size <= 0) return 1;
    const size_t page_size = (size_t)queried_page_size;
    const int flags = MAP_PRIVATE | MAP_ANON | MAP_JIT | MAP_NORESERVE;
    void *code = mmap(NULL, page_size, PROT_NONE, flags, -1, 0);
    if (code == MAP_FAILED) return 2;
    if (mprotect(code, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return 3;
    const uint32_t instructions[] = {0x52800540U, 0xd65f03c0U};
    pthread_jit_write_protect_np(0);
    memcpy(code, instructions, sizeof(instructions));
    sys_icache_invalidate(code, sizeof(instructions));
    pthread_jit_write_protect_np(1);
    const int result = ((int (*)(void))code)();
    if (munmap(code, page_size) != 0 || result != 42) return 4;
    puts("jit-code-cache-reserve-commit-ok result=42");
    return 0;
}
