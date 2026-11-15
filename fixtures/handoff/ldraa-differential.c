// Native-vs-arm64-metal check of LDRAA/LDRAB (libc++abi is_class_type uses
// LDRAA on vtable pointers): offset forms, negative offsets, pre-index
// writeback, and both keys. Pointers are signed with PACDZA/PACDZB.
// Build: xcrun clang -arch arm64 -O1 -fno-stack-protector ldraa-differential.c
#include <stdint.h>
#include <stdio.h>

static uint64_t table[1024];

int main(void) {
  for (int i = 0; i < 1024; ++i) table[i] = 0x0101010101010101ull * (uint64_t)(i & 0xff) ^ (uint64_t)i << 40;
  uint64_t *mid = table + 512;
  uint64_t v0, v1, v2, v3, v4;
  uint64_t p, q;

  __asm__ volatile(".arch armv8.3-a\n"
                   "mov %[p], %[mid]\n pacdza %[p]\n"
                   "ldraa %[v0], [%[p]]\n"
                   "ldraa %[v1], [%[p], #8]\n"
                   "ldraa %[v2], [%[p], #4088]\n"
                   "ldraa %[v3], [%[p], #-4096]\n"
                   "ldraa %[v4], [%[p], #-8]\n"
                   : [p] "=&r"(p), [v0] "=&r"(v0), [v1] "=&r"(v1), [v2] "=&r"(v2),
                     [v3] "=&r"(v3), [v4] "=&r"(v4)
                   : [mid] "r"(mid) : "memory");
  printf("ldraa %016llx %016llx %016llx %016llx %016llx\n", (unsigned long long)v0,
         (unsigned long long)v1, (unsigned long long)v2, (unsigned long long)v3,
         (unsigned long long)v4);

  __asm__ volatile(".arch armv8.3-a\n"
                   "mov %[p], %[mid]\n pacdzb %[p]\n"
                   "ldrab %[v0], [%[p], #16]\n"
                   "ldrab %[v1], [%[p], #-512]\n"
                   : [p] "=&r"(p), [v0] "=&r"(v0), [v1] "=&r"(v1)
                   : [mid] "r"(mid) : "memory");
  printf("ldrab %016llx %016llx\n", (unsigned long long)v0, (unsigned long long)v1);

  __asm__ volatile(".arch armv8.3-a\n"
                   "mov %[q], %[mid]\n pacdza %[q]\n"
                   "ldraa %[v0], [%[q], #24]!\n"
                   : [q] "=&r"(q), [v0] "=&r"(v0)
                   : [mid] "r"(mid) : "memory");
  printf("ldraa-pre %016llx base+%lld\n", (unsigned long long)v0,
         (long long)((uint64_t *)q - mid));

  __asm__ volatile(".arch armv8.3-a\n"
                   "mov %[q], %[mid]\n pacdzb %[q]\n"
                   "ldrab %[v0], [%[q], #-40]!\n"
                   : [q] "=&r"(q), [v0] "=&r"(v0)
                   : [mid] "r"(mid) : "memory");
  printf("ldrab-pre %016llx base%lld\n", (unsigned long long)v0,
         (long long)((uint64_t *)q - mid));
  fflush(stdout);
  return 0;
}
