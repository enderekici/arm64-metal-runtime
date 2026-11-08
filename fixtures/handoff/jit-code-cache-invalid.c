#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    const long queried_page_size = sysconf(_SC_PAGESIZE);
    if (queried_page_size <= 0) return 1;
    const int flags = MAP_SHARED | MAP_ANON | MAP_JIT | MAP_NORESERVE;
    void *mapping = mmap(NULL, (size_t)queried_page_size, PROT_NONE, flags, -1, 0);
    if (mapping != MAP_FAILED || errno != EINVAL) {
        if (mapping != MAP_FAILED) (void)munmap(mapping, (size_t)queried_page_size);
        return 2;
    }
    puts("invalid-map-jit-combination-rejected errno=EINVAL");
    return 0;
}
