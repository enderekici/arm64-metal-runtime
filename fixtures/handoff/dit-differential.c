// Native-vs-arm64-metal check of PSTATE.DIT (MRS/MSR DIT as used by
// libcorecrypto) and its independence from FPSR.
// Build: xcrun clang -arch arm64 -O1 -fno-stack-protector dit-differential.c
#include <stdint.h>
#include <stdio.h>

#define ARCH ".arch armv8.4-a\n"
static uint64_t read_dit(void) {
  uint64_t v;
  __asm__ volatile(ARCH "mrs %0, DIT" : "=r"(v));
  return v;
}
static void write_dit(uint64_t v) { __asm__ volatile(ARCH "msr DIT, %0" : : "r"(v)); }
static void dit_on(void) { __asm__ volatile(ARCH "msr DIT, #1"); }
static void dit_off(void) { __asm__ volatile(ARCH "msr DIT, #0"); }
static uint64_t read_fpsr(void) {
  uint64_t v;
  __asm__ volatile("mrs %0, FPSR" : "=r"(v));
  return v;
}
static void write_fpsr(uint64_t v) { __asm__ volatile("msr FPSR, %0" : : "r"(v)); }

int main(void) {
  printf("initial dit=%llx\n", (unsigned long long)read_dit());
  dit_on();
  printf("imm1 dit=%llx fpsr=%llx\n", (unsigned long long)read_dit(),
         (unsigned long long)read_fpsr());
  write_fpsr(~0ull);
  printf("fpsr-all dit=%llx fpsr=%llx\n", (unsigned long long)read_dit(),
         (unsigned long long)read_fpsr());
  write_fpsr(0);
  dit_off();
  printf("imm0 dit=%llx fpsr=%llx\n", (unsigned long long)read_dit(),
         (unsigned long long)read_fpsr());
  write_dit(~0ull);
  printf("reg-all dit=%llx fpsr=%llx\n", (unsigned long long)read_dit(),
         (unsigned long long)read_fpsr());
  write_dit(~(1ull << 24));
  printf("reg-not24 dit=%llx\n", (unsigned long long)read_dit());
  // libcorecrypto pattern: save, force on, restore.
  write_dit(1ull << 24);
  uint64_t saved = read_dit();
  dit_on();
  write_dit(saved & ~(1ull << 24));
  printf("restore dit=%llx\n", (unsigned long long)read_dit());
  fflush(stdout);
  return 0;
}
