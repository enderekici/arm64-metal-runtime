#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t v4u32 __attribute__((vector_size(16)));
typedef struct {
  v4u32 value;
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

#define DEFINE_BINARY_FP(name, instruction)                                    \
  __attribute__((noinline)) static outcome name(v4u32 lhs, v4u32 rhs,          \
                                                uint64_t fpcr) {               \
    outcome result;                                                            \
    __asm__ volatile("msr fpcr, %x2\n\tmsr fpsr, xzr\n\t" instruction          \
                     " %0.4s, %3.4s, %4.4s\n\tmrs %x1, fpsr"                   \
                     : "=&w"(result.value), "=&r"(result.fpsr)                 \
                     : "r"(fpcr), "w"(lhs), "w"(rhs)                           \
                     : "cc");                                                  \
    return result;                                                             \
  }

DEFINE_BINARY_FP(fmax, "fmax")
DEFINE_BINARY_FP(fmin, "fmin")
DEFINE_BINARY_FP(fmaxnm, "fmaxnm")
DEFINE_BINARY_FP(fminnm, "fminnm")

struct test_case {
  const char *name;
  v4u32 lhs;
  v4u32 rhs;
};

static const struct test_case cases[] = {
    {"zero-q-snan-left",
     {0x00000000, 0x80000000, 0x7fc01234, 0x7f801234},
     {0x80000000, 0x00000000, 0x3f800000, 0x3f800000}},
    {"zero-q-snan-right",
     {0x80000000, 0x00000000, 0x3f800000, 0x3f800000},
     {0x00000000, 0x80000000, 0x7fc05678, 0x7f805678}},
    {"nan-payload-choice",
     {0x7fc01234, 0x7fc05678, 0x7f801234, 0x7f805678},
     {0x7fc05678, 0x7fc01234, 0x7fc05678, 0x7fc01234}},
};

static void print_result(const char *operation, const char *mode,
                         const char *test_name, outcome result) {
  printf("%s %s %s %08x %08x %08x %08x fpsr=%llx\n", operation, mode, test_name,
         result.value[0], result.value[1], result.value[2], result.value[3],
         (unsigned long long)result.fpsr);
}

int main(void) {
  static const struct {
    const char *name;
    uint64_t fpcr;
  } modes[] = {{"rn", 0},
               {"fz", 1ull << 24},
               {"dn", 1ull << 25},
               {"fz-dn", (1ull << 24) | (1ull << 25)}};
  const uint64_t original_fpcr = read_fpcr();
  const uint64_t original_fpsr = read_fpsr();
  for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      print_result("fmax", modes[mode].name, cases[i].name,
                   fmax(cases[i].lhs, cases[i].rhs, modes[mode].fpcr));
      print_result("fmin", modes[mode].name, cases[i].name,
                   fmin(cases[i].lhs, cases[i].rhs, modes[mode].fpcr));
      print_result("fmaxnm", modes[mode].name, cases[i].name,
                   fmaxnm(cases[i].lhs, cases[i].rhs, modes[mode].fpcr));
      print_result("fminnm", modes[mode].name, cases[i].name,
                   fminnm(cases[i].lhs, cases[i].rhs, modes[mode].fpcr));
    }
  }
  __asm__ volatile("msr fpcr, %0" : : "r"(original_fpcr));
  __asm__ volatile("msr fpsr, %0" : : "r"(original_fpsr));
  return 0;
}
