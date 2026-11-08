#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

static pthread_t main_thread;
static pthread_t worker_thread;
static atomic_int worker_ready;
static atomic_int worker_release;
static uintptr_t main_top;
static uintptr_t main_sp;
static size_t main_size;
static uintptr_t worker_top;
static uintptr_t worker_sp;
static size_t worker_size;
static uintptr_t worker_main_top;
static size_t worker_main_size;

static void *worker(void *unused) {
    (void)unused;
    worker_thread = pthread_self();
    __asm__ volatile("mov %0, sp" : "=r"(worker_sp));
    worker_top = (uintptr_t)pthread_get_stackaddr_np(worker_thread);
    worker_size = pthread_get_stacksize_np(worker_thread);
    worker_main_top = (uintptr_t)pthread_get_stackaddr_np(main_thread);
    worker_main_size = pthread_get_stacksize_np(main_thread);
    atomic_store(&worker_ready, 1);
    while (!atomic_load(&worker_release)) sched_yield();
    return 0;
}

int main(int argc, char **argv) {
    const int check_sysctl = argc == 2 && strcmp(argv[1], "--check-sysctl") == 0;
    if (argc > 2 || (argc == 2 && !check_sysctl)) return 4;
    main_thread = pthread_self();
    __asm__ volatile("mov %0, sp" : "=r"(main_sp));
    main_top = (uintptr_t)pthread_get_stackaddr_np(main_thread);
    main_size = pthread_get_stacksize_np(main_thread);
    pthread_t thread;
    if (pthread_create(&thread, 0, worker, 0) != 0) return 1;
    while (!atomic_load(&worker_ready)) sched_yield();

    uintptr_t queried_worker_top = (uintptr_t)pthread_get_stackaddr_np(thread);
    size_t queried_worker_size = pthread_get_stacksize_np(thread);
    uintptr_t user_stack_top = 0;
    size_t user_stack_length = sizeof(user_stack_top);
    int sysctl_result = 0;
    if (check_sysctl) {
        int mib[] = {CTL_KERN, KERN_USRSTACK64};
        sysctl_result = sysctl(mib, 2, &user_stack_top, &user_stack_length, 0, 0);
    }

    atomic_store(&worker_release, 1);
    if (pthread_join(thread, 0) != 0) return 2;
    const int invalid = main_thread == 0 || worker_thread == 0 ||
        main_thread == worker_thread ||
        main_top == 0 || main_size == 0 || main_top < main_size ||
        main_sp < main_top - main_size ||
        main_sp >= main_top || worker_top == 0 || worker_size == 0 ||
        worker_top < worker_size ||
        worker_sp < worker_top - worker_size || worker_sp >= worker_top ||
        queried_worker_top == 0 ||
        queried_worker_size == 0 || worker_top != queried_worker_top ||
        worker_size != queried_worker_size || worker_main_top != main_top ||
        worker_main_size != main_size || (check_sysctl &&
            (user_stack_top != main_top || sysctl_result != 0 ||
             user_stack_length != sizeof(user_stack_top)));
    if (invalid) return 3;
    puts(check_sysctl ? "pthread stack and sysctl metadata ok"
                      : "pthread stack metadata ok");
    return 0;
}
