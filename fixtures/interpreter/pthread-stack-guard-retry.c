#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <ucontext.h>

extern char guest_faulting_store_site[] __asm__("_guest_faulting_store_site");

static volatile uintptr_t stack_low;
static volatile uintptr_t guard_page;
static void *stack_mapping;
static size_t stack_mapping_size;
static size_t page_size;
static volatile uintptr_t first_fault_address;
static volatile uintptr_t first_fault_pc;
static volatile uint32_t guard_before_retry;
static volatile uint32_t stack_before_retry;
static volatile sig_atomic_t signal_count;
static volatile sig_atomic_t handler_error;
static volatile sig_atomic_t first_signal_number;

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
        info == NULL || context_pointer == NULL ||
        signal_count != 0) {
        handler_error = 1;
        return;
    }
    signal_count = 1;
    first_signal_number = signal_number;
    first_fault_address = (uintptr_t)info->si_addr;
    ucontext_t *context = (ucontext_t *)context_pointer;
    first_fault_pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
    if (mprotect((void *)guard_page, (size_t)sysconf(_SC_PAGESIZE),
                 PROT_READ | PROT_WRITE) != 0) {
        handler_error = 2;
        return;
    }
    guard_before_retry = *(volatile uint32_t *)(stack_low - sizeof(uint32_t));
    stack_before_retry = *(volatile uint32_t *)stack_low;
}

static void *worker(void *unused) {
    (void)unused;
    const uintptr_t stack_top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    const size_t stack_size = pthread_get_stacksize_np(pthread_self());
    if (stack_top < stack_size) return (void *)(uintptr_t)1;
    stack_low = stack_top - stack_size;
    if (stack_low != (uintptr_t)stack_mapping + page_size ||
        (stack_low & ((uintptr_t)page_size - 1U)) != 0) return (void *)(uintptr_t)2;
    guard_page = (uintptr_t)stack_mapping;

    *(volatile uint32_t *)stack_low = 0x13579bdfU;
    faulting_store((volatile uint64_t *)(stack_low - sizeof(uint32_t)),
                   0x1122334455667788ULL);
    return (void *)(uintptr_t)0;
}

int main(void) {
    const long system_page_size = sysconf(_SC_PAGESIZE);
    if (system_page_size <= 0) return 89;
    page_size = (size_t)system_page_size;
    stack_mapping_size = page_size + 1024U * 1024U;
    stack_mapping = mmap(NULL, stack_mapping_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stack_mapping == MAP_FAILED ||
        mprotect(stack_mapping, page_size, PROT_NONE) != 0) return 88;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGSEGV, &action, NULL) != 0 ||
        sigaction(SIGBUS, &action, NULL) != 0) {
        return 90;
    }
    pthread_t thread;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0 ||
        pthread_attr_setstack(&attributes, (char *)stack_mapping + page_size,
                              stack_mapping_size - page_size) != 0) return 91;
    if (pthread_create(&thread, &attributes, worker, NULL) != 0) return 91;
    (void)pthread_attr_destroy(&attributes);
    void *thread_result = NULL;
    if (pthread_join(thread, &thread_result) != 0 || thread_result != NULL) return 92;
    const uint32_t guard_after_retry =
        *(volatile uint32_t *)(stack_low - sizeof(uint32_t));
    const uint32_t stack_after_retry = *(volatile uint32_t *)stack_low;
    const uintptr_t expected_fault_address = stack_low - sizeof(uint32_t);
    if (handler_error != 0 || signal_count != 1 ||
        (first_signal_number != SIGSEGV && first_signal_number != SIGBUS) ||
        first_fault_address != expected_fault_address ||
        first_fault_pc != (uintptr_t)guest_faulting_store_site ||
        guard_before_retry != 0 || stack_before_retry != 0x13579bdfU ||
        guard_after_retry != 0x55667788U || stack_after_retry != 0x11223344U) {
        return 93;
    }
    static const char result[] = "pthread-stack-guard-retry-ok\n";
    if (write(STDOUT_FILENO, result, sizeof(result) - 1) != (ssize_t)(sizeof(result) - 1)) {
        return 94;
    }
    (void)munmap(stack_mapping, stack_mapping_size);
    return 42;
}
