#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

struct RawResult { uint64_t value, carry; };
static atomic_int ready, completed, handlerCount;
static int interrupted[2], maskRestored[2];

static void on_signal(int signal) {
    (void)signal;
    atomic_fetch_add(&handlerCount, 1);
}

static struct RawResult raw_sigsuspend(uint32_t mask) {
    register uint64_t x0 __asm__("x0") = mask;
    register uint64_t x1 __asm__("x1") = 0;
    register uint64_t x2 __asm__("x2") = 0;
    register uint64_t x3 __asm__("x3") = 0;
    register uint64_t x4 __asm__("x4") = 0;
    register uint64_t x5 __asm__("x5") = 0;
    register uint64_t x6 __asm__("x6") = 0;
    register uint64_t x7 __asm__("x7") = 0;
    register uint64_t x16 __asm__("x16") = 410;
    uint64_t nzcv;
    __asm__ volatile("svc #0x80\n\tmrs %8, nzcv"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5),
          "+r"(x6), "+r"(x7), "=r"(nzcv)
        : "r"(x16) : "cc", "memory");
    return (struct RawResult){x0, (nzcv >> 29U) & 1U};
}

static void *waiter(void *argument) {
    const int index = (int)(intptr_t)argument;
    sigset_t blocked, after;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &blocked, NULL);
    atomic_fetch_add(&ready, 1);

    const struct RawResult result = raw_sigsuspend(0);
    interrupted[index] = result.carry != 0 && result.value == EINTR;
    pthread_sigmask(SIG_SETMASK, NULL, &after);
    maskRestored[index] = sigismember(&after, SIGUSR1);
    atomic_fetch_add(&completed, 1);
    return NULL;
}

int main(void) {
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    action.sa_flags = SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, NULL);

    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &blocked, NULL);

    pthread_t threads[2];
    pthread_create(&threads[0], NULL, waiter, (void *)0);
    pthread_create(&threads[1], NULL, waiter, (void *)1);
    while (atomic_load(&ready) != 2) __asm__ volatile("yield");
    const struct timespec delay = {0, 50000000};
    nanosleep(&delay, NULL);

    kill(getpid(), SIGUSR1);
    while (atomic_load(&completed) != 1) __asm__ volatile("yield");
    sigaction(SIGUSR1, &action, NULL);
    kill(getpid(), SIGUSR1);
    while (atomic_load(&completed) != 2) __asm__ volatile("yield");
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);

    const int count = atomic_load(&handlerCount);
    printf("sigsuspend completed=%d interrupted=%d,%d masks=%d,%d handlers=%d\n",
           atomic_load(&completed), interrupted[0], interrupted[1],
           maskRestored[0], maskRestored[1], count);
    return !(interrupted[0] && interrupted[1] && maskRestored[0] &&
             maskRestored[1] && count == 2);
}
