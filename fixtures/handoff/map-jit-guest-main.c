#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

int main(void) {
    volatile uint32_t* code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (code == MAP_FAILED) {
        puts("JIT_DENIED");
        return 21;
    }
    pthread_jit_write_protect_np(0);
    code[0] = 0xd2800540U;  // mov x0, #42; deliberately never executed
    pthread_jit_write_protect_np(1);
    if (code[0] != 0xd2800540U) return 22;
    puts("JIT_BYTES_READY");
    return 0;
}
