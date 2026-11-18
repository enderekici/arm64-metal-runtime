#define _XOPEN_SOURCE 700

#define _DARWIN_C_SOURCE 1
#include <arm_sme.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>

static volatile sig_atomic_t caught_signal;
static volatile uint64_t handler_svcr;
static volatile uint64_t handler_tpidr2;
static volatile uint64_t handler_cpsr;
static volatile uint64_t handler_post_tpidr2;
static volatile uint64_t handler_post_svcr;
static uint8_t handler_z0[64] __attribute__((aligned(64)));
static uint8_t handler_p0[8] __attribute__((aligned(8)));
static uint8_t handler_za_row[64] __attribute__((aligned(64)));
static volatile sig_atomic_t mutation_kind;

static void handle_signal(int signal_number, siginfo_t *info, void *context) {
  (void)info;
  ucontext_t *ucontext = (ucontext_t *)context;
  caught_signal = signal_number;
  __asm__ volatile("mrs %0, svcr\n\tmrs %1, tpidr2_el0"
                   : "=r"(handler_svcr), "=r"(handler_tpidr2) : : "memory");
  __asm__ volatile("ptrue p1.d\n\tmov w12, #0\n\t"
                   "str z0, [%0]\n\tstr p0, [%1]\n\t"
                   "mova z1.d, p1/m, za0h.d[w12, 0]\n\tstr z1, [%2]"
                   : : "r"(handler_z0), "r"(handler_p0), "r"(handler_za_row)
                   : "memory", "v1");
  if (mutation_kind == 't')
    __asm__ volatile("mov x9, #0x2000\n\tmsr tpidr2_el0, x9"
                     : : : "memory", "x9");
  if (mutation_kind == 'z')
    __asm__ volatile("mov z0.b, #0x33" : : : "memory", "v0");
  if (mutation_kind == 'p')
    __asm__ volatile("mov x9, #5\n\twhilelt p0.s, xzr, x9"
                     : : : "memory", "x9");
  if (mutation_kind == 'a')
    __asm__ volatile("mov x9, #0x4567\n\tdup z2.d, x9\n\t"
                     "ptrue p1.d\n\tmov w12, #0\n\t"
                     "mova za0h.d[w12, 0], p1/m, z2.d"
                     : : : "memory", "x9", "x12", "v2");
  if (mutation_kind == 's') __asm__ volatile("smstop za" : : : "memory");
  __asm__ volatile("mrs %0, tpidr2_el0\n\tmrs %1, svcr"
                   : "=r"(handler_post_tpidr2), "=r"(handler_post_svcr)
                   : : "memory");
  handler_cpsr = ucontext->uc_mcontext->__ss.__cpsr;
}

