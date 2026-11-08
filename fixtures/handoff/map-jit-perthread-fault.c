#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

static volatile uint32_t* jit_bytes;

static void* leave_write_mode(void* unused) {
    (void)unused;
    pthread_jit_write_protect_np(0);
    jit_bytes[0] = 0xd2800540U;
    return NULL;
}

int main(void) {
    jit_bytes = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit_bytes == MAP_FAILED) return 31;
    pthread_t worker;
    if (pthread_create(&worker, NULL, leave_write_mode, NULL) != 0) return 32;
    if (pthread_join(worker, NULL) != 0) return 33;
    puts("JIT_PARENT_RX");
    fflush(stdout);
    jit_bytes[1] = 0xd2800560U;
    puts("JIT_PARENT_WRITE_UNEXPECTED");
    return 34;
}
