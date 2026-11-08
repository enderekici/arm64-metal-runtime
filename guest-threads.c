#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int stage;
static _Atomic unsigned counter;
static sigset_t blocked;
static volatile sig_atomic_t delivered;
static uintptr_t main_tsd;
static uintptr_t main_mach_thread;

static uintptr_t thread_pointer(void) {
    uintptr_t pointer;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(pointer));
    return pointer;
}

static int valid_tsd(uintptr_t pointer) {
    const uintptr_t *slots = (const uintptr_t *)pointer;
    return pointer && slots[0] == (uintptr_t)pthread_self() && slots[1] == (uintptr_t)&errno;
}

static void handler(int signal) {
    delivered = signal;
    const struct timespec pause = {0, 1000000};
    (void)nanosleep(&pause, 0);
}

static void *worker(void *argument) {
    uintptr_t tsd = thread_pointer();
    if (tsd == main_tsd || !valid_tsd(tsd)) return (void *)20;
    uintptr_t mach_thread = ((const uintptr_t *)tsd)[3];
    if (!(mach_thread & 1) || mach_thread == main_mach_thread) return (void *)22;
    if (pthread_mutex_lock(&lock)) return (void *)1;
    if (pthread_mutex_trylock(&lock) != EBUSY) return (void *)2;
    if (pthread_sigmask(SIG_UNBLOCK, &blocked, 0) || delivered) return (void *)5;
    sigset_t blocked_signal;
    sigemptyset(&blocked_signal);
    sigaddset(&blocked_signal, SIGUSR2);
    if (pthread_sigmask(SIG_BLOCK, &blocked_signal, 0) || kill(getpid(), SIGUSR2)) return (void *)7;
    if (atomic_fetch_add(&counter, 2) != 11) return (void *)6;
    errno = 19;
    stage = 1;
    pthread_cond_signal(&condition);
    while (stage != 2) {
        if (pthread_cond_wait(&condition, &lock)) return (void *)3;
    }
    if (thread_pointer() != tsd || !valid_tsd(tsd)) return (void *)21;
    if (((const uintptr_t *)tsd)[3] != mach_thread) return (void *)23;
    if (errno != 19 || pthread_mutex_unlock(&lock)) return (void *)4;
    write(STDOUT_FILENO, "worker\n", 7);
    pthread_exit(argument);
}

int main(void) {
    main_tsd = thread_pointer();
    if (!valid_tsd(main_tsd)) return 20;
    main_mach_thread = ((const uintptr_t *)main_tsd)[3];
    if (!(main_mach_thread & 1)) return 22;
    pthread_t thread;
    void *result;
    struct sigaction action = {0};
    action.sa_handler = handler;
    sigaddset(&blocked, SIGUSR1);
    if (sigaction(SIGUSR1, &action, 0) || pthread_sigmask(SIG_BLOCK, &blocked, 0)
        || raise(SIGUSR1)) return 7;
    if (sigaction(SIGUSR2, &action, 0)) return 14;
    atomic_store(&counter, 11);
    errno = 11;
    if (pthread_mutex_lock(&lock) || pthread_create(&thread, 0, worker, (void *)42)) return 1;
    struct timespec pause = {1, 0}, remaining = {0, 0};
    if (pthread_mutex_unlock(&lock) || nanosleep(&pause, &remaining) != -1 || errno != EINTR
        || remaining.tv_sec < 0 || remaining.tv_sec >= pause.tv_sec
        || (remaining.tv_sec == 0 && remaining.tv_nsec == 0) || pthread_mutex_lock(&lock) || stage != 1
        || delivered != SIGUSR2) return 13;
    errno = 11;
    while (stage != 1) if (pthread_cond_wait(&condition, &lock)) return 2;
    if (errno != 11 || pthread_equal(thread, pthread_self())) return 3;
    stage = 2;
    if (pthread_cond_signal(&condition) || pthread_mutex_unlock(&lock)) return 4;
    if (pthread_join(thread, &result) || result != (void *)42) return 5;
    if (thread_pointer() != main_tsd || !valid_tsd(main_tsd)) return 21;
    unsigned expected = 13;
    if (atomic_load(&counter) != expected || !atomic_compare_exchange_strong(&counter, &expected, 17)) return 10;
    if (delivered != SIGUSR2 || pthread_sigmask(SIG_UNBLOCK, &blocked, 0) || delivered != SIGUSR1) return 8;
    pthread_mutexattr_t attributes;
    pthread_mutex_t error_lock;
    if (pthread_mutexattr_init(&attributes) || pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK)
        || pthread_mutex_init(&error_lock, &attributes) || pthread_mutexattr_destroy(&attributes)
        || pthread_mutex_lock(&error_lock) || pthread_mutex_lock(&error_lock) != EDEADLK
        || pthread_mutex_unlock(&error_lock) || pthread_mutex_destroy(&error_lock)) return 9;
    struct timespec timeout = {0, 1000000};
    if (pthread_mutex_lock(&lock) || pthread_cond_timedwait_relative_np(&condition, &lock, &timeout) != ETIMEDOUT
        || pthread_mutex_trylock(&lock) != EBUSY || pthread_mutex_unlock(&lock)) return 11;
    const struct timespec expired = {0, 0};
    if (pthread_mutex_lock(&lock) || pthread_cond_timedwait(&condition, &lock, &expired) != ETIMEDOUT
        || pthread_mutex_trylock(&lock) != EBUSY || pthread_mutex_unlock(&lock)) return 12;
    if (pthread_cond_destroy(&condition) || pthread_mutex_destroy(&lock)) return 6;
    write(STDOUT_FILENO, "joined\n", 7);
    return 0;
}
