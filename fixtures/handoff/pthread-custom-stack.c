#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static _Alignas(16384) unsigned char user_stack[128U * 1024U];
static uintptr_t worker_local;

static void *worker(void *unused) {
    volatile unsigned char marker = 0x6d;
    (void)unused;
    worker_local = (uintptr_t)&marker;
    return (void *)(uintptr_t)marker;
}

int main(void) {
    pthread_attr_t attributes;
    pthread_attr_t reset;
    pthread_t thread;
    void *stack_address = NULL;
    size_t stack_size = 0;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || pthread_attr_init(&attributes) != 0 ||
        pthread_attr_setstack(&attributes, user_stack, sizeof(user_stack)) != 0 ||
        pthread_attr_setguardsize(&attributes, (size_t)page_size * 2U) != 0 ||
        pthread_attr_getstack(&attributes, &stack_address, &stack_size) != 0 ||
        stack_address != user_stack || stack_size != sizeof(user_stack)) return 1;
    if (pthread_attr_setstack(&attributes, user_stack + 1, sizeof(user_stack)) != EINVAL ||
        pthread_attr_setstack(&attributes, user_stack,
                              (size_t)PTHREAD_STACK_MIN - 1U) != EINVAL) return 2;
    if (pthread_create(&thread, &attributes, worker, NULL) != 0 ||
        pthread_join(thread, NULL) != 0) return 3;
    if (worker_local < (uintptr_t)user_stack ||
        worker_local >= (uintptr_t)user_stack + sizeof(user_stack)) return 4;
    user_stack[0] = 0xa5;
    if (user_stack[0] != 0xa5) return 5;
    if (pthread_attr_init(&reset) != 0 ||
        pthread_attr_setstack(&reset, user_stack, sizeof(user_stack)) != 0 ||
        pthread_attr_setstacksize(&reset, 64U * 1024U) != 0 ||
        pthread_attr_getstack(&reset, &stack_address, &stack_size) != 0 ||
        stack_address != user_stack + 64U * 1024U ||
        stack_size != 64U * 1024U) return 6;
    pthread_attr_destroy(&reset);
    pthread_attr_destroy(&attributes);
    puts("pthread custom stack: caller range, guard behavior, and reset passed");
    return 0;
}
