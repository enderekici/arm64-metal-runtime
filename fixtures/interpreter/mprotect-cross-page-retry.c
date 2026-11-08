#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <ucontext.h>

extern char guest_faulting_store_site[] __asm__("_guest_faulting_store_site");

static volatile uintptr_t guard_page;
static volatile uintptr_t boundary;
static volatile uintptr_t first_fault_address;
static volatile uintptr_t first_fault_pc;
static volatile uint32_t guard_before_retry;
static volatile uint32_t writable_before_retry;
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
        info == NULL || context_pointer == NULL || signal_count != 0) {
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
    guard_before_retry = *(volatile uint32_t *)(boundary - sizeof(uint32_t));
    writable_before_retry = *(volatile uint32_t *)boundary;
}

int main(void) {
    const long system_page_size = sysconf(_SC_PAGESIZE);
    if (system_page_size <= 0) return 89;
    const size_t page_size = (size_t)system_page_size;
    void *mapping = mmap(NULL, 2U * page_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) return 88;
    guard_page = (uintptr_t)mapping;
    boundary = guard_page + page_size;
    *(volatile uint32_t *)boundary = 0x13579bdfU;
    if (mprotect((void *)guard_page, page_size, PROT_NONE) != 0) return 87;

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGSEGV, &action, NULL) != 0 ||
        sigaction(SIGBUS, &action, NULL) != 0) return 90;

    faulting_store((volatile uint64_t *)(boundary - sizeof(uint32_t)),
                   0x1122334455667788ULL);
    const uint32_t guard_after_retry =
        *(volatile uint32_t *)(boundary - sizeof(uint32_t));
    const uint32_t writable_after_retry = *(volatile uint32_t *)boundary;
    if (handler_error != 0 || signal_count != 1 ||
        (first_signal_number != SIGSEGV && first_signal_number != SIGBUS) ||
        first_fault_address != boundary - sizeof(uint32_t) ||
        first_fault_pc != (uintptr_t)guest_faulting_store_site ||
        guard_before_retry != 0 || writable_before_retry != 0x13579bdfU ||
        guard_after_retry != 0x55667788U || writable_after_retry != 0x11223344U) {
        return 93;
    }
    static const char result[] = "mprotect-cross-page-retry-ok\n";
    if (write(STDOUT_FILENO, result, sizeof(result) - 1) != (ssize_t)(sizeof(result) - 1)) {
        return 94;
    }
    (void)munmap(mapping, 2U * page_size);
    return 42;
}
