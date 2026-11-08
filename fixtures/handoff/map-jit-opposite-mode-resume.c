#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>

typedef int (*YieldFunction)(void);
typedef void (*MarkJitFunction)(void);
typedef int (*JitYieldFunction)(MarkJitFunction, YieldFunction);

static volatile uint32_t* jit_bytes;
static _Atomic uint32_t worker_write_mode;
static _Atomic uint32_t main_inside_jit;

static void mark_main_inside_jit(void) {
    atomic_store_explicit(&main_inside_jit, 1U, memory_order_release);
}

static void* write_mode_worker(void* unused) {
    (void)unused;
    pthread_jit_write_protect_np(0);
    atomic_store_explicit(&worker_write_mode, 1U, memory_order_release);
    puts("JIT_WORKER_WRITE_MODE");
    fflush(stdout);
    while (atomic_load_explicit(&main_inside_jit, memory_order_acquire) == 0U) {
        (void)sched_yield();
    }
    puts("JIT_WORKER_YIELD_IN_WRITE_MODE");
    fflush(stdout);
    (void)sched_yield();
    puts("JIT_WORKER_RESUMED");
    fflush(stdout);
    return NULL;
}

int main(void) {
    jit_bytes = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit_bytes == MAP_FAILED) return 41;
    pthread_jit_write_protect_np(0);
    jit_bytes[0] = 0xa9bf7bfdU;  // stp x29, x30, [sp, #-16]!
    jit_bytes[1] = 0x910003fdU;  // mov x29, sp
    jit_bytes[2] = 0xd63f0000U;  // blr x0 (mark main inside JIT)
    jit_bytes[3] = 0xd63f0020U;  // blr x1 (yield while saved PC is in JIT)
    jit_bytes[4] = 0xa8c17bfdU;  // ldp x29, x30, [sp], #16
    jit_bytes[5] = 0xd65f03c0U;  // ret
    sys_icache_invalidate((void*)jit_bytes, 6U * sizeof(uint32_t));
    pthread_jit_write_protect_np(1);

    pthread_t worker;
    if (pthread_create(&worker, NULL, write_mode_worker, NULL) != 0) return 42;
    while (atomic_load_explicit(&worker_write_mode, memory_order_acquire) == 0U) {
        (void)sched_yield();
    }
    const JitYieldFunction invoke_yield = (JitYieldFunction)(uintptr_t)jit_bytes;
    invoke_yield(mark_main_inside_jit, sched_yield);
    puts("JIT_MAIN_JIT_RESUMED");
    fflush(stdout);
    if (pthread_join(worker, NULL) != 0) return 43;
    puts("JIT_OPPOSITE_MODE_RESUMED");
    fflush(stdout);
    return 0;
}
