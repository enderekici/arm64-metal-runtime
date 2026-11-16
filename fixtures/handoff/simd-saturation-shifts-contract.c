#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char v16u8 __attribute__((vector_size(16)));
typedef struct {
  v16u8 value;
  uint64_t fpsr;
} result;
#define RUN2(name, ins)                                                        \
  __attribute__((noinline)) static result name(v16u8 a, v16u8 b,               \
                                               uint64_t seed) {                \
    result r;                                                                  \
    __asm__ volatile("msr fpsr, %x4\n\t" ins                                   \
                     " %0.16b, %2.16b, %3.16b\n\tmrs %x1, fpsr"                \
                     : "=&w"(r.value), "=&r"(r.fpsr)                           \
                     : "w"(a), "w"(b), "r"(seed)                               \
                     : "cc");                                                  \
    return r;                                                                  \
  }
RUN2(sqadd, "sqadd")
RUN2(uqadd, "uqadd")
RUN2(sqrshl, "sqrshl")
RUN2(uqrshl, "uqrshl")
static void print(const char *name, result r) {
  unsigned char b[16];
  memcpy(b, &r.value, 16);
  printf("%s", name);
  for (int i = 0; i < 16; i++)
    printf(" %02x", b[i]);
  printf(" fpsr=%016llx\n", (unsigned long long)r.fpsr);
}
int main(void) {
  const unsigned char av[16] = {0x7f, 0x80, 0x40, 0xc0, 0x01, 0xff, 0x00, 0x40,
                                0x80, 0x7f, 0x81, 0x20, 0x60, 0x9f, 0x55, 0xaa};
  const unsigned char bv[16] = {0x01, 0xff, 0x40, 0xc0, 0xff, 0x01, 0x00, 0x01,
                                0x80, 0x01, 0x7f, 0x20, 0xa0, 0x61, 0x55, 0x56};
  const unsigned char cv[16] = {0xff, 0x00, 0x01, 0x07, 0x08, 0x09, 0xf8, 0xf7,
                                0x01, 0xff, 0x08, 0xf8, 0x02, 0xfe, 0x04, 0xfc};
  v16u8 a, b, c;
  memcpy(&a, av, 16);
  memcpy(&b, bv, 16);
  memcpy(&c, cv, 16);
  for (int seed = 0; seed < 2; seed++) {
    uint64_t fpsr = seed ? (1ull << 27) : 0;
    char tag[64];
    snprintf(tag, sizeof tag, "sqadd-seed%d", seed);
    print(tag, sqadd(a, b, fpsr));
    snprintf(tag, sizeof tag, "uqadd-seed%d", seed);
    print(tag, uqadd(a, b, fpsr));
    snprintf(tag, sizeof tag, "sqrshl-seed%d", seed);
    print(tag, sqrshl(a, c, fpsr));
    snprintf(tag, sizeof tag, "uqrshl-seed%d", seed);
    print(tag, uqrshl(a, c, fpsr));
  }
  return 0;
}
