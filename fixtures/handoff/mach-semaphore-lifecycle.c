#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/sync_policy.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    semaphore_t semaphore;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    int ready;
    int completed;
    int order[2];
    int status[2];
} Fixture;

typedef struct {
    Fixture *fixture;
    int id;
} Waiter;

static void *waiter_main(void *opaque) {
    Waiter *waiter = opaque;
    Fixture *fixture = waiter->fixture;
    pthread_mutex_lock(&fixture->lock);
    fixture->ready++;
    pthread_cond_broadcast(&fixture->condition);
    pthread_mutex_unlock(&fixture->lock);

    fixture->status[waiter->id] = semaphore_wait(fixture->semaphore);

    pthread_mutex_lock(&fixture->lock);
    fixture->order[fixture->completed++] = waiter->id;
    pthread_cond_broadcast(&fixture->condition);
    pthread_mutex_unlock(&fixture->lock);
    return NULL;
}

static void wait_for_count(Fixture *fixture, int *field, int expected) {
    pthread_mutex_lock(&fixture->lock);
    while (*field != expected) pthread_cond_wait(&fixture->condition, &fixture->lock);
    pthread_mutex_unlock(&fixture->lock);
}

int main(void) {
    Fixture fixture = {0};
    pthread_t threads[2];
    Waiter waiters[2];
    const int policies[] = {SYNC_POLICY_FIFO, SYNC_POLICY_REVERSED, SYNC_POLICY_LIFO};
    const int invalid_policies[] = {
        SYNC_POLICY_FIXED_PRIORITY,
        SYNC_POLICY_FIXED_PRIORITY | SYNC_POLICY_REVERSED
    };
    int results[3][2] = {{0}};
    int invalid_results[2] = {0};

    pthread_mutex_init(&fixture.lock, NULL);
    pthread_cond_init(&fixture.condition, NULL);
    for (size_t policy = 0; policy < sizeof(policies) / sizeof(policies[0]); ++policy) {
        if (semaphore_create(mach_task_self(), &fixture.semaphore, policies[policy], 0) !=
            KERN_SUCCESS) return 10;
        mach_timespec_t zero = {0, 0};
        if (semaphore_signal(fixture.semaphore) != KERN_SUCCESS ||
            semaphore_timedwait(fixture.semaphore, zero) != KERN_SUCCESS ||
            semaphore_timedwait(fixture.semaphore, zero) != KERN_OPERATION_TIMED_OUT) return 11;
        mach_timespec_t short_wait = {0, 1000000};
        if (semaphore_timedwait(fixture.semaphore, short_wait) !=
            KERN_OPERATION_TIMED_OUT) return 17;
        mach_timespec_t invalid_wait = {0, -1};
        if (semaphore_timedwait(fixture.semaphore, invalid_wait) !=
            KERN_INVALID_VALUE) return 24;
        fixture.ready = 0;
        fixture.completed = 0;
        for (int id = 0; id < 2; ++id) {
            waiters[id] = (Waiter){&fixture, id};
            if (pthread_create(&threads[id], NULL, waiter_main, &waiters[id]) != 0) return 12;
            wait_for_count(&fixture, &fixture.ready, id + 1);
            usleep(50000);
        }
        if (semaphore_signal(fixture.semaphore) != KERN_SUCCESS) return 13;
        wait_for_count(&fixture, &fixture.completed, 1);
        if (semaphore_signal(fixture.semaphore) != KERN_SUCCESS) return 14;
        wait_for_count(&fixture, &fixture.completed, 2);
        for (int id = 0; id < 2; ++id) {
            pthread_join(threads[id], NULL);
            if (fixture.status[id] != KERN_SUCCESS) return 15;
        }
        results[policy][0] = fixture.order[0];
        results[policy][1] = fixture.order[1];
        if (semaphore_destroy(mach_task_self(), fixture.semaphore) != KERN_SUCCESS) return 16;
    }
    if (results[0][0] != 0 || results[0][1] != 1 ||
        results[1][0] != 1 || results[1][1] != 0 ||
        results[2][0] != 1 || results[2][1] != 0) return 18;
    for (size_t policy = 0; policy < sizeof(invalid_policies) / sizeof(invalid_policies[0]);
         ++policy) {
        fixture.semaphore = MACH_PORT_NULL;
        invalid_results[policy] = semaphore_create(mach_task_self(), &fixture.semaphore,
                                                    invalid_policies[policy], 0);
        if (invalid_results[policy] != KERN_INVALID_ARGUMENT ||
            fixture.semaphore != MACH_PORT_NULL) return 23;
    }
    if (semaphore_create(mach_task_self(), &fixture.semaphore,
                         SYNC_POLICY_REVERSED, 0) != KERN_SUCCESS) return 19;
    fixture.ready = 0;
    fixture.completed = 0;
    waiters[0] = (Waiter){&fixture, 0};
    if (pthread_create(&threads[0], NULL, waiter_main, &waiters[0]) != 0) return 20;
    wait_for_count(&fixture, &fixture.ready, 1);
    usleep(50000);
    if (semaphore_destroy(mach_task_self(), fixture.semaphore) != KERN_SUCCESS) return 21;
    pthread_join(threads[0], NULL);
    if (fixture.status[0] != KERN_TERMINATED) return 22;
    printf("fifo=%d,%d reversed=%d,%d lifo=%d,%d invalid=%d,%d timedout=%d destroyed=%d\n",
           results[0][0], results[0][1], results[1][0], results[1][1],
           results[2][0], results[2][1], invalid_results[0], invalid_results[1],
           KERN_OPERATION_TIMED_OUT,
           fixture.status[0]);
    pthread_cond_destroy(&fixture.condition);
    pthread_mutex_destroy(&fixture.lock);
    return 0;
}
