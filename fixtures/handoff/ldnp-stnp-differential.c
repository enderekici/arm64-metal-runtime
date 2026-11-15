// Native-vs-arm64-metal check of LDNP/STNP (GPR W/X and SIMD S/D/Q), which
// libsystem_platform and libcorecrypto use. Base registers must not change.
// Build: xcrun clang -arch arm64 -O1 -fno-stack-protector ldnp-stnp-differential.c
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t buf[256] __attribute__((aligned(16)));
static uint64_t h = 1469598103934665603ull;
static void mix(uint64_t v) { h ^= v; h *= 1099511628211ull; }
static void fill(void) { for (int i = 0; i < 256; ++i) buf[i] = (uint8_t)(i * 37 + 11); }
static void mix_buf(void) {
  uint64_t w;
  for (int i = 0; i < 256; i += 8) { memcpy(&w, buf + i, 8); mix(w); }
}

int main(void) {
  uint8_t *base = buf + 128, *b;
  uint64_t a0, a1;
  uint32_t w0, w1;

  fill();
  b = base;
  __asm__ volatile("ldnp %0, %1, [%2, #-16]" : "=&r"(a0), "=&r"(a1), "+r"(b) : : "memory");
  printf("ldnp-x %016llx %016llx base=%d\n", (unsigned long long)a0, (unsigned long long)a1,
         (int)(b - base));
  __asm__ volatile("ldnp %w0, %w1, [%2, #252]" : "=&r"(w0), "=&r"(w1) : "r"(buf - 8) : "memory");
  printf("ldnp-w-max %08x %08x\n", w0, w1);
  __asm__ volatile("ldnp %w0, %w1, [%2, #-256]" : "=&r"(w0), "=&r"(w1) : "r"(buf + 256) : "memory");
  printf("ldnp-w-min %08x %08x\n", w0, w1);

  a0 = 0x1122334455667788ull; a1 = 0x99aabbccddeeff00ull;
  __asm__ volatile("stnp %0, %1, [%2, #504]" : : "r"(a0), "r"(a1), "r"(buf - 512 + 8) : "memory");
  __asm__ volatile("stnp %w0, %w1, [%2]" : : "r"(a1), "r"(a0), "r"(base) : "memory");
  mix_buf();
  printf("stnp-gpr %016llx\n", (unsigned long long)h);

  fill();
  __asm__ volatile("ldnp q0, q1, [%0]\n stnp q1, q0, [%0, #64]\n"
                   "ldnp d2, d3, [%0, #8]\n stnp d3, d2, [%0, #-8]\n"
                   "ldnp s4, s5, [%0, #-128]\n stnp s5, s4, [%0, #120]\n"
                   : : "r"(base) : "v0", "v1", "v2", "v3", "v4", "v5", "memory");
  mix_buf();
  printf("simd %016llx\n", (unsigned long long)h);

  // Loads into the upper vector bits must be zeroed for S/D.
  uint64_t lanes[4];
  __asm__ volatile("movi v6.16b, #0xff\n movi v7.16b, #0xff\n ldnp s6, s7, [%1]\n"
                   "str q6, [%0]\n str q7, [%0, #16]\n"
                   : : "r"(lanes), "r"(base) : "v6", "v7", "memory");
  printf("zero-upper %016llx %016llx %016llx %016llx\n", (unsigned long long)lanes[0],
         (unsigned long long)lanes[1], (unsigned long long)lanes[2], (unsigned long long)lanes[3]);
  fflush(stdout);
  return 0;
}
