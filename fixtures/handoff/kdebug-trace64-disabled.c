#include <errno.h>
#include <stdint.h>
#include <sys/syscall.h>

#if SYS_kdebug_trace64 != 179 || SYS_write_nocancel != 397
#error "Unexpected installed Darwin syscall numbers"
#endif

struct raw_result {
  uint64_t value;
  uint64_t carry;
};

struct trace_case {
  uint32_t code;
  uint64_t arguments[4];
};

static struct raw_result raw_trace64(uint32_t code, const uint64_t arguments[4]) {
  register uint64_t x0 __asm__("x0") = code;
  register uint64_t x1 __asm__("x1") = arguments[0];
  register uint64_t x2 __asm__("x2") = arguments[1];
  register uint64_t x3 __asm__("x3") = arguments[2];
  register uint64_t x4 __asm__("x4") = arguments[3];
  register uint64_t x16 __asm__("x16") = SYS_kdebug_trace64;
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %5, nzcv"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4),
                     "=r"(nzcv)
                   : "r"(x16) : "cc", "memory");
  return (struct raw_result){x0, (nzcv >> 29) & 1U};
}

static struct raw_result raw_write(const char* bytes, uint64_t length) {
  register uint64_t x0 __asm__("x0") = 1;
  register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)bytes;
  register uint64_t x2 __asm__("x2") = length;
  register uint64_t x16 __asm__("x16") = SYS_write_nocancel;
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %3, nzcv"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "=r"(nzcv)
                   : "r"(x16) : "cc", "memory");
  return (struct raw_result){x0, (nzcv >> 29) & 1U};
}

int main(void) {
  static const struct trace_case cases[] = {
    {0x2bdc0010, {0x1122334455667788ULL, 2, 3, 4}},
    {0, {0, 0, 0, 0}},
    {0xffffffffU, {UINT64_MAX, 1, 2, 3}},
    {3, {1, 1, 1, 1}},
    {0x01000000U, {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}},
  };
  for (unsigned index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    errno = EDOM;
    const struct raw_result result = raw_trace64(cases[index].code,
                                                 cases[index].arguments);
    if (result.value != 0 || result.carry != 0 || errno != EDOM) return 1 + index;
  }

  static const char passed[] =
      "kdebug_trace64 disabled result=0 carry=0 errno=unchanged cases=5\n";
  const struct raw_result written = raw_write(passed, sizeof(passed) - 1);
  return written.carry == 0 && written.value == sizeof(passed) - 1 ? 0 : 7;
}
