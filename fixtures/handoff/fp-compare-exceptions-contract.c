#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  uint64_t nzcv;
  uint64_t fpsr;
} outcome;

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

static void write_fpsr(uint64_t value) {
  __asm__ volatile("msr fpsr, %0" : : "r"(value));
}

#define DEFINE_COMPARE(name, precision, compare)                               \
  __attribute__((noinline)) static outcome name(                               \
      uint##precision##_t lhs, uint##precision##_t rhs, uint64_t fpcr) {       \
    outcome result;                                                            \
    if (precision == 32) {                                                     \
      float left, right;                                                       \
      memcpy(&left, &lhs, sizeof(left));                                       \
      memcpy(&right, &rhs, sizeof(right));                                     \
      __asm__ volatile("msr fpcr, %x2\n\tmsr fpsr, xzr\n\t" compare            \
                       " %s3, %s4\n\tmrs %x0, nzcv\n\tmrs %x1, fpsr"           \
                       : "=&r"(result.nzcv), "=&r"(result.fpsr)                \
                       : "r"(fpcr), "w"(left), "w"(right)                      \
                       : "cc");                                                \
    } else {                                                                   \
      double left, right;                                                      \
      memcpy(&left, &lhs, sizeof(left));                                       \
      memcpy(&right, &rhs, sizeof(right));                                     \
      __asm__ volatile("msr fpcr, %x2\n\tmsr fpsr, xzr\n\t" compare            \
                       " %d3, %d4\n\tmrs %x0, nzcv\n\tmrs %x1, fpsr"           \
                       : "=&r"(result.nzcv), "=&r"(result.fpsr)                \
                       : "r"(fpcr), "w"(left), "w"(right)                      \
                       : "cc");                                                \
    }                                                                          \
    return result;                                                             \
  }

DEFINE_COMPARE(compare32, 32, "fcmp")
DEFINE_COMPARE(compare64, 64, "fcmp")
DEFINE_COMPARE(compare_exc32, 32, "fcmpe")
DEFINE_COMPARE(compare_exc64, 64, "fcmpe")

struct pair32 {
  const char *name;
  uint32_t lhs;
  uint32_t rhs;
};

struct pair64 {
  const char *name;
  uint64_t lhs;
  uint64_t rhs;
};

static const struct pair32 pairs32[] = {
    {"zero-sign", 0x00000000, 0x80000000},
    {"infinity", 0x7f800000, 0xff800000},
    {"quiet-nan", 0x7fc00001, 0x3f800000},
    {"signaling-nan", 0x7f800001, 0x3f800000},
    {"subnormal", 0x00000001, 0x00000000},
    {"negative-subnormal", 0x80000001, 0x80000000},
};

static const struct pair64 pairs64[] = {
    {"zero-sign", 0x0000000000000000, 0x8000000000000000},
    {"infinity", 0x7ff0000000000000, 0xfff0000000000000},
    {"quiet-nan", 0x7ff8000000000001, 0x3ff0000000000000},
    {"signaling-nan", 0x7ff0000000000001, 0x3ff0000000000000},
    {"subnormal", 0x0000000000000001, 0x0000000000000000},
    {"negative-subnormal", 0x8000000000000001, 0x8000000000000000},
};

static void print_outcome(const char *width, const char *op, const char *mode,
                          const char *pair, outcome result) {
  printf("%s %s %s %s nzcv=%llx fpsr=%llx\n", width, op, mode, pair,
         (unsigned long long)(result.nzcv >> 28),
         (unsigned long long)result.fpsr);
}

int main(void) {
  static const struct {
    const char *name;
    uint64_t fpcr;
  } modes[] = {
      {"rn", 0},
      {"rp", 1ull << 22},
      {"rm", 2ull << 22},
      {"rz", 3ull << 22},
      {"fz", 1ull << 24},
      {"dn", 1ull << 25},
      {"fz-rz", (1ull << 24) | (3ull << 22)},
  };
  const uint64_t original_fpcr = read_fpcr();
  const uint64_t original_fpsr = read_fpsr();
  for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
    for (unsigned i = 0; i < sizeof(pairs32) / sizeof(pairs32[0]); ++i) {
      print_outcome(
          "s", "fcmp", modes[mode].name, pairs32[i].name,
          compare32(pairs32[i].lhs, pairs32[i].rhs, modes[mode].fpcr));
      print_outcome(
          "s", "fcmpe", modes[mode].name, pairs32[i].name,
          compare_exc32(pairs32[i].lhs, pairs32[i].rhs, modes[mode].fpcr));
    }
    for (unsigned i = 0; i < sizeof(pairs64) / sizeof(pairs64[0]); ++i) {
      print_outcome(
          "d", "fcmp", modes[mode].name, pairs64[i].name,
          compare64(pairs64[i].lhs, pairs64[i].rhs, modes[mode].fpcr));
      print_outcome(
          "d", "fcmpe", modes[mode].name, pairs64[i].name,
          compare_exc64(pairs64[i].lhs, pairs64[i].rhs, modes[mode].fpcr));
    }
  }
  write_fpcr(original_fpcr);
  write_fpsr(original_fpsr);
  return 0;
}
