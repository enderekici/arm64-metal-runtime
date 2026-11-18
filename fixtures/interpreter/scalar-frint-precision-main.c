#include <stdint.h>
#include <stdio.h>
#include <string.h>

static double frint32z_d(double value) {
  double result;
  __asm__ volatile("frint32z %d0, %d1" : "=w"(result) : "w"(value));
  return result;
}

static double frint32x_d(double value) {
  double result;
  __asm__ volatile("frint32x %d0, %d1" : "=w"(result) : "w"(value));
  return result;
}

static double frint64z_d(double value) {
  double result;
  __asm__ volatile("frint64z %d0, %d1" : "=w"(result) : "w"(value));
  return result;
}

static double frint64x_d(double value) {
  double result;
  __asm__ volatile("frint64x %d0, %d1" : "=w"(result) : "w"(value));
  return result;
}

static float frint32z_s(float value) {
  float result;
  __asm__ volatile("frint32z %s0, %s1" : "=w"(result) : "w"(value));
  return result;
}

static float frint32x_s(float value) {
  float result;
  __asm__ volatile("frint32x %s0, %s1" : "=w"(result) : "w"(value));
  return result;
}

static float frint64z_s(float value) {
  float result;
  __asm__ volatile("frint64z %s0, %s1" : "=w"(result) : "w"(value));
  return result;
}

static float frint64x_s(float value) {
  float result;
  __asm__ volatile("frint64x %s0, %s1" : "=w"(result) : "w"(value));
  return result;
}

static uint64_t double_bits(double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static uint32_t float_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static uint64_t read_fpsr(void) {
  uint64_t value;
  __asm__ volatile("mrs %0, fpsr" : "=r"(value));
  return value;
}

static uint64_t read_fpcr(void) {
  uint64_t value;
  __asm__ volatile("mrs %0, fpcr" : "=r"(value));
  return value;
}

static void write_fpcr(uint64_t value) {
  __asm__ volatile("msr fpcr, %0" : : "r"(value));
}

static void clear_fpsr(void) {
  __asm__ volatile("msr fpsr, xzr");
}

int main(void) {
  static const uint64_t input_bits[] = {
      0x0000000000000000ULL, 0x8000000000000000ULL,
      0x3fe0000000000000ULL, 0xbfe0000000000000ULL,
      0x3ff8000000000000ULL, 0xbff8000000000000ULL,
      0x4004000000000000ULL, 0xc004000000000000ULL,
      0x41dfffffffc00000ULL, 0x41dfffffffe00000ULL,
      0x41e0000000000000ULL, 0xc1e0000000000000ULL,
      0x43e0000000000000ULL, 0xc3e0000000000000ULL,
      0x7ff8000000000000ULL, 0x7ff0000000000001ULL,
      0x7ff0000000000000ULL, 0xfff0000000000000ULL,
      0x0000000000000001ULL, 0x8000000000000001ULL,
  };
  static const uint64_t fpcr_modes[] = {
      0, 1ULL << 22, 2ULL << 22, 3ULL << 22,
      1ULL << 24, (1ULL << 24) | (1ULL << 22),
      (1ULL << 24) | (2ULL << 22), (1ULL << 24) | (3ULL << 22),
  };
  const uint64_t original_fpcr = read_fpcr();
  for (uint32_t mode = 0; mode < sizeof(fpcr_modes) / sizeof(fpcr_modes[0]); ++mode) {
    write_fpcr((original_fpcr & ~((3ULL << 22) | (1ULL << 24))) | fpcr_modes[mode]);
    for (uint32_t index = 0; index < sizeof(input_bits) / sizeof(input_bits[0]); ++index) {
      double d;
      memcpy(&d, &input_bits[index], sizeof(d));
      double d_results[4];
      uint64_t d_flags[4];
      clear_fpsr(); d_results[0] = frint32z_d(d); d_flags[0] = read_fpsr();
      clear_fpsr(); d_results[1] = frint32x_d(d); d_flags[1] = read_fpsr();
      clear_fpsr(); d_results[2] = frint64z_d(d); d_flags[2] = read_fpsr();
      clear_fpsr(); d_results[3] = frint64x_d(d); d_flags[3] = read_fpsr();
      float s = (float)d;
      float s_results[4];
      uint64_t s_flags[4];
      clear_fpsr(); s_results[0] = frint32z_s(s); s_flags[0] = read_fpsr();
      clear_fpsr(); s_results[1] = frint32x_s(s); s_flags[1] = read_fpsr();
      clear_fpsr(); s_results[2] = frint64z_s(s); s_flags[2] = read_fpsr();
      clear_fpsr(); s_results[3] = frint64x_s(s); s_flags[3] = read_fpsr();
      printf("%u %u", mode, index);
      for (uint32_t op = 0; op < 4; ++op) {
        printf(" %016llx:%llx", (unsigned long long)double_bits(d_results[op]),
               (unsigned long long)d_flags[op]);
      }
      for (uint32_t op = 0; op < 4; ++op) {
        printf(" %08x:%llx", float_bits(s_results[op]),
               (unsigned long long)s_flags[op]);
      }
      printf("\n");
    }
  }
  write_fpcr(original_fpcr);
  return 0;
}
