#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile uint32_t* jit_bytes;

static void report_bus(int signal_number, siginfo_t* information, void* context) {
    (void)context;
    const int address_matches = information->si_addr == (void*)&jit_bytes[1];
    printf("JIT_SIGBUS signo=%d code=%d address_match=%d\n",
           signal_number, information->si_code, address_matches);
    fflush(stdout);
    _exit(signal_number == SIGBUS && address_matches ? 77 : 78);
}

static void* leave_write_mode(void* unused) {
    (void)unused;
    pthread_jit_write_protect_np(0);
    jit_bytes[0] = 0xd2800540U;
    return NULL;
}

int main(void) {
    jit_bytes = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit_bytes == MAP_FAILED) return 41;
    struct sigaction action = {0};
    action.sa_sigaction = report_bus;
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGBUS, &action, NULL) != 0) return 42;
    pthread_t worker;
    if (pthread_create(&worker, NULL, leave_write_mode, NULL) != 0) return 43;
    if (pthread_join(worker, NULL) != 0) return 44;
    puts("JIT_PARENT_RX");
    fflush(stdout);
    jit_bytes[1] = 0xd2800560U;
    puts("JIT_PARENT_WRITE_UNEXPECTED");
    return 45;
}
