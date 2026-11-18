#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "self";
  const uint64_t sentinel = UINT64_C(0xabcd1ed019807654);
  volatile uint64_t output = sentinel;
  struct { uint64_t flags; uintptr_t output; } request = {0, (uintptr_t)&output};
  static const uint64_t read_only = 1;
  uintptr_t arg = (uintptr_t)&request;
  unsigned call = 90;
  const char *policy = "AMFI";
  if (strcmp(mode, "other-flags") == 0) request.flags = 1;
  else if (strcmp(mode, "null-output") == 0) request.output = 0;
  else if (strcmp(mode, "unmapped-output") == 0) request.output = UINT64_MAX;
  else if (strcmp(mode, "read-only-output") == 0) request.output = (uintptr_t)&read_only;
  else if (strcmp(mode, "null-request") == 0) arg = 0;
  else if (strcmp(mode, "other-call") == 0) call = 0;
  else if (strcmp(mode, "other-policy") == 0) policy = "UnknownPolicy";
  else if (strcmp(mode, "self") != 0) return 2;

  errno = EDOM;
  register uintptr_t x0 __asm__("x0") = (uintptr_t)policy;
  register uintptr_t x1 __asm__("x1") = call;
  register uintptr_t x2 __asm__("x2") = arg;
  register uintptr_t x16 __asm__("x16") = SYS___mac_syscall;
  unsigned carry;
  __asm__ volatile("svc #0x80\n\tcset %w[carry], cs"
                   : [carry] "=&r"(carry), "+r"(x0), "+r"(x1), "+r"(x2)
                   : "r"(x16) : "cc", "memory");
  if (errno != EDOM || carry != 1 || x0 != EPERM || output != sentinel) return 3;
  puts("self-policy=EPERM output=untouched errno=unchanged");
  return 0;
}
