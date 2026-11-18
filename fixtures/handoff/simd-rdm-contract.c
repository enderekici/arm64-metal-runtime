#include <stdint.h>
#include <stdio.h>

typedef uint32_t Words __attribute__((vector_size(16)));
typedef int16_t Halves __attribute__((vector_size(16)));

#define CHECK(instruction) do {                                                     \
    Words result;                                                                  \
    uint64_t fpsr;                                                                 \
    __asm__ volatile("mov v0.16b, %2.16b\n mov v1.16b, %3.16b\n"                   \
                     "mov v14.16b, %4.16b\n mov v18.16b, %4.16b\n"                  \
                     "msr fpsr, %x5\n " instruction "\n"                           \
                     "mov %0.16b, v0.16b\n mrs %x1, fpsr"                          \
                     : "=w"(result), "=r"(fpsr)                                    \
                     : "w"(d), "w"(n), "w"(m), "r"(seed)                           \
                     : "v0", "v1", "v14", "v18");                                  \
    printf("%s %08x %08x %08x %08x %08llx\n", instruction, result[0], result[1],    \
           result[2], result[3], (unsigned long long)fpsr);                         \
  } while (0)

#define BOTH(operands) do { CHECK("sqrdmlah " operands); CHECK("sqrdmlsh " operands); } while (0)

static void halves(Words d, Words n, Words m, uint64_t seed) {
  BOTH("v0.4h, v1.4h, v14.4h");
  BOTH("v0.8h, v1.8h, v14.8h");
  BOTH("h0, h1, h14");
  BOTH("v0.4h, v1.4h, v14.h[7]");
  BOTH("v0.8h, v1.8h, v14.h[5]");
  BOTH("h0, h1, v14.h[2]");
  CHECK("sqrdmlah v0.8h, v0.8h, v14.8h");
}

static void words(Words d, Words n, Words m, uint64_t seed) {
  BOTH("v0.2s, v1.2s, v14.2s");
  BOTH("v0.4s, v1.4s, v14.4s");
  BOTH("s0, s1, s14");
  BOTH("v0.2s, v1.2s, v18.s[3]");
  BOTH("v0.4s, v1.4s, v18.s[1]");
  BOTH("s0, s1, v18.s[2]");
  CHECK("sqrdmlsh v0.4s, v1.4s, v0.s[3]");
}

int main(void) {
  for (int qc = 0; qc <= 1; ++qc) {
    const uint64_t seed = 0x81u | ((uint64_t)qc << 27);
    // Positive and negative ties, adjacent values, and distinct indexed lanes.
    halves((Words)(Halves){0, 1, 2, 3, -4, -3, -2, -1},
           (Words)(Halves){1, -1, 1, -1, 3, -3, 1, -1},
           (Words)(Halves){16384, 16384, 16383, 16385, -16383, -16385, -16384, 32767}, seed);
    words((Words){0, 1, -1, 3}, (Words){1, -1, 1, -1},
          (Words){0x40000000, 0x40000000, 0x3fffffff, 0xc0000001}, seed);

    // min * min must remain unbounded until the destination is accumulated.
    // Signed extrema also exercise final saturation in opposite directions.
    const Halves hmin = {-32768, -32768, -32768, -32768, -32768, -32768, -32768, -32768};
    halves((Words)(Halves){-1, -32768, -2, -3, 32767, 1, 2, 3},
           (Words)hmin, (Words)hmin, seed);
    const Words wmin = {0x80000000, 0x80000000, 0x80000000, 0x80000000};
    words((Words){-1, 0x80000000, 0x7fffffff, 1}, wmin, wmin, seed);
  }
  return 0;
}
