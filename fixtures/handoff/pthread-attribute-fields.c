#include <pthread.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static volatile int worker_ran;

static void *worker(void *unused) {
    uint8_t stack_probe[4096];
    (void)unused;
    stack_probe[0] = 0x5a;
    stack_probe[sizeof(stack_probe) - 1] = 0xa5;
    worker_ran = stack_probe[0] == 0x5a && stack_probe[sizeof(stack_probe) - 1] == 0xa5;
    return NULL;
}

int main(void) {
    pthread_attr_t attributes;
    size_t stack_size = 0;
    size_t guard_size = 0;
    int detach_state = -1;
    pthread_t thread;

    if (pthread_attr_init(&attributes) != 0 ||
        pthread_attr_getstacksize(&attributes, &stack_size) != 0 ||
        pthread_attr_getguardsize(&attributes, &guard_size) != 0 ||
        pthread_attr_getdetachstate(&attributes, &detach_state) != 0 ||
        stack_size != 512U * 1024U || guard_size != 16U * 1024U ||
        detach_state != PTHREAD_CREATE_JOINABLE) return 1;
    if (pthread_attr_setstacksize(&attributes, 0) != EINVAL) return 2;
    if (pthread_attr_setstacksize(&attributes, 64U * 1024U) != 0 ||
        pthread_attr_setguardsize(&attributes, 0) != 0 ||
        pthread_attr_setdetachstate(&attributes, 0) != EINVAL ||
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) != 0) return 3;
    if (pthread_attr_getstacksize(&attributes, &stack_size) != 0 ||
        pthread_attr_getguardsize(&attributes, &guard_size) != 0 ||
        pthread_attr_getdetachstate(&attributes, &detach_state) != 0 ||
        stack_size != 64U * 1024U || guard_size != 0 ||
        detach_state != PTHREAD_CREATE_DETACHED) return 4;
    if (pthread_create(&thread, &attributes, worker, NULL) != 0) return 5;
    for (int attempt = 0; attempt < 100000 && !worker_ran; ++attempt) sched_yield();
    if (!worker_ran) return 6;
    if (pthread_attr_destroy(&attributes) != 0) return 7;
    if (pthread_create(&thread, NULL, worker, NULL) != 0 ||
        pthread_join(thread, NULL) != 0) return 8;
    puts("pthread attrs: defaults, stack size, detach, and worker execution passed");
    return 0;
}
