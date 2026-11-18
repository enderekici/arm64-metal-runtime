#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t convert(uint64_t bits) {
  double value;
  int32_t result;
  memcpy(&value, &bits, sizeof(value));
  __asm__ volatile("fjcvtzs %w0, %d1" : "=r"(result) : "w"(value));
  return (uint32_t)result;
}

static uint64_t read_fpcr(void) {
  uint64_t value;
  __asm__ volatile("mrs %0, fpcr" : "=r"(value));
  return value;
}

static uint64_t read_fpsr(void) {
  uint64_t value;
  __asm__ volatile("mrs %0, fpsr" : "=r"(value));
  return value;
}

static void write_fpcr(uint64_t value) {
  __asm__ volatile("msr fpcr, %0" : : "r"(value));
}

static void clear_fpsr(void) {
  __asm__ volatile("msr fpsr, xzr");
}

static uint64_t convert_discard(uint64_t bits) {
  double value;
  memcpy(&value, &bits, sizeof(value));
  clear_fpsr();
  __asm__ volatile("fjcvtzs wzr, %d0" : : "w"(value));
  return read_fpsr();
}

int main(void) {
  static const uint64_t values[] = {
      0x0000000000000000ULL, 0x8000000000000000ULL,
      0x400f333333333333ULL, 0xc00f333333333333ULL,
      0x3fefffffffffffffULL, 0xbfefffffffffffffULL,
      0x0000000000000001ULL, 0x8000000000000001ULL,
      0x41dfffffffc00000ULL, 0x41dfffffffe00000ULL,
      0x41e0000000000000ULL, 0xc1e0000000000000ULL,
      0x41f0000000000000ULL, 0xc1f0000000000000ULL,
      0x4530000000000000ULL, 0x4540000000000000ULL,
      0x7ff8000000000000ULL, 0x7ff0000000000001ULL,
      0x7ff0000000000000ULL, 0xfff0000000000000ULL,
  };
  static const uint64_t fpcr_modes[] = {
      0, 1ULL << 22, 2ULL << 22, 3ULL << 22,
      1ULL << 24, (1ULL << 24) | (1ULL << 22),
      (1ULL << 24) | (2ULL << 22), (1ULL << 24) | (3ULL << 22),
  };
  const uint64_t original_fpcr = read_fpcr();
  for (uint32_t mode = 0; mode < sizeof(fpcr_modes) / sizeof(fpcr_modes[0]); ++mode) {
    write_fpcr((original_fpcr & ~((3ULL << 22) | (1ULL << 24))) | fpcr_modes[mode]);
    for (uint32_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
      clear_fpsr();
      const uint32_t result = convert(values[index]);
      printf("%u %u %08x %llx\n", mode, index, result,
             (unsigned long long)read_fpsr());
    }
  }
  printf("discard %llx %llx\n",
         (unsigned long long)convert_discard(0x400f333333333333ULL),
         (unsigned long long)convert_discard(0x7ff8000000000000ULL));
  write_fpcr(original_fpcr);
  return 0;
}
