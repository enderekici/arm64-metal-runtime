#define _XOPEN_SOURCE 700

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <ucontext.h>

#ifdef TEST_UDF
#define TRAP_OP "udf "
#define TRAP_SIGNAL SIGILL
#define TRAP_CODE ILL_ILLTRP
#define LAST_IMMEDIATE "#0xffff"
#else
#define TRAP_OP "brk "
#define TRAP_SIGNAL SIGTRAP
#define TRAP_CODE 0
#define LAST_IMMEDIATE "#0xf001"
#endif

static volatile sig_atomic_t trap_count;
static volatile sig_atomic_t trap_signal;
static volatile sig_atomic_t trap_code;
static volatile sig_atomic_t address_pc_mismatches;
static volatile sig_atomic_t resumed_count;

static void handle_trap(int signal_number, siginfo_t *info, void *context) {
  ucontext_t *machine_context = (ucontext_t *)context;
  const uintptr_t pc = (uintptr_t)machine_context->uc_mcontext->__ss.__pc;
  ++trap_count;
  trap_signal = signal_number;
  trap_code = info->si_code;
  if ((uintptr_t)info->si_addr != pc) ++address_pc_mismatches;
  machine_context->uc_mcontext->__ss.__pc = pc + 4;
}

static void run_untaken_neighbor(void) {
  const int take_breakpoint = 0;
  __asm__ volatile(
      "cbz %w0, 2f\n"
      "1: " TRAP_OP "#1\n"
      "2:\n"
      :
      : "r"(take_breakpoint)
      : "cc", "memory");
  ++resumed_count;
}

#define DEFINE_TRAP_PROBE(name, instruction) \
  static void name(void) {                 \
    __asm__ volatile(instruction ::: "cc", "memory"); \
    ++resumed_count;                        \
  }

DEFINE_TRAP_PROBE(run_trap_0, TRAP_OP "#0")
DEFINE_TRAP_PROBE(run_trap_1, TRAP_OP "#1")
DEFINE_TRAP_PROBE(run_trap_2, TRAP_OP "#2")
DEFINE_TRAP_PROBE(run_trap_1000, TRAP_OP "#0x1000")
DEFINE_TRAP_PROBE(run_trap_f000, TRAP_OP "#0xf000")
DEFINE_TRAP_PROBE(run_trap_last, TRAP_OP LAST_IMMEDIATE)

int main(void) {
  struct sigaction action = {0};
  action.sa_sigaction = handle_trap;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(TRAP_SIGNAL, &action, NULL) != 0) return 2;

  run_untaken_neighbor();
  const sig_atomic_t untaken_count = trap_count;
  run_trap_0();
  run_trap_1();
  run_trap_2();
  run_trap_1000();
  run_trap_f000();
  run_trap_last();

  printf("untaken=%d variants=6 traps=%d signal=%d code=%d address_pc_mismatches=%d resumed=%d\n",
         (int)untaken_count, (int)trap_count, (int)trap_signal, (int)trap_code,
         (int)address_pc_mismatches, (int)resumed_count);
  return untaken_count == 0 && trap_count == 6 && trap_signal == TRAP_SIGNAL &&
                 trap_code == TRAP_CODE && address_pc_mismatches == 0 && resumed_count == 7
             ? 0
             : 1;
}
