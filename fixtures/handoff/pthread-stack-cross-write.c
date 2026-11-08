#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

struct worker_state {
    pthread_t thread;
    uintptr_t stack_top;
    size_t stack_size;
    uintptr_t stack_pointer;
    volatile uint64_t *slot;
};

static struct worker_state workers[2];
static atomic_int ready[2];
static atomic_int wrote[2];

static void *worker(void *argument) {
    const uintptr_t index = (uintptr_t)argument;
    const uintptr_t other = 1U - index;
    struct worker_state *self = &workers[index];
    self->thread = pthread_self();
    __asm__ volatile("mov %0, sp" : "=r"(self->stack_pointer));
    self->stack_top = (uintptr_t)pthread_get_stackaddr_np(self->thread);
    self->stack_size = pthread_get_stacksize_np(self->thread);
    if (self->stack_top < self->stack_size || self->stack_top < 64U) return (void *)1;
    self->slot = (volatile uint64_t *)(self->stack_top - 64U);
    if (self->stack_pointer < self->stack_top - self->stack_size ||
        self->stack_pointer >= self->stack_top ||
        ((uintptr_t)self->slot & (sizeof(uint64_t) - 1U)) != 0) return (void *)2;
    *self->slot = 0;
    atomic_store_explicit(&ready[index], 1, memory_order_release);
    while (!atomic_load_explicit(&ready[other], memory_order_acquire)) sched_yield();

    *workers[other].slot = index == 0 ? 0xA11CE001ULL : 0xB0B00102ULL;
    atomic_store_explicit(&wrote[index], 1, memory_order_release);
    while (!atomic_load_explicit(&wrote[other], memory_order_acquire)) sched_yield();

    const uint64_t expected = index == 0 ? 0xB0B00102ULL : 0xA11CE001ULL;
    return *self->slot == expected ? 0 : (void *)3;
}

int main(void) {
    pthread_t first, second;
    if (pthread_create(&first, 0, worker, (void *)0) != 0 ||
        pthread_create(&second, 0, worker, (void *)1) != 0) return 1;
    void *first_result = 0;
    void *second_result = 0;
    if (pthread_join(first, &first_result) != 0 ||
        pthread_join(second, &second_result) != 0) return 2;
    if (first_result != 0 || second_result != 0 ||
        workers[0].thread != first || workers[1].thread != second ||
        workers[0].stack_top == workers[1].stack_top ||
        workers[0].stack_size == 0 || workers[1].stack_size == 0) return 3;
    puts("cross-stack writes ok");
    return 0;
}
