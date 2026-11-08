#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <ucontext.h>

extern char guest_faulting_store_site[] __asm__("_guest_faulting_store_site");

static volatile uintptr_t stack_low;
static volatile uintptr_t expected_fault_pc;
static volatile uint32_t stack_before_fault;
static volatile sig_atomic_t signal_count;

__attribute__((naked, noinline)) static void faulting_store(
    volatile uint64_t *address, uint64_t value) {
    __asm__ volatile(
        ".globl _guest_faulting_store_site\n"
        "_guest_faulting_store_site:\n"
        "str x1, [x0]\n"
        "ret\n");
}

static void fault_handler(int signal_number, siginfo_t *info, void *context_pointer) {
    if ((signal_number != SIGSEGV && signal_number != SIGBUS) ||
        info == NULL || context_pointer == NULL || signal_count != 0) {
        _exit(93);
    }
    signal_count = 1;
    const ucontext_t *context = (const ucontext_t *)context_pointer;
    const uintptr_t fault_pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
    if ((uintptr_t)info->si_addr != stack_low - sizeof(uint32_t) ||
        fault_pc != expected_fault_pc || stack_before_fault != 0x13579bdfU) {
        _exit(94);
    }
    static const char result[] = "pthread-runtime-owned-guard-fault-ok\n";
    if (write(STDOUT_FILENO, result, sizeof(result) - 1) !=
        (ssize_t)(sizeof(result) - 1)) _exit(95);
    _exit(42);
}

static void *worker(void *unused) {
    (void)unused;
    const uintptr_t stack_top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    const size_t stack_size = pthread_get_stacksize_np(pthread_self());
    const long system_page_size = sysconf(_SC_PAGESIZE);
    if (stack_top < stack_size || system_page_size <= 0) return (void *)(uintptr_t)1;
    stack_low = stack_top - stack_size;
    if ((stack_low & ((uintptr_t)system_page_size - 1U)) != 0) {
        return (void *)(uintptr_t)2;
    }
    expected_fault_pc = (uintptr_t)guest_faulting_store_site;
    stack_before_fault = 0x13579bdfU;
    *(volatile uint32_t *)stack_low = stack_before_fault;
    faulting_store((volatile uint64_t *)(stack_low - sizeof(uint32_t)),
                   0x1122334455667788ULL);
    return NULL;
}

int main(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGSEGV, &action, NULL) != 0 ||
        sigaction(SIGBUS, &action, NULL) != 0) return 90;
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL) != 0) return 91;
    void *thread_result = NULL;
    if (pthread_join(thread, &thread_result) != 0 || thread_result != NULL) return 92;
    return 96;
}
