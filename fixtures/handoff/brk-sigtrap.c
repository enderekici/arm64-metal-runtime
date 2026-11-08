#define _XOPEN_SOURCE 700

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <ucontext.h>

static volatile sig_atomic_t trap_count;
static volatile sig_atomic_t trap_signal;
static volatile sig_atomic_t trap_code;
static volatile sig_atomic_t trap_address_matches;
static volatile sig_atomic_t trap_pc_matches;
static volatile sig_atomic_t resumed;
static uintptr_t breakpoint_pc;

static void handle_trap(int signal_number, siginfo_t *info, void *context) {
  ucontext_t *machine_context = (ucontext_t *)context;
  const uintptr_t pc = (uintptr_t)machine_context->uc_mcontext->__ss.__pc;
  ++trap_count;
  trap_signal = signal_number;
  trap_code = info->si_code;
  trap_address_matches = (uintptr_t)info->si_addr == breakpoint_pc;
  trap_pc_matches = pc == breakpoint_pc;
  if (pc == breakpoint_pc) {
    machine_context->uc_mcontext->__ss.__pc = breakpoint_pc + 4;
  }
}

static void run_brk(int take_breakpoint) {
  __asm__ volatile(
      "adr %0, 1f\n"
      "cbz %w1, 2f\n"
      "1: brk #1\n"
      "2:\n"
      : "=&r"(breakpoint_pc)
      : "r"(take_breakpoint)
      : "cc", "memory");
  resumed = 1;
}

int main(void) {
  struct sigaction action = {0};
  action.sa_sigaction = handle_trap;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGTRAP, &action, NULL) != 0) return 2;

  run_brk(0);
  const sig_atomic_t untaken_count = trap_count;
  const sig_atomic_t untaken_resumed = resumed;
  resumed = 0;
  run_brk(1);

  printf("untaken=%d untaken_resumed=%d taken=%d signal=%d code=%d address_match=%d pc_match=%d resumed=%d\n",
         (int)untaken_count, (int)untaken_resumed, (int)trap_count,
         (int)trap_signal, (int)trap_code, (int)trap_address_matches,
         (int)trap_pc_matches, (int)resumed);
  return untaken_count == 0 && untaken_resumed == 1 && trap_count == 1 &&
                 trap_signal == SIGTRAP && trap_code == 0 &&
                 trap_address_matches == 1 && trap_pc_matches == 1 &&
                 resumed == 1
             ? 0
             : 1;
}
