#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>

typedef int (*guest_function)(void);

static void write_return(uint32_t *code, uint16_t value) {
    // movz w0, #value ; ret
    code[0] = 0x52800000U | ((uint32_t)value << 5);
    code[1] = 0xd65f03c0U;
    sys_icache_invalidate(code, 2 * sizeof(*code));
}

static int call_after_revocation(const char *mode) {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return 10;
    void *page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) return 11;
    uint32_t *code = (uint32_t *)page;
    write_return(code, 42);
    if (mprotect(page, (size_t)page_size, PROT_READ | PROT_EXEC) != 0) return 12;
    if (mode[0] == 'r') {
        if (mprotect(page, (size_t)page_size, PROT_READ | PROT_WRITE) != 0) return 13;
    } else if (munmap(page, (size_t)page_size) != 0) {
        return 14;
    }
    puts("calling revoked code");
    fflush(stdout);
    return ((guest_function)page)();
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "rw-fault") == 0 ||
                      strcmp(argv[1], "unmapped-fault") == 0)) {
        return call_after_revocation(argv[1]);
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return 10;
    errno = 0;
    void *writable_executable = mmap(NULL, (size_t)page_size,
                                     PROT_READ | PROT_WRITE | PROT_EXEC,
                                     MAP_PRIVATE | MAP_ANON, -1, 0);
    const int mmap_wx_errno = errno;
    if (writable_executable != MAP_FAILED || mmap_wx_errno != EACCES) return 9;

    void *page = mmap(NULL, (size_t)page_size * 2, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); return 11; }

    errno = 0;
    const int wx_result = mprotect(page, (size_t)page_size,
                                   PROT_READ | PROT_WRITE | PROT_EXEC);
    const int mprotect_wx_errno = errno;
    if (wx_result != -1 || mprotect_wx_errno != EACCES) return 17;

    uint32_t *code = (uint32_t *)page;
    uint32_t *neighbor = (uint32_t *)((uint8_t *)page + page_size);
    write_return(code, 42);
    write_return(neighbor, 81);
    // Protect one page of a two-page allocation. The neighboring code remains RW.
    if (mprotect(page, (size_t)page_size, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect RX"); return 12;
    }
    neighbor[2] = 0x12345678U;
    if (neighbor[2] != 0x12345678U) return 18;
    int first = ((guest_function)page)();
    if (mprotect(page, (size_t)page_size, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect RW"); return 13;
    }
    write_return(code, 73);
    if (mprotect(page, (size_t)page_size, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect RX 2"); return 14;
    }
    int second = ((guest_function)page)();
    printf("first=%d second=%d wx=%d/%d\n", first, second, mmap_wx_errno, mprotect_wx_errno);
    if (munmap(page, (size_t)page_size * 2) != 0) { perror("munmap"); return 15; }
    return first == 42 && second == 73 ? 0 : 16;
}
