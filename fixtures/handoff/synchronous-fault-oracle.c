#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

static volatile sig_atomic_t memory_seen;
static volatile sig_atomic_t memory_signal;
static volatile sig_atomic_t memory_code;
static volatile sig_atomic_t illegal_seen;
static unsigned char *protected_page;
static size_t page_size;

static void fault_handler(int signal, siginfo_t *info, void *context_pointer) {
    ucontext_t *context = context_pointer;
    if ((signal == SIGSEGV || signal == SIGBUS) &&
        info->si_addr == protected_page) {
        memory_seen = 1;
        memory_signal = signal;
        memory_code = info->si_code;
        if (mprotect(protected_page, page_size, PROT_READ | PROT_WRITE) != 0)
            _exit(11);
        return;
    }
    if (signal == SIGILL && info->si_code == ILL_ILLTRP &&
        info->si_addr == (void *)context->uc_mcontext->__ss.__pc) {
        illegal_seen = 1;
        context->uc_mcontext->__ss.__pc += 4;
        return;
    }
    _exit(12);
}

int main(void) {
    struct sigaction action = {0};
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGSEGV, &action, NULL) != 0 ||
        sigaction(SIGBUS, &action, NULL) != 0 ||
        sigaction(SIGILL, &action, NULL) != 0) return 1;

    const long queried_page_size = sysconf(_SC_PAGESIZE);
    if (queried_page_size <= 0) return 1;
    page_size = (size_t)queried_page_size;
    protected_page = mmap(NULL, page_size, PROT_READ,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if (protected_page == MAP_FAILED) return 2;
    *protected_page = 0x5a;
    if (!memory_seen || *protected_page != 0x5a) return 3;
    if (memory_signal != SIGBUS || memory_code != BUS_ADRALN) return 6;

    __asm__ volatile(".long 0x00000000\n\t" ::: "memory");
    if (!illegal_seen) return 4;
    if (munmap(protected_page, page_size) != 0) return 5;
    puts("sync-faults-ok");
    return 0;
}