int main(int argc, char **argv) {
  mutation_kind = argc > 1 ? argv[1][0] : 0;
  if (!__arm_has_sme()) return 77;
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = handle_signal;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGALRM, &action, NULL) != 0) return 2;

  uint64_t old_tpidr2 = 0;
  const uint64_t marker = 0x1000ULL;
  uint64_t tpidr2_immediate = 0, tpidr2_after_timer = 0;
  __asm__ volatile("mrs %0, tpidr2_el0" : "=r"(old_tpidr2));
  __asm__ volatile("msr tpidr2_el0, %0" : : "r"(marker) : "memory");
  __asm__ volatile("mrs %0, tpidr2_el0" : "=r"(tpidr2_immediate));
  struct itimerval timer = {{0, 0}, {0, 10000}};
  if (setitimer(ITIMER_REAL, &timer, NULL) != 0) return 3;
  __asm__ volatile("mrs %0, tpidr2_el0" : "=r"(tpidr2_after_timer));

  uint64_t before_svcr = 0, after_svcr = 0;
  uint64_t tpidr2_before = 0, tpidr2_after = 0;
  uint8_t z0_after[64] __attribute__((aligned(64))) = {0};
  uint8_t p0_after[8] __attribute__((aligned(8))) = {0};
  uint8_t za_row_after[64] __attribute__((aligned(64))) = {0};
  __asm__ volatile("smstart sm\n\tsmstart za\n\tmrs %0, svcr\n\t"
                   "mrs %1, tpidr2_el0\n\tptrue p0.b\n\t"
                   "mov z0.b, #0x5a\n\t"
                   "ptrue p0.b\n\tmov x9, #0x1234\n\tdup z1.d, x9\n\t"
                   "ptrue p1.d\n\tmov w12, #0\n\t"
                   "mova za0h.d[w12, 0], p1/m, z1.d"
                   : "=r"(before_svcr), "=r"(tpidr2_before)
                   : : "memory", "x9", "x12", "v0", "v1");
  while (!caught_signal) __asm__ volatile("yield" : : : "memory");
  if (mutation_kind == 's') {
    __asm__ volatile("mrs %0, svcr\n\tmrs %1, tpidr2_el0\n\t"
                     "str z0, [%2]\n\tstr p0, [%3]"
                     : "=&r"(after_svcr), "=&r"(tpidr2_after)
                     : "r"(z0_after), "r"(p0_after) : "memory");
  } else {
    __asm__ volatile("mrs %0, svcr\n\tmrs %1, tpidr2_el0\n\t"
                     "str z0, [%2]\n\tstr p0, [%3]\n\t"
                     "ptrue p1.d\n\tmov w12, #0\n\t"
                     "mova z0.d, p1/m, za0h.d[w12, 0]\n\tstr z0, [%4]"
                     : "=&r"(after_svcr), "=&r"(tpidr2_after)
                     : "r"(z0_after), "r"(p0_after), "r"(za_row_after)
                     : "memory", "x12", "v0");
  }
  __asm__ volatile("smstop za\n\tsmstop sm" : : : "memory");
  __asm__ volatile("msr tpidr2_el0, %0" : : "r"(old_tpidr2) : "memory");
  printf("signal=%d before_svcr=%llu handler_svcr=%llu after_svcr=%llu "
         "write=%016llx after_timer=%016llx tpidr2_before=%016llx "
         "handler_tpidr2=%016llx handler_post_tpidr2=%016llx "
         "handler_post_svcr=%llu tpidr2_after=%016llx "
         "mutation=%c handler_cpsr=%08llx z0_after=%02x,%02x,%02x,%02x "
         "p0_after=%02x,%02x "
         "za_after=%02x,%02x handler_z0=%02x,%02x,%02x,%02x "
         "handler_p0=%02x,%02x handler_za=%02x,%02x\n",
         (int)caught_signal, (unsigned long long)before_svcr,
         (unsigned long long)handler_svcr, (unsigned long long)after_svcr,
         (unsigned long long)tpidr2_immediate,
         (unsigned long long)tpidr2_after_timer,
         (unsigned long long)tpidr2_before,
         (unsigned long long)handler_tpidr2,
         (unsigned long long)handler_post_tpidr2,
         (unsigned long long)handler_post_svcr,
         (unsigned long long)tpidr2_after,
         mutation_kind == 0 ? '-' : (char)mutation_kind,
         (unsigned long long)handler_cpsr,
         z0_after[0], z0_after[15],
         z0_after[16], z0_after[63],
         p0_after[0], p0_after[7], za_row_after[0], za_row_after[63],
         handler_z0[0], handler_z0[15], handler_z0[16], handler_z0[63],
         handler_p0[0], handler_p0[7], handler_za_row[0], handler_za_row[63]);
  return caught_signal == SIGALRM && before_svcr == 3 && handler_svcr == 3 &&
                 (mutation_kind != 's' || handler_post_svcr == 1) &&
                 (mutation_kind != 's' || after_svcr == 1) &&
                 (mutation_kind != 't' || tpidr2_after == 0x2000) &&
                 z0_after[0] == 0x5a ? 0 : 1;
}
