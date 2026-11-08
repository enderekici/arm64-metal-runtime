#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

static _Atomic int handlers_entered;
static _Atomic int first_worker_resumed;
static _Atomic unsigned handler_threads;
static _Atomic int handler_failures;
static _Thread_local int worker_id;

static void signal_handler(int signal_number) {
    const int id = worker_id;
    if (signal_number != SIGUSR1 || id < 1 || id > 2) {
        atomic_store_explicit(&handler_failures, 1, memory_order_relaxed);
        return;
    }
    atomic_fetch_or_explicit(&handler_threads, 1U << (unsigned)(id - 1),
                             memory_order_relaxed);
    atomic_fetch_or_explicit(&handlers_entered, 1 << (id - 1), memory_order_release);
    if (id == 1) {
        while (atomic_load_explicit(&handlers_entered, memory_order_acquire) != 3) {
            __asm__ volatile("" ::: "memory");
        }
    } else {
        while (atomic_load_explicit(&first_worker_resumed, memory_order_acquire) == 0) {
            __asm__ volatile("" ::: "memory");
        }
    }
}

static void *raise_from_worker(void *argument) {
    const int id = (int)(intptr_t)argument;
    worker_id = id;
    if (raise(SIGUSR1) != 0 || worker_id != id) {
        atomic_store_explicit(&handler_failures, 1, memory_order_relaxed);
        return (void *)(intptr_t)1;
    }
    if (id == 1) atomic_store_explicit(&first_worker_resumed, 1, memory_order_release);
    return NULL;
}

int main(void) {
    struct sigaction action = {0};
    pthread_t workers[2];
    void *results[2] = {NULL, NULL};
    action.sa_handler = signal_handler;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGUSR1, &action, NULL) != 0) return 1;
    if (pthread_create(&workers[0], NULL, raise_from_worker, (void *)(intptr_t)1) != 0 ||
        pthread_create(&workers[1], NULL, raise_from_worker, (void *)(intptr_t)2) != 0)
        return 2;
    if (pthread_join(workers[0], &results[0]) != 0 ||
        pthread_join(workers[1], &results[1]) != 0) return 3;
    if (results[0] != NULL || results[1] != NULL ||
        atomic_load_explicit(&handlers_entered, memory_order_relaxed) != 3 ||
        atomic_load_explicit(&handler_threads, memory_order_relaxed) != 3U ||
        atomic_load_explicit(&handler_failures, memory_order_relaxed) != 0) return 4;
    puts("threaded-raise-signals-ok handlers=2 out-of-order-resume=1");
    return 0;
}
