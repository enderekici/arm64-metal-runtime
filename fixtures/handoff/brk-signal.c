#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ucontext.h>

static volatile sig_atomic_t signal_number;
static volatile sig_atomic_t signal_code;
static volatile uintptr_t signal_address;
static volatile uintptr_t saved_pc;

static void handle_trap(int number, siginfo_t *info, void *context) {
  ucontext_t *machine_context = context;
  signal_number = number;
  signal_code = info->si_code;
  signal_address = (uintptr_t)info->si_addr;
  saved_pc = machine_context->uc_mcontext->__ss.__pc;
  machine_context->uc_mcontext->__ss.__pc += 4;
}

int main(void) {
  struct sigaction action = {0};
  action.sa_sigaction = handle_trap;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGTRAP, &action, 0) != 0) return 2;

  __asm__ volatile("brk #0x1234" ::: "memory");

  printf("signal=%d code=%d address_matches_pc=%d resumed=1\n",
         signal_number, signal_code, signal_address == saved_pc);
  return signal_number != SIGTRAP || signal_code != 0 ||
         signal_address != saved_pc;
}
